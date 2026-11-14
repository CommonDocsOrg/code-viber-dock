#include <Unknwn.h>
#include <windows.h>
#include <ole2.h>
#include <shellapi.h>

#include <winrt/base.h>

#include <algorithm>
#include <cwctype>
#include <exception>
#include <string>
#include <vector>
#include <string_view>

#include "App.h"
#include "Crash.h"
#include "DockWindow.h"
#include "StartMenuPlacer.h"
#include "TaskbarController.h"
#include "WidgetPackage.h"

namespace {

struct Options {
    bool quit = false;
    bool restoreTaskbar = false;
    bool settings = false;  // open the settings window (in the running dock, or after starting)
    bool welcome = false;   // round 9 phase 9C: the welcome dialog (in the running dock, or after starting); round 12 phase 12G: Quick setup
    bool validate = false;  // --validate-widget <folder>: check a widget's manifest, print, exit
    std::wstring validateFolder;
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
        else if (a == L"--settings") o.settings = true;
        else if (a == L"--welcome") o.welcome = true;
        else if (a == L"--validate-widget") {
            o.validate = true;
            if (i + 1 < argc) o.validateFolder = argv[++i];
        }
    }
    LocalFree(argv);
    return o;
}

// Posts a registered message to the running dock. If `waitForExit`, waits (bounded) for it to
// exit. `allowForeground`: the dock may take the foreground (it is about to show a window).
bool SignalRunningInstance(const wchar_t* messageName, bool waitForExit, bool allowForeground = false) {
    // The app window (always there, even when every dock is parked); an older build: its dock.
    HWND hwnd = FindWindowW(AppIpc::kAppWindowClass, nullptr);
    if (!hwnd) hwnd = FindWindowW(DockWindow::kClassName, nullptr);
    if (!hwnd) return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (allowForeground && pid) AllowSetForegroundWindow(pid);
    HANDLE process = nullptr;
    if (waitForExit) process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    const bool posted = PostMessageW(hwnd, RegisterWindowMessageW(messageName), 0, 0) != FALSE;
    if (process) {
        if (posted) WaitForSingleObject(process, 5000);
        CloseHandle(process);
    }
    return posted;
}

// (Round 25 phase 25E: the last-chance restore, CrashRestoreTaskbar, and the unhandled-exception
// filter moved to Crash.cpp (Crash::RestoreDesktop), with the handlers for the crash paths that
// never reached the filter.)

