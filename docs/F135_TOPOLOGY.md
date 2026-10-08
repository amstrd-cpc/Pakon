# F135 runtime USB topology (read-only descriptor discovery)

**Status:** implemented (`pakon-cli descriptors`, sources
`src/pakon/usb/descriptors.*`); **hardware run pending** — every
observation table below is `[PENDING]` and is filled verbatim from the
lab unit's captured output, then labelled.

Evidence labels follow [BOOT_CHAIN.md](BOOT_CHAIN.md):
**[PROVEN]** observed from the device on the record · **[STATIC]**
established by static analysis of committed artifacts · **[INFERRED]**
a reading of observed bytes with the reasoning stated · **[UNKNOWN]**
not established · **[PENDING]** awaiting the hardware run.

---

## 1. Goal and scope

Determine the complete USB topology of the live `0F05:F135` runtime —
device and configuration descriptors, every interface / alternate
setting / endpoint, and the WinUSB interface path — **without sending a
single vendor-specific command**. This closes the gap between what
[BOOT_CHAIN.md](BOOT_CHAIN.md) proved at the identity level (device
re-enumerates as `USB\VID_0F05&PID_F135`, bound to WinUSB) and what the
wire format of the operational device actually is.

Scope decisions:

- Only the **booted `0F05:F135`** is scanned. A cold `0F05:F235` unit is
  never touched by this diagnostic (the probe remains the cold-device
  read-only tool).
- The configuration **bytes** are read, never acted upon: no
  configuration select, no alternate-setting switch, no pipe policy.
- Endpoint **roles** are not observed by this scan — they are readings
  (§ 4), each labelled `[INFERRED]` until a future phase demonstrates
  traffic on them.

## 2. What the diagnostic sends (and what it never does)

Command line (Windows, repo checkout):

```powershell
.\build\apps\pakon-cli\Release\pakon-cli.exe descriptors
```

| Operation | Kind | Why it is inside the envelope |
|---|---|---|
| SetupAPI enumeration (identity rules) | no I/O | same discovery as `list`; opens nothing |
| `GET_DESCRIPTOR` device / configuration / string via `WinUsb_GetDescriptor` | **standard** control read (`bmRequestType` 0x80/0x82) | the definition of read-only descriptor discovery |
| `WinUsb_QueryDeviceInformation(DEVICE_SPEED)` | driver-side query | talks to winusb.sys, not the wire; `bConfigurationValue` has no WinUSB read API — an active configuration is implied by `WinUsb_Initialize` succeeding |
| `WinUsb_QueryInterfaceSettings` / `QueryPipe` / `GetAssociatedInterface` | driver-side query | topology cross-check independent of the raw bytes |
| CreateFile + `WinUsb_Initialize` (shared overlapped params, `usb/win_usb_open.hpp`) | host-side open | same proven open as `list` / transport; released immediately |

Explicitly **not** done — same prohibitions as the probe charter:
no vendor control transfer (nothing of the `0xA0`/`0xA3`/`0xA4`/`0xA9`
family, no `bRequest >= 0x40`), no firmware upload, no USB/port reset,
no set-configuration / alt-setting switch / pipe-policy change
(no device or driver reconfiguration), no bulk traffic of any kind.
Every write in the boot chain was already volatile; this diagnostic
performs **no writes at all**.

## 3. Observation record (lab unit) — `[PENDING]`

Filled from the pasted `pakon-cli descriptors` output.

### 3.1 Device context `[PENDING]`

| Field | Value |
|---|---|
| Instance ID | `[PENDING]` |
| Hardware ID | `[PENDING]` |
| WinUSB interface path | `[PENDING]` |
| Open mode | `[PENDING]` |
| Link speed | `[PENDING]` |
| Configuration active | implied by `WinUsb_Initialize` success — `bConfigurationValue` is **not exposed** by the WinUSB API (`[UNKNOWN]`/not readable, not a run result) |

### 3.2 Device descriptor (18 bytes) `[PENDING]`

