# Status

Last updated: 2026-10-08. Scope: Phase 1–4 first deliverable (structure, USB
detection, PPB infrastructure, safe identify/status comms, tests, CLI, docs)
plus the bootstrap evidence survey, the read-only probe, the
**validated cold→warm boot chain** ([BOOT_CHAIN.md](BOOT_CHAIN.md)) with its
loader-port design ([LOADER_DESIGN.md](LOADER_DESIGN.md)), and the
read-only descriptor/topology diagnostic
([F135_TOPOLOGY.md](F135_TOPOLOGY.md)).

Legend for evidence: **[COMPLETED]** verified by build/test evidence in this
repository · **[HARDWARE-VALIDATED]** executed and recorded on the lab unit
(2026-10-07, [BOOT_CHAIN.md](BOOT_CHAIN.md)) · **[PHYSICAL TEST REQUIRED]**
not yet run against real hardware.

---

## COMPLETED

- **Cold→warm boot chain validated on hardware (2026-10-07)** —
  [BOOT_CHAIN.md](BOOT_CHAIN.md): the OEM-derived sequence ran end-to-end
  on the lab unit — stage-1 upload (360 × `0xA0`, 4476 B, extracted from
  MD5-pinned `F235Ldr.sys`), `0xA4` preamble, gated `0xA9` answered
  `C0-05-0F-35-F2-07-AA-04` (→ `F235_AA07` → `Pakon7.hex`), Pakon7 download
  (709 × `0xA3` + 19 × `0xA0`, both shape-gated), final run → device
  re-enumerated as `USB\VID_0F05&PID_F135\010-203-04`, `status=OK`,
  `service=WINUSB`. Every write volatile (unplug = cold); probe unchanged.
  Frozen procedure: `tools/pakon_boot_reference.ps1`. Remaining UNKNOWNs
  listed in BOOT_CHAIN.md § 8.
- **Read-only F135 descriptor/topology diagnostic (implementation
  complete, hardware run pending)** — `pakon-cli descriptors` +
  `src/pakon/usb/descriptors.*`: locates the booted `0F05:F135`, reads
  the raw device + configuration descriptor bytes and referenced strings
  via **standard `GET_DESCRIPTOR` only**, walks every interface /
  alternate setting / endpoint (direction, transfer type, max packet,
  interval, class/subclass/protocol), cross-checks against an
  independent WinUSB `QueryInterfaceSettings`/`QueryPipe` walk, and
  reports the WinUSB interface path, link speed and current
  configuration. No vendor request (`0xA0`/`0xA3`/`0xA4`/`0xA9`
  untouched), no firmware upload, no reset, no reconfiguration, no bulk
  traffic. Observation tables in
  [F135_TOPOLOGY.md](F135_TOPOLOGY.md) await the lab run.
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
- **pakon-cli** — `list` (enumeration only), `probe` (cold-device read-only
  bootstrap probe: one documented vendor control read), `identify`,
  `status`, `--log LEVEL`.
- **Bootstrap evidence survey + read-only probe** — `docs/BOOTSTRAP.md`
  records how far repo evidence reaches for the cold→warm firmware path:
  the FX2 sequence is documented **by name only** and the firmware bytes
  are excluded by scope rules, so no upload path and no packet format is
  implemented (seven enumerated evidence gaps). Built instead:
  `src/pakon/bootstrap/probe.*` (stage-1 personality read `0xA9`/`wValue
  0`/`wIndex 0`/8 bytes, evidence-pinned) + `pakon-cli probe`, which opens
  a cold device for that single read and nothing else (no bulk, no
  control writes, no PPB/type-byte-0 exposure).
- **Tests** — 45 cases: **45/45 passing on Linux/GCC (CTest 4/4,
  2026-10-07)**; the MSVC Release re-run of the same 4 suites / 45 cases
  was **reported green (operator, 2026-10-08)** together with the
  `attrib` run — re-run after each landing change
  (command: `docs/BOOTSTRAP.md` § 6).
  All vectors verbatim from pakon-reference quotes and
  the `alibosworth/pakon-captures` corpus (F-135+ serial 16402); replay
  transport fails on any request not in the scripted captures. The
  `usb_identity` suite covers hardware-ID recognition (cold F235, warm
  F135, unrelated rejection, `&MI_` exclusion), serial-vs-PnP-location
  parsing, discovered-but-not-openable representation, pins the
  WinUSB INF's DeviceInterfaceGUID to the C++ constant, and pins the
  shared WinUSB open parameters (`usb/win_usb_open.hpp`: overlapped flag
  + both Windows open sites using the header — regression for the
  2026-10-07 `ERROR_INVALID_HANDLE` discrepancy). The
  `bootstrap_probe` suite pins the probe request layout (and forbids
  `0xA2`/`0xA4`) and proves against a recording fake transport that a
  probe performs exactly one control read and no writes/bulk traffic.
