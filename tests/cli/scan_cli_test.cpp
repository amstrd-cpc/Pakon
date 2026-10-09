// CLI gating proofs for the `scan` command (apps/pakon-cli/scan_cli.hpp).
//
// The single device entry point of the command is the injected
// SessionOpener: these tests replace it with a counting trap and prove
// that
//   - the default invocation (no mode flags, or missing required
//     arguments) exits 2 without ever calling it, and
//   - --dry-run prints the full plan (frames, endpoints, completion
//     policies, output paths) and exits 0 without ever calling it, and
//   - the --live-scan path calls it EXACTLY once (bounded: a failed
//     open is reported, never retried).
//
// scan_cli.cpp references no usb:: symbol outside that opener, and
// main.cpp (which wires the production opener) is not linked here — so
// "opener never called" is exactly "no USB code could have run".

#include <csignal>
#include <cstdio>
#include <format>
#include <span>
#include <string>
#include <vector>

#include "interrupt.hpp"
#include "scan_cli.hpp"
#include "pakon/scan/plan.hpp"
#include "support/test_harness.hpp"

namespace {

using namespace pakon;

std::string read_back(std::FILE* file) {
    std::string text;
    char buffer[4096];
    std::rewind(file);
    while (const std::size_t n = std::fread(buffer, 1, sizeof(buffer), file)) {
        text.append(buffer, n);
    }
    std::fclose(file);
    return text;
}

struct RunOutput {
    int code{2};
    std::string out;
    std::string err;
};

// Counting trap: records every call; always fails (the test asserts it
// is never reached, or reached exactly once on purpose).
struct OpenerTrap {
    int calls{0};

    cli::SessionOpener fn() {
        return [this]() -> Result<cli::LiveScanSession> {
            ++calls;
            return failure<cli::LiveScanSession>(
                ErrorKind::usb_open_failed,
                "TRAP: device must not be opened on this path");
        };
    }
};

RunOutput run(const std::vector<std::string>& args, cli::SessionOpener opener) {
    std::FILE* out = std::tmpfile();
    std::FILE* err = std::tmpfile();
    if (!out || !err) {
        std::fprintf(stderr, "FAIL: tmpfile() unavailable\n");
        return {99, "", ""};
    }
    cli::ScanCliDeps deps;
    deps.open_session = std::move(opener);
    const int code = cli::run_scan_command(args, deps, out, err);
    return {code, read_back(out), read_back(err)};
}

// A fully-valid argument set (mode supplied by the caller).
std::vector<std::string> valid_args(const std::string& mode) {
    return {mode,       "--config",
            "base4-ir-off", "--calibration-rows",
            "16",           "--idle-reads",
            "3",            "--film-end",
            "quiescence",   "--out-prefix",
            "/tmp/preflight-scan"};
}

std::string hex_of(std::span<const std::uint8_t> bytes) {
    std::string text;
    for (const auto byte : bytes) {
        text += std::format("{:02x}", byte);
    }
    return text;
}

} // namespace

PAKON_TEST(default_invocation_is_usage_error_and_never_opens) {
    OpenerTrap trap;
    const auto bare = run({}, trap.fn());
    EXPECT_EQ(bare.code, 2);
    EXPECT_EQ(trap.calls, 0);
    EXPECT(bare.err.find("--live-scan") != std::string::npos);

    // Present-but-incomplete arguments must not open anything either.
    OpenerTrap trap2;
    const auto partial = run({"--config", "base4-ir-off"}, trap2.fn());
    EXPECT_EQ(partial.code, 2);
    EXPECT_EQ(trap2.calls, 0);
}

PAKON_TEST(mode_and_completion_flags_are_enforced_before_any_open) {
    struct Case {
        std::vector<std::string> args;
        const char* expect_in_error;
    };
    const std::vector<Case> cases = {
        // no mode flag at all
        {{"--config", "base4-ir-off", "--calibration-rows", "16", "--idle-reads", "3",
          "--film-end", "quiescence", "--out-prefix", "/tmp/x"},
         "--live-scan"},
        // both modes
        {{"--dry-run", "--live-scan", "--config", "base4-ir-off", "--calibration-rows",
          "16", "--idle-reads", "3", "--film-end", "quiescence", "--out-prefix",
          "/tmp/x"},
         "mutually exclusive"},
        // no config
        {{"--live-scan", "--calibration-rows", "16", "--idle-reads", "3", "--film-end",
          "quiescence", "--out-prefix", "/tmp/x"},
         "--config"},
        // a single transient timeout must never end a window
        {{"--live-scan", "--config", "base4-ir-off", "--calibration-rows", "16",
          "--idle-reads", "1", "--film-end", "quiescence", "--out-prefix", "/tmp/x"},
         "transient timeout"},
        // film end choice
        {{"--live-scan", "--config", "base4-ir-off", "--calibration-rows", "16",
          "--idle-reads", "3", "--out-prefix", "/tmp/x"},
         "--film-end"},
        // rows mode without its budget
        {{"--live-scan", "--config", "base4-ir-off", "--calibration-rows", "16",
          "--idle-reads", "3", "--film-end", "rows", "--out-prefix", "/tmp/x"},
         "--film-rows"},
        // output prefix
        {{"--live-scan", "--config", "base4-ir-off", "--calibration-rows", "16",
          "--idle-reads", "3", "--film-end", "quiescence"},
         "--out-prefix"},
        // unknown argument
        {{"--live-scan", "--resolution", "base4"}, "unknown scan argument"},
    };
    for (const auto& c : cases) {
        OpenerTrap trap;
        const auto run_out = run(c.args, trap.fn());
        EXPECT_EQ(run_out.code, 2);
        EXPECT_EQ(trap.calls, 0);
        EXPECT(run_out.err.find(c.expect_in_error) != std::string::npos);
    }
}

