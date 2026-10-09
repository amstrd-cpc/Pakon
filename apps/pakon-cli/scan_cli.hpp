#pragma once

// The `scan` command: explicit live-scan gating, offline preflight, and
// the single-session wiring of command channel + image source + runner
// + raw output (scan/runner.hpp, image/raw_writer.hpp).
//
// Hard rules encoded here, proven by tests/cli/scan_cli_test.cpp:
//
//  - Nothing can reach USB unless the invocation carries --live-scan.
//    The default invocation (no mode flag) and --dry-run are decided
//    during argument validation, BEFORE any device entry point exists:
//    the ONLY USB door is the injected SessionOpener, called at most
//    once per run and only on the --live-scan path. The dry run prints
//    the exact plan — frames, endpoints, completion policies, output
//    paths — without opening anything.
//  - Mode, configuration, completion budgets and output prefix are
//    required arguments. No resolution/IR mode is chosen implicitly,
//    no image byte budget is invented, and a single transient timeout
//    can never end a window (--idle-reads >= 2 is enforced).
//  - The live path reuses ONE warm-booted session: the production
//    opener (main.cpp) is usb::open_first(cold_ok=false) →
//    Scanner::connect → identify — the same sequence `identify` and
//    `status` already use. No cold boot, no firmware reload, no second
//    connection: command frames (bulk 0x01/0x81) and image reads
//    (bulk 0x86) both ride that session's transport.
//
// Exit codes: 0 success, 1 runtime failure, 2 usage error (a usage
// error is decided before the opener could run — exit 2 always means
// zero device I/O from this command).

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

} // namespace pakon::cli
