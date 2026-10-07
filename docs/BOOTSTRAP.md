# Bootstrap (cold → warm): evidence status and the read-only probe

**Status 2026-10-07 — evidence survey complete. UPDATE (same day): every
gap in § 3 has since been closed by OEM-artifact reverse engineering, and
the whole cold→warm boot ran successfully on hardware — the validated
sequence, artifacts, MD5s and evidence markers live in
[BOOT_CHAIN.md](BOOT_CHAIN.md). What this repository *implements* is
still the conservative, read-only bootstrap probe (`pakon-cli probe`);
the C++ loader is designed but not yet written
([LOADER_DESIGN.md](LOADER_DESIGN.md)).**

Source of truth: pakon-reference `docs/usb-identity-and-firmware.md`,
`docs/calibration.md` § The read, `docs/per-unit-data-and-safety.md` —
mapped through [PAKON_REFERENCE.md](PAKON_REFERENCE.md).

---

## 1. The goal flow

```
cold 0f05:f235 ──WinUSB bound──► bootstrap protocol ──► firmware upload
      ──► reset / re-enumeration ──► warm identity (0f05:f135 / 35f2 / f335)
      ──► PPB init ──► model detection ──► scanner operation
```

The WinUSB transport stage is proven on Windows 11 (binding, GUID,
`CreateFile`/`WinUsb_Initialize`, endpoint discovery — see
[WINUSB_TEST.md](WINUSB_TEST.md) and [STATUS.md](STATUS.md)). This document
records exactly how far the repository's own evidence reaches along the rest
of that flow, and what was built in the meantime.

## 2. What the repository confirms

| Claim | Marker | Source (via PAKON_REFERENCE.md) |
|---|---|---|
| Cold identity `0f05:f235` = all models before firmware load; warm identities `f135`/`35f2`/`f335` | `[DOCUMENTED]` | § 2.1, usb-identity-and-firmware.md |
| Personality = **8-byte C0 record at I2C 0x51**, read via **vendor IN `0xA9` with `wIndex 0` from the stage-1 loader** | `[CONFIRMED on hardware, August 2026]` | § 2.1 |
| Vendor `0xA9`: `wValue` = offset, ≤ 32 bytes; vendor OUT `0xA4` (`wValue 0x00A5`, `wIndex 0x1234`) selects the per-unit EEPROM first | `[CONFIRMED]` | § 2.5, § 2.9, calibration.md § The read |
| Firmware load = "standard FX2 sequence (CPUCS reset, `0xA0`/`0xA3` downloads, re-enumerate)" | `[DOCUMENTED]` — **sequence name only; firmware bytes out of scope** (`CONVENTIONS.md` excludes them) | § 2.1 |
| Reading, polling and PPB commands to known controllers have no recorded incident | `[CONFIRMED]` (by absence of incidents) | § 2.11 rule 8 |

## 3. Evidence gaps — why no upload path is implemented

> **Update 2026-10-07:** all seven gaps below are closed — static sources
> plus hardware confirmation, item by item, in
> [BOOT_CHAIN.md](BOOT_CHAIN.md): the firmware bytes come from the pinned
> OEM artifacts (`F235Ldr.sys`, `Pakon7.hex`, fetched + MD5-gated at run
> time, never committed); the `0xA0`/`0xA3` request layout, the CPUCS
> addresses, the re-enumeration mechanism, the PID/REV selection rule and
> the stage-1 request set were recovered from `F235Ldr.sys` /
> `F235usb2.inf` and confirmed byte-for-byte in the boot run. The list
> below is kept as the historical survey.

The sequence above names the *steps* but none of the *parameters*. The
following are **not stated anywhere in this repository**; each would have to
be invented to write an uploader, and inventing proprietary packet formats is
forbidden by this project's rules:

1. **Firmware bytes** — permanently excluded by pakon-reference's own scope
   rules. An image can never ship here.
2. **`0xA0`/`0xA3` request layout** — only the request numbers are named;
   `wValue`/`wIndex` semantics, data-stage size limits and block sequencing
   are unstated.
3. **CPUCS reset** — the address and the reset/run value are not stated
   (the standard Cypress values are *external* knowledge, not repo evidence).
4. **Re-enumeration trigger** — how the device is told to drop the cold
   identity and come back warm (descriptor change? bus reset? internal
   command?) is unstated.
5. **Which warm PID a given unit re-enumerates as** — the identity table
   lists the candidates, not the selection rule.
6. **The stage-1 loader's full request set** — only the `0xA9` personality
   read is `[CONFIRMED]`.
7. **No captures in-repo** — the loader session lives (if anywhere) in the
   external `alibosworth/pakon-captures` corpus (PAKON_REFERENCE § 6.12).

One parameter is an explicit inference, kept visible on purpose: the
personality read's own `wValue` is not separately stated, so the probe sends
`wValue 0` by applying `0xA9`'s documented "wValue = offset" rule
(calibration.md). Mark `[INFERRED]`. If the device stalls or rejects it,
**that is evidence too** — record it verbatim (§ 6).

## 4. What is implemented: `pakon-cli probe`

A single read-only step, evidence-pinned end to end:

```
pakon-cli [--log trace] probe
```

1. Enumerate (read-only, no traffic) and open with `open_first(cold_ok=true)`
   — the **only** call site that accepts a cold device.
