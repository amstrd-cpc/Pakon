#include "pakon/scan/plan.hpp"

namespace pakon::scan {

namespace {

using protocol::ControllerAddresses;
using protocol::scan::acquire_line;
using protocol::scan::ConfigSubRegister;
using protocol::scan::SpeedSubRegister;
using protocol::scan::host_ready;

void arm_pair(const ControllerAddresses& addresses, std::vector<ppb::Frame>& plan) {
    // The ARM pair: HostReady immediately precedes every AcquireLine
    // in all captures.
    plan.push_back(host_ready(protocol::kAddrHost));
    plan.push_back(acquire_line(addresses.light));
}

} // namespace

std::vector<ppb::Frame> init_plan(const ControllerAddresses& addresses,
                                  const ScanPlanParameters& params) {
    namespace ps = protocol::scan;
    std::vector<ppb::Frame> plan;
    plan.reserve(31);

    // Capture t 0.951–0.974: the OEM opens the block with an ARM pair.
    plan.push_back(host_ready(protocol::kAddrHost));
    plan.push_back(acquire_line(addresses.light));

    // Capture t 0.976–1.004: light/CCD configuration, verbatim OEM
    // values (units/semantics unresolved; replayed as recorded).
    plan.push_back(ps::light_config(addresses.light, {0xE8, 0xFF, 0x18, 0x00}));
    plan.push_back(ps::ccd_exposure_g(addresses.light, {0xE0, 0xFF, 0x20, 0x00}));
    plan.push_back(ps::ccd_exposure_b(addresses.light, {0xF0, 0x00, 0x20, 0x03}));
    plan.push_back(ps::ccd_exposure_r(addresses.light, {0xA0, 0x00, 0x70, 0x03}));
    if (params.has_tec) {
        // F-135+ TEC: OEM values replayed verbatim only (TEC caution in
        // protocol/commands.hpp).
        plan.push_back(ps::tec_setpoint(addresses.light, 0x00));
        plan.push_back(ps::tec_enable(addresses.light, 0x01));
    }
    plan.push_back(ps::light_power(addresses.light, {0x00, 0x00}));
    plan.push_back(ps::lamp_mask(addresses.light, {true, false}));
    plan.push_back(ps::color_matrix(
        addresses.light, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                          0x00, 0xD6, 0x03}));
    plan.push_back(ps::lamp_mask(addresses.light, {false, false}));
    plan.push_back(ps::enable_scan(addresses.light, 0x00));

    // Capture t 1.007–1.037: motor speed/config block.
    plan.push_back(ps::motor_speed(addresses.motor, SpeedSubRegister::integration, 0x0FFD));
    plan.push_back(ps::motor_speed(addresses.motor, SpeedSubRegister::run, 0x0060));
    plan.push_back(ps::motor_speed(addresses.motor, 0x0B, 0x0000)); // semantics unresolved
    plan.push_back(ps::motor_speed(addresses.motor, SpeedSubRegister::offset, 0x003E));
    plan.push_back(ps::motor_speed(addresses.motor, SpeedSubRegister::offset_width, 0x080E));
    plan.push_back(ps::motor_speed(addresses.motor, 0x01, 0x0000)); // semantics unresolved
    plan.push_back(ps::motor_speed(addresses.motor, 0x02, 0x0000));
    plan.push_back(ps::motor_speed(addresses.motor, 0x03, 0x0000));
    plan.push_back(ps::motor_speed(addresses.motor, SpeedSubRegister::idx10, 0x0400));
    plan.push_back(ps::motor_config(addresses.motor, ConfigSubRegister::idx0, 0x0078));
    plan.push_back(ps::motor_config(addresses.motor, ConfigSubRegister::idx1, 0x0080));

