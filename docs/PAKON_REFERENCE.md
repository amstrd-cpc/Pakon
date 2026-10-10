# pakon-reference Analysis

How https://github.com/alibosworth/pakon-reference maps to this implementation.

Reference clone analyzed at commit of 2026-10-06. It is **documentation of
facts only** — no driver code, no firmware, no packet captures. Every claim
carries a confidence marker (`[CONFIRMED]`, `[CONFIRMED on hardware, August 2026]`,
`[DOCUMENTED]`, `[INFERRED]`, `[SPECULATIVE]`) defined in
`pakon-reference/docs/CONVENTIONS.md`.

---

## 1. What pakon-reference provides

| File | Content |
|---|---|
| `docs/CONVENTIONS.md` | Confidence markers, sourcing rules, scope rules |
| `docs/per-unit-data-and-safety.md` | Per-unit data stores, damage vectors, safe backup procedures. **Read first.** |
| `docs/usb-identity-and-firmware.md` | Cold/warm USB identity, personality mechanism, FX2 firmware load sequence |
| `docs/ppb-protocol.md` | Frame format, packet types, status codes, bus addresses, open handshake, reply structure |
| `docs/command-reference.md` | Per-controller command tables (light/CCD, motor, host) + sequences |
| `docs/image-stream.md` | Bulk image transport, row strides, RGB interleave, IR lane, marker bit, OEM raw export |
| `docs/calibration.md` | Per-unit EEPROM layout/read, CCD register banks, dark/bright fixed-pattern correction |
| `docs/color-pipeline.md` | Inversion LUT formula, colour matrix, pipeline placement |
| `docs/film-transport.md` | Motor engage/run/stop, speed encoding, frame advance, film sensing |
| `docs/dx-barcode.md` | DX barcode subsystem, sensor reply layout, frame numbering rules |
| `docs/scanner-family.md` | F-135/F-135+/F-235/F-335 differences, OEM host software stack |
| `docs/resources/` | EEPROM dump comparison, hardware photos, DX product-code table |

---

## 2. What can be directly translated into our implementation

### 2.1 USB identity (source: `docs/usb-identity-and-firmware.md`)

```
Cold (bootstrap):  0f05:f235   — all family members, needs firmware
Warm (F-135/F-135+): 0f05:f135 — operational 135-line scanner
Warm F-235:        0f05:35f2
Warm F-335:        0f05:f335
```

- F-135 and F-135+ are **indistinguishable by USB descriptors**
  (`[INFERRED]`, same identity + personality key `F235_AA07`). Model is only
  knowable via PPB presence probes or EEPROM `ScannerType` (1350/1351).
- Personality: 8-byte C0 record at I2C `0x51`, read via vendor request `0xA9`
  with `wIndex 0` from the stage-1 loader `[CONFIRMED on hardware, August 2026]`.
- Firmware load is the standard FX2 sequence (CPUCS reset, `0xA0`/`0xA3`
  downloads, re-enumerate) `[DOCUMENTED]` — **sequence only; firmware bytes
  are out of scope** (`CONVENTIONS.md` excludes them).

**Maps to:** `src/pakon/usb/` (device enumeration, VID/PID constants),
`pakon-cli list`.

### 2.2 PPB frame format (source: `docs/ppb-protocol.md`)

Frame: `[type:1][count:1][data:count]`; on-wire length = `2 + count`.
**No checksum, no padding** — relies on USB integrity `[CONFIRMED]`.

Type byte:

| Value | Name | Meaning |
|---|---|---|
| `0x01` | READ | read N bytes from device |
| `0x02` | WRITE | write N bytes to device register/buffer |
| `0x03` | READ_STATUS | one-byte status poll (no command byte) |
| `0x04` | CMD | command with no payload |
| `0x07` | ACK | device→host ack (to CMD/WRITE) |

`data[0]` = destination bus address; for command/write frames `data[2]` = command byte.

Reply forms:

| Request | Reply |
|---|---|
| CMD `0x04` / WRITE `0x02` | `07 02 <addr> <status>` |
| READ_STATUS `0x03` | `03 02 <addr> <status>` |
| READ `0x01` | `01 <count> <addr> <status/flags> <data…>` |

