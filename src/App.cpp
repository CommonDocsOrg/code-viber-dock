#include "App.h"

#include <dbt.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <string>
#include <vector>

#include "AppIdentity.h"
#include "ContextMenu.h"
#include "IconUtil.h"
#include "SettingsIds.h"
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

// Empties the Recycle Bin with the shell's confirmation and progress UI, on a thread of its own
// so the dock keeps running while the dialog is up (the dock watches the bin for the result).
DWORD WINAPI EmptyRecycleBinThread(LPVOID) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    SHEmptyRecycleBinW(nullptr, nullptr, 0);
    if (SUCCEEDED(co)) CoUninitialize();
    return 0;
}

// ChooseItemIcon: while its dialog is open, a thread-local CBT hook posts every activation on the
// UI thread (wParam = the window) to g_activateTarget as g_activateMessage.
HWND g_activateTarget = nullptr;
UINT g_activateMessage = 0;

LRESULT CALLBACK ActivateHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_ACTIVATE && g_activateTarget) PostMessageW(g_activateTarget, g_activateMessage, wp, 0);
    return CallNextHookEx(nullptr, code, wp, lp);
}

// Automation lParam: the dock (high word) and the old argument (low word). A negative lParam
// keeps its value for dock 0.
size_t DockOfParam(LPARAM lp) { return lp >= 0 ? static_cast<size_t>((lp >> 16) & 0xFFFF) : 0; }
LPARAM ArgOfParam(LPARAM lp) { return lp >= 0 ? (lp & 0xFFFF) : lp; }

constexpr UINT kReapMessage = WM_APP + 5;  // posted: free closed dock windows (DockManager::Reap)

}  // namespace

App::~App() { Shutdown(); }

LRESULT CALLBACK App::AppWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    const LRESULT result = self->OnAppMessage(msg, wp, lp);
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        self->hwnd_ = nullptr;
    }
    return result;
}

bool App::Initialize(HINSTANCE instance) {
    instance_ = instance;
    msgQuit_ = RegisterWindowMessageW(AppIpc::kQuitMessage);
    msgRestoreTaskbar_ = RegisterWindowMessageW(AppIpc::kRestoreTaskbarMessage);
    msgTaskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    msgItemCommand_ = RegisterWindowMessageW(AppIpc::kItemCommandMessage);
    msgDockCommand_ = RegisterWindowMessageW(AppIpc::kDockCommandMessage);
    msgOpenSettings_ = RegisterWindowMessageW(AppIpc::kOpenSettingsMessage);
    msgQueryState_ = RegisterWindowMessageW(DockWindow::kQueryStateMessage);
    msgQueryItem_ = RegisterWindowMessageW(DockWindow::kQueryItemMessage);

    if (config_.Load()) {
        // First run: no config file yet. Seed the default pins and write the file.
        SeedDefaultPins();
        config_.Save();
    } else if (config_.migrated) {
        config_.Save();  // a legacy file (pinned[], top-level dock keys): rewrite it as docks[] once
    }
    if (config_.Main().AutoHideOn()) lastOnMode_ = config_.Main().autoHide;

    // Crash recovery first, before anything else can fail: if a previous run died with the
    // taskbar hidden, put it back using the state saved in the marker.
    taskbar_.RecoverFromMarker();

    // The app window: hidden, top-level (it receives the broadcasts: TaskbarCreated, display and
    // setting changes, the end of the session), never shown or activated.
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &App::AppWndProc;
    wc.hInstance = instance;
    wc.lpszClassName = AppIpc::kAppWindowClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    if (!CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, AppIpc::kAppWindowClass, L"WinDock", WS_POPUP, 0, 0, 0, 0,
                         nullptr, nullptr, instance, this)) {
        return false;
    }

    DockManager::Callbacks callbacks;
    callbacks.hook = [this](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT& r) {
        return OnDockMessage(hwnd, msg, wp, lp, r);
    };
    callbacks.setup = [this](DockWindow& dock) { SetUpDock(dock); };
    callbacks.weatherChanged = [this] {
        if (settings_) settings_->WeatherChanged();  // the Weather page's status line
    };
    docks_.Initialize(instance, &config_, hwnd_, std::move(callbacks));
    const HRESULT hr = docks_.Sync();
    if (FAILED(hr)) {
        wchar_t text[256];
        swprintf_s(text, L"WinDock could not initialize Direct2D / DirectComposition (error 0x%08X).",
                   static_cast<unsigned>(hr));
        MessageBoxW(nullptr, text, L"WinDock", MB_ICONERROR | MB_OK);
        docks_.Shutdown();
        DestroyWindow(hwnd_);
        return false;
    }

    AddTrayIcon();
    taskbar_.SetEnabled(config_.hideTaskbar);
    // Only now: the controller has saved the original work area (without our reservations).
    docks_.AllowAppBar(true);
    UpdateReservedMonitors();
    return true;
}

void App::SetUpDock(DockWindow& dock) {
    DockWindow* d = &dock;  // the requests come from the dock itself, while it exists
    // Drag to reorder / unpin and file drops (the dock registers itself as an OLE drop target).
    DockWindow::PinRequests requests;
    requests.reorder = [this, d](const std::vector<AppKey>& order) { ApplyPinOrder(*d, order); };
    requests.unpin = [this, d](const AppKey& key) {
        const int index = IndexOfKey(*d, key);
        if (index >= 0 && d->Items()[static_cast<size_t>(index)].pinned) TogglePin(*d, key);
    };
    requests.pinFiles = [this, d](const std::vector<std::wstring>& paths, size_t at) {
        PinFiles(d->ConfigIndex(), d->Pins(), paths, at);
    };
    requests.moveItem = [this, d](size_t from, size_t to) { MoveItem(d->ConfigIndex(), from, to); };
    requests.moveDock = [this, d](DockEdge edge, DockAlign align, int offset) {
        const size_t cfg = d->ConfigIndex();
        DockConfig& c = config_.docks[cfg];
        if (c.edge == edge && c.align == align && c.offset == offset) {
            d->Relayout();  // back to its own place
            return;
        }
        c.edge = edge;
        c.align = align;
        c.offset = offset;
        docks_.ForConfig(cfg, [](DockWindow& w) { w.ApplyConfig(); });
        RequestSave();
        RefreshSettings();
    };
    dock.SetPinRequests(std::move(requests));
    // The docks' app bars (reserve screen space): the taskbar controller must not undo the work
    // areas they leave.
    dock.SetAppBarCallback([this](bool, const RECT&, const RECT&) { UpdateReservedMonitors(); });
    DockWindow::WeatherCallbacks weather;
    weather.openSettings = [this] { OpenSettings(SettingsIds::kPageWeather); };
    dock.SetWeatherCallbacks(std::move(weather));
    if (keepShown_) dock.SetKeepShown(true);
}