    // Capture t 1.040–1.067: disengage, telemetry reads, mux writes.
    plan.push_back(ps::disengage(addresses.motor));
    plan.push_back(ps::read_sensor_data(addresses.light));
    plan.push_back(ps::read_ccd_status(addresses.light));
    plan.push_back(ps::read_light_status(addresses.light));
    plan.push_back(ps::read_temperature(addresses.light));
    plan.push_back(ps::motor_speed(addresses.motor, SpeedSubRegister::mux, 0x0014));
    plan.push_back(ps::motor_speed(addresses.motor, SpeedSubRegister::mux, 0x0017));
    return plan;
}

std::vector<ppb::Frame> service_plan(const ControllerAddresses& addresses) {
    namespace ps = protocol::scan;
    // Capture t 15.360–15.373.
    return {ps::service_ack(addresses.light),
            ps::read_sensor_data(addresses.light),
            ps::read_light_status(addresses.light),
            ps::read_temperature(addresses.light)};
}

std::vector<ppb::Frame> calibration_plan(const ControllerAddresses& addresses,
                                         const ScanPlanParameters& params) {
    namespace ps = protocol::scan;
    std::vector<ppb::Frame> plan;
    plan.reserve(45);

    // Capture t 8.715 + 8.887: the window trigger and the mux write
    // that follows it (replayed at the window head — see plan.hpp).
    plan.push_back(ps::scan_trigger(addresses.light, params.line_params));
    plan.push_back(ps::motor_speed(addresses.motor, SpeedSubRegister::mux, 0x0313));

    // Capture t 15.589–15.700: pre-window ramp.
    plan.push_back(ps::motor_speed(addresses.motor, SpeedSubRegister::integration, 0x0753));
    plan.push_back(ps::motor_speed(addresses.motor, SpeedSubRegister::run, params.idle_speed));
    plan.push_back(ps::motor_speed(addresses.motor, SpeedSubRegister::offset, 0x0003));
    plan.push_back(ps::motor_speed(addresses.motor, SpeedSubRegister::offset_width, 0x0406));
    arm_pair(addresses, plan);
    plan.push_back(ps::motor_speed(addresses.motor, SpeedSubRegister::run,
                                   run_speed_for(params.idle_speed)));
    plan.push_back(ps::motor_config(addresses.motor, ConfigSubRegister::ad_gain_r, 0x000D));
    plan.push_back(ps::motor_config(addresses.motor, ConfigSubRegister::ad_gain_g, 0x000D));
    plan.push_back(ps::motor_config(addresses.motor, ConfigSubRegister::ad_gain_b, 0x000D));
    plan.push_back(ps::motor_config(addresses.motor, ConfigSubRegister::offset_trim_r, 0x000A));
    plan.push_back(ps::motor_config(addresses.motor, ConfigSubRegister::offset_trim_g, 0x000A));
    plan.push_back(ps::motor_config(addresses.motor, ConfigSubRegister::offset_trim_b, 0x000A));
    arm_pair(addresses, plan);

    // Capture t 15.776–16.338: offset-trim ramp with interleaved ARM
    // pairs (replayed verbatim; the ramp's purpose is unresolved).
    plan.push_back(ps::motor_config(addresses.motor, ConfigSubRegister::offset_trim_r, 0x0133));
    plan.push_back(ps::motor_config(addresses.motor, ConfigSubRegister::offset_trim_g, 0x0130));
    plan.push_back(ps::motor_config(addresses.motor, ConfigSubRegister::offset_trim_b, 0x0134));
    arm_pair(addresses, plan);
    plan.push_back(ps::motor_config(addresses.motor, ConfigSubRegister::offset_trim_r, 0x012B));
    plan.push_back(ps::motor_config(addresses.motor, ConfigSubRegister::offset_trim_g, 0x0128));
    plan.push_back(ps::motor_config(addresses.motor, ConfigSubRegister::offset_trim_b, 0x012C));
    arm_pair(addresses, plan);
    plan.push_back(ps::motor_config(addresses.motor, ConfigSubRegister::offset_trim_r, 0x0124));
    plan.push_back(ps::motor_config(addresses.motor, ConfigSubRegister::offset_trim_g, 0x0121));
    plan.push_back(ps::motor_config(addresses.motor, ConfigSubRegister::offset_trim_b, 0x0124));
    arm_pair(addresses, plan);
    plan.push_back(ps::motor_config(addresses.motor, ConfigSubRegister::offset_trim_g, 0x0122));
    plan.push_back(ps::motor_config(addresses.motor, ConfigSubRegister::offset_trim_b, 0x0125));
    arm_pair(addresses, plan);
    arm_pair(addresses, plan);
    arm_pair(addresses, plan);

    // Capture t 16.480–16.486: lamp on with LED currents and the
    // window's colour matrix.
    plan.push_back(ps::lamp_mask(addresses.light, {true, false}));
    plan.push_back(ps::led_current(addresses.light, {0x06, 0x00, 0x03, 0x00, 0x09}));
    plan.push_back(ps::color_matrix(
        addresses.light, {0x47, 0x00, 0x00, 0x00, 0xF1, 0x00, 0x00, 0x00, 0xA9,
                          0x00, 0xC2, 0x01}));
    // Capture t 18.505–18.995: mid-window ARM pairs with matrix
    // rewrites.
    arm_pair(addresses, plan);
    plan.push_back(ps::color_matrix(
        addresses.light, {0x47, 0x00, 0x00, 0x00, 0xF1, 0x00, 0x00, 0x00, 0xA9,
                          0x00, 0xC2, 0x01}));
    arm_pair(addresses, plan);
    plan.push_back(ps::color_matrix(
        addresses.light, {0x47, 0x00, 0x00, 0x00, 0xF1, 0x00, 0x00, 0x00, 0xA9,
                          0x00, 0xC2, 0x01}));
    arm_pair(addresses, plan);
    arm_pair(addresses, plan);
    return plan;
}

