#include "Crash.h"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <exception>

#include "DockWindow.h"
#include "StartMenuPlacer.h"
#include "TaskbarController.h"
#include "TrayHost.h"

namespace Crash {

namespace {

LONG g_restoreDone = 0;
std::atomic<bool> g_crashing{false};

LONG WINAPI UnhandledExceptionHandler(EXCEPTION_POINTERS*) {
    RestoreDesktop();
    return EXCEPTION_CONTINUE_SEARCH;  // the crash then proceeds as usual (WER)
}

// _set_invalid_parameter_handler: a *_s function's buffer too small, a bad format string ... In the
// release CRT the arguments are all null (no expression / file / line).
void InvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
    RestoreAndFail(FAST_FAIL_INVALID_ARG);
}

void PureCall() { RestoreAndFail(FAST_FAIL_FATAL_APP_EXIT); }

// SIGABRT: abort() raises it first; returning lets abort finish as it would have (it then ends the
// process itself), so the handler only restores.
void OnAbort(int) { RestoreDesktop(); }

[[noreturn]] void OnTerminate() { RestoreAndFail(FAST_FAIL_FATAL_APP_EXIT); }

}  // namespace

bool Crashing() noexcept { return g_crashing.load(std::memory_order_relaxed); }

void RestoreDesktop() noexcept {
    g_crashing.store(true);  // first: the restore's calls may come back into our window procedures
    if (InterlockedExchange(&g_restoreDone, 1) != 0) return;
    TrayHost::EmergencyRelease();         // the notify icons back to Explorer (our Shell_TrayWnd gone)
    DockWindow::EmergencyRemoveAppBar();  // the dock's reserved band first (best effort)
    TaskbarController::EmergencyRestore();
    // Round 16 phase 16A: a Start menu host shifted beside a button goes back to its place (the
    // shift persists in the shell's window: the next Windows key would open the menu there).
    StartMenuPlacer::EmergencyRestore();
}

void RestoreAndFail(unsigned code) noexcept {
    RestoreDesktop();
    __fastfail(code);
}

void InstallThreadHandlers() noexcept { std::set_terminate(&OnTerminate); }

void InstallProcessHandlers() {
    SetUnhandledExceptionFilter(&UnhandledExceptionHandler);
    InstallThreadHandlers();  // the main thread's (as before 25E: restore, then end)
    // Global, so every thread without a thread-local handler (nothing here sets one) uses it.
    _set_invalid_parameter_handler(&InvalidParameter);
    _set_purecall_handler(&PureCall);
    std::signal(SIGABRT, &OnAbort);
    // No "abort() has been called" message box in front of the user's desktop; the WER report
    // (_CALL_REPORTFAULT) stays.
    _set_abort_behavior(0, _WRITE_ABORT_MSG);
}

}  // namespace Crash
