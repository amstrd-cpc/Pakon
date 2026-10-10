// ImageStream against the simulator's EP6: no byte lost while commands
// run concurrently, ring overflow is a fault (never silent), stop/abort
// retires every queued read, restart works. No real USB.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "pakon/ppb/packet.hpp"
#include "pakon/stream/image_stream.hpp"
#include "support/sim_device.hpp"
#include "support/test_harness.hpp"

namespace {

using namespace std::chrono_literals;
using pakon::sim::SimConfig;
using pakon::sim::SimDevice;
using pakon::stream::ByteRing;
using pakon::stream::ImageStream;
using pakon::stream::StreamConfig;

void send(SimDevice& sim, const pakon::ppb::Frame& frame) {
    auto bytes = frame.serialize();
    EXPECT(bytes.has_value());
    if (bytes) {
        auto r = sim.command_exchange(*bytes);
        EXPECT(r.has_value());
    }
}

// FPGA geometry for a Base-4 calibration-sized line: 3..1030 = 1027 px.
void setup_geometry(SimDevice& sim) {
    send(sim, pakon::ppb::make_write(0x44, 0x82, std::array<std::uint8_t, 3>{4, 3, 0}));
    send(sim, pakon::ppb::make_write(0x44, 0x82, std::array<std::uint8_t, 3>{5, 0x06, 0x04}));
    send(sim, pakon::ppb::make_write(0x44, 0x82, std::array<std::uint8_t, 3>{6, 0x53, 0x07}));
}

void acquire(SimDevice& sim, bool on) {
    send(sim, pakon::ppb::make_write(
                  0x44, 0x82,
                  std::array<std::uint8_t, 3>{0, static_cast<std::uint8_t>(on ? 0x63 : 0x62), 0}));
}

std::unique_ptr<ImageStream> make_stream(SimDevice& sim, StreamConfig cfg = {}) {
    auto pipe = sim.open_bulk_in(0x86, cfg.queued_reads);
    EXPECT(pipe.has_value());
    return std::make_unique<ImageStream>(std::move(*pipe), cfg);
}

std::vector<std::uint8_t> drain(ImageStream& stream, std::chrono::milliseconds quiet) {
    std::vector<std::uint8_t> out;
    std::vector<std::uint8_t> buf(65536);
    for (;;) {
        auto n = stream.read(buf, quiet);
        EXPECT(n.has_value());
        if (!n || *n == 0) {
            break;
        }
        out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n));
    }
    return out;
}

} // namespace

PAKON_TEST(ring_wraps_and_refuses_what_does_not_fit) {
    ByteRing ring(8);
    const std::uint8_t a[6] = {1, 2, 3, 4, 5, 6};
    EXPECT(ring.push(a));
    std::uint8_t out[4];
    EXPECT_EQ(ring.pop(out), 4u);
    EXPECT(ring.push(a));  // wraps
    EXPECT(!ring.push(a)); // 2 + 6 + 6 > 8: refused whole
    EXPECT_EQ(ring.size(), 8u);
    std::uint8_t all[8];
    EXPECT_EQ(ring.pop(all), 8u);
    EXPECT_EQ(all[0], 5);
    EXPECT_EQ(all[2], 1);
    EXPECT_EQ(all[7], 6);
}

PAKON_TEST(no_byte_lost_while_commands_run_concurrently) {
    SimConfig cfg;
    cfg.command_latency = 2500us;
    SimDevice sim(cfg);
    setup_geometry(sim);
    auto stream = make_stream(sim);
    EXPECT(stream->start().has_value());
    acquire(sim, true);

    // A second thread keeps the command channel busy the whole time,
    // like the runner's corrections loop.
    std::atomic<bool> stop{false};
    std::thread chatter([&] {
        while (!stop) {
            send(sim, pakon::ppb::make_read_status(0x10));
        }
    });
    std::vector<std::uint8_t> got;
    std::vector<std::uint8_t> buf(65536);
    const auto until = std::chrono::steady_clock::now() + 400ms;
    while (std::chrono::steady_clock::now() < until) {
        auto n = stream->read(buf, 20ms);
        EXPECT(n.has_value());
        if (n) {
            got.insert(got.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n));
        }
    }
    stop = true;
    chatter.join();
    acquire(sim, false); // the host ends the window (OEM_RE.md §3)
    auto tail = drain(*stream, 100ms);
    got.insert(got.end(), tail.begin(), tail.end());
    stream->stop();

    EXPECT_EQ(sim.lost_bytes(), 0u);
    EXPECT(got.size() > 100000);
    EXPECT_EQ(static_cast<std::uint64_t>(got.size()), sim.streamed_bytes());
    const auto stats = stream->stats();
    EXPECT_EQ(stats.overflow_events, 0u);
    EXPECT_EQ(stats.bytes_in, sim.streamed_bytes());
    // Every line starts with its marker: 1027 px × 3 samples × 2 bytes.
    const std::size_t line = 1027 * 3 * 2;
    const std::size_t lines = got.size() / line;
    bool all_marked = true;
    for (std::size_t i = 0; i < lines; ++i) {
        all_marked = all_marked && (got[i * line] & 1) == 1;
    }
    EXPECT(all_marked);
}

