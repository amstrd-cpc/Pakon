#pragma once

// Explicit completion detection for one image window.
//
// pakon-reference's device-signalled model: the host does not invent a
// fixed byte budget for a window; the window ends when the device
// stops producing data or the host's own explicitly-chosen budget is
// met. There is deliberately NO default policy: a scan window's
// configuration must name one, and a session without one fails to
// construct (see scan/runner.hpp).
//
// Policy vocabulary, grounded in the capture corpus's own bridge
// (pakon-captures/bridge/pakonusb.py, whose watchdog constants were
// tuned against real sessions):
//
//  - SourceEnd: the image source reports end-of-stream — the device
//    stopped feeding the endpoint. This is the device-signalled case
//    the reference describes for end-of-roll (the device stops
//    autonomously; the capture corpus's -nofeed session shows it).
//
//  - NoProgress: the source delivered no bytes for N consecutive read
//    attempts. Maps to the bridge's watchdogs: IDLE_STOP = 45 s of
//    total device silence (the OEM heartbeats while scanning, so real
//    silence means it has given up) and IMG_IDLE_STOP = 120 s of image
//    silence while commands continue. Counting read attempts keeps the
//    policy deterministic and offline-testable; the live mapping
//    (bulk-read timeout == one idle tick) is the runner's job.
//
//  - RowBudget: an explicit host-side row budget. pakon-reference
//    disfavours byte/row budgets ("invented budgets... are exactly
//    this kind of guess"), so a caller choosing one is making a
//    deliberate, visible decision — which is all this policy is. The
//    pre-scan calibration pass (dark/bright reference lines) is not
//    device-signalled at all in the captured sessions, so an explicit
//    line budget is the only workable trigger there.
//
//  - Any: composite, completes when any member completes.

#include <cstddef>
#include <memory>
#include <vector>

namespace pakon::image {

// What the reader reports to the policy after each read attempt.
struct WindowStats {
    std::size_t rows_completed{0};   // complete rows framed so far
    std::size_t bytes_received{0};   // stream bytes delivered so far
    std::size_t idle_ticks{0};       // consecutive reads with no data
    bool source_ended{false};        // source reports device end-of-stream
};

enum class Completion { continue_reading, complete };

class ICompletionPolicy {
public:
    virtual ~ICompletionPolicy() = default;
    virtual Completion check(const WindowStats& stats) const = 0;
    // Human-readable label for logs and reports.
    virtual const char* label() const = 0;
};

// Device stopped feeding the endpoint (end-of-stream).
class SourceEndCompletion final : public ICompletionPolicy {
public:
    Completion check(const WindowStats& stats) const override {
        return stats.source_ended ? Completion::complete : Completion::continue_reading;
    }
    const char* label() const override { return "source-end (device signalled)"; }
};

// No data for `idle_limit` consecutive read attempts.
class NoProgressCompletion final : public ICompletionPolicy {
public:
    explicit NoProgressCompletion(std::size_t idle_limit) : idle_limit_(idle_limit) {}
    Completion check(const WindowStats& stats) const override {
        return stats.idle_ticks >= idle_limit_ ? Completion::complete
                                               : Completion::continue_reading;
    }
    const char* label() const override { return "no-progress (image quiescence)"; }

private:
    std::size_t idle_limit_;
};

// Explicit host-side row budget.
class RowBudgetCompletion final : public ICompletionPolicy {
public:
    explicit RowBudgetCompletion(std::size_t rows) : rows_(rows) {}
    Completion check(const WindowStats& stats) const override {
        return stats.rows_completed >= rows_ ? Completion::complete
                                             : Completion::continue_reading;
    }
    const char* label() const override { return "row-budget (explicit host budget)"; }

private:
    std::size_t rows_;
};

// Composite: complete when any member completes.
class AnyCompletion final : public ICompletionPolicy {
public:
    AnyCompletion() = default;
    AnyCompletion(std::vector<std::shared_ptr<const ICompletionPolicy>> members)
        : members_(std::move(members)) {}
    Completion check(const WindowStats& stats) const override {
        for (const auto& m : members_) {
            if (m->check(stats) == Completion::complete) {
                return Completion::complete;
            }
        }
        return Completion::continue_reading;
    }
    const char* label() const override { return "any-of"; }

private:
    std::vector<std::shared_ptr<const ICompletionPolicy>> members_;
};

} // namespace pakon::image
