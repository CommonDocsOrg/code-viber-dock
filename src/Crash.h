#pragma once

#include <windows.h>

// Round 25 phase 25E: the crash paths. Anything that can leave the desktop broken when the process
// ends abnormally (the hidden taskbar, a dock's app bar band, the tray takeover, a shifted Start
// menu host) is undone by RestoreDesktop (CLAUDE.md "Conventions"). Until 25E only the unhandled-
// exception filter and the main thread's terminate handler ran it; these end the process without
// either and left the taskbar hidden (only the marker helped, at the next start):
//  - CRT invalid-parameter failures (a *_s function given too small a buffer): the release CRT
//    calls _invoke_watson -> __fastfail, which no filter sees;
//  - abort() (SIGABRT), and a pure virtual call (_purecall -> abort);
//  - a C++ exception escaping a worker thread's procedure: std::terminate on that thread, whose
//    terminate handler is the default one (set_terminate is per thread in MSVC's CRT: "each new
//    thread needs to install its own terminate function").
// InstallProcessHandlers covers the first two process-wide (the global invalid-parameter handler
// applies to every thread that has no thread-local one, and nothing sets one); every worker thread
// procedure runs its body through Job (below).
namespace Crash {

// The handlers (unhandled-exception filter, terminate for the calling thread, invalid parameter,
// purecall, SIGABRT; abort's message box off). wWinMain, once the dock is going to run.
void InstallProcessHandlers();

// The emergency restore: at most once per process, from any thread, no heap allocation (every
// callee keeps that rule: TrayHost::EmergencyRelease, DockWindow::EmergencyRemoveAppBar,
// TaskbarController::EmergencyRestore, StartMenuPlacer::EmergencyRestore). Sets Crashing() first.
void RestoreDesktop() noexcept;

// True from the start of RestoreDesktop: the restore calls into the system can come back into our
// own window procedures (DestroyWindow of the tray window runs its WM_DESTROY; SPI_SETWORKAREA with
// SPIF_SENDCHANGE sends WM_SETTINGCHANGE to every top-level window, ours on the crashing thread
// directly; a thread blocked in a cross-process SendMessage, ABM_REMOVE, dispatches the messages
// sent to it). On state the crash may have corrupted those procedures must not run: the top-level
// window procedures check this first and hand the message to DefWindowProc.
bool Crashing() noexcept;

// RestoreDesktop, then the process ends (__fastfail: a Windows Error Reporting record as before,
// no further code of ours, no atexit / destructors on possibly corrupted state).
[[noreturn]] void RestoreAndFail(unsigned code) noexcept;

// The calling thread's terminate handler: RestoreAndFail. Job calls it; a thread that runs its own
// loop without Job may call it first thing.
void InstallThreadHandlers() noexcept;

// A worker thread's body. A C++ exception escaping `body` (std::bad_alloc, a winrt::hresult_error
// of an API the worker calls) no longer ends the process: it is caught, a line goes to the debugger
// output and `failed` runs (the job's own failure path: post an error result, free the job), then
// the thread returns normally; the dock keeps running and only that job failed. An exception out
// of `failed` itself is swallowed. A structured exception (an access violation) is not a C++
// exception: it still reaches the unhandled-exception filter (RestoreDesktop, then the crash). The
// thread's terminate handler is set too, so a noexcept violation inside runs the restore.
template <class Body, class Failed>
void Job(const wchar_t* name, Body&& body, Failed&& failed) noexcept {
    InstallThreadHandlers();
    try {
        body();
    } catch (...) {
        wchar_t line[160];
        wsprintfW(line, L"WinDock: an exception ended the worker job \"%s\"; the job failed, the dock keeps running\n", name);
        OutputDebugStringW(line);
        try {
            failed();
        } catch (...) {
        }
    }
}

}  // namespace Crash
