#include "pakon/scan/runner.hpp"

#include <algorithm>
#include <exception>
#include <format>
#include <thread>

#include "pakon/logging/logger.hpp"
#include "pakon/scan/film.hpp"

namespace pakon::scan {

namespace ps = protocol::scan;
using Clock = std::chrono::steady_clock;

namespace {

// Panel LED states the OEM writes at each step (FPGA sub 9, bDrvSetLed;
// cosmetic, replayed from base4.jsonl 6.692-42.432).
constexpr std::uint16_t kLedIdle = 0x0017;
constexpr std::uint16_t kLedCalibrating = 0x0313;
constexpr std::uint16_t kLedFilmArmed = 0x0217;
constexpr std::uint16_t kLedScanning = 0x0295;
constexpr std::uint16_t kLedFilmSeen = 0x0215;
constexpr std::uint16_t kLedFilmEnd = 0x02D4;

double seconds_since(Clock::time_point t) {
    return std::chrono::duration<double>(Clock::now() - t).count();
}

VoidResult check_cancel(const CancelToken* cancel, const char* where) {
    if (cancel && cancel->requested()) {
        return void_failure(ErrorKind::cancelled, std::format("interrupt observed {}", where));
    }
    return {};
}

} // namespace

std::string_view to_string(FilmEnd end) {
    switch (end) {
    case FilmEnd::none: return "none";
    case FilmEnd::film_passed: return "film passed (density detector)";
    case FilmEnd::no_film_timeout: return "no film within the timeout";
    case FilmEnd::row_budget: return "row budget reached";
    }
    return "unknown";
}

std::vector<ppb::Frame> ScanRunner::init_frames(const protocol::ControllerAddresses& a) {
    std::vector<ppb::Frame> f;
    for (auto& x : ps::reset_fifos(a)) f.push_back(x);
    for (auto& x : ps::lamp_temperature_init(a.light)) f.push_back(x);
    for (auto& x : ps::tec_init(a.light)) f.push_back(x);
    // bDrvInitCcd (TLB@0x1002d5c0)
    f.push_back(ps::lamp_power_init(a.light));
    f.push_back(ps::lamp_mask(a.light, true, false));
    f.push_back(ps::led_on_times(a.light, {0, 0, 0, 0, ps::on_time_base(0xFFD)}));
    f.push_back(ps::lamp_mask(a.light, false, false));
    f.push_back(ps::resample(a.light, false));
    f.push_back(ps::fpga(a.motor, ps::Fpga::integration, 0xFFD));
    f.push_back(ps::fpga(a.motor, ps::Fpga::control, ps::kControlInit));
    f.push_back(ps::fpga(a.motor, ps::Fpga::reg11, 0));
    f.push_back(ps::fpga(a.motor, ps::Fpga::pixel_start, 0x3E));
    f.push_back(ps::fpga(a.motor, ps::Fpga::pixel_end, 0x3E + 2000));
    f.push_back(ps::fpga(a.motor, ps::Fpga::reg1, 0));
    f.push_back(ps::fpga(a.motor, ps::Fpga::reg2, 0));
    f.push_back(ps::fpga(a.motor, ps::Fpga::reg3, 0));
    f.push_back(ps::fpga(a.motor, ps::Fpga::reg10, 0x400));
    f.push_back(ps::ad_register(a.motor, 0, 0x78));
    f.push_back(ps::ad_register(a.motor, 1, 0x80));
    // bDriveMotorStop, status reads, panel LEDs
    f.push_back(ps::motor_stop(a.motor));
    f.push_back(ps::read_dx_records(a.light));
    f.push_back(ps::read_lamp_flags(a.light));
    f.push_back(ps::read_lamp_setpoint(a.light));
    f.push_back(ps::read_lamp_temperatures(a.light));
    f.push_back(ps::fpga(a.motor, ps::Fpga::panel_leds, 0x0014));
    f.push_back(ps::fpga(a.motor, ps::Fpga::panel_leds, kLedIdle));
    return f;
}

Result<std::unique_ptr<ScanRunner>> ScanRunner::create(RunnerConfig config) {
    if (config.kind == ScanKind::film && config.film_row_budget == 0) {
        return failure<std::unique_ptr<ScanRunner>>(
            ErrorKind::image_no_completion_policy,
            "a film window needs a row budget (> 0) as its hard cap");
    }
    if (config.poll_interval.count() <= 0 || config.warmup_timeout.count() <= 0) {
        return failure<std::unique_ptr<ScanRunner>>(ErrorKind::scanner_timeout,
                                                    "poll interval and warm-up timeout must be > 0");
    }
    return std::unique_ptr<ScanRunner>(new ScanRunner(std::move(config)));
}

Result<ScanResult> ScanRunner::run(ICommandChannel& commands, stream::IImageStream& stream) {
    result_ = {};
    result_.mode = config_.mode;
    result_.kind = config_.kind;
    result_.unit = config_.unit;
    ScanDevice device(commands, config_.addresses);
    // LED settle waits drain the stream (it keeps flowing while the
    // acquire bit is set; an undrained ring would overflow).
    device.set_settle([&](std::chrono::milliseconds d) {
        const auto until = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                               d * config_.settle_scale);
        while (Clock::now() < until && !(config_.cancel && config_.cancel->requested())) {
            stream.discard();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        stream.discard();
    });

    // An exception (e.g. bad_alloc) must not skip the teardown either.
    VoidResult r;
    try {
        r = run_phases(device, stream);
    } catch (const std::exception& e) {
        r = void_failure(ErrorKind::scanner_unexpected_state,
                         std::format("exception during the scan: {}", e.what()));
    } catch (...) {
        r = void_failure(ErrorKind::scanner_unexpected_state, "unknown exception during the scan");
    }
    // The one teardown, on every path. Never cancellable.
    const auto t0 = Clock::now();
    result_.teardown = device.teardown();
    stream.stop();
    result_.phases.push_back({"teardown", seconds_since(t0)});
    result_.stream = stream.stats();
    if (!r) {
        log::Logger::instance().log(log::Level::error, "scan {}: {}",
                                    r.error().kind == ErrorKind::cancelled ? "cancelled" : "failed",
                                    r.error().message);
        return r.error();
    }
    return result_;
}

VoidResult ScanRunner::run_phases(ScanDevice& device, stream::IImageStream& stream) {
    const auto& a = config_.addresses;
    const ScanMode mode = config_.mode;
    const auto& base = config_.unit.base[base_index(mode.base)];
    const std::uint16_t integ = integration(mode);

    // --- init ---------------------------------------------------------
    auto t = Clock::now();
    for (const auto& frame : init_frames(a)) {
        if (auto r = device.read(frame); !r) {
            return r.error();
        }
    }
    result_.phases.push_back({"init", seconds_since(t)});

    // --- lamp warm-up (bLampTemperatureStable) -------------------------
    t = Clock::now();
    {
        const auto deadline = Clock::now() + config_.warmup_timeout;
        auto last_direct = Clock::now();
        auto heartbeat = Clock::now();
        bool stable = false;
        while (!stable) {
            if (auto c = check_cancel(config_.cancel, "during lamp warm-up"); !c) {
                return c;
            }
            if (Clock::now() >= deadline) {
                return void_failure(ErrorKind::scanner_timeout,
                                    std::format("lamp not stable after {} s (OEM limit 300 s)",
                                                config_.warmup_timeout.count() / 1000));
            }
            auto ev = device.poll_service();
            if (!ev) {
                return ev.error();
            }
            if (ev->pending) {
                result_.events.push_back(std::format(
                    "warm-up: service light 0x{:02x} motor 0x{:02x}", ev->light_status,
                    ev->motor_status));
            }
            stable = ev->lamp_stable();
            // The OEM also re-reads the lamp status on a timer, which
            // covers a lamp that was already warm (no new event).
            if (!stable && Clock::now() - last_direct >= std::chrono::seconds(2)) {
                last_direct = Clock::now();
                auto lamp = device.read_lamp_status();
                if (!lamp) {
                    return lamp.error();
                }
                stable = lamp->lamp_stable();
            }
            if (Clock::now() - heartbeat >= std::chrono::seconds(5)) {
                heartbeat = Clock::now();
                log::Logger::instance().log(log::Level::info, "lamp warm-up: waiting ({:.0f} s)",
                                            seconds_since(t));
            }
            if (!stable) {
                std::this_thread::sleep_for(config_.poll_interval);
            }
        }
    }
    result_.phases.push_back({"lamp warm-up", seconds_since(t)});

    // --- Corrections window --------------------------------------------
    t = Clock::now();
    if (auto r = check_cancel(config_.cancel, "before Corrections"); !r) {
        return r;
    }
    LineReader lines(stream, config_.cancel);
    if (auto r = stream.start(); !r) { // BEFORE the acquire bit (OEM_RE.md §6)
        return r;
    }
    if (auto r = device.panel_leds(kLedCalibrating); !r) {
        return r;
    }
    // Calibration geometry first, then bDrvCcdAcquireAndDxStart.
    {
        const Geometry g = calibration_geometry(mode, base.offset, false);
        if (auto r = device.fpga_settings(g, integ); !r) {
            return r;
        }
        // The OEM empties the FIFOs between the geometry and the acquire
        // bit (CAP base4.jsonl 27.648-27.674).
        if (auto r = device.reset_fifos(); !r) {
            return r;
        }
        if (auto r = device.acquire(true); !r) {
            return r;
        }
        if (auto r = device.dx_start(dx_word(mode)); !r) {
            return r;
        }
    }
    CorrectionsConfig cc;
    cc.mode = mode;
    cc.offset = base.offset;
    cc.line_timeout = config_.line_timeout;
    auto corr = run_corrections(device, lines, cc);
    if (!corr) {
        return corr.error();
    }
    result_.corrections = std::move(*corr);
    result_.phases.push_back({"corrections", seconds_since(t)});

    if (config_.kind == ScanKind::first_light) {
        t = Clock::now();
        // White lines at the final settings (lamp on, motor never engaged).
        image::RawImage white;
        white.geometry.samples_per_row = static_cast<std::uint32_t>(lines.samples_per_line());
        lines.flush();
        for (std::size_t n = 0; n < config_.first_light_lines; ++n) {
            auto line = lines.next_line(config_.line_timeout);
            if (!line) {
                return line.error();
            }
            white.samples.insert(white.samples.end(), line->begin(), line->end());
        }
        result_.white = std::move(white);
        if (auto r = device.acquire(false); !r) {
            return r;
        }
        result_.resyncs = lines.resyncs();
        result_.phases.push_back({"first-light lines", seconds_since(t)});
        return {};
    }

    // --- film window ---------------------------------------------------
    t = Clock::now();
    // Close the calibration window, then the film setup (OEM_RE.md §7).
    if (auto r = device.acquire(false); !r) {
        return r;
    }
    if (auto r = device.reset_fifos(); !r) {
        return r;
    }
    const Geometry fg = film_geometry(mode, base.offset);
    result_.film_geometry = fg;
    // The whole window is held in memory: reserve it now, before the
    // motor moves, so a row cap the host cannot hold fails here (caught
    // by run(), teardown follows) instead of mid-strip.
    if (config_.film_row_budget > result_.film.samples.max_size() / fg.samples_per_line()) {
        return void_failure(ErrorKind::image_no_completion_policy,
                            std::format("film row cap {} is too large to hold in memory",
                                        config_.film_row_budget));
    }
    result_.film.samples.reserve(config_.film_row_budget * fg.samples_per_line());
    if (auto r = device.fpga_settings(fg, integ); !r) {
        return r;
    }
    const auto& cr = result_.corrections;
    Duties film = cr.duties;
    film.r = std::min(1.0, film.r * kFilmBoost[0]);
    film.g = std::min(1.0, film.g * kFilmBoost[1]);
    film.b = std::min(1.0, film.b * kFilmBoost[2]);
    result_.film_duties = film;
    if (auto r = device.lamp_on(true, mode.ir, cr.currents, integ, film); !r) {
        return r;
    }
    if (auto r = device.panel_leds(kLedIdle); !r) {
        return r;
    }
    const std::uint16_t rate = eeprom::motor_rate(base, mode.ir);
    result_.motor_rate = rate;
    if (auto r = check_cancel(config_.cancel, "before the film window"); !r) {
        return r;
    }
    if (auto r = device.motor_rate(rate); !r) {
        return r;
    }
    if (auto r = device.motor_go(); !r) {
        return r;
    }
    lines.set_stride(fg.samples_per_line(), fg.pixels());
    if (auto r = device.acquire(true); !r) {
        return r;
    }
    if (auto r = device.dx_start(dx_word(mode)); !r) {
        return r;
    }
    if (auto r = device.panel_leds(kLedFilmArmed); !r) {
        return r;
    }
    if (auto r = device.panel_leds(kLedScanning); !r) {
        return r;
    }
    result_.phases.push_back({"film setup", seconds_since(t)});

    t = Clock::now();
    image::RawImage& img = result_.film;
    img.geometry.samples_per_row = static_cast<std::uint32_t>(fg.samples_per_line());
    FilmDetector detector(fg.pixels());
    const auto window_start = Clock::now();
    const auto bytes_at_start = stream.stats().bytes_in;
    auto last_poll = Clock::now();
    bool film_seen = false;
    while (true) {
        auto line = lines.next_line(config_.line_timeout);
        if (!line) {
            return line.error();
        }
        img.samples.insert(img.samples.end(), line->begin(), line->end());
        const auto state = detector.feed(*line);
        if (state == FilmDetector::State::in_film && !film_seen) {
            film_seen = true;
            if (auto r = device.panel_leds(kLedFilmSeen); !r) {
                return r;
            }
        }
        if (state == FilmDetector::State::ended) {
            result_.film_end = FilmEnd::film_passed;
            break;
        }
        if (img.rows() >= config_.film_row_budget) {
            result_.film_end = FilmEnd::row_budget;
            break;
        }
        if (!film_seen && Clock::now() - window_start >= config_.no_film_timeout) {
            result_.film_end = FilmEnd::no_film_timeout;
            break;
        }
        if (Clock::now() - last_poll >= config_.poll_interval) {
            last_poll = Clock::now();
            auto ev = device.poll_service();
            if (!ev) {
                return ev.error();
            }
            if (ev->pending) {
                result_.events.push_back(std::format(
                    "film window line {}: service light 0x{:02x}{}", img.rows(),
                    ev->light_status,
                    ev->dx_flags ? std::format(" DX flags 0x{:02x}", *ev->dx_flags) : ""));
            }
        }
        if (auto c = check_cancel(config_.cancel, "during the film window"); !c) {
            return c;
        }
    }
    // Line period from what the device delivered during the window, not
    // from how fast this thread consumed it (a slow consumer lags behind
    // a ring that is still filling at the device's rate).
    const double window_s = seconds_since(window_start);
    const double delivered_lines =
        static_cast<double>(stream.stats().bytes_in - bytes_at_start) /
        static_cast<double>(2 * fg.samples_per_line());
    // The HOST ends the window: acquire off (stream stops), LEDs.
    if (auto r = device.panel_leds(kLedFilmEnd); !r) {
        return r;
    }
    if (auto r = device.acquire(false); !r) {
        return r;
    }
    result_.film_start_line = detector.film_start();
    result_.film_end_line = detector.film_end();
    result_.resyncs = lines.resyncs();
    result_.line_period_ms = delivered_lines < 1 ? 0 : window_s * 1000.0 / delivered_lines;
    result_.phases.push_back({"film window", seconds_since(t)});
    if (result_.film_end == FilmEnd::row_budget) {
        return run_out(device);
    }
    return {};
}

// The row cap ended the image window, but the film may still be in the
// path: stopping the motor there left a roll stuck (2026-10-13, film3).
// The OEM never stops the transport mid-film (OEM_RE.md §7 step 5), so
// keep it running until the DX sensor reads clear. Film present = DX
// flags & 0x30 (film2, 2026-10-10: 0x20/0x30 while the strip passed,
// 0x10/0x00 after its tail). Frame time from film2: 9908 lines × 0.904 ms
// over 6 frames = 1.49 s at motor rate 25676, scaling with 1/rate.
VoidResult ScanRunner::run_out(ScanDevice& device) {
    const auto t = Clock::now();
    if (auto r = device.lamp_off(); !r) {
        return r;
    }
    const double frame_s = 1.49 * 25676.0 / std::max(1000.0, static_cast<double>(result_.motor_rate));
    const auto ms = [](double s) {
        return std::chrono::milliseconds(static_cast<long long>(s * 1000.0));
    };
    const auto clear =
        config_.run_out_clear.count() > 0 ? config_.run_out_clear : ms(2.0 * frame_s);
    const auto limit =
        config_.run_out_limit.count() > 0 ? config_.run_out_limit : ms(45.0 * frame_s);
    log::Logger::instance().log(
        log::Level::info,
        "run-out: row cap reached, motor runs on until the DX sensor is clear for {:.1f} s "
        "(limit {:.0f} s)",
        clear.count() / 1000.0, limit.count() / 1000.0);
    result_.run_out.ran = true;
    auto last_film = Clock::now();
    while (true) {
        if (auto c = check_cancel(config_.cancel, "during the run-out"); !c) {
            return c;
        }
        const auto now = Clock::now();
        if (now - last_film >= clear) {
            result_.run_out.cleared = true;
            break;
        }
        if (now - t >= limit) {
            break;
        }
        if (auto ev = device.poll_service(); !ev) { // keeps events acknowledged
            return ev.error();
        }
        auto dx = device.read_dx_flags();
        if (!dx) {
            return dx.error();
        }
        if (*dx != 0) {
            last_film = Clock::now();
        }
        std::this_thread::sleep_for(config_.poll_interval);
    }
    result_.run_out.seconds = seconds_since(t);
    if (!result_.run_out.cleared) {
        log::Logger::instance().log(log::Level::warn,
                                    "run-out: limit reached with film still reported - check "
                                    "the film path before the next scan");
    }
    result_.phases.push_back({"run-out", result_.run_out.seconds});
    return {};
}

} // namespace pakon::scan
