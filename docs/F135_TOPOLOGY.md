# F135 runtime USB topology (read-only descriptor discovery)

**Status:** implemented (`pakon-cli descriptors`, sources
`src/pakon/usb/descriptors.*`); **hardware run recorded 2026-10-08** on
the lab unit (booted `0F05:F135`, serial `010-203-04`) — observation
tables below hold the pasted output verbatim.

Evidence labels follow [BOOT_CHAIN.md](BOOT_CHAIN.md):
**[PROVEN]** observed from the device on the record · **[STATIC]**
established by static analysis of committed artifacts or cited spec ·
**[INFERRED]** a reading of observed bytes with the reasoning stated ·
**[UNKNOWN]** not established · **[PENDING]** awaiting evidence.

---

## 1. Goal and scope

Determine the complete USB topology of the live `0F05:F135` runtime —
device and configuration descriptors, every interface / alternate
setting / endpoint, and the WinUSB interface path — **without sending a
single vendor-specific command**. This closes the gap between what
[BOOT_CHAIN.md](BOOT_CHAIN.md) proved at the identity level (device
re-enumerates as `USB\VID_0F05&PID_F135`, bound to WinUSB) and what the
wire format of the operational device actually is.

Scope decisions:

- Only the **booted `0F05:F135`** is scanned. A cold `0F05:F235` unit is
  never touched by this diagnostic (the probe remains the cold-device
  read-only tool).
- The configuration **bytes** are read, never acted upon: no
  configuration select, no alternate-setting switch, no pipe policy.
- Endpoint **roles** are not observed by this scan — they are readings
  (§ 4), each labelled until proven by a future, separately-approved
  phase.

## 2. What the diagnostic sends (and what it never does)

Command line (Windows, repo checkout):

```powershell
.\build\apps\pakon-cli\Release\pakon-cli.exe descriptors
```

| Operation | Kind | Why it is inside the envelope |
|---|---|---|
| SetupAPI enumeration (identity rules) | no I/O | same discovery as `list`; opens nothing |
| `GET_DESCRIPTOR` device / configuration / string via `WinUsb_GetDescriptor` | **standard** control read (`bmRequestType` 0x80/0x82) | the definition of read-only descriptor discovery |
| `WinUsb_QueryDeviceInformation(DEVICE_SPEED)` | driver-side query | talks to winusb.sys, not the wire; `bConfigurationValue` has no WinUSB read API — an active configuration is implied by `WinUsb_Initialize` succeeding |
| `WinUsb_QueryInterfaceSettings` / `QueryPipe` / `GetAssociatedInterface` | driver-side query | topology cross-check independent of the raw bytes |
| CreateFile + `WinUsb_Initialize` (shared overlapped params, `usb/win_usb_open.hpp`) | host-side open | same proven open as `list` / transport; released immediately |

Explicitly **not** done — same prohibitions as the probe charter:
no vendor control transfer (nothing of the `0xA0`/`0xA3`/`0xA4`/`0xA9`
family, no `bRequest >= 0x40`), no firmware upload, no USB/port reset,
no set-configuration / alt-setting switch / pipe-policy change
(no device or driver reconfiguration), no bulk traffic of any kind.
Every write in the boot chain was already volatile; this diagnostic
performs **no writes at all**.

## 3. Observation record (lab unit, 2026-10-08) — verbatim from the run

### 3.1 Device context

| Field | Value | Label |
|---|---|---|
| Instance ID | `USB\VID_0F05&PID_F135\010-203-04` | `[PROVEN]` |
| Hardware ID | `USB\VID_0F05&PID_F135&REV_0002` | `[PROVEN]` |
| WinUSB interface path | `\\?\usb#vid_0f05&pid_f135#010-203-04#{0e9e6f29-e70a-4582-8d02-bde3ad701252}` | `[PROVEN]` |
| Open mode | `GENERIC_READ\|GENERIC_WRITE, FILE_FLAG_OVERLAPPED` (shared params, first attempt — no fallback needed) | `[PROVEN]` |
| Link speed | raw code `0x03` → **high speed or above** (WinUSB `DEVICE_SPEED` encoding: `0x01` = low/full, `0x03` = high-or-above, per Microsoft `WinUsb_QueryDeviceInformation` docs; consistent with 512-byte bulk endpoints + bcdUSB 0200). The first exe printed `unknown(3)` — display mapping corrected after the run. | `[PROVEN]` raw + `[STATIC]` meaning |
| Configuration active | implied by `WinUsb_Initialize` success — `bConfigurationValue` is **not exposed** by the WinUSB API (no `WinUsb_GetConfiguration` exists) | `[UNKNOWN]`/not readable |