PAKON_TEST(slow_consumer_overflow_is_a_fault_not_a_silent_drop) {
    SimDevice sim;
    setup_geometry(sim);
    StreamConfig small;
    small.ring_bytes = 256 * 1024;
    auto stream = make_stream(sim, small);
    EXPECT(stream->start().has_value());
    acquire(sim, true);
    std::this_thread::sleep_for(200ms); // never reads: ~1.3 MB arrive
    std::vector<std::uint8_t> buf(1024);
    auto r = stream->read(buf, 10ms);
    EXPECT(!r.has_value());
    acquire(sim, false);
    stream->stop();
    const auto stats = stream->stats();
    EXPECT(stats.overflow_events >= 1);
    EXPECT(stats.overflow_bytes > 0);
    EXPECT(stats.high_water <= small.ring_bytes);
}

PAKON_TEST(stop_retires_queued_reads_and_restart_works) {
    SimDevice sim;
    setup_geometry(sim);
    auto stream = make_stream(sim);
    for (int round = 0; round < 3; ++round) {
        EXPECT(stream->start().has_value());
        acquire(sim, true);
        std::this_thread::sleep_for(30ms);
        acquire(sim, false);
        stream->stop();
        EXPECT(!stream->running());
        auto bytes = drain(*stream, 5ms);
        EXPECT(!bytes.empty());
    }
    EXPECT_EQ(sim.lost_bytes(), 0u);
}

PAKON_TEST(transfer_failure_surfaces_through_read) {
    SimConfig cfg;
    cfg.stream_fault_after_bytes = 50000;
    SimDevice sim(cfg);
    setup_geometry(sim);
    auto stream = make_stream(sim);
    EXPECT(stream->start().has_value());
    acquire(sim, true);
    std::vector<std::uint8_t> buf(65536);
    bool failed = false;
    for (int i = 0; i < 100 && !failed; ++i) {
        failed = !stream->read(buf, 10ms).has_value();
    }
    EXPECT(failed);
    acquire(sim, false);
    stream->stop();
    EXPECT(stream->stats().transfer_errors >= 1);
}

// A pipe the test drives by hand: queued reads complete, in submit
// order, only when released; each one is filled with its submit number.
class ScriptedPipe final : public pakon::usb::IBulkInPipe {
public:
    pakon::VoidResult submit(std::size_t slot, std::span<std::uint8_t> buffer) override {
        std::scoped_lock lock(mutex_);
        queue_.push_back({slot, buffer, submitted_++});
        cv_.notify_all();
        return {};
    }
    pakon::Result<std::optional<pakon::usb::PipeCompletion>> wait(
        std::chrono::milliseconds timeout) override {
        std::unique_lock lock(mutex_);
        if (!cv_.wait_for(lock, timeout, [&] {
                return !queue_.empty() && (aborted_ || released_ > 0);
            })) {
            return std::optional<pakon::usb::PipeCompletion>{};
        }
        auto q = queue_.front();
        queue_.pop_front();
        pakon::usb::PipeCompletion c;
        c.slot = q.slot;
        if (aborted_) {
            c.aborted = true;
        } else {
            --released_;
            std::fill(q.buffer.begin(), q.buffer.end(), static_cast<std::uint8_t>(q.seq));
            c.bytes = q.buffer.size();
        }
        return std::optional<pakon::usb::PipeCompletion>{c};
    }
    void abort() override {
        std::scoped_lock lock(mutex_);
        aborted_ = true;
        cv_.notify_all();
    }
    void release(std::size_t n) {
        std::scoped_lock lock(mutex_);
        released_ += n;
        cv_.notify_all();
    }
    std::size_t submitted() {
        std::scoped_lock lock(mutex_);
        return submitted_;
    }

private:
    struct Queued {
        std::size_t slot;
        std::span<std::uint8_t> buffer;
        std::size_t seq;
    };
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Queued> queue_;
    std::size_t submitted_{0};
    std::size_t released_{0};
    bool aborted_{false};
};

void wait_submitted(ScriptedPipe& pipe, std::size_t n) {
    for (int i = 0; i < 2000 && pipe.submitted() < n; ++i) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(pipe.submitted(), n);
}

// Reads already queued when discard() is called carry lines from before
// the settings change and are dropped; reads submitted after it are
// delivered (film2 hardware run, 2026-10-10).
PAKON_TEST(discard_also_drops_the_reads_already_queued) {
    StreamConfig cfg;
    cfg.queued_reads = 4;
    cfg.transfer_bytes = 16;
    cfg.ring_bytes = 1024;
    auto owned = std::make_unique<ScriptedPipe>();
    ScriptedPipe& pipe = *owned;
    ImageStream stream(std::move(owned), cfg);
    EXPECT(stream.start().has_value());
    wait_submitted(pipe, 4); // reads 0..3

    pipe.release(2);         // reads 0, 1 complete; 4, 5 are queued
    std::vector<std::uint8_t> buf(64);
    std::vector<std::uint8_t> got;
    while (got.size() < 32) {
        auto n = stream.read(buf, 1000ms);
        EXPECT(n.has_value() && *n > 0);
        if (!n || *n == 0) {
            break;
        }
        got.insert(got.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n));
    }
    EXPECT_EQ(got.size(), 32u);
    EXPECT_EQ(got.front(), 0u);
    EXPECT_EQ(got.back(), 1u);
    wait_submitted(pipe, 6);

    stream.discard();        // reads 2..5 are stale
    pipe.release(4);
    wait_submitted(pipe, 10); // resubmitted as reads 6..9
    pipe.release(1);
    auto n = stream.read(buf, 1000ms);
    EXPECT(n.has_value());
    EXPECT(n.has_value() && *n == 16u);
    EXPECT_EQ(buf[0], 6u);
    EXPECT_EQ(stream.stats().bytes_discarded, 64u);
    stream.stop();
}

int main() { return pakon::test::run_all(); }
