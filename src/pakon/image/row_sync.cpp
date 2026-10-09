#include "pakon/image/row_sync.hpp"

#include <format>
#include <map>

namespace pakon::image {

namespace {

// Deterministic mode over counted votes: the value with the highest
// count, ties broken toward the smallest value. The Python source this
// was transcribed from (collections.Counter.most_common) breaks ties by
// insertion order; either rule is arbitrary, ours is reproducible, and
// the test streams are built so that no vote ties.
template <typename K>
K dominant(const std::map<K, std::size_t>& counts) {
    K best{};
    std::size_t best_count = 0;
    for (const auto& [value, count] : counts) {
        if (count > best_count) {
            best = value;
            best_count = count;
        }
    }
    return best;
}

} // namespace

Result<LineSync> find_line_sync(std::span<const std::uint8_t> bytes) {
    // Sample positions whose little-endian low byte has bit0 set (the
    // line marker, or dark-region LSB noise).
    std::vector<std::size_t> pos;
    const std::size_t samples = bytes.size() / 2;
    for (std::size_t i = 0; i < samples; ++i) {
        if (bytes[2 * i] & 1) {
            pos.push_back(i);
        }
    }
    if (pos.size() < kMinMarkerPositions) {
        return failure<LineSync>(
            ErrorKind::image_bad_marker,
            std::format("only {} set-bit positions in {} bytes; need at least {}",
                        pos.size(), bytes.size(), kMinMarkerPositions));
    }

    // Dominant spacing between consecutive positions = line period.
    std::map<std::size_t, std::size_t> gap_counts;
    for (std::size_t i = 1; i < pos.size(); ++i) {
        ++gap_counts[pos[i] - pos[i - 1]];
    }
    const std::size_t period = dominant(gap_counts);
    if (period < kMinLinePeriodSamples) {
        return failure<LineSync>(
            ErrorKind::image_bad_marker,
            std::format("dominant set-bit spacing {} samples is below the {}-sample "
                        "minimum (dark-noise cluster, not a line period)",
                        period, kMinLinePeriodSamples));
    }

    // Dominant (position mod period) = row phase.
    std::map<std::size_t, std::size_t> phase_counts;
    for (const auto p : pos) {
        ++phase_counts[p % period];
    }
    return LineSync{dominant(phase_counts), period};
}

Result<std::vector<std::uint16_t>> decode_samples(std::span<const std::uint8_t> bytes) {
    if (bytes.size() % 2 != 0) {
        return failure<std::vector<std::uint16_t>>(
            ErrorKind::image_truncated,
            std::format("odd byte count {} cannot be decoded as 16-bit samples",
                        bytes.size()));
    }
    std::vector<std::uint16_t> out(bytes.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::uint16_t>(bytes[2 * i]) |
                 (static_cast<std::uint16_t>(bytes[2 * i + 1]) << 8);
    }
    return out;
}

bool RowFramer::try_sync_window(std::size_t window_bytes) {
    auto sync = find_line_sync(std::span<const std::uint8_t>(buffer_).first(window_bytes));
    if (!sync) {
        return false;
    }
    // Drop everything before the first line start; the ring is
    // contiguous from here (pakonusb.py align_to_line()).
    buffer_.erase(buffer_.begin(),
                  buffer_.begin() + static_cast<std::ptrdiff_t>(sync->phase_samples * 2));
    sync_ = *sync;
    return true;
}

Result<std::vector<std::vector<std::uint16_t>>> RowFramer::drain_rows() {
    std::vector<std::vector<std::uint16_t>> rows;
    if (!sync_) {
        return rows;
    }
    const std::size_t row_bytes = sync_->period_samples * 2;
    while (buffer_.size() >= row_bytes) {
        auto samples = decode_samples(std::span<const std::uint8_t>(buffer_).first(row_bytes));
        if (!samples) {
            return samples.error();
        }
        rows.push_back(std::move(*samples));
        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + static_cast<std::ptrdiff_t>(row_bytes));
        ++rows_emitted_;
    }
    return rows;
}

Result<std::vector<std::vector<std::uint16_t>>>
RowFramer::feed(std::span<const std::uint8_t> bytes) {
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());

    if (!sync_) {
        // Mirror align_to_line(): try to lock on the first full window;
        // on failure drop that window whole and continue.
        while (buffer_.size() >= kAlignScanBytes) {
            if (try_sync_window(kAlignScanBytes)) {
                break;
            }
            buffer_.erase(buffer_.begin(),
                          buffer_.begin() + static_cast<std::ptrdiff_t>(kAlignScanBytes));
        }
    }
    return drain_rows();
}

Result<RowFramer::FlushResult> RowFramer::flush() {
    if (!sync_ && !buffer_.empty()) {
        // Short stream: one last sync attempt over everything
        // buffered before declaring the stream unframeable.
        if (!try_sync_window(buffer_.size())) {
            return failure<RowFramer::FlushResult>(
                ErrorKind::image_bad_marker,
                "no line-sync statistics in the whole stream");
        }
    }
    auto rows = drain_rows();
    if (!rows) {
        return rows.error();
    }
    FlushResult result{std::move(*rows), buffer_.size()};
    buffer_.clear();
    return result;
}

} // namespace pakon::image
