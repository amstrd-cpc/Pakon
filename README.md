# Pakon — native stack for the Kodak/Pakon F-135 / F-135+ film scanner

A from-scratch **C++20 / CMake** implementation of everything between a
USB port and a scan session: device enumeration, WinUSB transport, the
PPB protocol layer, scanner session logic, and a command-line tool —
plus the reverse-engineered **cold→warm firmware boot chain**, validated
end-to-end on real hardware (2026-10-07).

Three rules shape everything here:

- **Nothing is invented.** Every protocol claim is cited to the primary
  spec (`pakon-reference`) or the `alibosworth/pakon-captures` corpus and
  carries an evidence marker — `[DOCUMENTED]`, `[CONFIRMED]`, `[STATIC]`,
  `[PROVEN]`, `[INFERRED]`, `[UNKNOWN]` — so you can always see how far
  the evidence reaches.
- **Hardware is treated carefully.** All writes stay volatile, safety
  prohibitions are test-pinned, and hardware runs happen as single
  explicitly-approved commands whose output is recorded verbatim.
- **Firmware bytes never enter this repository** (scope rule of the
  primary spec). The boot procedure fetches the OEM artifacts at run
  time and MD5-gates them host-side before any USB operation.

## Where things stand

| Area | State |
|---|---|
| Build & tests | **4 CTest suites / 45 cases**, green on Linux/GCC (2026-10-07); MSVC Release target with `/W4 /permissive-`, same suite |
| USB layer | Two-pass SetupAPI enumeration (finds even Code-28 units), WinUSB transport, one shared overlapped-open path — **cold and warm bindings both observed on hardware** |
| Bootstrap probe | `pakon-cli probe` — exactly one read-only `0xA9` read — **hardware-validated**: cold ROM baseline `win32 121`, answers `C0-05-0F-35-F2-07-AA-04` once stage-1 runs |
| Descriptor discovery | `pakon-cli descriptors` — raw device + configuration descriptors, full interface/endpoint topology, strings, WinUSB cross-check; standard `GET_DESCRIPTOR` only — **recorded on hardware 2026-10-08** (live device descriptor byte-identical to the Pakon7 image) in [docs/F135_TOPOLOGY.md](docs/F135_TOPOLOGY.md) |
| Cold→warm boot chain | **Validated on hardware 2026-10-07**: stage-1 upload → `0xA4` preamble → gated `0xA9` → Pakon7 download (709 × `0xA3` + 19 × `0xA0`) → final run → re-enumeration as `0F05:F135` — full evidence record in [docs/BOOT_CHAIN.md](docs/BOOT_CHAIN.md) |
| C++ loader | **Designed, not yet written** — [docs/LOADER_DESIGN.md](docs/LOADER_DESIGN.md); the frozen PowerShell procedure ([tools/pakon_boot_reference.ps1](tools/pakon_boot_reference.ps1)) remains the trusted reference until each ported step is re-validated |
| PPB + scanner session | Implemented and replay-verified against captures (198 425 frames; scripted transport fails on any off-script request); **first on-hardware PPB exchange 2026-10-08**: connect handshake + presence probes OK, module-info reply well-formed but its documented `0x88` READ-flags byte hit a handling gap in `is_success` ([docs/STATUS.md](docs/STATUS.md)) |
| Phases 5–10 (init/scan/teardown, film transport, imaging, decode, calibration read, output) | Not implemented yet |
| Driver package | [driver/PakonWinUSB.inf](driver/PakonWinUSB.inf) — in-box `winusb.sys`, both device identities, interface GUID `{0e9e6f29-…}`; [driver/README.md](driver/README.md) |

Latest full status, including open questions and what is still
untested: **[docs/STATUS.md](docs/STATUS.md)**.

## Quick start

### Build & test (Linux / any host)

```sh
cmake -S . -B build -G Ninja        # CMake ≥ 3.20, C++20; any generator
cmake --build build
ctest --test-dir build --output-on-failure   # expect: 4/4 suites, 45 cases
```

### Build & test (Windows / MSVC)

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure   # expect: 4/4, 45
```

### The CLI

```
pakon-cli [--log trace] <command>

  list       enumerate attached Pakon scanners (no I/O sent)
  attrib     PnP/driver attribution report (read-only property queries)
  descriptors  read-only USB topology of the booted F135: device +
             configuration descriptor bytes, every interface/endpoint,
             strings (standard GET_DESCRIPTOR reads + WinUSB queries)
  probe      read-only bootstrap probe: one documented 0xA9 control read
  identify   open a PPB session and detect the scanner model
  status     identify + read-only status polls and register reads