- **WinUSB driver package** — `driver/PakonWinUSB.inf` (in-box
  `winusb.sys` via `winusb.inf`; targets `USB\VID_0F05&PID_F235` and
  `USB\VID_0F05&PID_F135&REV_0002`; project DeviceInterfaceGUID
  `{0e9e6f29-e70a-4582-8d02-bde3ad701252}`; no binaries, no firmware) +
  `driver/README.md` (catalog signing via WDK `inf2cat`, install/removal)
  + `docs/WINUSB_TEST.md` (exact manual procedure). **Physical bindings
  observed 2026-10-07** (cold probe opened through this package's GUID;
  post-boot F135 enumerated with `service=WINUSB` — attribution query
  pending, `BOOT_CHAIN.md` § 6); the WINUSB_TEST.md evidence table itself
  is still pending.
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

- Firmware loading **in the repository** — the evidence gaps below are now
  closed (OEM-artifact reverse engineering + hardware run,
  [BOOT_CHAIN.md](BOOT_CHAIN.md)) and the sequence is validated as an
  approved procedure (`tools/pakon_boot_reference.ps1`), but no C++
  loader exists yet; component design and rollout:
  [LOADER_DESIGN.md](LOADER_DESIGN.md). Firmware bytes remain excluded
  from the repo by scope rule (fetched + MD5-gated at run time).
- Initialization / scan / teardown sequences (Phase 5), film transport
  (Phase 6), image acquisition (7), decoding (8), calibration EEPROM read
  (9), output (10).
- DX barcode substitution, TEC control (`0xD0/0xD1`), any EEPROM write,
  colour pipeline, GUI.
- libusb backend (Linux enumeration currently stubbed).

## BLOCKED

- ~~I/O on a machine where the legacy Pakon driver owns the device~~ —
  **resolved on the lab machine (2026-10-07):** the cold unit opens over
  WinUSB through `driver/PakonWinUSB.inf` (probe + full boot ran, see
  [BOOT_CHAIN.md](BOOT_CHAIN.md)); the `Pakon135IoctlTransport` option
  remains only as a fallback for units still owned by the legacy stack.
  Enumeration (`list`) works regardless — including devices with no
  function driver at all (Code 28).

## PHYSICAL TEST REQUIRED

Superseded 2026-10-07 for the bootstrap/boot items: the probe and the
full cold→warm boot ran on the lab unit and their outputs are recorded in
[BOOT_CHAIN.md](BOOT_CHAIN.md). Still untested:

1. `pakon-cli list` against an attached unit (cold + warm identities,
   serial parse, endpoint detail) — the hardware runs used the approved
   PowerShell procedure, not the CLI enumerator.
2. **The WinUSB binding test's own evidence table** (`docs/WINUSB_TEST.md`)
   — the *bindings themselves* are now observed: cold `0f05:f235` opened
   over WinUSB with interface GUID `{0e9e6f29…}` from the first probe
   session, and the post-boot `0f05:f135&REV_0002` enumerated with
   `service=WINUSB` (attribution query run reported green 2026-10-08;
   BOOT_CHAIN.md § 6 rows await transcription from its output). The
   `identify`/`status`-refuse-cold checks and the SET_CONFIGURATION-at-bind
   risk adjudication still need their run.
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
7. ~~`pakon-cli probe` against the cold unit~~ — **recorded 2026-10-07:**
   cold ROM baseline `0xA9` → win32 `121` (no answer); after the stage-1
   upload the same read answered `C0-05-0F-35-F2-07-AA-04`, and the full
   boot transcript follows (`BOOT_CHAIN.md` § 3, § 5). A C++-path probe
   run against hardware (via the MSVC build) is still worth repeating.
8. `pakon-cli descriptors` against the booted F135 — fills the
   `[PENDING]` observation tables in
   [F135_TOPOLOGY.md](F135_TOPOLOGY.md) (method and safety sections
   already committed; standard `GET_DESCRIPTOR` reads + WinUSB queries
   only).

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

1. Re-run the MSVC build + CTest (expect 4 suites / 45 cases) on the
   Windows target, then run the read-only attribution diagnostic
   (`pakon-cli attrib`) against the enumerated F135 — it completes
   [BOOT_CHAIN.md](BOOT_CHAIN.md) § 6 (`DeviceDesc` / `FriendlyName` /
   bound INF; no device I/O).
2. Implement the loader per [LOADER_DESIGN.md](LOADER_DESIGN.md),
   component by component, each gated on its replay test **and** an
   explicit hardware go-ahead; `tools/pakon_boot_reference.ps1` stays
   the trusted procedure until each C++ step is validated against it.
3. Then: physical `list` → `identify` → `status` on the warm unit, in
   that order, with `--log trace` captured for the record (first PPB
   traffic from this stack on hardware).
4. Phase 5: init/scan/teardown sequences from command-reference.md +
   capture replays, still without motion until explicitly approved.
