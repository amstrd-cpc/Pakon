# USB layer

Source of truth: pakon-reference `docs/usb-identity-and-firmware.md`,
`docs/ppb-protocol.md` § Command channel, `docs/image-stream.md`.
Our implementation: `src/pakon/usb/`.

## Identity

| State | VID:PID | Meaning |
|---|---|---|
| cold (bootstrap) | `0f05:f235` | all family members before firmware load |
| warm F-135/F-135+ | `0f05:f135` | operational 135-line scanner |
| warm F-235 | `0f05:35f2` | out of scope |
| warm F-335 | `0f05:f335` | out of scope |

`[DOCUMENTED]` — usb-identity-and-firmware.md. F-135 and F-135+ are
**indistinguishable by USB descriptors**; the model is decided later by PPB
presence probes (see [SCANNER.md](SCANNER.md)).

This project **does not load firmware**. A cold device is listed but not
opened by the scanner commands (`open_first(cold_ok=false)` returns a clear
error). The firmware bytes are out of scope per pakon-reference's own scope
rules, and the upload request layout is not documented in-repo — the exact
evidence gaps are enumerated in [BOOTSTRAP.md](BOOTSTRAP.md). The one
exception that does open a cold device read-only is `pakon-cli probe`
([BOOTSTRAP.md](BOOTSTRAP.md) § 4): exactly one documented vendor control
read, no bulk traffic, no writes.

## Endpoints

| Endpoint | Direction | Role | Source |
|---|---|---|---|
| `0x01` | bulk OUT | host→device command frame | `[DOCUMENTED]` ppb-protocol.md |
| `0x81` | bulk IN | command reply | `[DOCUMENTED]` ppb-protocol.md |
| image | bulk IN (separate) | image stream, max packet 512 (high speed) | `[DOCUMENTED]` image-stream.md — **number not stated in pakon-reference** |

The image endpoint **number is read from the device's endpoint descriptors at
enumeration time**, never hardcoded from a guess. External evidence (not used
as a constant, only as corroboration): pakon-tlx-macos `docs/PROTOCOL.md` names
the OEM image endpoint `0x86` (`ReadFile` on bulk IN EP6), and the capture
corpus logs image traffic as `ep6` events.

## Enumeration (Phase 2)

`usb::enumerate()` on Windows merges two SetupAPI passes, both read-only
(never opens a device for I/O in either pass):

1. **PnP device-tree pass** — `SetupDiGetClassDevsA(…,
   DIGCF_PRESENT | DIGCF_ALLCLASSES)` + `SetupDiEnumDeviceInfo` +
   `SetupDiGetDeviceInstanceIdA`, over every present devnode. A devnode
   is kept when its hardware ID begins with `USB\VID_0F05&PID_F235` or
   `USB\VID_0F05&PID_F135` (composite function IDs containing `&MI_` are
   excluded — one scanner is listed once, at device level). VID/PID,
   hardware ID, instance ID and serial are parsed by
   `usb/identity.hpp`, which is platform-independent and unit-tested
   (`tests/usb/identity_test.cpp`).
2. **Device-interface pass** — `GUID_DEVINTERFACE_USB_DEVICE` (the
   original path): supplies `device_path` for devices a function driver
   exposed, merges into the PnP entry by instance ID, and keeps
   non-Pakon devices visible as before. Pakon `&MI_` duplicates of an
   already-listed devnode are dropped.

### Why a cold (Code 28) device was invisible before

Windows registers a `GUID_DEVINTERFACE_USB_DEVICE` interface instance
when a function driver binds. A device with **Code 28 (no compatible
driver installed)** has no function driver — hence no interface
instance — so interface-only enumeration returned nothing even though
Device Manager shows the unit (observed on the actual scanner:
`reg query … /f "vid_0f05"` over the interface key → 0 matches, while
`Get-PnpDevice` lists `USB\VID_0F05&PID_F235\6&1D7D6E45&0&4`). The PnP
pass finds the devnode regardless of driver state, so the cold unit is
now **discovered**: it appears in `pakon-cli list` with its hardware ID
and instance ID, `Device path: (none …)` and an `interface_note` that
says it is *discovered but not openable* — it is never presented as
openable until a driver is bound.

Two identity traps are handled explicitly:

- The instance segment after the backslash (`6&1D7D6E45&0&4`) is a PnP
  **location id**, not a USB serial number; `serial_from_instance`
  rejects any value containing `&`, so no fake serial is ever reported.
- `USB\VID_0F05&PID_F235&REV_::07` — Windows renders the non-BCD
  `bcdDevice` nibbles of the `F235_AA07` personality as `:`; the match
  is prefix-based so any cold revision is recognized.

Properties of enumeration:

- **Driver-independent** — works with no driver (Code 28), with the
  installed legacy Pakon driver, and with WinUSB; binding only affects
  *opening* for I/O.
- **Read-only** — enumeration never sends USB traffic to any device
  (per-unit-data-and-safety.md: reading/polling has no recorded incident).
- Interface/endpoint detail is added opportunistically for entries that
  have a `device_path`: the device is opened with WinUSB when that
  binding allows it; otherwise `interface_note` explains why endpoints
  are unavailable instead of failing.

