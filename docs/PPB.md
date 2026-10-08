# PPB protocol layer

Primary spec: pakon-reference `docs/ppb-protocol.md`, `docs/command-reference.md`,
`docs/dx-barcode.md`. Field-level details cross-checked against the
`alibosworth/pakon-captures` corpus (OEM stack driving an F-135+, serial 16402)
and pakon-tlx-macos `docs/PROTOCOL.md` (cited by pakon-reference).
Our implementation: `src/pakon/ppb/`, `src/pakon/protocol/`.

## Frame format

```
[type:1][count:1][data:count]          wire length = 2 + count
```

No checksum, no padding — USB integrity is relied upon `[DOCUMENTED]`.
The invariant `length == 2 + count` holds for **all 198,425 frames** in the
capture corpus; `Frame::parse`/`parse_reply` reject any deviation.

`data` field layout (established from captures + PROTOCOL.md; pakon-reference
states only `data[0]` = address and `data[2]` = command byte):

| Request | data | type byte |
|---|---|---|
| WRITE | `[addr][payload_len][reg][payload…]` | `0x02` (payload_len > 0) |
| CMD | `[addr][0][reg]` | `0x04` (payload_len == 0) |
| READ | `[addr][byte_count][reg]` (always 3 bytes) | `0x01` |
| READ_STATUS | `[addr]` | `0x03` |

Verbatim examples:

```
04 03 10 00 85        HostReset         (ppb-protocol.md)
02 04 10 01 8f 00     HostSetMode       (ppb-protocol.md)
04 03 44 00 00        motor probe       (ppb-protocol.md + captures)
01 03 40 1e 90        READ 30 bytes, PICL+ reg 0x90   (captures)
03 01 44              status poll, PICM+              (dx-barcode.md + captures)
```

### Safety: type byte 0

A frame whose *type* byte is `0` is not drained by the FX2 bridge and wedges
it until a physical power cycle `[CONFIRMED]` (per-unit-data-and-safety.md).
Invalid *payloads* are harmless (device answers status `0x02`).
`Frame::serialize()` refuses to emit type 0 (defense in depth — the enum
cannot hold it) and `Frame::parse()` rejects it from the wire.

## Reply forms

| Request | Reply |
|---|---|
| CMD `0x04` / WRITE `0x02` | `07 02 <addr> <status>` |
| READ_STATUS `0x03`, controllers | `03 02 <addr> <status>` |
| READ_STATUS `0x03`, HOST `0x10` | `03 03 10 <status> <0xaa>` (5 bytes) |
| READ `0x01` | `01 <2+n> <addr> <flags> <n payload bytes>` |

- Reply type mirrors request type `[CONFIRMED on hardware, August 2026]`;
  `parse_reply()` enforces this.
- The 5-byte HOST poll form (trailing constant `0xaa`, status `0x80` = host
  event pending) is **capture evidence** — pakon-reference documents only the
  4-byte controller form. Observed 8037:1 ordinary:event in one session.
- READ flags byte: `0x08` ordinary, `0x88` (= `0x08 | 0x80`) event pending
  `[CONFIRMED]`; observed 2657:1 in captures. `Reply::event_pending()` exposes
  the bit.
- A reply shorter than 4 bytes carries no status and must never be read as
  success `[DOCUMENTED]` → `ppb_reply_too_short`.

## Status codes

`0x00` ok · `0x01` not acked (also: controller absent) · `0x02` invalid packet
· `0x03` bad checksum · `0x04–0x06` USB errors · `0x07` host-algorithm error ·
`0x08` "also reported as success" · `0x09` bus error `[DOCUMENTED]`.
`is_success()` treats `0x00` and `0x08` as success. READ replies are checked
with `is_read_success(Reply)` instead: flags `0x08` and `0x88` accepted
(the `0x80` event bit ignored for success, still reported by
`event_pending()`), every other flags byte rejected, non-READ replies
declined — ordinary status handling stays `is_success()`.

## Session client (`ppb::Client`)

- `exchange(request)` → serialize → transport → `parse_reply` → address-echo
  check (a non-echoing reply is `ppb_unexpected_reply`; this also catches
  stale replies).
- **Destination allow-list**: only `0x10, 0x20, 0x24, 0x40, 0x44`. Bootloader
  addresses (`0x22/0x26/0x42/0x46` — flash erase with command bytes
  `0x0C–0x0F`) and EEPROM bus addresses (`0xA2/0xA4` — real units have lost
  data here) are refused before anything reaches the wire. Unit-tested.
- TX/RX hex dumps at `trace` level via `pakon-cli --log trace`.

## Command sets (protocol/commands.hpp)

Bytes are shared across controllers; **the frame address disambiguates them**
(`0x82` = SetColorMatrix on PICL, SetMotorSpeed on PICM). Source:
command-reference.md tables; every byte/payload size was re-verified against
the captures:

- **PICL `0x20`/`0x40`**: `0x02` read service status, `0x06` service ack
  (dx-barcode.md), `0x80` W1, `0x81` W5, `0x82` W12, `0x83` R1,
  `0x84` R2, `0x87` W2, `0x88` R4, `0x89` W1, `0x8A` C0, `0x8B/8C/8D` W4,
  `0x8F` **W4** (see discrepancy below), `0x90` R30, `0x91` W3, `0x92` C0,
  `0x93` R4, `0xD0/0xD1` W1 (F-135+ TEC — never probe; replay OEM values only).
- **PICM `0x24`/`0x44`**: `0x00` C0, `0x82` W3 (indexed: `[subreg][u16 LE]`),
  `0x84` W3, `0x97` W1, `0xA0` C0, `0xA1` C0, `0xA2` C0, `0xA5` W2.
- **HOST `0x10`**: `0x84` W1, `0x85` C0, `0x8F` W1.
- `ScanLineParams` values (`0x91` payload, six resolution/IR combinations)
  `[CONFIRMED live]` via dx-barcode.md.

Capture-verified registers **not in command-reference's tables**:
PICL/PICM `0x07` R12 (module info — the init sequence's "module-info read"),
PICL/PICM `0x03` W1 (early init), HOST `0x03` R2 (bridge info, observed
`0f 03`).

## Discrepancies vs pakon-reference (resolved with capture evidence)

| Topic | pakon-reference | Captures | Resolution |
|---|---|---|---|
| SetLightConfig `0x8F` payload | 2 bytes | count=4 in all 6 sessions | we build W4; ref entry incomplete |
| HOST status poll reply | generic `03 02 <addr> <status>` | `03 03 10 <status> aa` | both: controllers 4 bytes, HOST 5 bytes |
| READ request layout | only `data[0]` documented | `[addr][count][reg]` everywhere | full layout adopted |
| Image endpoint number | not stated | EP `0x86` (tlx docs + `ep6` events) | read from descriptors at runtime (USB.md) |

## Tests

`tests/ppb/packet_test.cpp` (22 cases): builders reproduce documented and
captured frames byte-for-byte; replies parsed from real capture hex;
type-0/length-mismatch/short-reply/wrong-type rejections; read-request shape
and write-length invariants.