void App::UpdateReservedMonitors() {
    const std::vector<RECT> rects = docks_.ReservedMonitors();
    taskbar_.SetReservedMonitors(rects.data(), static_cast<int>(rects.size()));
}

void App::SeedDefaultPins() {
    wchar_t windir[MAX_PATH]{};
    const UINT n = GetWindowsDirectoryW(windir, MAX_PATH);
    DockItemEntry explorer;
    explorer.path = (n > 0 && n < MAX_PATH ? std::wstring(windir) : std::wstring(L"C:\\Windows")) + L"\\explorer.exe";
    config_.Main().InsertPin(SIZE_MAX, std::move(explorer));  // before the default running / clock items

    static constexpr wchar_t kTerminal[] = L"Microsoft.WindowsTerminal_8wekyb3d8bbwe!App";
    if (AppIdentity::AumidInstalled(kTerminal)) {
        DockItemEntry terminal;
        terminal.aumid = kTerminal;
        config_.Main().InsertPin(SIZE_MAX, std::move(terminal));
    }
}

void App::RequestSave() {
    if (!hwnd_) {
        config_.Save();
        return;
    }
    SetTimer(hwnd_, kSaveTimerId, kSaveDelayMs, nullptr);  // restarts the debounce window
    savePending_ = true;
}

void App::FlushSave() {
    if (!savePending_) return;
    if (hwnd_) KillTimer(hwnd_, kSaveTimerId);
    savePending_ = false;
    config_.Save();
}

int App::IndexOfKey(const DockWindow& dock, const AppKey& key) {
    const auto& items = dock.Items();
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].key == key) return static_cast<int>(i);
    }
    return -1;
}

void App::ReloadPins(size_t cfg) {
    docks_.ForConfig(cfg, [](DockWindow& w) { w.ReloadPins(); });
}

void App::TogglePin(DockWindow& dock, const AppKey& key) {
    const int index = IndexOfKey(dock, key);
    if (index < 0) return;
    const DockItem& item = dock.Items()[static_cast<size_t>(index)];
    const size_t cfg = dock.ConfigIndex();
    DockConfig& dc = config_.docks[cfg];

    if (item.pinned) {
        // Unpin: drop every config entry that resolves to this app. The item stays while the
        // app has windows (as a running item) and disappears when they close.
        const auto& pins = dock.Pins();  // parallel to the pin items of the dock's config
        for (size_t i = std::min(pins.size(), dc.PinCount()); i-- > 0;) {
            if (pins[i].key == key) dc.ErasePin(i);
        }
    } else {
        // Pinning appends to the end of the pinned list; the item moves to that slot.
        std::optional<DockItemEntry> e = EntryForItem(item);
        if (!e) return;  // nothing we could launch
        dc.InsertPin(SIZE_MAX, std::move(*e));
    }
    ReloadPins(cfg);
    RequestSave();
    RefreshSettings();  // the Dock contents page
}

bool App::SetPinIcon(DockWindow& dock, const AppKey& key, const std::wstring& icon) {
    const auto& pins = dock.Pins();  // parallel to the pin items of the dock's config
    const size_t cfg = dock.ConfigIndex();
    DockConfig& dc = config_.docks[cfg];
    const std::vector<size_t> idx = dc.PinIndexes();
    bool found = false, changed = false;
    for (size_t i = 0; i < pins.size() && i < idx.size(); ++i) {
        if (pins[i].key != key || pins[i].kind == AppIdentity::PinKind::Separator) continue;
        found = true;
        DockItemEntry& e = dc.items[idx[i]];
        if (e.icon == icon) continue;
        e.icon = icon;  // stored as given (relative / %VAR% paths stay as written)
        changed = true;
    }
    if (!changed) return found;
    ReloadPins(cfg);
    RequestSave();
    RefreshSettings();  // the Dock contents page
    return true;
}

void App::ChooseItemIcon(DockWindow& dock, UINT cmd, const AppKey& key) {
    std::wstring file, image;  // copied: the pins are rebuilt while the dialog is open
    int index = 0;
    {
        const auto& pins = dock.Pins();
        const auto it = std::find_if(pins.begin(), pins.end(), [&](const AppIdentity::ResolvedPin& p) {
            return p.key == key && p.kind != AppIdentity::PinKind::Separator;
        });
        if (it == pins.end()) return;  // not pinned: nothing to store the icon in
        IconUtil::StartIconFor(*it, file, index);
        image = IconUtil::StartImageFor(*it);
    }
    // Owned by the (topmost) dock window, so the dialog is in front of it; the hold keeps the
    // dock shown while the mouse is over the dialog. The pin list may change meanwhile (the
    // settings window): SetPinIcon looks the key up again.
    const HWND owner = dock.Hwnd();
    std::wstring spec;
    dock.SetHold(true);
    ++modalDepth_;  // display changes wait: the dock must not go away under its dialog
    // Activations on this thread while the dialog is open are reported to OnAppMessage
    // (kDialogActivated), which gives the dialog the keyboard focus if it has none.
    g_activateTarget = hwnd_;
    g_activateMessage = kDialogActivated;
    HHOOK hook = SetWindowsHookExW(WH_CBT, &ActivateHook, nullptr, GetCurrentThreadId());
    const bool ok = cmd == ContextMenu::kChooseIcon ? IconUtil::ChooseIconResource(owner, file, index, spec)
                                                    : IconUtil::ChooseIconFile(owner, image, spec);
    if (hook) UnhookWindowsHookEx(hook);
    g_activateTarget = nullptr;
    // Quit (automation) may have closed the docks meanwhile: look the window up again.
    DockWindow* again = shutDown_ ? nullptr : docks_.ByHwnd(owner);
    if (again) again->SetHold(false);
    EndModal();
    if (ok && again) SetPinIcon(*again, key, spec);
}

