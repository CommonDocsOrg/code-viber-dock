#include <Unknwn.h>
#include <windows.h>
#include <shellapi.h>

#include <winrt/base.h>

#include <exception>
#include <string_view>

#include "App.h"
#include "DockWindow.h"
#include "TaskbarController.h"

namespace {

struct Options {
    bool quit = false;
    bool restoreTaskbar = false;
};

Options ParseCommandLine() {
    Options o;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return o;
    for (int i = 1; i < argc; ++i) {
        std::wstring_view a = argv[i];
        if (a == L"--quit") o.quit = true;
        else if (a == L"--restore-taskbar") o.restoreTaskbar = true;
    }
    LocalFree(argv);
    return o;
}

// Posts a registered message to the running dock. If `waitForExit`, waits (bounded) for it to exit.
bool SignalRunningInstance(const wchar_t* messageName, bool waitForExit) {
    HWND hwnd = FindWindowW(DockWindow::kClassName, nullptr);
    if (!hwnd) return false;

    HANDLE process = nullptr;
    if (waitForExit) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    }
    const bool posted = PostMessageW(hwnd, RegisterWindowMessageW(messageName), 0, 0) != FALSE;
    if (process) {
        if (posted) WaitForSingleObject(process, 5000);
        CloseHandle(process);
    }
    return posted;
}

// Last-chance taskbar restore on a crash. Minimal and re-entrancy safe: runs at most once,
// TaskbarController::Restore() does not allocate, and the crash then proceeds as usual (the
// crash marker covers anything this misses).
LONG g_crashRestoreDone = 0;

void CrashRestoreTaskbar() {
    if (InterlockedExchange(&g_crashRestoreDone, 1) == 0) TaskbarController::EmergencyRestore();
}

LONG WINAPI UnhandledExceptionHandler(EXCEPTION_POINTERS*) {
    CrashRestoreTaskbar();
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int) {
    const Options options = ParseCommandLine();

    if (options.quit) {
        SignalRunningInstance(AppIpc::kQuitMessage, true);
        return 0;
    }
    if (options.restoreTaskbar) {
        // Works whether or not a dock is running: with no dock (e.g. after a crash) restore
        // from the crash marker directly.
        if (!SignalRunningInstance(AppIpc::kRestoreTaskbarMessage, false)) {
            TaskbarController::RestoreStandalone();
        }
        return 0;
    }

    HANDLE mutex = CreateMutexW(nullptr, FALSE, AppIpc::kMutexName);
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return 0;  // already running
    }

    SetUnhandledExceptionFilter(UnhandledExceptionHandler);
    std::set_terminate([] {
        CrashRestoreTaskbar();
        std::abort();
    });

    int exitCode = 0;
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);

        App app;
        if (!app.Initialize(instance)) {
            exitCode = 1;
        } else {
            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            exitCode = static_cast<int>(msg.wParam);
        }
    } catch (const winrt::hresult_error& e) {
        MessageBoxW(nullptr, e.message().c_str(), L"WinDock", MB_ICONERROR | MB_OK);
        exitCode = 1;
    }

    CloseHandle(mutex);
    return exitCode;
}
