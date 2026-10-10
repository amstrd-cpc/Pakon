#pragma once

// Concurrent image stream for bulk IN 0x86.
//
// The device streams on its own clock while the CCD FPGA acquire bit is
// set and stops the instant it is cleared (docs/OEM_RE.md §3, all eleven
// captured sessions). Nothing about the stream waits for the host: data
// the host has not read is lost in the FX2's few-packet FIFO. The OEM
// solves this in its kernel driver (reads queued into an 8 MiB ring,
// OEM_RE.md §6); this is the userspace equivalent:
//
//   reader thread ── keeps queued_reads transfers in flight on the pipe,
//                    pushes every completed transfer into a locked ring,
//                    resubmits the slot immediately;
//   consumer      ── read()/discard() at its own pace (the scan runner,
//                    which also sends commands meanwhile).
//
// A full ring is an ERROR (stream fault), never a silent drop: the
// overflow counter, the dropped byte count and the fault message all
// surface through stats() and the next read(). Stopping aborts the
// queued transfers and joins the thread; buffered bytes stay readable
// (a window is drained after the acquire bit is cleared).
//
// No image byte is ever logged, at any level.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "pakon/errors/error.hpp"
#include "pakon/usb/bulk_pipe.hpp"

namespace pakon::stream {

// 12 transfers in flight (the requested 8–16 band). At the measured
// ~6.6 MB/s one 20 KiB transfer fills in ~3 ms, so 12 give the reader
// thread ~37 ms of slack before the device FIFO could overflow — the
// OEM kernel driver resubmits at DPC level with far less in flight.
inline constexpr std::size_t kQueuedReads = 12;
// The OEM ring's packet size (OEM_RE.md §6, BRIDGE pkusb.c:270).
inline constexpr std::size_t kTransferBytes = 20480;
// Consumer slack: 32 MiB ≈ 5 s of Base-4 stream (the OEM ring is 8 MiB
// plus a separate processed-lines ring).
inline constexpr std::size_t kRingBytes = 32u << 20;

struct StreamConfig {
    std::size_t queued_reads{kQueuedReads};
    std::size_t transfer_bytes{kTransferBytes};
    std::size_t ring_bytes{kRingBytes};
};

struct StreamStats {
    std::uint64_t bytes_in{0};        // delivered by the pipe
    std::uint64_t bytes_out{0};       // handed to the consumer
    std::uint64_t bytes_discarded{0}; // dropped on purpose by discard()
    std::uint64_t transfers{0};       // completed transfers with data
    std::uint64_t zero_length{0};     // completed transfers without data
    std::size_t high_water{0};        // most bytes ever buffered
    std::uint64_t overflow_events{0}; // pushes that did not fit (fault)
    std::uint64_t overflow_bytes{0};  // bytes lost to those
    std::uint64_t transfer_errors{0};
};

// The consumer-side contract (the scan runner depends on this only).
class IImageStream {
public:
    virtual ~IImageStream() = default;
    virtual VoidResult start() = 0;
    // Stop reading from the device (abort + join). Buffered bytes remain
    // readable. Idempotent.
    virtual void stop() = 0;
    // Copy up to dst.size() buffered bytes; waits up to `timeout` for the
    // first byte. Returns 0 on timeout. Error: the stream has faulted
    // (ring overflow, transfer failure) — sticky until the next start().
    virtual Result<std::size_t> read(std::span<std::uint8_t> dst,
                                     std::chrono::milliseconds timeout) = 0;
    // Drop everything buffered (stale lines after a settings change).
    virtual void discard() = 0;
    virtual StreamStats stats() const = 0;
    virtual bool running() const = 0;
};

// Fixed-capacity byte FIFO. Not thread-safe by itself; ImageStream guards
// it with its mutex.
class ByteRing {
public:
    explicit ByteRing(std::size_t capacity) : buffer_(capacity) {}
    std::size_t capacity() const { return buffer_.size(); }
    std::size_t size() const { return size_; }
    std::size_t free_space() const { return buffer_.size() - size_; }
    // All-or-nothing: false (and nothing stored) when it does not fit.
    bool push(std::span<const std::uint8_t> bytes);
    std::size_t pop(std::span<std::uint8_t> dst);
    void clear() { head_ = size_ = 0; }

private:
    std::vector<std::uint8_t> buffer_;
    std::size_t head_{0}; // read position
    std::size_t size_{0};
};

class ImageStream final : public IImageStream {
public:
    ImageStream(std::unique_ptr<usb::IBulkInPipe> pipe, StreamConfig config = {});
    ~ImageStream() override;
    ImageStream(const ImageStream&) = delete;
    ImageStream& operator=(const ImageStream&) = delete;

    VoidResult start() override;
    void stop() override;
    Result<std::size_t> read(std::span<std::uint8_t> dst,
                             std::chrono::milliseconds timeout) override;
    void discard() override;
    StreamStats stats() const override;
    bool running() const override { return running_.load(); }

private:
    void reader_loop();
    void fault(std::string message); // caller holds mutex_

    std::unique_ptr<usb::IBulkInPipe> pipe_;
    StreamConfig config_;
    std::vector<std::vector<std::uint8_t>> slots_;

    mutable std::mutex mutex_;
    std::condition_variable data_ready_;
    ByteRing ring_;
    StreamStats stats_;
    std::string fault_; // non-empty = faulted (sticky until start())

    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace pakon::stream