std::optional<DockItemEntry> App::EntryForItem(const DockItem& item) {
    // Widgets (the clock, the weather) are items of their own kind, not pins.
    if (item.key.IsClassFallback() || item.IsWidget()) return std::nullopt;
    DockItemEntry e;
    if (item.kind == DockItem::Kind::Folder) e.kind = ToString(ItemKind::Folder);
    if (item.launchPath && !item.launchPath->empty()) {
        e.path = *item.launchPath;
    } else if (item.key.isAumid) {
        e.aumid = item.key.Source();
    } else {
        e.path = item.key.Source();
    }
    return e;
}

void App::ApplyPinOrder(DockWindow& dock, const std::vector<AppKey>& order) {
    const auto& pins = dock.Pins();  // parallel to the pin items of the dock's config
    const size_t cfg = dock.ConfigIndex();
    const std::vector<DockItemEntry> current = config_.docks[cfg].Pins();
    std::vector<bool> used(current.size(), false);
    std::vector<DockItemEntry> next;
    next.reserve(current.size() + 1);
    for (const AppKey& key : order) {
        bool found = false;
        // Every entry resolving to this app moves along (duplicates stay together).
        for (size_t i = 0; i < current.size() && i < pins.size(); ++i) {
            if (!used[i] && pins[i].key == key) {
                next.push_back(current[i]);
                used[i] = true;
                found = true;
            }
        }
        if (found) continue;
        const int index = IndexOfKey(dock, key);  // a running item dragged into the pinned section
        if (index < 0) continue;
        if (std::optional<DockItemEntry> e = EntryForItem(dock.Items()[static_cast<size_t>(index)])) {
            next.push_back(std::move(*e));
        }
    }
    for (size_t i = 0; i < current.size(); ++i) {
        if (!used[i]) next.push_back(current[i]);  // not in `order`: keep, after the others
    }
    config_.docks[cfg].SetPins(std::move(next));
    ReloadPins(cfg);
    RequestSave();
    RefreshSettings();
}

void App::PinFiles(size_t cfg, const std::vector<AppIdentity::ResolvedPin>& pins,
                   const std::vector<std::wstring>& paths, size_t at) {
    if (cfg >= config_.docks.size()) return;
    DockConfig& dock = config_.docks[cfg];
    // Already pinned apps (by key) are skipped, also among the dropped files themselves. `pins`
    // are the dock's resolved pins (empty for a parked dock: its entries are resolved here).
    std::vector<AppKey> keys;
    if (pins.size() == dock.PinCount()) {
        for (const auto& existing : pins) keys.push_back(existing.key);
    } else {
        for (const DockItemEntry& e : dock.Pins()) keys.push_back(AppIdentity::ResolvePin(e).key);
    }
    at = std::min(at, dock.PinCount());
    bool changed = false;
    for (const std::wstring& path : paths) {
        DockItemEntry entry;
        entry.path = path;
        const AppIdentity::ResolvedPin pin = AppIdentity::ResolvePin(entry);
        if (std::find(keys.begin(), keys.end(), pin.key) != keys.end()) continue;
        keys.push_back(pin.key);
        DockItemEntry e;
        e.path = pin.launchPath.empty() ? path : pin.launchPath;
        if (pin.kind == AppIdentity::PinKind::Folder) e.kind = ToString(ItemKind::Folder);
        dock.InsertPin(at++, std::move(e));
        changed = true;
    }
    if (!changed) return;
    ReloadPins(cfg);
    RequestSave();
    RefreshSettings();
}

void App::MoveItem(size_t cfg, size_t from, size_t to) {
    if (cfg >= config_.docks.size() || !config_.docks[cfg].MoveItem(from, to)) return;
    ReloadPins(cfg);
    RequestSave();
    RefreshSettings();  // the Dock contents page
}

void App::AddWidget(size_t cfg, ItemKind kind) {
    if (config_.docks[cfg].AddWidget(kind) == SIZE_MAX) {
        MessageBeep(MB_ICONWARNING);  // a second running item
        return;
    }
    ReloadPins(cfg);  // also starts the weather service for a weather item
    RequestSave();
    RefreshSettings();
}

