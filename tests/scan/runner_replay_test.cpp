// Full offline replay of the captured base4 session.
//
// The runner's frames are checked against the capture fixture
// (tests/support/scan_capture_fixture.hpp, generated from
// alibosworth/pakon-captures by tools/gen_scan_fixture.py) as an
// ordered subsequence: every non-poll frame must appear in the
// recorded session, in recorded order, with the recorded reply. The
// two image windows are synthetic marker streams with DIFFERENT row
// periods (6108 and 3081 samples — periods in the range the capture
// corpus and its bridge measured), proving per-window measurement
// rather than a shared stride.
//
// One divergence from the base4 recording is allowed and documented:
// the calibration-head trigger and its mux write. In base4 the OEM
// sent trigger1 during the idle phase, BEFORE the service event; in
// three of the six captured configurations the service event comes
// first. The runner places the trigger at the head of the window it
// arms (the bridge: the trigger resets the line counter / arms the
// window), i.e. the base8/base16/base4-ir ordering. Those two frames
// are still sent byte-exactly and still checked against recorded hex
// — only their position is allowed to differ.

#include <algorithm>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pakon/scan/runner.hpp"
#include "support/scan_capture_fixture.hpp"
#include "support/scripted_channel.hpp"
#include "support/synthetic_image.hpp"
#include "support/test_harness.hpp"

namespace {

using namespace pakon;
namespace fixture = pakon::test::scan_fixture;
using Step = test::ScriptedCommandChannel::Step;

std::string hex_of(const ppb::Frame& frame) {
    auto bytes = frame.serialize();
    if (!bytes) {
        return {};
    }
    return test::ScriptedCommandChannel::to_hex(*bytes);
}

// Fixture cursor: finds each next recorded frame with a given request,
// so sent frames are validated as an ordered subsequence.
struct FixtureCursor {
    std::string find(const std::string& req) {
        for (std::size_t i = pos; i < std::size(fixture::kSession); ++i) {
            if (req == fixture::kSession[i].req) {
                pos = i + 1;
                return fixture::kSession[i].rsp;
            }
        }
        return {};
    }
    std::size_t pos{0};
};

// W1 (calibration): 16 rows of 6108 samples; W2 (film): 24 rows of
// 3081 samples. Idle reads between the windows model the capture's
// quiet gap (W1 ends, W2 starts seconds later).
constexpr std::size_t kW1Period = 6108;
constexpr std::size_t kW1Rows = 16;
constexpr std::size_t kW2Period = 3081;
constexpr std::size_t kW2Rows = 24;

// Service-phase script shape built by build_replay: three idle polls,
// then the recorded service-wanted poll.
constexpr std::size_t kScriptedServicePolls = 4;

// Requests the cancel token on its `at`-th exchange (1-based), then
// passes everything through unchanged: models Ctrl+C landing inside a
// command phase, at an exactly known frame.
struct InterruptingChannel final : scan::ICommandChannel {
    InterruptingChannel(test::ScriptedCommandChannel& inner, scan::CancelToken& token,
                        std::size_t at)
        : inner_(inner), token_(token), at_(at) {}

    Result<ppb::Reply> exchange(const ppb::Frame& frame) override {
        if (++count_ == at_) {
            token_.request();
        }
        return inner_.exchange(frame);
    }

