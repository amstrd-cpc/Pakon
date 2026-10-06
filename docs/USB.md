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
opened (`open_first(cold_ok=false)` returns a clear error). The firmware bytes
are out of scope per pakon-reference's own scope rules.

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

`usb::enumerate()` on Windows uses SetupAPI over
`GUID_DEVINTERFACE_USB_DEVICE` and parses VID/PID/serial from the device
instance identity (`USB\VID_xxxx&PID_xxxx\<serial>`):

- **No driver dependency** — a unit claimed by the installed legacy Pakon
  driver is still listed (the driver binding only affects opening for I/O).
- **Read-only** — enumeration never sends USB traffic to any device
  (per-unit-data-and-safety.md: reading/polling has no recorded incident).
- Interface/endpoint detail is added opportunistically: the device is opened
  with WinUSB when that binding allows it; otherwise `interface_note`
  explains why endpoints are unavailable instead of failing.

`pakon-cli list` output is enumeration-only; it does not open a PPB session.

## Transport (`IUsbTransport`)

One interface for all higher layers:

- `command_exchange(frame)` — atomic bulk OUT `0x01` + bulk IN `0x81`
  read-back (ppb-protocol.md: "as an atomic write-then-read"). The read loops
  until the reply frame is complete (`2 + count` bytes), then stops.
- `bulk_read(endpoint, max)` — image stream reads (Phase 7; OEM reads
  transfers up to 20480 bytes).
- `control_read/control_write` — vendor control requests; the documented
  read-only EEPROM path uses `0xA4` (select, `wValue 0x00A5`,
  `wIndex 0x1234`) + `0xA9` (read, ≤32 bytes). Vendor `0xA2` is an EEPROM
  **write** and is never issued by this stack.

Backends:

| Backend | Platforms | State |
|---|---|---|
| `WinUsbTransport` | Windows | implemented: SetupAPI + WinUSB, 2 s pipe timeouts, TX/RX hex logging |
| non-Windows stub | Linux/macOS | enumeration empty, `open_first` → `usb_not_supported` (protocol tests still run) |
| libusb | Linux/macOS | future, drops in behind `IUsbTransport` |

Verified: both Windows sources compile clean (`-Wall -Wextra`) for
`x86_64-windows-gnu` and a full `pakon-cli.exe` links; CLI smoke-tested and
both test suites pass as Windows binaries. Real MSVC build still to be run
(STATUS.md: WINDOWS MSVC BUILD REQUIRED).

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

1. Cold devices are never opened (no firmware loading).
2. Control writes are stubbed except through explicit, documented sequences;
   vendor `0xA2` (EEPROM write) does not exist anywhere in the codebase.
3. All outbound PPB frames are checked against the controller address
   allow-list in `ppb::Client` (bootloaders/EEPROM addresses refused).
4. Enumeration sends no traffic.