Status codes: `0x00` ok, `0x01` not acked, `0x02` invalid packet, `0x03` bad
checksum, `0x04–0x06` USB errors, `0x07` host-algorithm error, `0x08` also
reported as success, `0x09` bus error `[DOCUMENTED]`.

READ-reply status/flags byte: `0x08` ordinary, `0x88` with event pending
(`0x80` event flag) `[CONFIRMED on hardware, August 2026]`.

**Safety-critical:** a packet whose *type* byte is `0` wedges the FX2 bridge
state machine; only a power cycle clears it. Invalid *payload* is harmless
(status `0x02`) (source: `per-unit-data-and-safety.md`) `[CONFIRMED]`.

**Maps to:** `src/pakon/ppb/` — packet serialization/parsing, status decoding.

### 2.3 Bus addresses and model detection (source: `docs/ppb-protocol.md`, `docs/scanner-family.md`)

| Address | Device |
|---|---|
| `0x10` | HOST (bridge) |
| `0x20` / `0x22` | PICL / PICL bootloader (F-135) |
| `0x24` / `0x26` | PICM / PICM bootloader (F-135) |
| `0x40` / `0x42` | PICL+ / bootloader (F-135+) |
| `0x44` / `0x46` | PICM+ / bootloader (F-135+) |

Model detection = presence probes after bridge open: status `0x00` = present,
`0x01` = absent. F-135+ answers `0x44` present / `0x24` absent; inverted on
F-135 `[CONFIRMED on hardware, August 2026]`.

**Bootloader addresses are dangerous**: type-4 packets to `0x42`/`0x46` with
command byte bits 3+2 set (`0x0C`–`0x0F`) trigger 64-byte flash row erase —
a real unit lost a motor-firmware row this way `[CONFIRMED by incident]`.
Writes to unknown bus addresses can erase the boot personality EEPROM
(`0xA2`/`0xA4` are the shifted I2C EEPROM addresses) `[CONFIRMED by incident]`.

**Maps to:** `src/pakon/scanner/` (model detection), enforced address
allow-list in the PPB layer.

### 2.4 Open handshake (source: `docs/ppb-protocol.md`)

```
host → 04 03 10 00 85        HostReset      → 07 02 10 00
host → 02 04 10 01 8f 00     HostSetMode    → 07 02 10 00
host → 04 03 <picm> 00 00    motor probe    → 07 02 <picm> <status>
   … further presence probes
```

The HostReset/HostSetMode replies arrive **only on the first open after
power-on/firmware load**; later opens time out while the bridge works
normally. Treat those two replies as best-effort `[CONFIRMED on hardware, August 2026]`.

**Maps to:** `src/pakon/scanner/` connect sequence.

### 2.5 Command channel + image channel (source: `docs/ppb-protocol.md`, `docs/image-stream.md`)

- Commands: bulk OUT `0x01` → bulk IN `0x81`, atomic write-then-read.
- Image: a separate bulk IN endpoint; OEM reads it in 20480-byte transfers
  (host choice, ceiling `0x5000`), endpoint max packet 512 (high speed).
- Per-unit EEPROM read: vendor control requests `0xA4` (select, `wValue 0x00A5`,
  `wIndex 0x1234`) + `0xA9` (read, `wValue` = offset, ≤32 bytes) — read-only path.

**Maps to:** `src/pakon/usb/` (`IUsbTransport`), `src/pakon/image/` (bulk reader).

### 2.6 Command sets (source: `docs/command-reference.md`)

> **Superseded labels.** The OEM's own names and the register semantics
> recovered from TLB.dll are in [OEM_RE.md](OEM_RE.md) §3 and take
> precedence. Several working labels here are wrong: PICL `0x81` = LED
> currents `[B, IR, R, _, G]`, PICL `0x82` = LED on-times (not a colour
> matrix), PICM `0x82` = CCD FPGA settings by sub-register (control word with
> the acquire bit, pixel window, integration, panel LEDs — not motor speed),
> PICM `0x84` = A/D gains/offsets, PICL `0x8A` + HOST `0x84=02` = FIFO reset,
> PICL `0x91`/`0x92` = DX reader start/stop (not scan-line parameters or
> end of acquisition), PICM `0xA5` = motor rate.