    test::ScriptedCommandChannel& inner_;
    scan::CancelToken& token_;
    std::size_t at_;
    std::size_t count_{0};
};

struct Replay {
    std::vector<Step> steps;
    test::SyntheticImageSource source{test::ImageTail::eos};
    scan::ScanRunnerConfig config;
};

// Builds the whole replay script; reports construction problems via
// `error` (empty == success). When `transport_fault` is set, the reply
// to that transport-plan frame is flipped into a rejection AND the
// remaining transport frames are dropped from the script — modelling a
// device that rejects mid-phase: the runner must stop there and send
// only its best-effort teardown next (the script's tail is exactly the
// teardown plan, so consuming it proves the teardown ran).
bool build_replay(Replay& out, std::string& error,
                  std::optional<std::size_t> transport_fault = std::nullopt) {
    const auto& addresses = protocol::kF135Plus;
    const scan::ScanPlanParameters params{};

    FixtureCursor cursor;
    std::vector<Step> steps;

    auto add_plan = [&](const std::vector<ppb::Frame>& frames, std::size_t diverged,
                        std::size_t limit = std::numeric_limits<std::size_t>::max()) {
        const std::size_t count = std::min(frames.size(), limit);
        for (std::size_t i = 0; i < count; ++i) {
            const auto req = hex_of(frames[i]);
            std::string rsp;
            if (req.empty()) {
                error = "frame failed to serialize";
                return;
            }
            if (i < diverged) {
                // Position-diverged frame: recorded reply looked up by
                // request hex anywhere in the session.
                for (const auto& entry : fixture::kSession) {
                    if (req == entry.req) {
                        rsp = entry.rsp;
                        break;
                    }
                }
            } else {
                rsp = cursor.find(req);
            }
            if (rsp.empty()) {
                error = "no recorded reply for " + req;
                return;
            }
            steps.push_back({req, rsp});
        }
    };

    add_plan(scan::init_plan(addresses, params), 0);
    if (!error.empty()) {
        return false;
    }
    // ready: a few idle service polls (the recorded idle reply), then
    // the recorded service-wanted poll.
    for (int i = 0; i < 3; ++i) {
        steps.push_back({fixture::kServicePollReq, "0103400800"});
    }
    steps.push_back({fixture::kServicePollReq, fixture::kServicePollRsp});
    add_plan(scan::service_plan(addresses), 0);
    if (!error.empty()) {
        return false;
    }
    // Calibration head: trigger + mux are the position-diverged pair.
    add_plan(scan::calibration_plan(addresses, params), 2);
    if (!error.empty()) {
        return false;
    }
    const auto transport = scan::transport_plan(addresses, params);
    const std::size_t transport_limit =
        transport_fault ? std::min(*transport_fault + 1, transport.size())
                        : transport.size();
    add_plan(transport, 0, transport_limit);
    if (!error.empty()) {
        return false;
    }
    if (transport_fault) {
        if (*transport_fault >= transport.size()) {
            error = "transport fault index out of range";
            return false;
        }
        // Status 0x01 = not acknowledged: the rejection that faults
        // the runner mid-phase (same ack shape the capture corpus
        // records for write/command frames).
        steps.back().rsp = "07024401";
    }
    add_plan(scan::teardown_plan(addresses, params), 0);
    if (!error.empty()) {
        return false;
    }

    test::SyntheticImageSource source(test::ImageTail::eos);
    source.append_data(test::encode_rows(test::make_rows(kW1Period, kW1Rows)));
    source.append_idle(2); // quiet gap between the windows
    source.append_data(test::encode_rows(test::make_rows(kW2Period, kW2Rows)));

    scan::ScanRunnerConfig config;
    config.addresses = addresses;
    config.plan = params;
    config.calibration_completion =
        std::make_shared<const image::RowBudgetCompletion>(kW1Rows);
    config.transport_completion = std::make_shared<const image::SourceEndCompletion>();
    config.read_bytes = image::kCaptureChunkBytes; // the captures' 20480

    out = Replay{std::move(steps), std::move(source), std::move(config)};
    return true;
}

} // namespace

