#pragma once

// LineReader: whole scan lines out of the concurrent image stream.
//
// Line format (docs/OEM_RE.md §10, TLB@0x1001d170): 16-bit LE samples,
// the line's first sample has bit0 set (pixel 0's red), pixels are
// interleaved R,G,B, an IR block of N samples follows the 3N RGB samples.
// The stride is known from the FPGA geometry, so sync is a vote: the
// phase whose sample is odd in EVERY one of kSyncLines consecutive lines
// is the marker (LSB noise is odd half the time; a noise phase survives
// 24 lines with probability 2^-24). After sync every line's marker is
// checked; a missing marker drops sync and the vote runs again (counted).

#include <chrono>
#include <cstdint>
#include <span>
#include <vector>

#include "pakon/errors/error.hpp"
#include "pakon/scan/cancel.hpp"
#include "pakon/stream/image_stream.hpp"

namespace pakon::scan {

inline constexpr std::size_t kSyncLines = 24;

// Per-channel, per-pixel averages of N lines (planes R, G, B[, IR]).
struct LineAverage {
    std::size_t pixels{0};
    std::size_t lines{0};
    std::vector<std::vector<double>> planes; // [channel][pixel]
};

class LineReader {
public:
    LineReader(stream::IImageStream& stream, const CancelToken* cancel)
        : stream_(stream), cancel_(cancel) {}

    // New geometry: drops buffered bytes and sync.
    void set_stride(std::size_t samples_per_line, std::size_t pixels);
    // Drop stale data after a settings change (the OEM's bCalibrateFlush).
    void flush();
    // Next whole line; fails on a stream fault, on cancel, or when no
    // line arrives within `timeout`.
    Result<std::vector<std::uint16_t>> next_line(std::chrono::milliseconds timeout);
    // Average `lines` consecutive lines (the OEM averages 32).
    Result<LineAverage> average(std::size_t lines, std::chrono::milliseconds timeout);

    std::size_t samples_per_line() const { return stride_; }
    std::size_t pixels() const { return pixels_; }
    std::size_t resyncs() const { return resyncs_; }
    std::size_t lines_read() const { return lines_read_; }

private:
    VoidResult fill(std::size_t want_bytes, std::chrono::milliseconds timeout);
    bool try_sync();

    stream::IImageStream& stream_;
    const CancelToken* cancel_;
    std::size_t stride_{0};
    std::size_t pixels_{0};
    std::vector<std::uint8_t> buffer_;
    std::size_t pos_{0};
    bool synced_{false};
    std::size_t resyncs_{0};
    std::size_t lines_read_{0};
};

} // namespace pakon::scan
