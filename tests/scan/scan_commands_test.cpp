// Byte-exact checks for the typed scan-sequence builders: every frame
// is compared against request hex recorded verbatim in the capture
// corpus (alibosworth/pakon-captures, F-135+ serial 16402; the
// six-configuration trigger values come from the six session files).

#include <format>
#include <string>
#include <vector>

#include "pakon/protocol/scan_commands.hpp"
#include "pakon/scan/plan.hpp"
#include "support/replay_transport.hpp"
#include "support/test_harness.hpp"

namespace {

using pakon::protocol::ScanLineParams;
using namespace pakon::protocol::scan;

std::string hex(const pakon::ppb::Frame& frame) {
    auto bytes = frame.serialize();
    if (!bytes) {
        return "<serialize failed>";
    }
    std::string out;
    for (const auto b : *bytes) {
        out += std::format("{:02x}", b);
    }
    return out;
}

constexpr std::uint8_t kPicl = 0x40;
constexpr std::uint8_t kPicm = 0x44;

} // namespace

PAKON_TEST(motor_frames_match_captures) {
    EXPECT_EQ(hex(motor_speed(kPicm, SpeedSubRegister::run, 0x0063)), "0206440382006300");
    EXPECT_EQ(hex(motor_speed(kPicm, SpeedSubRegister::run, 0x0062)), "0206440382006200");
    EXPECT_EQ(hex(motor_speed(kPicm, SpeedSubRegister::mux, 0x0295)), "0206440382099502");
    EXPECT_EQ(hex(motor_speed(kPicm, SpeedSubRegister::integration, 0x0FFD)),
              "020644038206fd0f");
    EXPECT_EQ(hex(motor_speed(kPicm, 0x0B, 0x0000)), "02064403820b0000");
    EXPECT_EQ(hex(motor_config(kPicm, ConfigSubRegister::ad_gain_r, 0x000D)),
              "0206440384020d00");
    EXPECT_EQ(hex(motor_config(kPicm, ConfigSubRegister::offset_trim_g, 0x0128)),
              "0206440384062801");
    EXPECT_EQ(hex(motor_calibration(kPicm, 0x647E)), "02054402a57e64");
    EXPECT_EQ(hex(engage(kPicm)), "04034400a0");
    EXPECT_EQ(hex(stop_drive(kPicm)), "04034400a1");
    EXPECT_EQ(hex(disengage(kPicm)), "04034400a2");
    EXPECT_EQ(hex(reset_motor(kPicm)), "0403440000");
    EXPECT_EQ(hex(init_motor(kPicm)), "020444019701");
}

PAKON_TEST(lamp_mask_matches_capture_values) {
    // Payloads 00/01/02/03 are the only values ever written to 0x80 in
    // the corpus: off / visible / IR / visible+IR.
    EXPECT_EQ(hex(lamp_mask(kPicl, {false, false})), "020440018000");
    EXPECT_EQ(hex(lamp_mask(kPicl, {true, false})), "020440018001");
    EXPECT_EQ(hex(lamp_mask(kPicl, {false, true})), "020440018002");
    EXPECT_EQ(hex(lamp_mask(kPicl, {true, true})), "020440018003");
}

PAKON_TEST(light_frames_match_captures) {
    EXPECT_EQ(hex(led_current(kPicl, {0x06, 0x00, 0x03, 0x00, 0x09})),
              "02084005810600030009");
    EXPECT_EQ(hex(light_config(kPicl, {0xE8, 0xFF, 0x18, 0x00})), "020740048fe8ff1800");
    EXPECT_EQ(hex(ccd_exposure_g(kPicl, {0xE0, 0xFF, 0x20, 0x00})), "020740048ce0ff2000");
    EXPECT_EQ(hex(ccd_exposure_b(kPicl, {0xF0, 0x00, 0x20, 0x03})), "020740048bf0002003");
    EXPECT_EQ(hex(ccd_exposure_r(kPicl, {0xA0, 0x00, 0x70, 0x03})), "020740048da0007003");
    EXPECT_EQ(hex(light_power(kPicl, {0x00, 0x00})), "02054002870000");
    EXPECT_EQ(hex(color_matrix(kPicl, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                       0x00, 0x00, 0xD6, 0x03})),
              "020f400c8200000000000000000000d603");
    EXPECT_EQ(hex(enable_scan(kPicl, 0x00)), "020440018900");
    EXPECT_EQ(hex(tec_setpoint(kPicl, 0x00)), "02044001d000");
    EXPECT_EQ(hex(tec_enable(kPicl, 0x01)), "02044001d101");
}

PAKON_TEST(scan_triggers_match_all_six_configurations) {
    // One trigger pair per session file; payloads verified against the
    // recorded hex of all six configurations.
    EXPECT_EQ(hex(scan_trigger(kPicl, ScanLineParams::base4_ir_off)), "0206400391070101");
    EXPECT_EQ(hex(scan_trigger(kPicl, ScanLineParams::base4_ir_on)), "0206400391c50001");
    EXPECT_EQ(hex(scan_trigger(kPicl, ScanLineParams::base8_ir_off)), "0206400391750001");
    EXPECT_EQ(hex(scan_trigger(kPicl, ScanLineParams::base8_ir_on)), "02064003914d0001");
    EXPECT_EQ(hex(scan_trigger(kPicl, ScanLineParams::base16_ir_off)), "02064003913c0001");
    EXPECT_EQ(hex(scan_trigger(kPicl, ScanLineParams::base16_ir_on)), "0206400391310001");
}

PAKON_TEST(arm_pair_and_acquisition_frames_match_captures) {
    EXPECT_EQ(hex(host_ready(0x10)), "020410018402");
    EXPECT_EQ(hex(acquire_line(kPicl)), "040340008a");
    EXPECT_EQ(hex(end_acquisition(kPicl)), "0403400092");
}

PAKON_TEST(service_frames_match_captures) {
    EXPECT_EQ(hex(service_ack(kPicl)), "02054002060002");
    EXPECT_EQ(hex(read_service_status(kPicl)), "0103400102");
    EXPECT_EQ(hex(read_sensor_data(kPicl)), "0103401e90");
    EXPECT_EQ(hex(read_ccd_status(kPicl)), "0103400183");
    EXPECT_EQ(hex(read_light_status(kPicl)), "0103400284");
    EXPECT_EQ(hex(read_temperature(kPicl)), "0103400488");
}

PAKON_TEST(service_requested_reads_the_payload_byte) {
    const auto wanted = pakon::ppb::parse_reply(
        pakon::test::ReplayUsbTransport::from_hex("0103408802"), pakon::ppb::FrameType::read);
    EXPECT(wanted.has_value());
    EXPECT_EQ(wanted.has_value() && service_requested(*wanted), true);

    const auto idle = pakon::ppb::parse_reply(
        pakon::test::ReplayUsbTransport::from_hex("0103400800"), pakon::ppb::FrameType::read);
    EXPECT(idle.has_value());
    EXPECT_EQ(idle.has_value() && service_requested(*idle), false);
}

PAKON_TEST(run_and_idle_speeds_keep_the_captured_parity) {
    // The run word is the idle word with bit0 set in all captures
    // (base4 98↔99 etc.). The plans derive it; pin the relation.
    EXPECT_EQ(pakon::scan::run_speed_for(0x0062), 0x0063);
    EXPECT_EQ(pakon::scan::run_speed_for(0x0060), 0x0061);
    EXPECT_EQ(pakon::scan::run_speed_for(0x0064), 0x0065);
}

int main() { return pakon::test::run_all(); }
