// Offline tests for the scan-session state machine: the full legal
// walk, illegal transitions, and the terminal abort semantics.

#include "pakon/scan/session.hpp"
#include "support/test_harness.hpp"

using pakon::scan::ScanEvent;
using pakon::scan::ScanSession;
using pakon::scan::ScanState;

namespace {

void walk_to(ScanSession& session, ScanEvent event, ScanState expected) {
    auto next = session.handle(event);
    EXPECT(next.has_value());
    if (next.has_value()) {
        EXPECT_EQ(*next, expected);
    }
}

} // namespace

PAKON_TEST(full_session_walk_reaches_complete) {
    ScanSession session;
    EXPECT_EQ(session.state(), ScanState::idle);
    EXPECT(!session.terminal());

    walk_to(session, ScanEvent::start, ScanState::initialising);
    walk_to(session, ScanEvent::init_done, ScanState::ready);
    walk_to(session, ScanEvent::service_requested, ScanState::servicing);
    walk_to(session, ScanEvent::service_done, ScanState::calibrating);
    walk_to(session, ScanEvent::calibration_done, ScanState::preparing_transport);
    walk_to(session, ScanEvent::transport_prepared, ScanState::transporting);
    walk_to(session, ScanEvent::completion_signalled, ScanState::completing);
    walk_to(session, ScanEvent::teardown_ready, ScanState::tearing_down);
    walk_to(session, ScanEvent::teardown_done, ScanState::complete);

    EXPECT(session.terminal());
    EXPECT_EQ(session.log().size(), 9u);
    EXPECT_EQ(session.log().front().from, ScanState::idle);
    EXPECT_EQ(session.log().back().to, ScanState::complete);

    // Terminal states reject everything, including further faults.
    EXPECT(!session.handle(ScanEvent::start).has_value());
    EXPECT(!session.handle(ScanEvent::fault).has_value());
}

PAKON_TEST(illegal_transitions_are_rejected_and_state_holds) {
    ScanSession session;
    // service before start
    EXPECT(!session.handle(ScanEvent::service_requested).has_value());
    EXPECT_EQ(session.state(), ScanState::idle);

    walk_to(session, ScanEvent::start, ScanState::initialising);
    // skip init
    EXPECT(!session.handle(ScanEvent::calibration_done).has_value());
    EXPECT_EQ(session.state(), ScanState::initialising);
    // events out of order in the middle
    EXPECT(!session.handle(ScanEvent::completion_signalled).has_value());
    EXPECT(!session.handle(ScanEvent::teardown_done).has_value());

    walk_to(session, ScanEvent::init_done, ScanState::ready);
    walk_to(session, ScanEvent::service_requested, ScanState::servicing);
    // restart mid-session
    EXPECT(!session.handle(ScanEvent::start).has_value());
    EXPECT_EQ(session.state(), ScanState::servicing);
}

PAKON_TEST(fault_is_terminal_from_every_active_state) {
    const ScanEvent starters[] = {
        ScanEvent::start,        ScanEvent::init_done,     ScanEvent::service_requested,
        ScanEvent::service_done, ScanEvent::calibration_done,
        ScanEvent::transport_prepared, ScanEvent::completion_signalled,
        ScanEvent::teardown_ready, ScanEvent::teardown_done,
    };
    const ScanState reached[] = {
        ScanState::initialising, ScanState::ready, ScanState::servicing,
        ScanState::calibrating, ScanState::preparing_transport,
        ScanState::transporting, ScanState::completing, ScanState::tearing_down,
        ScanState::complete,
    };
    for (std::size_t i = 0; i < std::size(starters); ++i) {
        ScanSession session;
        for (std::size_t j = 0; j <= i; ++j) {
            EXPECT(session.handle(starters[j]).has_value());
        }
        EXPECT_EQ(session.state(), reached[i]);
        const bool was_complete = session.state() == ScanState::complete;
        auto result = session.handle(ScanEvent::fault);
        // fault lands in failed from every ACTIVE state; from complete
        // it is refused (nothing to abort).
        EXPECT_EQ(result.has_value(), !was_complete);
        if (!was_complete) {
            EXPECT_EQ(session.state(), ScanState::failed);
            EXPECT(session.terminal());
            EXPECT(!session.handle(ScanEvent::start).has_value());
        } else {
            EXPECT_EQ(session.state(), ScanState::complete);
        }
    }
}

int main() { return pakon::test::run_all(); }
