#pragma once

// The live run's interruption path (live-scan audit, blocker B): there
// used to be NO signal/console handler anywhere in the process, so
// Ctrl+C killed the scan with no teardown and no disconnect — lamp and
// drive left energised.
//
// Division of labour:
//   - install_interrupt_handlers() registers handlers that do exactly
//     one async-signal-safe thing: latch interrupt_token() (a relaxed
//     lock-free store, scan/cancel.hpp). On Windows that covers the
//     console ctrl events (CTRL_C / CTRL_BREAK / CTRL_CLOSE / ...);
//     on POSIX it covers SIGINT/SIGTERM.
//   - the running scan observes the token at bounded points — service
//     wait, phase boundaries, before every image read — and unwinds
//     through the runner's best-effort teardown and the CLI's
//     DisconnectGuard (scan/runner.cpp, scan_cli.cpp). Worst-case
//     latency is one in-flight transfer (usb::kPipeTimeoutMs).
//
// Handlers are installed ONLY for the duration of ScanRunner::run
// (run_live): dry-runs, usage errors and ordinary commands keep the
// default terminate-on-Ctrl+C behaviour, and a Ctrl+C during the
// .pakraw writes below run() still terminates the process instead of
// being latched and ignored.

#include "pakon/scan/cancel.hpp"

namespace pakon::cli {

// The one token: handlers write it, the live run reads it.
scan::CancelToken& interrupt_token();

// Register the platform handlers. Idempotent; returns false if the
// platform call failed (the run then simply has no interrupt path —
// reported as a warning by run_live, never fatal).
bool install_interrupt_handlers();

// Restore the default dispositions (POSIX: the actions saved at
// install; Windows: detach our handler). Idempotent.
void restore_interrupt_handlers();

} // namespace pakon::cli
