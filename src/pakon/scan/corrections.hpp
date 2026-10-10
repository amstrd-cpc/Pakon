#pragma once

// Corrections: the OEM's closed calibration loop (bCalibrateLEDs,
// TLB@0x10020dc0; docs/OEM_RE.md §9), run inside one acquisition window:
//
//   1. calibration geometry (masked pixels + gate), lamp off;
//   2. dark-offset servo: gains = code(1.2), offsets from +10, up to 8
//      rounds of 32-line averages over the masked pixels, target 300 ±32,
//      step trunc(−(mean − 300) × 3/112)            (TLB@0x1001e1c0);
//   3. dark reference (IR plane on first in IR modes);
//   4. LED-current servo: from 1, +1 while the peak stays ≤ 0xFA00 (R, G),
//      0xFFDC (B), 0x9C40 (IR) and below the ceiling; on-time fraction
//      (n−1)/n                                       (TLB@0x1001e7b0);
//   5. on-time servo: duty × 63968/peak (IR 39968) into [0xF9C0, 0xFA00];
//      a channel that needs duty > 1 gets current +1 (below ceiling), else
//      A/D gain +1 (≤ 0x3E), else EC_InsufficientLight (TLB@0x1001ec90);
//   6. white (open-gate) reference with the final settings.
//
// Every value reaches the device through ScanDevice, so LED currents are
// clamped to the firmware ceilings whatever the loop asks for.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "pakon/errors/error.hpp"
#include "pakon/scan/device.hpp"
#include "pakon/scan/lines.hpp"
#include "pakon/scan/modes.hpp"

namespace pakon::scan {

inline constexpr std::size_t kAverageLines = 32;    // bCalibrateAcquireAndAverageLines
inline constexpr int kDarkTarget = 300;             // TLB@0x1001e1c0
inline constexpr int kDarkTolerance = 32;
inline constexpr int kDarkStartOffset = 10;
inline constexpr int kDarkRounds = 8;
inline constexpr double kGainStart = 1.2;           // code 13
inline constexpr std::uint16_t kGainCodeMax = 0x3E;
inline constexpr std::uint32_t kPeakVisible = 0xFA00; // R, G
inline constexpr std::uint32_t kPeakBlue = 0xFFDC;
inline constexpr std::uint32_t kPeakIr = 0x9C40;
inline constexpr double kDutyTarget = 63968.0;
inline constexpr double kDutyTargetIr = 39968.0;

struct CorrectionsResult {
    std::array<int, 3> offsets{};               // A/D offsets R, G, B
    std::array<std::uint16_t, 3> gains{};       // A/D gain codes R, G, B
    protocol::scan::LedCurrents currents{};
    Duties duties{};
    LineAverage dark;                           // per-pixel dark reference
    LineAverage white;                          // per-pixel open-gate reference
    std::size_t dark_rounds{0};
    std::size_t current_rounds{0};
    std::size_t duty_rounds{0};
    std::vector<std::string> log;               // one line per servo step
};

struct CorrectionsConfig {
    ScanMode mode{};
    std::uint16_t offset{0}; // this unit's EEPROM Offset for the mode's base
    std::chrono::milliseconds line_timeout{std::chrono::seconds(5)};
};

// Precondition: stream started, calibration_geometry(mode, offset, false)
// applied and the acquire bit set. Leaves the lamp on with the final
// settings and the acquire bit set (the caller ends the window).
Result<CorrectionsResult> run_corrections(ScanDevice& device, LineReader& lines,
                                          const CorrectionsConfig& config);

} // namespace pakon::scan
