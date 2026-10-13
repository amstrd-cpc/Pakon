#pragma once

// Corrections: the OEM's closed calibration loop (bCalibrateLEDs,
// TLB@0x10020dc0; docs/OEM_RE.md §9), run inside one acquisition window:
//
//   1. calibration geometry (masked pixels + gate), lamp off;
//   2. dark-offset servo: gains = code(1.2), offsets from +10, up to 8
//      rounds of 32-line averages over the masked pixels, target 300 ±32,
//      step trunc(−(mean − 300) × 3/112)            (TLB@0x1001e1c0);
//   3. dark reference (IR plane on first in IR modes);
//   4. LED-current servo: from 1 at the start on-times kServoStartDuty,
//      +1 while the peak stays ≤ 0xFA00 (R, G), 0xFFDC (B), 0x9C40 (IR)
//      and below the ceiling; then on-time = start × (n−1)/n
//                                                    (TLB@0x1001e7b0);
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

// Colour-negative film boost R, G, B: film-pass on-time = open-gate
// on-time × boost (the OEM's DutyCycle vs DutyCycleOpenGate pair, ~10^D
// of the film base). From unit 16402's film/open-gate ratios [CAP
// base4.jsonl 22.117 vs 24.832]; this unit's OEM run gives 1.397 2.514
// 5.05 [CAP 17373 oem-base4.jsonl 67.628 vs 68.106, 2026-10-13].
inline constexpr std::array<double, 3> kFilmBoost{1.39, 2.51, 5.18};
// The OEM runs the LED-current servo at these on-times, not at 1.0:
// 324/180/87 of base 450 at currents 1 [CAP 17373 oem-base4.jsonl
// 56.325], ~1/boost per channel, so the currents climb until the
// BOOSTED film on-time fits (ended at R3 G14 B7; this stack's 1.0 start
// ended at R2 G3 B2 and clipped the film boost). Back-offs (n−1)/n scale
// this start (64.750: 0.480 0.371 0.167). Their source in TLB is
// [UNKNOWN]; the values are the captured ones (180/450 is 0.400 exactly,
// not 1/2.51). IR start 1.0: no IR capture yet.
inline constexpr std::array<double, 3> kServoStartDuty{0.720, 0.400, 0.193};

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
