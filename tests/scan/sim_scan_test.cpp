// Simulated scans against a device that streams on its own clock — the
// primary proof that a scan works (docs/STATUS.md).
//
// f1/f2 are the two cases that FAILED on the old runner (commit c3f9c3e,
// WILL_FAIL there): F1 lines lost while the command block ran, F2 a
// window that only ended on device quiescence never ended. They pass on
// the rebuilt runner, together with every configuration, first light,
// no film, late film, a slow consumer, a corrupt EEPROM, and a fault
// injected across the whole session with the teardown sent exactly once.

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <stdexcept>
#include <thread>

#include "pakon/eeprom/eeprom.hpp"
#include "pakon/ppb/client.hpp"
#include "pakon/scan/runner.hpp"
#include "pakon/scan/sidecar.hpp"
#include "pakon/stream/image_stream.hpp"
#include "support/sim_device.hpp"
#include "support/test_harness.hpp"

namespace {

using namespace std::chrono_literals;
using namespace pakon;

class Forward final : public usb::IUsbTransport {
public:
    explicit Forward(sim::SimDevice& s) : sim_(s) {}
    Result<std::vector<std::uint8_t>> command_exchange(std::span<const std::uint8_t> f) override {
        return sim_.command_exchange(f);
    }
    Result<std::vector<std::uint8_t>> bulk_read(std::uint8_t ep, std::size_t n) override {
        return sim_.bulk_read(ep, n);
    }
    Result<std::unique_ptr<usb::IBulkInPipe>> open_bulk_in(std::uint8_t ep, std::size_t n) override {
        return sim_.open_bulk_in(ep, n);
    }
    Result<std::vector<std::uint8_t>> control_read(std::uint8_t r, std::uint16_t v,
                                                   std::uint16_t i, std::uint16_t n) override {
        return sim_.control_read(r, v, i, n);
    }
    VoidResult control_write(std::uint8_t r, std::uint16_t v, std::uint16_t i) override {
        return sim_.control_write(r, v, i);
    }
    const usb::DeviceInfo& device_info() const override { return sim_.device_info(); }

private:
    sim::SimDevice& sim_;
};

// Optionally slows every exchange after `after` frames (a busy consumer).
// When set, the channel throws once, on the first exchange after the
// motor-go command (an exception in the middle of the film window).
bool g_throw_after_go = false;

class Channel final : public scan::ICommandChannel {
public:
    Channel(ppb::Client& c, std::size_t after, std::chrono::milliseconds delay)
        : inner_(c), after_(after), delay_(delay) {}
    Result<ppb::Reply> exchange(const ppb::Frame& f) override {
        if (++count_ > after_ && delay_.count() > 0) {
            std::this_thread::sleep_for(delay_);
        }
        if (go_seen_ && g_throw_after_go) {
            g_throw_after_go = false;
            throw std::runtime_error("injected exception");
        }
        const auto bytes = f.serialize();
        go_seen_ = go_seen_ || (bytes && *bytes == std::vector<std::uint8_t>{0x04, 0x03, 0x44, 0x00, 0xA0});
        return inner_.exchange(f);
    }

private:
    scan::PpbCommandChannel inner_;
    std::size_t after_;
    std::chrono::milliseconds delay_;
    std::size_t count_{0};
    bool go_seen_{false};
};

// 4x the real line rate. Instrumented builds (TSan slows the consumer
// ~10x) set PAKON_SIM_SPEED lower: the ring would otherwise overflow -
// correctly - because the consumer really is slower than the device.
sim::SimConfig fast_sim() {
    sim::SimConfig c;
    const char* speed = std::getenv("PAKON_SIM_SPEED");
    c.speed = speed ? std::atof(speed) : 4.0;
    c.command_latency = 300us;
    c.lamp_ready_delay = 50ms;
    return c;
}

scan::RunnerConfig fast_runner(scan::ScanMode mode, scan::ScanKind kind) {
    scan::RunnerConfig r;
    r.mode = mode;
    r.kind = kind;
    r.settle_scale = 0.0;
    r.poll_interval = 20ms;
    r.warmup_timeout = 5s;
    r.line_timeout = 3s;
    r.first_light_lines = 64;
    r.film_row_budget = 4000;
    return r;
}

struct Outcome {
    bool ok{false};
    std::string error;
    scan::ScanResult result;
    scan::TeardownReport teardown;
    std::vector<std::vector<std::uint8_t>> frames;
    std::vector<std::string> violations;
    std::vector<std::string> unknown;
    std::uint64_t lost{0};
    bool acquiring{true};
    bool lamp{true};
    bool motor{true};
};

Outcome run(sim::SimConfig scfg, scan::RunnerConfig rcfg, stream::StreamConfig strcfg = {},
            std::size_t slow_after = SIZE_MAX, std::chrono::milliseconds slow = 0ms) {
    sim::SimDevice device(std::move(scfg));
    ppb::Client client(std::make_unique<Forward>(device));
    auto image = eeprom::read_sections(client.transport());
    EXPECT(image.has_value());
    if (image) {
        rcfg.unit = eeprom::parse(*image);
    }
    auto pipe = client.transport().open_bulk_in(0x86, strcfg.queued_reads);
    EXPECT(pipe.has_value());
    stream::ImageStream stream(std::move(*pipe), strcfg);
    Channel channel(client, slow_after, slow);
    Outcome o;
    auto runner = scan::ScanRunner::create(rcfg);
    EXPECT(runner.has_value());
    if (runner) {
        auto r = (*runner)->run(channel, stream);
        o.ok = r.has_value();
        if (r) {
            o.result = std::move(*r);
        } else {
            o.error = r.error().message;
            o.result = (*runner)->partial();
        }
        o.teardown = (*runner)->teardown_report();
    }
    // Let the producer notice the final control write.
    std::this_thread::sleep_for(5ms);
    o.frames = device.frames();
    o.violations = device.violations();
    o.unknown = device.unknown_writes();
    o.lost = device.lost_bytes();
    o.acquiring = device.acquiring();
    o.lamp = device.lamp_lit();
    o.motor = device.motor_running();
    return o;
}

// The teardown's nine frames, located by their registers (the control
// word value depends on the mode): ctrl-idle, lamp off, FIFO pair, DX
// stop, 0xA2, rate 0, go, 0xA2.
constexpr std::size_t kTeardownFrames = 10;

bool is_teardown_at(const std::vector<std::vector<std::uint8_t>>& f, std::size_t i) {
    if (i + kTeardownFrames > f.size()) {
        return false;
    }
    const auto eq = [&](std::size_t k, std::initializer_list<std::uint8_t> v) {
        return std::equal(v.begin(), v.end(), f[i + k].begin(), f[i + k].end());
    };
    return f[i].size() == 8 && f[i][2] == 0x44 && f[i][4] == 0x82 && f[i][5] == 0 &&
           (f[i][6] & 1) == 0 && eq(1, {0x02, 0x04, 0x40, 0x01, 0x80, 0x00}) &&
           eq(2, {0x02, 0x04, 0x10, 0x01, 0x84, 0x02}) && eq(3, {0x04, 0x03, 0x40, 0x00, 0x8A}) &&
           eq(4, {0x04, 0x03, 0x40, 0x00, 0x92}) && eq(5, {0x04, 0x03, 0x44, 0x00, 0xA2}) &&
           eq(6, {0x02, 0x05, 0x44, 0x02, 0xA5, 0x00, 0x00}) &&
           eq(7, {0x04, 0x03, 0x44, 0x00, 0xA0}) && eq(8, {0x04, 0x03, 0x44, 0x00, 0xA2}) &&
           eq(9, {0x02, 0x06, 0x44, 0x03, 0x82, 0x09, 0x17, 0x00});
}

void expect_teardown_exactly_once(const Outcome& o) {
    std::size_t found = 0;
    std::size_t at = 0;
    for (std::size_t i = 0; i < o.frames.size(); ++i) {
        if (is_teardown_at(o.frames, i)) {
            ++found;
            at = i;
        }
    }
    EXPECT_EQ(found, 1u);
    EXPECT_EQ(at + kTeardownFrames, o.frames.size()); // nothing after it
    EXPECT(o.teardown.ran);
    EXPECT_EQ(o.teardown.steps.size(), kTeardownFrames);
}

void expect_safe_end(const Outcome& o) {
    EXPECT(!o.acquiring);
    EXPECT(!o.lamp);
    EXPECT(!o.motor);
    EXPECT(o.violations.empty());
    for (const auto& v : o.violations) {
        std::fprintf(stderr, "  violation: %s\n", v.c_str());
    }
    EXPECT(o.unknown.empty());
    for (const auto& u : o.unknown) {
        std::fprintf(stderr, "  unknown write: %s\n", u.c_str());
    }
}

} // namespace

