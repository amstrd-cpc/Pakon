// Byte-exact checks for the scan builders against request hex recorded
// in the capture corpus (alibosworth/pakon-captures, F-135+ serial 16402),
// and the per-mode constants against all six capture configurations.

#include <format>
#include <string>

#include "pakon/protocol/scan_commands.hpp"
#include "pakon/scan/modes.hpp"
#include "support/test_harness.hpp"

namespace {

using namespace pakon::protocol::scan;
using pakon::scan::Base;
using pakon::scan::ScanMode;

std::string hex(const pakon::ppb::Frame& frame) {
    auto bytes = frame.serialize();
    std::string out;
    if (bytes) {
        for (const auto b : *bytes) {
            out += std::format("{:02x}", b);
        }
    }
    return out;
}

constexpr std::uint8_t kPicl = 0x40;
constexpr std::uint8_t kPicm = 0x44;

} // namespace

PAKON_TEST(fpga_and_ad_frames_match_captures) {
    EXPECT_EQ(hex(fpga(kPicm, Fpga::control, 0x0063)), std::string("0206440382006300"));
    EXPECT_EQ(hex(fpga(kPicm, Fpga::control, 0x0161)), std::string("0206440382006101"));
    EXPECT_EQ(hex(fpga(kPicm, Fpga::integration, 0x0FFD)), std::string("020644038206fd0f"));
    EXPECT_EQ(hex(fpga(kPicm, Fpga::pixel_end, 0x080E)), std::string("0206440382050e08"));
    EXPECT_EQ(hex(fpga(kPicm, Fpga::panel_leds, 0x0295)), std::string("0206440382099502"));
    EXPECT_EQ(hex(ad_gain(kPicm, Channel::r, gain_code(1.2))), std::string("0206440384020d00"));
    // The captured dark-offset ramp: -51, -48, -52, then -34 (sign-magnitude)
    EXPECT_EQ(hex(ad_offset(kPicm, Channel::r, -51)), std::string("0206440384053301"));
    EXPECT_EQ(hex(ad_offset(kPicm, Channel::g, -48)), std::string("0206440384063001"));
    EXPECT_EQ(hex(ad_offset(kPicm, Channel::b, -52)), std::string("0206440384073401"));
    EXPECT_EQ(hex(ad_offset(kPicm, Channel::r, 10)), std::string("0206440384050a00"));
    EXPECT_EQ(encode_offset(-300), 0x1FF);
    EXPECT_EQ(gain_code(1.0), 0);
    EXPECT_EQ(gain_code(99.0), 63);
}

PAKON_TEST(lamp_and_service_frames_match_captures) {
    EXPECT_EQ(hex(lamp_mask(kPicl, true, false)), std::string("020440018001"));
    EXPECT_EQ(hex(lamp_mask(kPicl, true, true)), std::string("020440018003"));
    EXPECT_EQ(hex(lamp_mask(kPicl, false, false)), std::string("020440018000"));
    // base4.jsonl 22.111: B=6 IR=0 R=3 G=9
    EXPECT_EQ(hex(led_currents(kPicl, {3, 9, 6, 0}, led_ceiling(kPicm, false))),
              std::string("02084005810600030009"));
    // base4.jsonl 22.114: on-times [B, IR, R, 0, G, base]
    EXPECT_EQ(hex(led_on_times(kPicl, {0xF1, 0xA9, 0x47, 0, 0x1C2})),
              std::string("020f400c8247000000f1000000a900c201"));
    EXPECT_EQ(hex(interrupt_ack(kPicl, 0x02)), std::string("02054002060002"));
    EXPECT_EQ(hex(interrupt_ack(kPicl, 0x22)), std::string("02054002060022"));
    EXPECT_EQ(hex(read_interrupt_status(kPicl)), std::string("0103400102"));
    EXPECT_EQ(hex(read_dx_records(kPicl)), std::string("0103401e90"));
    const auto fifos = reset_fifos(pakon::protocol::kF135Plus);
    EXPECT_EQ(hex(fifos[0]), std::string("020410018402"));
    EXPECT_EQ(hex(fifos[1]), std::string("040340008a"));
    EXPECT_EQ(hex(dx_stop(kPicl)), std::string("0403400092"));
}

PAKON_TEST(motor_frames_match_captures) {
    EXPECT_EQ(hex(motor_rate(kPicm, 0x647E)), std::string("02054402a57e64"));
    EXPECT_EQ(hex(motor_go(kPicm)), std::string("04034400a0"));
    EXPECT_EQ(hex(motor_stop(kPicm)), std::string("04034400a2"));
}

