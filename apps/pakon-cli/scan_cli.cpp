// The `scan` command - see scan_cli.hpp for the safety rules this file
// implements. Everything before run_live() is pure argument handling
// and plan rendering: no usb:: symbol is referenced anywhere outside
// the single SessionOpener call, which exists only on the --live-scan
// path (tests/cli/scan_cli_test.cpp pins both facts).

#include "scan_cli.hpp"

#include <charconv>
#include <format>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>

#include "interrupt.hpp"
#include "pakon/image/completion.hpp"
#include "pakon/image/raw_writer.hpp"
#include "pakon/image/source.hpp"
#include "pakon/protocol/scan_commands.hpp"
#include "pakon/scan/runner.hpp"
#include "pakon/scan/transport.hpp"

namespace pakon::cli {
namespace {

using image::AnyCompletion;
using image::ICompletionPolicy;
using image::NoProgressCompletion;
using image::RowBudgetCompletion;

// One bulk read that returns nothing counts as an idle tick; a window
// may only end after this many CONSECUTIVE ticks, so a single transient
// timeout can never be read as end-of-scan (requirement: never silently
// treat a timeout as a successful end).
constexpr std::size_t kMinIdleReads = 2;

// --- explicit configuration ------------------------------------------

enum class Mode { unset, dry_run, live };
enum class FilmEnd { unset, quiescence, rows };

// The six recorded configurations (protocol/commands.hpp, all six
// trigger values capture-confirmed). Only base4-ir-off has a byte-exact
// replayed phase plan (scan/plan.hpp): the other five sessions differ
// STRUCTURALLY from base4 (integration/offset/width ramp words, an
// extra enable_scan, different lamp masks and speed words), so running
// the base4 plan under their name would invent bytes. They parse, then
// are refused explicitly - never silently substituted.
struct ConfigChoice {
    protocol::ScanLineParams params;
    const char* capture;     // capture file behind the name
    bool plan_available;
};

std::optional<ConfigChoice> parse_config(std::string_view name) {
    if (name == "base4-ir-off") {
        return ConfigChoice{protocol::ScanLineParams::base4_ir_off, "base4.jsonl", true};
    }
    if (name == "base4-ir-on") {
        return ConfigChoice{protocol::ScanLineParams::base4_ir_on, "base4-ir.jsonl", false};
    }
    if (name == "base8-ir-off") {
        return ConfigChoice{protocol::ScanLineParams::base8_ir_off, "base8.jsonl", false};
    }
    if (name == "base8-ir-on") {
        return ConfigChoice{protocol::ScanLineParams::base8_ir_on, "base8-ir.jsonl", false};
    }
    if (name == "base16-ir-off") {
        return ConfigChoice{protocol::ScanLineParams::base16_ir_off, "base16.jsonl", false};
    }
    if (name == "base16-ir-on") {
        return ConfigChoice{protocol::ScanLineParams::base16_ir_on, "base16-ir.jsonl", false};
    }
    return std::nullopt;
}

struct Options {
    Mode mode{Mode::unset};
    std::string config; // raw --config value (required, validated)
    protocol::ScanLineParams line_params{protocol::ScanLineParams::base4_ir_off};
    std::size_t calibration_rows{0};
    std::size_t idle_reads{0};
    FilmEnd film_end{FilmEnd::unset};
    std::size_t film_rows{0};
    std::string out_prefix;
};

struct ParseOutcome {
    int code{2}; // 0 = parsed and fully validated
    Options options;
    std::string error;
};

std::optional<std::size_t> parse_count(const std::string& text) {
    if (text.empty()) {
        return std::nullopt;
    }
    std::size_t value = 0;
    const auto [end, ec] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

// Full validation happens BEFORE any mode-specific action: a usage
// error (exit 2) is decided while the opener cannot possibly have run.
ParseOutcome parse_options(const std::vector<std::string>& args) {
    ParseOutcome outcome;
    Options& o = outcome.options;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        auto next = [&]() -> const std::string* {
            return (i + 1 < args.size()) ? &args[i + 1] : nullptr;
        };
        auto next_count = [&](const char* flag, std::size_t& slot) -> bool {
            const auto* value = next();
            if (!value) {
                outcome.error = std::format("{} requires a value", flag);
                return false;
            }
            const auto parsed = parse_count(*value);
            if (!parsed) {
                outcome.error =
                    std::format("{}: '{}' is not a non-negative integer", flag, *value);
                return false;
            }
            slot = *parsed;
            ++i;
            return true;
        };

        if (arg == "--dry-run" || arg == "--live-scan") {
            const Mode mode = arg == "--dry-run" ? Mode::dry_run : Mode::live;
            if (o.mode != Mode::unset && o.mode != mode) {
                outcome.error = "--dry-run and --live-scan are mutually exclusive";
                return outcome;
            }
            o.mode = mode;
        } else if (arg == "--config") {
            const auto* value = next();
            if (!value) {
                outcome.error = "--config requires a value";
                return outcome;
            }
            o.config = *value;
            ++i;
        } else if (arg == "--calibration-rows") {
            if (!next_count(arg.c_str(), o.calibration_rows)) {
                return outcome;
            }
        } else if (arg == "--idle-reads") {
            if (!next_count(arg.c_str(), o.idle_reads)) {
                return outcome;
            }
        } else if (arg == "--film-rows") {
            if (!next_count(arg.c_str(), o.film_rows)) {
                return outcome;
            }
        } else if (arg == "--film-end") {
            const auto* value = next();
            if (!value) {
                outcome.error = "--film-end requires a value";
                return outcome;
            }
            if (*value == "quiescence") {
                o.film_end = FilmEnd::quiescence;
            } else if (*value == "rows") {
                o.film_end = FilmEnd::rows;
            } else {
                outcome.error =
                    std::format("--film-end must be quiescence or rows (got '{}')", *value);
                return outcome;
            }
            ++i;
        } else if (arg == "--out-prefix") {
            const auto* value = next();
            if (!value) {
                outcome.error = "--out-prefix requires a value";
                return outcome;
            }
            o.out_prefix = *value;
            ++i;
        } else {
            outcome.error = std::format("unknown scan argument '{}'", arg);
            return outcome;
        }
    }

    // Mode: explicit opt-in. No mode means nothing may run.
    if (o.mode == Mode::unset) {
        outcome.error =
            "choose the mode explicitly: --dry-run (print the plan, no device) or "
            "--live-scan (execute on the scanner)";
        return outcome;
    }

    // Configuration: explicit selection, never defaulted.
    if (o.config.empty()) {
        outcome.error =
            "--config is required: select the scan configuration explicitly "
            "(base4-ir-off is the capture-validated one)";
        return outcome;
    }
    const auto choice = parse_config(o.config);
    if (!choice) {
        outcome.error = std::format(
            "unknown --config '{}'; expected one of base4-ir-off, base4-ir-on, "
            "base8-ir-off, base8-ir-on, base16-ir-off, base16-ir-on",
            o.config);
        return outcome;
    }
    if (!choice->plan_available) {
        outcome.error = std::format(
            "--config {}: recorded in the captures ({}) but its phase plan is not "
            "replayed yet - only base4-ir-off has a byte-exact plan; refusing to "
            "substitute another configuration's frames",
            o.config, choice->capture);
        return outcome;
    }
    o.line_params = choice->params;

    // Completion: every budget comes from the command line. No byte
    // budget exists anywhere, and one timeout can never end a window.
    if (o.calibration_rows == 0) {
        outcome.error =
            "--calibration-rows is required and must be >= 1 (explicit W1 row "
            "budget; there is no default)";
        return outcome;
    }
    if (o.idle_reads == 0) {
        outcome.error =
            "--idle-reads is required (explicit quiescence limit in bulk 0x86 reads)";
        return outcome;
    }
    if (o.idle_reads < kMinIdleReads) {
        outcome.error = std::format(
            "--idle-reads {} is below the minimum {}: a single transient timeout "
            "must never be treated as end-of-scan",
            o.idle_reads, kMinIdleReads);
        return outcome;
    }
    if (o.film_end == FilmEnd::unset) {
        outcome.error =
            "--film-end is required: quiescence (film window ends when the device "
            "stops feeding) or rows (explicit budget plus the quiescence safety)";
        return outcome;
    }
    if (o.film_end == FilmEnd::rows && o.film_rows == 0) {
        outcome.error = "--film-end rows requires --film-rows >= 1";
        return outcome;
    }
    if (o.film_end == FilmEnd::quiescence && o.film_rows != 0) {
        outcome.error =
            "--film-rows was given but --film-end is quiescence; pass --film-end "
            "rows or drop --film-rows (an ignored argument is not allowed)";
        return outcome;
    }
    if (o.out_prefix.empty()) {
        outcome.error = "--out-prefix is required (the output paths are part of the plan)";
        return outcome;
    }

    outcome.code = 0;
    return outcome;
}

// --- completion policies (the ones the preflight prints) ---------------

std::shared_ptr<const ICompletionPolicy> make_calibration_policy(const Options& o) {
    // W1 is not device-signalled in any captured session; an explicit
    // row budget is the only workable trigger, with the quiescence
    // limit as the stall safety (image/completion.hpp).
    return std::make_shared<const AnyCompletion>(
        std::vector<std::shared_ptr<const ICompletionPolicy>>{
            std::make_shared<const RowBudgetCompletion>(o.calibration_rows),
            std::make_shared<const NoProgressCompletion>(o.idle_reads)});
}

std::shared_ptr<const ICompletionPolicy> make_film_policy(const Options& o) {
    auto quiescence = std::make_shared<const NoProgressCompletion>(o.idle_reads);
    if (o.film_end == FilmEnd::rows) {
        return std::make_shared<const AnyCompletion>(
            std::vector<std::shared_ptr<const ICompletionPolicy>>{
                std::make_shared<const RowBudgetCompletion>(o.film_rows), quiescence});
    }
    return quiescence;
}

std::string calibration_text(const Options& o) {
    return std::format(
        "any-of [row-budget ({} rows), no-progress ({} consecutive idle reads)]",
        o.calibration_rows, o.idle_reads);
}

std::string film_text(const Options& o) {
    if (o.film_end == FilmEnd::rows) {
        return std::format(
            "any-of [row-budget ({} rows), no-progress ({} consecutive idle reads)]",
            o.film_rows, o.idle_reads);
    }
    return std::format(
        "no-progress ({} consecutive idle reads) - ends when the device stops "
        "feeding",
        o.idle_reads);
}

// --- preflight rendering ------------------------------------------------

std::string hex_of(std::span<const std::uint8_t> bytes) {
    std::string out;
    for (const auto byte : bytes) {
        out += std::format("{:02x}", byte);
    }
    return out;
}

std::string frame_hex(const ppb::Frame& frame) {
    auto bytes = frame.serialize();
    if (!bytes) {
        return "<unserializable>";
    }
    return hex_of(*bytes);
}

void print_frames(std::FILE* out, const char* phase,
                  const std::vector<ppb::Frame>& frames) {
    std::fprintf(out, "  %s (%zu):\n", phase, frames.size());
    std::string line;
    for (const auto& frame : frames) {
        const std::string hex = frame_hex(frame);
        if (!line.empty() && line.size() + hex.size() + 1 > 92) {
            std::fprintf(out, "    %s\n", line.c_str());
            line.clear();
        }
        if (!line.empty()) {
            line += ' ';
        }
        line += hex;
    }
    if (!line.empty()) {
        std::fprintf(out, "    %s\n", line.c_str());
    }
}

// Dry-run output: the exact plan. Builds the same phase plans the live
// run will send - no device object exists anywhere in this path.
void print_preflight(const Options& o, std::FILE* out) {
    const auto& addresses = protocol::kF135Plus;
    scan::ScanPlanParameters params{};
    params.line_params = o.line_params;
    const auto init = scan::init_plan(addresses, params);
    const auto service = scan::service_plan(addresses);
    const auto calibration = scan::calibration_plan(addresses, params);
    const auto transport = scan::transport_plan(addresses, params);
    const auto teardown = scan::teardown_plan(addresses, params);

    std::fprintf(out, "pakon-cli scan - preflight (dry run)\n");
    std::fprintf(out, "No device is opened and no USB traffic is sent.\n\n");

    std::fprintf(out, "mode:              dry run (rerun with --live-scan to execute)\n");
    std::fprintf(out, "configuration:     %s (ScanLineParams 0x%04x) - explicit --config\n",
                 o.config.c_str(),
                 static_cast<unsigned>(protocol::ScanLineParams::base4_ir_off));
    std::fprintf(out, "plan basis:        byte-exact replay of capture base4.jsonl\n");
    std::fprintf(out, "                   (alibosworth/pakon-captures, F-135+ serial 16402)\n");
    std::fprintf(out, "live session:      one warm connection - usb::open_first(cold_ok=false)\n");
    std::fprintf(out, "                   -> Scanner::connect -> identify; no cold boot, no\n");
    std::fprintf(out, "                   firmware reload, no second connection; commands and\n");
    std::fprintf(out, "                   image reads share that transport\n");
    std::fprintf(out, "model gate:        F-135+ only - identify() must report 0x40/0x44 or the\n");
    std::fprintf(out, "                   run refuses before any frame is sent\n");
    std::fprintf(out, "addresses:         light 0x%02x, motor 0x%02x (assumed here, verified live)\n",
                 addresses.light, addresses.motor);
    std::fprintf(out, "endpoints:         commands OUT 0x%02x / IN 0x%02x (allow-list 0x10/0x20/\n",
                 protocol::scan::kCommandOutEndpoint, protocol::scan::kCommandInEndpoint);
    std::fprintf(out, "                   0x24/0x40/0x44 only - never 0x22/0x26/0x42/0x46, never\n");
    std::fprintf(out, "                   0xA2/0xA4); image IN 0x%02x, %zu-byte reads\n",
                 image::kImageEndpoint, image::kCaptureChunkBytes);
    std::fprintf(out, "completion:\n");
    std::fprintf(out, "  calibration W1:  %s\n", calibration_text(o).c_str());
    std::fprintf(out, "  film W2:         %s\n", film_text(o).c_str());
    std::fprintf(out, "  timeout rule:    a single idle read never ends a window\n");
    std::fprintf(out, "                   (--idle-reads >= %zu enforced); no byte budget anywhere\n",
                 kMinIdleReads);
    std::fprintf(out, "  idle deadline:   %lu ms per image IN 0x%02x read (pipe policy set at\n",
                 usb::kPipeTimeoutMs, image::kImageEndpoint);
    std::fprintf(out, "                   open) -> quiescence waits idle-reads x deadline\n");
    std::fprintf(out, "outputs:\n");
    std::fprintf(out, "  calibration W1:  %s\n", (o.out_prefix + ".calibration.pakraw").c_str());
    std::fprintf(out, "  film W2:         %s\n", (o.out_prefix + ".film.pakraw").c_str());
    std::fprintf(out, "                   PAKRAW01 stream-raw; IR lane off; rotation NOT applied\n");
    std::fprintf(out, "service polls:     %s repeats between init and service until the device\n",
                 frame_hex(protocol::scan::read_service_status(addresses.light)).c_str());
    std::fprintf(out, "                   reports service-wanted (captured replies: idle 0103400800,\n");
    std::fprintf(out, "                   service-wanted 0103408802 - fixture provenance header)\n");
    std::fprintf(out, "                   bounded: %lu ms deadline (capture: wanted 6.5 s after\n",
                 scan::kServiceWaitTimeoutMs);
    std::fprintf(out, "                   init), %lu ms cadence, progress logged; Ctrl+C ->\n",
                 scan::kServicePollIntervalMs);
    std::fprintf(out, "                   cancel at a bounded point, then best-effort teardown\n");
    std::fprintf(out, "planned frames:   init %zu, service %zu, calibration %zu, transport %zu,\n",
                 init.size(), service.size(), calibration.size(), transport.size());
    std::fprintf(out, "                   teardown %zu\n", teardown.size());
    std::fprintf(out, "frames (exact bytes, send order):\n");
    print_frames(out, "init", init);
    print_frames(out, "service", service);
    print_frames(out, "calibration", calibration);
    print_frames(out, "transport", transport);
    print_frames(out, "teardown", teardown);
    std::fprintf(out, "dry run only - nothing was sent; --live-scan executes this plan "
                      "(separate approval).\n");
}

// --- live path (the ONLY code that can touch the device) ----------------

int run_live(const Options& o, const ScanCliDeps& deps, std::FILE* out, std::FILE* err) {
    if (!deps.open_session) {
        std::fprintf(err, "error: scan: no session opener wired\n");
        return 1;
    }

    // The single device entry point of this command - called at most
    // once, only here, only under --live-scan.
    auto opened = deps.open_session();
    if (!opened) {
        std::fprintf(err, "error: scan: open session: %s: %s\n",
                     std::string(to_string(opened.error().kind)).c_str(),
                     opened.error().message.c_str());
        return 1;
    }

    // Close the one session on every exit below (no documented close
    // frame exists - disconnect drops the transport, scanner.cpp).
    struct DisconnectGuard {
        scanner::Scanner* instance;
        ~DisconnectGuard() {
            if (instance) {
                instance->disconnect();
            }
        }
    } guard{opened->scanner.get()};

    if (opened->identity.model != scanner::Model::f135_plus) {
        std::fprintf(err,
                     "error: scan: live scan requires an F-135+ (the captured plans "
                     "target 0x40/0x44); identify reported %s - refusing\n",
                     std::string(scanner::to_string(opened->identity.model)).c_str());
        return 1;
    }

    scan::ScanRunnerConfig config;
    config.addresses = opened->identity.addresses;
    config.plan = scan::ScanPlanParameters{};
    config.plan.line_params = o.line_params;
    config.calibration_completion = make_calibration_policy(o);
    config.transport_completion = make_film_policy(o);
    config.cancel = &interrupt_token();

    auto runner = scan::ScanRunner::create(config);
    if (!runner) {
        std::fprintf(err, "error: scan: runner: %s: %s\n",
                     std::string(to_string(runner.error().kind)).c_str(),
                     runner.error().message.c_str());
        return 1;
    }

    // ONE session, two channels: PPB commands (0x01/0x81) through the
    // allow-listed client, pixels (0x86) through the same transport.
    // The image source carries the interrupt token too, so Ctrl+C is
    // observed before each bulk read (within one pipe deadline).
    scan::PpbCommandChannel commands(*opened->client);
    scan::UsbImageSource images(opened->client->transport(), &interrupt_token());

    std::fprintf(out, "live scan: %s, calibration budget %zu rows, film end %s, "
                      "idle limit %zu reads\n",
                 o.config.c_str(), o.calibration_rows,
                 o.film_end == FilmEnd::rows ? "rows" : "quiescence", o.idle_reads);

    // Interrupt handling spans exactly the runner: the handler latches
    // the token, the runner observes it at bounded points and unwinds
    // through the best-effort teardown + disconnect. Restored as soon
    // as the run returns so a Ctrl+C during the file writes below
    // terminates the process instead of being latched and ignored.
    if (!install_interrupt_handlers()) {
        std::fprintf(err,
                     "warning: scan: interrupt handlers unavailable - Ctrl+C will "
                     "not stop the run cleanly\n");
    }
    auto result = (*runner)->run(commands, images);
    restore_interrupt_handlers();
    if (!result) {
        std::fprintf(err, "error: scan: %s: %s\n",
                     std::string(to_string(result.error().kind)).c_str(),
                     result.error().message.c_str());
        std::fprintf(err, "session state: %s (terminal - not retried; %s)\n",
                     std::string(scan::to_string((*runner)->session().state())).c_str(),
                     (*runner)->teardown_attempted()
                         ? "the best-effort teardown ran before this point"
                         : "no teardown was attempted (fault before acquisition "
                           "started)");
        return 1;
    }

    const std::string calibration_path = o.out_prefix + ".calibration.pakraw";
    const std::string film_path = o.out_prefix + ".film.pakraw";
    const image::RawStreamMeta meta{}; // base4-ir-off: no IR lane, region unknown
    if (auto w = image::write_stream_raw(calibration_path, result->calibration, meta);
        !w) {
        std::fprintf(err, "error: scan: write %s: %s\n", calibration_path.c_str(),
                     w.error().message.c_str());
        return 1;
    }
    if (auto w = image::write_stream_raw(film_path, result->film, meta); !w) {
        std::fprintf(err, "error: scan: write %s: %s\n", film_path.c_str(),
                     w.error().message.c_str());
        return 1;
    }

    const auto print_window = [](std::FILE* sink, const char* label,
                                 const image::WindowReport& report,
                                 std::size_t samples_per_row,
                                 const std::string& path) {
        std::fprintf(sink,
                     "%s: %zu rows, %zu bytes, %zu samples/row, completion: %s\n",
                     label, report.rows, report.bytes_received, samples_per_row,
                     report.completion);
        if (report.truncated_tail_bytes != 0) {
            std::fprintf(sink,
                         "  dropped device-cut tail: %zu bytes (partial row at end of "
                         "stream)\n",
                         report.truncated_tail_bytes);
        }
        std::fprintf(sink, "  written: %s\n", path.c_str());
    };
    std::fprintf(out, "scan complete (session state: %s)\n",
                 std::string(scan::to_string((*runner)->session().state())).c_str());
    print_window(out, "calibration W1", result->calibration_report,
                 result->calibration.geometry.samples_per_row, calibration_path);
    print_window(out, "film W2", result->transport_report,
                 result->film.geometry.samples_per_row, film_path);
    return 0;
}

} // namespace

int run_scan_command(const std::vector<std::string>& args, const ScanCliDeps& deps,
                     std::FILE* out, std::FILE* err) {
    const auto parsed = parse_options(args);
    if (parsed.code != 0) {
        std::fprintf(err, "error: scan: %s\n", parsed.error.c_str());
        std::fprintf(err,
                     "usage: pakon-cli scan (--dry-run | --live-scan) --config <name> "
                     "--calibration-rows <N> --idle-reads <N>\n"
                     "                     --film-end <quiescence|rows> [--film-rows <N>] "
                     "--out-prefix <path>\n");
        return parsed.code;
    }
    if (parsed.options.mode == Mode::dry_run) {
        print_preflight(parsed.options, out);
        return 0;
    }
    return run_live(parsed.options, deps, out, err);
}

void print_scan_usage(std::FILE* out) {
    std::fprintf(out,
                 "scan - capture a scan session (explicit opt-in; nothing runs by default)\n"
                 "  pakon-cli scan --dry-run ...   print the exact plan (PPB frames,\n"
                 "                                endpoint usage, completion policies,\n"
                 "                                output paths) without opening the\n"
                 "                                scanner or sending USB traffic\n"
                 "  pakon-cli scan --live-scan ... execute it over ONE warm session\n"
                 "                                (F-135+ only; no cold boot, no firmware\n"
                 "                                reload, no second connection)\n"
                 "required (both modes):\n"
                 "  --config <name>          base4-ir-off (capture-validated plan) |\n"
                 "                           base4-ir-on | base8-ir-off | base8-ir-on |\n"
                 "                           base16-ir-off | base16-ir-on (the other five\n"
                 "                           have no replayed plan yet and are refused,\n"
                 "                           never substituted)\n"
                 "  --calibration-rows <N>   explicit W1 row budget (N >= 1)\n"
                 "  --idle-reads <N>         quiescence limit in bulk reads (N >= %zu - one\n"
                 "                           timeout must never end a window)\n"
                 "  --film-end <mode>        quiescence | rows\n"
                 "  --film-rows <N>          film row budget (required with --film-end rows)\n"
                 "  --out-prefix <path>      writes <path>.calibration.pakraw and\n"
                 "                           <path>.film.pakraw (PAKRAW01 stream-raw)\n"
                 "interrupts:       Ctrl+C during a live run stops at the next bounded\n"
                 "                   point (at most one %lu ms pipe deadline) and runs\n"
                 "                   the best-effort teardown before exiting\n"
                 "exit codes: 0 ok, 1 runtime failure, 2 usage error (usage errors are\n"
                 "decided before any device entry point - exit 2 means no USB traffic)\n",
                 kMinIdleReads, usb::kPipeTimeoutMs);
}

} // namespace pakon::cli
