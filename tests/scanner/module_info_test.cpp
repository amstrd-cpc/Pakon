// ModuleInfo decode tests: captured payloads vs the lab unit's payloads.
//
// The only capture-evidenced text in a 12-byte module-info payload is the
// 5-byte ASCII id at offset 5..9 — present in both captured replies
// (alibosworth/pakon-captures, base4.jsonl events 151/159, F-135+ serial
// 16402, PICL+ and PICM+). The lab unit (010-203-04, identify run
// 2026-10-08) returns a different, non-printable layout from the same
// request; decoding must report no string rather than fabricate text from
// arbitrary printable bytes (the old whole-payload scan rendered the lab
// unit's light payload as "@!" and its motor payload as spaces).

#include "pakon/scanner/scanner.hpp"
#include "support/test_harness.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace {

pakon::scanner::ModuleInfo make(std::array<std::uint8_t, 12> raw) {
    pakon::scanner::ModuleInfo info;
    info.raw = raw;
    return info;
}

// Captured PICL+ reply payload (base4.jsonl event 151).
const auto kCaptureLight =
    make({0x0f, 0x0a, 0x05, 0x00, 0x00, 0x31, 0x32, 0x33, 0x34, 0x35, 0x00, 0x00});
// Captured PICM+ reply payload (base4.jsonl event 159).
const auto kCaptureMotor =
    make({0x10, 0x06, 0x05, 0x00, 0x00, 0x31, 0x32, 0x33, 0x34, 0x35, 0x00, 0x00});

// Lab unit 010-203-04, live replies to the identical request.
const auto kLabLight =
    make({0x04, 0x20, 0x40, 0x12, 0x04, 0xc0, 0x21, 0x02, 0x00, 0x00, 0x92, 0x00});
const auto kLabMotor =
    make({0x02, 0x20, 0x00, 0xa0, 0x00, 0x8c, 0x08, 0x00, 0x00, 0x20, 0x00, 0x00});

} // namespace

PAKON_TEST(module_info_capture_light_decodes_ascii_id) {
    EXPECT_EQ(kCaptureLight.printable(), std::string("12345"));
}

PAKON_TEST(module_info_capture_motor_decodes_ascii_id) {
    EXPECT_EQ(kCaptureMotor.printable(), std::string("12345"));
}

PAKON_TEST(module_info_lab_light_is_not_text) {
    // Regression: the old whole-payload scan returned "@!" (bytes
    // 0x40/0x21) for this payload. No string may be fabricated.
    EXPECT_EQ(kLabLight.printable(), std::string(""));
}

PAKON_TEST(module_info_lab_motor_is_not_text) {
    // The old scan returned two spaces (bytes 0x20 at offsets 1 and 9).
    EXPECT_EQ(kLabMotor.printable(), std::string(""));
}

PAKON_TEST(module_info_broken_window_is_not_a_string) {
    // A non-printable byte inside offsets 5..9 invalidates the id field.
    const auto broken =
        make({0x0f, 0x0a, 0x05, 0x00, 0x00, 0x31, 0x32, 0x33, 0x00, 0x35, 0x00, 0x00});
    EXPECT_EQ(broken.printable(), std::string(""));
}

PAKON_TEST(module_info_hex_lists_every_byte) {
    EXPECT_EQ(kLabLight.hex(),
              std::string("04 20 40 12 04 c0 21 02 00 00 92 00"));
    EXPECT_EQ(kCaptureLight.hex(),
              std::string("0f 0a 05 00 00 31 32 33 34 35 00 00"));
}

int main() {
    return pakon::test::run_all();
}
