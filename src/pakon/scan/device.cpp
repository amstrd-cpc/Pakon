#include "pakon/scan/device.hpp"

#include <algorithm>
#include <format>

#include "pakon/logging/logger.hpp"

namespace pakon::scan {

namespace ps = protocol::scan;

ScanDevice::ScanDevice(ICommandChannel& channel, protocol::ControllerAddresses addresses)
    : channel_(channel), addresses_(addresses) {}

Result<ppb::Reply> ScanDevice::read(const ppb::Frame& frame) {
    ++frames_sent_;
    auto reply = channel_.exchange(frame);
    if (!reply) {
        return reply.error();
    }
    const bool ok = frame.type == ppb::FrameType::read ? ppb::is_read_success(*reply)
                    : frame.type == ppb::FrameType::read_status
                        ? ppb::is_poll_success(reply->status)
                        : ppb::is_success(reply->status);
    if (!ok) {
        return failure<ppb::Reply>(
            ErrorKind::ppb_bad_status,
            std::format("frame to 0x{:02x} reg 0x{:02x} answered status {}", frame.data[0],
                        frame.data.size() > 2 ? frame.data[2] : 0,
                        ppb::to_string(reply->status)));
    }
    return reply;
}

VoidResult ScanDevice::send(const ppb::Frame& frame) {
    auto r = read(frame);
    if (!r) {
        return r.error();
    }
    return {};
}

VoidResult ScanDevice::reset_fifos() {
    for (const auto& f : ps::reset_fifos(addresses_)) {
        if (auto r = send(f); !r) {
            return r;
        }
    }
    return {};
}

VoidResult ScanDevice::lamp_on(bool visible, bool ir, ps::LedCurrents currents,
                               std::uint16_t integration_time, const Duties& duties) {
    // bDrvLampOn (TLB@0x1002c5f0): mask, clamped currents, on-times; then
    // the LED settle wait for any current increase.
    const auto ceiling = ps::led_ceiling(addresses_.motor, ir);
    currents = ps::clamp_currents(currents, ceiling);
    if (!visible) {
        currents.r = currents.g = currents.b = 0;
    }
    if (!ir) {
        currents.ir = 0;
    }
    const bool mask_changed = lamp_.visible != visible || lamp_.ir != ir;
    if (mask_changed) {
        if (auto r = send(ps::lamp_mask(addresses_.light, visible, ir)); !r) {
            return r;
        }
    }
    // Settle: 5 s × Δcurrent / ceiling, the largest channel wins
    // (WaitForLamp_c = 5.0 s registry default, OEM_RE.md §9).
    double settle_s = 0;
    const auto rise = [&](std::uint8_t now, std::uint8_t before, std::uint8_t ceil) {
        if (ceil > 0 && now > before) {
            settle_s = std::max(settle_s, 5.0 * (now - before) / ceil);
        }
    };
    rise(currents.r, lamp_.currents.r, ceiling.r);
    rise(currents.g, lamp_.currents.g, ceiling.g);
    rise(currents.b, lamp_.currents.b, ceiling.b);
    rise(currents.ir, lamp_.currents.ir, ceiling.ir);

    const auto c = lamp_.currents;
    if (mask_changed || c.r != currents.r || c.g != currents.g || c.b != currents.b ||
        c.ir != currents.ir) {
        if (auto r = send(ps::led_currents(addresses_.light, currents, ceiling)); !r) {
            return r;
        }
    }
    const std::uint16_t base = ps::on_time_base(integration_time);
    ps::LedOnTimes t;
    t.base = base;
    t.r = visible ? ps::on_time(base, duties.r) : 0;
    t.g = visible ? ps::on_time(base, duties.g) : 0;
    t.b = visible ? ps::on_time(base, duties.b) : 0;
    t.ir = ir ? ps::on_time(base, duties.ir) : 0;
    const auto& o = lamp_.on_times;
    if (mask_changed || o.r != t.r || o.g != t.g || o.b != t.b || o.ir != t.ir ||
        o.base != t.base) {
        if (auto r = send(ps::led_on_times(addresses_.light, t)); !r) {
            return r;
        }
    }
    lamp_ = {visible, ir, currents, t};
    if (settle_s > 0 && settle_) {
        settle_(std::chrono::milliseconds(static_cast<long long>(settle_s * 1000)));
    }
    return {};
}

VoidResult ScanDevice::lamp_off() {
    if (auto r = send(ps::lamp_mask(addresses_.light, false, false)); !r) {
        return r;
    }
    lamp_.visible = lamp_.ir = false;
    return {};
}

VoidResult ScanDevice::fpga_settings(const Geometry& g, std::uint16_t integration_time) {
    const std::uint16_t ctrl = control_word(g, (control_ & ps::kControlAcquire) != 0);
    const ppb::Frame frames[] = {
        ps::fpga(addresses_.motor, ps::Fpga::integration, integration_time),
        ps::fpga(addresses_.motor, ps::Fpga::control, ctrl),
        ps::fpga(addresses_.motor, ps::Fpga::pixel_start, g.start),
        ps::fpga(addresses_.motor, ps::Fpga::pixel_end, g.end),
        ps::resample(addresses_.light, g.resample),
    };
    for (const auto& f : frames) {
        if (auto r = send(f); !r) {
            return r;
        }
    }
    control_ = ctrl;
    return {};
}

VoidResult ScanDevice::acquire(bool on) {
    const std::uint16_t ctrl = on ? static_cast<std::uint16_t>(control_ | ps::kControlAcquire)
                                  : static_cast<std::uint16_t>(control_ & ~ps::kControlAcquire);
    if (auto r = send(ps::fpga(addresses_.motor, ps::Fpga::control, ctrl)); !r) {
        return r;
    }
    control_ = ctrl;
    return {};
}

VoidResult ScanDevice::dx_start(std::uint16_t word) {
    return send(ps::dx_start(addresses_.light, word));
}

VoidResult ScanDevice::dx_stop() { return send(ps::dx_stop(addresses_.light)); }

VoidResult ScanDevice::ad_gains(const std::array<std::uint16_t, 3>& codes) {
    for (std::size_t c = 0; c < 3; ++c) {
        if (auto r = send(ps::ad_gain(addresses_.motor, static_cast<ps::Channel>(c), codes[c]));
            !r) {
            return r;
        }
    }
    return {};
}

VoidResult ScanDevice::ad_offsets(const std::array<int, 3>& offsets) {
    for (std::size_t c = 0; c < 3; ++c) {
        if (auto r = send(ps::ad_offset(addresses_.motor, static_cast<ps::Channel>(c), offsets[c]));
            !r) {
            return r;
        }
    }
    return {};
}

VoidResult ScanDevice::panel_leds(std::uint16_t state) {
    return send(ps::fpga(addresses_.motor, ps::Fpga::panel_leds, state));
}

VoidResult ScanDevice::motor_rate(std::uint16_t rate) {
    return send(ps::motor_rate(addresses_.motor, rate));
}
VoidResult ScanDevice::motor_go() { return send(ps::motor_go(addresses_.motor)); }
VoidResult ScanDevice::motor_stop() { return send(ps::motor_stop(addresses_.motor)); }

Result<ServiceEvent> ScanDevice::read_lamp_status() {
    ServiceEvent ev;
    auto flags = read(ps::read_lamp_flags(addresses_.light));
    if (!flags) {
        return flags.error();
    }
    if (flags->payload.size() == 1) {
        ev.lamp_flags = flags->payload[0];
    }
    auto setpoint = read(ps::read_lamp_setpoint(addresses_.light));
    if (!setpoint) {
        return setpoint.error();
    }
    if (setpoint->payload.size() == 2) {
        ev.lamp_setpoint_c = (setpoint->payload[0] | (setpoint->payload[1] << 8)) / 16.0;
    }
    auto temps = read(ps::read_lamp_temperatures(addresses_.light));
    if (!temps) {
        return temps.error();
    }
    if (temps->payload.size() == 4) {
        const auto& p = temps->payload;
        ev.lamp_temps_c = std::pair<double, double>{(p[0] | (p[1] << 8)) / 16.0,
                                                    (p[2] | (p[3] << 8)) / 16.0};
    }
    return ev;
}

Result<ServiceEvent> ScanDevice::poll_service() {
    ServiceEvent ev;
    auto host = read(ppb::make_read_status(protocol::kAddrHost));
    if (!host) {
        return host.error();
    }
    ev.pending = (static_cast<std::uint8_t>(host->status) & 0x80) != 0;
    if (!ev.pending) {
        return ev;
    }
    // TLB@0x1000bdd0: PICL then PICM; ack when the READ flags carry 0x80.
    for (const std::uint8_t address : {addresses_.light, addresses_.motor}) {
        auto st = read(ps::read_interrupt_status(address));
        if (!st) {
            return st.error();
        }
        if (!st->event_pending() || st->payload.size() != 1) {
            continue;
        }
        const std::uint8_t status = st->payload[0];
        if (auto r = send(ps::interrupt_ack(address, status)); !r) {
            return r.error();
        }
        if (address == addresses_.light) {
            ev.light_status = status;
        } else {
            ev.motor_status = status;
        }
    }
    if ((ev.light_status & 0xA4) != 0) {
        auto dx = read(ps::read_dx_records(addresses_.light));
        if (!dx) {
            return dx.error();
        }
        if (dx->payload.size() >= 4) {
            ev.dx_flags = static_cast<std::uint8_t>(dx->payload[3] & 0x30);
        }
    }
    if ((ev.light_status & 0x5B) != 0) {
        auto lamp = read_lamp_status();
        if (!lamp) {
            return lamp.error();
        }
        ev.lamp_flags = lamp->lamp_flags;
        ev.lamp_setpoint_c = lamp->lamp_setpoint_c;
        ev.lamp_temps_c = lamp->lamp_temps_c;
    }
    return ev;
}

std::vector<TeardownFrame> teardown_frames(const protocol::ControllerAddresses& a,
                                           std::uint16_t idle) {
    const auto fifos = ps::reset_fifos(a);
    // The OEM's bAfterScan order (TLB@0x1002a900, capture base4.jsonl
    // 38.014-38.088), acquire off first: it is what stops the stream and
    // the line-clocked transport (OEM_RE.md §7). Then the bridge's stop
    // (pakon-captures bridge/pakonusb.py safe_stop) as an independent
    // second stop per the hardware safety rules. The OEM itself never
    // writes a rate below 1000 (TLB@0x1000b6d0); with rate 0 the go
    // command has nothing to drive. Well-formed frames, PICM only.
    return {
        {"acquire off (FPGA control idle)", ps::fpga(a.motor, ps::Fpga::control, idle)},
        {"lamp off", ps::lamp_mask(a.light, false, false)},
        {"reset FIFOs (HOST 0x84)", fifos[0]},
        {"reset FIFOs (PICL 0x8A)", fifos[1]},
        {"DX stop (0x92)", ps::dx_stop(a.light)},
        {"motor stop / disengage (0xA2)", ps::motor_stop(a.motor)},
        {"rate = 0 (0xA5)", ps::motor_rate(a.motor, 0)},
        {"go (0xA0)", ps::motor_go(a.motor)},
        {"idle (0xA2)", ps::motor_stop(a.motor)},
    };
}

const TeardownReport& ScanDevice::teardown() {
    if (teardown_.ran) {
        return teardown_;
    }
    teardown_.ran = true;
    const auto step = [&](std::string what, const ppb::Frame& frame) {
        TeardownStep s{std::move(what), true, {}};
        if (auto r = send(frame); !r) {
            s.ok = false;
            s.error = r.error().message;
            log::Logger::instance().log(log::Level::warn, "teardown step '{}' failed: {}",
                                        s.what, s.error);
        }
        teardown_.steps.push_back(std::move(s));
    };
    const auto idle = static_cast<std::uint16_t>(control_ & ~ps::kControlAcquire);
    for (const auto& f : teardown_frames(addresses_, idle)) {
        step(f.what, f.frame);
    }
    control_ = idle;
    lamp_.visible = lamp_.ir = false;
    return teardown_;
}

} // namespace pakon::scan
