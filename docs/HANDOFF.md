# Handoff — continuing on the scanner PC

Written 2026-10-10 for a Claude Code session running on the Windows PC the
F-135+ (serial 17373, USB serial 010-203-04) is attached to. Read this,
then [STATUS.md](STATUS.md), then [OEM_RE.md](OEM_RE.md) (the single
device/protocol reference, evidence-tagged), then
[HARDWARE_RUNBOOK.md](HARDWARE_RUNBOOK.md).

## Goal

1. Now: `pakon-cli scan` produces correct images on this unit, all modes
   (Base 4/8/16 × IR).
2. Long term: modern software for the Pakon series incl. Digital ICE and the
   Ansel/"Pakon look" pipeline. Not started; seams in OEM_RE §13. Keep the
   raw output lossless (`.pakraw` + `.scan.json` sidecar) and keep
   processing out of the scan runner.

## Hard safety rules (non-negotiable, enforced in code and tests)

- **Never write the EEPROM**: no `0xA2` at any wIndex, no write-select on
  `0xA4`. Reads only (`eeprom::is_allowed`, plus the WinUSB backend guard).
- Never address the PIC bootloader addresses `0x22/0x26/0x42/0x46`, or
  `0xA2/0xA4` on the PPB bus. Keep the allow-list and its tests.
- No PIC firmware flashing, ever. Firmware-update paths in
  TLXClientDemo/TLB are documented, never implemented. FX2 RAM boot only.
- Clamp LED currents (PICL `0x81`, payload `[B, IR, R, _, G]`) to
  `LED_CEILINGS` for the probed board and IR state; strictest row when
  unknown. Corrections output is clamped too.
- Every code path that lights the lamp or moves the motor has a guaranteed
  teardown (acquire off, lamp off, FIFO reset, DX stop, `0xA2`, rate 0 →
  go → `0xA2`, panel LEDs idle) that also runs on error, exception and
  Ctrl+C.
- TEC writes replay OEM-recovered values only.
- No value from unit 16402 where this unit's EEPROM can supply it, unless
  marked as a fallback.
- **Live scans light the lamp and move film: run them only when the user is
  present and has said go for that specific run.** Read-only commands
  (`list`, `identify`, `status`, `eeprom`, `scan --dry-run`) are fine.
  Film is loaded only after `corrections: final` appears (calibration must
  see an open gate).

## Repo conventions

- Small focused commits; evidence citations in messages and comments
  (`[DIS TLB@addr]`, `[CAP base4.jsonl:t]`, `[DYN run]`, hardware date).
- Zero warnings (MSVC `/W4 /permissive-`, GCC/Clang `-Wall -Wextra`),
  tests green at every commit.