// Text to the caller (a GUI-subsystem exe: its redirected standard output, else the console of the
// process that started it), UTF-8 into a pipe or file, UTF-16 into a console.
void Print(const std::wstring& text) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        }
        if (!out || out == INVALID_HANDLE_VALUE) return;
    }
    DWORD mode = 0, written = 0;
    if (GetConsoleMode(out, &mode)) {
        WriteConsoleW(out, text.c_str(), static_cast<DWORD>(text.size()), &written, nullptr);
        return;
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(std::max(n, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), utf8.data(), n, nullptr, nullptr);
    WriteFile(out, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
}

// WinDock.exe --validate-widget <folder>: the manifest check of docs/widgets.md "Validating a
// widget", printed one problem per line. Exit code 0: no errors (warnings may be listed), 1: errors,
// 2: no folder given / not a widget folder.
int ValidateWidget(const std::wstring& folder) {
    if (folder.empty()) {
        Print(L"usage: WinDock.exe --validate-widget <widget folder>\r\n");
        return 2;
    }
    winrt::init_apartment(winrt::apartment_type::single_threaded);  // Windows.Data.Json
    std::vector<Widgets::Issue> issues;
    std::wstring name;
    Widgets::Manifest manifest;
    const bool read = Widgets::ValidateFolder(folder, issues, name, &manifest);
    std::wstring text = L"ViberDock widget check: " + folder + L"\r\n";
    if (!name.empty()) text += L"  name: " + name + L"\r\n";
    // Round 25 phase 25D: what installing it will ask the user to allow (the consent dialog's lines).
    if (read && !name.empty()) {
        const std::vector<std::wstring> lines = Widgets::PermissionLines(manifest);
        std::wstring id = folder;
        while (id.size() > 3 && (id.back() == L'\\' || id.back() == L'/')) id.pop_back();
        if (const size_t cut = id.find_last_of(L"\\/"); cut != std::wstring::npos) id = id.substr(cut + 1);
        for (wchar_t& c : id) c = static_cast<wchar_t>(std::towlower(c));
        const bool shadow = Widgets::IsBuiltInId(id);  // installed, it replaces that built-in
        if (lines.empty()) {
            text += shadow ? L"  permissions: none\r\n" : L"  permissions: none (installed, it runs without asking)\r\n";
        } else {
            text += L"  permissions (installed, it runs only after the user allows them):\r\n";
            for (const std::wstring& l : lines) text += L"    - " + l + L"\r\n";
        }
        if (shadow) {
            text += L"  note: the folder name \"" + id + L"\" is a built-in widget's id: installed, it replaces that widget "
                    L"(its places and saved data) and asks for the user's consent even without permissions\r\n";
        }
    }
    size_t errors = 0, warnings = 0;
    for (const Widgets::Issue& i : issues) {
        (i.error ? errors : warnings)++;
        text += L"  " + i.Text() + L"\r\n";
    }
    if (!read) {
        text += L"not a widget folder\r\n";
    } else if (issues.empty()) {
        text += L"OK: no problems\r\n";
    } else {
        text += (errors ? L"FAILED: " : L"OK: ") + std::to_wstring(errors) + (errors == 1 ? L" error, " : L" errors, ") +
                std::to_wstring(warnings) + (warnings == 1 ? L" warning\r\n" : L" warnings\r\n");
    }
    Print(text);
    return !read ? 2 : errors ? 1 : 0;
}

}  // namespace

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int) {
    // Round 25 phase 25E: DLLs only from System32 from here on, before anything can load one. The
    // static imports that are not KnownDLLs (version, dwmapi, uxtheme, winhttp, dcomp, d2d1, ...) are
    // covered by the link option /DEPENDENTLOADFLAG:0x800 (LOAD_LIBRARY_SEARCH_SYSTEM32, CMakeLists.txt),
    // which the loader applies to the exe's import table before this line runs; this call covers
    // every later load by name: LoadLibrary, the delay-loads, and what a system DLL loads in turn.
    // COM / WinRT activation loads its servers by the full path in the registry and is unaffected.
    // Nothing of WinDock's lives beside the exe (one static exe), so no load needs its folder.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    const Options options = ParseCommandLine();

    // A tool for widget authors and CI: no dock, no mutex, nothing shown.
    if (options.validate) return ValidateWidget(options.validateFolder);

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
    // A running dock opens its settings window; otherwise start normally and open it then.
    if (options.settings && SignalRunningInstance(AppIpc::kOpenSettingsMessage, false, true)) return 0;
    if (options.welcome && SignalRunningInstance(AppIpc::kWelcomeMessage, false, true)) return 0;

    HANDLE mutex = CreateMutexW(nullptr, FALSE, AppIpc::kMutexName);
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return 0;  // already running
    }

    // Round 25 phase 25E: the unhandled-exception filter and this thread's terminate handler as
    // before, plus invalid-parameter, purecall and SIGABRT (Crash.h); the worker threads set their
    // own terminate handler (Crash::Job).
    Crash::InstallProcessHandlers();

    int exitCode = 0;
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        // OLE on top of the STA: RegisterDragDrop (file drops on the dock) requires it.
        const HRESULT ole = OleInitialize(nullptr);

        App app;
        if (!app.Initialize(instance)) {
            exitCode = 1;
        } else {
            if (options.settings) app.OpenSettings();
            if (options.welcome) app.OpenWelcome();
            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                if (app.PreTranslateMessage(msg)) continue;  // settings window keyboard navigation
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            exitCode = static_cast<int>(msg.wParam);
        }
        if (SUCCEEDED(ole)) OleUninitialize();
    } catch (const winrt::hresult_error& e) {
        MessageBoxW(nullptr, e.message().c_str(), L"ViberDock", MB_ICONERROR | MB_OK);
        exitCode = 1;
    }

    CloseHandle(mutex);
    return exitCode;
}
