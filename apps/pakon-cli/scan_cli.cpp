// The `scan` and `eeprom` commands - see scan_cli.hpp for the safety
// rules. Everything before run_live() is argument handling and plan
// rendering: no usb:: entry point is reached outside the single
// SessionOpener call (tests/cli/scan_cli_test.cpp pins that).

#include "scan_cli.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>

#include "interrupt.hpp"
#include "pakon/eeprom/eeprom.hpp"
#include "pakon/image/raw_writer.hpp"
#include "pakon/image/tiff.hpp"
#include "pakon/scan/runner.hpp"
#include "pakon/scan/sidecar.hpp"
#include "pakon/stream/image_stream.hpp"

namespace pakon::cli {
namespace {

enum class Mode { unset, dry_run, live };

struct Options {
    Mode mode{Mode::unset};
    std::string config;
    scan::ScanMode scan_mode{};
    bool first_light{false};
    std::size_t film_rows{0};
    std::size_t first_light_lines{256};
    std::string out_prefix;
};

struct ParseOutcome {
    int code{2};
    Options options;
    std::string error;
};

std::optional<std::size_t> parse_count(const std::string& text) {
    std::size_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || ec != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

ParseOutcome parse_options(const std::vector<std::string>& args) {
    ParseOutcome outcome;
    Options& o = outcome.options;
    bool have_film_rows = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        const auto value = [&]() -> const std::string* {
            return i + 1 < args.size() ? &args[++i] : nullptr;
        };
        if (arg == "--dry-run" || arg == "--live-scan") {
            const Mode m = arg == "--dry-run" ? Mode::dry_run : Mode::live;
            if (o.mode != Mode::unset && o.mode != m) {
                outcome.error = "--dry-run and --live-scan are mutually exclusive";
                return outcome;
            }
            o.mode = m;
        } else if (arg == "--first-light") {
            o.first_light = true;
        } else if (arg == "--config" || arg == "--out-prefix" || arg == "--film-rows" ||
                   arg == "--first-light-lines") {
            const auto* v = value();
            if (!v) {
                outcome.error = std::format("{} requires a value", arg);
                return outcome;
            }
            if (arg == "--config") {
                o.config = *v;
            } else if (arg == "--out-prefix") {
                o.out_prefix = *v;
            } else {
                const auto n = parse_count(*v);
                if (!n || *n == 0) {
                    outcome.error = std::format("{}: '{}' is not a positive integer", arg, *v);
                    return outcome;
                }
                if (arg == "--film-rows") {
                    o.film_rows = *n;
                    have_film_rows = true;
                } else {
                    o.first_light_lines = *n;
                }
            }
        } else {
            outcome.error = std::format("unknown scan argument '{}'", arg);
            return outcome;
        }
    }
    if (o.mode == Mode::unset) {
        outcome.error = "choose the mode explicitly: --dry-run (print the plan, no device) or "
                        "--live-scan (execute on the scanner)";
        return outcome;
    }
    if (o.config.empty()) {
        outcome.error = "--config is required: base4 | base4-ir | base8 | base8-ir | base16 | "
                        "base16-ir";
        return outcome;
    }
    const auto m = scan::parse_mode(o.config);
    if (!m) {
        outcome.error = std::format(
            "unknown --config '{}'; expected base4, base4-ir, base8, base8-ir, base16 or "
            "base16-ir",
            o.config);
        return outcome;
    }
    o.scan_mode = *m;
    if (o.first_light == have_film_rows) {
        outcome.error = "choose exactly one window: --first-light (calibration window only, "
                        "motor never engaged) or --film-rows <N> (film pass, N = hard row cap)";
        return outcome;
    }
    if (o.out_prefix.empty()) {
        outcome.error = "--out-prefix is required (the output paths are part of the plan)";
        return outcome;
    }
    outcome.code = 0;
    return outcome;
}

std::string hex_of(const ppb::Frame& frame) {
    auto bytes = frame.serialize();
    std::string out;
    if (bytes) {
        for (const auto b : *bytes) {
            out += std::format("{:02x}", b);
        }
    }
    return out;
}

void print_frames(std::FILE* out, std::span<const ppb::Frame> frames) {
    std::string line;
    for (const auto& f : frames) {
        const std::string h = hex_of(f);
        if (!line.empty() && line.size() + h.size() + 1 > 92) {
            std::fprintf(out, "    %s\n", line.c_str());
            line.clear();
        }
        line += line.empty() ? h : " " + h;
    }
    if (!line.empty()) {
        std::fprintf(out, "    %s\n", line.c_str());
    }
}

struct Outputs {
    std::string raw;
    std::string tiff;
    std::string text;
    std::string json;
};

Outputs outputs_for(const Options& o) {
    const std::string kind = o.first_light ? "white" : "film";
    return {o.out_prefix + "." + kind + ".pakraw", o.out_prefix + "." + kind + ".tiff",
            o.out_prefix + ".scan-stats.txt", o.out_prefix + ".scan.json"};
}

void print_preflight(const Options& o, std::FILE* out) {
    const auto& a = protocol::kF135Plus;
    const auto& mc = scan::constants(o.scan_mode.base);
    const auto outputs = outputs_for(o);
    std::fprintf(out, "pakon-cli scan - preflight (dry run)\n");
    std::fprintf(out, "No device is opened and no USB traffic is sent.\n\n");
    std::fprintf(out, "configuration:  %s (integration %u, DX word 0x%04x, %s)\n",
                 std::string(scan::mode_name(o.scan_mode)).c_str(),
                 scan::integration(o.scan_mode), scan::dx_word(o.scan_mode),
                 o.scan_mode.ir ? "IR lane on" : "IR off");
    std::fprintf(out, "window:         %s\n",
                 o.first_light
                     ? std::format("first light - Corrections + {} white lines, lamp on, motor "
                                   "never engaged",
                                   o.first_light_lines)
                           .c_str()
                     : std::format("film pass - ends on film end, no film within 45 s, or {} "
                                   "rows (hard cap)",
                                   o.film_rows)
                           .c_str());
    std::fprintf(out, "session:        one warm connection - usb::open_first(cold_ok=false) -> "
                      "Scanner::connect\n");
    std::fprintf(out, "                -> identify (incl. PICM 0x97=01, 0x03=01 page select); no "
                      "cold boot, no\n                firmware reload, no second connection\n");
    std::fprintf(out, "model gate:     F-135+ only (PICL 0x%02x / PICM 0x%02x)\n", a.light, a.motor);
    std::fprintf(out, "EEPROM:         read-only: 0xA4 wValue 0x00A5 + 0xA9, wIndex 0x1234 "
                      "(no 0xA2, no write-select)\n");
    std::fprintf(out, "                -> Offset, MotorSpeed%s, MotorAdjust for Base %d\n",
                 o.scan_mode.ir ? " IR" : "",
                 o.scan_mode.base == scan::Base::b4 ? 4 : o.scan_mode.base == scan::Base::b8 ? 8 : 16);
    std::fprintf(out, "endpoints:      commands OUT 0x01 / IN 0x81; image IN 0x86 with %zu queued "
                      "reads of %zu B\n                into a %zu MiB ring (overflow = error)\n",
                 stream::kQueuedReads, stream::kTransferBytes, stream::kRingBytes >> 20);
    std::fprintf(out, "geometry:       calibration pixels %u..Offset+%u, film Offset..Offset+%u%s\n",
                 mc.cal_start, mc.width, mc.width, mc.resample ? " (3/4 resample)" : "");
    std::fprintf(out, "outputs:        %s\n                %s (preview)\n                %s\n"
                      "                %s (settings, calibration references)\n",
                 outputs.raw.c_str(), outputs.tiff.c_str(), outputs.text.c_str(),
                 outputs.json.c_str());
    const auto init = scan::ScanRunner::init_frames(a);
    std::fprintf(out, "\nphases (exact frames where fixed):\n");
    std::fprintf(out, "  1 init block (%zu frames, byte-identical to base4.jsonl 0.951-1.067):\n",
                 init.size());
    print_frames(out, init);
    std::fprintf(out, "  2 lamp warm-up: poll 030110 every 200 ms; on an event read 0103400102 / "
                      "0103440102,\n    ack 0x06 [00][status]; until lamp status 0x83 bit1 "
                      "(stable); limit 300 s\n");
    std::fprintf(out, "  3 Corrections (data-dependent, bounded): stream started, calibration "
                      "geometry,\n    acquire bit on + DX start; dark servo <= 8 rounds; LED "
                      "current servo; on-time servo;\n    every measurement = 32 averaged lines "
                      "after a FIFO reset; LED currents clamped\n    to the board ceilings\n");
    if (o.first_light) {
        std::fprintf(out, "  4 %zu white lines, acquire off\n", o.first_light_lines);
    } else {
        std::fprintf(out, "  4 film: geometry at Offset, lamp on (film on-times), motor rate from "
                          "EEPROM, go,\n    acquire + DX start; lines consumed while the window "
                          "streams; the HOST ends it\n    (acquire off) - never by waiting for "
                          "the device to go quiet\n");
    }
    std::fprintf(out, "  5 teardown (exactly once, every path incl. Ctrl+C):\n");
    std::vector<ppb::Frame> td;
    const auto idle = scan::control_word(scan::film_geometry(o.scan_mode, 30), false);
    for (const auto& f : scan::teardown_frames(a, idle)) {
        td.push_back(f.frame);
    }
    print_frames(out, td);
    std::fprintf(out, "    (the first frame's control word depends on the mode; shown for this "
                      "mode)\n");
    std::fprintf(out, "Ctrl+C:         observed at bounded points; the teardown always runs\n");
    std::fprintf(out, "dry run only - nothing was sent; --live-scan executes this plan.\n");
}

// RGB preview, transposed so that width = travel (lines) and height =
// CCD pixels, the way the OEM export orients it. Linear, unprocessed.
VoidResult write_preview(const std::string& path, const image::RawImage& img, std::size_t pixels,
                         bool ir) {
    const std::size_t rows = img.rows();
    if (rows == 0 || pixels == 0) {
        return void_failure(ErrorKind::image_bad_geometry, "no lines to preview");
    }
    const std::size_t spr = img.geometry.samples_per_row;
    (void)ir;
    std::vector<std::uint16_t> rgb(rows * pixels * 3);
    for (std::size_t r = 0; r < rows; ++r) {
        for (std::size_t p = 0; p < pixels; ++p) {
            for (std::size_t c = 0; c < 3; ++c) {
                rgb[(p * rows + r) * 3 + c] = img.samples[r * spr + 3 * p + c];
            }
        }
    }
    return image::write_tiff16(path, static_cast<std::uint32_t>(rows),
                               static_cast<std::uint32_t>(pixels), 3, rgb);
}

void print_unit(std::FILE* out, const eeprom::UnitCalibration& u, scan::ScanMode m) {
    const auto& b = u.base[scan::base_index(m.base)];
    std::fprintf(out, "EEPROM:  serial %u, type %u, hw %u (section A %s, section B %s)\n", u.serial,
                 u.scanner_type, u.hardware_version, std::string(to_string(u.section_a)).c_str(),
                 std::string(to_string(u.section_b)).c_str());
    std::fprintf(out, "         %s: Offset %u, MotorSpeed %u, MotorSpeed IR %u, adjust %u/%u/%u/%u"
                      " -> rate %u\n",
                 std::string(scan::mode_name(m)).c_str(), b.offset, b.motor_speed,
                 b.motor_speed_ir, b.adjust[0], b.adjust[1], b.adjust[2], b.adjust[3],
                 eeprom::motor_rate(b, m.ir));
    for (const auto& n : u.notes) {
        std::fprintf(out, "         note: %s\n", n.c_str());
    }
}

void print_stats(std::FILE* out, const scan::ScanResult& r, std::size_t pixels) {
    const auto& s = r.stream;
    const auto& img = r.kind == scan::ScanKind::first_light ? r.white : r.film;
    std::fprintf(out, "scan-stats:\n");
    std::fprintf(out, "  rows %zu, samples/row %u, pixels/row %zu, bytes %zu\n", img.rows(),
                 img.geometry.samples_per_row, pixels, img.samples.size() * 2);
    if (r.kind == scan::ScanKind::film) {
        std::fprintf(out, "  line period %.3f ms, film end: %s", r.line_period_ms,
                     std::string(scan::to_string(r.film_end)).c_str());
        if (r.film_start_line) {
            std::fprintf(out, " (film lines %zu..%s)", *r.film_start_line,
                         r.film_end_line ? std::to_string(*r.film_end_line).c_str() : "?");
        }
        std::fprintf(out, "\n  motor rate %u\n", r.motor_rate);
    }
    std::fprintf(out, "  stream: in %llu B, out %llu B, discarded %llu B, transfers %llu, "
                      "ring high-water %zu B,\n          overflows %llu (%llu B), transfer "
                      "errors %llu, line resyncs %zu\n",
                 static_cast<unsigned long long>(s.bytes_in),
                 static_cast<unsigned long long>(s.bytes_out),
                 static_cast<unsigned long long>(s.bytes_discarded),
                 static_cast<unsigned long long>(s.transfers), s.high_water,
                 static_cast<unsigned long long>(s.overflow_events),
                 static_cast<unsigned long long>(s.overflow_bytes),
                 static_cast<unsigned long long>(s.transfer_errors), r.resyncs);
    const auto& c = r.corrections;
    std::fprintf(out, "  corrections: A/D offsets %d %d %d, gains %u %u %u, LED R%u G%u B%u IR%u, "
                      "on-time %.3f %.3f %.3f %.3f\n",
                 c.offsets[0], c.offsets[1], c.offsets[2], c.gains[0], c.gains[1], c.gains[2],
                 c.currents.r, c.currents.g, c.currents.b, c.currents.ir, c.duties.r, c.duties.g,
                 c.duties.b, c.duties.ir);
    std::fprintf(out, "               rounds: dark %zu, current %zu, on-time %zu\n", c.dark_rounds,
                 c.current_rounds, c.duty_rounds);
    for (const auto& p : r.phases) {
        std::fprintf(out, "  phase %-18s %8.3f s\n", p.name.c_str(), p.seconds);
    }
    for (const auto& e : r.events) {
        std::fprintf(out, "  event: %s\n", e.c_str());
    }
}

void print_teardown(std::FILE* out, const scan::TeardownReport& t) {
    if (!t.ran) {
        std::fprintf(out, "teardown: not reached\n");
        return;
    }
    std::fprintf(out, "teardown: %s\n", t.all_ok() ? "all steps acknowledged" : "STEP FAILURES");
    for (const auto& s : t.steps) {
        if (!s.ok) {
            std::fprintf(out, "  FAILED %s: %s\n", s.what.c_str(), s.error.c_str());
        }
    }
}

int run_live(const Options& o, const ScanCliDeps& deps, std::FILE* out, std::FILE* err) {
    if (!deps.open_session) {
        std::fprintf(err, "error: scan: no session opener wired\n");
        return 1;
    }
    auto opened = deps.open_session(); // the single device entry point
    if (!opened) {
        std::fprintf(err, "error: scan: open session: %s: %s\n",
                     std::string(to_string(opened.error().kind)).c_str(),
                     opened.error().message.c_str());
        return 1;
    }
    struct DisconnectGuard {
        scanner::Scanner* s;
        ~DisconnectGuard() {
            if (s) {
                s->disconnect();
            }
        }
    } guard{opened->scanner.get()};
    if (opened->identity.model != scanner::Model::f135_plus) {
        std::fprintf(err, "error: scan: requires an F-135+ (0x40/0x44); identify reported %s\n",
                     std::string(scanner::to_string(opened->identity.model)).c_str());
        return 1;
    }
    auto& transport = opened->client->transport();
    auto image = eeprom::read_sections(transport);
    if (!image) {
        std::fprintf(err, "error: scan: EEPROM read: %s\n", image.error().message.c_str());
        return 1;
    }
    const auto unit = eeprom::parse(*image);
    print_unit(out, unit, o.scan_mode);
    if (unit.uses_fallback()) {
        std::fprintf(err, "WARNING: this unit's EEPROM could not supply every field - the scan "
                          "uses FALLBACK values from capture unit 16402 for them (see notes)\n");
    }
    auto pipe = transport.open_bulk_in(protocol::scan::kImageEndpoint, stream::kQueuedReads);
    if (!pipe) {
        std::fprintf(err, "error: scan: image pipe: %s\n", pipe.error().message.c_str());
        return 1;
    }
    stream::ImageStream stream(std::move(*pipe));
    scan::RunnerConfig cfg;
    cfg.mode = o.scan_mode;
    cfg.kind = o.first_light ? scan::ScanKind::first_light : scan::ScanKind::film;
    cfg.addresses = opened->identity.addresses;
    cfg.unit = unit;
    cfg.first_light_lines = o.first_light_lines;
    cfg.film_row_budget = o.film_rows;
    cfg.cancel = &interrupt_token();
    auto runner = scan::ScanRunner::create(cfg);
    if (!runner) {
        std::fprintf(err, "error: scan: %s\n", runner.error().message.c_str());
        return 1;
    }
    scan::PpbCommandChannel commands(*opened->client);
    std::fprintf(out, "live scan: %s, %s\n", std::string(scan::mode_name(o.scan_mode)).c_str(),
                 o.first_light ? "first light" : std::format("film, cap {} rows", o.film_rows).c_str());
    if (!install_interrupt_handlers()) {
        std::fprintf(err, "warning: scan: interrupt handlers unavailable\n");
    }
    auto result = (*runner)->run(commands, stream);
    restore_interrupt_handlers();

    const auto geom = o.first_light
                          ? scan::calibration_geometry(o.scan_mode,
                                                       unit.base[scan::base_index(o.scan_mode.base)].offset,
                                                       o.scan_mode.ir)
                          : scan::film_geometry(o.scan_mode,
                                                unit.base[scan::base_index(o.scan_mode.base)].offset);
    const auto& r = result ? *result : (*runner)->partial();
    if (!result) {
        std::fprintf(err, "error: scan: %s: %s\n",
                     std::string(to_string(result.error().kind)).c_str(),
                     result.error().message.c_str());
    }
    print_stats(out, r, geom.pixels());
    print_teardown(result && r.teardown.all_ok() ? out : err, r.teardown);

    // Save whatever was captured, even after a failure (for analysis).
    const auto outputs = outputs_for(o);
    const auto& img = o.first_light ? r.white : r.film;
    int code = result && r.teardown.all_ok() ? 0 : 1;
    if (img.rows() > 0) {
        image::RawStreamMeta meta;
        meta.has_ir_lane = geom.ir;
        if (geom.ir) {
            meta.ir_region = std::pair<std::uint32_t, std::uint32_t>{
                static_cast<std::uint32_t>(3 * geom.pixels()), static_cast<std::uint32_t>(geom.pixels())};
        }
        if (auto w = image::write_stream_raw(outputs.raw, img, meta); !w) {
            std::fprintf(err, "error: write %s: %s\n", outputs.raw.c_str(), w.error().message.c_str());
            code = 1;
        } else {
            std::fprintf(out, "written: %s\n", outputs.raw.c_str());
        }
        if (auto w = write_preview(outputs.tiff, img, geom.pixels(), geom.ir); !w) {
            std::fprintf(err, "error: write %s: %s\n", outputs.tiff.c_str(), w.error().message.c_str());
            code = 1;
        } else {
            std::fprintf(out, "written: %s (linear RGB preview, unprocessed)\n", outputs.tiff.c_str());
        }
    }
    // Everything later processing needs besides the pixels (scan/sidecar.hpp).
    if (std::FILE* f = std::fopen(outputs.json.c_str(), "w")) {
        const auto json = scan::sidecar_json(r);
        std::fwrite(json.data(), 1, json.size(), f);
        std::fclose(f);
        std::fprintf(out, "written: %s\n", outputs.json.c_str());
    }
    if (std::FILE* f = std::fopen(outputs.text.c_str(), "w")) {
        print_unit(f, unit, o.scan_mode);
        print_stats(f, r, geom.pixels());
        print_teardown(f, r.teardown);
        for (const auto& line : r.corrections.log) {
            std::fprintf(f, "corrections: %s\n", line.c_str());
        }
        std::fclose(f);
        std::fprintf(out, "written: %s\n", outputs.text.c_str());
    }
    return code;
}

void print_usage_line(std::FILE* err) {
    std::fprintf(err, "usage: pakon-cli scan (--dry-run | --live-scan) --config <mode>\n"
                      "                     (--first-light [--first-light-lines N] | --film-rows N)"
                      " --out-prefix <path>\n");
}

} // namespace

int run_scan_command(const std::vector<std::string>& args, const ScanCliDeps& deps,
                     std::FILE* out, std::FILE* err) {
    const auto parsed = parse_options(args);
    if (parsed.code != 0) {
        std::fprintf(err, "error: scan: %s\n", parsed.error.c_str());
        print_usage_line(err);
        return parsed.code;
    }
    if (parsed.options.mode == Mode::dry_run) {
        print_preflight(parsed.options, out);
        return 0;
    }
    return run_live(parsed.options, deps, out, err);
}

int run_eeprom_command(const std::vector<std::string>& args, const ScanCliDeps& deps,
                       std::FILE* out, std::FILE* err) {
    std::string dump;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--out" && i + 1 < args.size()) {
            dump = args[++i];
        } else {
            std::fprintf(err, "error: eeprom: unknown argument '%s'\nusage: pakon-cli eeprom "
                              "[--out <file>]\n",
                         args[i].c_str());
            return 2;
        }
    }
    if (!deps.open_session) {
        std::fprintf(err, "error: eeprom: no session opener wired\n");
        return 1;
    }
    auto opened = deps.open_session();
    if (!opened) {
        std::fprintf(err, "error: eeprom: open session: %s\n", opened.error().message.c_str());
        return 1;
    }
    struct DisconnectGuard {
        scanner::Scanner* s;
        ~DisconnectGuard() {
            if (s) {
                s->disconnect();
            }
        }
    } guard{opened->scanner.get()};
    auto image = eeprom::read_sections(opened->client->transport());
    if (!image) {
        std::fprintf(err, "error: eeprom: %s\n", image.error().message.c_str());
        return 1;
    }
    const auto unit = eeprom::parse(*image);
    for (const auto m : {scan::ScanMode{scan::Base::b4, false}, scan::ScanMode{scan::Base::b4, true},
                         scan::ScanMode{scan::Base::b8, false}, scan::ScanMode{scan::Base::b8, true},
                         scan::ScanMode{scan::Base::b16, false},
                         scan::ScanMode{scan::Base::b16, true}}) {
        const auto& b = unit.base[scan::base_index(m.base)];
        std::fprintf(out, "%-10s Offset %3u  rate %5u\n", std::string(scan::mode_name(m)).c_str(),
                     b.offset, eeprom::motor_rate(b, m.ir));
    }
    print_unit(out, unit, {scan::Base::b4, false});
    if (unit.uses_fallback()) {
        std::fprintf(err, "WARNING: EEPROM sections unreadable - FALLBACK values shown\n");
    }
    if (!dump.empty()) {
        if (std::FILE* f = std::fopen(dump.c_str(), "wb")) {
            std::fwrite(image->data(), 1, image->size(), f);
            std::fclose(f);
            std::fprintf(out, "raw sections written to %s (0xFF = not read)\n", dump.c_str());
        }
    }
    return 0;
}

