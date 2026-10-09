#pragma once

// Row framing for the bulk image stream (endpoint 0x86).
//
// Evidence, transcribed not invented:
//
//  - pakon-reference/docs/image-stream.md: pixel data is 16-bit
//    little-endian samples out a bulk IN endpoint; the channel phase
//    floats with the stream's start offset; the scanner marks each
//    scan line by setting the least-significant bit of one fixed word
//    per line, and finding that word recovers the true row origin.
//
//  - The capture corpus's own bridge implements exactly this
//    (github.com/alibosworth/pakon-captures, bridge/pakonusb.py,
//    line_sync(), transcribed from psix's decode.py; the same test the
//    OEM's TLB.dll performs at 0x1001d2b0, `test byte ptr [ebp], 1`):
//
//        pos    = sample indices whose low byte has bit0 set
//        >= 8 positions required
//        period = most common spacing between consecutive positions
//                 (rejected if < 1000 samples: dark-noise cluster)
//        phase  = most common value of (position mod period)
//
//    Dark-region LSB noise sets bit0 at random positions too, so the
//    FIRST set bit is not a line start; the dominant statistics are.
//    The alignment window the bridge uses is ~131072 bytes (~10 lines
//    of 3-channel data or 8 of 4-channel). A window with no valid
//    statistics is dropped whole and scanning continues.
//
//  - After alignment the ring is contiguous: one row = period samples,
//    starting at the phase. The period is MEASURED PER WINDOW here and
//    must not be replaced by any fixed stride.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "pakon/errors/error.hpp"

namespace pakon::image {

// Line period statistics for one image window.
struct LineSync {
    std::size_t phase_samples;  // sample index of the first line start
    std::size_t period_samples; // samples per line (marker-to-marker)
};

// Minimum dominant spacing accepted as a line period; shorter results
// are dark-noise clusters (pakonusb.py line_sync()).
inline constexpr std::size_t kMinLinePeriodSamples = 1000;

// Minimum set-bit positions before the dominant statistics are trusted
// (pakonusb.py: fewer than 8, no sync).
inline constexpr std::size_t kMinMarkerPositions = 8;

// Bytes inspected to lock phase + period (pakonusb.py ALIGN_SCAN;
// ~10 lines of 3-channel or 8 lines of 4-channel data).
inline constexpr std::size_t kAlignScanBytes = 131072;

// Compute (phase, period) from a head buffer per the algorithm above.
// Failure kind: image_bad_marker (buffer too small, noise cluster, or
// no dominant statistics).
Result<LineSync> find_line_sync(std::span<const std::uint8_t> bytes);

// Streaming row framer: feed arbitrary byte chunks (any USB read
// size), get complete rows once sync is locked.
//
// Sync behaviour mirrors align_to_line(): sync is attempted on the
// first kAlignScanBytes buffered; on failure that window is dropped
// whole and the next one is tried; on success phase*2 bytes are
// dropped and every subsequent period*2 bytes are one row. Rows are
// returned in stream order as little-endian uint16 samples, marker bit
// intact (the marker is the row's first sample's bit0, an ordinary
// sample value — see format.hpp).
//
// Each RowFramer instance measures its own period; callers must not
// share one across image windows (window geometries may differ).
class RowFramer {
public:
    // Feed one chunk of stream bytes. Returns complete rows framed so
    // far (empty until sync locks and enough bytes accumulate).
    Result<std::vector<std::vector<std::uint16_t>>>
    feed(std::span<const std::uint8_t> bytes);

    // End of stream. Attempts a final sync on whatever is buffered if
    // none locked yet. Returns the remaining complete rows plus the
    // leftover byte count (a partial trailing row is reported, not
    // padded and not silently dropped — the caller's completion policy
    // decides whether a device-cut tail is acceptable).
    struct FlushResult {
        std::vector<std::vector<std::uint16_t>> rows;
        std::size_t leftover_bytes{0};
    };
    Result<FlushResult> flush();

    bool synced() const { return sync_.has_value(); }
    std::optional<LineSync> sync() const { return sync_; }
    // Rows emitted so far (for stats/policies).
    std::size_t rows_emitted() const { return rows_emitted_; }
    // Bytes held back waiting for a complete row (tail diagnostics).
    std::size_t buffered_bytes() const { return buffer_.size(); }

private:
    Result<std::vector<std::vector<std::uint16_t>>> drain_rows();
    // Try to lock sync on the first window_bytes buffered. Returns true
    // when locked (and the pre-phase bytes are dropped); false when the
    // window has no usable statistics — the caller drops it whole,
    // mirroring the bridge, which is not an error.
    bool try_sync_window(std::size_t window_bytes);

    std::vector<std::uint8_t> buffer_;
    std::optional<LineSync> sync_;
    std::size_t rows_emitted_{0};
};

// Decode little-endian 16-bit samples from a byte span (whole samples
// only; a trailing odd byte is an error).
Result<std::vector<std::uint16_t>> decode_samples(std::span<const std::uint8_t> bytes);

} // namespace pakon::image