PAKON_TEST(f1_no_line_is_lost_while_the_window_streams) {
    auto scfg = fast_sim();
    scfg.speed = 1.0; // the real line rate
    scfg.command_latency = 2500us;
    scfg.film = sim::FilmModel{200, 400};
    auto rcfg = fast_runner({scan::Base::b4, false}, scan::ScanKind::film);
    rcfg.film_row_budget = 300;
    auto o = run(scfg, rcfg);
    EXPECT(o.ok);
    if (!o.ok) {
        std::fprintf(stderr, "  %s\n", o.error.c_str());
    }
    EXPECT_EQ(o.lost, 0u);
    EXPECT_EQ(o.result.stream.overflow_events, 0u);
    EXPECT_EQ(o.result.resyncs, 0u);
    EXPECT_EQ(o.result.film.rows(), 300u);
    EXPECT_EQ(o.result.film.geometry.samples_per_row, 3000u);
    // Real F-135+ Base-4 line period: 1875 × 0.48 µs = 0.9 ms.
    EXPECT(o.result.line_period_ms > 0.7 && o.result.line_period_ms < 1.3);
    expect_teardown_exactly_once(o);
    expect_safe_end(o);
}

PAKON_TEST(f2_a_film_window_ends_while_the_device_still_streams) {
    auto scfg = fast_sim(); // no film fed: the device would stream forever
    auto rcfg = fast_runner({scan::Base::b4, false}, scan::ScanKind::film);
    rcfg.no_film_timeout = 300ms;
    auto o = run(scfg, rcfg);
    EXPECT(o.ok);
    EXPECT(o.result.film_end == scan::FilmEnd::no_film_timeout);
    EXPECT(o.result.film.rows() > 0);
    expect_teardown_exactly_once(o);
    expect_safe_end(o);
}

