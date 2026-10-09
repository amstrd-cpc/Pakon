#pragma once

// Cooperative cancellation for a running scan.
//
// Why this exists: a live run used to have NO interruption path at all
// — Ctrl+C killed the process with no teardown and no disconnect,
// leaving lamp/drive energised (live-scan audit, blocker B). The fix
// is a latch, not a handler chain: the console/signal handler does
// exactly one async-signal-safe thing (request() below), and every
// bounded point of the run observes the token and unwinds through the
// runner's ONE best-effort teardown (scan/runner.cpp), the
// DisconnectGuard (apps/pakon-cli/scan_cli.cpp) and a bounded pipe
// deadline on any in-flight transfer (usb::kPipeTimeoutMs).
//
// Observation points (never inside a teardown — cleanup must finish):
//   - service wait: every poll iteration (scan/runner.cpp);
//   - phase boundaries: before the calibration window, before the
//     film window (scan/runner.cpp);
//   - every image read, before the pipe is touched: UsbImageSource
//     (scan/transport.hpp), so an interrupt never becomes a stuck
//     transfer mid-window.

#include <atomic>

namespace pakon::scan {

class CancelToken {
public:
    // Safe in a signal/console-handler context: a relaxed store to an
    // always-lock-free bool (pinned below) — nothing else runs there.
    void request() noexcept { requested_.store(true, std::memory_order_relaxed); }
    bool requested() const noexcept { return requested_.load(std::memory_order_relaxed); }

    // Bookkeeping for tests and for a token reused across runs: a
    // fresh run starts un-cancelled.
    void reset() noexcept { requested_.store(false, std::memory_order_relaxed); }

private:
    std::atomic<bool> requested_{false};
};

static_assert(std::atomic<bool>::is_always_lock_free,
              "the cancel token must be a lock-free store to be safe in a "
              "signal handler");

} // namespace pakon::scan
