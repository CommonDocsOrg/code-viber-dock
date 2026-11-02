#include "App.h"

#include <windowsx.h>

#include <algorithm>
#include <string>
#include <vector>

#include "AppIdentity.h"
#include "ContextMenu.h"
#include "resource.h"

namespace {

// "Start with Windows": a value under HKCU\...\Run holding the quoted exe path.
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"WinDock";

bool AutostartEnabled() {
    DWORD type = 0, size = 0;
    const LSTATUS rc = RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ | RRF_NOEXPAND, &type,
                                    nullptr, &size);
    return rc == ERROR_SUCCESS && size > sizeof(wchar_t);
}

bool SetAutostart(bool enable) {
    if (!enable) {
        const LSTATUS rc = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue);
        return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
    }
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n == 0) return false;
        if (n < path.size()) {
            path.resize(n);
            break;
        }
        path.resize(path.size() * 2);
    }
    const std::wstring value = L"\"" + path + L"\"";
    return RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, REG_SZ, value.c_str(),
                           static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

}  // namespace

App::~App() { Shutdown(); }

bool App::Initialize(HINSTANCE instance) {
    instance_ = instance;
    msgQuit_ = RegisterWindowMessageW(AppIpc::kQuitMessage);
    msgRestoreTaskbar_ = RegisterWindowMessageW(AppIpc::kRestoreTaskbarMessage);
    msgTaskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    msgItemCommand_ = RegisterWindowMessageW(AppIpc::kItemCommandMessage);
    msgDockCommand_ = RegisterWindowMessageW(AppIpc::kDockCommandMessage);
    msgOpenSettings_ = RegisterWindowMessageW(AppIpc::kOpenSettingsMessage);
    msgQueryState_ = RegisterWindowMessageW(DockWindow::kQueryStateMessage);

    if (config_.Load()) {
        // First run: no config file yet. Seed the default pins and write the file.
        SeedDefaultPins();
        config_.Save();
    }
    if (config_.AutoHideOn()) lastOnMode_ = config_.autoHide;

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

    // Drag to reorder / unpin and file drops (the dock registers itself as an OLE drop target).
    DockWindow::PinRequests requests;
    requests.reorder = [this](const std::vector<AppKey>& order) { ApplyPinOrder(order); };
    requests.unpin = [this](const AppKey& key) {
        const int index = IndexOfKey(key);
        if (index >= 0 && dock_.Items()[static_cast<size_t>(index)].pinned) TogglePin(key);
    };
    requests.pinFiles = [this](const std::vector<std::wstring>& paths, size_t at) { PinFiles(paths, at); };
    dock_.SetPinRequests(std::move(requests));

    AddTrayIcon();
    taskbar_.SetEnabled(config_.hideTaskbar);
    return true;
}

void App::SeedDefaultPins() {
    wchar_t windir[MAX_PATH]{};
    const UINT n = GetWindowsDirectoryW(windir, MAX_PATH);
    PinnedEntry explorer;
    explorer.path = (n > 0 && n < MAX_PATH ? std::wstring(windir) : std::wstring(L"C:\\Windows")) + L"\\explorer.exe";
    config_.pinned.push_back(std::move(explorer));

    static constexpr wchar_t kTerminal[] = L"Microsoft.WindowsTerminal_8wekyb3d8bbwe!App";
    if (AppIdentity::AumidInstalled(kTerminal)) {
        PinnedEntry terminal;
        terminal.aumid = kTerminal;
        config_.pinned.push_back(std::move(terminal));
    }
}

void App::RequestSave() {
    HWND hwnd = dock_.Hwnd();
    if (!hwnd) {
        config_.Save();
        return;
    }
    SetTimer(hwnd, kSaveTimerId, kSaveDelayMs, nullptr);  // restarts the debounce window
    savePending_ = true;
}

void App::FlushSave() {
    if (!savePending_) return;
    if (dock_.Hwnd()) KillTimer(dock_.Hwnd(), kSaveTimerId);
    savePending_ = false;
    config_.Save();
}