Bytes are shared across controllers, disambiguated by frame address. Command
**names are [INFERRED] working labels**; bytes/payloads/sequencing are
[DOCUMENTED].

Light/CCD (`0x20`/`0x40`): `0x80` SetCcdConfig(W,1), `0x81` SetCcdGainOffset(W,5),
`0x82` SetColorMatrix(W,12), `0x83` ReadCcdStatus(R,1), `0x84` ReadLightStatus(R,2),
`0x87` SetLightPower(W,2), `0x88` ReadTemperature(R,4), `0x89` EnableScan(W,1),
`0x8A` AcquireLine(CMD), `0x8B/0x8C/0x8D` SetCcdExposure B/G/R (W,4),
`0x8F` SetLightConfig(W,2), `0x90` ReadSensorData(R,30), `0x91` SetScanLineParams(W,3),
`0x92` EndAcquisition(CMD), `0x93` ReadDxSensors(R,4), `0xD0/0xD1` SetTEC (F-135+ only, W,1).

Motor (`0x24`/`0x44`): `0x00` ResetMotor(CMD), `0x82` SetMotorSpeed(W,3),
`0x84` SetMotorConfig(W,3), `0x97` InitMotor(W,1), `0xA0` EngageFilmDrive(CMD),
`0xA1` StopFilmDrive(CMD), `0xA2` DisengageFilmDrive(CMD), `0xA5` SetMotorCalibration(W,2).

Host (`0x10`): `0x84` HostReady(W,1), `0x85` HostReset(CMD), `0x8F` HostSetMode(W,1).

**TEC (`0xD0`/`0xD1`) warning**: semantics unknown, plausible hardware-damage
risk — replay OEM exact values only, never probe or sweep
(`command-reference.md` caution box).

**Safety note:** `0x82` collides (SetColorMatrix vs SetMotorSpeed) — address
disambiguates.

**Maps to:** `src/pakon/ppb/command.*` (typed commands), `src/pakon/protocol/`
(address/command constants).

### 2.7 Sequences (source: `docs/command-reference.md`)

- **Initialisation**: bridge reset (HostReset ×3 + HostSetMode) → motor
  reset + init → module-info reads → CCD/light config → motor speed ramp +
  config → disengage → idle poll loop.
- **Scan**: pre-scan calibration lines → motor engage → HostReady→AcquireLine
  loop with status polls while image streams → EndAcquisition → DisengageFilmDrive.
- **Teardown**: CCD config to disable acquisition → EndAcquisition →
  disengage → motor speed to exit value → idle.

**Maps to:** `src/pakon/scanner/` state machine (Phase 5).

### 2.8 Image stream (source: `docs/image-stream.md`)

- Samples: 16-bit little-endian. Raw USB stream spans full 16-bit range
  (pre-calibration); processed/OEM export is 14-bit `[CONFIRMED]`.
- Per-row layout: interleaved `R,G,B,…`; with Digital ICE a separate IR lane
  (4th channel, non-interleaved).
- Measured F-135+ strides (August 2026): highest/6000, medium/4500, lowest/3000
  samples per row (RGB); lowest+IR = 4000. Visible width = stride/3 (or /4 with IR).
  **Stride must not be hardcoded** — derive from scan settings.
- Row-origin recovery: scanner sets LSB of one fixed word per line (marker
  bit); find it to make channel phase-independent `[CONFIRMED on hardware]`.
- OEM export: 16-byte `SiPlanarFileHeader` (size, width, height, bpp=48) +
  planar RGB (de-interleaved by OEM).

**Maps to:** `src/pakon/image/` (row assembly, marker-bit alignment, raw capture file).

### 2.9 Calibration / per-unit EEPROM (source: `docs/calibration.md`)

Two sections, each `{u32 length, u32 crc32}` header + payload, CRC-32 (zlib),
stored twice (primary + backup):

| Section | Primary | Backup | Length |
|---|---|---|---|
| A | `0x000` | `0x400` | 398 bytes |
| B | `0x800` | `0xA00` | 36 bytes |