// With PAKON_SESSION_DIR set, every_configuration writes each scan's
// command frames as <dir>/<mode>.jsonl (corpus schema; t = frame index
// in ms, no ep0/ep6 events) for tools/compare_sessions.py against the
// OEM captures.
static void dump_session(const std::vector<std::vector<std::uint8_t>>& frames, scan::ScanMode mode) {
    const char* dir = std::getenv("PAKON_SESSION_DIR");
    if (!dir) {
        return;
    }
    const std::string path = std::string(dir) + "/" + std::string(scan::mode_name(mode)) + ".jsonl";
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        return;
    }
    std::fprintf(f, "{\"d\":\"meta\",\"label\":\"sim-%s\",\"bridge\":\"sim_scan_test\"}\n",
                 std::string(scan::mode_name(mode)).c_str());
    for (std::size_t i = 0; i < frames.size(); ++i) {
        std::string hex;
        for (const auto b : frames[i]) {
            hex += std::format("{:02x}", b);
        }
        std::fprintf(f, "{\"t\":%.3f,\"d\":\"cmd\",\"hex\":\"%s\"}\n", i / 1000.0, hex.c_str());
    }
    std::fclose(f);
}

PAKON_TEST(every_configuration_scans_a_strip_end_to_end) {
    const scan::ScanMode modes[] = {{scan::Base::b4, false},  {scan::Base::b4, true},
                                    {scan::Base::b8, false},  {scan::Base::b8, true},
                                    {scan::Base::b16, false}, {scan::Base::b16, true}};
    const sim::EepromSpec spec; // this unit's Offsets 28/56/59
    for (const auto mode : modes) {
        auto scfg = fast_sim();
        scfg.film = sim::FilmModel{200, 600};
        auto o = run(scfg, fast_runner(mode, scan::ScanKind::film));
        dump_session(o.frames, mode);
        EXPECT(o.ok);
        if (!o.ok) {
            std::fprintf(stderr, "  %s: %s\n", std::string(scan::mode_name(mode)).c_str(),
                         o.error.c_str());
            continue;
        }
        const auto offset = spec.base[scan::base_index(mode.base)][0];
        const auto g = scan::film_geometry(mode, offset);
        EXPECT_EQ(o.result.film.geometry.samples_per_row, g.samples_per_line());
        EXPECT(o.result.film_end == scan::FilmEnd::film_passed);
        EXPECT(o.result.film_start_line.has_value());
        if (o.result.film_start_line && o.result.film_end_line) {
            const auto len = *o.result.film_end_line - *o.result.film_start_line;
            EXPECT(len > 560 && len < 640); // the simulated 600-line strip
        }
        // The motor rate is this unit's EEPROM speed, not unit 16402's.
        const auto& b = spec.base[scan::base_index(mode.base)];
        EXPECT_EQ(o.result.motor_rate, mode.ir ? b[2] : b[1]);
        EXPECT_EQ(o.lost, 0u);
        EXPECT_EQ(o.result.stream.overflow_events, 0u);
        expect_teardown_exactly_once(o);
        expect_safe_end(o);
    }
}

