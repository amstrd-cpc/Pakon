# Status

Last updated: 2026-10-10. Device and protocol facts live in
[OEM_RE.md](OEM_RE.md) (single reference); this page only says what works,
how it is verified, and what is open.

## Works, verified in this repository

| Area | Verification |
|---|---|
| Cold→warm boot chain (FX2 RAM boot only) | hardware 2026-10-07, [BOOT_CHAIN.md](BOOT_CHAIN.md) |
| `list`, `descriptors`, `probe`, `identify`, `status` | hardware 2026-10-08/09 |
| `identify` OEM pre-init (`0x97=01`, `0x03=01` before each `0x07`) | **hardware 2026-10-10**: module page now `12345` (= corpus), `Version USB 0x03,0x0F Lamp 0x05,0x0A Motor 0x05,0x06` (= this unit's OEM log) |
| Read-only EEPROM reader, compile-time allow-list, CRC, primary/backup, marked 16402 fallback (`pakon-cli eeprom`) | **hardware 2026-10-10**: both sections primary, CRC good; serial 17373, type 1351, hw 400; Offset 35/70/69, base4 speed 25676/19240, adjust 1000/1008 |
| First light, Base 4-IR | **hardware 2026-10-10**: currents R3 G6 B3 IR4, on-times 0.973/0.906/0.478/0.937 (IR target ~40000 met), 0 overflows, teardown acknowledged |
| Film pass mechanics, Base 4, 3000-row cap | **hardware 2026-10-10**: line period 0.904 ms (expected 0.9), motor rate 25676 from EEPROM, 0 overflows/resyncs, ended at the row cap, teardown acknowledged; panel was left in the film-end state (fixed: teardown now ends with panel LEDs idle) |
| First light, Base 4 (lamp, Corrections, 256 white lines, teardown) | **hardware 2026-10-10**: warm-up 6.8 s (lamp-ready event 0x02), dark 3 rounds → 289/292/312, currents R2 G3 B2, on-times 0.983/0.942/0.507, gains 13; 0 overflows, 0 resyncs, teardown all acknowledged |
| Concurrent EP6 stream: 12 queued 20 KiB reads into a 32 MiB ring, overflow = error | unit + simulator tests; WinUSB overlapped pipe (RAW_IO) compiled for Windows, hardware pending |
| Scan runner on the OEM state machine: init block (byte-identical to capture), warm-up via service events, Corrections closed loop, host-ended windows, film-end detector + row cap, teardown exactly once on every path | simulator: all six modes, first light, late film, no film, slow consumer, corrupt EEPROM, fault at every frame |
| `pakon-cli scan --dry-run/--live-scan`: `.pakraw` + TIFF preview + `.scan-stats.txt` + `.scan.json` sidecar | CLI gating tests (no device opened on usage/dry run) |
| Session tools `pcap_to_jsonl.py`, `compare_sessions.py` | synthetic USBPcap tests; stack-vs-OEM diff explained in OEM_RE §11.3 |
| OEM stack under Wine against the simulator | runs through connect/probe/EEPROM/identify; stops at Kodak CMS init (OEM_RE §11.2) |

Builds: GCC Debug, clang ASan+UBSan, clang TSan (`PAKON_SIM_SPEED=0.5`),
MinGW-w64 x86_64 cross build (`cmake/mingw-w64-x86_64.cmake`, all 13 test
exes also pass under Wine) — zero warnings, 14 CTest entries (107 C++ cases,
9 Python cases).

## Not verified on hardware yet

The IR first light and every film pass (motor, film detection, line period,
exposure). The next live steps are in
[HARDWARE_RUNBOOK.md](HARDWARE_RUNBOOK.md), in order; each one has its
expected output and the failure signatures to look for.

## Riskiest assumptions (ranked)

1. **Teardown tail `rate=0 → go → 0xA2`** (kept from the bridge per the
   safety rules). The OEM never writes a rate below 1000; if the PIC
   rejects it the teardown reports a failed step (exit 1), the lamp and
   acquire are already off by then.
2. **Film on-time boost R1.39 G2.51 B5.18** comes from one capture's ratios,
   not from TLB code. Wrong values mean a mis-exposed film pass, not harm.
3. **Film start/end detector** is this stack's own (TLB's is unknown); the
   row cap bounds it.
4. **LED clock 0.24 per integration unit** (on-time base) is fitted to the
   captures; the register that supplies it is unknown.
5. **DX words and lamp temperature/TEC values** are replayed from the
   capture, not derived.
6. **Simulator optics** (dark/white levels, LED response) are a model; the
   Corrections servo is tested against it, not against this unit.
7. **This unit's history**: its OEM logs show `EC_FilmInGuides` ×392 and
   `EC_HardwareFault 0x40000100/0x80000100` ×405 (lamp warning path).
   First light will show whether the lamp side is healthy.

## Open

- OEM_RE §12 open questions (LED clock, unexplained registers, DX formula,
  film-end algorithm, the lamp/TEC/Corrections/ScanPictures sequence not
  yet observed dynamically — needs the real KODAKCMS/ekjpegi/xerces DLLs).
- Image pipeline (flat-field, Digital ICE, colour/"Pakon look"): not
  started; the seams are recorded in OEM_RE §13 and the raw output keeps
  everything it needs.
- C++ FX2 loader ([LOADER_DESIGN.md](LOADER_DESIGN.md)); the PowerShell
  reference procedure remains the boot path.
