#include "interrupt.hpp"

#ifdef _WIN32

#include <windows.h>

#else

#include <csignal>

#endif

namespace pakon::cli {
namespace {

scan::CancelToken g_interrupt;

#ifdef _WIN32

// Runs on a thread the system creates for the ctrl event — the only
// safe work here is the latch. Returning TRUE suppresses the default
// termination: the main thread notices the token at its next bounded
// point (<= one pipe deadline) and unwinds through the teardown. For
// CTRL_CLOSE the system still enforces its own short grace period
// (~5 s) after the handler returns; the unwind fits inside it.
BOOL WINAPI on_console_event(DWORD type) {
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        g_interrupt.request();
        return TRUE;
    default:
        return FALSE;
    }
}

#else

void on_signal(int) {
    // Async-signal-safe by construction: one relaxed atomic store.
    g_interrupt.request();
}

struct sigaction g_previous_int {};
struct sigaction g_previous_term {};
bool g_saved{false};

#endif

bool g_installed{false};

} // namespace

scan::CancelToken& interrupt_token() { return g_interrupt; }

bool install_interrupt_handlers() {
    if (g_installed) {
        return true;
    }
#ifdef _WIN32
    if (SetConsoleCtrlHandler(on_console_event, TRUE) == 0) {
        return false;
    }
#else
    struct sigaction action {};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    if (sigaction(SIGINT, &action, &g_previous_int) != 0) {
        return false;
    }
    if (sigaction(SIGTERM, &action, &g_previous_term) != 0) {
        sigaction(SIGINT, &g_previous_int, nullptr);
        return false;
    }
    g_saved = true;
#endif
    g_installed = true;
    return true;
}

void restore_interrupt_handlers() {
    if (!g_installed) {
        return;
    }
#ifdef _WIN32
    SetConsoleCtrlHandler(on_console_event, FALSE);
#else
    if (g_saved) {
        sigaction(SIGINT, &g_previous_int, nullptr);
        sigaction(SIGTERM, &g_previous_term, nullptr);
        g_saved = false;
    }
#endif
    g_installed = false;
}

} // namespace pakon::cli
