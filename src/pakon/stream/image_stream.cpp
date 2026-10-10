#include "pakon/stream/image_stream.hpp"

#include <algorithm>
#include <cstring>
#include <format>

#include "pakon/logging/logger.hpp"

namespace pakon::stream {

bool ByteRing::push(std::span<const std::uint8_t> bytes) {
    if (bytes.size() > free_space()) {
        return false;
    }
    std::size_t tail = (head_ + size_) % buffer_.size();
    std::size_t first = std::min(bytes.size(), buffer_.size() - tail);
    std::memcpy(buffer_.data() + tail, bytes.data(), first);
    std::memcpy(buffer_.data(), bytes.data() + first, bytes.size() - first);
    size_ += bytes.size();
    return true;
}

std::size_t ByteRing::pop(std::span<std::uint8_t> dst) {
    const std::size_t n = std::min(dst.size(), size_);
    std::size_t first = std::min(n, buffer_.size() - head_);
    std::memcpy(dst.data(), buffer_.data() + head_, first);
    std::memcpy(dst.data() + first, buffer_.data(), n - first);
    head_ = (head_ + n) % buffer_.size();
    size_ -= n;
    if (size_ == 0) {
        head_ = 0;
    }
    return n;
}

ImageStream::ImageStream(std::unique_ptr<usb::IBulkInPipe> pipe, StreamConfig config)
    : pipe_(std::move(pipe)), config_(config), ring_(config.ring_bytes) {}

ImageStream::~ImageStream() { stop(); }

VoidResult ImageStream::start() {
    if (running_.load() || thread_.joinable()) {
        return void_failure(ErrorKind::scanner_unexpected_state,
                            "image stream already started");
    }
    if (!pipe_ || config_.queued_reads == 0 || config_.transfer_bytes == 0 ||
        config_.ring_bytes < config_.transfer_bytes) {
        return void_failure(ErrorKind::image_bad_geometry,
                            "image stream needs a pipe, >= 1 queued read and a ring "
                            "at least one transfer deep");
    }
    slots_.assign(config_.queued_reads, std::vector<std::uint8_t>(config_.transfer_bytes));
    {
        std::scoped_lock lock(mutex_);
        fault_.clear();
    }
    stop_requested_ = false;
    running_ = true;
    thread_ = std::thread([this] { reader_loop(); });
    return {};
}

void ImageStream::stop() {
    if (!thread_.joinable()) {
        return;
    }
    stop_requested_ = true;
    // abort() is safe from another thread; the reader also aborts on its
    // own when it sees the flag. Either way every queued slot completes
    // and is collected before the buffers can go away.
    pipe_->abort();
    thread_.join();
    data_ready_.notify_all();
}

void ImageStream::fault(std::string message) {
    if (fault_.empty()) {
        fault_ = std::move(message);
        log::Logger::instance().log(log::Level::error, "image stream fault: {}", fault_);
    }
}

void ImageStream::reader_loop() {
    std::size_t outstanding = 0;
    bool aborted = false;
    auto abort_all = [&] {
        if (!aborted) {
            aborted = true;
            pipe_->abort();
        }
    };

    for (std::size_t slot = 0; slot < slots_.size(); ++slot) {
        if (auto s = pipe_->submit(slot, slots_[slot]); !s) {
            std::scoped_lock lock(mutex_);
            fault(std::format("submit of read {} failed: {}", slot, s.error().message));
            abort_all();
            break;
        }
        ++outstanding;
    }

    // After an abort, give the backend a bounded time to retire every
    // queued read (WinUSB completes them with ERROR_OPERATION_ABORTED).
    constexpr auto kNever = std::chrono::steady_clock::time_point::max();
    auto abort_deadline = kNever;
    while (outstanding > 0) {
        if (stop_requested_.load()) {
            abort_all();
        }
        if (aborted && abort_deadline == kNever) {
            abort_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        }
        auto waited = pipe_->wait(std::chrono::milliseconds(50));
        if (!waited) {
            {
                std::scoped_lock lock(mutex_);
                fault(std::format("pipe wait failed: {}", waited.error().message));
            }
            abort_all();
            if (std::chrono::steady_clock::now() > abort_deadline) {
                break;
            }
            continue;
        }
        if (!*waited) { // timeout
            if (std::chrono::steady_clock::now() > abort_deadline) {
                std::scoped_lock lock(mutex_);
                fault(std::format("{} queued reads did not retire after abort",
                                  outstanding));
                break;
            }
            continue;
        }
        const usb::PipeCompletion c = std::move(**waited);
        --outstanding;
        if (c.aborted) {
            continue;
        }
        bool resubmit = !aborted;
        {
            std::scoped_lock lock(mutex_);
            if (c.failed) {
                ++stats_.transfer_errors;
                fault(std::format("read on slot {} failed: {}", c.slot, c.error));
                resubmit = false;
            } else if (c.bytes == 0) {
                ++stats_.zero_length;
            } else {
                const std::size_t n = std::min(c.bytes, slots_[c.slot].size());
                ++stats_.transfers;
                stats_.bytes_in += n;
                if (!ring_.push(std::span<const std::uint8_t>(slots_[c.slot]).first(n))) {
                    ++stats_.overflow_events;
                    stats_.overflow_bytes += n;
                    fault(std::format(
                        "ring overflow: {} bytes did not fit ({} of {} buffered) - "
                        "the consumer fell behind the device",
                        n, ring_.size(), ring_.capacity()));
                    resubmit = false;
                } else {
                    stats_.high_water = std::max(stats_.high_water, ring_.size());
                }
            }
        }
        data_ready_.notify_all();
        if (!resubmit) {
            abort_all();
            continue;
        }
        if (auto s = pipe_->submit(c.slot, slots_[c.slot]); !s) {
            {
                std::scoped_lock lock(mutex_);
                fault(std::format("resubmit of read {} failed: {}", c.slot,
                                  s.error().message));
            }
            abort_all();
            continue;
        }
        ++outstanding;
    }
    {
        // Publish "stopped" under the lock so a reader that just checked
        // the predicate cannot miss the wake-up.
        std::scoped_lock lock(mutex_);
        running_ = false;
    }
    data_ready_.notify_all();
}

Result<std::size_t> ImageStream::read(std::span<std::uint8_t> dst,
                                      std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    data_ready_.wait_for(lock, timeout, [&] {
        return ring_.size() > 0 || !fault_.empty() || !running_.load();
    });
    if (!fault_.empty()) {
        return failure<std::size_t>(ErrorKind::usb_io_failed,
                                    std::format("image stream: {}", fault_));
    }
    const std::size_t n = ring_.pop(dst);
    stats_.bytes_out += n;
    return n;
}

void ImageStream::discard() {
    std::scoped_lock lock(mutex_);
    stats_.bytes_discarded += ring_.size();
    ring_.clear();
}

StreamStats ImageStream::stats() const {
    std::scoped_lock lock(mutex_);
    return stats_;
}

} // namespace pakon::stream