void App::RunItemCommand(DockWindow& dock, UINT cmd, const AppKey& key, const std::vector<HWND>& windows) {
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
            TogglePin(dock, key);
            return;
        case ContextMenu::kNewWindow:
            dock.LaunchItem(IndexOfKey(dock, key), true);
            return;
        case ContextMenu::kOpenInExplorer: {
            const int index = IndexOfKey(dock, key);
            if (index >= 0 && dock.Items()[static_cast<size_t>(index)].kind == DockItem::Kind::Folder) {
                dock.LaunchItem(index, false);  // a folder opens in Explorer
            }
            return;
        }
        case ContextMenu::kToggleFolderPopup:
            dock.ToggleFolderPopup(IndexOfKey(dock, key));
            return;
        case ContextMenu::kChooseIcon:
        case ContextMenu::kChooseImage:
            ChooseItemIcon(dock, cmd, key);
            return;
        case ContextMenu::kResetIcon:
            SetPinIcon(dock, key, {});
            return;
        case ContextMenu::kEmptyRecycleBin: {
            const int index = IndexOfKey(dock, key);
            if (index < 0 || dock.Items()[static_cast<size_t>(index)].kind != DockItem::Kind::RecycleBin) return;
            AllowSetForegroundWindow(ASFW_ANY);
            if (HANDLE thread = CreateThread(nullptr, 0, &EmptyRecycleBinThread, nullptr, 0, nullptr)) {
                CloseHandle(thread);
            } else {
                MessageBeep(MB_ICONWARNING);
            }
            return;
        }
        case ContextMenu::kOpenFileLocation: {
            const int index = IndexOfKey(dock, key);
            if (index < 0) return;
            const DockItem& item = dock.Items()[static_cast<size_t>(index)];
            std::wstring target = item.targetPath;
            if (target.empty() && !key.isAumid && !key.IsClassFallback() && !key.IsSynthetic()) target = key.Source();
            if (target.empty()) return;
            const std::wstring args = L"/select,\"" + target + L"\"";
            AllowSetForegroundWindow(ASFW_ANY);
            ShellExecuteW(nullptr, nullptr, L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
            return;
        }
        default:
            if (cmd != ContextMenu::kNone) OnCommand(cmd, dock.ConfigIndex(), &dock);
            return;
    }
}

void App::Shutdown() {
    if (shutDown_) return;
    // First: closing it may commit a pending edit (focus loss), which must still be saved.
    settings_.reset();
    geocodeDone_ = nullptr;
    shutDown_ = true;
    FlushSave();
    docks_.AllowAppBar(false);  // first: the restored work area must not keep our bands
    taskbar_.SetReservedMonitors(nullptr, 0);
    taskbar_.Restore();
    RemoveTrayIcon();
    docks_.Shutdown();
    if (trayIcon_) {
        DestroyIcon(trayIcon_);
        trayIcon_ = nullptr;
    }
    // Destroying the app window posts WM_QUIT, which ends the message loop in wWinMain.
    if (hwnd_) DestroyWindow(hwnd_);
}

void App::Quit() { Shutdown(); }

void App::AddTrayIcon() {
    if (!hwnd_) return;
    if (!trayIcon_) {
        const UINT dpi = GetDpiForSystem();
        trayIcon_ = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                                  GetSystemMetricsForDpi(SM_CXSMICON, dpi),
                                                  GetSystemMetricsForDpi(SM_CYSMICON, dpi), LR_DEFAULTCOLOR));
    }

    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd_;
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
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    trayAdded_ = false;
}

ContextMenu::TrayMenuState App::MenuState(size_t cfg) const {
    const DockConfig& d = config_.docks[cfg < config_.docks.size() ? cfg : 0];
    ContextMenu::TrayMenuState state;
    state.autoHide = d.autoHide;
    state.hideTaskbar = taskbar_.Enabled();
    state.startWithWindows = AutostartEnabled();
    state.weatherShown = d.Has(ItemKind::Weather);
    state.weatherConfigured = config_.weather.Configured();
    state.locked = d.locked;
    state.hasRunning = d.Has(ItemKind::Running);
    state.edge = d.edge;
    state.dockCount = static_cast<int>(config_.docks.size());
    for (const DockConfig& o : config_.docks) {
        (o.locked ? state.anyLocked : state.anyUnlocked) = true;
    }
    return state;
}

UINT App::RunMenu(DockWindow* dock, const std::function<UINT()>& show) {
    const HWND hwnd = dock ? dock->Hwnd() : nullptr;
    if (dock) dock->SetHold(true);
    ++modalDepth_;
    const UINT cmd = show();
    // The dock may be gone (quit by automation while the menu was open): look it up again.
    if (DockWindow* again = hwnd && !shutDown_ ? docks_.ByHwnd(hwnd) : nullptr) again->SetHold(false);
    EndModal();
    return cmd;
}

void App::EndModal() {
    if (modalDepth_ > 0) --modalDepth_;
    if (modalDepth_ == 0 && syncPending_ && hwnd_) PostMessageW(hwnd_, kDisplaySync, 0, 0);
}

void App::ShowMenu(DockWindow* dock, POINT pt) {
    if (!dock) dock = docks_.First();
    const size_t cfg = dock ? dock->ConfigIndex() : 0;
    const HWND owner = dock ? dock->Hwnd() : hwnd_;
    const ContextMenu::TrayMenuState state = MenuState(cfg);
    const UINT cmd = RunMenu(dock, [&] { return ContextMenu::ShowTrayMenu(owner, pt, state); });
    if (cmd == ContextMenu::kNone || shutDown_) return;
    OnCommand(cmd, cfg, dock ? docks_.ByHwnd(owner) : nullptr);
}

