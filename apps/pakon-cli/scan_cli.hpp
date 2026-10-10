#pragma once

// The `scan` and `eeprom` commands.
//
// Hard rules encoded here, proven by tests/cli/scan_cli_test.cpp:
//
//  - Nothing can reach USB unless the invocation carries --live-scan (or
//    is the explicit `eeprom` read). Usage errors and --dry-run are
//    decided during argument validation, BEFORE any device entry point
//    exists: the ONLY USB door is the injected SessionOpener, called at
//    most once per run. The dry run prints the exact fixed frames (init
//    block, teardown), the data-dependent phases with their bounds, the
//    endpoints and the output paths.
//  - Mode, configuration and the window bound are required: --first-light
//    (calibration window only, lamp on, motor never engaged) or
//    --film-rows N (film window, N = hard row cap on top of film-end
//    detection and the no-film timeout).
//  - The live path reuses ONE warm-booted session (usb::open_first ->
//    Scanner::connect -> identify), reads the unit's EEPROM (read-only
//    allow-list), and runs scan::ScanRunner on the concurrent image
//    stream. Teardown runs exactly once on every path, Ctrl+C included
//    (apps/pakon-cli/interrupt.hpp latches the token).
//
// Exit codes: 0 success, 1 runtime failure (including a teardown step
// that failed), 2 usage error (decided before any device entry point).

#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "pakon/errors/error.hpp"
#include "pakon/ppb/client.hpp"
#include "pakon/scanner/scanner.hpp"

namespace pakon::cli {

// One open device session for a live scan. `scanner` owns the single
// transport connection; `client` (and everything reached through it)
// is valid until the session is disconnected.
struct LiveScanSession {
    std::unique_ptr<scanner::Scanner> scanner;
    scanner::Identity identity; // captured at open time (identify())
    ppb::Client* client{nullptr}; // shared by both wire channels
};

// Opens the session. Tests replace it with a trap that counts calls:
// a session may only ever be opened under --live-scan.
using SessionOpener = std::function<Result<LiveScanSession>()>;

struct ScanCliDeps {
    SessionOpener open_session;
};

// Handle the whole `scan` command; returns the process exit code.
int run_scan_command(const std::vector<std::string>& args,
                     const ScanCliDeps& deps, std::FILE* out, std::FILE* err);

// The `scan` usage block (printed by main's --help as well).
void print_scan_usage(std::FILE* out);

// `eeprom [--out <file>]`: read the per-unit EEPROM (read-only allow-list),
// print the parsed fields, optionally save the raw bytes.
int run_eeprom_command(const std::vector<std::string>& args, const ScanCliDeps& deps,
                       std::FILE* out, std::FILE* err);

} // namespace pakon::cli
