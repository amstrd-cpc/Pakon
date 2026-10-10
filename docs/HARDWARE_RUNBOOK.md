# Hardware runbook — first scans on the F-135+

Run the steps **in order** and stop at the first one that does not match its
expected output. Each step says what to send back. Commands are for the
Windows machine the scanner is attached to (PowerShell, repo root,
`pakon-cli.exe` from the MinGW cross build or MSVC). Add `--log debug` to any
`pakon-cli` command when sending output back; image bytes are never logged.

Safety, built into the tool rather than left to you: the EEPROM is only
ever read (allow-listed `0xA4/0x00A5` + `0xA9`; the WinUSB backend refuses
anything else), the PIC bootloader addresses are never addressed, LED
currents are clamped to the board ceilings, and every path that lit the
lamp or moved the motor ends with the teardown (acquire off, lamp off, FIFO
reset, DX stop, motor `0xA2`, then `rate 0 → go → 0xA2`) — also on errors
and Ctrl+C. Nothing touches USB without `--live-scan` (or one of the read
commands below).

## 0. Before you start

- WinUSB bound to the scanner (`docs/WINUSB_TEST.md`).
- Empty film path for steps 1–6 (no strip in the gate or the guides).
- Collect into one folder per session; note the date and whether the unit
  was power-cycled.

## 1. Boot (cold unit)

```
powershell -ExecutionPolicy Bypass -File .\tools\pakon_boot_reference.ps1
```

Expected: the device re-enumerates as `USB\VID_0F05&PID_F135\…`,
`status=OK`, `service=WINUSB` ([BOOT_CHAIN.md](BOOT_CHAIN.md)).
Send back: the script's output if it differs.

## 2. Identify

```
.\pakon-cli.exe --log debug identify
```

Expected: model **F-135+** (controllers 0x40/0x44) and the OEM-style
version line, which this unit's own OEM logs report as
`Version USB 0x03,0x0F  Lamp 0x05,0x0A  Motor 0x05,0x06`.
Failure signatures: `usb_open_failed` (driver/binding), `ppb_bad_status`
or timeouts on `0x07` (the `0x03 = 01` page select did not take — send the
log). Send back: full console output.

## 3. Status

```
.\pakon-cli.exe --log debug status
```

Expected: all three polls answer; temperature payload is 1/16 °C
(e.g. `d2 01` = 466 → 29.1 °C). Send back: console output.

## 4. EEPROM (read-only)

```
.\pakon-cli.exe --log debug eeprom --out eeprom.bin
```

Expected: `serial 17373, type 1351`, section A and B `primary` (or
`backup` with a note), six lines `baseN[-ir]  Offset …  rate …`.
**A `WARNING: … FALLBACK values` line means this unit's EEPROM could not be
read — stop and send everything.** Send back: console output and
`eeprom.bin` (it is this unit's factory calibration; keep a copy).

## 5. Dry run

```
.\pakon-cli.exe scan --dry-run --config base4 --first-light --out-prefix fl1
```

Prints the exact init and teardown frames and the outputs; opens nothing.
Nothing to send unless it fails.

## 6. First light (lamp on, motor never moves)

Empty gate.

```
.\pakon-cli.exe --log debug scan --live-scan --config base4 --first-light --out-prefix fl1
```

What happens: init block → lamp warm-up (polls until the lamp reports
stable; OEM limit 300 s, ~6.5 s on the captured unit) → Corrections (dark
servo, LED current servo with settle waits, on-time servo; a minute or two)
→ 256 white lines → teardown.

Expected:
- `corrections:` A/D offsets near the start values, dark rounds ≤ 8, LED
  currents within R≤4 G≤20 B≤20 IR 0 (IR off), on-times well below 1.0;
- `stream:` overflows 0, transfer errors 0, line resyncs 0;
- `teardown: all steps acknowledged`; exit code 0;
- files `fl1.white.pakraw`, `fl1.white.tiff`, `fl1.scan-stats.txt`,
  `fl1.scan.json`. The TIFF should be an even bright field (with fixed
  column structure), no black bands, no diagonal shear.

Failure signatures:
- `lamp not stable after … s` — lamp/TEC side; send the stats file;
- `EC_InsufficientLight: <channel> …` — that channel hit its LED ceiling and
  gain limit (weak LED or a bad current mapping);
- `no image data … (is the acquire bit set?)` — the stream did not start;
- `ring overflow` — the host fell behind (USB/driver); send stats;
- `teardown: STEP FAILURES` — send everything immediately, and power-cycle
  the scanner if the lamp is visibly on.

Then repeat with IR: `--config base4-ir --out-prefix fl1ir` (IR currents
then allowed up to 8).

Send back: console output and all four files of each run.

## 7. Short film pass (row budget)

Load a test strip you do not care about. Keep a hand near the power switch
the first time.

```
.\pakon-cli.exe --log debug scan --live-scan --config base4 --film-rows 3000 --out-prefix film1
```

Expected: the motor starts, the window ends at 3000 rows (or at film end),
the motor stops. `scan-stats` shows `line period ≈ 0.9 ms` (Base 4:
1875 × 0.48 µs), overflows 0, `film end: row_budget` or `film_passed`.
Also try Ctrl+C once mid-pass: expected `cancelled` and the teardown
report, exit code 1, motor stopped, lamp off.

Send back: console, `film1.*`, and what the motor/strip physically did.

## 8. Full scan

```
.\pakon-cli.exe --log debug scan --live-scan --config base4 --film-rows 60000 --out-prefix strip1
```

The row cap is only a safety bound; film end normally ends the pass (a
45 s no-film timeout ends it if no film is seen). Then the other modes:
`base8`, `base16`, each `-ir`. Send back: console + all outputs.

## 9. Capturing the OEM stack for comparison (optional, very useful)

This records what the OEM software sends on **this** unit, to diff against
pakon-cli with `tools/compare_sessions.py`.

1. Install Wireshark with **USBPcap**.
2. **Driver swap to OEM:** Device Manager → the scanner → Update driver →
   Browse → Let me pick → choose the OEM Pakon driver (`F135usb2`). Unplug
   and replug if it does not rebind.
3. Start Wireshark on the USBPcap interface of the root hub the scanner is
   on; find its device address from the first packets after replug (display
   filter `usb.idVendor == 0x0f05`), then filter
   `usb.device_address == <N>` for checking (capture everything; the
   converter filters).
4. TLXClientDemo: start it, wait for "Scanner Initialized", then **Scan** →
   Base 4, Negative, 35 mm, no options → **Scan** with a strip loaded; let
   it finish. Stop the capture, save as `oem-base4.pcapng`.
5. **Driver swap back to WinUSB** (same dialog, pick WinUSB / the
   `driver/PakonWinUSB.inf` package), run step 8's command while capturing
   the same way → `ours-base4.pcapng`.
6. Convert and compare (any machine with Python 3):

```
python tools/pcap_to_jsonl.py oem-base4.pcapng -o oem-base4.jsonl --label oem-base4
python tools/pcap_to_jsonl.py ours-base4.pcapng -o ours-base4.jsonl --label ours-base4
python tools/compare_sessions.py oem-base4.jsonl ours-base4.jsonl > diff.txt
```

`pcap_to_jsonl` keeps commands, replies, control-request setup fields and
image byte counts only — no pixels, no EEPROM contents. The expected
differences are listed in [OEM_RE.md](OEM_RE.md) §11.3; anything else in
`diff.txt` is new information. Send back: both `.jsonl` files and
`diff.txt` (the `.pcapng` files contain image data and EEPROM payloads —
keep them local).
