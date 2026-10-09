#pragma once

// Synthetic image-stream generation for offline tests.
//
// Rows follow the documented stream shape (image-stream.md + the
// capture corpus's bridge): little-endian 16-bit samples, one marker
// per line at the line's first sample (bit0), everything else even.
// Values are deterministic per (row, index) so tests can verify
// framing, channel phase and file output exactly.
//
// The source serves SEGMENTS in order: data segments (reads never
// cross a segment boundary — the capture corpus's window byte totals
// are exact multiples of the host's read size, i.e. windows ended on
// transfer boundaries) and idle segments (empty timed-out reads). Once
// all segments are consumed, the tail behaviour applies.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "pakon/errors/error.hpp"
#include "pakon/image/source.hpp"

namespace pakon::test {

// Sample value at (row, index): index 0 is the line marker (always
// odd); every other sample is even (no accidental markers).
inline std::uint16_t row_sample(std::size_t row, std::size_t index) {
    if (index == 0) {
        return static_cast<std::uint16_t>(0x2001 + row * 2);
    }
    return static_cast<std::uint16_t>((index * 2 + row * 7) & 0xFFFE);
}

inline std::vector<std::uint16_t> make_row(std::size_t period, std::size_t row) {
    std::vector<std::uint16_t> out(period);
    for (std::size_t i = 0; i < period; ++i) {
        out[i] = row_sample(row, i);
    }
    return out;
}

inline std::vector<std::vector<std::uint16_t>> make_rows(std::size_t period,
                                                         std::size_t count) {
    std::vector<std::vector<std::uint16_t>> rows;
    rows.reserve(count);
    for (std::size_t r = 0; r < count; ++r) {
        rows.push_back(make_row(period, r));
    }
    return rows;
}

inline std::vector<std::uint8_t>
encode_rows(const std::vector<std::vector<std::uint16_t>>& rows) {
    std::vector<std::uint8_t> out;
    for (const auto& row : rows) {
        for (const auto sample : row) {
            out.push_back(static_cast<std::uint8_t>(sample & 0xFF));
            out.push_back(static_cast<std::uint8_t>((sample >> 8) & 0xFF));
        }
    }
    return out;
}

// What the source does once every segment is consumed.
enum class ImageTail {
    eos,          // report device end-of-stream (empty read, end_of_stream)
    idle_forever, // stay alive but deliver nothing (quiescence tests)
};

class SyntheticImageSource final : public image::IImageSource {
public:
    explicit SyntheticImageSource(ImageTail tail = ImageTail::eos) : tail_(tail) {}

    // Convenience: a single data segment.
    static SyntheticImageSource once(std::vector<std::uint8_t> bytes,
                                     ImageTail tail = ImageTail::eos) {
        SyntheticImageSource source(tail);
        source.append_data(std::move(bytes));
        return source;
    }

    void append_data(std::vector<std::uint8_t> bytes) {
        segments_.push_back(Segment{std::move(bytes), 0});
    }

    void append_idle(std::size_t reads) { segments_.push_back(Segment{{}, reads}); }

    pakon::Result<image::ImageChunk> read(std::size_t max_bytes) override {
        namespace img = pakon::image;
        while (segment_ < segments_.size()) {
            auto& segment = segments_[segment_];
            if (!segment.bytes.empty()) {
                const std::size_t remaining = segment.bytes.size() - segment.pos;
                if (remaining == 0) {
                    ++segment_; // segment done; the next read starts the next
                    continue;   // segment without delivering anything
                }
                const std::size_t n = std::min(max_bytes, remaining);
                img::ImageChunk chunk;
                chunk.bytes.assign(
                    segment.bytes.begin() + static_cast<std::ptrdiff_t>(segment.pos),
                    segment.bytes.begin() + static_cast<std::ptrdiff_t>(segment.pos + n));
                segment.pos += n;
                return chunk;
            }
            if (segment.idle_reads > 0) {
                --segment.idle_reads;
                return img::ImageChunk{{}, true, false};
            }
            ++segment_;
        }
        return img::ImageChunk{{}, true, tail_ == ImageTail::eos};
    }

private:
    struct Segment {
        std::vector<std::uint8_t> bytes;
        std::size_t idle_reads{0};
        std::size_t pos{0};
    };

    std::vector<Segment> segments_;
    ImageTail tail_;
    std::size_t segment_{0};
};

} // namespace pakon::test