**Interface-path GUID finding:** the path carries
`{0e9e6f29-e70a-4582-8d02-bde3ad701252}`, the GUID that
`tests/usb/identity_test.cpp` pins byte-identical to
`driver/PakonWinUSB.inf` `[STATIC]` → the booted F135's WinUSB interface
was registered **by this repo's INF** `[PROVEN chain]` (cross-reference:
BOOT_CHAIN.md § 6, whose `DeviceDesc`/`FriendlyName` rows remain
`[PENDING]` — the `attrib` output has not been transcribed).

### 3.2 Device descriptor (18 bytes, `GET_DESCRIPTOR(DEVICE)`)

| Field | Value | Label |
|---|---|---|
| bcdUSB / class triad / bMaxPacketSize0 | `0200` / `00/00/00` / `64` | `[PROVEN]` |
| idVendor / idProduct / bcdDevice | `0f05` / `f135` / `0002` | `[PROVEN]` |
| iManufacturer / iProduct / iSerial / bNumConfigurations | 1 / 2 / 3 / 1 | `[PROVEN]` |
| raw 18 bytes | `12 01 00 02 00 00 00 40 05 0f 35 f1 02 00 01 02 03 01` | `[PROVEN]` |

**Cross-check vs BOOT_CHAIN § 5** (static read of Pakon7 @ `0x1000` =
`1201000200000040050f35f1020001020301`): the live bytes are
**byte-identical** → `[PROVEN]` link between the downloaded Pakon7 image
and the enumerated device identity: the firmware we statically inspected
is what is running.

### 3.3 Configuration descriptor (39 bytes, `GET_DESCRIPTOR(CONFIGURATION)`)

| Field | Value | Label |
|---|---|---|
| wTotalLength / bNumInterfaces / bConfigurationValue | `0x0027` (39) / 1 / 1 | `[PROVEN]` |
| bmAttributes / bMaxPower / iConfiguration | `0xA0` (bus-powered + remote-wakeup capable, USB spec bit decode) / `0x32` = 50 → **100 mA** at 2 mA units / 0 | `[PROVEN]` raw `[STATIC]` bit meaning |
| TLV parse anomalies | none (both views agree byte-for-byte) | `[PROVEN]` |
| raw bytes | `09 02 27 00 01 01 00 a0 32 09 04 00 00 03 ff 00` `00 00 07 05 01 02 00 02 00 07 05 81 02 00 02 00` `07 05 86 02 00 02 00` | `[PROVEN]` |

### 3.4 Interfaces, alternate settings, endpoints

Raw-parse view and WinUSB `QueryInterfaceSettings`/`QueryPipe`
cross-check **agree exactly** `[PROVEN]`:

| interface | alt | class/sub | proto | EP | dir | type | wMaxPacket | bInterval | Label |
|---|---|---|---|---|---|---|---|---|---|
| 0 | 0 | `ff/00` | `00` | `0x01` | OUT | bulk | 0x0200 (512) | 0 | `[PROVEN]` |
| 0 | 0 | `ff/00` | `00` | `0x81` | IN | bulk | 0x0200 (512) | 0 | `[PROVEN]` |
| 0 | 0 | `ff/00` | `00` | `0x86` | IN | bulk | 0x0200 (512) | 0 | `[PROVEN]` |

One interface, one alternate setting, vendor-specific class `ff/00/00`,
`iInterface 0`; **no interrupt, isochronous or extra endpoints** — the
device exposes exactly three bulk pipes `[PROVEN]`.

### 3.5 Referenced string descriptors (`GET_DESCRIPTOR(STRING)`, lang `0x0409`)

| Index | Text | Label |
|---|---|---|
| 1 (iManufacturer) | `Pakon` | `[PROVEN]` |
| 2 (iProduct) | `F135-USB Film Scanner` | `[PROVEN]` |
| 3 (iSerialNumber) | `010-203-04` | `[PROVEN]` |

**Serial-derivation question (BOOT_CHAIN § 8) advanced:** the descriptor
string at index 3 is **exactly** the PnP instance segment
(`USB\VID_0F05&PID_F135\010-203-04`) → the instance id mirrors the USB
`iSerialNumber` `[PROVEN same value]` (Windows using `iSerialNumber` as
the instance segment is documented PnP behavior `[STATIC]`). How the
firmware *produces* that value (EEPROM? baked into Pakon7?) remains
`[UNKNOWN]`.

## 4. Endpoint-role reading — labels stated per claim

1. **`0x01 OUT` + `0x81 IN` (bulk) = the PPB command channel.**
   `[STATIC]` from the primary spec (pakon-reference ppb-protocol.md
   "Command channel", cited in `usb/transport.hpp`) → **upgraded to
   `[PROVEN]` live** by the operator-run `identify` trace of the same
   session (2026-10-08, recorded in STATUS.md): frames transmitted on
   `0x01`, mirrored replies received on `0x81`, connect handshake
   completed (`04 03 10 00 85` → `07 02 10 00`; `02 04 10 01 8f 00` →
   `07 02 10 00`, state → ready).