void print_scan_usage(std::FILE* out) {
    std::fprintf(out,
                 "scan - capture a scan (explicit opt-in; nothing runs by default)\n"
                 "  pakon-cli scan --dry-run ...    print the exact plan, open nothing\n"
                 "  pakon-cli scan --live-scan ...  run it over ONE warm session (F-135+)\n"
                 "  --config <mode>         base4 | base4-ir | base8 | base8-ir | base16 | base16-ir\n"
                 "  --first-light           calibration window only: Corrections + white lines,\n"
                 "                          lamp on, motor never engaged\n"
                 "  --first-light-lines <N> white lines to keep (default 256)\n"
                 "  --film-rows <N>         film pass; N = hard row cap (film end and a 45 s\n"
                 "                          no-film timeout end it earlier)\n"
                 "  --out-prefix <path>     writes <path>.{white|film}.pakraw, .tiff preview,\n"
                 "                          <path>.scan-stats.txt, <path>.scan.json\n"
                 "eeprom [--out <file>] - read-only EEPROM dump + parsed per-unit values\n"
                 "Ctrl+C during a live run stops at the next bounded point; the teardown\n"
                 "(lamp off, acquire off, motor stop, rate=0 -> go -> idle) always runs.\n"
                 "exit codes: 0 ok, 1 runtime failure, 2 usage error (no USB traffic)\n");
}

} // namespace pakon::cli