- Commit trailer: `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Docs: no duplicated claims; device facts only in OEM_RE.md; STATUS.md
  short and current.

## Build and test on this PC (MSVC)

```powershell
$cmake = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake -S . -B build                      # once
& $cmake --build build --config Release
$ctest = Join-Path (Split-Path $cmake) ctest.exe
& $ctest --test-dir build -C Release --output-on-failure
$p = ".\build\apps\pakon-cli\Release\pakon-cli.exe"
```

The simulator suites (`sim_scan`, `sim_device`, `image_stream`, `eeprom`)
run on Windows too; `pakon_sim_server` and the Wine tooling are Linux-only.
The Python tool tests need Python 3 on PATH. The portable sources were also
syntax-checked against libc++ to catch MSVC missing-include errors; if MSVC
reports a missing `std::` name, add the include.

## Hardware results so far (2026-10-10)

| Step | Result |
|---|---|
| identify | F-135+; module page `12345`; `Version USB 0x03,0x0F Lamp 0x05,0x0A Motor 0x05,0x06` (= OEM log) — root cause F4 confirmed |
| status | temperature `81 02 71 01` (369/16 ≈ 23 °C) |
| eeprom | sections A/B primary, CRC good; serial 17373, type 1351, hw 400; Offset 35/70/69 (Base 4/8/16); base4 speed 25676, IR 19240; adjust 1000/1008 |
| first light base4 | warm-up 6.8 s; dark 3 rounds → 289/292/312; currents R2 G3 B2; on-times 0.983/0.942/0.507; 0 overflows |
| first light base4-ir | currents R3 G6 B3 IR4; on-times 0.973/0.906/0.478/0.937; 18 duty rounds (blue clipped at 65534 for 12) |
| film base4, cap 3000 | line period 0.904 ms (expected 0.900); ended at cap (film not yet at gate); panel left blinking — fixed in `4e933f9` |
| film base4, cap 15000 | film lines 3211..13119, ended by the density detector; 0 overflows/resyncs; DX service events every poll while film passes; dark servo did NOT settle (issue 1) |

The user keeps the console logs and outputs (`fl1.*`, `fl1ir.*`, `film1.*`,
`film2.*`) in the repo folder on this PC; ask for them.

## Open issues, in priority order

1. **Dark servo instability (film2 run).** With an unchanged offset, the
   masked-pixel dark mean jumped by ~128 between rounds (G 271, 247, 251,
   121, 249, 121, 249). First lights settled cleanly. Hypothesis: stray
   light from the DX reader LEDs (DX is started at calibration start) if
   the strip was already in the entry slot — ask the user when the strip
   was inserted; inspect `film2.scan.json` dark profile; consider starting
   DX only for the film window, and logging per-round spread
   (`src/pakon/scan/corrections.cpp`, dark loop).
2. **Film exposure headroom.** Calibration ends at low LED currents and
   high on-times (R 0.98, G 0.94, B 0.51), so the film boost
   (R1.39 G2.51 B5.18, `kBoost*` in `src/pakon/scan/runner.cpp`) clamps at
   on-time 1.0 — film is underexposed, blue by ~1.4 stops. The OEM (unit
   16402) ended with higher currents and short on-times. Resolve with
   evidence: **USBPcap capture of the OEM stack on this unit**
   (HARDWARE_RUNBOOK §9; this PC has the OEM software), then
   `tools/pcap_to_jsonl.py` + `tools/compare_sessions.py`; read the OEM's
   `0x81` currents and `0x82` on-times for calibration and film.
3. **Validate the rest:** base4 film with the strip inserted after
   `corrections: final`; base4-ir film; base8 (`--film-rows 20000`),
   base16 (`--film-rows 30000`) with and without IR; one Ctrl+C mid-pass.
4. **Duty servo slow when clipped** (peak 65534 → ×63968/65534 per
   round). Halve the on-time while clipped; compare with the OEM capture.
5. **Feed prompt**: print a clear "feed the strip now (45 s)" line when the
   film window starts.
6. **DX decoding** (frame numbers, film type) from the `0x90` records the
   runner already sees — useful for the pipeline.
7. **OEM dynamic run** (OEM_RE §11) needed KODAKCMS.dll, ekjpegi.dll,
   xerces-c_2_2_0.dll, absent from the OEM bundle — search this PC
   (`C:\`, System32, the PSI/TLX install dirs). On this PC the OEM stack can
   run natively against the real scanner (with the OEM driver) for the
   USBPcap capture.
8. Lower: EEPROM backup read only on CRC failure (match OEM); C++ FX2 loader
   (LOADER_DESIGN.md); stream film lines to disk instead of RAM.

## Where things are

- Scan path: `src/pakon/scan/` (runner, device + teardown, corrections,
  lines, film detector, modes, sidecar), `src/pakon/stream/`,
  `src/pakon/eeprom/`, `src/pakon/protocol/scan_commands.hpp`,
  WinUSB pipe in `src/pakon/usb/win_usb_transport.cpp`, CLI in
  `apps/pakon-cli/scan_cli.cpp`.
- Simulator: `tests/support/sim_device.*`; scan tests
  `tests/scan/sim_scan_test.cpp` (fault sweep, exception, slow consumer).
- References (public, clone next to the repo if needed):
  `alibosworth/pakon-captures` @ cf41ba6 (OEM USB captures, unit 16402),
  `alibosworth/pakon-reference` @ 76d9cd0,
  `pablonavarrob/pakon-tlx-macos` @ cd925d3 (bridge, pkusb shim,
  base-config.reg).
- Not in the repo (proprietary decompilation, transferred separately by the
  user as `pakon-re-bundle.tar.gz` if needed): Ghidra output of TLB.dll,
  tlx.dll, TLXClientDemo.exe, F135usb2.sys (`TLB.annot.c` is TLB with
  error-reporter call sites labelled), Wine run logs. Addresses in OEM_RE.md
  refer to these binaries (MD5s in the OEM_RE.md header).
