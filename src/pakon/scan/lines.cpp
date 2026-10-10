#include "pakon/scan/lines.hpp"

#include <algorithm>
#include <format>

namespace pakon::scan {

void LineReader::set_stride(std::size_t samples_per_line, std::size_t pixels) {
    stride_ = samples_per_line;
    pixels_ = pixels;
    flush();
}

void LineReader::flush() {
    stream_.discard();
    buffer_.clear();
    pos_ = 0;
    synced_ = false;
}

VoidResult LineReader::fill(std::size_t want_bytes, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::vector<std::uint8_t> chunk(256 * 1024);
    while (buffer_.size() - pos_ < want_bytes) {
        if (cancel_ && cancel_->requested()) {
            return void_failure(ErrorKind::cancelled, "interrupt observed while reading lines");
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return void_failure(
                ErrorKind::scanner_timeout,
                std::format("no image data: {} of {} bytes after {} ms (is the acquire bit set?)",
                            buffer_.size() - pos_, want_bytes, timeout.count()));
        }
        const auto wait = std::min<std::chrono::milliseconds>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now),
            std::chrono::milliseconds(50));
        auto n = stream_.read(chunk, wait);
        if (!n) {
            return n.error();
        }
        if (pos_ > (1u << 20)) { // compact
            buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(pos_));
            pos_ = 0;
        }
        buffer_.insert(buffer_.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(*n));
    }
    return {};
}

bool LineReader::try_sync() {
    // Vote over kSyncLines lines: count, per phase, the lines whose sample
    // at that phase is odd. Only a phase odd in every line can be the
    // marker; a unique one locks.
    const std::size_t line_bytes = stride_ * 2;
    std::vector<std::uint16_t> votes(stride_, 0);
    const std::uint8_t* base = buffer_.data() + pos_;
    for (std::size_t k = 0; k < kSyncLines; ++k) {
        const std::uint8_t* line = base + k * line_bytes;
        for (std::size_t s = 0; s < stride_; ++s) {
            votes[s] = static_cast<std::uint16_t>(votes[s] + (line[2 * s] & 1));
        }
    }
    std::size_t found = stride_;
    for (std::size_t s = 0; s < stride_; ++s) {
        if (votes[s] == kSyncLines) {
            if (found != stride_) {
                return false; // ambiguous: wait for more lines
            }
            found = s;
        }
    }
    if (found == stride_) {
        return false;
    }
    pos_ += found * 2;
    synced_ = true;
    return true;
}

Result<std::vector<std::uint16_t>> LineReader::next_line(std::chrono::milliseconds timeout) {
    if (stride_ == 0) {
        return failure<std::vector<std::uint16_t>>(ErrorKind::image_bad_geometry,
                                                   "line reader has no geometry");
    }
    const std::size_t line_bytes = stride_ * 2;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!synced_) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (auto r = fill((kSyncLines + 1) * line_bytes,
                          std::max(left, std::chrono::milliseconds(1)));
            !r) {
            return r.error();
        }
        if (!try_sync()) {
            pos_ += line_bytes; // slide one line and vote again
        }
    }
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (auto r = fill(line_bytes, std::max(left, std::chrono::milliseconds(1))); !r) {
        return r.error();
    }
    const std::uint8_t* p = buffer_.data() + pos_;
    if ((p[0] & 1) == 0) {
        // Lost the marker (dropped bytes upstream): vote again.
        synced_ = false;
        ++resyncs_;
        return next_line(std::max(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      deadline - std::chrono::steady_clock::now()),
                                  std::chrono::milliseconds(1)));
    }
    std::vector<std::uint16_t> line(stride_);
    for (std::size_t s = 0; s < stride_; ++s) {
        line[s] = static_cast<std::uint16_t>(p[2 * s] | (p[2 * s + 1] << 8));
    }
    pos_ += line_bytes;
    ++lines_read_;
    return line;
}

Result<LineAverage> LineReader::average(std::size_t lines, std::chrono::milliseconds timeout) {
    LineAverage avg;
    avg.pixels = pixels_;
    const std::size_t channels = pixels_ == 0 ? 0 : stride_ / pixels_;
    avg.planes.assign(channels, std::vector<double>(pixels_, 0.0));
    for (std::size_t n = 0; n < lines; ++n) {
        auto line = next_line(timeout);
        if (!line) {
            return line.error();
        }
        for (std::size_t p = 0; p < pixels_; ++p) {
            for (std::size_t c = 0; c < 3 && c < channels; ++c) {
                avg.planes[c][p] += (*line)[3 * p + c];
            }
            if (channels == 4) {
                avg.planes[3][p] += (*line)[3 * pixels_ + p];
            }
        }
        ++avg.lines;
    }
    for (auto& plane : avg.planes) {
        for (auto& v : plane) {
            v /= static_cast<double>(std::max<std::size_t>(lines, 1));
        }
    }
    return avg;
}

} // namespace pakon::scan
