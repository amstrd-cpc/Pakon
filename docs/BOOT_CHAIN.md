# Boot chain (cold → warm): the validated F235 → F135 sequence

**Status 2026-10-07 — VALIDATED ON HARDWARE.** The complete cold-boot
sequence (stage-1 loader → gate → Pakon7 download → re-enumeration as the
warm `0f05:f135` identity) was reconstructed from OEM artifacts by static
reverse engineering, then executed on the lab machine as a series of
explicitly approved single-command experiments. Every predicted value
matched the hardware result byte-for-byte. This note is the evidence
record; the frozen executable procedure is
[`tools/pakon_boot_reference.ps1`](../tools/pakon_boot_reference.ps1).

Evidence markers used below:

| Marker | Meaning |
|---|---|
| **[PROVEN]** | observed verbatim in a hardware run on 2026-10-07 (lab machine, Windows, WinUSB) |
| **[STATIC]** | read byte-exactly out of an OEM artifact (disassembly or image), not yet independently re-verified |
| **[INFERRED]** | strong, consistent evidence; the named confirming check has not run yet |
| **[UNKNOWN]** | not determined; listed deliberately in § 8 |
| **[PENDING]** | field was not captured by the boot transcript; the read-only attribution query (§ 6) reports it |

---

## 1. What happened, in one paragraph

A cold Pakon F-235 (`0f05:f235`, REV `AA07`) was booted to its warm
identity (`0f05:f135`, REV `0002`) with **zero persistent writes**: the
host uploaded the OEM-embedded 4.5 KB first-stage loader from
`F235Ldr.sys` into FX2 RAM, started the CPU, sent the OEM's `0xA4`
preamble, answered the stage-1 loader's gated `0xA9` personality read with
the exact expected 8 bytes, downloaded `Pakon7.hex` (709 records via `0xA3`
while the CPU ran, then 19 records via `0xA0` while halted), started the
CPU — and the device re-enumerated as `USB\VID_0F05&PID_F135\010-203-04`
with `status=OK`, bound to WinUSB. All writes were to FX2 RAM and the
halt/run register only; unplugging restores the cold state.

## 2. The exact inputs (host-gated before any USB traffic)

Fetched at run time over HTTPS from the OEM installer mirror
(`github.com/plonsker/pakon-scanning-software`,
`Pakon Update/fx35install/program files/Pakon/F-135/F135Driver/`);
the run aborts host-side — no USB operation is attempted — unless both
MD5s match **[PROVEN: both gates passed in the boot run]**:

| Artifact | Size | MD5 |
|---|---|---|
| `F235Ldr.sys` (stage-1 source) | 16 000 B | `6be781bad6fb9cf7ade4095baed336e0` |
| `Pakon7.hex` (main firmware) | 30 129 B | `07f5001a951c4be179008d948be82b69` |

No firmware byte is committed to this repository (scope rule,
[BOOTSTRAP.md](BOOTSTRAP.md) § 3.1); the reference script fetches and
gates them at run time.

## 3. The sequence as executed (boot run, 2026-10-07)

Every row: `bmRequestType` `0x40` = vendor OUT (host-to-device), `0xC0` =
vendor IN. wIndex = 0 unless noted. “Pair” = two writes, `0x7F92` first
then `0xE600`, both `0xA0`.

| Step | USB operation | Prediction (static) | Hardware result | Marker |
|---|---|---|---|---|
| 0 | WinUSB open: R/W, share 3, `OPEN_EXISTING`, `FILE_FLAG_OVERLAPPED` | cold instance openable | path `\\?\usb#vid_0f05&pid_f235#6&1d7d6e45&0&4#{0e9e6f29…}`, opened | [PROVEN] |
| 1 | CPUCS halt pair, data `01` | ACK | OK | [PROVEN] |
| 2 | CPUCS halt pair again (in-downloader re-halt) | ACK | OK | [PROVEN] |
| 3 | **Stage-1 upload:** 360 × `0xA0` (wValue = record address, data ≤ 16 B) | 4476 payload bytes, addrs `0x0000–0x11EF` | `360 × 0xA0 … OK (4476 bytes)` | [PROVEN] |
| 4 | CPUCS run pair, data `00` | ACK → stage-1 executes | OK | [PROVEN] |
| 5 | Preamble `40 A4 00A1 0000 0000` | OEM sends `0xA4`, wValue `0x00A1` | `OK` | [PROVEN] |
| 6 | **Gate:** `C0 A9 0000 0000 0800` (8-byte read) | tag `C0`, VID `0F05`, PID `F235`, REV `AA07` → key `F235_AA07` → `Pakon7.hex` | `C0-05-0F-35-F2-07-AA-04` — all eight bytes exact | [PROVEN] |
| 7 | **Pakon7 phase 1:** 709 × `0xA3` (addr `> 0x1B3F`, CPU running) | 709 records / 10 128 B | `709 × 0xA3 … OK` | [PROVEN] |
| 8 | CPUCS halt pair | ACK | OK | [PROVEN] |
| 9 | **Pakon7 phase 2:** 19 × `0xA0` (addr `≤ 0x1B3F`, CPU halted) | 19 records / 198 B | `19 × 0xA0 … OK` | [PROVEN] |
| 10 | Final CPUCS halt pair | ACK | OK | [PROVEN] |
| 11 | Final CPUCS run pair | firmware executes → re-enumeration | OK | [PROVEN] |
| — | Enumeration poll (≤ 15 s) | `0F05:F135:0002` appears, cold path gone | `USB\VID_0F05&PID_F135\010-203-04  status=OK class=USBDevice service=WINUSB`; no `PID_F235` device remained | [PROVEN] |

