#include "App.h"

#include <windowsx.h>

#include <string>

#include "ContextMenu.h"
#include "resource.h"

App::~App() { Shutdown(); }

bool App::Initialize(HINSTANCE instance) {
    instance_ = instance;
    msgQuit_ = RegisterWindowMessageW(AppIpc::kQuitMessage);
    msgRestoreTaskbar_ = RegisterWindowMessageW(AppIpc::kRestoreTaskbarMessage);
    msgTaskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");

    config_.Load();

    // Crash recovery first, before anything else can fail: if a previous run died with the
    // taskbar hidden, put it back using the state saved in the marker.
    taskbar_.RecoverFromMarker();

    HRESULT hr = dock_.Create(instance, &config_, [this](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT& r) {
        return OnMessage(hwnd, msg, wp, lp, r);
    });
    if (FAILED(hr)) {
        wchar_t text[256];
        swprintf_s(text, L"WinDock could not initialize Direct2D / DirectComposition (error 0x%08X).",
                   static_cast<unsigned>(hr));
        MessageBoxW(nullptr, text, L"WinDock", MB_ICONERROR | MB_OK);
        return false;
    }

    AddTrayIcon();
    taskbar_.SetEnabled(config_.hideTaskbar);
    return true;
}

void App::Shutdown() {
    if (shutDown_) return;
    shutDown_ = true;
    taskbar_.Restore();
    RemoveTrayIcon();
    dock_.Destroy();
    if (trayIcon_) {
        DestroyIcon(trayIcon_);
        trayIcon_ = nullptr;
    }
}

void App::Quit() {
    // Destroying the dock posts WM_QUIT, which ends the message loop in wWinMain.
    Shutdown();
}

void App::AddTrayIcon() {
    HWND hwnd = dock_.Hwnd();
    if (!hwnd) return;
    if (!trayIcon_) {
        const UINT dpi = GetDpiForWindow(hwnd);
        trayIcon_ = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                                  GetSystemMetricsForDpi(SM_CXSMICON, dpi),
                                                  GetSystemMetricsForDpi(SM_CYSMICON, dpi), LR_DEFAULTCOLOR));
    }

    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = kTrayCallback;
    nid.hIcon = trayIcon_ ? trayIcon_ : LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"WinDock");
    trayAdded_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    if (trayAdded_) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    }
}

void App::RemoveTrayIcon() {
    if (!trayAdded_) return;
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = dock_.Hwnd();
    nid.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    trayAdded_ = false;
}

void App::ShowMenu(POINT pt) {
    ContextMenu::TrayMenuState state;
    state.autoHide = config_.autoHide;
    state.hideTaskbar = config_.hideTaskbar;
    state.startWithWindows = config_.startWithWindows;
    UINT cmd = ContextMenu::ShowTrayMenu(dock_.Hwnd(), pt, state);
    if (cmd != ContextMenu::kNone) OnCommand(cmd);
}

void App::OnCommand(UINT cmd) {
    switch (cmd) {
        case ContextMenu::kOpenConfig:
            OpenConfigFile();
            break;
        case ContextMenu::kRestoreTaskbar:
            taskbar_.Restore();
            break;
        case ContextMenu::kQuit:
            Quit();
            break;
        default:
            break;  // toggles are wired in later phases
    }
}

void App::OpenConfigFile() {
    const std::wstring path = Config::FilePath();
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) config_.Save();

    // Use the user's .json association; fall back to Notepad when there is none.
    auto rc = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (rc <= 32) {
        const std::wstring args = L"\"" + path + L"\"";
        ShellExecuteW(nullptr, L"open", L"notepad.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }
}

bool App::OnMessage(HWND, UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) {
    result = 0;

    if (msg == kTrayCallback) {
        // NOTIFYICON_VERSION_4: event in LOWORD(lParam), anchor point in wParam.
        switch (LOWORD(lp)) {
            case WM_CONTEXTMENU:
            case NIN_SELECT:
            case NIN_KEYSELECT:
                ShowMenu(POINT{GET_X_LPARAM(wp), GET_Y_LPARAM(wp)});
                break;
            default:
                break;
        }
        return true;
    }
    if (msg == msgQuit_) {
        Quit();
        return true;
    }
    if (msg == msgRestoreTaskbar_) {
        taskbar_.Restore();
        return true;
    }
    if (msg == msgTaskbarCreated_) {
        // Explorer restarted: the tray icon is gone and the new taskbar must be re-hidden.
        trayAdded_ = false;
        AddTrayIcon();
        taskbar_.OnTaskbarCreated();
        return true;
    }

    switch (msg) {
        case WM_RBUTTONUP: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(dock_.Hwnd(), &pt);
            ShowMenu(pt);
            return true;
        }
        case WM_QUERYENDSESSION:
            // Restore while Explorer is certainly still alive to persist the appbar state; if
            // the logoff is cancelled, WM_ENDSESSION(FALSE) hides the taskbar again.
            taskbar_.Restore();
            result = TRUE;
            return true;
        case WM_ENDSESSION:
            if (wp) {
                taskbar_.Restore();  // the session is ending (no-op if already restored)
            } else if (config_.hideTaskbar) {
                taskbar_.Hide();     // shutdown was cancelled
            }
            return true;
        case WM_SETTINGCHANGE:
            if (wp == SPI_SETWORKAREA) taskbar_.OnWorkAreaChanged();
            return false;  // DockWindow relayouts on the same message
        default:
            return false;
    }
}