void App::ShowItemMenu(DockWindow& dock, int index, POINT pt) {
    if (index < 0 || index >= static_cast<int>(dock.Items().size())) return;
    // Copy what we need: the item list can change while the menu is open.
    const DockItem& item = dock.Items()[static_cast<size_t>(index)];
    const AppKey key = item.key;
    const std::vector<HWND> windows = item.windows;
    const std::wstring name = item.displayName;
    const size_t cfg = dock.ConfigIndex();
    ContextMenu::ItemMenuState itemState;
    itemState.kind = item.kind;
    itemState.pinned = item.pinned;
    if (item.kind == DockItem::Kind::RecycleBin) {
        SHQUERYRBINFO info{sizeof(info)};
        itemState.binEmpty = SUCCEEDED(SHQueryRecycleBinW(nullptr, &info)) && info.i64NumItems == 0;
    }
    if (item.pinned) {
        // Reset icon follows the config entry, so an `icon` whose file is gone can be cleared too.
        const auto& pins = dock.Pins();
        const DockConfig& dc = config_.docks[cfg];
        const std::vector<size_t> idx = dc.PinIndexes();
        for (size_t i = 0; i < pins.size() && i < idx.size(); ++i) {
            if (pins[i].key == key && !dc.items[idx[i]].icon.empty()) itemState.hasIcon = true;
        }
    }
    itemState.canPin = !key.IsClassFallback();
    itemState.canLaunch = item.kind != DockItem::Kind::Separator &&
                          (!key.IsClassFallback() || item.launchPath.has_value());
    itemState.running = !windows.empty();
    // The special kinds have no file behind their synthetic key.
    itemState.canOpenLocation =
        !item.targetPath.empty() || (!key.isAumid && !key.IsClassFallback() && !key.IsSynthetic());

    std::vector<std::wstring> titles;
    titles.reserve(windows.size());
    for (HWND w : windows) {
        wchar_t buf[256]{};
        GetWindowTextW(w, buf, static_cast<int>(std::size(buf)));  // cached caption, never blocks
        titles.emplace_back(buf);
    }
    const ContextMenu::TrayMenuState state = MenuState(cfg);
    const HWND owner = dock.Hwnd();
    const UINT cmd =
        RunMenu(&dock, [&] { return ContextMenu::ShowItemMenu(owner, pt, name, titles, itemState, state); });
    DockWindow* again = shutDown_ ? nullptr : docks_.ByHwnd(owner);
    if (again) RunItemCommand(*again, cmd, key, windows);
}

void App::OnCommand(UINT cmd, size_t cfg, DockWindow* dock) {
    if (cfg >= config_.docks.size()) cfg = 0;
    DockConfig& d = config_.docks[cfg];
    switch (cmd) {
        case ContextMenu::kAutoHide:
            SetAutoHide(cfg, d.AutoHideOn() ? AutoHideMode::Off : lastOnMode_);
            RefreshSettings();
            break;
        case ContextMenu::kAutoHideOff:
            SetAutoHide(cfg, AutoHideMode::Off);
            RefreshSettings();
            break;
        case ContextMenu::kAutoHideAlways:
            SetAutoHide(cfg, AutoHideMode::Always);
            RefreshSettings();
            break;
        case ContextMenu::kAutoHideIntelligent:
            SetAutoHide(cfg, AutoHideMode::Intelligent);
            RefreshSettings();
            break;
        case ContextMenu::kHideTaskbar: {
            const bool hide = !taskbar_.Enabled();
            config_.hideTaskbar = hide;
            SetTaskbarEnabled(hide);
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
        case ContextMenu::kRefreshWeather:
            docks_.WeatherData().RefreshNow();
            break;
        case ContextMenu::kWeatherSettings:
            OpenSettings(SettingsIds::kPageWeather);
            break;
        case ContextMenu::kToggleCalendar:
            if (dock) dock->TogglePopup(ItemKind::Clock);
            break;
        case ContextMenu::kToggleWeatherPopup:
            if (dock) dock->TogglePopup(ItemKind::Weather);
            break;
        case ContextMenu::kLockDock:
            d.locked = !d.locked;  // read by the dock when a drag / drop starts
            RequestSave();
            RefreshSettings();
            break;
        case ContextMenu::kLockAllDocks:
        case ContextMenu::kUnlockAllDocks:
            for (DockConfig& o : config_.docks) o.locked = cmd == ContextMenu::kLockAllDocks;
            RequestSave();
            RefreshSettings();
            break;
        case ContextMenu::kAddClock:
        case ContextMenu::kAddWeather:
        case ContextMenu::kAddRunning:
            if (d.locked) break;  // greyed in the menu; Settings > Dock contents still adds
            AddWidget(cfg, cmd == ContextMenu::kAddClock     ? ItemKind::Clock
                           : cmd == ContextMenu::kAddWeather ? ItemKind::Weather
                                                             : ItemKind::Running);
            break;
        case ContextMenu::kAddSeparator: {
            if (d.locked) break;
            DockItemEntry separator;
            separator.kind = ToString(ItemKind::Separator);
            d.InsertPin(SIZE_MAX, std::move(separator));  // after the last pin
            ReloadPins(cfg);
            RequestSave();
            RefreshSettings();
            break;
        }
        case ContextMenu::kEdgeBottom:
        case ContextMenu::kEdgeTop:
        case ContextMenu::kEdgeLeft:
        case ContextMenu::kEdgeRight: {
            // The dock moves to that edge of its monitor (alignment and offset are kept).
            const DockEdge edge = static_cast<DockEdge>(cmd - ContextMenu::kEdgeBottom);
            if (d.edge == edge) break;
            d.edge = edge;
            docks_.ForConfig(cfg, [](DockWindow& w) { w.ApplyConfig(); });
            RequestSave();
            RefreshSettings();
            break;
        }
        case ContextMenu::kOpenConfig:
            OpenConfigFile();
            break;
        case ContextMenu::kReloadConfig:
            ReloadConfig();
            break;
        case ContextMenu::kRestoreTaskbar:
            // For this session only: the controller is disabled (so "Hide taskbar" shows
            // unchecked and Explorer restarts do not re-hide), config.hideTaskbar is untouched.
            SetTaskbarEnabled(false);
            RefreshSettings();
            break;
        case ContextMenu::kQuit:
            Quit();
            break;
        default:
            break;
    }
}

void App::SetTaskbarEnabled(bool enabled) {
    // The controller saves / restores the work area: never with our app bars registered.
    docks_.AllowAppBar(false);
    taskbar_.SetEnabled(enabled);
    docks_.AllowAppBar(!shutDown_);
}

void App::SetAutoHide(size_t cfg, AutoHideMode mode) {
    if (mode != AutoHideMode::Off) lastOnMode_ = mode;
    config_.docks[cfg].autoHide = mode;
    docks_.ForConfig(cfg, [](DockWindow& w) { w.ApplyAutoHide(); });
    RequestSave();
}

void App::ScheduleDisplaySync() {
    if (syncPending_ || !hwnd_) return;
    syncPending_ = true;
    PostMessageW(hwnd_, kDisplaySync, 0, 0);
}

void App::SyncDocks() {
    if (shutDown_) return;
    docks_.Sync();
    UpdateReservedMonitors();
    if (hwnd_) PostMessageW(hwnd_, kReapMessage, 0, 0);  // closed windows are freed outside their messages
}

void App::ReloadConfig() {
    if (modalDepth_ > 0) {
        MessageBeep(MB_ICONWARNING);  // a menu or dialog of a dock is open: its dock must stay
        return;
    }
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
        KillTimer(hwnd_, kSaveTimerId);
        savePending_ = false;
    }
    fresh.renderer = config_.renderer;  // the device type is chosen at startup only
    fresh.editDock = config_.editDock < fresh.docks.size() ? config_.editDock : 0;
    config_ = std::move(fresh);
    if (config_.migrated) RequestSave();  // a legacy file was loaded: rewrite it as docks[]
    if (config_.Main().AutoHideOn()) lastOnMode_ = config_.Main().autoHide;
    // Unknown preset or broken theme file: the dock uses dark (the config itself still applies).
    if (config_.themeResolveFailed) MessageBeep(MB_ICONWARNING);
    // Docks added / removed / moved to another monitor; the ones that stay re-read everything.
    SyncDocks();
    docks_.ForEach([](DockWindow& w) {
        w.ApplyConfig();
        w.ReloadPins();
    });
    if (taskbar_.Enabled() != config_.hideTaskbar) SetTaskbarEnabled(config_.hideTaskbar);
    if (AutostartEnabled() != config_.startWithWindows) SetAutostart(config_.startWithWindows);
    RefreshSettings();
}

