#pragma once

// PPB command bytes per controller.
//
// Source: pakon-reference/docs/command-reference.md — command bytes,
// types, payload sizes and sequencing are [DOCUMENTED] from a PPB debug
// trace of a real scanner and the OEM driver's API strings. Command
// NAMES are largely [INFERRED] working labels, not OEM identifiers.
//
// Command bytes are SHARED numbers interpreted per destination address:
// e.g. 0x82 is SetColorMatrix for the light controller and SetMotorSpeed
// for the motor controller. The address disambiguates them — hence
// separate enums per controller namespace.

#include <cstdint>

namespace pakon::protocol {

enum class LightCommand : std::uint8_t {
    // service/status exchange [CONFIRMED live], dx-barcode.md
    read_service_status = 0x02, // read register 0x02; bit 0x02 = wants service
    write_service_ack = 0x06,   // payload 00 02 (acknowledge service)

    set_ccd_config = 0x80,        // WRITE, 1 byte; general CCD config, sent repeatedly during init
    set_ccd_gain_offset = 0x81,   // WRITE, 5 bytes; gain/offset [INFERRED layout]
    set_color_matrix = 0x82,      // WRITE, 12 bytes; colour-correction matrix [INFERRED]
    read_ccd_status = 0x83,       // READ, 1 byte; ready/busy/error flags
    read_light_status = 0x84,     // READ, 2 bytes; lamp level + status
    set_light_power = 0x87,       // WRITE, 2 bytes; light source power/PWM level
    read_temperature = 0x88,      // READ, 4 bytes; temperature sensors
    enable_scan = 0x89,           // WRITE, 1 byte; arm the CCD for scanning
    acquire_line = 0x8A,          // CMD, no payload; trigger one CCD line acquisition
    set_ccd_exposure_b = 0x8B,    // WRITE, 4 bytes; per-channel exposure timing
    set_ccd_exposure_g = 0x8C,    // WRITE, 4 bytes
    set_ccd_exposure_r = 0x8D,    // WRITE, 4 bytes
    set_light_config = 0x8F,      // WRITE, 2 bytes; light source configuration
    read_sensor_data = 0x90,      // READ, 30 bytes; DX/position entries; only after service flag [CONFIRMED]
    set_scan_line_params = 0x91,  // WRITE, 3 bytes; scan-line params; resets position counter [CONFIRMED]
    end_acquisition = 0x92,       // CMD; closes the DX decode window [CONFIRMED]
    read_dx_sensors = 0x93,       // READ, 4 bytes; one per DX photodetector [CONFIRMED live]
    set_tec_1 = 0xD0,             // WRITE, 1 byte; TEC setpoint — F-135+ ONLY, see TEC note
    set_tec_2 = 0xD1,             // WRITE, 1 byte; TEC enable/mode — F-135+ ONLY
};

// TEC caution (command-reference.md): 0xD0/0xD1 byte semantics are not
// understood; thermoelectric cooling drives a real thermal load. Until the
// semantics are understood, an implementation should replay the OEM's exact
// TEC values in the OEM's sequence, and NOT probe or sweep them.

enum class MotorCommand : std::uint8_t {
    reset_motor = 0x00,           // CMD; reset controller to initial state
    set_motor_speed = 0x82,       // WRITE, 3 bytes; speed (16-bit, tenths of mm/s) + mode; indexed by sub-register
    set_motor_config = 0x84,      // WRITE, 3 bytes; accel/decel/step-mode profile
    init_motor = 0x97,            // WRITE, 1 byte; initialise motor controller
    engage_film_drive = 0xA0,     // CMD; start pulling film
    stop_film_drive = 0xA1,       // CMD; immediate stop
    disengage_film_drive = 0xA2,  // CMD; release the drive
    set_motor_calibration = 0xA5, // WRITE, 2 bytes; home/adjust parameters, before engage
};

enum class HostCommand : std::uint8_t {
    host_ready = 0x84,   // WRITE, 1 byte; readiness for next scan line (immediately precedes each AcquireLine)
    host_reset = 0x85,   // CMD; reset the bridge (sent in clusters)
    host_set_mode = 0x8F, // WRITE, 1 byte; set bridge operating mode
};

// Expected payload sizes from the command-reference tables; used by
// builders/tests to catch malformed frames. READ sizes are the number of
// data bytes the device returns.
enum class LightPayloadSize : std::uint8_t {
    set_ccd_config = 1, set_ccd_gain_offset = 5, set_color_matrix = 12,
    read_ccd_status = 1, read_light_status = 2, set_light_power = 2,
    read_temperature = 4, enable_scan = 1, acquire_line = 0,
    set_ccd_exposure = 4, set_light_config = 2, read_sensor_data = 30,
    set_scan_line_params = 3, end_acquisition = 0, read_dx_sensors = 4,
    set_tec = 1, read_service_status = 0, write_service_ack = 2,
};

enum class MotorPayloadSize : std::uint8_t {
    reset_motor = 0, set_motor_speed = 3, set_motor_config = 3,
    init_motor = 1, engage_film_drive = 0, stop_film_drive = 0,
    disengage_film_drive = 0, set_motor_calibration = 2,
};

// Names back to bytes (for logging).
constexpr std::uint8_t to_byte(LightCommand c) noexcept {
    return static_cast<std::uint8_t>(c);
}
constexpr std::uint8_t to_byte(MotorCommand c) noexcept {
    return static_cast<std::uint8_t>(c);
}
constexpr std::uint8_t to_byte(HostCommand c) noexcept {
    return static_cast<std::uint8_t>(c);
}

// The scan-line trigger value of SetScanLineParams (0x91) names the scan
// resolution + Digital ICE state; issued twice per scan (pre-scan
// calibration and transport pass) with the same value.
// [CONFIRMED live, six configurations], dx-barcode.md:
enum class ScanLineParams : std::uint16_t {
    base4_ir_off = 0x0107,
    base4_ir_on = 0x00C5,
    base8_ir_off = 0x0075,
    base8_ir_on = 0x004D,
    base16_ir_off = 0x003C,
    base16_ir_on = 0x0031,
};

} // namespace pakon::protocol
