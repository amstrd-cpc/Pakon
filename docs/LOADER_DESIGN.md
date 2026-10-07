# Loader design note: porting the validated boot sequence into the repo

**Status 2026-10-07 — DESIGN ONLY.** No loader code exists in this
repository yet. The sequence it will implement is proven hardware
([BOOT_CHAIN.md](BOOT_CHAIN.md)); the executable reference is
[`tools/pakon_boot_reference.ps1`](../tools/pakon_boot_reference.ps1)
(frozen — its transcript is the specification). This note says how the
port decomposes into reusable components *before* any of it is written.

**Governing rule: do not casually rewrite working boot logic.** The port
must issue the same requests, in the same order, with the same gates and
abort semantics as the reference; any deviation is a design change that
needs its own evidence.

## 1. Goals and non-goals

Goals

- Decompose the proven sequence into reusable, individually testable
  components (§ 2) that reuse the existing `usb` layer unchanged.
- Byte-equal request sequence vs. the reference fixture, verified by a
  replay test (the pattern already used by `scanner_replay_test`).
- Host-side validation before any USB operation (same gates as the
  reference script), abort-not-retry, one gated `0xA9` per boot.
- Windows/MSVC is the target; everything non-transport stays
  platform-independent and unit-testable anywhere.

Non-goals (v1)

- No EEPROM/I2C access, no bootloader addresses, no type-byte-0 frames —
  the safety list in BOOT_CHAIN.md § 7 carries over verbatim.
- No `PknInit.hex` fallback path (post-gate tags other than `C0`/`C2`
  abort the run; the OEM fallback is documented, not implemented).
- No firmware bytes committed to the repo (scope rule, BOOTSTRAP.md § 3.1):
  artifacts are fetched at run time and MD5-gated, exactly like the
  reference script.
- No PPB/bulk traffic, no report-app emulation, no non-Windows loader
  (transport stub stays a stub).

## 2. Component decomposition

Six components plus the orchestrator. “Reuse” names what exists today;
nothing in `usb/` or `ppb/` is to be rewritten for this.

### 2.1 Firmware artifact loading + validation — `bootstrap/firmware_artifact`

*Pure host-side; no USB involvement.*

- Fetch (or open a local path) the two pinned artifacts: `F235Ldr.sys`
  (MD5 `6be781…`) and `Pakon7.hex` (MD5 `07f500…`); verify MD5 before
  anything else. (Small RFC 1321 implementation with published test
  vectors, or the platform crypto API — decide at implementation time;
  the *digests themselves* are pinned constants.)
- Extract the stage-1 record buffer: `F235Ldr.sys[0x1700..0x35EF]`
  (360 × `0x16`), terminator `0x35F0..0x3605 == 00 00 00 00 01`, payload
  sum 4476, addrs ≤ `0x1B3F`.
- Parse `Pakon7.hex` (Intel HEX) into the OEM record buffer
  (`len, 0, addr_le, flag, data`, stride `0x16`, terminating record) and
  apply the shape gates: 728 records / 10 326 B, split 709/10 128 vs
  19/198 at `0x1B3F`, max addr `0x478F`, no addr ≥ `0xC000`, no unexpected
  record type.
- Output: validated value types (`Stage1Image`, `FirmwareImage`) that
  only exist if every gate passed — the loader accepts these types, not
  raw bytes.

*Reuse:* the Intel HEX logic is a direct port of the proven PowerShell
parser (same assertions, same order). *Tests:* synthetic mini-images for
the parser/gates (real firmware bytes stay out of the repo), published
MD5 vectors, terminator/negative cases.

### 2.2 Stage-1 transport — `bootstrap/stage1`

- CPUCS halt pair (`0xA0` `0x7F92`=01 → `0xA0` `0xE600`=01), twice
  (once pre-upload, once in-downloader re-halt); write all 360 records as
  `0xA0` (wValue = record address, data = record payload); CPUCS run pair
  (00 → 00).
- Depends only on `usb::IUsbTransport`'s vendor control write — no
  Windows headers.

*Tests:* fake transport asserts the exact request sequence (count, bRequest,
wValue list, payloads) and that nothing else is issued.

### 2.3 `0xA4` / `0xA9` handshake — `bootstrap/handshake`

- Preamble: `40 A4 00A1 0000 0000`.
- Gated read: `C0 A9 0000 0000 0800`; verify all eight bytes
  `C0 05 0F 35 F2 07 AA 04`-shaped: tag ∈ {`C0`, `C2`}, VID `0F05`,
  PID `F235`, REV present; build the personality key
  (`F235_AA07`); map key → artifact (`F235_AA05/07/08 → Pakon5/7/8.hex`,
  the OEM registry rule from `F235usb2.inf`).
