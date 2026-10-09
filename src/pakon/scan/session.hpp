#pragma once

// Explicit scan-session state machine.
//
// The states and their order follow the reference's scan sequence
// (pakon-reference/docs/command-reference.md § "Scan sequence") as the
// capture corpus actually ran it (base4.jsonl, ten sessions, all
// byte-exact): initialise → idle/service → pre-scan calibration window
// → transport engage → film-scan window → teardown. Every transition
// is a pure (state, event) decision — no I/O here — so the whole
// machine is testable offline, and an illegal transition or any fault
// aborts the session rather than being retried.

#include <string_view>
#include <vector>

#include "pakon/errors/error.hpp"

namespace pakon::scan {

enum class ScanState {
    idle,                 // constructed; nothing sent yet
    initialising,         // init command block in flight
    ready,                // init done; polling for service
    servicing,            // service event being handled (ack + sensor read)
    calibrating,          // W1: pre-scan calibration window
    preparing_transport,  // engage sequence between the windows
    transporting,         // W2: film-scan window
    completing,           // window ended; image reader finalised
    tearing_down,         // motor idle, lamp off, EndAcquisition, disengage
    complete,             // terminal
    failed,               // terminal (abort; never retried)
};

enum class ScanEvent {
    start,                // idle → initialising
    init_done,
    service_requested,    // service-wanted poll observed
    service_done,
    calibration_done,     // calibration window completed by its policy
    transport_prepared,   // engage sequence fully sent
    completion_signalled, // transport window completed by its policy
    teardown_ready,
    teardown_done,
    fault,                // any I/O or protocol failure; terminal
};

std::string_view to_string(ScanState state);
std::string_view to_string(ScanEvent event);

struct Transition {
    ScanState from;
    ScanEvent event;
    ScanState to;
};

// The transition table, as a pure function. Returns false when the
// event is not legal from `from` (including from the terminal states).
bool is_legal_transition(ScanState from, ScanEvent event, ScanState& to);

class ScanSession {
public:
    ScanState state() const { return state_; }
    bool terminal() const {
        return state_ == ScanState::complete || state_ == ScanState::failed;
    }
    // Every applied transition, in order (reports/diagnostics).
    const std::vector<Transition>& log() const { return log_; }

    // Apply an event. Failure (scanner_unexpected_state) means the
    // caller drove the session wrongly — a bug, not a device fault —
    // and leaves the state untouched.
    Result<ScanState> handle(ScanEvent event);

private:
    ScanState state_{ScanState::idle};
    std::vector<Transition> log_;
};

} // namespace pakon::scan