Step-6 byte interpretation **[STATIC — F235Ldr.sys disassembly]**, confirmed
byte-for-byte **[PROVEN]**:

```
C0       tag 0xC0 → registry lookup branch (not the PknInit.hex fallback)
05 0F    VID 0x0F05 (LE)   35 F2  PID 0xF235 (LE)
07 AA    REV 0xAA07 (LE)   04    unused by the OEM parser [UNKNOWN meaning]
```

The returned REV `AA07` is the same `bcdDevice` the cold device exposes on
the bus (its hardware ID renders as `USB\VID_0F05&PID_F235&REV_::07` —
non-BCD nibbles `AA` display as `:`), so the gate answer cross-validates
against the enumerated identity **[PROVEN + STATIC]**.

The selected key implements the OEM registry chain
**[STATIC — `F235usb2.inf` `[WDGTLDR.AddServiceReg]`]**:

```ini
HKR,,FirmwareDirectory,,\SystemRoot\System32\F235Firmware
HKR,,F235_AA05,,Pakon5.hex
HKR,,F235_AA07,,Pakon7.hex
HKR,,F235_AA08,,Pakon8.hex
```

## 4. Image facts (static, verified by the run's host-side gates)

**Stage-1 (from `F235Ldr.sys`)** — embedded record buffer in `.data`,
file offset `0x1700` (VA `0x11700`), terminator at file `0x35F0`
(VA `0x135F0`) **[STATIC]**:

- 360 records, fixed stride `0x16`: `len, 0, addr(LE), flag, data`;
  all data records `flag=0`, payload bytes sum to **4476**, addresses
  `0x0000–0x11EF` (all `≤ 0x1B3F`, i.e. entirely in the `0xA0` domain);
- terminator record = `00 00 00 00 01` (flag 1) — checked, not assumed
  [PROVEN: gate passed].

**`Pakon7.hex` (Intel HEX → record buffer)** **[STATIC + PROVEN gates]**:

- 728 type-00 records, payload **10 326 B**, max address `0x478F`;
- phase split at `0x1B3F` — the boundary the OEM downloader uses: phase 1
  (`> 0x1B3F`) 709 records / 10 128 B via `0xA3` while the CPU runs;
  phase 2 (`≤ 0x1B3F`) 19 records / 198 B via `0xA0` while halted;
- **no record reaches `0xC000`** — the image never touches CPUCS
  (`0x7F92`/`0xE600`); the halt/run pairs are host-side only;
- device descriptor at `0x1000`:
  `12 01 00 02 00 00 00 40 05 0F 35 F1 02 00 01 02 03 01` → USB 2.0
  full-speed, `bMaxPacketSize0` 64, VID `0F05`, PID `F135`,
  **bcdDevice `0002`**, iManufacturer 1, iProduct 2, **iSerial 3**,
  1 configuration. The `REV_0002` match rule in both INFs is therefore
  satisfiable [STATIC], and `iSerial` explains the serial-bearing instance
  `…\010-203-04` [PROVEN];
- serial `010-203-04` does **not** occur as text anywhere in the image —
  it is assembled at run time [UNKNOWN source; our runs never touched the
  I2C/EEPROM];
- no `MSFT…`/WinUSB strings → the firmware declares **no** Microsoft OS
  (WCID) descriptors [STATIC].

**Phase-split provenance** [STATIC]: the `0x1B3F` boundary and the
`0xA3`/`0xA0` choice come from the F235Ldr downloader (disassembly
`0x10964`); CPUCS writes `0x7F92` then `0xE600` (disassembly `0x10922`).

## 5. Evidence chain (how confidence accumulated — four runs)

