// Offline tests for line-sync statistics and row framing: marker
// detection with dark noise, chunk-boundary invariance, per-window
// period measurement, and the malformed-stream failure modes.
//
// The algorithm under test is the one the capture corpus's own bridge
// uses (pakonusb.py line_sync()/align_to_line(), transcribed from
// psix's decode.py and matching TLB.dll's marker test).

#include <cstdint>
#include <vector>

#include "pakon/image/row_sync.hpp"
#include "support/synthetic_image.hpp"
#include "support/test_harness.hpp"

namespace {

using pakon::image::find_line_sync;
using pakon::image::LineSync;
using pakon::image::RowFramer;
using namespace pakon::test;

// Append `count` even junk samples (no markers) as a phase prefix.
void prepend_junk(std::vector<std::uint8_t>& bytes, std::size_t count) {
    std::vector<std::uint8_t> prefix;
    prefix.reserve(count * 2);
    for (std::size_t i = 0; i < count; ++i) {
        prefix.push_back(0x40);
        prefix.push_back(0x10);
    }
    bytes.insert(bytes.begin(), prefix.begin(), prefix.end());
}

std::vector<std::uint8_t> stream_of(std::size_t period, std::size_t rows) {
    return encode_rows(make_rows(period, rows));
}

} // namespace

PAKON_TEST(line_sync_finds_period_and_phase) {
    const auto bytes = stream_of(6108, 24);
    auto sync = find_line_sync(bytes);
    EXPECT(sync.has_value());
    if (sync) {
        EXPECT_EQ(sync->period_samples, 6108u);
        EXPECT_EQ(sync->phase_samples, 0u);
    }
}

PAKON_TEST(line_sync_measures_phase_offset_from_stream_start) {
    // A stream that starts 5 samples before a line boundary: phase 5.
    auto bytes = stream_of(3081, 24);
    prepend_junk(bytes, 5);
    auto sync = find_line_sync(bytes);
    EXPECT(sync.has_value());
    if (sync) {
        EXPECT_EQ(sync->period_samples, 3081u);
        EXPECT_EQ(sync->phase_samples, 5u);
    }
}

PAKON_TEST(line_sync_survives_sparse_dark_noise) {
    // Dark-region LSB noise sets bit0 at random positions; a few
    // samples must not defeat the dominant statistics.
    auto bytes = stream_of(6108, 32);
    // Deterministic noise: 4 odd samples inside otherwise-even rows
    // (sample indices inside the 195456-sample stream, off any row
    // start so they never collide with a marker).
    const std::size_t noise_at[] = {5000, 40000, 90000, 150000};
    for (const auto sample_index : noise_at) {
        bytes[sample_index * 2] |= 0x01; // force bit0 of one sample
    }
    auto sync = find_line_sync(bytes);
    EXPECT(sync.has_value());
    if (sync) {
        EXPECT_EQ(sync->period_samples, 6108u);
        EXPECT_EQ(sync->phase_samples, 0u);
    }
}

PAKON_TEST(line_sync_rejects_noise_clusters) {
    // Odd bits far denser than one per line: the dominant spacing is a
    // noise cluster below the 1000-sample floor, not a line period.
    std::vector<std::uint8_t> bytes(40000, 0x00);
    for (std::size_t i = 0; i < bytes.size(); i += 14) {
        bytes[i] = 0x01; // an odd low byte every 7 samples
    }
    auto sync = find_line_sync(bytes);
    EXPECT(!sync.has_value());
    if (!sync) {
        EXPECT_EQ(sync.error().kind, pakon::ErrorKind::image_bad_marker);
    }
}

PAKON_TEST(line_sync_requires_enough_marker_positions) {
    std::vector<std::uint8_t> bytes(40000, 0x00);
    for (std::size_t i = 0; i < 5; ++i) {
        bytes[i * 20000] = 0x01; // only 5 odd positions
    }
    auto sync = find_line_sync(bytes);
    EXPECT(!sync.has_value());
    if (!sync) {
        EXPECT_EQ(sync.error().kind, pakon::ErrorKind::image_bad_marker);
    }
}