Section A payload (little-endian, absolute offsets): hardware version `0x008`
u32 (400), **scanner type `0x00C` u32 (1350 = F-135, 1351 = F-135+)**,
serial `0x010` u32; per-resolution (bases 4/8/16) triples at `0x014`/`0x01A`/`0x020`
(Offset, MotorSpeed, MotorSpeed-IR); `NegMatrix0–29` f32×30 at `0x026`;
`PosMatrix0–29` f32×30 at `0x09E` (each row = `R,G,B,R²,G²,B²,RG,GB,BR,const`).
Section B: 12× u16 motor-adjust words + 1 u32.

Read: only vendor OUT `0xA4` (`wValue 0x00A5`) + vendor IN `0xA9` — never
`0xA2` (write) `[CONFIRMED]`. Read once per power cycle (one project saw
degradation on re-reads). Verify all 4 CRCs and compare primary/backup.

**Maps to:** `src/pakon/calibration/` (read-only Phase 9), model detection via
`ScannerType`.

### 2.10 Film transport (source: `docs/film-transport.md`)

Run: `SetMotorCalibration (0xA5)` → `EngageFilmDrive (0xA0)` →
`SetMotorSpeed (0x82)` → motion → **restore idle speed first** →
`DisengageFilmDrive (0xA2)`.

Critical confirmed fact: run/stop state lives in the speed register, not the
disengage; disengaging with a running speed leaves the motor turning.
A clean stop = idle speed, then disengage `[CONFIRMED on hardware, August 2026]`.

Speed: 16-bit tenths of mm/s; OEM UI forward 10–355, reverse −10…−355, 0 invalid.
Frame advance = open-loop distance move; frame boundaries determined in
software from image density transitions (no hardware frame sensor).

**Maps to:** `src/pakon/scanner/` (Phase 6).

### 2.11 Safety rules distilled (source: `docs/per-unit-data-and-safety.md`)

1. Never send a frame with type byte `0` (wedges FX2; power cycle only).
2. Never write to bus addresses except known controllers; `0xA2`/`0xA4` write
   the EEPROMs and can erase the boot personality.
3. Never issue vendor request `0xA2` (per-unit EEPROM write).
4. Never send `0x0C`–`0x0F` command bytes to bootloader addresses (`0x42`/`0x46`).
5. TEC `0xD0`/`0xD1`: replay OEM values only.
6. LED current ceilings differ per board — don't cross-apply.
7. EEPROM reads: read once per power cycle.
8. Reading, polling, and PPB commands to known controller addresses have
   **no recorded incident**.

**Maps to:** allow-list validation in `src/pakon/ppb/`, `docs/STATUS.md`.

---

## 3. What is documentation only (no code in pakon-reference)

The repository contains **zero implementation code** — it is a mkdocs site.
Everything must be re-implemented. Specifically documentation-only:

- All packet/byte formats (translate by hand into explicit C++ serializers).
- OEM raw export format (`SiPlanarFileHeader`) — binary analysis notes only.
- Fixed-pattern gain formula: `gain[col] = (125·2^32) / (bright − dark − <prefix>)`
  — explicitly **partial**, not implementable as written.
- DX barcode host-read protocol (reconstructed from decompilation + live
  replay; `pakon-captures` repo holds the actual captures, referenced externally).

---

## 4. What parts are already implemented (elsewhere, not here)

Nothing in our repo. Implemented elsewhere and cross-referenced by the
reference (useful as behavioral evidence, not to be copied):

- `pakon-macos` (AGPL-3.0): drove F-135+ end-to-end; source of reply-direction
  facts, per-resolution strides, motor run/stop register behavior, marker-bit
  row-origin.
- `pakon-mac` (MIT + non-commercial): EEPROM layout decode, bootloader incidents.
- `pakon-tlx-macos` (MIT): `tools/eedump.py` read-only EEPROM sequence,
  OEM driver-contract notes.
- `libpakon` (private, Stefan Dierauf): independent C++ driver; the
  `125·2^32` constant cross-confirms against it.

---

## 5. What still needs to be implemented (our project)