std::vector<ppb::Frame> transport_plan(const ControllerAddresses& addresses,
                                       const ScanPlanParameters& params) {
    namespace ps = protocol::scan;
    std::vector<ppb::Frame> plan;
    plan.reserve(10);

    // Capture t 19.159–19.237: window close / pre-engage.
    plan.push_back(ps::motor_speed(addresses.motor, SpeedSubRegister::run, params.idle_speed));
    plan.push_back(ps::motor_speed(addresses.motor, SpeedSubRegister::offset, 0x001E));
    plan.push_back(ps::color_matrix(
        addresses.light, {0x70, 0x01, 0x00, 0x00, 0x4F, 0x01, 0x00, 0x00, 0xA8,
                          0x01, 0xC2, 0x01}));
    plan.push_back(ps::motor_speed(addresses.motor, SpeedSubRegister::mux, 0x0017));

    // Capture t 22.529–22.687: engage sequence (film-transport.md
    // "Engage, run, stop", with the speed register carrying run/stop).
    plan.push_back(ps::motor_calibration(addresses.motor, params.motor_rate_word));
    plan.push_back(ps::engage(addresses.motor));
    plan.push_back(ps::motor_speed(addresses.motor, SpeedSubRegister::run,
                                   run_speed_for(params.idle_speed)));
    plan.push_back(ps::scan_trigger(addresses.light, params.line_params));
    plan.push_back(ps::motor_speed(addresses.motor, SpeedSubRegister::mux, 0x0217));
    plan.push_back(ps::motor_speed(addresses.motor, SpeedSubRegister::mux, 0x0295));
    return plan;
}

std::vector<ppb::Frame> teardown_plan(const ControllerAddresses& addresses,
                                      const ScanPlanParameters& params) {
    namespace ps = protocol::scan;
    // Capture t 32.385–32.460.
    return {ps::motor_speed(addresses.motor, SpeedSubRegister::run, params.idle_speed),
            ps::lamp_mask(addresses.light, {false, false}),
            host_ready(protocol::kAddrHost),
            acquire_line(addresses.light),
            ps::end_acquisition(addresses.light),
            ps::disengage(addresses.motor)};
}

} // namespace pakon::scan
