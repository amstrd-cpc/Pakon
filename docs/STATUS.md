# Status

Last updated: 2026-10-06. Scope: Phase 1–4 first deliverable (structure, USB
detection, PPB infrastructure, safe identify/status comms, tests, CLI, docs).

Legend for evidence: **[COMPLETED]** verified by build/test evidence in this
repository · **[PHYSICAL TEST REQUIRED]** nothing here has touched real
hardware yet.

---

## COMPLETED

- **Reference analysis** — `docs/PAKON_REFERENCE.md`: what pakon-reference
  provides, what translates directly, gaps/limitations, file-by-file
  citations. pakon-reference is treated as primary spec throughout.
- **Build system** — CMake ≥3.20, C++20, MSVC `/W4 /permissive-`, GCC/Clang
  `-Wall -Wextra`; static `pakon_core` library + `pakon-cli` + CTest suites.
- **Error handling** — `ErrorKind`/`Error`/`Result<T>` with `std::expected`
  when available and a minimal identical fallback (exercised: this toolchain
  compiles the fallback path).
- **Logging** — leveled logger with trace-level TX/RX hex dumps
  (`endpoint=… length=…` + hex rows); `pakon-cli --log trace`.
- **USB abstraction** — `IUsbTransport` (command exchange / bulk read /
  vendor control), `DeviceInfo`, enumeration API.
  - Windows backend: two-pass SetupAPI discovery — PnP device tree
    (`DIGCF_ALLCLASSES`, finds devices with **no function driver**, i.e.
    Code 28 cold units) merged with `GUID_DEVINTERFACE_USB_DEVICE`
    (supplies `device_path` where a driver registered one); driver-independent,
    read-only. Hardware-ID recognition lives in platform-independent
    `usb/identity.hpp`. WinUSB transport with 2 s pipe timeouts and
    packet logging; `open_first` reports "discovered but not openable"
    honestly instead of "not detected".
  - Non-Windows stub so all protocol tests build and run anywhere.
  - **Evidence:** both Windows sources compile clean for
    `x86_64-windows-gnu`; full `pakon-cli.exe` links; CLI smoke-tested
    (`--help`, `list`, bad-flag handling, error paths); test binaries run
    as Windows executables. Native MSVC Release (2026-10-06): zero
    `/W4 /permissive-` warnings, CTest 3/3, `pakon-cli list` run on the
    lab machine (enumeration over the full PnP tree; the scanner was
    detached at the time — no hardware result yet, see
    PHYSICAL TEST REQUIRED).
- **PPB layer** — frame serialize/parse with the `2 + count` invariant,
  reply parsing with mirror-type + address-echo validation, status decoding,
  five builders reproducing documented/captured request forms byte-for-byte,
  destination safety allow-list.
- **Scanner session** — connect (best-effort handshake), identify (presence
  probes → model, module info `0x07`, bridge info HOST `0x03`), status
  (HOST/light/motor polls + `0x83`/`0x84`/`0x88` reads), explicit
  `State`/`Model` enums with logged transitions.
- **pakon-cli** — `list` (enumeration only), `identify`, `status`,
  `--log LEVEL`.
- **Tests** — 36 cases, all passing on Linux/GCC **and** the native MSVC
  build (CTest 3/3). All vectors verbatim from pakon-reference quotes and
  the `alibosworth/pakon-captures` corpus (F-135+ serial 16402); replay
  transport fails on any request not in the scripted captures. The
  `usb_identity` suite covers hardware-ID recognition (cold F235, warm
  F135, unrelated rejection, `&MI_` exclusion), serial-vs-PnP-location
  parsing, discovered-but-not-openable representation, and pins the
  WinUSB INF's DeviceInterfaceGUID to the C++ constant.
- **WinUSB driver package** — `driver/PakonWinUSB.inf` (in-box
  `winusb.sys` via `winusb.inf`; targets `USB\VID_0F05&PID_F235` and
  `USB\VID_0F05&PID_F135&REV_0002`; project DeviceInterfaceGUID
  `{0e9e6f29-e70a-4582-8d02-bde3ad701252}`; no binaries, no firmware) +
  `driver/README.md` (catalog signing via WDK `inf2cat`, install/removal)
  + `docs/WINUSB_TEST.md` (exact manual procedure). **Physical binding
  test: PENDING — no hardware result exists.**
- **Documentation** — `PAKON_REFERENCE.md`, `ARCHITECTURE.md`, `USB.md`,
  `PPB.md`, `SCANNER.md`, `STATUS.md`, `WINUSB_TEST.md`; headers carry
  per-claim citations.