int App::IndexOfKey(const AppKey& key) const {
    const auto& items = dock_.Items();
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].key == key) return static_cast<int>(i);
    }
    return -1;
}

void App::TogglePin(const AppKey& key) {
    const int index = IndexOfKey(key);
    if (index < 0) return;
    const DockItem& item = dock_.Items()[static_cast<size_t>(index)];

    if (item.pinned) {
        // Unpin: drop every config entry that resolves to this app. The item stays while the
        // app has windows (as a running item) and disappears when they close.
        const auto& pins = dock_.Pins();
        for (size_t i = std::min(pins.size(), config_.pinned.size()); i-- > 0;) {
            if (pins[i].key == key) config_.pinned.erase(config_.pinned.begin() + static_cast<ptrdiff_t>(i));
        }
    } else {
        // Pinning appends to the end of the pinned list; the item moves to that slot.
        std::optional<PinnedEntry> e = EntryForItem(item);
        if (!e) return;  // nothing we could launch
        config_.pinned.push_back(std::move(*e));
    }
    dock_.ReloadPins();
    RequestSave();
}

std::optional<PinnedEntry> App::EntryForItem(const DockItem& item) {
    if (item.key.IsClassFallback()) return std::nullopt;
    PinnedEntry e;
    if (item.launchPath && !item.launchPath->empty()) {
        e.path = *item.launchPath;
    } else if (item.key.isAumid) {
        e.aumid = item.key.Source();
    } else {
        e.path = item.key.Source();
    }
    return e;
}

void App::ApplyPinOrder(const std::vector<AppKey>& order) {
    const auto& pins = dock_.Pins();  // parallel to config_.pinned
    std::vector<bool> used(config_.pinned.size(), false);
    std::vector<PinnedEntry> next;
    next.reserve(config_.pinned.size() + 1);
    for (const AppKey& key : order) {
        bool found = false;
        // Every entry resolving to this app moves along (duplicates stay together).
        for (size_t i = 0; i < config_.pinned.size() && i < pins.size(); ++i) {
            if (!used[i] && pins[i].key == key) {
                next.push_back(config_.pinned[i]);
                used[i] = true;
                found = true;
            }
        }
        if (found) continue;
        const int index = IndexOfKey(key);  // a running item dragged into the pinned section
        if (index < 0) continue;
        if (std::optional<PinnedEntry> e = EntryForItem(dock_.Items()[static_cast<size_t>(index)])) {
            next.push_back(std::move(*e));
        }
    }
    for (size_t i = 0; i < config_.pinned.size(); ++i) {
        if (!used[i]) next.push_back(config_.pinned[i]);  // not in `order`: keep, after the others
    }
    config_.pinned = std::move(next);
    dock_.ReloadPins();
    RequestSave();
}

void App::PinFiles(const std::vector<std::wstring>& paths, size_t at) {
    // Already pinned apps (by key) are skipped, also among the dropped files themselves.
    std::vector<AppKey> keys;
    for (const auto& existing : dock_.Pins()) keys.push_back(existing.key);
    at = std::min(at, config_.pinned.size());
    bool changed = false;
    for (const std::wstring& path : paths) {
        const AppIdentity::ResolvedPin pin = AppIdentity::ResolvePin(path, {});
        if (std::find(keys.begin(), keys.end(), pin.key) != keys.end()) continue;
        keys.push_back(pin.key);
        PinnedEntry e;
        e.path = pin.launchPath.empty() ? path : pin.launchPath;
        config_.pinned.insert(config_.pinned.begin() + static_cast<ptrdiff_t>(at++), std::move(e));
        changed = true;
    }
    if (!changed) return;
    dock_.ReloadPins();
    RequestSave();
}