- Any mismatch → typed error, **abort, no retry, no download**.
  Byte `[7]` is carried through as opaque data (its meaning is UNKNOWN).

*Tests:* golden success vector, every single-byte mutation rejected,
short read rejected (mirrors `bootstrap/probe_test.cpp` discipline).

### 2.4 Pakon7 phase loading — `bootstrap/pakon7_loader`

- Phase 1: 709 × `0xA3` (addr `> 0x1B3F`, CPU running) — after the
  handshake leaves the stage-1 loader executing.
- Halt pair → phase 2: 19 × `0xA0` (addr `≤ 0x1B3F`) → final halt pair →
  final run pair.
- The phase split is computed from the address (not baked-in counts);
  the *gates* in 2.1 pin the counts so a wrong image never reaches here.

*Tests:* replay of the proven request sequence against a fake transport;
boundary record `0x1B3F`/`0x1B40` routing.

### 2.5 F135 enumeration wait — `bootstrap/enumerate_wait`

- After the run pair, poll `usb::enumerate()` (already driver-independent)
  until a `warm_f135` device appears (identity classification already
  exists in `usb/identity.hpp`), with a timeout and an honest
  “not seen in N s” report.
- Report what bound: this is where `usb::collect_driver_attributions()`
  (the new read-only diagnostic) supplies service/class/desc so the boot
  log records the binding, not just the presence.

*Tests:* fake enumeration sequence (cold gone, warm appears / never
appears → timeout wording).

### 2.6 Runtime communication layer — existing `usb` (unchanged)

- Reuse `IUsbTransport`, `win_usb_transport`, `kWinUsbOpenParams`,
  `kDeviceInterfaceGuid` as-is. The booted device opens through the same
  WinUSB path as the cold probe did; post-boot PPB work stays in
  `scanner`/`ppb` and is out of scope here.

### Orchestrator — `bootstrap/boot_session`

A state machine mirroring reference steps 1–11 exactly (one state per
step, structured log line per transition, `--log trace` hex dumps via the
existing logger). Failure semantics: log the failing step + Win32 error,
return `Result` — never loop, never “try again”, never continue past a
closed gate.

## 3. Safety invariants (compile-time visible)

- Write helpers accept only the address sets the reference uses
  (RAM `< 0xC000`, plus the two CPUCS registers); EEPROM (`0xA2` I2C
  addresses), bootloader addresses and type-byte-0 frames have no call
  site in this path (as `probe_test` pins them today).
- `bootstrap/probe` remains exactly what it is — the read-only probe is
  not modified, absorbed or widened by the loader.
- Exactly one gated `0xA9` per boot session (structurally enforced: the
  gate lives in the orchestrator between phase 0 and the download, and
  has no caller after it).

## 4. Test strategy

| Layer | Method |
|---|---|
| Request sequence | fake transport records every control request → byte-compare against the reference transcript (same pattern as `scanner_replay_test`) |
| Parsing/gates | synthetic HEX fixtures + negative cases; MD5 published vectors |
| Identity of output types | a `FirmwareImage` can only be constructed by the gated parser (constructor is private to the artifact module) |
| Existing suites | must stay green: 4 ctest suites / 45 cases before and after |
| Hardware | after unit green: re-run the frozen reference, then the C++ path as single approved experiments — outputs compared side by side |

## 5. Rollout (each step = small reviewable change + explicit go-ahead)

1. `firmware_artifact` + gates (pure host-side; no USB code touched).
2. `stage1` + handshake against the fake transport; replay test green.
3. Hardware validation of 1–2 (single approved experiment).
4. `pakon7_loader` + orchestrator; replay test green; hardware validation.
5. `enumerate_wait` + `attrib` wiring; full boot via the C++ path.
6. CLI surface (`pakon-cli boot`, or library-only — open question) and
   docs/STATUS update.

Until a step's hardware validation lands, the reference script remains
the only trusted implementation of that step.

## 6. Open questions

- MD5 implementation choice (own RFC 1321 vs platform API).
- Artifact source: always fetch the pinned URL vs accept a local path
  (reference does both: fetch → MD5 gate).
- CLI shape: `pakon-cli boot` subcommand vs library-only for v1.
- Whether the boot transcript should be persisted as a machine-readable
  JSON side-by-side with the reference output (nice for § 4 comparison).
