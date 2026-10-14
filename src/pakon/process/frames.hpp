#pragma once

// Frame separation for a film window (processing, never on the scan
// path): which line ranges of a .pakraw hold the individual pictures.
//
// 1. Line profile: green transmission t = (raw - dark) / (white - dark)
//    per pixel, with the Corrections dark and open-gate references from
//    the sidecar (calibration window shifted onto the film window); per
//    line its mean and 10th / 90th percentile over the central 80 %.
// 2. Gaps: on a negative the space between frames is bare film base:
//    the line is uniform at the base transmission (p10 >= 0.85 base,
//    p90 <= 1.15 base), where a picture always has denser areas. Base =
//    median mean of the uniform lines. Runs closer than 3 % of a pitch
//    merge; runs shorter than 8 lines are dropped.
// 3. Pitch: 35 mm frames repeat every 38 mm. The seed comes from the
//    transport: film2 (Base 4, motor rate 25676, 0.904 ms lines) showed
//    gaps every 1633 lines = 1.476 s, which scales with 1/rate. The
//    pitch and phase are then fitted to the gaps found (most gaps within
//    4 % of the grid, least squares), so a thin or blank frame without a
//    visible gap does not shift the others.
// 4. Frames: between consecutive grid gaps, each edge at the seen gap
//    run (or the predicted line when none was seen), clipped to the
//    film; kept when at least 80 % of a pitch long.
//
// Measured on this unit's scans of 2026-10-10/13; [INFERRED design] —
// the OEM's framing code (TLB processing thread) is not recovered.

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "pakon/errors/error.hpp"
#include "pakon/process/json.hpp"
#include "pakon/process/pakraw.hpp"

namespace pakon::process {

inline constexpr double kFramePitchSecondsAtRate25676 = 1.476; // film2: 1633 x 0.904 ms

// What frame separation needs from <prefix>.scan.json.
struct ScanInfo {
    std::string mode;
    std::size_t film_start{0};             // first film line (detector), else 0
    std::optional<std::size_t> film_end;   // end line, when the film passed
    double motor_rate{0};
    double line_period_ms{0};
    std::size_t pixels{0};                 // film-window pixels per line
    bool ir{false};
    std::size_t reference_shift{0};        // film pixel p = calibration pixel p + shift
    std::vector<double> dark_green;        // calibration-window references
    std::vector<double> white_green;
};
Result<ScanInfo> scan_info_from_sidecar(const Json& sidecar);

struct LineStats {
    float mean{0};
    float p10{0};
    float p90{0};
};
Result<std::vector<LineStats>> line_profile(PakrawReader& raw, const ScanInfo& info);

struct GapRun {
    std::size_t first{0}; // [first, last) lines of bare base
    std::size_t last{0};
};
struct Frame {
    std::size_t first{0}; // [first, last) lines of the picture
    std::size_t last{0};
    bool leading_gap_seen{false};
    bool trailing_gap_seen{false};
};
struct FrameSplit {
    double pitch_seed{0};
    double pitch{0};
    double base{0};
    std::size_t gaps_on_grid{0};
    std::vector<GapRun> gaps;
    std::vector<Frame> frames;
};
FrameSplit find_frames(std::span<const LineStats> profile, const ScanInfo& info);

} // namespace pakon::process