| Field | Value | Label |
|---|---|---|
| bcdUSB / class triad / bMaxPacketSize0 | `[PENDING]` | `[PENDING]` |
| idVendor / idProduct / bcdDevice | `[PENDING]` | `[PENDING]` |
| iManufacturer / iProduct / iSerial / bNumConfigurations | `[PENDING]` | `[PENDING]` |
| raw 18 bytes | `[PENDING]` | `[PENDING]` |

Cross-check against BOOT_CHAIN § 5 static read (Pakon7 descriptor @
`0x1000` = `12 01 00 02 00 00 00 40 05 0F 35 F1 02 00 01 02 03 01`):
does the live device match the bytes that were written to its RAM?
Agreement = `[PROVEN]` link between the downloaded image and the
enumerated identity; any difference is recorded as observed.

### 3.3 Configuration descriptor `[PENDING]`

| Field | Value | Label |
|---|---|---|
| wTotalLength / bNumInterfaces / bConfigurationValue | `[PENDING]` | `[PENDING]` |
| bmAttributes / bMaxPower / iConfiguration | `[PENDING]` | `[PENDING]` |
| raw bytes (hex rows) | `[PENDING]` | `[PENDING]` |
| TLV parse anomalies (if any) | `[PENDING]` | `[PENDING]` |

### 3.4 Interfaces, alternate settings, endpoints `[PENDING]`

| # | alt | class/subclass/protocol | EP | dir | type | wMaxPacket | bInterval | Label |
|---|---|---|---|---|---|---|---|---|
| `[PENDING]` | | | | | | | | |

Recorded twice in the output — parsed from the raw descriptor bytes and
queried through WinUSB — and the two views must agree. A mismatch is
reported as observed (it would itself be a finding).

### 3.5 Referenced string descriptors `[PENDING]`

| Index | Language | Text | Label |
|---|---|---|---|
| `[PENDING]` | | | |

Note: `iSerialNumber` resolution addresses the carried `[UNKNOWN]`
"serial derivation" (BOOT_CHAIN § 8): instance segment `010-203-04`
vs. whatever string index 3 actually returns.

## 4. Endpoint-role reading — all `[INFERRED]` `[PENDING]`

Framing (labels as they stand before the run):

- The **command channel** endpoints `0x01 OUT` / `0x81 IN` (bulk pair)
  are `[STATIC]` from the primary spec (pakon-reference ppb-protocol.md
  "Command channel", cited in `usb/transport.hpp`); their presence in
  the live descriptors will be `[PROVEN]` when the bytes show them.
- The **image stream** rides "a separate bulk IN endpoint" whose number
  the primary spec does not state — `[UNKNOWN]` until § 3.4 shows it;
  the candidate identification is `[INFERRED]`.
- Any further endpoints (status/interrupt, vendor-class interfaces) get
  role hypotheses here only, each with its reasoning, all `[INFERRED]`.

Conclusions are written into this section after the run.

## 5. Safety record

- Traffic actually sent: standard `GET_DESCRIPTOR` reads (device,
  configuration, string) and WinUSB driver-side queries — nothing else.
- Never touched: `0xA0`/`0xA3`/`0xA4`/`0xA9`, any `bRequest >= 0x40`,
  firmware upload, reset, configuration/alt-setting changes, pipe
  policies, bulk endpoints.
- No new writes beyond what the validated boot chain already performs
  (all volatile); this diagnostic adds none.
- Implementation guards: the scan lives in `descriptors_win.cpp` and
  contains no `WinUsb_ControlTransfer`, no `WinUsb_Set*`, no
  `WinUsb_ResetPipe`, no `WinUsb_ReadPipe`/`WritePipe` call sites
  (searchable claims — verify with `grep` on that file).

## 6. Open questions this run can and cannot settle

Settled by the run (if the stages succeed): raw descriptor bytes,
live topology, string contents, link speed, current configuration,
raw-vs-WinUSB agreement.

Remains `[UNKNOWN]` afterwards (out of scope): endpoint data formats
and traffic semantics (needs future, separately-approved phases);
why `0xA9` byte[7] = `0x04`; whether the `0xA4` preamble is necessary
given stage-1 (ablation never run); OEM report-stack identity beyond
what `pakon-cli attrib` showed (Image `F135usb2.sys` vs WinUSB).
