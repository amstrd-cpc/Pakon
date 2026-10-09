#pragma once

// Phase command plans for one scan session, replayed byte-exact from
// the capture corpus (alibosworth/pakon-captures, F-135+ serial 16402,
// session base4; see tests/support/scan_capture_fixture.hpp, generated
// by tools/gen_scan_fixture.py).
//
// Every frame below appears in that session's recorded order. The
// plans omit OEM traffic whose semantics are unresolved (see the
// per-phase comments) — never new or invented commands, and never a
// byte the captures did not show. The runner-replay test checks each
// sent frame as an ordered subsequence of the recorded session.

#include <cstdint>
#include <vector>

#include "pakon/ppb/packet.hpp"
#include "pakon/protocol/addresses.hpp"
#include "pakon/protocol/scan_commands.hpp"

namespace pakon::scan {

// Values for the captured base4 configuration (4exp neg, IR off). The
// per-resolution 0x91 trigger value and the per-unit 0xA5 MotorSpeed
// word are the only fields that vary across the six captured
// configurations; everything else recorded here is session-shape.
struct ScanPlanParameters {
    protocol::ScanLineParams line_params{protocol::ScanLineParams::base4_ir_off};
    // 0xA5 MotorSpeed word read from the unit EEPROM (capture: base4
    // `7e 64` = 0x647E; units not established).
    std::uint16_t motor_rate_word{0x647E};
    // Sub-0 idle speed word (capture: 0x0062 = 98). The run word is
    // always idle|1 in the captures (98↔99, 4/4 configurations), so it
    // is derived, not stored.
    std::uint16_t idle_speed{0x0062};
    // F-135+ only: the init block carries 0xD0/0xD1 TEC writes.
    bool has_tec{true};
};

constexpr std::uint16_t run_speed_for(std::uint16_t idle_speed) {
    return static_cast<std::uint16_t>(idle_speed | 1);
}

// idle→initialising block (capture t 0.951–1.067): one ARM pair, the
// light/CCD/TEC configuration, the motor speed/config block, a
// disengage, four telemetry reads, and two mux writes. The capture's
// 8.668 s block (periodic sensor/telemetry reads plus an ARM pair) is
// the OEM's idle polling cadence and is not replayed.
std::vector<ppb::Frame>
init_plan(const protocol::ControllerAddresses& addresses, const ScanPlanParameters& params);

// Service handling (capture t 15.360–15.373): the 00 02 ack plus the
// sensor/telemetry reads that follow it.
std::vector<ppb::Frame> service_plan(const protocol::ControllerAddresses& addresses);

// Calibration window W1. Capture order: trigger1 was sent during the
// idle phase (t 8.715, before the service event in this session — in
// three of the six captured configurations it follows the service
// event instead, so the trigger's position relative to service is not
// fixed; replayed here at the head of the window it arms), then the
// pre-window motor/CCD ramp (15.589–16.486) and the mid-window
// ARM/matrix rewrites (18.505–18.995). The trailing window-close
// writes (19.159 on) belong to transport preparation.
std::vector<ppb::Frame>
calibration_plan(const protocol::ControllerAddresses& addresses, const ScanPlanParameters& params);

// Between the windows and up to the film trigger (capture 19.159–
// 22.687): pre-run idle restore, per-resolution Offset word, second
// colour matrix, mux write, 0xA5 rate, 0xA0 engage, run speed, the
// film-pass trigger, and the two post-trigger mux writes. The mux
// (0x82 sub-9) values are opaque capture replays — their semantics are
// unresolved (scan audit, 2026-10).
std::vector<ppb::Frame>
transport_plan(const protocol::ControllerAddresses& addresses, const ScanPlanParameters& params);

// Capture 32.385–32.460: motor stop (sub-0 idle restore — run/stop
// state lives in the speed register, film-transport.md), lamp off, the
// final ARM pair, EndAcquisition (0x92), DisengageFilmDrive (0xA2).
// The captures also show mux writes after disengage (32.476, 36.805);
// they are omitted as unresolved.
std::vector<ppb::Frame>
teardown_plan(const protocol::ControllerAddresses& addresses, const ScanPlanParameters& params);

} // namespace pakon::scan
