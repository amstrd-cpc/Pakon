// Capture fidelity (secondary to the simulator suites): the runner's init
// block and the OEM part of the teardown, byte-compared with the base4
// session of the corpus (tests/support/scan_capture_fixture.hpp, from
// alibosworth/pakon-captures base4.jsonl).

#include <format>
#include <string>
#include <vector>

#include "pakon/scan/device.hpp"
#include "pakon/scan/runner.hpp"
#include "support/scan_capture_fixture.hpp"
#include "support/test_harness.hpp"

namespace {

using namespace pakon;

std::string hex(const ppb::Frame& frame) {
    auto bytes = frame.serialize();
    std::string out;
    if (bytes) {
        for (const auto b : *bytes) {
            out += std::format("{:02x}", b);
        }
    }
    return out;
}

// Records frames and answers success in the reply form of each type.
class Recorder final : public scan::ICommandChannel {
public:
    Result<ppb::Reply> exchange(const ppb::Frame& frame) override {
        sent.push_back(hex(frame));
        ppb::Reply r;
        r.type = frame.type == ppb::FrameType::read || frame.type == ppb::FrameType::read_status
                     ? frame.type
                     : ppb::FrameType::ack;
        r.address = frame.data[0];
        r.status = frame.type == ppb::FrameType::read ? ppb::Status::success_alt : ppb::Status::ok;
        if (frame.type == ppb::FrameType::read) {
            r.payload.assign(frame.data[1], 0);
        }
        return r;
    }
    std::vector<std::string> sent;
};

} // namespace

PAKON_TEST(init_block_is_byte_identical_to_the_capture) {
    const auto frames = scan::ScanRunner::init_frames(protocol::kF135Plus);
    // base4.jsonl 0.951-1.067 (fixture entries 10..40).
    const std::size_t first = 10;
    EXPECT_EQ(frames.size(), 31u);
    for (std::size_t i = 0; i < frames.size() && i + first < std::size(test::scan_fixture::kSession);
         ++i) {
        EXPECT_EQ(hex(frames[i]), std::string(test::scan_fixture::kSession[first + i].req));
    }
}

PAKON_TEST(teardown_starts_with_the_oem_stop_in_capture_order) {
    Recorder rec;
    scan::ScanDevice device(rec, protocol::kF135Plus);
    // Film-window state of a Base-4 scan: control 0x63.
    const auto g = scan::film_geometry({scan::Base::b4, false}, 30);
    EXPECT(device.fpga_settings(g, 1875).has_value());
    EXPECT(device.acquire(true).has_value());
    rec.sent.clear();
    const auto& report = device.teardown();
    EXPECT(report.ran);
    EXPECT(report.all_ok());
    // base4.jsonl 32.385-32.460, then the bridge's rate=0 -> go -> idle.
    const char* expected[] = {"0206440382006200", "020440018000", "020410018402",
                              "040340008a",       "0403400092",   "04034400a2",
                              "02054402a50000",   "04034400a0",   "04034400a2"};
    EXPECT_EQ(rec.sent.size(), std::size(expected));
    for (std::size_t i = 0; i < rec.sent.size() && i < std::size(expected); ++i) {
        EXPECT_EQ(rec.sent[i], std::string(expected[i]));
    }
    // Exactly once: a second call sends nothing.
    device.teardown();
    EXPECT_EQ(rec.sent.size(), std::size(expected));
}

int main() { return pakon::test::run_all(); }
