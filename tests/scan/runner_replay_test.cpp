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

#include <cstdio>
#include <memory>
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

struct Replay {
    std::vector<Step> steps;
    test::SyntheticImageSource source{test::ImageTail::eos};
    scan::ScanRunnerConfig config;
};

// Builds the whole replay script; reports construction problems via
// `error` (empty == success).
bool build_replay(Replay& out, std::string& error) {
    const auto& addresses = protocol::kF135Plus;
    const scan::ScanPlanParameters params{};

    FixtureCursor cursor;
    std::vector<Step> steps;

    auto add_plan = [&](const std::vector<ppb::Frame>& frames, std::size_t diverged) {
        for (std::size_t i = 0; i < frames.size(); ++i) {
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
    add_plan(scan::transport_plan(addresses, params), 0);
    if (!error.empty()) {
        return false;
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

int main() { return pakon::test::run_all(); }
