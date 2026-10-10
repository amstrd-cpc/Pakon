#pragma once

// Scan modes (Base 4/8/16 × IR) and the per-mode constants TLB.dll
// hard-codes (docs/OEM_RE.md §7.1, TLB@0x10010760 region): line width,
// resample/binning, calibration window start, integration time, DX word.
// Every value is checked against all six capture configurations in
// tests/scan/scan_commands_test.cpp. Per-UNIT values (Offset, motor
// speeds) never live here — they come from the EEPROM (eeprom/).

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

#include "pakon/protocol/scan_commands.hpp"

namespace pakon::scan {

enum class Base { b4 = 0, b8 = 1, b16 = 2 };

struct ScanMode {
    Base base{Base::b4};
    bool ir{false};
};

struct ModeConstants {
    std::uint16_t width;          // CCD pixels across the gate (before resample)
    bool resample;                // PICL 0x89 = 1: 3/4 horizontal resample (Base 8)
    bool binning;                 // FPGA control bit1 (Base 4)
    std::uint16_t cal_start;      // calibration window start pixel
    std::uint16_t integration;    // FPGA sub 6, no IR
    std::uint16_t integration_ir;
    std::uint16_t dx_word;        // 0x91 payload word, no IR
    std::uint16_t dx_word_ir;
};

inline constexpr std::array<ModeConstants, 3> kModes{{
    {1000, false, true, 3, 1875, 1250, 0x0107, 0x00C5},  // Base 4
    {2000, true, false, 6, 2813, 2128, 0x0075, 0x004D},  // Base 8
    {2000, false, false, 6, 4093, 2498, 0x003C, 0x0031}, // Base 16
}};

inline const ModeConstants& constants(Base base) {
    return kModes[static_cast<std::size_t>(base)];
}

inline std::size_t base_index(Base base) { return static_cast<std::size_t>(base); }

// "base4", "base4-ir", "base8", "base8-ir", "base16", "base16-ir"; the
// older "-ir-off" / "-ir-on" spellings are accepted too.
inline std::optional<ScanMode> parse_mode(std::string_view name) {
    struct Entry {
        std::string_view name;
        ScanMode mode;
    };
    static constexpr Entry kNames[] = {
        {"base4", {Base::b4, false}},        {"base4-ir", {Base::b4, true}},
        {"base8", {Base::b8, false}},        {"base8-ir", {Base::b8, true}},
        {"base16", {Base::b16, false}},      {"base16-ir", {Base::b16, true}},
        {"base4-ir-off", {Base::b4, false}}, {"base4-ir-on", {Base::b4, true}},
        {"base8-ir-off", {Base::b8, false}}, {"base8-ir-on", {Base::b8, true}},
        {"base16-ir-off", {Base::b16, false}}, {"base16-ir-on", {Base::b16, true}},
    };
    for (const auto& e : kNames) {
        if (e.name == name) {
            return e.mode;
        }
    }
    return std::nullopt;
}

inline std::string_view mode_name(ScanMode m) {
    static constexpr std::string_view kNames[3][2] = {
        {"base4", "base4-ir"}, {"base8", "base8-ir"}, {"base16", "base16-ir"}};
    return kNames[static_cast<std::size_t>(m.base)][m.ir ? 1 : 0];
}

inline std::uint16_t integration(ScanMode m) {
    const auto& c = constants(m.base);
    return m.ir ? c.integration_ir : c.integration;
}

inline std::uint16_t dx_word(ScanMode m) {
    const auto& c = constants(m.base);
    return m.ir ? c.dx_word_ir : c.dx_word;
}

// One acquisition's line layout (OEM_RE.md §10): samples per line =
// channels × pixels, pixels = (end − start) × (resample ? 3/4 : 1).
struct Geometry {
    std::uint16_t start{0};
    std::uint16_t end{0};
    bool resample{false};
    bool binning{false};
    bool ir{false};

    std::size_t pixels() const {
        const std::size_t raw = end > start ? static_cast<std::size_t>(end - start) : 0;
        return resample ? raw * 3 / 4 : raw;
    }
    std::size_t channels() const { return ir ? 4 : 3; }
    std::size_t samples_per_line() const { return pixels() * channels(); }
};

// Calibration window: from the masked pixels (cal_start) to the gate's
// end (Offset + width) — Base 4 = 3..1030 = 1027 px (OEM_RE.md §9).
inline Geometry calibration_geometry(ScanMode m, std::uint16_t offset, bool ir_plane) {
    const auto& c = constants(m.base);
    return {c.cal_start, static_cast<std::uint16_t>(offset + c.width), c.resample, c.binning,
            ir_plane};
}

// Film window: the gate itself, starting at this unit's Offset.
inline Geometry film_geometry(ScanMode m, std::uint16_t offset) {
    const auto& c = constants(m.base);
    return {offset, static_cast<std::uint16_t>(offset + c.width), c.resample, c.binning, m.ir};
}

// FPGA control word (OEM_RE.md §3): init bits, binning, IR mode, acquire.
inline std::uint16_t control_word(const Geometry& g, bool acquire) {
    namespace ps = protocol::scan;
    return static_cast<std::uint16_t>(ps::kControlInit | (g.binning ? ps::kControlBinning : 0) |
                                      (g.ir ? ps::kControlIr : 0) |
                                      (acquire ? ps::kControlAcquire : 0));
}

// Line period from the integration time: integration × 0.48 µs — the
// same effective 2.0833 MHz clock that fits the LED on-time base
// (OEM_RE.md §9) and the captured stream rates [INFERRED].
inline double line_time_us(std::uint16_t integration_time) {
    return static_cast<double>(integration_time) * 0.48;
}

} // namespace pakon::scan
