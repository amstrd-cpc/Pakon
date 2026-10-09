#pragma once

// Scan session executor: drives the state machine (scan/session.hpp)
// through the capture-backed phase plans (scan/plan.hpp) over the
// command channel, while draining image windows over the image source
// under explicit completion policies.
//
// The runner itself performs no USB I/O: commands arrive through
// ICommandChannel and pixels through image::IImageSource. The approved
// live wiring (apps/pakon-cli/scan_cli.cpp) binds those to
// PpbCommandChannel + UsbImageSource over ONE already-open session;
// every test in this repository drives the runner with scripted
// channels and synthetic sources only.

#include <memory>

#include "pakon/image/completion.hpp"
#include "pakon/image/reader.hpp"
#include "pakon/image/source.hpp"
#include "pakon/scan/cancel.hpp"
#include "pakon/scan/plan.hpp"
#include "pakon/scan/session.hpp"
#include "pakon/scan/transport.hpp"

namespace pakon::scan {

// Default service-poll bounds. The wait used to be an unbounded
// while(true) (live-scan audit, blocker C); it is now deadline-bounded
// and progress-logged, with these capture-derived defaults:
//   - deadline: in base4-ir.jsonl init ends t=5.010, the first service
//     poll goes out at t=5.013 and the device answers service-wanted
//     at t=11.506 — 6.5 s. Worst reply latency over all 11741 captured
//     exchanges is 36 ms, so nothing near the deadline is ambiguous.
//     60 s is ~9x that margin; on expiry the session faults while the
//     device is still idle (lamp off, disengaged).
inline constexpr unsigned long kServiceWaitTimeoutMs = 60000;
//   - cadence: the OEM's background polls repeat at 20 Hz
//     (bridge/pakonusb.py:796) and the captured status reads average
//     ~29 ms apart. 50 ms sits in that band, bounds log volume and
//     gives the cancel/deadline checks sub-frame granularity.
inline constexpr unsigned long kServicePollIntervalMs = 50;

struct ScanRunnerConfig {
    protocol::ControllerAddresses addresses{protocol::kF135Plus};
    ScanPlanParameters plan{};

    // Completion policies for the two image windows. Both are
    // mandatory — there is no default end condition (see
    // image/completion.hpp).
    std::shared_ptr<const image::ICompletionPolicy> calibration_completion;
    std::shared_ptr<const image::ICompletionPolicy> transport_completion;

    // Image read size (defaults to the capture corpus's 20480-byte
    // transfers; any size works per image-stream.md).
    std::size_t read_bytes{image::kCaptureChunkBytes};

    // Cooperative cancellation (Ctrl+C): observed at the service wait,
    // at the two phase boundaries and — through UsbImageSource — before
    // every image read. NEVER observed inside a teardown: cleanup runs
    // to completion once it starts. nullptr = never cancelled (offline
    // runs and tests).
    const CancelToken* cancel{nullptr};

    // Service-poll deadline; see kServiceWaitTimeoutMs above for the
    // capture evidence behind the default. 0 would fault before the
    // first poll and is rejected by create().
    unsigned long service_wait_timeout_ms{kServiceWaitTimeoutMs};
};

struct ScanResult {
    image::RawImage calibration;  // W1: pre-scan calibration pass
    image::RawImage film;         // W2: film-transport pass
    image::WindowReport calibration_report;
    image::WindowReport transport_report;
};

class ScanRunner {
public:
    // Fails when either completion policy is missing (or the read size
    // is unusable) — before any I/O is possible.
    static Result<std::unique_ptr<ScanRunner>> create(ScanRunnerConfig config);

    // Run the whole session: init → service poll → calibration window
    // → engage → film window → teardown. Any command error, bad
    // status, or image error faults the session (terminal — abort, not
    // retry) and is returned.
    Result<ScanResult> run(ICommandChannel& commands, image::IImageSource& images);

    const ScanSession& session() const { return session_; }

    // For failure reporting (apps/pakon-cli/scan_cli.cpp): whether the
    // acquisition phase was reached, and whether the one teardown pass
    // (success or best-effort) was taken — so the CLI never claims a
    // teardown that did not run, and never claims one was needed when
    // the fault was before acquisition.
    bool acquisition_started() const { return acquisition_started_; }
    bool teardown_attempted() const { return teardown_attempted_; }

private:
    ScanRunner() = default;

    VoidResult run_plan(ICommandChannel& commands, const std::vector<ppb::Frame>& frames,
                        const char* phase);

    ScanSession session_;
    ScanRunnerConfig config_;
    std::vector<ppb::Frame> init_frames_;
    std::vector<ppb::Frame> service_frames_;
    std::vector<ppb::Frame> calibration_frames_;
    std::vector<ppb::Frame> transport_frames_;
    std::vector<ppb::Frame> teardown_frames_;

    // Failure policy: once the acquisition has started (service
    // handled — the lamp/CCD configuration and window streams follow),
    // a fault first attempts ONE best-effort teardown pass (idle motor,
    // lamp off, EndAcquisition, disengage) so a failed run leaves the
    // hardware quiescent. Bounded: never retried, never allowed to
    // replace the original error, and a fault inside the normal
    // teardown phase never re-enters it (teardown_attempted_ marks
    // both paths).
    bool acquisition_started_{false};
    bool teardown_attempted_{false};
};

} // namespace pakon::scan
