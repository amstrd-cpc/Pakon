# Scanner session layer

Source: pakon-reference `docs/ppb-protocol.md` § The open handshake / §
Presence probes, `docs/command-reference.md` § Initialisation,
`docs/calibration.md` (ScannerType), plus capture-verified details noted
below. Our implementation: `src/pakon/scanner/`.

## State machine (Phase 4 subset)

```
disconnected ──connect()──▶ connecting ──handshake ok──▶ ready
                                 │                        │
                                 ▼                        ├──identify()──▶ ready (identity filled)
                               error ◀──── any failure ───┤
                                                          └──disconnect()──▶ disconnected
```

Transitions are explicit and logged (`debug`). Operational states
(Loading/Scanning/Transferring) arrive with Phases 5–7; they are not
invented here.

## `connect()` — the open handshake

```
host → 04 03 10 00 85        HostReset      → 07 02 10 00
host → 02 04 10 01 8f 00     HostSetMode    → 07 02 10 00
```

`[DOCUMENTED, verbatim in ppb-protocol.md]`.

**Best-effort replies:** these two replies arrive only on the *first* open
after power-on or firmware load; later opens time out while the bridge works
normally `[CONFIRMED on hardware, August 2026]`. `connect()` therefore treats
`usb_timeout` on exactly these two frames as expected (logged at debug) and
does not fail the session. Any other error fails with state → `error`.

## `identify()` — model detection

1. **Presence probes** — `04 03 <addr> 00 00` (CMD reg `0x00` = ResetMotor,
   the documented probe form) to `0x44` then `0x24`:
   - status `0x00` = present, `0x01` = absent `[DOCUMENTED]`
   - F-135+: `0x44` present, `0x24` absent; F-135 inverted
     `[CONFIRMED on hardware, August 2026]`
2. **Model decision** — exactly one motor controller answering decides the
   model and its address pair (`kF135{0x20,0x22,0x24,0x26}` /
   `kF135Plus{0x40,0x42,0x44,0x46}`). **Both present or both absent →
   `Model::unknown`, never a guess.** Unit-tested.
3. **Module info** — READ reg `0x07`, 12 bytes, at light then motor
   controller (the init sequence's "module-info read from each controller"
   `[DOCUMENTED]`; register number `0x07` from captures). Payload is stored
   raw; beyond the window below no semantic decoding is claimed
   (undocumented). The only capture-evidenced text is the 5-byte ASCII id
   at offsets `[5..9]`, identical in both captured replies
   (`base4.jsonl` events 151/159, unit 16402):
   `0f 0a 05 00 00 '12345' 00 00` (PICL+) / `10 06 05 00 00 '12345' 00 00`
   (PICM+). `ModuleInfo::printable()` returns exactly that window and only
   when all five bytes are printable — otherwise no string is shown and
   the CLI prints the raw hex. Scanning the whole payload for printable
   bytes (the original heuristic) fabricated `module: @!` from the lab
   unit's binary. **Lab unit 010-203-04 (identify, 2026-10-08) returns a
   different, fully non-printable layout from the identical request:**
   `04 20 40 12 04 c0 21 02 00 00 92 00` (PICL+) /
   `02 20 00 a0 00 8c 08 00 00 20 00 00` (PICM+) — semantics `[UNKNOWN]`
   (payload differs per unit/firmware; the OEM stack also writes PICL/
   PICM reg `0x03` = `01` immediately before each module-info read
   `base4.jsonl` events 147/155, which our sequence does not).
4. **Bridge info** — READ HOST reg `0x03`, 2 bytes (capture-verified;
   observed `0f 03`; semantics unknown → shown as hex only).

Cross-check available for Phase 9: EEPROM `ScannerType` is `1350` (F-135) /
`1351` (F-135+) `[DOCUMENTED]` — compare with the probe result once the
read-only EEPROM path exists.

## `status()` — read-only status

Requires a successful `identify()`. Exactly the documented operations:

| Operation | Frame | Source |
|---|---|---|
| HOST poll | `03 01 10` → `03 03 10 <st> aa` | captures (8037×/session) |
| light poll | `03 01 40` (or `0x20`) → `03 02 <addr> <st>` | dx-barcode.md + captures |
| motor poll | `03 01 44` (or `0x24`) | same |
| CCD status | `01 03 40 01 83` → 1 byte | command-reference `0x83` READ 1 |
| light status | `01 03 40 02 84` → 2 bytes | command-reference `0x84` READ 2 |
| temperature | `01 03 40 04 88` → 4 bytes | command-reference `0x88` READ 4 |

All are polls/reads — "reading, polling and PPB commands to the known
controller addresses have no recorded incident"
(per-unit-data-and-safety.md). Expected payload sizes are validated; a
mismatch is `ppb_unexpected_reply`, not silent truncation. `0x90`
ReadSensorData is **deliberately not queried** here: it may only be read after
the controller raises its service flag `[CONFIRMED]` (a Phase 5+ concern).

## What this layer does not do (yet)

- No initialization/scan/teardown sequences (Phase 5) — the capture corpus
  contains them and they will be implemented strictly from
  command-reference.md § Sequences.
- No motor motion, light/CCD configuration, TEC writes.
- No teardown frame exists for disconnect; the OEM simply closes the handle,
  and so do we.

## Tests

`tests/scanner/scanner_replay_test.cpp` (7 cases) replays capture pairs:
handshake, Plus-unit identification (addresses, module payloads, bridge info),
status register values, the inconclusive-probe path, status-before-identify
rejection, and allow-list refusals (bootloader, EEPROM bus address). Any
byte the implementation sends that is not in the scripted capture fails the
test.