```

Binaries: `build/apps/pakon-cli/pakon-cli` (single-config) or
`build\apps\pakon-cli\Release\pakon-cli.exe` (MSVC). `--log trace`
prints full TX/RX packet hex dumps.

## Hardware safety charter (enforced by code and tests)

- EEPROM/I2C vendor paths, bootloader addresses and type-byte-0 PPB
  frames have no call site in the bootstrap path — pinned as forbidden
  by `bootstrap_probe_test` and `usb_identity_test`.
- The boot sequence writes FX2 RAM and the halt/run registers only, with
  addresses gated below `0xC000` — **unplug/replug always restores the
  cold device**.
- Gates abort, never retry; exactly one gated `0xA9` per boot; both
  firmware artifacts are MD5-verified host-side before any USB traffic.
- `tools/pakon_boot_reference.ps1` is a frozen, proven procedure — run
  it only as an explicitly approved experiment.

## Repository map

```
apps/pakon-cli/       CLI: list / attrib / probe / descriptors / identify / status
src/pakon/
  usb/                IUsbTransport, SetupAPI enumeration, WinUSB
                      backend, identity rules, driver attribution
  bootstrap/          read-only probe (stage-1 personality read)
  ppb/                frame serialize/parse + session client
  scanner/            session state machine + model detection
  protocol/           header-only constants (addresses, commands)
  errors/ logging/    Result<T> error types; leveled logger + hex dumps
tests/                4 CTest suites, 45 cases — no hardware required
driver/               PakonWinUSB.inf + signing/install notes
tools/                frozen boot reference (approved runs only)
docs/                 all documentation (below)
```

Layering rules (who may call whom, and what each layer may emit) are in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Documentation — recommended reading order

1. **[docs/STATUS.md](docs/STATUS.md)** — what is done, in progress,
   blocked and unknown. Start here.
2. **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** — the layers and
   the rules between them.
3. **[docs/BOOT_CHAIN.md](docs/BOOT_CHAIN.md)** — the hardware-validated
   cold→warm boot milestone: exact requests, gate bytes, artifact MD5s,
   per-claim evidence markers.
4. **[docs/LOADER_DESIGN.md](docs/LOADER_DESIGN.md)** — how the boot
   sequence becomes reusable C++ components, and the rollout plan.
5. **[docs/USB.md](docs/USB.md)** and
   **[docs/BOOTSTRAP.md](docs/BOOTSTRAP.md)** — transport decisions;
   the probe's evidence trail.
6. **[docs/PPB.md](docs/PPB.md)** and
   **[docs/SCANNER.md](docs/SCANNER.md)** — protocol layer and session
   layer, both capture-verified.
7. **[docs/PAKON_REFERENCE.md](docs/PAKON_REFERENCE.md)** — how the
   primary external spec maps into this repository, gap by gap.
8. **[docs/WINUSB_TEST.md](docs/WINUSB_TEST.md)** — physical WinUSB
   binding procedure and its evidence table.
9. **[docs/F135_TOPOLOGY.md](docs/F135_TOPOLOGY.md)** — the read-only
   descriptor/topology scan of the booted runtime: method, safety
   envelope, observation record.

## Hardware record (lab unit)

| Run | Result | Record |
|---|---|---|
| WinUSB binding, cold unit | probe opens through interface GUID `{0e9e6f29-…}` | docs/BOOTSTRAP.md § 6 |
| Cold `0xA9` baseline | `win32 121` — boot ROM silent (expected) | docs/BOOT_CHAIN.md § 5 |
| Stage-1 upload → `0xA9` | answered `C0-05-0F-35-F2-07-AA-04` | docs/BOOT_CHAIN.md § 5 |
| **Full boot, 2026-10-07** | all 11 steps OK → `USB\VID_0F05&PID_F135\010-203-04`, `status=OK`, `service=WINUSB` | docs/BOOT_CHAIN.md § 3 |
| **Descriptor scan, 2026-10-08** | `descriptors` — device descriptor byte-identical to the Pakon7 image @ `0x1000`; 1 vendor interface, bulk `0x01`/`0x81`/`0x86` @ 512; strings incl. serial `010-203-04`; interface path carries this repo's GUID | docs/F135_TOPOLOGY.md |
| **First PPB session, 2026-10-08** | `identify` — connect handshake + probes `0x44`/`0x24` answered; module-info reply well-formed (12 B) but `0x88` READ-flags byte rejected by `is_success` → stopped (payload uninterpreted) | docs/STATUS.md |

Not yet run on hardware: the CLI's `list` and `status` commands. (`attrib`,
`descriptors` and `identify` ran 2026-10-08 — see
[docs/STATUS.md](docs/STATUS.md) and [docs/F135_TOPOLOGY.md](docs/F135_TOPOLOGY.md).)

## External references

- **Primary spec:** `pakon-reference` (external repository) — mapped in
  [docs/PAKON_REFERENCE.md](docs/PAKON_REFERENCE.md).
- **Capture corpus:** `alibosworth/pakon-captures` — replayed by the
  test suite.
- **OEM artifacts** (fetched + MD5-pinned at run time, never committed):
  the public `plonsker/pakon-scanning-software` mirror of the Pakon
  F-135 driver install.

This repository re-derives and cites; it does not redistribute firmware
and is not affiliated with the OEM.