PAKON_TEST(full_session_replays_the_captured_session) {
    Replay replay;
    std::string error;
    if (!build_replay(replay, error)) {
        EXPECT_EQ(error, std::string{});
        return;
    }
    test::ScriptedCommandChannel channel(replay.steps);

    auto runner = scan::ScanRunner::create(replay.config);
    EXPECT(runner.has_value());
    if (!runner) {
        return;
    }
    auto result = (*runner)->run(channel, replay.source);
    if (!result) {
        std::fprintf(stderr, "run failed: %s\n", result.error().message.c_str());
    }
    EXPECT(result.has_value());
    if (!result) {
        return;
    }

    // Every scripted frame was sent, in order, exactly once.
    EXPECT(channel.exhausted());

    // The session walked its full state machine.
    EXPECT_EQ((*runner)->session().state(), scan::ScanState::complete);
    EXPECT_EQ((*runner)->session().log().size(), 9u);

    // Both windows framed, with their OWN measured periods.
    EXPECT_EQ(result->calibration.geometry.samples_per_row, kW1Period);
    EXPECT_EQ(result->film.geometry.samples_per_row, kW2Period);
    EXPECT_EQ(result->calibration.rows(), kW1Rows);
    EXPECT_EQ(result->film.rows(), kW2Rows);
    EXPECT_EQ(result->calibration_report.rows, kW1Rows);
    EXPECT_EQ(result->transport_report.rows, kW2Rows);
    EXPECT_EQ(result->calibration_report.truncated_tail_bytes, 0u);
    EXPECT_EQ(result->transport_report.truncated_tail_bytes, 0u);

    // Completion policies are named in the reports (explicit, visible).
    EXPECT(std::string(result->calibration_report.completion).find("row-budget") !=
           std::string::npos);
    EXPECT(std::string(result->transport_report.completion).find("source-end") !=
           std::string::npos);

    // Pixel fidelity: row r, sample i is exactly what the generator
    // produced — framing, phase and endianness all correct.
    for (std::size_t r = 0; r < kW1Rows; ++r) {
        for (std::size_t i = 0; i < kW1Period; i += 97) {
            EXPECT_EQ(result->calibration.samples[r * kW1Period + i],
                      test::row_sample(r, i));
        }
    }
    for (std::size_t r = 0; r < kW2Rows; ++r) {
        for (std::size_t i = 0; i < kW2Period; i += 53) {
            EXPECT_EQ(result->film.samples[r * kW2Period + i], test::row_sample(r, i));
        }
    }
    // Byte accounting: every delivered byte reached the reader.
    EXPECT_EQ(result->calibration_report.bytes_received, kW1Period * kW1Rows * 2);
    EXPECT_EQ(result->transport_report.bytes_received, kW2Period * kW2Rows * 2);
}

PAKON_TEST(runner_requires_completion_policies) {
    scan::ScanRunnerConfig config;
    auto bare = scan::ScanRunner::create(config);
    EXPECT(!bare.has_value());
    if (!bare) {
        EXPECT_EQ(bare.error().kind, ErrorKind::image_no_completion_policy);
    }
    config.calibration_completion = std::make_shared<const image::RowBudgetCompletion>(1);
    auto still_bare = scan::ScanRunner::create(config);
    EXPECT(!still_bare.has_value());
}

PAKON_TEST(rejected_reply_faults_the_session_without_retry) {
    Replay replay;
    std::string error;
    if (!build_replay(replay, error)) {
        EXPECT_EQ(error, std::string{});
        return;
    }
    // Flip the very last teardown reply (DisengageFilmDrive's ack) into
    // a rejection: status 0x01 = not acknowledged. The runner must
    // abort at that exact frame — session faulted, nothing retried.
    EXPECT(!replay.steps.empty());
    replay.steps.back().rsp = "07024401";
    test::ScriptedCommandChannel channel(replay.steps);

    auto runner = scan::ScanRunner::create(replay.config);
    EXPECT(runner.has_value());
    if (!runner) {
        return;
    }
    auto result = (*runner)->run(channel, replay.source);
    EXPECT(!result.has_value());
    if (!result) {
        EXPECT_EQ(result.error().kind, ErrorKind::ppb_bad_status);
    }
    EXPECT_EQ((*runner)->session().state(), scan::ScanState::failed);
    EXPECT((*runner)->session().terminal());
    // The failure happened on the last scripted frame: the session got
    // all the way through teardown, then aborted on the rejection.
    EXPECT(channel.exhausted());
}

PAKON_TEST(fault_in_transport_runs_one_best_effort_teardown) {
    Replay replay;
    std::string error;
    // Reject the FIRST transport frame (the pre-run idle restore): the
    // runner must stop the phase there, attempt exactly one teardown
    // pass, and return the original error.
    if (!build_replay(replay, error, /*transport_fault=*/0)) {
        EXPECT_EQ(error, std::string{});
        return;
    }
    test::ScriptedCommandChannel channel(replay.steps);

    auto runner = scan::ScanRunner::create(replay.config);
    EXPECT(runner.has_value());
    if (!runner) {
        return;
    }
    auto result = (*runner)->run(channel, replay.source);
    EXPECT(!result.has_value());
    if (!result) {
        EXPECT_EQ(result.error().kind, ErrorKind::ppb_bad_status);
    }
    EXPECT_EQ((*runner)->session().state(), scan::ScanState::failed);
    EXPECT((*runner)->session().terminal());

    // The script's tail after the rejected frame is exactly the
    // teardown plan — exhausting it proves the best-effort teardown ran
    // after the fault, once, and stopped there.
    EXPECT(channel.exhausted());
    const auto teardown =
        scan::teardown_plan(protocol::kF135Plus, scan::ScanPlanParameters{});
    const auto& sent = channel.sent();
    EXPECT(sent.size() > teardown.size());
    if (sent.size() > teardown.size()) {
        // The frame just before the teardown tail is the rejected one.
        const auto rejected = scan::transport_plan(
            protocol::kF135Plus, scan::ScanPlanParameters{}).front().serialize();
        EXPECT(rejected.has_value());
        if (rejected) {
            EXPECT_EQ(sent[sent.size() - teardown.size() - 1],
                      test::ScriptedCommandChannel::to_hex(*rejected));
        }
    }
    for (std::size_t i = 0; i < teardown.size() && i < sent.size(); ++i) {
        const auto bytes = teardown[i].serialize();
        EXPECT(bytes.has_value());
        if (bytes) {
            EXPECT_EQ(sent[sent.size() - teardown.size() + i],
                      test::ScriptedCommandChannel::to_hex(*bytes));
        }
    }
}

