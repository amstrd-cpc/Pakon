#include "pakon/scan/corrections.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "pakon/logging/logger.hpp"

namespace pakon::scan {

namespace ps = protocol::scan;

namespace {

// Measure after a settings change: FIFO reset on the device, stale bytes
// dropped on the host (the OEM's bDrvResetFifos + bCalibrateFlush).
Result<LineAverage> measure(ScanDevice& device, LineReader& lines, std::size_t n,
                            std::chrono::milliseconds timeout) {
    if (auto r = device.reset_fifos(); !r) {
        return r.error();
    }
    lines.flush();
    return lines.average(n, timeout);
}

double mean(const std::vector<double>& plane, std::size_t from, std::size_t to) {
    to = std::min(to, plane.size());
    if (from >= to) {
        return 0.0;
    }
    double s = 0;
    for (std::size_t i = from; i < to; ++i) {
        s += plane[i];
    }
    return s / static_cast<double>(to - from);
}

double peak(const std::vector<double>& plane, std::size_t from) {
    double m = 0;
    for (std::size_t i = from; i < plane.size(); ++i) {
        m = std::max(m, plane[i]);
    }
    return m;
}

// The OEM's on-time back-off after the current servo: (n−1)/n, 0.5 below 3.
double back_off(std::uint8_t n) {
    return n < 3 ? 0.5 : static_cast<double>(n - 1) / static_cast<double>(n);
}

} // namespace

Result<CorrectionsResult> run_corrections(ScanDevice& device, LineReader& lines,
                                          const CorrectionsConfig& cfg) {
    CorrectionsResult out;
    const auto& mc = constants(cfg.mode.base);
    const std::uint16_t integ = integration(cfg.mode);
    const auto log = [&](std::string line) {
        log::Logger::instance().log(log::Level::info, "corrections: {}", line);
        out.log.push_back(std::move(line));
    };

    // 1. Calibration geometry, IR plane off for the dark servo — applied
    // by the caller together with the acquire bit (the window is open).
    Geometry g = calibration_geometry(cfg.mode, cfg.offset, false);
    lines.set_stride(g.samples_per_line(), g.pixels());
    if (auto r = device.lamp_off(); !r) {
        return r.error();
    }
    // Masked pixels at the start of the calibration window, and the
    // gate (active) pixels used for light peaks.
    const std::size_t masked = mc.cal_start;
    const std::size_t gate_from =
        std::min<std::size_t>(g.pixels(), cfg.offset > mc.cal_start ? cfg.offset - mc.cal_start : 0);

    // 2. Dark-offset servo.
    const std::uint16_t g12 = ps::gain_code(kGainStart);
    out.gains = {g12, g12, g12};
    if (auto r = device.ad_gains(out.gains); !r) {
        return r.error();
    }
    out.offsets = {kDarkStartOffset, kDarkStartOffset, kDarkStartOffset};
    std::array<bool, 3> settled{};
    for (int round = 0; round < kDarkRounds && !(settled[0] && settled[1] && settled[2]);
         ++round) {
        if (auto r = device.ad_offsets(out.offsets); !r) {
            return r.error();
        }
        auto avg = measure(device, lines, kAverageLines, cfg.line_timeout);
        if (!avg) {
            return avg.error();
        }
        ++out.dark_rounds;
        std::string line = std::format("dark round {}:", round + 1);
        for (std::size_t c = 0; c < 3; ++c) {
            const double m = mean(avg->planes[c], 0, masked);
            line += std::format(" {}={:.0f}@{}", "RGB"[c], m, out.offsets[c]);
            if (settled[c]) {
                continue;
            }
            if (std::abs(m - kDarkTarget) <= kDarkTolerance) {
                settled[c] = true;
            } else {
                const int step = static_cast<int>(std::trunc(-(m - kDarkTarget) * 3.0 / 112.0));
                out.offsets[c] = std::clamp(out.offsets[c] + step, -255, 255);
            }
        }
        log(std::move(line));
    }
    if (!(settled[0] && settled[1] && settled[2])) {
        log("dark offsets did not settle within 8 rounds (the OEM continues; so do we)");
    }

    // 3. IR plane on (IR modes), dark reference.
    if (cfg.mode.ir) {
        g = calibration_geometry(cfg.mode, cfg.offset, true);
        if (auto r = device.fpga_settings(g, integ); !r) {
            return r.error();
        }
        lines.set_stride(g.samples_per_line(), g.pixels());
    }
    {
        auto dark = measure(device, lines, kAverageLines, cfg.line_timeout);
        if (!dark) {
            return dark.error();
        }
        out.dark = std::move(*dark);
    }

    // 4. LED-current servo from 1 at full on-time.
    const bool ir = cfg.mode.ir;
    const auto ceiling = ps::led_ceiling(device.addresses().motor, ir);
    out.currents = {1, 1, 1, static_cast<std::uint8_t>(ir ? 1 : 0)};
    out.duties = {1.0, 1.0, 1.0, ir ? 1.0 : 0.0};
    // 0 = searching, 1 = at ceiling, 2 = saturated (peak above target)
    std::array<int, 4> state{0, 0, 0, ir ? 0 : 2};
    const std::uint32_t limits[4] = {kPeakVisible, kPeakVisible, kPeakBlue, kPeakIr};
    std::uint8_t* cur[4] = {&out.currents.r, &out.currents.g, &out.currents.b, &out.currents.ir};
    const std::uint8_t ceil[4] = {ceiling.r, ceiling.g, ceiling.b, ceiling.ir};
    for (int round = 0; round < 64; ++round) {
        if (auto r = device.lamp_on(true, ir, out.currents, integ, out.duties); !r) {
            return r.error();
        }
        auto avg = measure(device, lines, kAverageLines, cfg.line_timeout);
        if (!avg) {
            return avg.error();
        }
        ++out.current_rounds;
        std::string line = std::format("current round {}:", round + 1);
        for (std::size_t c = 0; c < 4; ++c) {
            if (state[c] != 0 || c >= avg->planes.size()) {
                continue;
            }
            const double pk = peak(avg->planes[c], gate_from);
            line += std::format(" {}={}@{:.0f}", "RGBI"[c], *cur[c], pk);
            if (pk > limits[c]) {
                state[c] = 2;
            } else if (*cur[c] < ceil[c]) {
                ++*cur[c];
            } else {
                state[c] = 1;
            }
        }
        log(std::move(line));
        if (state[0] && state[1] && state[2] && state[3]) {
            break;
        }
    }
    out.duties = {back_off(out.currents.r), back_off(out.currents.g), back_off(out.currents.b),
                  ir ? back_off(out.currents.ir) : 0.0};

    // 5. On-time (duty) servo with the need-more-light escalation.
    for (int outer = 0; outer < 64; ++outer) {
        std::array<bool, 4> done{false, false, false, !ir};
        std::array<bool, 4> more{false, false, false, false};
        // The OEM also stops a channel whose quantized on-time repeats one
        // of the two previous values (TLB@0x1001ec90): the target band is
        // narrower than one on-time step at some bases.
        std::array<std::array<int, 2>, 4> seen{};
        for (auto& v : seen) {
            v = {-1, -1};
        }
        const std::uint16_t base_units = ps::on_time_base(integ);
        for (int inner = 0; inner < 12; ++inner) {
            if (auto r = device.lamp_on(true, ir, out.currents, integ, out.duties); !r) {
                return r.error();
            }
            auto avg = measure(device, lines, kAverageLines, cfg.line_timeout);
            if (!avg) {
                return avg.error();
            }
            ++out.duty_rounds;
            double* duty[4] = {&out.duties.r, &out.duties.g, &out.duties.b, &out.duties.ir};
            std::string line = std::format("duty round {}:", out.duty_rounds);
            for (std::size_t c = 0; c < 4; ++c) {
                if (done[c] || c >= avg->planes.size()) {
                    continue;
                }
                const double target = c == 3 ? kDutyTargetIr : kDutyTarget;
                const double lo = c == 3 ? 0x9C00 : 0xF9C0;
                const double hi = c == 3 ? 0x9C40 : 0xFA00;
                const double pk = peak(avg->planes[c], gate_from);
                line += std::format(" {}={:.3f}@{:.0f}", "RGBI"[c], *duty[c], pk);
                const int q = ps::on_time(base_units, *duty[c]);
                if ((pk >= lo && pk <= hi) || q == seen[c][0] || q == seen[c][1]) {
                    done[c] = true;
                    continue;
                }
                seen[c] = {seen[c][1], q};
                const double next = pk <= 0 ? *duty[c] * 2 : *duty[c] * target / pk;
                if (next > 1.0) {
                    *duty[c] = 1.0;
                    more[c] = true;
                    done[c] = true;
                } else {
                    *duty[c] = next;
                }
            }
            log(std::move(line));
            if (done[0] && done[1] && done[2] && done[3]) {
                break;
            }
        }
        if (!(more[0] || more[1] || more[2] || more[3])) {
            break;
        }
        // Escalate: current below the ceiling first, then the A/D gain.
        for (std::size_t c = 0; c < 4; ++c) {
            if (!more[c]) {
                continue;
            }
            if (*cur[c] < ceil[c]) {
                ++*cur[c];
                double* duty[4] = {&out.duties.r, &out.duties.g, &out.duties.b, &out.duties.ir};
                *duty[c] = back_off(*cur[c]);
            } else if (c < 3 && out.gains[c] < kGainCodeMax) {
                ++out.gains[c];
            } else {
                return failure<CorrectionsResult>(
                    ErrorKind::scanner_unexpected_state,
                    std::format("EC_InsufficientLight: {} channel reached its LED ceiling "
                                "and A/D gain limit without reaching the white target",
                                c == 0 ? "Red" : c == 1 ? "Green" : c == 2 ? "Blue" : "Ir"));
            }
        }
        if (auto r = device.ad_gains(out.gains); !r) {
            return r.error();
        }
        log(std::format("escalated: currents R{} G{} B{} IR{} gains {} {} {}", out.currents.r,
                        out.currents.g, out.currents.b, out.currents.ir, out.gains[0],
                        out.gains[1], out.gains[2]));
    }

    // 6. White reference with the final settings.
    if (auto r = device.lamp_on(true, ir, out.currents, integ, out.duties); !r) {
        return r.error();
    }
    auto white = measure(device, lines, kAverageLines, cfg.line_timeout);
    if (!white) {
        return white.error();
    }
    out.white = std::move(*white);
    log(std::format("final: offsets {} {} {}, gains {} {} {}, currents R{} G{} B{} IR{}, duties "
                    "{:.3f} {:.3f} {:.3f} {:.3f}",
                    out.offsets[0], out.offsets[1], out.offsets[2], out.gains[0], out.gains[1],
                    out.gains[2], out.currents.r, out.currents.g, out.currents.b,
                    out.currents.ir, out.duties.r, out.duties.g, out.duties.b, out.duties.ir));
    return out;
}

} // namespace pakon::scan