void App::RefreshSettings() {
    if (settings_) settings_->Refresh();
}

void App::OpenSettings(int page) {
    if (shutDown_) return;
    if (settings_ && settings_->Hwnd()) {
        if (page >= 0) settings_->ShowPage(page);
        settings_->Activate();
        return;
    }
    static const std::vector<AppIdentity::ResolvedPin> kNoPins;
    SettingsHost host;
    host.config = &config_;
    host.changed = [this](SettingsScope scope) { OnSettingsChanged(scope); };
    // "The dock": the first window of the dock the pages edit (config.Edit()).
    host.pins = [this]() -> const std::vector<AppIdentity::ResolvedPin>& {
        DockWindow* d = docks_.FirstOf(config_.editDock);
        return d ? d->Pins() : kNoPins;
    };
    host.pinFiles = [this](const std::vector<std::wstring>& paths) {
        DockWindow* d = docks_.FirstOf(config_.editDock);
        PinFiles(config_.editDock, d ? d->Pins() : kNoPins, paths, SIZE_MAX);
    };
    host.command = [this](UINT cmd) { OnCommand(cmd, config_.editDock, docks_.FirstOf(config_.editDock)); };
    host.usingWarp = [this] {
        DockWindow* d = docks_.First();
        return d && d->UsingWarp();
    };
    host.dockMonitor = [this] {
        if (DockWindow* d = docks_.FirstOf(config_.editDock)) return d->MonitorRect();
        if (DockWindow* d = docks_.First()) return d->MonitorRect();
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
        return mi.rcMonitor;
    };
    host.dockDpi = [this] {
        DockWindow* d = docks_.FirstOf(config_.editDock);
        if (!d) d = docks_.First();
        return d ? d->Dpi() : GetDpiForSystem();
    };
    host.keepDockShown = [this](bool keep) {
        if (shutDown_) return;
        keepShown_ = keep;
        docks_.ForEach([&](DockWindow& w) { w.SetKeepShown(keep); });
    };
    host.dockBar = [this] {
        DockWindow* d = docks_.FirstOf(config_.editDock);
        return d ? d->RestBarRect() : RECT{};
    };
    host.taskbarHidden = [this] { return taskbar_.Enabled(); };
    host.autostartEnabled = [] { return AutostartEnabled(); };
    host.weatherStatus = [this] { return docks_.WeatherData().StatusText(); };
    host.geocode = [this](const std::wstring& query,
                          std::function<void(const std::vector<WeatherService::Place>&, const std::wstring&)> done) {
        // A newer search replaces an older one (its result is dropped when it arrives).
        if (!WeatherService::Geocode(query, hwnd_, WeatherService::kGeocodeMessage, ++geocodeTag_)) return false;
        geocodeDone_ = std::move(done);
        return true;
    };
    host.destroyed = [this] {
        if (hwnd_) PostMessageW(hwnd_, kSettingsClosed, 0, 0);
    };
    host.editDocks = [this](SettingsDockEdit edit, size_t index) { return EditDocks(edit, index); };
    host.dockWindows = [this](size_t index) {
        int n = 0;
        docks_.ForConfig(index, [&](DockWindow&) { ++n; });
        return n;
    };
    host.dockRects = [this] {
        std::vector<SettingsDockRect> rects;
        for (const DockManager::Instance& i : docks_.Instances()) {
            SettingsDockRect r;
            r.cfg = i.cfg;
            r.monitor = i.window ? i.monitorIndex : -1;
            if (i.window) r.bar = i.window->RestBarRect();
            rects.push_back(r);
        }
        return rects;
    };
    settings_ = std::make_unique<SettingsWindow>(std::move(host));
    if (!settings_->Create(instance_)) {
        settings_.reset();
        MessageBeep(MB_ICONWARNING);
        return;
    }
    if (page >= 0) settings_->ShowPage(page);
}