- **Capture-based verification of the reference** — all 198,425 frames in
  six capture sessions satisfy `length == 2 + count`; command table
  cross-checked (see `PPB.md` § Discrepancies: SetLightConfig `0x8F` payload
  is 4 bytes not 2; HOST poll replies are 5 bytes; READ request layout fully
  recovered).

## IN PROGRESS

- Nothing actively in progress; Phase 1–4 code complete pending validation
  below.

## NOT IMPLEMENTED (by design for this phase)

- Firmware loading for cold (`0f05:f235`) devices — out of scope (firmware
  bytes excluded by pakon-reference's own scope rules).
- Initialization / scan / teardown sequences (Phase 5), film transport
  (Phase 6), image acquisition (7), decoding (8), calibration EEPROM read
  (9), output (10).
- DX barcode substitution, TEC control (`0xD0/0xD1`), any EEPROM write,
  colour pipeline, GUI.
- libusb backend (Linux enumeration currently stubbed).

## BLOCKED

- **I/O on a machine where the legacy Pakon driver owns the device.**
  WinUSB cannot open a WinUSB-unbound device. Two documented paths exist,
  neither taken yet (needs a decision + a machine with the unit):
  1. `Pakon135IoctlTransport` — talk IOCTL `0x222090`/`0x222059` +
     `ReadFile(EP 0x86)` through the *installed* driver (documented in
     pakon-tlx-macos `docs/PROTOCOL.md`), modifying nothing;
  2. rebind the scanner to WinUSB (affects the legacy stack — not to be
     done without explicit approval). A test package is ready for this:
     `driver/PakonWinUSB.inf`, physical validation pending
     (`docs/WINUSB_TEST.md`).
  Enumeration (`list`) already works regardless — including devices with
  no function driver at all (Code 28).

## PHYSICAL TEST REQUIRED

Nothing has been run against real hardware. Specifically untested:

1. `pakon-cli list` against an attached unit (cold + warm identities,
   serial parse, endpoint detail). *The scanner was detached from the
   lab machine on 2026-10-06 (both devnodes `Present=False`, phantom
   entries; cold `Service` absent → Code 28 state unchanged), so even
   the discovery fix has no live-device result yet.*
2. **The WinUSB binding test** — install `driver/PakonWinUSB.inf` on the
   cold unit, verify `Service=WinUSB` + interface GUID, re-run `list`
   (`identify`/`status` must refuse: firmware loading not implemented).
   Exact procedure + evidence table: `docs/WINUSB_TEST.md` (PENDING).
   This test also adjudicates the SET_CONFIGURATION-at-bind risk noted
   in the WinUSB decision (KMDF fallback trigger).
3. The full connect handshake on a real unit, including the first-open vs
   later-open reply behavior.
4. Presence probes `0x44`/`0x24` on a real F-135+ (and an F-135, if
   available) — model detection evidence so far comes from pakon-reference
   and third-party projects, not from this code.
5. Module-info / bridge-info / status register reads (`0x83`, `0x84`,
   `0x88`) — replies are capture-verified but were captured from another
   stack; our frames must be confirmed to elicit them.
6. Any claim that the driver-stack coexists with the running legacy
   software — assumed only, untested.

## UNKNOWN / NEEDS INVESTIGATION

- Module-info (`0x07`, 12 bytes) payload semantics.
- HOST reg `0x03` bridge-info semantics (observed `0f 03`).
- Whether reading `0x88` (temperature) is valid on F-135 (non-Plus).
- Exact meaning of WRITE/CMD `data[1]` byte: "payload length" vs
  "sub-register index" fit the two verbatim ppb-protocol.md examples
  equally; captures are consistent with payload length (every WRITE obeys
  `len = payload_len + 3`), adopted as such — still worth an
  interventional check on hardware.
- Stale-reply edge after a tolerated handshake timeout (address-echo check
  guards it; behavior untested on hardware).

## NEXT RECOMMENDED STEP

1. Attach the cold F135+ and run `docs/WINUSB_TEST.md` (evidence table
   included) — this decides WinUSB adequacy for the cold device and
   removes the `list`-against-hardware gap.
2. Decide the device-access path (BLOCKED item: IOCTL transport vs WinUSB
   rebind); implement `Pakon135IoctlTransport` if approved — it keeps the
   legacy stack untouched.
3. Then: physical `list` → `identify` → `status` on a warm unit, in that
   order, with `--log trace` captured for the record.
4. Phase 5: init/scan/teardown sequences from command-reference.md +
   capture replays, still without motion until explicitly approved.