`pakon-cli list` output is enumeration-only; it does not open a PPB
session. `probe` opens the device with `open_first(cold_ok=true)` and sends
only the documented stage-1 personality read ([BOOTSTRAP.md](BOOTSTRAP.md)).
`identify`/`status` go through `open_first`, which refuses
devices without an interface (honest "discovered but not openable" error)
and refuses cold devices (firmware loading not implemented).

## Driver package — WinUSB INF (`driver/`)

WinUSB (`winusb.sys`) is the **current preferred transport
architecture**; a custom KMDF driver is only a documented fallback if
the pending physical cold-device test shows WinUSB inadequate
(WINUSB_TEST.md § Known risk). The package:

- `driver/PakonWinUSB.inf` — targets `USB\VID_0F05&PID_F235` (cold) and
  `USB\VID_0F05&PID_F135&REV_0002` (warm) and installs **Microsoft's
  in-box WinUSB** through the system `winusb.inf` (`Include`/`Needs`
  mechanism, per Microsoft's *WinUSB Installation for Developers*).
- Registers the project device interface GUID
  `{0e9e6f29-e70a-4582-8d02-bde3ad701252}`, shared byte-for-byte with
  `kDeviceInterfaceGuid` in `usb/identity.hpp` and pinned by the test
  suite.
- Contains **no driver binaries, no firmware, no third-party driver
  files** — and **no custom kernel driver exists at this stage**; only
  Microsoft's in-box driver is bound.

**Firmware loading is intentionally not implemented yet.** The package
and the stack only reach: discovery → driver binding → descriptors.
Physical validation of the binding is the next milestone and is
[pending](WINUSB_TEST.md).

## Transport (`IUsbTransport`)

One interface for all higher layers:

- `command_exchange(frame)` — atomic bulk OUT `0x01` + bulk IN `0x81`
  read-back (ppb-protocol.md: "as an atomic write-then-read"). The read loops
  until the reply frame is complete (`2 + count` bytes), then stops.
- `bulk_read(endpoint, max)` — image stream reads (Phase 7; OEM reads
  transfers up to 20480 bytes).
- `control_read/control_write` — vendor control requests; the documented
  read-only EEPROM path uses `0xA4` (select, `wValue 0x00A5`,
  `wIndex 0x1234`) + `0xA9` (read, ≤32 bytes), and the stage-1 personality
  path uses `0xA9` (`wIndex 0`, 8 bytes — the `probe` command; see
  [BOOTSTRAP.md](BOOTSTRAP.md)). Vendor `0xA2` is an EEPROM
  **write** and is never issued by this stack.

Backends:

| Backend | Platforms | State |
|---|---|---|
| `WinUsbTransport` | Windows | implemented: SetupAPI + WinUSB, 2 s pipe timeouts, TX/RX hex logging |
| non-Windows stub | Linux/macOS | enumeration empty, `open_first` → `usb_not_supported` (protocol tests still run) |
| libusb | Linux/macOS | future, drops in behind `IUsbTransport` |

Both Windows `CreateFile` sites — `WinUsbTransport::open`
(`probe`/`identify`/`status`) and `enrich_interfaces` (`list`'s interface
detail) — build their open from the shared, unit-tested
`usb/win_usb_open.hpp`. `FILE_FLAG_OVERLAPPED` is mandatory: on the cold
unit (2026-10-07) a non-overlapped open of the same path made
`WinUsb_Initialize` fail with `ERROR_INVALID_HANDLE` (6) while the
overlapped open succeeded; both sites and the flag value are pinned by
`tests/usb/identity_test.cpp`.

Verified: both Windows sources compile clean (`-Wall -Wextra`) for
`x86_64-windows-gnu` and a full `pakon-cli.exe` links; CLI smoke-tested;
native MSVC Release builds with zero `/W4 /permissive-` warnings and all
three CTest suites pass on both platforms (Linux/GCC and Windows/MSVC).
The only remaining verification is against attached hardware
(STATUS.md: PHYSICAL TEST REQUIRED).

## Known limitation — device access when the legacy driver owns the unit

WinUSB can only perform I/O when the device is WinUSB-bound. On a machine
with the installed Pakon driver (`\\.\Pakon135`), `CreateFile`/`WinUsb_Initialize`
fails with access-denied and `open_first` reports it plainly.

The OEM's own driver surface is documented (pakon-tlx-macos
`docs/PROTOCOL.md`, which pakon-reference cites):

| Mechanism | Purpose |
|---|---|
| `DeviceIoControl(0x222090)` | bulk OUT `0x01` + bulk IN `0x81` — the command channel |
| `DeviceIoControl(0x222059)` | EP0 vendor/class control transfer (EEPROM reads) |
| `ReadFile` on bulk IN `0x86` | image stream |

A `Pakon135IoctlTransport` behind the same `IUsbTransport` interface would
talk to hardware through the installed driver **without modifying or
rebinding it**, keeping the legacy stack intact. This is the recommended
path for the first physical test (STATUS.md).

## Safety rules implemented here

1. Cold devices are opened only by `probe`, with an explicit
   `cold_ok=true`, for exactly one read-only vendor control request
   (stage-1 personality read — BOOTSTRAP.md); `identify`/`status` and the
   scanner layer never open them (no firmware loading).
2. Control writes are stubbed except through explicit, documented sequences;
   vendor `0xA2` (EEPROM write) does not exist anywhere in the codebase.
3. All outbound PPB frames are checked against the controller address
   allow-list in `ppb::Client` (bootloaders/EEPROM addresses refused).
4. Enumeration sends no traffic.
