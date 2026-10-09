#pragma once

// Typed frames for the scan sequence: motor engage/run/stop, lamp and
// CCD configuration, scan-line triggers, service handling, and the
// acquisition arm pair.
//
// Every builder reproduces request forms observed verbatim in the
// capture corpus (alibosworth/pakon-captures, F-135+ serial 16402,
// ten OEM-driven sessions) and/or quoted in pakon-reference. Command
// NAMES are working labels, not OEM identifiers (command-reference.md's
// own marker). Two register names carry a reference-vs-capture-corpus
// divergence and are called out inline below (0x80, 0x81).
//
// SAFETY: these builders only ever target the documented controller
// addresses (protocol/addresses.hpp). Nothing here addresses the
// bootloader (0x22/0x26/0x42/0x46) or the shifted EEPROM bus
// (0xA2/0xA4); the PPB client's allow-list enforces that on the wire.

#include <array>
#include <cstdint>
#include <span>

#include "pakon/ppb/packet.hpp"
#include "pakon/protocol/addresses.hpp"
#include "pakon/protocol/commands.hpp"

namespace pakon::protocol::scan {

// --- Channel topology -------------------------------------------------
// Command frames ride bulk OUT 0x01 / bulk IN 0x81; pixel data rides
// bulk IN 0x86 (image/source.hpp). The capture corpus's bridge
// constants: EP_CMD_OUT, EP_CMD_IN, EP_IMG_IN = 0x01, 0x81, 0x86.
inline constexpr std::uint8_t kCommandOutEndpoint = 0x01;
inline constexpr std::uint8_t kCommandInEndpoint = 0x81;

// --- Motor (PICM) indexed writes --------------------------------------
//
// 0x82/0x84 writes carry a sub-register index as their first payload
// byte (capture corpus: `02 06 44 03 82 <sub> <lo> <hi>`). Sub-index
// meanings beyond "0 = the run/idle speed word" are NOT established by
// pakon-reference; the labels below are the capture corpus's own
// bridge labels (pakon-captures/bridge/ppb.py, transcribed from
// PakonKit — that table's own caveat: names from observed traffic, not
// OEM identifiers).
enum class SpeedSubRegister : std::uint8_t {
    run = 0x00,         // run/idle speed word; sub-0 carries run/stop state
    offset = 0x04,      // "offset"; the per-resolution EEPROM Offset word
                        // lands here in the captures
    offset_width = 0x05, // "offset+width"
    integration = 0x06,  // "integration"
    mux = 0x09,         // "mux"; the only 0x82 write seen mid-transport
    idx10 = 0x0A,       // semantics unresolved
    idx11 = 0x0B,       // semantics unresolved
};

enum class ConfigSubRegister : std::uint8_t {
    idx0 = 0x00,
    idx1 = 0x01,
    ad_gain_r = 0x02,     // A/D gain R
    ad_gain_g = 0x03,
    ad_gain_b = 0x04,
    offset_trim_r = 0x05, // offset trim R
    offset_trim_g = 0x06,
    offset_trim_b = 0x07,
};

inline ppb::Frame motor_speed(std::uint8_t address, SpeedSubRegister sub,
                              std::uint16_t value) {
    return ppb::make_write(address, to_byte(MotorCommand::set_motor_speed),
                           std::array<std::uint8_t, 3>{
                               static_cast<std::uint8_t>(sub),
                               static_cast<std::uint8_t>(value & 0xFF),
                               static_cast<std::uint8_t>((value >> 8) & 0xFF)});
}

inline ppb::Frame motor_speed(std::uint8_t address, std::uint8_t sub,
                              std::uint16_t value) {
    return ppb::make_write(address, to_byte(MotorCommand::set_motor_speed),
                           std::array<std::uint8_t, 3>{
                               sub, static_cast<std::uint8_t>(value & 0xFF),
                               static_cast<std::uint8_t>((value >> 8) & 0xFF)});
}

inline ppb::Frame motor_config(std::uint8_t address, ConfigSubRegister sub,
                               std::uint16_t value) {
    return ppb::make_write(address, to_byte(MotorCommand::set_motor_config),
                           std::array<std::uint8_t, 3>{
                               static_cast<std::uint8_t>(sub),
                               static_cast<std::uint8_t>(value & 0xFF),
                               static_cast<std::uint8_t>((value >> 8) & 0xFF)});
}

inline ppb::Frame motor_config(std::uint8_t address, std::uint8_t sub,
                               std::uint16_t value) {
    return ppb::make_write(address, to_byte(MotorCommand::set_motor_config),
                           std::array<std::uint8_t, 3>{
                               sub, static_cast<std::uint8_t>(value & 0xFF),
                               static_cast<std::uint8_t>((value >> 8) & 0xFF)});
}

// 0xA5, 2-byte payload, little-endian: the EEPROM MotorSpeed word for
// the unit (capture: base4 `7e 64` = 0x647E). Units not established.
inline ppb::Frame motor_calibration(std::uint8_t address, std::uint16_t word) {
    return ppb::make_write(address, to_byte(MotorCommand::set_motor_calibration),
                           std::array<std::uint8_t, 2>{
                               static_cast<std::uint8_t>(word & 0xFF),
                               static_cast<std::uint8_t>((word >> 8) & 0xFF)});
}

inline ppb::Frame engage(std::uint8_t address) {
    return ppb::make_cmd(address, to_byte(MotorCommand::engage_film_drive));
}

inline ppb::Frame stop_drive(std::uint8_t address) {
    return ppb::make_cmd(address, to_byte(MotorCommand::stop_film_drive));
}

inline ppb::Frame disengage(std::uint8_t address) {
    return ppb::make_cmd(address, to_byte(MotorCommand::disengage_film_drive));
}

inline ppb::Frame reset_motor(std::uint8_t address) {
    return ppb::make_cmd(address, to_byte(MotorCommand::reset_motor));
}

inline ppb::Frame init_motor(std::uint8_t address, std::uint8_t value = 0x01) {
    return ppb::make_write(address, to_byte(MotorCommand::init_motor),
                           std::array<std::uint8_t, 1>{value});
}

// --- Light (PICL) ------------------------------------------------------

// Lamp mask, register 0x80. REFERENCE-vs-CAPTURE DIVERGENCE:
// pakon-reference's command table calls 0x80 "SetCcdConfig … general
// CCD config", while the capture corpus's bridge decodes it as the
// lamp mask (bit0 = visible, bit1 = IR; PakonKit docs/PROTOCOL.md) and
// the captures themselves only ever write 00/01/02/03 there — exactly
// the four visible/IR combinations, with 01 before the calibration
// window and 00 at teardown ("lamp-off" in the bridge's narration).
// The lamp-mask reading is the one the payload values support.
struct LampMask {
    bool visible{false};
    bool ir{false};
};

inline ppb::Frame lamp_mask(std::uint8_t address, LampMask mask) {
    std::uint8_t bits = 0;
    if (mask.visible) {
        bits |= 0x01;
    }
    if (mask.ir) {
        bits |= 0x02;
    }
    return ppb::make_write(address, to_byte(LightCommand::set_ccd_config),
                           std::array<std::uint8_t, 1>{bits});
}

// LED current, register 0x81, 5-byte payload [B, IR, R, _, G].
// REFERENCE-vs-CAPTURE DIVERGENCE: the reference table calls 0x81
// "SetCcdGainOffset" ([INFERRED layout]); the capture corpus's bridge
// labels it "LED CURRENT" with the slot order above, and its LED
// ceiling clamp operates on exactly these writes. The clamp context
// supports the LED-current reading.
inline ppb::Frame led_current(std::uint8_t address,
                              const std::array<std::uint8_t, 5>& currents) {
    return ppb::make_write(address, to_byte(LightCommand::set_ccd_gain_offset),
                           currents);
}

inline ppb::Frame ccd_exposure_b(std::uint8_t address,
                                 const std::array<std::uint8_t, 4>& timing) {
    return ppb::make_write(address, to_byte(LightCommand::set_ccd_exposure_b), timing);
}

inline ppb::Frame ccd_exposure_g(std::uint8_t address,
                                 const std::array<std::uint8_t, 4>& timing) {
    return ppb::make_write(address, to_byte(LightCommand::set_ccd_exposure_g), timing);
}

inline ppb::Frame ccd_exposure_r(std::uint8_t address,
                                 const std::array<std::uint8_t, 4>& timing) {
    return ppb::make_write(address, to_byte(LightCommand::set_ccd_exposure_r), timing);
}

// Register 0x8F. The reference table lists a 2-byte payload; the
// captures write 4 bytes there (`8f e8 ff 18 00`, `8f e0 ff 20 00` …).
// Replay capture-exact lengths, not the table's.
inline ppb::Frame light_config(std::uint8_t address,
                               const std::array<std::uint8_t, 4>& config) {
    return ppb::make_write(address, to_byte(LightCommand::set_light_config), config);
}

inline ppb::Frame light_power(std::uint8_t address,
                              const std::array<std::uint8_t, 2>& power) {
    return ppb::make_write(address, to_byte(LightCommand::set_light_power), power);
}

inline ppb::Frame color_matrix(std::uint8_t address,
                               const std::array<std::uint8_t, 12>& matrix) {
    return ppb::make_write(address, to_byte(LightCommand::set_color_matrix), matrix);
}

inline ppb::Frame enable_scan(std::uint8_t address, std::uint8_t value) {
    return ppb::make_write(address, to_byte(LightCommand::enable_scan),
                           std::array<std::uint8_t, 1>{value});
}

// TEC: replay OEM values verbatim only (commands.hpp TEC caution).
inline ppb::Frame tec_setpoint(std::uint8_t address, std::uint8_t value) {
    return ppb::make_write(address, to_byte(LightCommand::set_tec_1),
                           std::array<std::uint8_t, 1>{value});
}

inline ppb::Frame tec_enable(std::uint8_t address, std::uint8_t value) {
    return ppb::make_write(address, to_byte(LightCommand::set_tec_2),
                           std::array<std::uint8_t, 1>{value});
}

// Scan-line trigger, register 0x91, 3-byte payload:
// [le16 ScanLineParams value][0x01]. The third byte is 0x01 in all 12
// captured triggers (six configurations, two triggers each) — a
// capture-verified constant, semantics unknown. The trigger resets the
// scanner's line counter / arms the window (capture corpus bridge:
// "TRIGGER (line ctr reset, EP6 starts)"); issued once per window.
inline ppb::Frame scan_trigger(std::uint8_t address, ScanLineParams params,
                               std::uint8_t tail = 0x01) {
    const auto value = static_cast<std::uint16_t>(params);
    return ppb::make_write(address, to_byte(LightCommand::set_scan_line_params),
                           std::array<std::uint8_t, 3>{
                               static_cast<std::uint8_t>(value & 0xFF),
                               static_cast<std::uint8_t>((value >> 8) & 0xFF), tail});
}

// HostReady: WRITE HOST reg 0x84, 1-byte payload 02. Pairs with
// AcquireLine ("the ARM pair"): HostReady immediately precedes every
// AcquireLine in all captures [capture-evidenced; the reference marks
// the pairing INFERRED].
inline ppb::Frame host_ready(std::uint8_t address) {
    return ppb::make_write(address, to_byte(HostCommand::host_ready),
                           std::array<std::uint8_t, 1>{0x02});
}

inline ppb::Frame acquire_line(std::uint8_t address) {
    return ppb::make_cmd(address, to_byte(LightCommand::acquire_line));
}

inline ppb::Frame end_acquisition(std::uint8_t address) {
    return ppb::make_cmd(address, to_byte(LightCommand::end_acquisition));
}

// --- Service handling --------------------------------------------------

inline ppb::Frame read_service_status(std::uint8_t address) {
    return ppb::make_read(address, 1, to_byte(LightCommand::read_service_status));
}

// Service ack: WRITE PICL reg 0x06, payload 00 02 (capture-exact).
inline ppb::Frame service_ack(std::uint8_t address) {
    return ppb::make_write(address, to_byte(LightCommand::write_service_ack),
                           std::array<std::uint8_t, 2>{0x00, 0x02});
}

inline ppb::Frame read_sensor_data(std::uint8_t address) {
    return ppb::make_read(address, 30, to_byte(LightCommand::read_sensor_data));
}

inline ppb::Frame read_ccd_status(std::uint8_t address) {
    return ppb::make_read(address, 1, to_byte(LightCommand::read_ccd_status));
}

inline ppb::Frame read_light_status(std::uint8_t address) {
    return ppb::make_read(address, 2, to_byte(LightCommand::read_light_status));
}

inline ppb::Frame read_temperature(std::uint8_t address) {
    return ppb::make_read(address, 4, to_byte(LightCommand::read_temperature));
}

// True when a READ reg 0x02 reply reports "wants service" (payload
// byte 0x02; commands.hpp; capture corpus: idle replies carry payload
// 00 with flags 0x08, service replies carry payload 02 with flags
// 0x88 — the 0x80 event bit OR-ed on).
inline bool service_requested(const ppb::Reply& reply) {
    return ppb::is_read_success(reply) && reply.payload.size() >= 1 &&
           reply.payload[0] == 0x02;
}

} // namespace pakon::protocol::scan
