# WinUSB physical binding test

**Status: PENDING — this procedure has not been run. No hardware result
exists for it; do not treat any step below as validated.**

Purpose: bind Microsoft's WinUSB (`winusb.sys`) to the *cold* Pakon
F135+ (Code 28) via `driver/PakonWinUSB.inf` and verify that our stack
discovers and opens it — the milestone that decides whether WinUSB (our
current preferred architecture) works for the cold device, or whether the
KMDF fallback trigger list applies.

## Safety boundary for this test

Allowed: enumeration, driver-package installation on the host, USB
descriptor reads, read-only CLI probes (`list`; `identify`/`status`
expected to *refuse*, see step 7).

Forbidden — the CLI does not implement these, and none may be improvised
during the test: firmware transfer, `0xA3` firmware download, `0xA0`
reset/firmware operations, EEPROM access (`0xA2`/`0xA4`), any write to
scanner memory, acquisition, calibration changes.

**Stop condition: the test ends after step 8. Firmware loading is a
separate future milestone requiring its own explicit approval.**

## Prerequisites

- Windows 11 x64 machine with the cold F135+ attached (`USB\VID_0F05&PID_F235&REV_::07`,
  Code 28 — verify before starting, step 3).
- Repository available on that machine (this checkout or a clone of it).
- An **Administrator** shell for steps 4 and the rollback section.
- WDK installed *only if* the lab has never generated a catalog before
  (`inf2cat`); see `driver/README.md`. This is catalog tooling, not a
  project build dependency.

## Procedure

### 1. Build Release

```bat
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Record: zero warnings, all tests pass (includes `usb_identity`, which
also validates the INF ↔ C++ GUID consistency).

### 2. Connect the cold F135+

Plug the scanner into the USB port it will use. Wait for Windows to
finish device enumeration (a few seconds; the device does not re-enumerate
again by itself).

### 3. Verify Device Manager sees F235

- Device Manager → look under **Other devices** (or **Universal Serial
  Bus devices**): expect the device with a yellow warning — *Code 28:
  The required drivers for this device are not installed*.
- Cross-check in PowerShell:

  ```powershell
  Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'USB\VID_0F05*' } |
      Format-List Status, Class, InstanceId, Service
  ```

  Expected: `Status = Error` (Code 28), `InstanceId = USB\VID_0F05&PID_F235\6&1D7D6E45&0&4`,
  `Service` empty.
- `pakon-cli list` must show the cold unit as **discovered but not
  openable** (`Device path: (none …)`, PnP instance printed). Capture
  this output — it is the pre-install baseline.

### 4. Install the test WinUSB INF package

Sign and stage the package exactly as `driver/README.md` describes
(`inf2cat` → `signtool` → elevated `pnputil /add-driver driver\PakonWinUSB.inf /install`).
Record the `oemNN.inf` name pnputil reports — the rollback section needs it.

### 5. Verify WinUSB became the function driver

```powershell
Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'USB\VID_0F05*' } |
    Format-List Status, InstanceId, Service
Get-PnpDeviceProperty -InstanceId 'USB\VID_0F05&PID_F235\6&1D7D6E45&0&4' |
    Where-Object { $_.KeyName -like '*Service*' -or $_.KeyName -like '*InterfaceGUID*' }
```

Expected: `Status = OK`, `Service = WinUSB`; the device's
`DeviceInterfaceGUIDs` value contains
`{0e9e6f29-e70a-4582-8d02-bde3ad701252}`. Device Manager → device →
Driver tab must show provider **Microsoft**, driver `winusb.sys`.

**Known risk under test:** `winusb.sys` performs a SET_CONFIGURATION at
bind; the FX35's own loader never config-selects the cold device. If the
unit instead shows Code 43/10, restarts repeatedly, or disappears from
the bus, record it — that is the documented KMDF-fallback trigger, not a
reason to modify the scanner.

### 6. Re-enumerate if required

If step 5 shows the device still in an error state, re-plug it (or
disable/enable in Device Manager) once and re-check. Note which behavior
occurred (bind took immediately vs needed re-plug).

### 7. Run the CLI probes (record verbatim output)

```bat
pakon-cli --log trace list > winusb-test-list.log 2>&1
pakon-cli --log trace identify > winusb-test-identify.log 2>&1
pakon-cli --log trace status > winusb-test-status.log 2>&1
```

Expected:

| Command | Expectation |
|---|---|
| `list` | cold unit now shows a `Device path` and WinUSB-read interface/endpoint detail (bulk OUT `0x01` / bulk IN `0x81` should be visible) |
| `identify` | **must fail** with the honest cold-state error — `open_first` refuses cold devices because firmware loading is not implemented. A failure here is the safety system working, not a bug to "fix" |
| `status` | same refusal |

Do not attempt to make `identify`/`status` succeed — that would require
firmware loading, which is out of scope for this test.

### 8. Capture logs and stop

- Save the three `.log` files, the step 3/5 PowerShell output, a Device
  Manager screenshot, and `pnputil /enum-drivers | findstr /i PakonWinUSB`.
- Record every row of the evidence table below with observed values and
  the date.
- **Stop here.** No firmware operation of any kind (see Safety boundary).

## Evidence table (fill in only from actual hardware runs)

| # | Check | Expected | Observed | Date |
|---|---|---|---|---|
| 1 | Build + ctest | 0 warnings, all tests pass | *(pending)* | |
| 2 | Device Manager before install | Code 28, `USB\VID_0F05&PID_F235` | *(pending)* | |
| 3 | `pakon-cli list` before install | discovered, `Device path: (none …)` | *(pending)* | |
| 4 | `pnputil /add-driver` | package staged + installed, `oemNN.inf` | *(pending)* | |
| 5 | Function driver after install | `Service = WinUSB`, GUID registered | *(pending)* | |
| 6 | Re-enumeration needed? | record which behavior occurred | *(pending)* | |
| 7 | `pakon-cli list` after install | device path + endpoint detail present | *(pending)* | |
| 8 | `pakon-cli identify` | honest cold-state refusal (no I/O) | *(pending)* | |
| 9 | `pakon-cli status` | honest cold-state refusal (no I/O) | *(pending)* | |

If any row deviates from expectation, record what actually happened —
**this document must never claim the test passed without hardware
evidence.**

## Rollback (any time)

```bat
:: elevated; NN from pnputil /enum-drivers
pnputil /delete-driver oemNN.inf /uninstall /force
```

Then re-plug: the device returns to Code 28. Installing/removing this
package changes only the host; nothing on the scanner is modified.

## After a successful test

Next milestone (separate approval required): user-mode firmware-loader
design for the cold device. Until then the stack deliberately stops at
discovery + binding + read-only descriptors.