void App::RunItemCommand(UINT cmd, const AppKey& key, const std::vector<HWND>& windows) {
    if (cmd >= ContextMenu::kCloseWindowBase && cmd - ContextMenu::kCloseWindowBase < windows.size()) {
        HWND w = windows[cmd - ContextMenu::kCloseWindowBase];
        if (IsWindow(w)) PostMessageW(w, WM_CLOSE, 0, 0);
        return;
    }
    switch (cmd) {
        case ContextMenu::kCloseAllWindows:
            for (HWND w : windows) {
                if (IsWindow(w)) PostMessageW(w, WM_CLOSE, 0, 0);
            }
            return;
        case ContextMenu::kTogglePin:
            TogglePin(key);
            return;
        case ContextMenu::kNewWindow:
            dock_.LaunchItem(IndexOfKey(key), true);
            return;
        case ContextMenu::kOpenFileLocation: {
            const int index = IndexOfKey(key);
            if (index < 0) return;
            const DockItem& item = dock_.Items()[static_cast<size_t>(index)];
            std::wstring target = item.targetPath;
            if (target.empty() && !key.isAumid && !key.IsClassFallback()) target = key.Source();
            if (target.empty()) return;
            const std::wstring args = L"/select,\"" + target + L"\"";
            AllowSetForegroundWindow(ASFW_ANY);
            ShellExecuteW(nullptr, nullptr, L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
            return;
        }
        default:
            if (cmd != ContextMenu::kNone) OnCommand(cmd);
            return;
    }
}

void App::Shutdown() {
    if (shutDown_) return;
    // First: closing it may commit a pending edit (focus loss), which must still be saved.
    settings_.reset();
    shutDown_ = true;
    FlushSave();
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

ContextMenu::TrayMenuState App::MenuState() const {
    ContextMenu::TrayMenuState state;
    state.autoHide = config_.autoHide;
    state.hideTaskbar = taskbar_.Enabled();
    state.startWithWindows = AutostartEnabled();
    return state;
}

UINT App::RunMenu(const std::function<UINT()>& show) {
    dock_.SetHold(true);
    const UINT cmd = show();
    dock_.SetHold(false);
    return cmd;
}

void App::ShowMenu(POINT pt) {
    const ContextMenu::TrayMenuState state = MenuState();
    const UINT cmd = RunMenu([&] { return ContextMenu::ShowTrayMenu(dock_.Hwnd(), pt, state); });
    if (cmd != ContextMenu::kNone) OnCommand(cmd);
}

void App::ShowItemMenu(int index, POINT pt) {
    if (index < 0 || index >= static_cast<int>(dock_.Items().size())) return;
    // Copy what we need: the item list can change while the menu is open.
    const DockItem& item = dock_.Items()[static_cast<size_t>(index)];
    const AppKey key = item.key;
    const std::vector<HWND> windows = item.windows;
    const std::wstring name = item.displayName;
    ContextMenu::ItemMenuState itemState;
    itemState.pinned = item.pinned;
    itemState.canPin = !key.IsClassFallback();
    itemState.canLaunch = !key.IsClassFallback() || item.launchPath.has_value();
    itemState.running = !windows.empty();
    itemState.canOpenLocation = !item.targetPath.empty() || (!key.isAumid && !key.IsClassFallback());

    std::vector<std::wstring> titles;
    titles.reserve(windows.size());
    for (HWND w : windows) {
        wchar_t buf[256]{};
        GetWindowTextW(w, buf, static_cast<int>(std::size(buf)));  // cached caption, never blocks
        titles.emplace_back(buf);
    }
    const ContextMenu::TrayMenuState state = MenuState();
    const UINT cmd =
        RunMenu([&] { return ContextMenu::ShowItemMenu(dock_.Hwnd(), pt, name, titles, itemState, state); });
    RunItemCommand(cmd, key, windows);
}

void App::OnCommand(UINT cmd) {
    switch (cmd) {
        case ContextMenu::kAutoHide:
            SetAutoHide(config_.AutoHideOn() ? AutoHideMode::Off : lastOnMode_);
            RefreshSettings();
            break;
        case ContextMenu::kAutoHideOff:
            SetAutoHide(AutoHideMode::Off);
            RefreshSettings();
            break;
        case ContextMenu::kAutoHideAlways:
            SetAutoHide(AutoHideMode::Always);
            RefreshSettings();
            break;
        case ContextMenu::kAutoHideIntelligent:
            SetAutoHide(AutoHideMode::Intelligent);
            RefreshSettings();
            break;
        case ContextMenu::kHideTaskbar: {
            const bool hide = !taskbar_.Enabled();
            config_.hideTaskbar = hide;
            taskbar_.SetEnabled(hide);
            RequestSave();
            RefreshSettings();
            break;
        }
        case ContextMenu::kStartWithWindows: {
            const bool enable = !AutostartEnabled();
            if (SetAutostart(enable)) {
                config_.startWithWindows = enable;
                RequestSave();
            } else {
                MessageBeep(MB_ICONWARNING);
            }
            RefreshSettings();
            break;
        }
        case ContextMenu::kOpenSettings:
            OpenSettings();
            break;
        case ContextMenu::kOpenConfig:
            OpenConfigFile();
            break;
        case ContextMenu::kReloadConfig:
            ReloadConfig();
            break;
        case ContextMenu::kRestoreTaskbar:
            // For this session only: the controller is disabled (so "Hide taskbar" shows
            // unchecked and Explorer restarts do not re-hide), config.hideTaskbar is untouched.
            taskbar_.SetEnabled(false);
            RefreshSettings();
            break;
        case ContextMenu::kQuit:
            Quit();
            break;
        default:
            break;
    }
}

void App::SetAutoHide(AutoHideMode mode) {
    if (mode != AutoHideMode::Off) lastOnMode_ = mode;
    config_.autoHide = mode;
    dock_.ApplyAutoHide();
    RequestSave();
}

void App::ReloadConfig() {
    Config fresh;
    const Config::LoadResult result = fresh.LoadFile();
    if (result == Config::LoadResult::Malformed) {
        // Keep running with the current settings; the user can fix the file and reload again.
        MessageBeep(MB_ICONWARNING);
        return;
    }
    if (result == Config::LoadResult::Missing) {
        config_.Save();  // the file was deleted: write the current settings back
        return;
    }
    // A pending debounced save would overwrite the file we just read.
    if (savePending_) {
        KillTimer(dock_.Hwnd(), kSaveTimerId);
        savePending_ = false;
    }
    fresh.renderer = config_.renderer;  // the device type is chosen at startup only
    config_ = std::move(fresh);
    if (config_.AutoHideOn()) lastOnMode_ = config_.autoHide;
    // Unknown preset or broken theme file: the dock uses dark (the config itself still applies).
    if (config_.themeResolveFailed) MessageBeep(MB_ICONWARNING);
    dock_.ApplyConfig();
    dock_.ReloadPins();
    if (taskbar_.Enabled() != config_.hideTaskbar) taskbar_.SetEnabled(config_.hideTaskbar);
    if (AutostartEnabled() != config_.startWithWindows) SetAutostart(config_.startWithWindows);
    RefreshSettings();
}

void App::RefreshSettings() {
    if (settings_) settings_->Refresh();
}

void App::OpenSettings() {
    if (shutDown_) return;
    if (settings_ && settings_->Hwnd()) {
        settings_->Activate();
        return;
    }
    SettingsHost host;
    host.config = &config_;
    host.changed = [this](SettingsScope scope) { OnSettingsChanged(scope); };
    host.pins = [this]() -> const std::vector<AppIdentity::ResolvedPin>& { return dock_.Pins(); };
    host.pinFiles = [this](const std::vector<std::wstring>& paths) { PinFiles(paths, config_.pinned.size()); };
    host.command = [this](UINT cmd) { OnCommand(cmd); };
    host.usingWarp = [this] { return dock_.UsingWarp(); };
    host.dockMonitor = [this] { return dock_.MonitorRect(); };
    host.dockDpi = [this] { return dock_.Dpi(); };
    host.keepDockShown = [this](bool keep) {
        if (!shutDown_) dock_.SetKeepShown(keep);
    };
    host.dockBar = [this] { return dock_.RestBarRect(); };
    host.taskbarHidden = [this] { return taskbar_.Enabled(); };
    host.autostartEnabled = [] { return AutostartEnabled(); };
    host.destroyed = [this] {
        if (dock_.Hwnd()) PostMessageW(dock_.Hwnd(), kSettingsClosed, 0, 0);
    };
    settings_ = std::make_unique<SettingsWindow>(std::move(host));
    if (!settings_->Create(instance_)) {
        settings_.reset();
        MessageBeep(MB_ICONWARNING);
    }
}

void App::OnSettingsChanged(SettingsScope scope) {
    if (shutDown_) return;
    switch (scope) {
        case SettingsScope::Appearance:
            dock_.ApplyConfig();
            break;
        case SettingsScope::AutoHide:
            SetAutoHide(config_.autoHide);  // keeps lastOnMode_ in step; saves
            return;
        case SettingsScope::Pins:
            dock_.ReloadPins();
            break;
        case SettingsScope::Taskbar:
            taskbar_.SetEnabled(config_.hideTaskbar);
            break;
        case SettingsScope::Autostart:
            if (!SetAutostart(config_.startWithWindows)) {
                MessageBeep(MB_ICONWARNING);
                config_.startWithWindows = AutostartEnabled();
                RefreshSettings();
            }
            break;
        case SettingsScope::SaveOnly:
            break;
    }
    RequestSave();
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
        OnCommand(ContextMenu::kRestoreTaskbar);
        return true;
    }
    if (msgOpenSettings_ && msg == msgOpenSettings_) {
        OpenSettings();
        return true;
    }
    if (msg == kSettingsClosed) {
        // The window is gone (its WM_NCDESTROY posted this); a newly opened one is kept.
        if (settings_ && !settings_->Hwnd()) settings_.reset();
        return true;
    }
    if (msgQueryState_ && msg == msgQueryState_ && wp == 16) {
        result = settings_ && settings_->Hwnd() ? 1 : 0;  // 16: the settings window exists
        return true;
    }
    if (msgDockCommand_ && msg == msgDockCommand_) {
        const UINT cmd = static_cast<UINT>(wp);
        const bool known = cmd >= ContextMenu::kAutoHide && cmd <= ContextMenu::kLastDockCommand;
        result = known ? 1 : 0;
        if (known) OnCommand(cmd);  // after setting result: kQuit destroys the window
        return true;
    }
    if (msgItemCommand_ && msg == msgItemCommand_) {
        int index = -1;
        HWND target = reinterpret_cast<HWND>(lp);
        if (lp > 0xFFFF && IsWindow(target)) {
            index = dock_.IndexOfWindow(target);
        } else if (lp >= 0 && lp < static_cast<LPARAM>(dock_.Items().size())) {
            index = static_cast<int>(lp);
        }
        if (index >= 0) {
            const DockItem& item = dock_.Items()[static_cast<size_t>(index)];
            const AppKey key = item.key;
            const std::vector<HWND> windows = item.windows;
            RunItemCommand(static_cast<UINT>(wp), key, windows);
        }
        result = index >= 0 ? 1 : 0;
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
            if (dock_.InDrag()) return true;  // no menus in the middle of a drag
            const POINT client{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            POINT pt = client;
            ClientToScreen(dock_.Hwnd(), &pt);
            if (const int index = dock_.IndexAt(client); index >= 0) {
                ShowItemMenu(index, pt);
            } else {
                ShowMenu(pt);
            }
            return true;
        }
        case WM_TIMER:
            if (wp != kSaveTimerId) return false;
            KillTimer(dock_.Hwnd(), kSaveTimerId);  // one-shot
            savePending_ = false;
            config_.Save();
            return true;
        case WM_QUERYENDSESSION:
            FlushSave();
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