PAKON_TEST(row_framer_is_chunk_boundary_invariant) {
    // The same stream fed in wildly different chunk sizes must frame
    // identical rows — this is the property the marker bit exists for
    // (channel phase independent of stream start).
    const std::size_t period = 6108;
    const std::size_t rows = 24;
    const auto bytes = stream_of(period, rows);
    const auto expected = make_rows(period, rows);

    const std::size_t chunk_sizes[] = {20480, 4096, 1000, 2};
    for (const auto chunk_size : chunk_sizes) {
        RowFramer framer;
        std::vector<std::vector<std::uint16_t>> got;
        for (std::size_t pos = 0; pos < bytes.size(); pos += chunk_size) {
            const std::size_t n = std::min(chunk_size, bytes.size() - pos);
            auto feed = framer.feed(std::span<const std::uint8_t>(bytes).subspan(pos, n));
            EXPECT(feed.has_value());
            if (!feed) {
                return;
            }
            got.insert(got.end(), feed->begin(), feed->end());
        }
        auto tail = framer.flush();
        EXPECT(tail.has_value());
        if (!tail) {
            return;
        }
        got.insert(got.end(), tail->rows.begin(), tail->rows.end());
        EXPECT_EQ(tail->leftover_bytes, 0u);

        EXPECT(framer.sync().has_value());
        EXPECT_EQ(got.size(), rows);
        if (got.size() == rows) {
            for (std::size_t r = 0; r < rows; ++r) {
                EXPECT(got[r] == expected[r]);
            }
        }
    }
}

PAKON_TEST(row_framer_drops_an_unframeable_leading_window) {
    // First 131072+ bytes have no usable statistics (all even); the
    // framer drops that window whole and locks onto the next. The
    // remainder (4000 B) is under one line period, so the first marker
    // lies in the alignment window's first period and framing starts
    // exactly on it.
    std::vector<std::uint8_t> bytes(135072, 0x10);
    const auto good = stream_of(3081, 32);
    bytes.insert(bytes.end(), good.begin(), good.end());

    RowFramer framer;
    auto feed = framer.feed(bytes);
    EXPECT(feed.has_value());
    auto tail = framer.flush();
    EXPECT(tail.has_value());
    if (!feed || !tail) {
        return;
    }
    std::vector<std::vector<std::uint16_t>> got = *feed;
    got.insert(got.end(), tail->rows.begin(), tail->rows.end());
    EXPECT(framer.sync().has_value());
    EXPECT_EQ(got.size(), 32u);
    if (got.size() == 32u) {
        EXPECT(got[0] == make_row(3081, 0));
    }
}

PAKON_TEST(row_framer_reports_a_truncated_tail) {
    const std::size_t period = 2048;
    auto bytes = stream_of(period, 12);
    bytes.resize(period * 2 * 12 + 100); // + half a row's worth of bytes

    RowFramer framer;
    auto feed = framer.feed(bytes);
    EXPECT(feed.has_value());
    auto tail = framer.flush();
    EXPECT(tail.has_value());
    if (tail) {
        EXPECT_EQ(tail->rows.size(), 12u);
        EXPECT_EQ(tail->leftover_bytes, 100u);
        EXPECT_EQ(framer.buffered_bytes(), 0u); // buffer cleared on flush
    }
}

PAKON_TEST(row_framer_rejects_an_unframeable_stream) {
    // Too short to fill an alignment window and with no statistics in
    // what is there.
    std::vector<std::uint8_t> bytes(50000, 0x00);
    RowFramer framer;
    auto feed = framer.feed(bytes);
    EXPECT(feed.has_value()); // feeding is fine; the end is not
    auto tail = framer.flush();
    EXPECT(!tail.has_value());
    if (!tail) {
        EXPECT_EQ(tail.error().kind, pakon::ErrorKind::image_bad_marker);
    }
}

PAKON_TEST(decode_samples_rejects_odd_byte_counts) {
    std::vector<std::uint8_t> bytes(3, 0x00);
    auto samples = pakon::image::decode_samples(bytes);
    EXPECT(!samples.has_value());
    if (!samples) {
        EXPECT_EQ(samples.error().kind, pakon::ErrorKind::image_truncated);
    }
}

int main() { return pakon::test::run_all(); }
