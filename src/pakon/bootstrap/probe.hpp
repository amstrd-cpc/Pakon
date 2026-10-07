#pragma once

// Bootstrap (cold → warm) read-only probe: the stage-1 loader personality
// read, with every constant pinned to pakon-reference evidence.
//
// Why this exists: the FX2 firmware-load sequence is documented by name
// only — "CPUCS reset, 0xA0/0xA3 downloads, re-enumerate" [DOCUMENTED]
// (docs/PAKON_REFERENCE.md § 2.1) — and the firmware bytes themselves are
// excluded from pakon-reference's scope rules. The request layout, the
// CPUCS details and the re-enumeration trigger are therefore NOT in this
// repository, so no upload path is implemented and no packet format is
// invented (the exact evidence gaps are listed in docs/BOOTSTRAP.md).
//
// What IS confirmed is the read-only personality read from the stage-1
// loader: "read via vendor request 0xA9 with wIndex 0 from the stage-1
// loader [CONFIRMED on hardware, August 2026]" — the 8-byte C0 record at
// I2C 0x51 (docs/PAKON_REFERENCE.md § 2.1).
//
// Everything here is pure logic — no I/O, no Windows headers — so
// tests/bootstrap/probe_test.cpp can pin the request layout and prove
// (with a fake transport) that a probe performs exactly one control read
// and nothing else.
//
// Safety: probe() may only ever issue the read below. It never issues a
// control write, never touches vendor 0xA2 (EEPROM write) or 0xA4
// (EEPROM select), and never sends PPB frames (no type-byte-0 exposure).
// Reading has no recorded incident (per-unit-data-and-safety.md rule 8).

#include <array>
#include <cstdint>
#include <span>
#include <string>

#include "pakon/errors/error.hpp"
#include "pakon/usb/transport.hpp"

namespace pakon::bootstrap {

// Documented stage-1 personality read:
//   - request 0xA9, wIndex 0 — "[CONFIRMED on hardware, August 2026]"
//     (usb-identity-and-firmware.md via PAKON_REFERENCE.md § 2.1);
//   - wValue = offset — 0xA9's documented meaning from calibration.md
//     § The read ("0xA9 (read, wValue = offset, ≤32 bytes)"); the
//     personality read's own wValue is not separately stated in-repo,
//     so offset 0 is an [INFERRED] application of that rule (see
//     docs/BOOTSTRAP.md § Evidence gaps — a stall here would itself be
//     evidence);
//   - length 8 — the record is "an 8-byte C0 record".
inline constexpr std::uint8_t kPersonalityRequest = 0xA9;
inline constexpr std::uint16_t kPersonalityValue = 0x0000;  // offset 0
inline constexpr std::uint16_t kPersonalityIndex = 0x0000;  // wIndex 0
inline constexpr std::uint16_t kPersonalityLength = 8;      // C0 record size

// The 8-byte C0 personality record. The byte layout is not documented
// in-repo ("8-byte C0 record at I2C 0x51" is all that is stated), so the
// record is carried as raw bytes and never re-encoded or decoded.
struct Personality {
    std::array<std::uint8_t, 8> bytes{};
};

// A completed read-only probe of one device.
struct ProbeReport {
    usb::DeviceInfo device;      // identity as enumerated (state, IDs)
    Personality personality;     // raw stage-1 loader record
};

// Validate a raw control-read result: exactly kPersonalityLength bytes.
// A short transfer must never be read as a personality
// (ErrorKind::usb_short_transfer).
Result<Personality> parse_personality(std::span<const std::uint8_t> raw);

// Perform the read-only probe: one vendor control read (the request
// above) and nothing else — no bulk traffic, no control writes, no PPB.
Result<ProbeReport> probe(usb::IUsbTransport& transport);

// "xx xx xx …" lowercase hex rendering for CLI output.
std::string hex_string(std::span<const std::uint8_t> bytes);

} // namespace pakon::bootstrap