PAKON_TEST(unsupported_config_is_refused_instead_of_substituted) {
    // Recorded in the captures, but no replayed plan: must be refused
    // explicitly — never silently run with base4 bytes.
    OpenerTrap trap;
    const auto refused =
        run({"--live-scan", "--config", "base8-ir-on", "--calibration-rows", "16",
             "--idle-reads", "3", "--film-end", "quiescence", "--out-prefix", "/tmp/x"},
            trap.fn());
    EXPECT_EQ(refused.code, 2);
    EXPECT_EQ(trap.calls, 0);
    EXPECT(refused.err.find("base8-ir.jsonl") != std::string::npos);
    EXPECT(refused.err.find("refus") != std::string::npos);

    // Unknown names are a plain usage error too.
    OpenerTrap trap2;
    const auto unknown =
        run({"--dry-run", "--config", "base7", "--calibration-rows", "16",
             "--idle-reads", "3", "--film-end", "quiescence", "--out-prefix", "/tmp/x"},
            trap2.fn());
    EXPECT_EQ(unknown.code, 2);
    EXPECT_EQ(trap2.calls, 0);
    EXPECT(unknown.err.find("unknown --config") != std::string::npos);
}

PAKON_TEST(dry_run_prints_the_full_plan_and_never_opens) {
    OpenerTrap trap;
    const auto dry = run(valid_args("--dry-run"), trap.fn());
    EXPECT_EQ(dry.code, 0);
    EXPECT_EQ(trap.calls, 0); // the proof: no device entry point ran

    // The preflight must carry the plan in full.
    EXPECT(dry.out.find("No device is opened") != std::string::npos);
    EXPECT(dry.out.find("base4-ir-off") != std::string::npos);

    // Endpoint usage: commands 0x01/0x81, image 0x86.
    EXPECT(dry.out.find("0x01") != std::string::npos);
    EXPECT(dry.out.find("0x81") != std::string::npos);
    EXPECT(dry.out.find("0x86") != std::string::npos);

    // Exact planned frames — computed here from the same plan builders
    // and required verbatim in the output (init head, teardown tail).
    const auto& addresses = protocol::kF135Plus;
    const scan::ScanPlanParameters params{};
    const auto init = scan::init_plan(addresses, params);
    const auto teardown = scan::teardown_plan(addresses, params);
    EXPECT(!init.empty());
    EXPECT(!teardown.empty());
    const auto init_bytes = init.front().serialize();
    const auto teardown_bytes = teardown.back().serialize();
    EXPECT(init_bytes.has_value());
    EXPECT(teardown_bytes.has_value());
    if (init_bytes && teardown_bytes) {
        EXPECT(dry.out.find(hex_of(*init_bytes)) != std::string::npos);
        EXPECT(dry.out.find(hex_of(*teardown_bytes)) != std::string::npos);
    }

    // Completion policy, spelled out with the chosen numbers.
    EXPECT(dry.out.find("row-budget (16 rows)") != std::string::npos);
    EXPECT(dry.out.find("no-progress (3 consecutive idle reads)") != std::string::npos);
    EXPECT(dry.out.find("single idle read never ends a window") != std::string::npos);
    EXPECT(dry.out.find("no byte budget anywhere") != std::string::npos);

    // The service wait's bounds and the interrupt path, also spelled out.
    EXPECT(dry.out.find("60000 ms deadline") != std::string::npos);
    EXPECT(dry.out.find("Ctrl+C") != std::string::npos);

    // Output paths.
    EXPECT(dry.out.find("/tmp/preflight-scan.calibration.pakraw") != std::string::npos);
    EXPECT(dry.out.find("/tmp/preflight-scan.film.pakraw") != std::string::npos);

    // Single-session wiring spelled out for the reviewer.
    EXPECT(dry.out.find("cold_ok=false") != std::string::npos);
    EXPECT(dry.out.find("no second connection") != std::string::npos);
}

PAKON_TEST(live_opens_exactly_once_and_a_failure_is_not_retried) {
    OpenerTrap trap;
    const auto live = run(valid_args("--live-scan"), trap.fn());
    EXPECT_EQ(live.code, 1); // runtime failure, not usage
    EXPECT_EQ(trap.calls, 1); // bounded: exactly one open attempt
    EXPECT(live.err.find("TRAP") != std::string::npos);
}

#ifndef _WIN32
PAKON_TEST(interrupt_handler_latches_the_token_and_restores_default) {
    // Blocker B, wiring half: the POSIX handler must latch the token
    // (and nothing else — async-signal-safe by construction) and
    // restore must hand SIGINT back to the default disposition, so the
    // process never keeps a latch it no longer polls. Windows covers
    // the same token through SetConsoleCtrlHandler; its console-event
    // generation can't be raised safely from inside the test process,
    // so only the platform-independent half is asserted here (the
    // Windows branch compiles with the pakon-cli target).
    const bool installed = cli::install_interrupt_handlers();
    EXPECT(installed);
    if (!installed) {
        return;
    }
    cli::interrupt_token().reset();
    std::raise(SIGINT);
    EXPECT(cli::interrupt_token().requested());
    cli::restore_interrupt_handlers();
    cli::interrupt_token().reset();
    EXPECT(!cli::interrupt_token().requested());
}
#endif

int main() { return pakon::test::run_all(); }