PAKON_TEST(first_light_keeps_dark_and_white_lines_and_never_moves_the_motor) {
    auto o = run(fast_sim(), fast_runner({scan::Base::b4, false}, scan::ScanKind::first_light));
    EXPECT(o.ok);
    EXPECT_EQ(o.result.white.rows(), 64u);
    EXPECT_EQ(o.result.white.geometry.samples_per_row, 1025u * 3u); // 3..1028 with Offset 28
    const auto& c = o.result.corrections;
    EXPECT(c.dark.lines == 32);
    EXPECT(c.white.lines == 32);
    // Per-line means for every averaged line; each dark round logs their
    // spread (film2 hardware run, 2026-10-10: the dark level jumped
    // between rounds at an unchanged offset).
    EXPECT_EQ(c.dark.line_means.size(), 3u);
    for (const auto& m : c.dark.line_means) {
        EXPECT_EQ(m.size(), 32u);
    }
    std::size_t dark_lines = 0;
    for (const auto& l : c.log) {
        if (l.starts_with("dark round")) {
            ++dark_lines;
            EXPECT(l.find(" | line means R ") != std::string::npos);
        }
    }
    EXPECT_EQ(dark_lines, c.dark_rounds);
    EXPECT(c.currents.r >= 1 && c.currents.r <= 4);
    EXPECT(c.currents.g >= 1 && c.currents.g <= 20);
    // Dark level converged into the OEM target band on the masked pixels.
    for (std::size_t ch = 0; ch < 3; ++ch) {
        double m = 0;
        for (std::size_t p = 0; p < 3; ++p) m += c.dark.planes[ch][p];
        m /= 3;
        EXPECT(m > 300 - 40 && m < 300 + 40);
    }
    // No motor command before the teardown.
    const std::size_t end = o.frames.size() - kTeardownFrames;
    for (std::size_t i = 0; i < end; ++i) {
        const auto& f = o.frames[i];
        const bool motor = f.size() >= 5 && f[2] == 0x44 &&
                           ((f[0] == 0x04 && (f[4] == 0xA0 || f[4] == 0xA1)) ||
                            (f[0] == 0x02 && f[4] == 0xA5));
        EXPECT(!motor);
    }
    // The sidecar carries the calibration references for later processing.
    const auto json = scan::sidecar_json(o.result);
    EXPECT(json.find("\"schema\":\"pakon-scan/1\"") != std::string::npos);
    EXPECT(json.find("\"serial\":17373") != std::string::npos);
    EXPECT(json.find("\"dark\":{\"lines\":32,\"pixels\":1025,\"planes\":[[") != std::string::npos);
    EXPECT(json.find("\"white\":{\"lines\":32") != std::string::npos);
    if (const char* dir = std::getenv("PAKON_SESSION_DIR")) {
        if (std::FILE* f = std::fopen((std::string(dir) + "/first_light.scan.json").c_str(), "w")) {
            std::fwrite(json.data(), 1, json.size(), f);
            std::fclose(f);
        }
    }
    expect_teardown_exactly_once(o);
    expect_safe_end(o);
}

PAKON_TEST(late_film_is_still_found) {
    auto scfg = fast_sim();
    scfg.film = sim::FilmModel{2500, 500};
    auto o = run(scfg, fast_runner({scan::Base::b4, false}, scan::ScanKind::film));
    EXPECT(o.ok);
    EXPECT(o.result.film_end == scan::FilmEnd::film_passed);
    if (o.result.film_start_line) {
        EXPECT(*o.result.film_start_line > 2400);
    }
    expect_teardown_exactly_once(o);
    expect_safe_end(o);
}