| Area | Reference source | Phase |
|---|---|---|
| USB enumeration (cold/warm VID:PID) | `usb-identity-and-firmware.md` | 2 |
| USB abstraction + WinUSB transport | `ppb-protocol.md` endpoints | 1–2 |
| PPB frame serialize/parse | `ppb-protocol.md` | 3 |
| PPB status/reply decoding | `ppb-protocol.md` reply structure | 3 |
| Open handshake + presence probes (identify) | `ppb-protocol.md` | 4 |
| Status/info queries (`ReadCcdStatus`, module info) | `command-reference.md` | 4 |
| Scanner state machine | `command-reference.md` sequences | 5 |
| Film transport (documented commands only) | `film-transport.md` | 6 |
| Image stream capture → raw file | `image-stream.md` | 7 |
| Row/marker-bit decoding, RGB de-interleave | `image-stream.md` | 8 |
| Calibration read (read-only) | `calibration.md` | 9 |
| Output TIFF/JPEG | — | 10 |
| Colour pipeline (135-line path **undocumented**) | `color-pipeline.md` open questions | later |

**Not implemented by design at this stage:** firmware loading, DX substitution,
TEC control, any EEPROM write, colour processing.

---

## 6. Limitations and known gaps in pakon-reference

1. **Reply payloads for most commands unobserved** — captures were
   transmit-only; only live-exercised exchanges are confirmed.
2. **READ status/flags byte bit semantics incomplete** (`0x08`/`0x88` observed).
3. **135-line colour path undocumented** — the LUT+matrix path is the F-235
   engine's; `TLB.dll` uses a per-unit 3×10 polynomial whose inversion stage
   is not yet documented. 135-line implementations must NOT assume the F-235 path.
4. **Fixed-pattern correction formula partial** (prefix terms and clamp unknown).
5. **Command names are [INFERRED]** working labels, not OEM identifiers.
6. **Payload layouts unknown for most write commands.**
7. **Film-present bit within the 30-byte sensor state not pinned down.**
8. **End-of-roll signaling and advance distance encoding not documented.**
9. **Calibration-phase (pre-film) stream structure undocumented.**
10. **F-135+ highest-resolution IR-on stride unmeasured** (expected 8000).
11. **F-235/F-335 PPB addressing/command dialects/image geometry unknown.**
12. **No packet captures in the repo** — captures live in external
    `alibosworth/pakon-captures` (referenced from `dx-barcode.md`).

---

## 7. Exact files/sections used as references

| Our component | Reference file + section |
|---|---|
| `usb/device_ids.*` | `usb-identity-and-firmware.md` § "Cold and warm identity" |
| `usb/` enumeration | `usb-identity-and-firmware.md` § "The personality mechanism" |
| `ppb/packet.*` | `ppb-protocol.md` § "Frame format", § "Type byte values" |
| `ppb/response.*` | `ppb-protocol.md` § "Reply frames and the status byte", § "Reply structure, recovered by driving the scanner" |
| `ppb/endpoints` | `ppb-protocol.md` § "Command channel" |
| `scanner/connect.*` | `ppb-protocol.md` § "The open handshake", § "Presence probes are model detection" |
| `scanner/model.*` | `ppb-protocol.md` § "Presence probes", `calibration.md` § "Layout" (ScannerType 1350/1351) |
| `protocol/commands.*` | `command-reference.md` all tables; per-command notes cite this |
| `scanner/state_machine.*` | `command-reference.md` § "Sequences" |
| `scanner/transport.*` | `film-transport.md` § "Engage, run, stop", § "Speed" |
| `image/row_layout.*` | `image-stream.md` § "Row layout", § "Per-resolution strides" |
| `image/marker.*` | `image-stream.md` § "Marker bit for row origin" |
| `image/raw_export.*` | `image-stream.md` § "The exported raw file" |
| `calibration/eeprom_read.*` | `calibration.md` § "The read", `per-unit-data-and-safety.md` § "Backing up the per-unit EEPROM" |
| `calibration/sections.*` | `calibration.md` § "Layout" |
| safety allow-list | `per-unit-data-and-safety.md` § "What can be damaged, and how" |
| `ppb/status.*` tests | `ppb-protocol.md` status table; `dx-barcode.md` § "Reply layout" (34-byte `0x90` reply: `01 20 <addr> 08` header) |
