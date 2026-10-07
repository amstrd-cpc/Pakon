# Architecture

Native C++20/CMake stack for the Kodak/Pakon F-135 / F-135+ film scanner.
`pakon-reference` (analysed in [PAKON_REFERENCE.md](PAKON_REFERENCE.md)) is the
primary specification; nothing in this repository invents protocol.

## Layers

```
apps/pakon-cli                command-line tool (list / probe / identify / status)
        │
src/pakon/scanner             session + state machine + model detection
        │
src/pakon/ppb                 frame serialization, reply parsing, session client
        │
src/pakon/usb                 IUsbTransport abstraction + backends
        │
      scanner hardware (FX2 bridge)
```

`src/pakon/bootstrap` is a sibling side path: the CLI's `probe` command
talks straight to `usb::IUsbTransport` through evidence-pinned read-only
helpers (a cold device has no PPB stack yet — see
[BOOTSTRAP.md](BOOTSTRAP.md)).

Cross-cutting: `errors/` (Result/Error types), `logging/` (leveled log + packet
hex dumps), `protocol/` (header-only constants: bus addresses, command bytes —
no logic).

Rules enforced by this layering:

- `ppb` never includes Windows headers; it talks to `usb::IUsbTransport` only.
- `scanner` never builds raw frames; it uses `ppb::Client`.
- `bootstrap` may issue only the documented stage-1 personality read
  (`0xA9`, `wIndex 0`) — no bulk, no control writes (test-pinned).
- `protocol/` constants are documented against specific pakon-reference files
  (citations in the headers).

## Directory map

```
CMakeLists.txt                 top level (C++20, /W4 on MSVC, -Wall -Wextra)
src/pakon/
  errors/error.hpp             ErrorKind, Error, Result<T> (std::expected when
                               available, minimal identical fallback when not)
  logging/logger.hpp           leveled logger, TX/RX hex dumps (trace level)
  protocol/addresses.hpp       bus addresses + safety notes
  protocol/commands.hpp        command bytes per controller (address-disambiguated)
  usb/transport.hpp            DeviceInfo, IUsbTransport, enumeration API
  usb/identity.hpp             hardware-ID recognition, PnP instance parsing,
                               shared DeviceInterfaceGUID (unit-tested on any host)
  usb/enumerate_win.cpp        SetupAPI: PnP device tree + device interfaces
                               (driver-independent — finds Code 28 devices)
  usb/win_usb_transport.cpp    WinUSB backend (Windows)
  usb/transport_stub.cpp       non-Windows stub (protocol tests still build)
  bootstrap/probe.hpp          stage-1 personality read constants + probe API
                               (evidence-pinned, read-only — BOOTSTRAP.md)
  bootstrap/probe.cpp          the single permitted control read, nothing else
  ppb/packet.hpp               Frame serialize/parse, reply parsing, builders
  ppb/client.hpp               exchange() + destination allow-list
  scanner/scanner.hpp          connect / identify / status, State/Model enums
apps/pakon-cli/                CLI
driver/                        WinUSB INF package (installs in-box winusb.sys;
                               no binaries, no firmware — see driver/README.md)
tests/                         self-contained harness (no test framework dep)
  support/replay_transport.hpp mock transport fed with captured request/reply
  ppb/packet_test.cpp          frame vectors from pakon-reference + captures
  scanner/scanner_replay_test.cpp  connect/identify/status replay
  usb/identity_test.cpp        hardware-ID recognition + INF/GUID consistency
  bootstrap/probe_test.cpp     probe request pinning + one-read-only-I/O proof
docs/                          this documentation set
  BOOTSTRAP.md                 cold→warm evidence status + probe procedure
  WINUSB_TEST.md               pending physical WinUSB binding test (procedure)
```

## Design rules

- **Explicit serialization.** Frames are built and parsed byte by byte; no
  `reinterpret_cast` of USB buffers.
- **RAII everywhere.** Transports release handles in destructors; `Scanner`
  owns its client; no manual matching of opens and closes.
- **Strongly typed enums.** `ErrorKind`, `FrameType`, `Status`, `Model`,
  `State`, per-controller command enums (number collisions between controllers
  are resolved by type, mirroring how the protocol disambiguates by address).
- **`Result<T>` (expected-style)** for fallible operations; exceptions are not
  used across the stack.
- **No global mutable state.** The logger singleton is the one shared object
  and holds configuration only (level, prefix).
- **Safety allow-list.** `ppb::Client::exchange()` refuses frames addressed to
  anything but the documented controller addresses — bootloaders
  (`0x22/0x26/0x42/0x46`) and EEPROM bus addresses (`0xA2/0xA4`) can be
  physically damaged by writes (pakon-reference `per-unit-data-and-safety.md`).

## Test strategy

No hardware is required for CI-style testing:

1. **Frame vectors** in tests are copied verbatim from pakon-reference quotes
   and from the `alibosworth/pakon-captures` corpus (OEM-driven F-135+,
   serial 16402). Fabricated protocol behavior is not tested as if real.
2. **`ReplayUsbTransport`** scripts request→reply pairs; an unexpected
   request is a test failure, so protocol drift breaks the build, not the
   scanner.
3. Cross-cutting invariant: `wire length == 2 + count` holds for all
   198,425 captured frames, and tests assert it for the builders.

## Build

```
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Targets Windows (MSVC) first; Linux/macOS builds compile the transport-independent
layers against the non-Windows USB stub so tests run anywhere. See
[USB.md](USB.md) for backend status.

## Phases

| Phase | Scope | State |
|---|---|---|
| 1 | project structure, docs, logging, errors, transport abstraction, tests | done (see STATUS.md) |
| 2 | USB detection (`pakon-cli list`) | done, physical check pending |
| 3 | PPB packet infrastructure | done |
| 4 | safe comms: identify/status | done, physical check pending |
| — | bootstrap: evidence survey + read-only probe (`pakon-cli probe`) | done, hardware check pending (BOOTSTRAP.md) |
| 5 | scanner state machine (init/scan sequences) | not started |
| 6 | transport (film motion) | not started |
| 7 | image acquisition | not started |
| 8 | image decoding | not started |
| 9 | calibration (read-only EEPROM) | not started |
| 10 | output (TIFF/JPEG) | not started |