PAKON_TEST(slow_consumer_overflow_fails_the_scan_and_still_tears_down_once) {
    stream::StreamConfig small;
    small.ring_bytes = 256 * 1024;
    // After the init block every exchange takes 150 ms: the stream keeps
    // flowing into a 256 KiB ring that nobody drains in time.
    auto o = run(fast_sim(), fast_runner({scan::Base::b4, false}, scan::ScanKind::first_light),
                 small, 45, 150ms);
    EXPECT(!o.ok);
    EXPECT(o.result.stream.overflow_events >= 1);
    expect_teardown_exactly_once(o);
    expect_safe_end(o);
}

PAKON_TEST(corrupt_eeprom_falls_back_loudly_and_still_scans) {
    auto scfg = fast_sim();
    sim::EepromSpec spec;
    spec.corrupt_all = true;
    scfg.eeprom = sim::make_eeprom(spec);
    scfg.film = sim::FilmModel{200, 400};
    auto o = run(scfg, fast_runner({scan::Base::b4, false}, scan::ScanKind::film));
    EXPECT(o.ok);
    EXPECT(o.result.unit.uses_fallback());
    EXPECT_EQ(o.result.motor_rate, 25726); // unit 16402's speed, flagged as fallback
    expect_teardown_exactly_once(o);
    expect_safe_end(o);
}

PAKON_TEST(a_fault_at_any_frame_sends_the_teardown_exactly_once) {
    // Count the frames of a clean run, then inject a fault across it.
    auto clean = run(fast_sim(), fast_runner({scan::Base::b4, false}, scan::ScanKind::first_light));
    EXPECT(clean.ok);
    const std::size_t total = clean.frames.size();
    const std::size_t teardown_at = total > kTeardownFrames + 3 ? total - kTeardownFrames - 3 : 0;
    const bool full = std::getenv("PAKON_FULL_FAULT_SWEEP") != nullptr;
    const std::size_t stride = full ? 1 : 7;
    std::size_t runs = 0;
    for (std::size_t at = 0; at < total; at += (at < 40 || at >= teardown_at) ? 1 : stride) {
        for (const auto kind : {sim::FaultKind::transport_error, sim::FaultKind::bad_status}) {
            auto scfg = fast_sim();
            scfg.fault_at_frame = std::pair{at, kind};
            scfg.fault_sticky = (at % 3) == 0 && kind == sim::FaultKind::transport_error;
            auto o = run(scfg, fast_runner({scan::Base::b4, false}, scan::ScanKind::first_light));
            ++runs;
            // Frame counts vary a little with the warm-up poll count, so
            // classify by outcome: an abort, or a completed scan whose
            // teardown reports the failed step (or a fault that landed
            // past the end of this run).
            if (o.ok) {
                EXPECT(!o.teardown.all_ok() || at + kTeardownFrames >= o.frames.size() || at >= o.frames.size());
            }
            expect_teardown_exactly_once(o);
            EXPECT(o.violations.empty());
            if (!scfg.fault_sticky && at < teardown_at) {
                EXPECT(!o.acquiring);
                EXPECT(!o.lamp);
                EXPECT(!o.motor);
            }
        }
    }
    std::printf("  fault sweep: %zu runs over %zu frames (stride %zu)\n", runs, total, stride);
}

PAKON_TEST(an_exception_mid_film_still_tears_down_once) {
    auto scfg = fast_sim();
    scfg.film = sim::FilmModel{200, 600};
    g_throw_after_go = true;
    auto o = run(scfg, fast_runner({scan::Base::b4, false}, scan::ScanKind::film));
    EXPECT(!o.ok);
    EXPECT(o.error.find("injected exception") != std::string::npos);
    EXPECT(!g_throw_after_go); // it did fire
    expect_teardown_exactly_once(o);
    expect_safe_end(o);
}

PAKON_TEST(a_row_cap_that_cannot_fit_in_memory_fails_before_the_motor_moves) {
    auto rcfg = fast_runner({scan::Base::b16, true}, scan::ScanKind::film);
    rcfg.film_row_budget = static_cast<std::size_t>(-1) / 2;
    auto o = run(fast_sim(), rcfg);
    EXPECT(!o.ok);
    EXPECT(o.error.find("too large") != std::string::npos);
    // No motor go before the teardown (whose tail is rate 0 -> go -> idle).
    for (std::size_t i = 0; i + kTeardownFrames < o.frames.size(); ++i) {
        const auto& f = o.frames[i];
        EXPECT(!(f.size() == 5 && f[2] == 0x44 && f[4] == 0xA0));
    }
    expect_teardown_exactly_once(o);
    expect_safe_end(o);
}

int main() { return pakon::test::run_all(); }