2. Send **exactly one** vendor control read: `0xA9`, `wValue 0`, `wIndex 0`,
   length 8 (§ 2's confirmed stage-1 path).
3. Print the raw 8 bytes (hex + ASCII-if-printable) and the bootstrap state.
   The C0 record's byte layout is not documented in-repo, so the probe
   **never decodes it** — raw bytes only.

What it structurally cannot do (proven by `tests/bootstrap/probe_test.cpp`
against a recording fake transport):

- no bulk traffic → no PPB frames → **no type-byte-0 exposure**;
- no control writes at all; vendor `0xA2` (EEPROM write) and `0xA4`
  (EEPROM select) are test-pinned as forbidden in this path;
- a short/wrong-length reply is rejected (`usb_short_transfer`), never read
  as a personality.

`identify`/`status` are unchanged: they still refuse cold devices with the
honest "firmware loading is not implemented" error.

Output shape (schematic — not a hardware capture; hardware output must be
recorded from the real run, § 6):

```
State: cold/bootstrap (0f05:f235) — firmware not loaded
VID: f235...  (Hardware ID / Device instance lines as in `list`)
Personality: 8-byte C0 record (stage-1 loader, vendor IN 0xA9, wValue 0, wIndex 0)
  raw:   xx xx xx xx xx xx xx xx
  ascii: ........
```

## 5. Safety analysis

| Rule | How the probe complies |
|---|---|
| Never type-byte-0 frames | No PPB/bulk I/O exists in the probe path |
| Never vendor `0xA2` / `0xA4` | Only `0xA9` is issued; constants test-pinned |
| Never bootloaders / EEPROM bus writes | No writes of any kind |
| EEPROM reads once per power cycle | The `0xA4`-select per-unit read path is **not used** by the probe at all; the stage-1 personality read is a separate confirmed path. When Phase 9 adds the EEPROM read, the read-once rule applies |
| Cold devices never opened for I/O | Narrowed, not removed: only `probe` opens them (`cold_ok=true`), for one read; `identify`/`status` still refuse |

## 6. Hardware validation required (Windows — record verbatim)

Run on the Windows 11 machine with the cold unit attached:

```powershell
cd "C:\Users\Clime Film Lab\Desktop\pakon"
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure   # expect 4/4 suites, 45 cases
.\build\apps\pakon-cli\Release\pakon-cli.exe --log trace probe 2>&1 | Tee-Object -FilePath probe-cold.log
```

Note (2026-10-07): a first probe run failed inside the open path with
`WinUsb_Initialize failed: 6` (`ERROR_INVALID_HANDLE`) because the
transport's `CreateFile` opened non-overlapped while the enumeration path
was opened `FILE_FLAG_OVERLAPPED` — the two sites had drifted. Both now
share the unit-tested `usb/win_usb_open.hpp`; hardware re-verification of
the open is exactly what this run does.

**Recorded 2026-10-07:** the open fix verified on hardware (both WinUSB
open sites behave identically). The probe's cold baseline answered row 3
(`0xA9` → win32 `121` — boot ROM silent, as expected); after the approved
stage-1 upload the same read answered `C0-05-0F-35-F2-07-AA-04` (row 1),
and the full boot transcript follows in [BOOT_CHAIN.md](BOOT_CHAIN.md)
§ 3, with the interpretation chain in § 5.

Interpretation — record which row matched, with the date:

| Observed | Meaning |
|---|---|
| `Personality: … raw: <8 bytes>` + exit 0 | **Stage-1 loader answered over WinUSB** — first protocol I/O from this stack. Save the 8 bytes; they are the comparison basis for the `F235_AA07` personality key |
| `usb_short_transfer: personality read returned N bytes` | Loader answered partially — record `N` and the trace log; itself new evidence |
| `control read 0xA9/0x0000/0x0000 failed: <code>` | Request rejected/stalled — record the Win32 code; may falsify the `wValue` inference (§ 3), which is still a result |
| `CreateFile failed … Windows error <n> (<text>)` | The interface path could not be opened at all (stage 1) — record code + text; binding/permission problem |
| `WinUsb_Initialize failed … Windows error <n> (<text>)` | CreateFile succeeded, initialization rejected the valid handle (stage 2) — record code + text; expected to be gone after the shared overlapped-open fix, and if it appears it is new evidence |
| `discovered but not openable` | No function driver bound at all (Code 28) → install the INF package |
| `no Pakon F-X35 device detected` | Nothing enumerated → check `pakon-cli list` / Device Manager |

**Stop condition:** `probe` is the only I/O. No `0xA0`/`0xA3`, no firmware
bytes, no EEPROM access — those remain a separate future milestone requiring
explicit approval and new evidence (WINUSB_TEST.md § Safety boundary).
*(Superseded by an explicit full-boot approval on 2026-10-07; the approved
sequence and its safety gates are recorded in
[BOOT_CHAIN.md](BOOT_CHAIN.md) § 7 — EEPROM/bootloader/type-byte-0 rules
remain in force unchanged.)*

## 7. Next steps once the probe result exists

*(2026-10-07: items 1–3 satisfied — personality layout decoded as the
registry key `F235_AA07`, evidence collected, loader design written;
see [BOOT_CHAIN.md](BOOT_CHAIN.md) and [LOADER_DESIGN.md](LOADER_DESIGN.md).)*

1. Compare the observed 8 bytes with the reference's personality key
   (`F235_AA07`); if a layout becomes derivable, document it with its
   confidence marker — do not assume one before then.
2. Collect the evidence listed in § 3 (OEM loader captures, CPUCS and
   re-enumeration mechanics) before any uploader design is written.
3. Only then: a firmware-loader design document + explicit approval
   milestone; until then the stack deliberately stops at discovery →
   binding → read-only probing.