bool App::EditDocks(SettingsDockEdit edit, size_t index) {
    if (shutDown_ || modalDepth_ > 0) return false;  // a dock's menu / dialog is open
    auto& list = config_.docks;
    switch (edit) {
        case SettingsDockEdit::Add: {
            // Bottom, on the primary monitor, the running apps and the clock (DockConfig defaults).
            DockConfig d;
            d.id = config_.UniqueDockId(L"dock");
            d.monitor = DockConfig::kMonitorPrimary;
            list.push_back(std::move(d));
            config_.editDock = list.size() - 1;
            break;
        }
        case SettingsDockEdit::Duplicate: {
            if (index >= list.size()) return false;
            DockConfig d = list[index];
            d.id = config_.UniqueDockId(d.id);
            list.insert(list.begin() + static_cast<ptrdiff_t>(index) + 1, std::move(d));
            config_.editDock = index + 1;
            break;
        }
        case SettingsDockEdit::Remove:
            if (index >= list.size() || list.size() <= 1) return false;  // never the last dock
            list.erase(list.begin() + static_cast<ptrdiff_t>(index));
            if (config_.editDock > index || config_.editDock >= list.size()) {
                config_.editDock = config_.editDock > 0 ? config_.editDock - 1 : 0;
            }
            break;
    }
    SyncDocks();
    RequestSave();
    return true;
}