PAKON_TEST(service_poll_deadline_faults_while_device_still_idle) {
    // Blocker C: the service wait was an unbounded while(true). A zero
    // deadline is rejected before any I/O...
    Replay replay;
    std::string error;
    if (!build_replay(replay, error)) {
        EXPECT_EQ(error, std::string{});
        return;
    }
    scan::ScanRunnerConfig zero = replay.config;
    zero.service_wait_timeout_ms = 0;
    auto rejected = scan::ScanRunner::create(zero);
    EXPECT(!rejected.has_value());
    if (!rejected) {
        EXPECT_EQ(rejected.error().kind, ErrorKind::scanner_timeout);
    }

    // ...and a short one ends the wait with a diagnostic instead of
    // hanging: the service-wanted poll is replaced by idle replies, so
    // the DEADLINE (not the device) must end the wait.
    const std::size_t init =
        scan::init_plan(protocol::kF135Plus, scan::ScanPlanParameters{}).size();
    replay.steps.insert(replay.steps.begin() + static_cast<std::ptrdiff_t>(init + 3),
                        12, Step{fixture::kServicePollReq, "0103400800"});
    replay.config.service_wait_timeout_ms = 120;

    test::ScriptedCommandChannel channel(replay.steps);
    auto runner = scan::ScanRunner::create(replay.config);
    EXPECT(runner.has_value());
    if (!runner) {
        return;
    }
    auto result = (*runner)->run(channel, replay.source);
    EXPECT(!result.has_value());
    if (!result) {
        EXPECT_EQ(result.error().kind, ErrorKind::scanner_timeout);
        EXPECT(std::string(result.error().message).find("did not request service") !=
               std::string::npos);
    }
    EXPECT_EQ((*runner)->session().state(), scan::ScanState::failed);
    EXPECT((*runner)->session().terminal());
    // The fault was before acquisition: the device never left its
    // idle configuration and no teardown was attempted (none needed).
    EXPECT(!(*runner)->acquisition_started());
    EXPECT(!(*runner)->teardown_attempted());
    // Only init frames and a bounded number of polls went out — the
    // wait cannot have run away (script far from exhausted).
    EXPECT(!channel.exhausted());
    EXPECT(channel.position() < init + 15);
}

PAKON_TEST(cancel_before_service_ends_wait_without_teardown) {
    // Blocker B: a pre-latched interrupt is observed at the FIRST
    // service-wait iteration — only init frames were sent, no service
    // poll, no teardown (the device is still idle).
    Replay replay;
    std::string error;
    if (!build_replay(replay, error)) {
        EXPECT_EQ(error, std::string{});
        return;
    }
    scan::CancelToken token;
    token.request();
    replay.config.cancel = &token;

    test::ScriptedCommandChannel channel(replay.steps);
    auto runner = scan::ScanRunner::create(replay.config);
    EXPECT(runner.has_value());
    if (!runner) {
        return;
    }
    auto result = (*runner)->run(channel, replay.source);
    EXPECT(!result.has_value());
    if (!result) {
        EXPECT_EQ(result.error().kind, ErrorKind::cancelled);
        EXPECT(std::string(result.error().message).find("still idle") !=
               std::string::npos);
    }
    EXPECT_EQ((*runner)->session().state(), scan::ScanState::failed);
    EXPECT((*runner)->session().terminal());
    const std::size_t init =
        scan::init_plan(protocol::kF135Plus, scan::ScanPlanParameters{}).size();
    EXPECT_EQ(channel.position(), init);
    EXPECT(!(*runner)->acquisition_started());
    EXPECT(!(*runner)->teardown_attempted());
}