| Run | What was sent | Result | What it proved |
|---|---|---|---|
| 1 (probe, earlier session) | one gated `0xA9` read on the cold device | win32 `121` (timeout) | The FX2 boot ROM does not answer `0xA9` — expected, and the baseline for causality [PROVEN] |
| 2 — experiment “0xA4 preamble” | `40 A4 00A1` then `0xA9` | preamble OK; `0xA9` still `121` | The preamble **alone** is not the unlock [PROVEN] |
| 3 — stage-1 run | full stage-1 upload + run, then `0xA9` | `C0-05-0F-35-F2-07-AA-04` | The embedded stage-1 firmware is what makes `0xA9` answer [PROVEN] |
| 4 — full boot run (2026-10-07) | complete sequence of § 3 | F135 enumerated | The whole chain works end-to-end [PROVEN] |

## 6. Driver binding of the enumerated F135

Observed **[PROVEN]**: `class=USBDevice`, `service=WINUSB`, instance
`USB\VID_0F05&PID_F135\010-203-04`, `status=OK`.

Attribution:

- The OEM `F235usb2.inf` also matches this identity
  (`USB\VID_0F05&PID_F135&REV_0002`) but would bind **Class=Image,
  service `F135usb2`** — that is *not* what bound [STATIC + PROVEN
  absence]; the OEM package is evidently not installed on this machine.
- The firmware contains no WCID descriptors [STATIC], so the WinUSB
  binding did not originate in the device either.
- **[INFERRED]** the binder is *this repository's* `driver/PakonWinUSB.inf`:
  it matches exactly `USB\VID_0F05&PID_F135&REV_0002`, installs in-box
  `winusb.sys` (`service=WINUSB`, class `USBDevice`) and registers
  interface GUID `{0e9e6f29-e70a-4582-8d02-bde3ad701252}` — the same GUID
  the lab machine's cold device has exposed since the first probe session.
- **[PROVEN] (2026-10-08, descriptor scan):** the booted device's
  WinUSB interface path is
  `\\?\usb#vid_0f05&pid_f135#010-203-04#{0e9e6f29-e70a-4582-8d02-bde3ad701252}` —
  it carries exactly the GUID `identity_test` pins byte-identical to
  `driver/PakonWinUSB.inf`, so the binding INF declared *this repo's*
  `DeviceInterfaceGUIDs` value. The `[INFERRED]` binder above is thereby
  upgraded to `[PROVEN]` by the path+GUID chain
  ([F135_TOPOLOGY.md](F135_TOPOLOGY.md) § 3.1).
- The remaining attribution queries (`pakon-cli attrib` — no device open,
  no USB traffic) reported green by the operator (2026-10-08) but **not
  yet transcribed**: service, class + class GUID, hardware and
  compatible IDs, driver INF/provider.
- `FriendlyName` / `DeviceDesc`: **[PENDING]** — neither the boot
  transcript nor a pasted `attrib` output has captured them; the
  transcription fills these rows.

If the inference holds, the boot milestone simultaneously validates the
repo's WinUSB package decision: the warm device bound through the exact
INF rule written for it before the hardware existed (`driver/README.md`
“warm F-135/F-135+” row).

## 7. Safety properties (why this run cannot have bricked anything)

- Every write was volatile: FX2 RAM (`0xA0`/`0xA3`) and the halt/run
  registers (`0x7F92`, `0xE600`). **Unplug/replug restores cold state.**
- No EEPROM/I2C (`0xA2`-class) access, no bootloader addresses
  (`0x22`/`0x26`/`0x42`/`0x46`), no type-byte-0 PPB frames, no bulk
  traffic — nothing in the sequence can touch persistent state.
- Record addresses were gated `< 0xC000` (CPUCS) host-side before any
  transfer; records ≤ 16 B; terminators verified; both artifact MD5s
  pinned; gates abort (never retry); exactly **one** gated `0xA9` per run.
- The read-only probe (`pakon-cli probe`, `bootstrap/probe.*`) was not
  modified.

## 8. Residual [UNKNOWN]s (deliberately open)

1. Byte `[7] = 0x04` in the `0xA9` reply — unused by the OEM parser.
2. Whether the `0xA4` preamble (wValue `0x00A1`) is *required* once
   stage-1 runs — not ablated; the reference sequence includes it because
   the OEM does.
3. How the serial `010-203-04` is derived at run time (not in the image).
4. The identity of the installed INF that bound WinUSB (§ 6; query ready).
5. Whether the OEM report software expects the Image-class `F135usb2.sys`
   stack rather than WinUSB — outside bootstrap scope, flagged only.

## 9. Related

- Frozen, byte-exact procedure: [`tools/pakon_boot_reference.ps1`](../tools/pakon_boot_reference.ps1)
  (any future implementation must reproduce its transcript).
- Port design (components, tests, rollout): [LOADER_DESIGN.md](LOADER_DESIGN.md).
- Prior evidence survey (§ 3 gaps now closed by OEM-artifact RE + this
  run): [BOOTSTRAP.md](BOOTSTRAP.md).
- Repo state, test counts, next steps: [STATUS.md](STATUS.md).
