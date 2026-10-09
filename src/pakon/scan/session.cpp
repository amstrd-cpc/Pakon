#include "pakon/scan/session.hpp"

#include <format>

namespace pakon::scan {

std::string_view to_string(ScanState state) {
    switch (state) {
    case ScanState::idle: return "idle";
    case ScanState::initialising: return "initialising";
    case ScanState::ready: return "ready";
    case ScanState::servicing: return "servicing";
    case ScanState::calibrating: return "calibrating";
    case ScanState::preparing_transport: return "preparing-transport";
    case ScanState::transporting: return "transporting";
    case ScanState::completing: return "completing";
    case ScanState::tearing_down: return "tearing-down";
    case ScanState::complete: return "complete";
    case ScanState::failed: return "failed";
    }
    return "unknown";
}

std::string_view to_string(ScanEvent event) {
    switch (event) {
    case ScanEvent::start: return "start";
    case ScanEvent::init_done: return "init-done";
    case ScanEvent::service_requested: return "service-requested";
    case ScanEvent::service_done: return "service-done";
    case ScanEvent::calibration_done: return "calibration-done";
    case ScanEvent::transport_prepared: return "transport-prepared";
    case ScanEvent::completion_signalled: return "completion-signalled";
    case ScanEvent::teardown_ready: return "teardown-ready";
    case ScanEvent::teardown_done: return "teardown-done";
    case ScanEvent::fault: return "fault";
    }
    return "unknown";
}

bool is_legal_transition(ScanState from, ScanEvent event, ScanState& to) {
    // fault is legal from every active state and always lands in
    // failed — the abort path (never retried).
    if (event == ScanEvent::fault) {
        if (from == ScanState::complete || from == ScanState::failed) {
            return false;
        }
        to = ScanState::failed;
        return true;
    }
    if (from == ScanState::complete || from == ScanState::failed) {
        return false;
    }
    switch (from) {
    case ScanState::idle:
        if (event == ScanEvent::start) { to = ScanState::initialising; return true; }
        break;
    case ScanState::initialising:
        if (event == ScanEvent::init_done) { to = ScanState::ready; return true; }
        break;
    case ScanState::ready:
        if (event == ScanEvent::service_requested) { to = ScanState::servicing; return true; }
        break;
    case ScanState::servicing:
        if (event == ScanEvent::service_done) { to = ScanState::calibrating; return true; }
        break;
    case ScanState::calibrating:
        if (event == ScanEvent::calibration_done) { to = ScanState::preparing_transport; return true; }
        break;
    case ScanState::preparing_transport:
        if (event == ScanEvent::transport_prepared) { to = ScanState::transporting; return true; }
        break;
    case ScanState::transporting:
        if (event == ScanEvent::completion_signalled) { to = ScanState::completing; return true; }
        break;
    case ScanState::completing:
        if (event == ScanEvent::teardown_ready) { to = ScanState::tearing_down; return true; }
        break;
    case ScanState::tearing_down:
        if (event == ScanEvent::teardown_done) { to = ScanState::complete; return true; }
        break;
    default:
        break;
    }
    return false;
}

Result<ScanState> ScanSession::handle(ScanEvent event) {
    ScanState to{};
    if (!is_legal_transition(state_, event, to)) {
        return failure<ScanState>(
            ErrorKind::scanner_unexpected_state,
            std::format("scan event '{}' is not legal in state '{}'",
                        to_string(event), to_string(state_)));
    }
    log_.push_back(Transition{state_, event, to});
    state_ = to;
    return state_;
}

} // namespace pakon::scan