PAKON_TEST(led_ceilings_are_the_firmware_table_and_clamp) {
    const auto plus_ir = led_ceiling(kPicm, true);
    EXPECT_EQ(plus_ir.r, 8);
    EXPECT_EQ(plus_ir.g, 24);
    EXPECT_EQ(plus_ir.ir, 8);
    const auto plus = led_ceiling(kPicm, false);
    EXPECT_EQ(plus.r, 4);
    EXPECT_EQ(plus.ir, 0);
    const auto base = led_ceiling(0x24, false);
    EXPECT_EQ(base.r, 6);
    EXPECT_EQ(base.g, 8);
    const auto unknown = led_ceiling(0x99, true);
    EXPECT_EQ(unknown.r, 4);  // strictest of every column
    EXPECT_EQ(unknown.ir, 0);
    // A request above the ceiling is clamped inside the builder.
    EXPECT_EQ(hex(led_currents(kPicl, {99, 99, 99, 99}, plus)),
              std::string("02084005811400040014"));
}

PAKON_TEST(on_time_base_fits_the_four_captured_configurations) {
    EXPECT_EQ(on_time_base(1875), 450);
    EXPECT_EQ(on_time_base(1250), 300);
    EXPECT_EQ(on_time_base(2813), 675);
    EXPECT_EQ(on_time_base(4093), 982); // 0x3D6, the init block
    EXPECT_EQ(on_time(450, 1.5), 448);  // never above base - 2
}

PAKON_TEST(mode_constants_match_all_six_captures) {
    using pakon::scan::calibration_geometry;
    using pakon::scan::dx_word;
    using pakon::scan::film_geometry;
    using pakon::scan::integration;
    // 0x91 words and integrations (sub 6) per capture file.
    EXPECT_EQ(dx_word({Base::b4, false}), 0x0107);
    EXPECT_EQ(dx_word({Base::b4, true}), 0x00C5);
    EXPECT_EQ(dx_word({Base::b8, false}), 0x0075);
    EXPECT_EQ(dx_word({Base::b8, true}), 0x004D);
    EXPECT_EQ(dx_word({Base::b16, false}), 0x003C);
    EXPECT_EQ(dx_word({Base::b16, true}), 0x0031);
    EXPECT_EQ(integration({Base::b4, false}), 1875);
    EXPECT_EQ(integration({Base::b4, true}), 1250);
    EXPECT_EQ(integration({Base::b8, false}), 2813);
    EXPECT_EQ(integration({Base::b8, true}), 2128);
    EXPECT_EQ(integration({Base::b16, false}), 4093);
    EXPECT_EQ(integration({Base::b16, true}), 2498);
    // Geometry with unit 16402's Offsets: sub4/sub5 and the measured
    // line sizes (bridge narration 6162/9234/12324 bytes).
    auto c4 = calibration_geometry({Base::b4, false}, 30, false);
    EXPECT_EQ(c4.start, 3);
    EXPECT_EQ(c4.end, 1030);
    EXPECT_EQ(c4.samples_per_line() * 2, 6162u);
    auto c8 = calibration_geometry({Base::b8, false}, 58, false);
    EXPECT_EQ(c8.end, 2058);
    EXPECT_EQ(c8.samples_per_line() * 2, 9234u);
    auto c16 = calibration_geometry({Base::b16, false}, 60, false);
    EXPECT_EQ(c16.end, 2060);
    EXPECT_EQ(c16.samples_per_line() * 2, 12324u);
    // Film windows: 3000 / 4500 / 6000 samples, IR 4000 / 6000 / 8000.
    EXPECT_EQ(film_geometry({Base::b4, false}, 30).samples_per_line(), 3000u);
    EXPECT_EQ(film_geometry({Base::b8, false}, 58).samples_per_line(), 4500u);
    EXPECT_EQ(film_geometry({Base::b16, false}, 60).samples_per_line(), 6000u);
    EXPECT_EQ(film_geometry({Base::b4, true}, 30).samples_per_line(), 4000u);
    EXPECT_EQ(film_geometry({Base::b8, true}, 58).samples_per_line(), 6000u);
    EXPECT_EQ(film_geometry({Base::b16, true}, 60).samples_per_line(), 8000u);
    // Control words: base4 0x62/0x63, base8 0x60, IR 0x160/0x161.
    EXPECT_EQ(pakon::scan::control_word(film_geometry({Base::b4, false}, 30), true), 0x63);
    EXPECT_EQ(pakon::scan::control_word(film_geometry({Base::b8, false}, 58), false), 0x60);
    EXPECT_EQ(pakon::scan::control_word(film_geometry({Base::b16, true}, 60), true), 0x161);
    EXPECT(pakon::scan::parse_mode("base8-ir").has_value());
    EXPECT(pakon::scan::parse_mode("base16-ir-off").has_value());
    EXPECT(!pakon::scan::parse_mode("base32").has_value());
}

int main() { return pakon::test::run_all(); }
