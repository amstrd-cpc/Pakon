// CLI gating proofs for the `scan` command (apps/pakon-cli/scan_cli.hpp).
//
// The single device entry point of the command is the injected
// SessionOpener: these tests replace it with a counting trap and prove
// that
//   - the default invocation (no mode flags, or missing required
//     arguments) exits 2 without ever calling it, and
//   - --dry-run prints the full plan (exact init and teardown frames,
//     endpoints, window, output paths) and exits 0 without calling it, and
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
#include "pakon/scan/device.hpp"
#include "pakon/scan/modes.hpp"
#include "pakon/scan/runner.hpp"
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
    return {mode, "--config", "base4", "--first-light", "--out-prefix", "/tmp/preflight-scan"};
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
    const auto partial = run({"--config", "base4"}, trap2.fn());
    EXPECT_EQ(partial.code, 2);
    EXPECT_EQ(trap2.calls, 0);
}

PAKON_TEST(mode_and_window_flags_are_enforced_before_any_open) {
    struct Case {
        std::vector<std::string> args;
        const char* expect_in_error;
    };
    const std::vector<Case> cases = {
        {{"--config", "base4", "--first-light", "--out-prefix", "/tmp/x"}, "--live-scan"},
        {{"--dry-run", "--live-scan", "--config", "base4", "--first-light", "--out-prefix",
          "/tmp/x"},
         "mutually exclusive"},
        {{"--live-scan", "--first-light", "--out-prefix", "/tmp/x"}, "--config"},
        // exactly one window: neither, or both
        {{"--live-scan", "--config", "base4", "--out-prefix", "/tmp/x"}, "exactly one window"},
        {{"--live-scan", "--config", "base4", "--first-light", "--film-rows", "100",
          "--out-prefix", "/tmp/x"},
         "exactly one window"},
        // the film row cap is mandatory and positive
        {{"--live-scan", "--config", "base4", "--film-rows", "0", "--out-prefix", "/tmp/x"},
         "positive integer"},
        {{"--live-scan", "--config", "base4", "--film-rows", "12x", "--out-prefix", "/tmp/x"},
         "positive integer"},
        {{"--live-scan", "--config", "base4", "--film-rows"}, "requires a value"},
        {{"--live-scan", "--config", "base4", "--first-light"}, "--out-prefix"},
        {{"--live-scan", "--resolution", "base4"}, "unknown scan argument"},
        {{"--dry-run", "--config", "base7", "--first-light", "--out-prefix", "/tmp/x"},
         "unknown --config"},
    };
    for (const auto& c : cases) {
        OpenerTrap trap;
        const auto run_out = run(c.args, trap.fn());
        EXPECT_EQ(run_out.code, 2);
        EXPECT_EQ(trap.calls, 0);
        EXPECT(run_out.err.find(c.expect_in_error) != std::string::npos);
    }
}

PAKON_TEST(every_configuration_has_a_dry_run_plan) {
    for (const char* config : {"base4", "base4-ir", "base8", "base8-ir", "base16", "base16-ir",
                               "base4-ir-off", "base16-ir-on"}) {
        OpenerTrap trap;
        const auto dry = run({"--dry-run", "--config", config, "--film-rows", "5000",
                              "--out-prefix", "/tmp/p"},
                             trap.fn());
        EXPECT_EQ(dry.code, 0);
        EXPECT_EQ(trap.calls, 0);
        EXPECT(dry.out.find("5000 rows (hard cap)") != std::string::npos);
        EXPECT(dry.out.find("/tmp/p.film.pakraw") != std::string::npos);
    }
}

PAKON_TEST(dry_run_prints_the_full_plan_and_never_opens) {
    OpenerTrap trap;
    const auto dry = run(valid_args("--dry-run"), trap.fn());
    EXPECT_EQ(dry.code, 0);
    EXPECT_EQ(trap.calls, 0); // the proof: no device entry point ran

    EXPECT(dry.out.find("No device is opened") != std::string::npos);
    EXPECT(dry.out.find("base4") != std::string::npos);
    EXPECT(dry.out.find("motor never engaged") != std::string::npos);

    // Endpoint usage: commands 0x01/0x81, image 0x86.
    EXPECT(dry.out.find("0x01") != std::string::npos);
    EXPECT(dry.out.find("0x81") != std::string::npos);
    EXPECT(dry.out.find("0x86") != std::string::npos);

    // Exact frames: every init frame and every teardown frame verbatim.
    const auto& a = protocol::kF135Plus;
    const auto frame_hex = [](const ppb::Frame& f) {
        const auto bytes = f.serialize();
        return bytes ? hex_of(*bytes) : std::string("?");
    };
    for (const auto& f : scan::ScanRunner::init_frames(a)) {
        EXPECT(dry.out.find(frame_hex(f)) != std::string::npos);
    }
    const auto idle = scan::control_word(
        scan::film_geometry(*scan::parse_mode("base4"), 30), false);
    for (const auto& f : scan::teardown_frames(a, idle)) {
        EXPECT(dry.out.find(frame_hex(f.frame)) != std::string::npos);
    }

    // The EEPROM path is read-only and says so.
    EXPECT(dry.out.find("no 0xA2") != std::string::npos);
    EXPECT(dry.out.find("Ctrl+C") != std::string::npos);
    EXPECT(dry.out.find("/tmp/preflight-scan.white.pakraw") != std::string::npos);
    EXPECT(dry.out.find("/tmp/preflight-scan.white.tiff") != std::string::npos);
    EXPECT(dry.out.find("cold_ok=false") != std::string::npos);
    EXPECT(dry.out.find("no second connection") != std::string::npos);
}

PAKON_TEST(eeprom_command_opens_exactly_once_and_rejects_bad_arguments) {
    OpenerTrap trap;
    std::FILE* out = std::tmpfile();
    std::FILE* err = std::tmpfile();
    cli::ScanCliDeps deps;
    deps.open_session = trap.fn();
    EXPECT_EQ(cli::run_eeprom_command({"--write"}, deps, out, err), 2);
    EXPECT_EQ(trap.calls, 0);
    EXPECT_EQ(cli::run_eeprom_command({}, deps, out, err), 1);
    EXPECT_EQ(trap.calls, 1);
    std::fclose(out);
    std::fclose(err);
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
