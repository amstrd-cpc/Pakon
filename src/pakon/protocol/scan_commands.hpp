#pragma once

// Typed frames for the scan path, named after the OEM driver functions
// that send them (docs/OEM_RE.md §3 — register map recovered from
// TLB.dll, every builder byte-checked against the capture corpus in
// tests/scan/scan_commands_test.cpp).
//
// Several older working labels were wrong and are gone: PICM 0x82 sub 0
// is the CCD FPGA control register (not a motor speed; bit0 gates the
// image stream), 0x91/0x92 are DX start/stop (not a scan trigger /
// EndAcquisition), the "ARM pair" is bDrvResetFifos, PICM 0x82 sub 9 is
// the front-panel LED register (not a "mux"), PICL 0x82 holds the LED
// on-times (not a colour matrix).
//
// SAFETY: builders only take the documented controller addresses; the
// PPB client's allow-list refuses the bootloader (0x22/0x26/0x42/0x46)
// and EEPROM bus (0xA2/0xA4) addresses on the wire. TEC registers can
// only carry the two OEM literals (tec_init()). LED currents are clamped
// to the firmware ceilings by led_currents() itself.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "pakon/ppb/packet.hpp"
#include "pakon/protocol/addresses.hpp"

namespace pakon::protocol::scan {

inline constexpr std::uint8_t kImageEndpoint = 0x86;

// --- HOST / FIFO -------------------------------------------------------

// bDrvResetFifos (TLB@0x1000a730): HOST 0x84 = 02, then PICL CMD 0x8A.
inline std::array<ppb::Frame, 2> reset_fifos(const ControllerAddresses& a) {
    return {ppb::make_write(kAddrHost, 0x84, std::array<std::uint8_t, 1>{0x02}),
            ppb::make_cmd(a.light, 0x8A)};
}

// --- PICL: lamp --------------------------------------------------------

inline ppb::Frame lamp_mask(std::uint8_t light, bool visible, bool ir) {
    const auto bits = static_cast<std::uint8_t>((visible ? 1 : 0) | (ir ? 2 : 0));
    return ppb::make_write(light, 0x80, std::array<std::uint8_t, 1>{bits});
}

struct LedCurrents {
    std::uint8_t r{0};
    std::uint8_t g{0};
    std::uint8_t b{0};
    std::uint8_t ir{0};
};

// The firmware's own ceilings (TLB@0x100203c0, identical to the bridge's
// LED_CEILINGS): by board (PICM address) and IR state. An unknown board
// gets the strictest value of every column.
inline LedCurrents led_ceiling(std::uint8_t motor_address, bool ir_lit) {
    if (motor_address == kAddrPicmPlus) {
        return ir_lit ? LedCurrents{8, 24, 24, 8} : LedCurrents{4, 20, 20, 0};
    }
    if (motor_address == kAddrPicm) {
        return ir_lit ? LedCurrents{8, 8, 8, 8} : LedCurrents{6, 8, 8, 0};
    }
    return LedCurrents{4, 8, 8, 0};
}

inline LedCurrents clamp_currents(LedCurrents c, LedCurrents ceiling) {
    return {std::min(c.r, ceiling.r), std::min(c.g, ceiling.g), std::min(c.b, ceiling.b),
            std::min(c.ir, ceiling.ir)};
}

// Register 0x81, payload [B, IR, R, 0, G]; always clamped.
inline ppb::Frame led_currents(std::uint8_t light, LedCurrents c, LedCurrents ceiling) {
    c = clamp_currents(c, ceiling);
    return ppb::make_write(light, 0x81, std::array<std::uint8_t, 5>{c.b, c.ir, c.r, 0, c.g});
}

struct LedOnTimes {
    std::uint16_t r{0};
    std::uint16_t g{0};
    std::uint16_t b{0};
    std::uint16_t ir{0};
    std::uint16_t base{0};
};

// Register 0x82 (PICL), six u16 [B, IR, R, 0, G, base] (TLB@0x1002c5f0).
inline ppb::Frame led_on_times(std::uint8_t light, const LedOnTimes& t) {
    std::array<std::uint8_t, 12> p{};
    const std::uint16_t v[6] = {t.b, t.ir, t.r, 0, t.g, t.base};
    for (std::size_t i = 0; i < 6; ++i) {
        p[2 * i] = static_cast<std::uint8_t>(v[i] & 0xFF);
        p[2 * i + 1] = static_cast<std::uint8_t>(v[i] >> 8);
    }
    return ppb::make_write(light, 0x82, p);
}

// The on-time base for an integration time: round(integration × 0.24),
// fitted on four F-135+ configurations (1875→450, 1250→300, 2813→675,
// 4093→982; OEM_RE.md §9), integration clamped to 0xFFD.
inline std::uint16_t on_time_base(std::uint16_t integration) {
    return static_cast<std::uint16_t>(std::lround(std::min<double>(integration, 0xFFD) * 0.24));
}

// Each channel's on-time = round(base × duty), at most base − 2.
inline std::uint16_t on_time(std::uint16_t base, double duty) {
    duty = std::clamp(duty, 0.0, 1.0);
    const long v = std::lround(base * duty);
    return static_cast<std::uint16_t>(std::clamp<long>(v, 0, std::max<long>(base - 2, 0)));
}

// --- PICL: status / service -----------------------------------------------

inline ppb::Frame read_interrupt_status(std::uint8_t address) {
    return ppb::make_read(address, 1, 0x02);
}

// Acknowledge: WRITE reg 0x06 [00][status], status 0 acked as ff
// (TLB@0x1000bdd0; captured 00 02 / 00 20 / 00 22).
inline ppb::Frame interrupt_ack(std::uint8_t address, std::uint8_t status) {
    const std::uint8_t st = status == 0 ? std::uint8_t{0xFF} : status;
    return ppb::make_write(address, 0x06, std::array<std::uint8_t, 2>{0x00, st});
}

inline ppb::Frame read_dx_records(std::uint8_t light) { return ppb::make_read(light, 30, 0x90); }
inline ppb::Frame read_lamp_flags(std::uint8_t light) { return ppb::make_read(light, 1, 0x83); }
inline ppb::Frame read_lamp_setpoint(std::uint8_t light) { return ppb::make_read(light, 2, 0x84); }
inline ppb::Frame read_lamp_temperatures(std::uint8_t light) {
    return ppb::make_read(light, 4, 0x88);
}

// --- PICL: init and DX ------------------------------------------------------

// bDrvInitLampTemperatures (TLB@0x1002d190): four 4-byte temperature
// limit registers, the OEM registry defaults replayed as captured
// (base4.jsonl 6.605-6.613, identical in all six sessions).
inline std::array<ppb::Frame, 4> lamp_temperature_init(std::uint8_t light) {
    return {ppb::make_write(light, 0x8F, std::array<std::uint8_t, 4>{0xE8, 0xFF, 0x18, 0x00}),
            ppb::make_write(light, 0x8C, std::array<std::uint8_t, 4>{0xE0, 0xFF, 0x20, 0x00}),
            ppb::make_write(light, 0x8B, std::array<std::uint8_t, 4>{0xF0, 0x00, 0x20, 0x03}),
            ppb::make_write(light, 0x8D, std::array<std::uint8_t, 4>{0xA0, 0x00, 0x70, 0x03})};
}

// TEC: the two literals inside bDrvInitLampTemperatures, nothing else.
inline std::array<ppb::Frame, 2> tec_init(std::uint8_t light) {
    return {ppb::make_write(light, 0xD0, std::array<std::uint8_t, 1>{0x00}),
            ppb::make_write(light, 0xD1, std::array<std::uint8_t, 1>{0x01})};
}

inline ppb::Frame lamp_power_init(std::uint8_t light) { // 0x87 = 00 00 (bDrvInitCcd)
    return ppb::make_write(light, 0x87, std::array<std::uint8_t, 2>{0x00, 0x00});
}

inline ppb::Frame resample(std::uint8_t light, bool three_quarters) { // 0x89
    const std::uint8_t v = three_quarters ? 1 : 0;
    return ppb::make_write(light, 0x89, std::array<std::uint8_t, 1>{v});
}

// bDrvDxStart (TLB@0x1000a7b0): [u16 DX word][flag]; flag 0x01 in all
// twelve captured writes.
inline ppb::Frame dx_start(std::uint8_t light, std::uint16_t word, std::uint8_t flag = 0x01) {
    return ppb::make_write(light, 0x91,
                           std::array<std::uint8_t, 3>{static_cast<std::uint8_t>(word & 0xFF),
                                                       static_cast<std::uint8_t>(word >> 8), flag});
}

inline ppb::Frame dx_stop(std::uint8_t light) { return ppb::make_cmd(light, 0x92); }

// --- PICM: CCD FPGA (0x82 sub) ------------------------------------------------

enum class Fpga : std::uint8_t {
    control = 0,
    reg1 = 1,
    reg2 = 2,
    reg3 = 3,
    pixel_start = 4,
    pixel_end = 5,
    integration = 6,
    panel_leds = 9,
    reg10 = 10,
    reg11 = 11,
};

// Control-register bits (TLB@0x10029770 / 0x10029810 / 0x10029860).
inline constexpr std::uint16_t kControlAcquire = 0x0001;
inline constexpr std::uint16_t kControlBinning = 0x0002; // Base 4
inline constexpr std::uint16_t kControlInit = 0x0060;    // set once at init
inline constexpr std::uint16_t kControlIr = 0x0100;

inline ppb::Frame fpga(std::uint8_t motor, Fpga sub, std::uint16_t value) {
    return ppb::make_write(motor, 0x82,
                           std::array<std::uint8_t, 3>{static_cast<std::uint8_t>(sub),
                                                       static_cast<std::uint8_t>(value & 0xFF),
                                                       static_cast<std::uint8_t>(value >> 8)});
}

// --- PICM: CCD A/D (0x84 sub) --------------------------------------------------

enum class Channel : std::uint8_t { r = 0, g = 1, b = 2 };

inline ppb::Frame ad_register(std::uint8_t motor, std::uint8_t sub, std::uint16_t value) {
    return ppb::make_write(motor, 0x84,
                           std::array<std::uint8_t, 3>{sub, static_cast<std::uint8_t>(value & 0xFF),
                                                       static_cast<std::uint8_t>(value >> 8)});
}

// Gain code for an amplification g (TLB@0x100201d0): round((1 − 1/g) ×
// 75.6), g clamped to [1, 6]. 1.2 → 13 (0x0D, captured).
inline std::uint16_t gain_code(double g) {
    g = std::clamp(g, 1.0, 6.0);
    return static_cast<std::uint16_t>(std::lround((1.0 - 1.0 / g) * 75.6));
}

inline ppb::Frame ad_gain(std::uint8_t motor, Channel c, std::uint16_t code) {
    return ad_register(motor, static_cast<std::uint8_t>(2 + static_cast<int>(c)),
                       std::min<std::uint16_t>(code, 0x3F));
}

// Sign-magnitude offset (TLB@0x100299c0): |v| ≤ 255, bit 8 = negative.
inline std::uint16_t encode_offset(int v) {
    v = std::clamp(v, -255, 255);
    return static_cast<std::uint16_t>(v < 0 ? (0x100 | -v) : v);
}

inline ppb::Frame ad_offset(std::uint8_t motor, Channel c, int offset) {
    return ad_register(motor, static_cast<std::uint8_t>(5 + static_cast<int>(c)),
                       encode_offset(offset));
}

// --- PICM: motor -------------------------------------------------------------

inline ppb::Frame motor_rate(std::uint8_t motor, std::uint16_t rate) { // 0xA5
    return ppb::make_write(motor, 0xA5,
                           std::array<std::uint8_t, 2>{static_cast<std::uint8_t>(rate & 0xFF),
                                                       static_cast<std::uint8_t>(rate >> 8)});
}
inline ppb::Frame motor_go(std::uint8_t motor) { return ppb::make_cmd(motor, 0xA0); }
inline ppb::Frame motor_stop(std::uint8_t motor) { return ppb::make_cmd(motor, 0xA2); }

} // namespace pakon::protocol::scan
