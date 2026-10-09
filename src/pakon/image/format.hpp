#pragma once

// Geometry types for reconstructed scan rows.
//
// Everything here is derived from the stream itself (see row_sync.hpp)
// or supplied explicitly by the caller. Nothing is filled in from the
// disputed fixed stride lists (3081/4617/6162 vs 3000/4500/6000): the
// row period is measured per image window from the line-sync markers.

#include <cstdint>
#include <optional>
#include <utility>

namespace pakon::image {

// Geometry of one reconstructed scan row (one CCD readout) as framed by
// the line markers.
struct RowGeometry {
    // Samples per row = the measured marker-to-marker period. This
    // includes whatever the row carries beyond active pixels (porch,
    // the marker word itself, an IR region): the evidence does not let
    // us split those, so the raw frame keeps them all.
    std::uint32_t samples_per_row{0};

    // True when the caller declared an IR region for this window.
    bool has_ir_lane() const { return ir_region.has_value(); }

    // Region of the row (sample offset, length) holding IR samples.
    // pakon-reference says IR "comes from a different region of the
    // line, not interleaved" but does NOT give the region's offset, and
    // the capture corpus omits pixels, so the offset is unresolved
    // offline. Callers that know it (from live measurement) set it;
    // otherwise the lane stays inside the opaque row tail.
    std::optional<std::pair<std::uint32_t, std::uint32_t>> ir_region;
};

// RGB triplet framing of a row: samples cycle R,G,B starting at
// pixel_phase. The documented R,G,B identity is the marker-aligned
// order (image-stream.md § "Marker bit for row origin"), so for a row
// framed by RowFramer the default phase is 0.
struct RgbWindow {
    std::uint32_t pixel_phase{0};
    std::uint32_t triplet_count{0};
};

// Compute the triplet count for a row under this window; samples that
// do not complete a triplet (or that fall before the phase) are left
// out.
inline RgbWindow rgb_window_for(std::uint32_t samples_per_row,
                                std::uint32_t pixel_phase) {
    if (pixel_phase >= samples_per_row) {
        return {pixel_phase, 0};
    }
    return {pixel_phase, (samples_per_row - pixel_phase) / 3};
}

} // namespace pakon::image
