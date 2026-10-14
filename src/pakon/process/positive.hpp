#pragma once

// Stage 1 negative -> positive (processing, never on the scan path): a
// neutral, physically grounded conversion. Not the "Pakon look" (that is
// PakonIMAu.dll's ColNeg + Ansel pipeline, OEM_RE §13 — stage 2).
//
// Per pixel and channel c (R, G, B):
//   t  = (raw - dark) / (white - dark) x on_cal / on_film
//        flat field with the Corrections references; the white was
//        measured at the calibration on-time, the film lit at the boosted
//        one (LED output taken as linear in on-time [INFERRED]);
//   t' = t / base_c        base = orange-mask transmission, measured on
//                          the bare-base gap lines between the frames;
//   D  = -log10(t')        density above base;
//   x  = (D - Dlo_c) / (Dhi_c - Dlo_c), clamped to [0, 1]
//        Dlo/Dhi = the 0.1 / 99.9 % density percentiles over every
//        picture of the window: one black / white point per channel for
//        the whole roll, which also balances the channels;
//   y  = (10^(R (x - 1)) - 10^-R) / (1 - 10^-R)   scene-linear, R decades;
//   out = sRGB-encoded y, 16 bit.
// The IR lane is not used here (Digital ICE is a later stage).

#include <array>
#include <cstdint>
#include <vector>

#include "pakon/errors/error.hpp"
#include "pakon/process/frames.hpp"
#include "pakon/process/pakraw.hpp"

namespace pakon::process {

struct PositiveOptions {
    double range_decades{2.2};
    double low_percentile{0.001};
    double high_percentile{0.999};
};

struct PositiveParams {
    std::array<double, 3> base{};    // base transmission per channel
    std::array<double, 3> d_low{};   // density mapped to black
    std::array<double, 3> d_high{};  // density mapped to white
    std::size_t base_lines{0};       // gap lines the base came from
    double range_decades{2.2};
};

// Roll-wide parameters from the gaps (base) and the pictures (densities).
Result<PositiveParams> measure_positive(PakrawReader& raw, const ScanInfo& info,
                                        const FrameSplit& split, const PositiveOptions& opt = {});

// Which end of the film went in first. It decides the picture's
// orientation in the stream (feeding end-for-end rotates it 180 degrees):
//  - head first (the leader first; the normal load — the OEM's status
//    word flags "Tail first", bit 0x20000000, OEM_RE §8): the CCD pixel
//    axis is the picture's vertical as is, the travel axis runs right to
//    left. roll1 (fogged leader scanned first): faces upright only so.
//  - tail first: the opposite on both axes. film2: its seven-segment date
//    imprint reads "22:18" at the bottom right only so (2026-10-14).
// Only the viewing outputs are oriented; the .pakraw keeps stream order.
enum class Feed { head_first, tail_first };
constexpr std::size_t picture_row(std::size_t p, std::size_t pixels, Feed feed) {
    return feed == Feed::tail_first ? pixels - 1 - p : p;
}
constexpr std::size_t picture_col(std::size_t line, std::size_t lines, Feed feed) {
    return feed == Feed::head_first ? lines - 1 - line : line;
}

// One frame as interleaved 16-bit sRGB, width = film travel, height =
// CCD pixels, oriented for viewing (picture_row / picture_col).
Result<std::vector<std::uint16_t>> develop_frame(PakrawReader& raw, const ScanInfo& info,
                                                 const PositiveParams& params, const Frame& frame,
                                                 Feed feed);

} // namespace pakon::process
