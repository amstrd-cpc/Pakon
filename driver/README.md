# driver/ — WinUSB driver package (INF only)

`PakonWinUSB.inf` installs **Microsoft's in-box WinUSB function driver**
(`winusb.sys`, staged from the system `winusb.inf`) for the Pakon F135+
scanner. This directory contains:

- `PakonWinUSB.inf` — the entire package. No `.sys`, no `.cat`, no
  firmware, no third-party files. `PakonWinUSB.cat` is *generated* at
  signing time (see below) and is intentionally not committed.

WinUSB is the project's current preferred transport architecture (see
[../docs/USB.md](../docs/USB.md)). There is **no custom kernel driver**
in this project, and **firmware loading is intentionally not
implemented** — the physical WinUSB binding test is pending and is
documented in [../docs/WINUSB_TEST.md](../docs/WINUSB_TEST.md).

## Targeted hardware IDs

| Identity | Hardware ID | When |
|---|---|---|
| cold/bootstrap | `USB\VID_0F05&PID_F235` | out of the box (Code 28 today) |
| warm F-135/F-135+ | `USB\VID_0F05&PID_F135&REV_0002` | after firmware-load re-enumeration (future milestone) |

Both model lines install the same `USB_Install` section: the same
Microsoft driver, the same device interface GUID. The identities are kept
as separate hardware-ID rules (not one broad `PID_F135` rule) to match
the architecture — the warm rule pins the exact `REV_0002` identity the
scanner re-enumerates as.

## DeviceInterfaceGUID

```
{0e9e6f29-e70a-4582-8d02-bde3ad701252}
```

Defined in **two places that must stay identical**:

1. `PakonWinUSB.inf` → `[Dev_AddReg]` → `HKR,,DeviceInterfaceGUIDs`
2. `src/pakon/usb/identity.hpp` → `kDeviceInterfaceGuid`

`tests/usb/identity_test.cpp` (`device_interface_guid_matches_inf_package`)
reads both and fails if they drift. If the device's protocol/interface
ever changes, generate a *new* GUID rather than reusing this one
(Microsoft's guidance in *WinUSB Installation for Developers*).

## Signing and installing (lab / test signing)

The INF must be accompanied by a signed catalog file before Windows will
install it. **Catalog generation is the only step that needs WDK
tooling** (`inf2cat`); the project build itself does not use the WDK and
no build-system changes are required.

From a Developer/VS command prompt, on the Windows machine:

```bat
:: 1. generate the catalog (WDK required for inf2cat only)
inf2cat /driver:driver /os:10_X64

:: 2. sign it with a certificate trusted on THIS machine
::    (create one once, as Administrator):
::      New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=Pakon Lab Test" -CertStoreLocation Cert:\CurrentUser\My
signtool sign /v /fd SHA256 /s My /n "Pakon Lab Test" driver\PakonWinUSB.cat

:: 3. stage + install (elevated)
pnputil /add-driver driver\PakonWinUSB.inf /install
```

If Windows still refuses the package, enable test signing
(`bcdedit /set testsigning on` + reboot) — acceptable for the lab test
only. Production distribution would use an EV certificate + attestation
signing via the Microsoft Partner Center; that is out of scope for this
milestone.

Check the result:

```bat
pnputil /enum-drivers | findstr /i PakonWinUSB
powershell -c "Get-PnpDevice -PresentOnly | Where-Object {$_.InstanceId -like 'USB\VID_0F05*'} | Format-List Status,InstanceId,Service"
```

Expected after success: the device's `Service` is `WinUSB` (device
Manager: provider Microsoft, function driver `winusb.sys`).

## Removing the package

```bat
:: elevated; NN = the oemNN.inf shown by pnputil /enum-drivers
pnputil /delete-driver oemNN.inf /uninstall /force
```

The device then returns to its pre-install state (Code 28 for the cold
unit). Nothing on the scanner itself is modified by installing or
removing this package.

## Safety boundary

Installing this package is a *host-side* driver-binding operation. It
sends no traffic to the scanner beyond what Windows itself performs
during enumeration. It does not — and this project will not, without a
separate explicit milestone — transfer firmware, perform `0xA3`/`0xA0`
vendor requests, touch the EEPROM (`0xA2`/`0xA4`), write scanner memory,
run acquisition, or modify calibration. See
[../docs/WINUSB_TEST.md](../docs/WINUSB_TEST.md) for the exact test
procedure and its stop condition.