void App::OnSettingsChanged(SettingsScope scope) {
    if (shutDown_) return;
    const size_t cfg = config_.editDock < config_.docks.size() ? config_.editDock : 0;
    switch (scope) {
        case SettingsScope::Appearance:
            // Sizes, position, monitor, theme, clock: the theme may be per dock, the monitor
            // decides which windows exist.
            config_.ResolveDockThemes();
            if (modalDepth_ == 0) SyncDocks();
            docks_.ForEach([](DockWindow& w) { w.ApplyConfig(); });
            break;
        case SettingsScope::AutoHide:
            SetAutoHide(cfg, config_.docks[cfg].autoHide);  // keeps lastOnMode_ in step; saves
            return;
        case SettingsScope::Pins:
            ReloadPins(cfg);
            break;
        case SettingsScope::Taskbar:
            SetTaskbarEnabled(config_.hideTaskbar);
            break;
        case SettingsScope::Autostart:
            if (!SetAutostart(config_.startWithWindows)) {
                MessageBeep(MB_ICONWARNING);
                config_.startWithWindows = AutostartEnabled();
                RefreshSettings();
            }
            break;
        case SettingsScope::Weather:
            docks_.ForEach([](DockWindow& w) { w.ApplyWeather(); });
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

LRESULT App::QueryState(WPARAM wp, LPARAM lp) {
    const size_t index = DockOfParam(lp);
    const LPARAM arg = ArgOfParam(lp);
    const DockManager::Instance* inst = docks_.At(index);
    switch (wp) {
        case 16:
            return settings_ && settings_->Hwnd() ? 1 : 0;  // the settings window exists
        case 47:
            return static_cast<LRESULT>(docks_.Count());  // dock instances (parked ones included)
        case 48:
            return inst ? inst->monitorIndex : -2;  // the dock's monitor index (-1 parked, -2 no such dock)
        case 49:
            return inst ? (inst->window ? 0 : 1) : -1;  // parked (no window)
        case 50:
            return inst ? static_cast<LRESULT>(inst->cfg) : -1;  // its index in docks[]
        case 51:
            return inst && inst->window ? reinterpret_cast<LRESULT>(inst->window->Hwnd()) : 0;  // its window
        default:
            break;
    }
    if (!inst || !inst->window) return -1;
    return inst->window->QueryState(wp, arg);
}

bool App::OnSharedMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) {
    result = 0;
    if (!msg) return false;
    if (msg == msgQuit_) {
        Quit();
        return true;
    }
    if (msg == msgRestoreTaskbar_) {
        OnCommand(ContextMenu::kRestoreTaskbar, 0, nullptr);
        return true;
    }
    if (msg == msgOpenSettings_) {
        OpenSettings();
        return true;
    }
    if (msg == msgQueryState_) {
        result = QueryState(wp, lp);
        return true;
    }
    if (msg == msgQueryItem_) {
        DockWindow* dock = docks_.WindowAt(DockOfParam(lp));
        result = dock ? dock->QueryItemPoint(wp, ArgOfParam(lp)) : -1;
        return true;
    }
    if (msg == msgDockCommand_) {
        const UINT cmd = static_cast<UINT>(wp);
        DockManager::Instance* inst = docks_.At(DockOfParam(lp));
        const bool known = inst && cmd >= ContextMenu::kAutoHide && cmd <= ContextMenu::kLastDockCommand;
        result = known ? 1 : 0;
        if (known) OnCommand(cmd, inst->cfg, inst->window.get());  // after setting result: kQuit closes the docks
        return true;
    }
    if (msg == msgItemCommand_) {
        int index = -1;
        DockWindow* dock = nullptr;
        size_t dockIndex = 0;
        HWND target = reinterpret_cast<HWND>(lp);
        // A window one of the docks shows (the first one showing it), else dock / item indexes.
        if (lp > 0xFFFF && IsWindow(target)) {
            for (size_t i = 0; i < docks_.Count() && !dock; ++i) {
                DockWindow* w = docks_.WindowAt(i);
                const int at = w ? w->IndexOfWindow(target) : -1;
                if (at >= 0) {
                    dock = w;
                    index = at;
                    dockIndex = i;
                }
            }
        }
        if (!dock) {
            dockIndex = DockOfParam(lp);
            dock = docks_.WindowAt(dockIndex);
            const LPARAM item = ArgOfParam(lp);
            if (dock && item >= 0 && item < static_cast<LPARAM>(dock->Items().size())) index = static_cast<int>(item);
        }
        const UINT cmd = static_cast<UINT>(wp);
        if (index >= 0 && (cmd == ContextMenu::kChooseIcon || cmd == ContextMenu::kChooseImage) && InSendMessage()) {
            // A modal dialog would block the sender until it closes: run it after the call.
            PostMessageW(dock->Hwnd(), msgItemCommand_, wp, static_cast<LPARAM>((dockIndex << 16) | static_cast<size_t>(index)));
        } else if (index >= 0) {
            const DockItem& item = dock->Items()[static_cast<size_t>(index)];
            const AppKey key = item.key;
            const std::vector<HWND> windows = item.windows;
            RunItemCommand(*dock, cmd, key, windows);
        }
        result = index >= 0 ? 1 : 0;
        return true;
    }
    return false;
}

bool App::OnDockMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) {
    result = 0;
    if (msg >= 0xC000 && OnSharedMessage(msg, wp, lp, result)) return true;  // registered messages
    switch (msg) {
        case WM_RBUTTONUP: {
            DockWindow* dock = docks_.ByHwnd(hwnd);
            if (!dock) return false;
            if (dock->InDrag()) return true;  // no menus in the middle of a drag
            const POINT client{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            POINT pt = client;
            ClientToScreen(hwnd, &pt);
            // An item's own menu, or the dock menu (no item there, or a widget).
            if (const int index = dock->MenuIndexAt(client); index >= 0) {
                ShowItemMenu(*dock, index, pt);
            } else {
                ShowMenu(dock, pt);
            }
            return true;
        }
        case WM_DPICHANGED:
            ScheduleDisplaySync();  // the dock handles its own new scale
            return false;
        default:
            return false;
    }
}

LRESULT App::OnAppMessage(UINT msg, WPARAM wp, LPARAM lp) {
    if (docks_.HandleMessage(msg, wp, lp)) return 0;  // tracker, foreground watch, weather
    LRESULT result = 0;
    if (msg >= 0xC000 && msg != msgTaskbarCreated_ && OnSharedMessage(msg, wp, lp, result)) return result;

    if (msg == kTrayCallback) {
        // NOTIFYICON_VERSION_4: event in LOWORD(lParam), anchor point in wParam.
        switch (LOWORD(lp)) {
            case WM_CONTEXTMENU:
            case NIN_SELECT:
            case NIN_KEYSELECT:
                ShowMenu(nullptr, POINT{GET_X_LPARAM(wp), GET_Y_LPARAM(wp)});
                break;
            default:
                break;
        }
        return 0;
    }
    if (msg == kDialogActivated) {
        // An icon picker owned by a dock was activated (posted by the CBT hook, so this runs
        // once the activation is complete). After the item menu the dock was the active window:
        // the dialog then comes up active but without a focus window, and no key (Escape, Enter,
        // arrows) reaches it. Give it the focus its first control would have had.
        HWND dialog = reinterpret_cast<HWND>(wp);
        if (IsWindow(dialog) && GetActiveWindow() == dialog && !GetFocus()) {
            if (HWND first = GetNextDlgTabItem(dialog, nullptr, FALSE)) {
                SendMessageW(dialog, WM_NEXTDLGCTL, reinterpret_cast<WPARAM>(first), TRUE);
            } else {
                SetFocus(dialog);
            }
        }
        return 0;
    }
    if (msg == kSettingsClosed) {
        // The window is gone (its WM_NCDESTROY posted this); a newly opened one is kept.
        if (settings_ && !settings_->Hwnd()) {
            settings_.reset();
            geocodeDone_ = nullptr;
        }
        return 0;
    }
    if (msg == kDisplaySync) {
        if (modalDepth_ > 0) return 0;  // EndModal posts it again
        syncPending_ = false;
        SyncDocks();
        RefreshSettings();  // the Docks page's parked states, the display lists
        return 0;
    }
    if (msg == kReapMessage) {
        docks_.Reap();
        return 0;
    }
    if (msg == WeatherService::kGeocodeMessage) {
        // A place search of the settings window finished (posted by its worker; ours to free).
        std::unique_ptr<WeatherService::Response> response(reinterpret_cast<WeatherService::Response*>(lp));
        if (wp != geocodeTag_ || !geocodeDone_ || !settings_ || !response) return 0;  // superseded / closed
        std::vector<WeatherService::Place> places;
        std::wstring error;
        WeatherService::ParsePlaces(*response, places, error);
        auto done = std::move(geocodeDone_);
        geocodeDone_ = nullptr;
        done(places, error);
        return 0;
    }
    if (msgTaskbarCreated_ && msg == msgTaskbarCreated_) {
        // Explorer restarted: the tray icon is gone and the new taskbar must be re-hidden.
        trayAdded_ = false;
        AddTrayIcon();
        // The new Explorer knows no app bars: register again once the taskbar is re-hidden.
        docks_.AllowAppBar(false);
        taskbar_.OnTaskbarCreated();
        docks_.AllowAppBar(!shutDown_);
        return 0;
    }

    switch (msg) {
        case WM_TIMER:
            if (wp != kSaveTimerId) break;
            KillTimer(hwnd_, kSaveTimerId);  // one-shot
            savePending_ = false;
            config_.Save();
            return 0;
        case WM_QUERYENDSESSION:
            FlushSave();
            // Restore while Explorer is certainly still alive to persist the appbar state; if
            // the logoff is cancelled, WM_ENDSESSION(FALSE) hides the taskbar again.
            docks_.AllowAppBar(false);
            taskbar_.Restore();
            return TRUE;
        case WM_ENDSESSION:
            if (wp) {
                docks_.AllowAppBar(false);
                taskbar_.Restore();  // the session is ending (no-op if already restored)
            } else {
                if (config_.hideTaskbar) taskbar_.Hide();  // shutdown was cancelled
                docks_.AllowAppBar(true);
            }
            return 0;
        case WM_SETTINGCHANGE:
            if (wp == SPI_SETWORKAREA) taskbar_.OnWorkAreaChanged();
            return 0;  // the dock windows relayout on the same broadcast
        case WM_DISPLAYCHANGE:
            ScheduleDisplaySync();  // monitors came, went or changed: park / unpark / re-point docks
            return 0;
        case WM_DEVICECHANGE:
            if (wp == DBT_DEVNODES_CHANGED) ScheduleDisplaySync();  // a display may have been (dis)connected
            return TRUE;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