2. **`0x86 IN` (bulk, 512) = the image stream.** `[INFERRED]` —
   reasoning: the primary spec says the image stream rides "a separate
   bulk IN endpoint" whose number it does not state, with max packet
   512 at high speed (image-stream.md, cited in `usb/transport.hpp`);
   `0x86` is the only bulk IN that is not the command reply endpoint,
   and its parameters match exactly. Alternative reading (a second
   vendor/debug channel) is not excluded `[UNKNOWN]`; confirmation
   requires streaming an image in a future, separately-approved phase.
   No run has ever touched `0x86` `[PROVEN absence from all transcripts]`.
3. **No sideband status channel.** With zero interrupt/ISO endpoints,
   all host↔device traffic (PPB + future image data) must ride these
   three bulk pipes `[PROVEN structure]`; per-endpoint data semantics
   beyond (1)/(2) remain `[UNKNOWN]`.
4. **Vendor interface `ff/00/00`** is consistent with a
   vendor-protocol device and with why the OEM `F235usb2.inf`
   (Class=Image) never matched functionally `[STATIC chain, BOOT_CHAIN
   § 6]`.

## 5. Side observation: first on-hardware `identify` (operator-run, 2026-10-08)

Recorded because it was pasted; it went **beyond the descriptor-only
envelope** — no further protocol runs are proposed here.

- Connect handshake + state machine worked on real hardware for the
  first time: `[PROVEN]` (see § 4.1 for bytes).
- Presence probes: `0x44` → status `00` (present), `0x24` → status `01`
  (absent) `[PROVEN]` — exactly the F-135+ answer pattern documented in
  `scanner.cpp` `[STATIC rule]` → `[INFERRED]` unit follows the Plus
  pattern; model not confirmed because `identify` did not complete.
- Module-info read (`01 03 40 0c 07`): the device answered
  `01 0e 40 88` + **exactly 12 payload bytes** — count `0x0e` = 2 + 12,
  address echoed, payload size exactly what `scanner.cpp` expects
  `[PROVEN well-formed]`.
- The `0x88` byte is the READ reply **flags** byte: `0x88 = 0x08 |
  0x80`, event-pending — documented *in this repo*
  (`ppb/packet.hpp`, `docs/PPB.md`, `docs/PAKON_REFERENCE.md`;
  "2657:1 in the captures", pinned by `parse_read_reply_event_flag`)
  `[STATIC]`.
- `ppb::is_success` accepts only `0x00`/`0x08` (`packet.cpp`),
  so the scanner surfaced `ppb_bad_status: … status unknown` and
  stopped `[PROVEN behavior]`.
- **Reading:** the device answered per our own documented READ-flags
  semantics; our success check does not mask the documented event bit
  for READ replies `[INFERRED — chain: §5 bytes + §5 static docs +
  is_success source]`. The 12-byte payload was **not** interpreted and
  no retry was made. Fixing `is_success` (plus test adjudication) is a
  code change awaiting explicit go-ahead — out of scope of this
  artifact.

## 6. Safety record

- Traffic actually sent by **this diagnostic**: standard `GET_DESCRIPTOR`
  reads (device, configuration, string) and WinUSB driver-side queries —
  nothing else.
- Never touched: `0xA0`/`0xA3`/`0xA4`/`0xA9`, any `bRequest >= 0x40`,
  firmware upload, reset, configuration/alt-setting changes, pipe
  policies, bulk endpoints.
- No writes of any kind; the device was left exactly as found.
- Implementation guards: `descriptors_win.cpp` calls only
  `WinUsb_Initialize`, `WinUsb_GetDescriptor`,
  `WinUsb_QueryDeviceInformation`, `WinUsb_QueryInterfaceSettings`,
  `WinUsb_QueryPipe`, `WinUsb_GetAssociatedInterface` (plus close/free):
  no `WinUsb_ControlTransfer`, no `WinUsb_Set*`, no `WinUsb_ResetPipe`,
  no `WinUsb_ReadPipe`/`WritePipe` **call sites** — a grep for those
  names hits only the safety comment that states their absence.
- The separate operator-run `identify` (§ 5) is recorded in
  `docs/STATUS.md`; it did not modify this diagnostic's envelope.

## 7. Open questions this run settled — and what remains

**Settled:** raw descriptor bytes; full live topology (two agreeing
views); string contents incl. serial; link speed (high-or-above);
interface-path/INF attribution chain; device-descriptor identity with
the downloaded Pakon7 image; the presence-probe answer pattern.

**Remains `[UNKNOWN]`:** payload semantics of `0x86` (and any traffic
format beyond PPB); origin of the serial value inside firmware;
`bConfigurationValue` (not exposed by WinUSB); OEM report-stack identity
beyond `pakon-cli attrib` (its output still awaits transcription into
BOOT_CHAIN § 6); `0xA9` byte[7] = `0x04`; necessity of the `0xA4`
preamble given stage-1 (ablation never run); the `0x88` event-flag
handling gap in `is_success` (§ 5) — fix decision pending.