PAKON_TEST(cancel_at_phase_boundary_runs_teardown_before_windows) {
    // Interrupt landing on the LAST service-phase frame: the boundary
    // check after service_done (acquisition already marked started)
    // must cancel before any calibration frame, and the one
    // best-effort teardown must be attempted — its first frame finds
    // the script's next step is a calibration frame, the mismatch is
    // logged and swallowed, and the CANCELLED error is what returns.
    Replay replay;
    std::string error;
    if (!build_replay(replay, error)) {
        EXPECT_EQ(error, std::string{});
        return;
    }
    scan::CancelToken token;
    test::ScriptedCommandChannel inner(replay.steps);
    const std::size_t init =
        scan::init_plan(protocol::kF135Plus, scan::ScanPlanParameters{}).size();
    const std::size_t service =
        scan::service_plan(protocol::kF135Plus).size();
    const std::size_t at = init + kScriptedServicePolls + service; // last service frame
    InterruptingChannel channel(inner, token, at);
    replay.config.cancel = &token;

    auto runner = scan::ScanRunner::create(replay.config);
    EXPECT(runner.has_value());
    if (!runner) {
        return;
    }
    auto result = (*runner)->run(channel, replay.source);
    EXPECT(!result.has_value());
    if (!result) {
        EXPECT_EQ(result.error().kind, ErrorKind::cancelled);
        EXPECT(std::string(result.error().message).find("before the calibration window") !=
               std::string::npos);
    }
    EXPECT_EQ((*runner)->session().state(), scan::ScanState::failed);
    EXPECT((*runner)->session().terminal());
    EXPECT((*runner)->acquisition_started());
    EXPECT((*runner)->teardown_attempted());
    // Beyond the interrupt frame, at most the single teardown frame
    // that hit the mismatching script step: no calibration frame ran.
    EXPECT(inner.position() <= at + 1);
}

PAKON_TEST(cancel_after_calibration_never_starts_film_window) {
    // Interrupt landing on the LAST calibration frame: W1 drains to
    // its row budget (the synthetic source ignores the token, exactly
    // like a device mid-stream), then the boundary before the film
    // window fires — the transport plan must NOT start, and the one
    // best-effort teardown is attempted.
    Replay replay;
    std::string error;
    if (!build_replay(replay, error)) {
        EXPECT_EQ(error, std::string{});
        return;
    }
    scan::CancelToken token;
    test::ScriptedCommandChannel inner(replay.steps);
    const std::size_t init =
        scan::init_plan(protocol::kF135Plus, scan::ScanPlanParameters{}).size();
    const std::size_t service = scan::service_plan(protocol::kF135Plus).size();
    const std::size_t calibration =
        scan::calibration_plan(protocol::kF135Plus, scan::ScanPlanParameters{}).size();
    const std::size_t at = init + kScriptedServicePolls + service + calibration;
    InterruptingChannel channel(inner, token, at);
    replay.config.cancel = &token;

    auto runner = scan::ScanRunner::create(replay.config);
    EXPECT(runner.has_value());
    if (!runner) {
        return;
    }
    auto result = (*runner)->run(channel, replay.source);
    EXPECT(!result.has_value());
    if (!result) {
        EXPECT_EQ(result.error().kind, ErrorKind::cancelled);
        EXPECT(std::string(result.error().message).find(
                   "after the calibration window") != std::string::npos);
    }
    EXPECT_EQ((*runner)->session().state(), scan::ScanState::failed);
    EXPECT((*runner)->session().terminal());
    EXPECT((*runner)->acquisition_started());
    EXPECT((*runner)->teardown_attempted());
    // The film window never started: past the interrupt frame (92),
    // exactly TWO more exchanges — teardown[0] (motor idle) is
    // byte-identical to transport[0] so it consumes that step, and
    // teardown[1] (lamp off) mismatches transport[1] and aborts the
    // best-effort pass, whose failure is swallowed. No transport frame
    // of its own ever went out.
    EXPECT_EQ(inner.position(), at + 2);
}

int main() { return pakon::test::run_all(); }
