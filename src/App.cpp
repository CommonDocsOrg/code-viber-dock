#include "App.h"
#include "ClockFaces.h"

#include "InputDialog.h"

#include <dbt.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <string>
#include <vector>

#include "AppIdentity.h"
#include "ContextMenu.h"
#include "DesktopLayer.h"
#include "Hotkey.h"
#include "IconUtil.h"
#include "SettingsIds.h"
#include "WidgetPackage.h"
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
        config_.EnsureItemSettings();  // the default clock item's settings (from the defaults)
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
    // Every open settings window (index loops: a page may open a window from a notification).
    callbacks.weatherChanged = [this] {
        for (size_t i = 0; i < settings_.size(); ++i) settings_[i]->WeatherChanged();  // the Place fields' status lines
    };
    callbacks.notificationsChanged = [this] {
        for (size_t i = 0; i < settings_.size(); ++i) settings_[i]->NotificationsChanged();  // the Notifications page
    };
    callbacks.calendarChanged = [this] {
        for (size_t i = 0; i < settings_.size(); ++i) settings_[i]->CalendarChanged();  // the Calendar feeds page
    };
    // Folder windows (phase 12): moved / resized / collapsed / a menu setting -> saved; their own
    // menu's Remove (posted: the window is in one of its own messages) and New folder window….
    callbacks.folderChanged = [this] {
        RequestSave();
        RefreshSettings();
    };
    callbacks.folderRemove = [this](size_t cfg) {
        if (hwnd_) PostMessageW(hwnd_, kRemoveFolderMessage, cfg, 0);
    };
    callbacks.folderNew = [this] {
        if (hwnd_) PostMessageW(hwnd_, msgDockCommand_, ContextMenu::kNewFolderWindow, 0);
    };
    // Standalone widgets (round 6 phase 6B): moved / resized -> saved; their menu; weather.openSettings().
    callbacks.widgetChanged = [this] {
        RequestSave();
        RefreshSettings();
    };
    callbacks.widgetMenu = [this](size_t cfg, POINT pt) { ShowWidgetMenu(cfg, pt); };
    callbacks.widgetSettings = [this](size_t cfg) { OpenItemSettings(cfg, 0); };
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

    // A previous run died while it owned the notify icons: when the takeover starts again (posted
    // by Sync) its own broadcast asks the apps to register; otherwise ask them now, for Explorer.
    if (!docks_.SyncTray()) TrayHost::RecoverFromMarker();
    AddTrayIcon();
    taskbar_.SetEnabled(config_.hideTaskbar);
    // Only now: the controller has saved the original work area (without our reservations).
    docks_.AllowAppBar(true);
    UpdateReservedMonitors();
    ApplyHotkey();  // the dashboard hotkey (round 6 phase 6A)
    return true;
}

void App::ApplyHotkey() {
    if (!hwnd_ || shutDown_) return;
    const std::wstring& text = config_.dashboard.hotkey;
    if (hotkeyTried_ && text == hotkeyText_ && (hotkeyRegistered_ || text.empty())) return;  // unchanged
    RemoveHotkey();
    hotkeyTried_ = true;
    hotkeyText_ = text;
    Hotkey::Combo combo;
    if (text.empty() || !Hotkey::Parse(text, combo)) return;  // none
    hotkeyRegistered_ = RegisterHotKey(hwnd_, kDashboardHotkeyId, combo.mods | MOD_NOREPEAT, combo.vk) != FALSE;
    if (!hotkeyRegistered_) {
        wchar_t buf[160];
        swprintf_s(buf, L"WinDock: the dashboard hotkey %s could not be registered (error %lu; another app uses it)\n",
                   text.c_str(), GetLastError());
        OutputDebugStringW(buf);
    }
}

void App::RemoveHotkey() {
    if (hotkeyRegistered_ && hwnd_) UnregisterHotKey(hwnd_, kDashboardHotkeyId);
    hotkeyRegistered_ = false;
    hotkeyTried_ = false;
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
    requests.group = [this, d](const AppKey& source, const AppKey& target) { GroupWith(*d, source, target); };
    requests.addToGroup = [this, d](const AppKey& group, const std::vector<std::wstring>& paths) {
        AddToGroup(*d, group, paths);
    };
    requests.memberOut = [this, d](const AppKey& group, size_t member, size_t at) { MemberOut(*d, group, member, at); };
    requests.removeMember = [this, d](const AppKey& group, size_t member) { RemoveMember(*d, group, member); };
    requests.trayOrder = [this, d](size_t item, const std::wstring& key, const std::wstring& before) {
        TrayOrder(d->ConfigIndex(), item, key, before);
    };
    requests.pinTray = [this, d](const std::wstring& key, size_t at) { PinTray(d->ConfigIndex(), key, at); };
    requests.unpinTray = [this, d](const std::wstring& key) { UnpinTray(d->ConfigIndex(), key); };
    requests.trayHide = [this, d](size_t item, const std::wstring& key, bool hide) { TrayHide(d->ConfigIndex(), item, key, hide); };
    requests.traySettings = [this, d](size_t item) {
        // The tray item's own window (the menu's item, else the dock's first tray item); without
        // one: Global > Tray icons.
        const size_t cfg = d->ConfigIndex();
        const auto& items = config_.docks[cfg].items;
        if (item >= items.size() || items[item].Kind() != ItemKind::Tray) {
            const auto it = std::find_if(items.begin(), items.end(), [](const DockItemEntry& e) { return e.Kind() == ItemKind::Tray; });
            item = it == items.end() ? SIZE_MAX : static_cast<size_t>(it - items.begin());
        }
        if (item == SIZE_MAX) {
            OpenSettings(SettingsIds::kPageTray);
        } else {
            OpenItemSettings(cfg, item);
        }
    };
    requests.moveCircle = [this, d](int x, int y) {
        const size_t cfg = d->ConfigIndex();
        DockConfig& c = config_.docks[cfg];
        c.x = x;
        c.y = y;
        docks_.ForConfig(cfg, [](DockWindow& w) { w.ApplyConfig(); });
        RequestSave();
        RefreshSettings();
    };
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
    weather.openSettings = [this, d](size_t item) { OpenItemSettings(d->ConfigIndex(), item); };
    dock.SetWeatherCallbacks(std::move(weather));
    if (KeepShownFor(dock.ConfigIndex())) dock.SetKeepShown(true);  // its dock / item window is active
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
    config_.EnsureItemSettings();  // a new clock / weather item starts from the defaults (round 5)
    docks_.ForConfig(cfg, [](DockWindow& w) { w.ReloadPins(); });
    docks_.ApplyWidgetWindow(cfg);  // a standalone widget's settings (its Item window)
    docks_.SyncTray();  // a tray item came or went
    docks_.SyncNotifications();  // a notifications item came or went
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
    RefreshSettings();  // the Contents tab's strip
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
    RefreshSettings();  // the Contents tab's strip
    return true;
}

void App::SetItemView(DockWindow& dock, const AppKey& key, ItemView view) {
    const auto& pins = dock.Pins();  // parallel to the pin items of the dock's config
    DockConfig& dc = config_.docks[dock.ConfigIndex()];
    const std::vector<size_t> idx = dc.PinIndexes();
    bool changed = false;
    for (size_t i = 0; i < pins.size() && i < idx.size(); ++i) {
        if (pins[i].key != key ||
            (pins[i].kind != AppIdentity::PinKind::Folder && pins[i].kind != AppIdentity::PinKind::Group)) {
            continue;
        }
        DockItemEntry& e = dc.items[idx[i]];
        if (e.view == view) continue;
        e.view = view;
        changed = true;
    }
    if (!changed) return;
    ReloadPins(dock.ConfigIndex());
    RequestSave();
    RefreshSettings();
}

void App::ItemsChanged(size_t cfg) {
    ReloadPins(cfg);
    RequestSave();
    RefreshSettings();  // the Contents tab's strip
}

void App::TrayOrder(size_t cfg, size_t item, const std::wstring& key, const std::wstring& before) {
    if (cfg >= config_.docks.size() || key.empty()) return;
    DockConfig& d = config_.docks[cfg];
    if (item >= d.items.size() || d.items[item].Kind() != ItemKind::Tray || d.locked) return;
    DockItemEntry& tray = d.items[item];
    std::erase(tray.hidden, key);  // dropped into the grid: shown there
    // The grid as it is (the icons running now), with the key moved; keys of icons that do not run
    // now keep their place after them.
    std::vector<std::wstring> keys;
    if (docks_.TrayService().Running()) {
        for (const TrayIcon* t : TrayIconsFor(docks_.TrayService(), d, tray, true)) keys.push_back(t->key);
    }
    std::erase(keys, key);
    auto at = before.empty() ? keys.end() : std::find(keys.begin(), keys.end(), before);
    keys.insert(at, key);
    for (const std::wstring& k : tray.order) {
        if (std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
    }
    tray.order = std::move(keys);
    ItemsChanged(cfg);
}

void App::PinTray(size_t cfg, const std::wstring& key, size_t at) {
    if (cfg >= config_.docks.size() || key.empty()) return;
    DockConfig& d = config_.docks[cfg];
    if (d.locked) return;
    const bool pinned = std::any_of(d.items.begin(), d.items.end(),
                                    [&](const DockItemEntry& e) { return e.Kind() == ItemKind::TrayApp && e.id == key; });
    if (pinned) return;
    DockItemEntry e;
    e.kind = ToString(ItemKind::TrayApp);
    e.id = key;
    d.InsertPin(at, std::move(e));  // where pin `at` would go (a trayApp is not a pin itself)
    ItemsChanged(cfg);
}

void App::UnpinTray(size_t cfg, const std::wstring& key) {
    if (cfg >= config_.docks.size()) return;
    DockConfig& d = config_.docks[cfg];
    if (d.locked) return;
    if (std::erase_if(d.items, [&](const DockItemEntry& e) { return e.Kind() == ItemKind::TrayApp && e.id == key; }) == 0) return;
    ItemsChanged(cfg);
}

void App::TrayHide(size_t cfg, size_t item, const std::wstring& key, bool hide) {
    if (cfg >= config_.docks.size() || key.empty()) return;
    DockConfig& d = config_.docks[cfg];
    if (item >= d.items.size() || d.items[item].Kind() != ItemKind::Tray || d.locked) return;
    std::vector<std::wstring>& hidden = d.items[item].hidden;
    const bool is = std::find(hidden.begin(), hidden.end(), key) != hidden.end();
    if (is == hide) return;
    if (hide) {
        hidden.push_back(key);
    } else {
        std::erase(hidden, key);
    }
    ItemsChanged(cfg);
}

DockItemEntry* App::PinEntryOf(DockWindow& dock, const AppKey& key, size_t* pinIndex) {
    const auto& pins = dock.Pins();  // parallel to the pin items of the dock's config
    DockConfig& dc = config_.docks[dock.ConfigIndex()];
    const std::vector<size_t> idx = dc.PinIndexes();
    for (size_t i = 0; i < pins.size() && i < idx.size(); ++i) {
        if (pins[i].key != key) continue;
        if (pinIndex) *pinIndex = i;
        return &dc.items[idx[i]];
    }
    return nullptr;
}

namespace {
// A member entry: the target and its custom icon (no view, no members).
DockItemEntry MemberEntry(const DockItemEntry& e) {
    DockItemEntry m;
    m.kind = e.Kind() == ItemKind::Folder ? ToString(ItemKind::Folder) : ToString(ItemKind::App);
    m.path = e.path;
    m.aumid = e.aumid;
    m.icon = e.icon;
    return m;
}
}  // namespace

void App::GroupWith(DockWindow& dock, const AppKey& source, const AppKey& target) {
    if (source == target) return;
    const size_t cfg = dock.ConfigIndex();
    DockConfig& dc = config_.docks[cfg];
    if (dc.locked) return;
    size_t ti = 0, si = SIZE_MAX;
    DockItemEntry* t = PinEntryOf(dock, target, &ti);
    if (!t || (t->Kind() != ItemKind::App && t->Kind() != ItemKind::Folder && t->Kind() != ItemKind::Group)) return;
    // The source: its pin entry (it leaves the pins), or a running app's entry.
    std::optional<DockItemEntry> s;
    if (DockItemEntry* pinned = PinEntryOf(dock, source, &si)) {
        if (pinned->Kind() != ItemKind::App && pinned->Kind() != ItemKind::Folder) return;  // not a group / separator / bin
        s = MemberEntry(*pinned);
    } else {
        const int index = IndexOfKey(dock, source);
        if (index < 0) return;
        s = EntryForItem(dock.Items()[static_cast<size_t>(index)]);
        if (!s) return;
        s = MemberEntry(*s);
    }
    if (t->Kind() == ItemKind::Group) {
        // Already a member: only the pin goes (it is in the group anyway).
        const AppIdentity::ResolvedPin* g = dock.GroupPin(target);
        const bool member = g && std::any_of(g->members.begin(), g->members.end(),
                                             [&](const AppIdentity::ResolvedPin& m) { return m.key == source; });
        if (!member) t->members.push_back(std::move(*s));
    } else {
        DockItemEntry group;
        group.kind = ToString(ItemKind::Group);
        group.members.push_back(MemberEntry(*t));
        group.members.push_back(std::move(*s));
        *t = std::move(group);  // at the target's place
    }
    if (si != SIZE_MAX) dc.ErasePin(si);
    ItemsChanged(cfg);
}

void App::AddToGroup(DockWindow& dock, const AppKey& group, const std::vector<std::wstring>& paths) {
    DockItemEntry* g = PinEntryOf(dock, group);
    if (!g || g->Kind() != ItemKind::Group || config_.docks[dock.ConfigIndex()].locked) return;
    std::vector<AppKey> keys;
    if (const AppIdentity::ResolvedPin* rp = dock.GroupPin(group)) {
        for (const AppIdentity::ResolvedPin& m : rp->members) keys.push_back(m.key);
    }
    bool changed = false;
    for (const std::wstring& path : paths) {
        DockItemEntry entry;
        entry.path = path;
        const AppIdentity::ResolvedPin pin = AppIdentity::ResolvePin(entry);
        if (pin.kind != AppIdentity::PinKind::App && pin.kind != AppIdentity::PinKind::Folder) continue;
        if (std::find(keys.begin(), keys.end(), pin.key) != keys.end()) continue;  // already a member
        keys.push_back(pin.key);
        DockItemEntry m;
        m.kind = pin.kind == AppIdentity::PinKind::Folder ? ToString(ItemKind::Folder) : ToString(ItemKind::App);
        m.path = pin.launchPath.empty() ? path : pin.launchPath;
        g->members.push_back(std::move(m));
        changed = true;
    }
    if (changed) ItemsChanged(dock.ConfigIndex());
}

void App::MemberOut(DockWindow& dock, const AppKey& group, size_t member, size_t at) {
    size_t gi = 0;
    DockItemEntry* g = PinEntryOf(dock, group, &gi);
    if (!g || g->Kind() != ItemKind::Group || member >= g->members.size()) return;
    DockConfig& dc = config_.docks[dock.ConfigIndex()];
    DockItemEntry m = std::move(g->members[member]);
    g->members.erase(g->members.begin() + static_cast<ptrdiff_t>(member));
    if (at == SIZE_MAX) at = gi + 1;
    if (g->members.empty()) {
        dc.ErasePin(gi);  // the last member out: the group goes
        if (at > gi) --at;
    }
    dc.InsertPin(at, std::move(m));
    ItemsChanged(dock.ConfigIndex());
}

void App::RemoveMember(DockWindow& dock, const AppKey& group, size_t member) {
    size_t gi = 0;
    DockItemEntry* g = PinEntryOf(dock, group, &gi);
    if (!g || g->Kind() != ItemKind::Group || member >= g->members.size()) return;
    g->members.erase(g->members.begin() + static_cast<ptrdiff_t>(member));
    if (g->members.empty()) config_.docks[dock.ConfigIndex()].ErasePin(gi);
    ItemsChanged(dock.ConfigIndex());
}

void App::Ungroup(DockWindow& dock, const AppKey& group) {
    size_t gi = 0;
    DockItemEntry* g = PinEntryOf(dock, group, &gi);
    if (!g || g->Kind() != ItemKind::Group) return;
    DockConfig& dc = config_.docks[dock.ConfigIndex()];
    std::vector<DockItemEntry> members = std::move(g->members);
    dc.ErasePin(gi);
    for (size_t k = 0; k < members.size(); ++k) dc.InsertPin(gi + k, std::move(members[k]));
    ItemsChanged(dock.ConfigIndex());
}

void App::RenameGroup(DockWindow& dock, const AppKey& group) {
    const DockItemEntry* g = PinEntryOf(dock, group);
    if (!g || g->Kind() != ItemKind::Group) return;
    std::wstring name = g->name.empty() ? std::wstring(L"Group") : g->name;
    const HWND owner = dock.Hwnd();
    dock.SetHold(true);
    ++modalDepth_;
    g_activateTarget = hwnd_;  // the dialog gets the keyboard focus (see ChooseItemIcon)
    g_activateMessage = kDialogActivated;
    HHOOK hook = SetWindowsHookExW(WH_CBT, &ActivateHook, nullptr, GetCurrentThreadId());
    const bool ok = InputDialog::Ask(owner, L"Rename group", L"Name:", name);
    if (hook) UnhookWindowsHookEx(hook);
    g_activateTarget = nullptr;
    DockWindow* again = shutDown_ ? nullptr : docks_.ByHwnd(owner);
    if (again) again->SetHold(false);
    EndModal();
    if (!ok || !again) return;
    // The items may have changed while the dialog was open: look the group up again.
    DockItemEntry* now = PinEntryOf(*again, group);
    if (!now || now->Kind() != ItemKind::Group) return;
    const std::wstring stored = name == L"Group" ? std::wstring() : name;
    if (now->name == stored) return;
    now->name = stored;
    ItemsChanged(again->ConfigIndex());
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
    RefreshSettings();  // the Contents tab's strip
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
        case ContextMenu::kViewAuto:
        case ContextMenu::kViewGrid:
        case ContextMenu::kViewList:
            SetItemView(dock, key, static_cast<ItemView>(cmd - ContextMenu::kViewAuto));
            return;
        case ContextMenu::kRenameGroup:
            RenameGroup(dock, key);
            return;
        case ContextMenu::kUngroup:
            Ungroup(dock, key);
            return;
        case ContextMenu::kReloadWidget:
            dock.ReloadWidget(IndexOfKey(dock, key));
            return;
        case ContextMenu::kOpenItemSettings: {
            // The item's own settings window (the slot's config item).
            const int index = IndexOfKey(dock, key);
            if (index >= 0) OpenItemSettings(dock.ConfigIndex(), dock.Items()[static_cast<size_t>(index)].element);
            return;
        }
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
    // First: closing them may commit a pending edit (focus loss), which must still be saved.
    {
        std::vector<std::unique_ptr<SettingsWindow>> windows = std::move(settings_);
        settings_.clear();
        windows.clear();
    }
    activeSettings_ = nullptr;
    console_.reset();  // before the docks: it listens to the widget runtime
    geocodeDone_ = nullptr;
    geocodeOwner_ = nullptr;
    RemoveHotkey();
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
    state.hasTray = d.Has(ItemKind::Tray);
    state.hasNotifications = d.Has(ItemKind::Notifications);
    state.weatherConfigured = std::any_of(d.items.begin(), d.items.end(), [&](const DockItemEntry& e) {
        return e.Kind() == ItemKind::Weather && WeatherOf(config_, e).Configured();
    });
    state.locked = d.locked;
    state.hasRunning = d.Has(ItemKind::Running);
    state.edge = d.edge;
    state.dockCount = static_cast<int>(config_.docks.size());
    state.dashboard = docks_.Dashboard();
    for (const DockConfig& o : config_.docks) {
        (o.locked ? state.anyLocked : state.anyUnlocked) = true;
        if (o.Has(ItemKind::Widget)) state.hasWidgets = true;
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

void App::ShowMenu(DockWindow* dock, POINT pt, int slot) {
    if (!dock) dock = docks_.First();
    const size_t cfg = dock ? dock->ConfigIndex() : 0;
    const HWND owner = dock ? dock->Hwnd() : hwnd_;
    ContextMenu::TrayMenuState state = MenuState(cfg);
    // Over a widget / clock / weather slot: "<its name> settings…" first (its Item window).
    size_t item = SIZE_MAX;
    DockItemEntry entry;
    if (dock && slot >= 0 && slot < static_cast<int>(dock->Items().size()) && cfg < config_.docks.size()) {
        const size_t element = dock->Items()[static_cast<size_t>(slot)].element;
        const auto& items = config_.docks[cfg].items;
        if (element < items.size()) {
            const ItemKind k = items[element].Kind();
            if (k == ItemKind::Clock || k == ItemKind::Weather || k == ItemKind::Widget || k == ItemKind::Tray) {
                item = element;
                entry = items[element];
                state.itemSettings = k == ItemKind::Clock     ? std::wstring(L"Clock settings…")
                                     : k == ItemKind::Weather ? std::wstring(L"Weather settings…")
                                     : k == ItemKind::Tray    ? std::wstring(L"Tray settings…")
                                                              : Widgets::DisplayName(entry.id) + L" settings…";
            }
        }
    }
    const UINT cmd = RunMenu(dock, [&] { return ContextMenu::ShowTrayMenu(owner, pt, state); });
    if (cmd == ContextMenu::kNone || shutDown_) return;
    if (cmd == ContextMenu::kOpenItemSettings) {
        // The items may have changed while the menu was open: only the same entry.
        if (cfg < config_.docks.size() && item < config_.docks[cfg].items.size() && config_.docks[cfg].items[item] == entry) {
            OpenItemSettings(cfg, item);
        } else {
            MessageBeep(MB_ICONWARNING);
        }
        return;
    }
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
            if (pins[i].key != key) continue;
            if (!dc.items[idx[i]].icon.empty()) itemState.hasIcon = true;
            itemState.view = dc.items[idx[i]].view;
            itemState.members = dc.items[idx[i]].members.size();
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
    if (!d.IsDock()) {
        // A folder window or a standalone widget (round 6 phase 6B) has no items, edge or auto-hide
        // of a dock: the commands that edit those do nothing for it.
        switch (cmd) {
            case ContextMenu::kAutoHide:
            case ContextMenu::kAutoHideOff:
            case ContextMenu::kAutoHideAlways:
            case ContextMenu::kAutoHideIntelligent:
            case ContextMenu::kAddSeparator:
            case ContextMenu::kWeatherSettings:
            case ContextMenu::kToggleCalendar:
            case ContextMenu::kToggleWeatherPopup:
            case ContextMenu::kAddClock:
            case ContextMenu::kAddWeather:
            case ContextMenu::kAddRunning:
            case ContextMenu::kEdgeBottom:
            case ContextMenu::kEdgeTop:
            case ContextMenu::kEdgeLeft:
            case ContextMenu::kEdgeRight:
            case ContextMenu::kAddTray:
            case ContextMenu::kToggleBadges:
            case ContextMenu::kToggleCentre:
            case ContextMenu::kAddNotifications:
                return;
            default:
                break;
        }
    }
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
            OpenSettings();  // the Global window
            break;
        case ContextMenu::kDockSettings:
            OpenDockSettings(cfg);
            break;
        case ContextMenu::kFlashDock:
            FlashDock(cfg);
            break;
        case ContextMenu::kToggleDashboard:
            docks_.SetDashboard(!docks_.Dashboard());
            break;
        case ContextMenu::kEnterDashboard:
            docks_.SetDashboard(true);
            break;
        case ContextMenu::kLeaveDashboard:
            docks_.SetDashboard(false);
            break;
        case ContextMenu::kRefreshWeather:
            docks_.RefreshWeather(nullptr);  // every place
            break;
        case ContextMenu::kWeatherSettings: {
            // The dock's first weather item's settings window (none: the dock's Contents tab).
            const auto it = std::find_if(d.items.begin(), d.items.end(), [](const DockItemEntry& e) { return e.Kind() == ItemKind::Weather; });
            if (it != d.items.end()) {
                OpenItemSettings(cfg, static_cast<size_t>(it - d.items.begin()));
            } else {
                OpenDockSettings(cfg, SettingsIds::kTabContents);
            }
            break;
        }
        case ContextMenu::kToggleCalendar:
            if (dock) dock->TogglePopup(ItemKind::Clock);
            break;
        case ContextMenu::kToggleWeatherPopup:
            if (dock) dock->TogglePopup(ItemKind::Weather);
            break;
        case ContextMenu::kLockDock:
            d.locked = !d.locked;  // read by the dock when a drag / drop starts
            docks_.ApplyWidgetWindow(cfg);  // a standalone widget: dock.locked
            RequestSave();
            RefreshSettings();
            break;
        case ContextMenu::kLockAllDocks:
        case ContextMenu::kUnlockAllDocks:
            for (DockConfig& o : config_.docks) o.locked = cmd == ContextMenu::kLockAllDocks;
            RequestSave();
            RefreshSettings();
            break;
        case ContextMenu::kAddTray:
            // One tray per dock; adding it is asking for the notify icons: the takeover goes on.
            if (d.locked || d.Has(ItemKind::Tray)) break;
            config_.tray.enabled = true;
            AddWidget(cfg, ItemKind::Tray);  // reloads (SyncTray starts the host) and saves
            break;
        case ContextMenu::kTrayEnable:
        case ContextMenu::kTrayDisable:
            config_.tray.enabled = cmd == ContextMenu::kTrayEnable;
            docks_.SyncTray();
            RequestSave();
            RefreshSettings();
            break;
        case ContextMenu::kPollNotifications:
            docks_.Notifications().PollNow();
            break;
        case ContextMenu::kRefreshCalendars:
            docks_.Calendar().RefreshNow();
            break;
        case ContextMenu::kSortDesktopNow:
            if (docks_.SortDesktop() == 0) MessageBeep(MB_OK);  // nothing matched (or a sort is running)
            break;
        case ContextMenu::kNewFolderWindow: {
            // The picker is owned by the settings window it was asked from (the foreground one).
            HWND owner = dock ? dock->Hwnd() : hwnd_;
            for (const auto& w : settings_) {
                if (w->Hwnd() && GetForegroundWindow() == w->Hwnd()) owner = w->Hwnd();
            }
            NewFolderWindow(owner);
            break;
        }
        case ContextMenu::kToggleCentre:
            if (dock) dock->TogglePopup(ItemKind::Notifications);
            break;
        case ContextMenu::kAddNotifications:
            if (d.locked || d.Has(ItemKind::Notifications)) break;
            AddWidget(cfg, ItemKind::Notifications);  // reloads (the service starts with the element) and saves
            break;
        case ContextMenu::kToggleBadges:
            d.badges = !d.badges;
            docks_.SyncNotifications();  // starts / stops the service, redraws the badges
            RequestSave();
            RefreshSettings();
            break;
        case ContextMenu::kAddClock:
        case ContextMenu::kAddWeather:
        case ContextMenu::kAddRunning:
            if (d.locked) break;  // greyed in the menu; the dock window's Contents tab still adds
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
        case ContextMenu::kReloadWidgets:
            docks_.Widgets().ReloadAll();
            break;
        case ContextMenu::kWidgetConsole:
            OpenConsole();
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
    if (config_.docks[cfg].Floating()) mode = AutoHideMode::Off;  // its layer and the dashboard decide (6A)
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
    config_.EnsureItemSettings();  // a new dock's clock starts from the defaults
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
    ApplyHotkey();  // a new dashboard hotkey (or a retry of one another app held)
    RefreshSettings();
}

void App::RefreshSettings(SettingsWindow* except) {
    SyncSettingsTargets();
    // Index loop: nothing below adds or frees windows (closed ones are freed by kSettingsClosed).
    for (size_t i = 0; i < settings_.size(); ++i) {
        SettingsWindow* w = settings_[i].get();
        if (w == except || !w->Hwnd()) continue;
        w->Refresh();
    }
}

bool App::PreTranslateMessage(MSG& msg) {
    // A window may open another one from inside its own message: the index loop and raw pointer
    // stay valid (the objects themselves never move).
    for (size_t i = 0; i < settings_.size(); ++i) {
        if (SettingsWindow* w = settings_[i].get(); w->Hwnd() && w->PreTranslateMessage(msg)) return true;
    }
    return false;
}

size_t App::DockIndexOf(const std::wstring& id) const {
    for (size_t i = 0; i < config_.docks.size(); ++i) {
        if (config_.docks[i].id == id) return i;
    }
    return SIZE_MAX;
}

SettingsWindow* App::FindSettings(SettingsKind kind, const std::wstring& dock, size_t item) const {
    for (const auto& w : settings_) {
        if (!w->Hwnd() || w->Kind() != kind) continue;
        if (kind == SettingsKind::Global) return w.get();
        if (w->DockId() != dock) continue;
        if (kind == SettingsKind::Dock || w->ItemIndex() == item) return w.get();
    }
    return nullptr;
}

int App::CountSettings(SettingsKind kind) const {
    int n = 0;
    for (const auto& w : settings_) {
        if (w->Hwnd() && w->Kind() == kind) ++n;
    }
    return n;
}

bool App::KeepShownFor(size_t cfg) const {
    // The active settings window is a Dock / Item window of this dock (the Global one keeps nothing shown).
    return activeSettings_ && activeSettings_->Hwnd() && activeSettings_->Kind() != SettingsKind::Global &&
           activeSettings_->DockIndex() == cfg;
}

void App::UpdateKeepShown() {
    if (shutDown_) return;
    docks_.ForEach([&](DockWindow& w) { w.SetKeepShown(KeepShownFor(w.ConfigIndex())); });
}

void App::OnSettingsActivated(SettingsWindow* w, bool active) {
    if (active) {
        activeSettings_ = w;
    } else if (activeSettings_ == w) {
        activeSettings_ = nullptr;
    }
    UpdateKeepShown();
}

void App::FlashDock(size_t cfg) {
    if (cfg >= config_.docks.size()) return;
    const uint32_t color = config_.DockColor(cfg);
    bool any = false;
    docks_.ForConfig(cfg, [&](DockWindow& w) {
        w.Flash(color);
        any = true;
    });
    if (WidgetWindow* w = docks_.WidgetOf(cfg)) {
        w->Flash(color);  // a standalone widget's border (round 6 phase 6B)
        any = true;
    }
    if (!any) MessageBeep(MB_OK);  // parked, or a folder window
}

bool App::HasItemWindow(size_t cfg, size_t item) {
    if (cfg >= config_.docks.size() || item >= config_.docks[cfg].items.size()) return false;
    const DockItemEntry& e = config_.docks[cfg].items[item];
    switch (e.Kind()) {
        case ItemKind::Widget:
        case ItemKind::Clock:
        case ItemKind::Weather:
        case ItemKind::Folder:
        case ItemKind::Group:
        case ItemKind::Tray:  // round 5 phase D: its grid and icons
            return true;
        case ItemKind::App: {
            // A shortcut to a folder is a folder item too (its popup's layout).
            DockWindow* d = docks_.FirstOf(cfg);
            if (!d) return false;
            const std::vector<size_t> idx = config_.docks[cfg].PinIndexes();
            const auto& pins = d->Pins();
            for (size_t k = 0; k < idx.size() && k < pins.size(); ++k) {
                if (idx[k] == item) return pins[k].kind == AppIdentity::PinKind::Folder;
            }
            return false;
        }
        default:
            return false;
    }
}

namespace {
// The same item as far as re-finding it goes: kind and what it points at (not its settings, icon,
// view or name, which an edit in place may change).
bool SameIdentity(const DockItemEntry& a, const DockItemEntry& b) {
    return a.kind == b.kind && a.path == b.path && a.aumid == b.aumid && a.id == b.id;
}
}  // namespace

void App::SyncSettingsTargets() {
    for (size_t i = 0; i < settings_.size(); ++i) {
        SettingsWindow* w = settings_[i].get();
        if (!w->Hwnd()) continue;
        const size_t d = DockIndexOf(w->DockId());
        if (w->Kind() == SettingsKind::Global) {
            if (d == SIZE_MAX && !config_.docks.empty()) w->SelectDock(config_.docks.front().id);
            continue;
        }
        if (d == SIZE_MAX) {
            DestroyWindow(w->Hwnd());  // its dock was removed (freed by kSettingsClosed)
            continue;
        }
        if (w->Kind() != SettingsKind::Item) continue;
        const auto& items = config_.docks[d].items;
        const size_t at = w->ItemIndex();
        const DockItemEntry& copy = w->ItemCopy();
        size_t found = SIZE_MAX;
        if (at < items.size() && items[at] == copy) {
            found = at;
        } else {
            // The equal entry elsewhere (moved): the nearest one.
            size_t best = SIZE_MAX;
            for (size_t k = 0; k < items.size(); ++k) {
                if (!(items[k] == copy)) continue;
                const size_t dist = k > at ? k - at : at - k;
                if (best == SIZE_MAX || dist < best) {
                    best = dist;
                    found = k;
                }
            }
            // Edited in place (by another window or the dock itself): nothing came or went.
            if (found == SIZE_MAX && items.size() == w->ItemCountCopy() && at < items.size() && SameIdentity(items[at], copy)) {
                found = at;
            }
        }
        if (found == SIZE_MAX) {
            DestroyWindow(w->Hwnd());  // its item was removed
            continue;
        }
        w->SetItemCopy(items[found], items.size());
        if (found != at) w->Retarget(found);
    }
}

SettingsHost App::MakeSettingsHost(SettingsWindow* w) {
    static const std::vector<AppIdentity::ResolvedPin> kNoPins;
    SettingsHost host;
    host.config = &config_;
    // "The dock": the first window of the window's dock (by id, every time).
    auto dockOf = [this, w]() -> size_t { return w->DockIndex(); };
    host.changed = [this, w](SettingsScope scope, size_t dock) { OnSettingsChanged(scope, dock, w); };
    host.pins = [this, dockOf]() -> const std::vector<AppIdentity::ResolvedPin>& {
        DockWindow* d = dockOf() != SIZE_MAX ? docks_.FirstOf(dockOf()) : nullptr;
        return d ? d->Pins() : kNoPins;
    };
    host.pinFiles = [this, dockOf](const std::vector<std::wstring>& paths) {
        const size_t cfg = dockOf();
        if (cfg == SIZE_MAX) return;
        DockWindow* d = docks_.FirstOf(cfg);
        PinFiles(cfg, d ? d->Pins() : kNoPins, paths, SIZE_MAX);
    };
    host.command = [this, dockOf](UINT cmd) {
        const size_t cfg = dockOf() != SIZE_MAX ? dockOf() : 0;
        OnCommand(cmd, cfg, docks_.FirstOf(cfg));
    };
    host.usingWarp = [this] {
        DockWindow* d = docks_.First();
        return d && d->UsingWarp();
    };
    host.dockMonitor = [this, dockOf] {
        const size_t cfg = dockOf();
        if (DockWindow* d = cfg != SIZE_MAX ? docks_.FirstOf(cfg) : nullptr) return d->MonitorRect();
        if (FolderWindow* f = cfg != SIZE_MAX ? docks_.FolderOf(cfg) : nullptr) {
            const RECT r = f->ScreenRect();
            MONITORINFO mi{sizeof(mi)};
            GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTOPRIMARY), &mi);
            return mi.rcMonitor;
        }
        if (WidgetWindow* wi = cfg != SIZE_MAX ? docks_.WidgetOf(cfg) : nullptr) {
            const RECT r = wi->ScreenRect();
            MONITORINFO mi{sizeof(mi)};
            GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTOPRIMARY), &mi);
            return mi.rcMonitor;
        }
        if (DockWindow* d = docks_.First()) return d->MonitorRect();
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
        return mi.rcMonitor;
    };
    host.dockDpi = [this, dockOf] {
        DockWindow* d = dockOf() != SIZE_MAX ? docks_.FirstOf(dockOf()) : nullptr;
        if (!d) d = docks_.First();
        return d ? d->Dpi() : GetDpiForSystem();
    };
    host.activated = [this, w](bool active) { OnSettingsActivated(w, active); };
    host.dockBar = [this, dockOf] {
        if (WidgetWindow* wi = dockOf() != SIZE_MAX ? docks_.WidgetOf(dockOf()) : nullptr) return wi->ScreenRect();
        DockWindow* d = dockOf() != SIZE_MAX ? docks_.FirstOf(dockOf()) : nullptr;
        return d ? d->RestBarRect() : RECT{};
    };
    host.openDock = [this](size_t dock, int tab) { OpenDockSettings(dock, tab); };
    host.openItem = [this](size_t dock, size_t item) { OpenItemSettings(dock, item); };
    host.openGlobal = [this](int page) { OpenSettings(page); };
    host.flashDock = [this, dockOf] {
        if (dockOf() != SIZE_MAX) FlashDock(dockOf());
    };
    host.anchor = [this, w] {
        // An Item window: 40 DIPs from its dock's window, when that is open (and not minimized).
        RECT r{};
        if (SettingsWindow* d = FindSettings(SettingsKind::Dock, w->DockId()); d && !IsIconic(d->Hwnd())) {
            GetWindowRect(d->Hwnd(), &r);
        }
        return r;
    };
    host.taskbarHidden = [this] { return taskbar_.Enabled(); };
    host.autostartEnabled = [] { return AutostartEnabled(); };
    host.weatherStatus = [this](const WeatherConfig& place) -> std::wstring {
        if (!place.Configured()) return L"Not configured";
        const WeatherService* ws = docks_.FindWeather(place);
        return ws ? ws->StatusText() : std::wstring(L"Not shown on a dock");
    };
    host.refreshWeather = [this](const WeatherConfig& place) { docks_.RefreshWeather(&place); };
    host.geocode = [this, w](const std::wstring& query,
                             std::function<void(const std::vector<WeatherService::Place>&, const std::wstring&)> done) {
        // A newer search (of any window) replaces an older one (its result is dropped when it arrives).
        if (!WeatherService::Geocode(query, hwnd_, WeatherService::kGeocodeMessage, ++geocodeTag_)) return false;
        geocodeDone_ = std::move(done);
        geocodeOwner_ = w;
        return true;
    };
    host.destroyed = [this] {
        if (hwnd_) PostMessageW(hwnd_, kSettingsClosed, 0, 0);
    };
    host.editDocks = [this](SettingsDockEdit edit, size_t index) { return EditDocks(edit, index); };
    host.dockWindows = [this](size_t index) {
        int n = docks_.FolderOf(index) || docks_.WidgetOf(index) ? 1 : 0;
        docks_.ForConfig(index, [&](DockWindow&) { ++n; });
        return n;
    };
    host.dockRects = [this] {
        std::vector<SettingsDockRect> rects;
        for (const DockManager::Instance& i : docks_.Instances()) {
            SettingsDockRect r;
            r.cfg = i.cfg;
            r.monitor = i.Live() ? i.monitorIndex : -1;
            r.folderWindow = i.isFolder;
            r.widgetWindow = i.isWidget;
            if (i.window) r.bar = i.window->RestBarRect();
            if (i.folder) r.bar = i.folder->ScreenRect();
            if (i.widget) r.bar = i.widget->ScreenRect();
            rects.push_back(r);
        }
        return rects;
    };
    host.trayIcons = [this] {
        std::vector<SettingsTrayIcon> icons;
        const TrayHost& tray = docks_.TrayService();
        if (!tray.Running()) return icons;
        for (const TrayIcon& t : tray.Icons()) {
            if (!t.Shown()) continue;
            const size_t slash = t.exe.find_last_of(L"\\/");
            icons.push_back(SettingsTrayIcon{t.key, t.Title(), slash == std::wstring::npos ? t.exe : t.exe.substr(slash + 1)});
        }
        return icons;
    };
    host.notificationStatus = [this] {
        const NotificationService& n = docks_.Notifications();
        SettingsNotificationStatus st;
        st.state = static_cast<int>(n.GetState());
        st.reason = n.StateReason();
        st.listed = n.Items().size();
        st.unread = n.Unread();
        return st;
    };
    host.hotkeyRegistered = [this] { return hotkeyRegistered_; };
    host.addWidgetWindow = [this](const std::wstring& id, size_t nearDock, bool open) { return AddWidgetWindow(id, nearDock, open); };
    host.dashboardOn = [this] { return docks_.Dashboard(); };
    host.calendarFeeds = [this] {
        std::vector<SettingsCalendarFeed> feeds;
        const CalendarService& c = docks_.Calendar();
        for (size_t i = 0; i < c.Feeds().size(); ++i) feeds.push_back(SettingsCalendarFeed{c.StatusText(i)});
        return feeds;
    };
    host.notificationApps = [this] {
        // Global > Notifications (round 5 phase D: every dock's apps, not one dock's).
        std::vector<SettingsNotificationApp> apps;
        auto add = [&](const AppKey& key, const std::wstring& name) {
            if (key.id.empty() || key.IsSynthetic()) return;
            if (std::any_of(apps.begin(), apps.end(), [&](const SettingsNotificationApp& a) { return a.key == key.id; })) return;
            apps.push_back(SettingsNotificationApp{key.id, name.empty() ? key.Source() : name,
                                                   docks_.Notifications().UnreadFor(key)});
        };
        for (size_t cfg = 0; cfg < config_.docks.size(); ++cfg) {
            DockWindow* d = docks_.FirstOf(cfg);
            if (!d) continue;
            for (const AppIdentity::ResolvedPin& p : d->Pins()) {
                if (p.kind == AppIdentity::PinKind::App) add(p.key, p.displayName);
                for (const AppIdentity::ResolvedPin& m : p.members) {
                    if (m.kind == AppIdentity::PinKind::App) add(m.key, m.displayName);
                }
            }
        }
        for (const NotificationItem& n : docks_.Notifications().Items()) add(n.key, n.app);
        return apps;
    };
    return host;
}

bool App::AddSettingsWindow(std::unique_ptr<SettingsWindow> w) {
    SettingsWindow* raw = w.get();
    raw->Host() = MakeSettingsHost(raw);
    if (!raw->Create(instance_)) {
        if (activeSettings_ == raw) activeSettings_ = nullptr;
        if (geocodeOwner_ == raw) {
            geocodeOwner_ = nullptr;
            geocodeDone_ = nullptr;
        }
        MessageBeep(MB_ICONWARNING);
        return false;
    }
    settings_.push_back(std::move(w));
    return true;
}

void App::OpenSettings(int page) {
    if (shutDown_) return;
    if (SettingsWindow* w = FindSettings(SettingsKind::Global)) {
        if (page >= 0) w->ShowPage(page);
        w->Activate();
        return;
    }
    // The Docks and displays page starts on the first dock.
    auto w = std::make_unique<SettingsWindow>(SettingsHost{}, SettingsKind::Global,
                                              config_.docks.empty() ? std::wstring() : config_.docks.front().id);
    SettingsWindow* raw = w.get();
    if (AddSettingsWindow(std::move(w)) && page >= 0) raw->ShowPage(page);
}

void App::OpenDockSettings(size_t dock, int tab) {
    if (shutDown_ || dock >= config_.docks.size()) return;
    const std::wstring id = config_.docks[dock].id;
    if (SettingsWindow* w = FindSettings(SettingsKind::Dock, id)) {
        if (tab >= 0) w->ShowPage(tab);
        w->Activate();
        return;
    }
    auto w = std::make_unique<SettingsWindow>(SettingsHost{}, SettingsKind::Dock, id);
    SettingsWindow* raw = w.get();
    if (AddSettingsWindow(std::move(w)) && tab >= 0) raw->ShowPage(tab);
}

void App::OpenItemSettings(size_t dock, size_t item) {
    if (shutDown_ || dock >= config_.docks.size() || item >= config_.docks[dock].items.size()) return;
    // Round 5 phase B: the item's own window (an item without settings: its dock's window).
    if (!HasItemWindow(dock, item)) {
        OpenDockSettings(dock, SettingsIds::kTabContents);
        return;
    }
    const std::wstring id = config_.docks[dock].id;
    if (SettingsWindow* w = FindSettings(SettingsKind::Item, id, item)) {
        w->Activate();
        return;
    }
    auto w = std::make_unique<SettingsWindow>(SettingsHost{}, SettingsKind::Item, id, item);
    w->SetItemCopy(config_.docks[dock].items[item], config_.docks[dock].items.size());
    AddSettingsWindow(std::move(w));
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
            break;
        }
        case SettingsDockEdit::AddFloating: {
            // Round 6 phase 6A: a floating bar on the desktop (shown while the desktop is, and by the
            // dashboard hotkey), centred on the primary display's work area, the default items.
            DockConfig d;
            d.id = config_.UniqueDockId(L"floating");
            d.monitor = DockConfig::kMonitorPrimary;
            d.floating = true;
            d.layer = DockLayer::Desktop;
            d.autoHide = AutoHideMode::Off;
            d.reserveSpace = false;
            list.push_back(std::move(d));
            break;
        }
        case SettingsDockEdit::Duplicate: {
            if (index >= list.size()) return false;
            DockConfig d = list[index];
            d.id = config_.UniqueDockId(d.id);
            d.color = 0;  // its own colour (the palette by index), not the copied dock's
            list.insert(list.begin() + static_cast<ptrdiff_t>(index) + 1, std::move(d));
            break;
        }
        case SettingsDockEdit::TrayDock: {
            if (index >= list.size()) return false;
            // Bottom right of the same display, the same theme, always shown, reserving nothing
            // (the dock beside it may reserve the edge already).
            DockConfig d;
            d.id = config_.UniqueDockId(L"tray");
            d.monitor = list[index].Each() ? DockConfig::kMonitorPrimary : list[index].monitor;
            d.edge = DockEdge::Bottom;
            d.align = DockAlign::End;
            d.autoHide = AutoHideMode::Off;
            d.reserveSpace = false;
            d.themePreset = list[index].themePreset;
            DockItemEntry tray{ToString(ItemKind::Tray)};
            auto& from = list[index].items;
            if (auto it = std::find_if(from.begin(), from.end(), [](const DockItemEntry& e) { return e.Kind() == ItemKind::Tray; });
                it != from.end()) {
                tray = *it;  // its rows, columns, hidden icons and order come along
                from.erase(it);
            }
            d.items = {std::move(tray)};
            config_.tray.enabled = true;
            list.push_back(std::move(d));
            SyncDocks();        // the new dock's window
            ReloadPins(index);  // the source dock without its tray (Sync does not reload items)
            RequestSave();
            RefreshSettings();  // the source dock's item windows (its tray moved), headers
            return true;
        }
        case SettingsDockEdit::Remove: {
            if (index >= list.size() || list.size() <= 1) return false;  // never the last dock
            const auto docks = std::count_if(list.begin(), list.end(), [](const DockConfig& d) { return d.IsDock(); });
            if (list[index].IsDock() && docks <= 1) return false;  // folder windows / standalone widgets need a dock beside them
            list.erase(list.begin() + static_cast<ptrdiff_t>(index));
            break;
        }
    }
    SyncDocks();
    RequestSave();
    RefreshSettings();  // a removed dock's windows close; every window's header (indexes and colours moved)
    return true;
}

void App::NewFolderWindow(HWND owner) {
    if (shutDown_ || modalDepth_ > 0) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    POINT at{};
    GetCursorPos(&at);  // where the menu was chosen
    std::wstring path;
    ++modalDepth_;  // display syncs wait: the owner must not go away under the dialog
    winrt::com_ptr<IFileOpenDialog> dialog;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())))) {
        FILEOPENDIALOGOPTIONS options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dialog->SetTitle(L"New folder window");
        dialog->SetOkButtonLabel(L"Show on the desktop");
        winrt::com_ptr<IShellItem> item;
        PWSTR name = nullptr;
        if (SUCCEEDED(dialog->Show(owner)) && SUCCEEDED(dialog->GetResult(item.put())) &&
            SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)) && name) {
            path = name;
        }
        if (name) CoTaskMemFree(name);
    }
    EndModal();
    if (!path.empty() && !shutDown_ && AddFolderWindow(path, at) == SIZE_MAX) MessageBeep(MB_ICONWARNING);
}

size_t App::AddFolderWindow(const std::wstring& path, POINT at) {
    if (shutDown_ || modalDepth_ > 0 || path.empty()) return SIZE_MAX;
    const DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return SIZE_MAX;
    // The monitor under the point, in DockManager's order; the window's top-left at the point,
    // kept on the work area.
    HMONITOR mon = MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST);
    const std::vector<HMONITOR>& monitors = docks_.Monitors();
    const auto it = std::find(monitors.begin(), monitors.end(), mon);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dpiX = 96, dpiY = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    const float s = static_cast<float>(dpiX) / 96.0f;
    DockConfig d;
    d.dockKind = DockKind::FolderWindow;
    d.items.clear();
    d.id = config_.UniqueDockId(L"folder");
    d.folder.path = path;
    d.monitor = it == monitors.end() ? DockConfig::kMonitorPrimary : static_cast<int>(it - monitors.begin());
    const RECT& w = mi.rcWork;
    const int workW = static_cast<int>(std::lround((w.right - w.left) / s)), workH = static_cast<int>(std::lround((w.bottom - w.top) / s));
    d.x = std::clamp(static_cast<int>(std::lround((at.x - w.left) / s)), 0, std::max(0, workW - d.folder.w));
    d.y = std::clamp(static_cast<int>(std::lround((at.y - w.top) / s)), 0, std::max(0, workH - d.folder.h));
    config_.docks.push_back(std::move(d));
    // The Global window's Docks and displays page selects the new folder window.
    if (SettingsWindow* g = FindSettings(SettingsKind::Global)) g->SelectDock(config_.docks.back().id);
    SyncDocks();
    RequestSave();
    RefreshSettings();
    for (size_t i = 0; i < docks_.Count(); ++i) {
        const DockManager::Instance* inst = docks_.At(i);
        if (inst && inst->cfg == config_.docks.size() - 1) return i;
    }
    return SIZE_MAX;
}

void App::RemoveDesktopEntry(size_t cfg) {
    if (cfg >= config_.docks.size() || config_.docks[cfg].IsDock()) return;
    if (!EditDocks(SettingsDockEdit::Remove, cfg)) MessageBeep(MB_ICONWARNING);  // EditDocks refreshes the settings windows
}

size_t App::AddWidgetWindow(const std::wstring& id, size_t nearDock, bool open) {
    if (shutDown_ || modalDepth_ > 0 || !Widgets::ValidId(id)) return SIZE_MAX;
    std::wstring error;
    const auto pkg = Widgets::Load(id, error);
    if (!pkg) return SIZE_MAX;
    DockConfig d;
    d.dockKind = DockKind::Widget;
    d.id = config_.UniqueDockId(id);
    DockItemEntry e;
    e.kind = ToString(ItemKind::Widget);
    e.id = id;
    d.items = {std::move(e)};
    pkg->manifest.DefaultStandaloneSize(d.widgetWindow.w, d.widgetWindow.h);
    d.layer = DockLayer::Behind;
    // The display of the dock it was added from ("each": the primary), centred on its work area.
    d.monitor = 0;
    if (nearDock < config_.docks.size() && !config_.docks[nearDock].Each()) {
        const int m = docks_.MonitorIndexFor(config_.docks[nearDock]);
        d.monitor = m >= 0 ? m : 0;
    }
    d.x = d.y = DockConfig::kCentred;
    config_.docks.push_back(std::move(d));
    const size_t cfg = config_.docks.size() - 1;
    // Placed now (centred on that work area) so the file says where it is.
    const std::vector<HMONITOR>& monitors = docks_.Monitors();
    HMONITOR mon = static_cast<size_t>(config_.docks[cfg].monitor) < monitors.size() ? monitors[static_cast<size_t>(config_.docks[cfg].monitor)]
                                                                                     : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    UINT dpiX = 96, dpiY = 96;
    if (GetMonitorInfoW(mon, &mi) && SUCCEEDED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
        const float s = static_cast<float>(dpiX) / 96.0f;
        const int workW = static_cast<int>(std::lround((mi.rcWork.right - mi.rcWork.left) / s));
        const int workH = static_cast<int>(std::lround((mi.rcWork.bottom - mi.rcWork.top) / s));
        DockConfig& n = config_.docks[cfg];
        n.x = std::max(0, (workW - n.widgetWindow.w) / 2);
        n.y = std::max(0, (workH - n.widgetWindow.h) / 2);
    }
    if (SettingsWindow* g = FindSettings(SettingsKind::Global)) g->SelectDock(config_.docks[cfg].id);
    SyncDocks();
    RequestSave();
    RefreshSettings();
    if (open) OpenDockSettings(cfg);
    return cfg;
}

void App::ShowWidgetMenu(size_t cfg, POINT pt) {
    if (shutDown_ || cfg >= config_.docks.size() || !config_.docks[cfg].IsWidgetWindow()) return;
    WidgetWindow* w = docks_.WidgetOf(cfg);
    if (!w || !w->Hwnd()) return;
    const HWND owner = w->Hwnd();
    const std::wstring id = config_.docks[cfg].id;
    const bool locked = config_.docks[cfg].locked;
    const DockItemEntry* item = config_.docks[cfg].WidgetItem();
    const std::wstring name = item ? Widgets::DisplayName(item->id) : std::wstring(L"Widget");
    // The automation ids double as the menu's (README "Standalone widgets").
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, ContextMenu::kOpenItemSettings, (name + L" settings…").c_str());
    AppendMenuW(menu, MF_STRING, ContextMenu::kDockSettings, L"Window settings…");
    AppendMenuW(menu, MF_STRING | (locked ? MF_CHECKED : 0), ContextMenu::kWidgetToggleLock, locked ? L"Unlock" : L"Lock");
    AppendMenuW(menu, MF_STRING, ContextMenu::kReloadWidget, L"Reload widget");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ContextMenu::kWidgetRemove, L"Remove from desktop");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ContextMenu::kOpenSettings, L"Global settings…");
    const UINT cmd = RunMenu(nullptr, [&] {
        SetForegroundWindow(owner);
        const UINT c = static_cast<UINT>(TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, owner, nullptr));
        PostMessageW(owner, WM_NULL, 0, 0);
        return c;
    });
    DestroyMenu(menu);
    if (cmd == 0 || shutDown_) return;
    // The entries may have changed while the menu was open: the same entry by id.
    const size_t now = DockIndexOf(id);
    if (now == SIZE_MAX || !config_.docks[now].IsWidgetWindow()) return;
    if (cmd == ContextMenu::kDockSettings) {
        OpenDockSettings(now);
    } else if (cmd == ContextMenu::kOpenSettings) {
        OpenSettings();
    } else {
        WidgetEntryCommand(cmd, now);
    }
}

LRESULT App::WidgetEntryCommand(UINT cmd, size_t cfg) {
    if (cfg >= config_.docks.size() || !config_.docks[cfg].IsWidgetWindow()) return -1;
    switch (cmd) {
        case ContextMenu::kReloadWidget: {
            WidgetWindow* w = docks_.WidgetOf(cfg);
            return w && w->ReloadWidget() ? 1 : 0;
        }
        case ContextMenu::kOpenItemSettings:
            OpenItemSettings(cfg, 0);
            return 1;
        case ContextMenu::kWidgetToggleLock:
            config_.docks[cfg].locked = !config_.docks[cfg].locked;
            docks_.ApplyWidgetWindow(cfg);
            RequestSave();
            RefreshSettings();
            return 1;
        case ContextMenu::kWidgetRemove:
            // Posted: the entry may be in one of its own window's messages.
            if (hwnd_) PostMessageW(hwnd_, kRemoveFolderMessage, cfg, 0);
            return 1;
        default:
            return -1;
    }
}

void App::OnSettingsChanged(SettingsScope scope, size_t cfg, SettingsWindow* source) {
    if (shutDown_) return;
    if (cfg >= config_.docks.size()) cfg = 0;
    // An Item window edited its own item: what it is now is what to find again.
    if (source && source->Kind() == SettingsKind::Item) {
        if (const DockItemEntry* e = source->TargetItem()) source->SetItemCopy(*e, source->Dock().items.size());
    }
    switch (scope) {
        case SettingsScope::Appearance:
            // Sizes, position, monitor, theme, clock: the theme may be per dock, the monitor
            // decides which windows exist.
            config_.ResolveDockThemes();
            if (modalDepth_ == 0) {
                SyncDocks();  // folder windows and standalone widgets apply their entries in Sync
            } else {
                for (size_t k = 0; k < config_.docks.size(); ++k) docks_.ApplyWidgetWindow(k);
            }
            docks_.ForEach([](DockWindow& w) { w.ApplyConfig(); });
            break;
        case SettingsScope::AutoHide:
            SetAutoHide(cfg, config_.docks[cfg].autoHide);  // keeps lastOnMode_ in step; saves
            break;
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
        case SettingsScope::Notifications:
            docks_.SyncNotifications();
            break;
        case SettingsScope::Calendars:
            docks_.ConfigureCalendar();
            docks_.ForEach([](DockWindow& w) { w.OnCalendarChanged(); });  // showDots
            break;
        case SettingsScope::Dashboard:
            ApplyHotkey();
            break;
        case SettingsScope::SaveOnly:
            docks_.ApplyWidgetWindow(cfg);  // a standalone widget's lock (its header button)
            break;
    }
    RequestSave();
    // The other windows follow (their pages, headers; a removed item's window closes); the source
    // window keeps its controls (a half-typed value stays) and updates its header.
    RefreshSettings(source);
    if (source) source->UpdateIdentity();
}

void App::OpenConsole() {
    if (console_ && console_->Hwnd()) {
        console_->Activate();
        return;
    }
    console_ = std::make_unique<WidgetConsole>(docks_.Widgets(), [this] { PostMessageW(hwnd_, kConsoleClosed, 0, 0); });
    if (!console_->Create(instance_)) {
        console_.reset();
        MessageBeep(MB_ICONWARNING);
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

LRESULT App::QueryState(WPARAM wp, LPARAM lp) {
    const size_t index = DockOfParam(lp);
    const LPARAM arg = ArgOfParam(lp);
    const DockManager::Instance* inst = docks_.At(index);
    switch (wp) {
        case 16:  // some settings window is open (any kind)
            return CountSettings(SettingsKind::Global) + CountSettings(SettingsKind::Dock) + CountSettings(SettingsKind::Item) > 0 ? 1 : 0;
        case 222:  // round 5 phase B: open Global windows (0 / 1)
            return CountSettings(SettingsKind::Global);
        case 223:  // open Dock windows (folder window entries' included)
            return CountSettings(SettingsKind::Dock);
        case 224:  // open Item windows
            return CountSettings(SettingsKind::Item);
        case 225: {  // the Dock window of dock HIWORD(lParam) is open: 1 (LOWORD(lParam) 1: its HWND), else 0
            SettingsWindow* w = inst && inst->cfg < config_.docks.size() ? FindSettings(SettingsKind::Dock, config_.docks[inst->cfg].id) : nullptr;
            if (!w) return 0;
            return arg == 1 ? reinterpret_cast<LRESULT>(w->Hwnd()) : 1;
        }
        case 229:    // round 5 phase C, the Dock window's Contents tab: a strip tile's centre (item LOWORD(lParam))
        case 230:    // a drawer row's centre (entry LOWORD(lParam))
        case 231:    // the strip's selected item
        case 232:    // the drawer is shown
        case 233:    // the strip's insertion marker (-1 none)
        case 252: {  // round 6 phase 6B: a drawer row's "Desktop" button centre (entry LOWORD); -2 for each: no Dock window of that dock
            SettingsWindow* w = inst && inst->cfg < config_.docks.size() ? FindSettings(SettingsKind::Dock, config_.docks[inst->cfg].id) : nullptr;
            LRESULT out = -2;
            if (w && !w->QueryPages(static_cast<int>(wp), arg, out)) out = -2;
            return out;
        }
        case 227: {  // the HWND of the Item window of item LOWORD(lParam) of dock HIWORD(lParam) (0: none open)
            SettingsWindow* w = inst && inst->cfg < config_.docks.size() && arg >= 0
                                    ? FindSettings(SettingsKind::Item, config_.docks[inst->cfg].id, static_cast<size_t>(arg))
                                    : nullptr;
            return w ? reinterpret_cast<LRESULT>(w->Hwnd()) : 0;
        }
        case 234:  // round 6 phase 6A: dashboard mode is on
            return docks_.Dashboard() ? 1 : 0;
        case 240:  // the dashboard hotkey is registered (RegisterHotKey succeeded)
            return hotkeyRegistered_ ? 1 : 0;
        case 241:  // round 6 phase 6B: the kind of docks[] entry behind index HIWORD(lParam): 0 dock, 1 folder window, 2 standalone widget
            return inst ? (inst->isWidget ? 2 : inst->isFolder ? 1 : 0) : -1;
        case 47:
            return static_cast<LRESULT>(docks_.Count());  // dock instances (parked ones included)
        case 48:
            return inst ? inst->monitorIndex : -2;  // the dock's monitor index (-1 parked, -2 no such dock)
        case 49:
            return inst ? (inst->Live() ? 0 : 1) : -1;  // parked (no window)
        case 50:
            return inst ? static_cast<LRESULT>(inst->cfg) : -1;  // its index in docks[]
        case 51:
            if (inst && inst->folder) return reinterpret_cast<LRESULT>(inst->folder->Hwnd());
            if (inst && inst->widget) return reinterpret_cast<LRESULT>(inst->widget->Hwnd());
            return inst && inst->window ? reinterpret_cast<LRESULT>(inst->window->Hwnd()) : 0;  // its window
        case 183:
            return static_cast<LRESULT>(docks_.FolderCount());  // folder windows (not parked)
        case 184:
            return inst ? (inst->isFolder ? 1 : 0) : -1;  // the kind of docks[] entry behind index HIWORD(lParam)
        case 207:
            return static_cast<LRESULT>(docks_.RulesMoved());  // files moved by Desktop rules this session
        case 208:
            return docks_.DesktopWatched() ? 1 : 0;  // the Desktop is watched (rules with autoSort)
        case 213:
            return static_cast<LRESULT>(docks_.Widgets().WeatherUsers());  // widgets using the weather provider
        case 214:  // forecast requests started (every place's service; = 221)
        case 221:
            return static_cast<LRESULT>(WeatherService::TotalFetches());
        case 220:
            return static_cast<LRESULT>(docks_.WeatherServiceCount());  // weather services alive (one per place)
        case 215:
            return static_cast<LRESULT>(docks_.Widgets().WeatherNotices());  // weather onChange deliveries
        case 216: {  // the face of dock HIWORD's first clock item (else the defaults): 0 "digital", 1 a face file, -1 unreadable
            ClockConfig clock = config_.clock;
            if (inst && inst->cfg < config_.docks.size()) {
                for (const DockItemEntry& e : config_.docks[inst->cfg].items) {
                    if (e.Kind() != ItemKind::Clock) continue;
                    clock = ClockOf(config_, e);
                    break;
                }
            }
            std::wstring error;
            const bool face = ClockFaces::Load(clock.face, error) != nullptr;
            return face ? 1 : (error.empty() ? 0 : -1);
        }
        case 217:
            return static_cast<LRESULT>(ClockFaces::List().size());  // faces offered (shipped + user files)
        case 202:
            if (inst && inst->folder) break;  // the window's own answer
            return static_cast<LRESULT>(DesktopLayer::Count());
        case 63:  // the widget Console is open (1) / its window (lParam 1)
            if (!console_ || !console_->Hwnd()) return 0;
            return arg == 1 ? reinterpret_cast<LRESULT>(console_->Hwnd()) : 1;
        case 64:  // log lines the Console shows now
            return console_ && console_->Hwnd() ? console_->ShownLines() : -1;
        default:
            break;
    }
    if (inst && inst->folder) return wp >= 185 ? inst->folder->QueryState(wp, arg) : -1;
    if (inst && inst->widget) return inst->widget->QueryState(wp, arg);  // a standalone widget (6B)
    if (inst && inst->isWidget) return -1;  // parked
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
    if (msg == msgDockCommand_ && wp == ContextMenu::kCreateFolderWindow) {
        // The path comes as a global atom (LOWORD(lParam)); the window opens at the pointer.
        wchar_t buf[256]{};
        POINT pt{};
        GetCursorPos(&pt);
        const size_t index = GlobalGetAtomNameW(static_cast<ATOM>(lp & 0xFFFF), buf, static_cast<int>(std::size(buf)))
                                 ? AddFolderWindow(buf, pt)
                                 : SIZE_MAX;
        result = index == SIZE_MAX ? 0 : static_cast<LRESULT>(index) + 1;  // the new automation index + 1
        return true;
    }
    if (msg == msgDockCommand_ && wp == ContextMenu::kRemoveFolderWindow) {
        DockManager::Instance* inst = docks_.At(DockOfParam(lp));
        result = inst && inst->isFolder && EditDocks(SettingsDockEdit::Remove, inst->cfg) ? 1 : 0;
        if (result) RefreshSettings();
        return true;
    }
    if (msg == msgDockCommand_ && wp == ContextMenu::kInstallWidgetFolder) {
        // Add from folder… without its folder dialog: the path in a global atom. 1 installed (the
        // settings window's lists refresh), 0 refused (the reason goes to the debugger output).
        wchar_t buf[256]{};
        std::wstring id, error;
        result = GlobalGetAtomNameW(static_cast<ATOM>(lp & 0xFFFF), buf, static_cast<int>(std::size(buf))) &&
                         Widgets::InstallFolder(buf, id, error)
                     ? 1
                     : 0;
        if (!result) OutputDebugStringW((L"WinDock: install widget folder: " + error + L"\n").c_str());
        if (result) RefreshSettings();
        return true;
    }
    if (msg == msgDockCommand_ && wp == ContextMenu::kItemSettings) {
        // The settings of item LOWORD(lParam) (its index in the dock's items) of dock HIWORD(lParam).
        DockManager::Instance* inst = docks_.At(DockOfParam(lp));
        LPARAM item = ArgOfParam(lp);
        // A standalone widget entry (round 6 phase 6B): its one item is its widget (0xFFFF: the same).
        if (inst && inst->isWidget && item == 0xFFFF) item = 0;
        result = inst && !inst->isFolder && item >= 0 && static_cast<size_t>(item) < config_.docks[inst->cfg].items.size() ? 1 : 0;
        if (result) OpenItemSettings(inst->cfg, static_cast<size_t>(item));
        return true;
    }
    if (msg == msgDockCommand_ && wp == ContextMenu::kSortDesktopNow) {
        result = static_cast<LRESULT>(docks_.SortDesktop());  // the files queued
        return true;
    }
    if (msg == msgDockCommand_ && wp == ContextMenu::kSimulateExplorerRestart) {
        result = DesktopLayer::SimulateExplorerRestart();
        return true;
    }
    if (msg == msgDockCommand_ && wp == ContextMenu::kNewFolderWindow && InSendMessage()) {
        // The folder picker is modal: after the call.
        PostMessageW(hwnd_, msg, wp, lp);
        result = 1;
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
    if (msg == msgItemCommand_ && (wp & 0xFFFF) >= ContextMenu::kFolderSelect && (wp & 0xFFFF) <= ContextMenu::kFolderPaste) {
        // A folder window (phase 12): the entry / argument in LOWORD(lParam), more in HIWORD(wParam).
        const UINT cmd = static_cast<UINT>(wp & 0xFFFF);
        const int high = static_cast<int>((wp >> 16) & 0xFFFF);
        FolderWindow* folder = docks_.FolderAt(DockOfParam(lp));
        if (!folder) {
            result = -1;
        } else if (cmd == ContextMenu::kFolderDropOn) {
            FolderWindow* to = docks_.FolderAt(static_cast<size_t>(high));
            result = to && to != folder ? static_cast<LRESULT>(to->TestDrop(folder->SelectedPaths(), -1, 0)) : -1;
        } else {
            result = folder->Command(cmd, static_cast<int>(ArgOfParam(lp)), high);
        }
        return true;
    }
    if (msg == msgItemCommand_ && lp >= 0 && (lp <= 0xFFFF || !IsWindow(reinterpret_cast<HWND>(lp))) && docks_.At(DockOfParam(lp)) &&
        docks_.At(DockOfParam(lp))->isWidget) {
        // A standalone widget entry (round 6 phase 6B): 809 reload, 854 its widget's settings, 855
        // lock / unlock, 856 remove (posted); anything else: -1.
        result = WidgetEntryCommand(static_cast<UINT>(wp & 0xFFFF), docks_.At(DockOfParam(lp))->cfg);
        return true;
    }
    if (msg == msgItemCommand_ && (wp & 0xFFFF) >= ContextMenu::kCentreDismiss && (wp & 0xFFFF) <= ContextMenu::kCentreActivate) {
        // The dock's open notification centre; the row is in the high word of wParam.
        DockWindow* dock = docks_.WindowAt(DockOfParam(lp));
        result = dock && dock->CentreCommand(static_cast<UINT>(wp & 0xFFFF), static_cast<size_t>((wp >> 16) & 0xFFFF)) ? 1 : 0;
        return true;
    }
    if (msg == msgItemCommand_ && (wp & 0xFFFF) >= ContextMenu::kReminderFireNext && (wp & 0xFFFF) <= ContextMenu::kReminderDismiss) {
        const UINT cmd = static_cast<UINT>(wp & 0xFFFF);
        if (cmd == ContextMenu::kReminderFireNext) {
            result = docks_.Calendar().FireNextReminder() ? 1 : 0;
        } else {
            DockWindow* dock = docks_.WindowAt(DockOfParam(lp));
            result = dock && dock->ReminderCommand(cmd) ? 1 : 0;
        }
        return true;
    }
    if (msg == msgItemCommand_ && (wp & 0xFFFF) == ContextMenu::kCalendarSelect) {
        DockWindow* dock = docks_.WindowAt(DockOfParam(lp));
        result = dock && dock->CalendarSelect(static_cast<int16_t>((wp >> 16) & 0xFFFF)) ? 1 : 0;
        return true;
    }
    if (msg == msgItemCommand_ && (wp & 0xFFFF) >= ContextMenu::kScrollTo && (wp & 0xFFFF) <= ContextMenu::kMagnifyAt) {
        // A scrolling dock's run: an offset in the high word of wParam, or (833) the slot in lParam.
        DockWindow* dock = docks_.WindowAt(DockOfParam(lp));
        const UINT cmd = static_cast<UINT>(wp & 0xFFFF);
        const int arg = cmd == ContextMenu::kScrollIntoView ? static_cast<int>(ArgOfParam(lp))
                                                            : static_cast<int>((wp >> 16) & 0xFFFF);
        result = dock && dock->ScrollCommand(cmd, arg) ? 1 : 0;
        return true;
    }
    if (msg == msgItemCommand_ && wp >= ContextMenu::kPopupSelect && wp <= ContextMenu::kPopupOpen) {
        // The dock's open folder popup; the low word of lParam is the entry / offset.
        DockWindow* dock = docks_.WindowAt(DockOfParam(lp));
        const int arg = static_cast<int>(ArgOfParam(lp));
        bool ok = false;
        if (dock) {
            FolderPopup& popup = dock->Folder();
            ok = wp == ContextMenu::kPopupSelect   ? popup.SelectEntry(arg)
                 : wp == ContextMenu::kPopupScroll ? popup.ScrollTo(arg)
                                                   : popup.OpenEntry(arg);
        }
        result = ok ? 1 : 0;
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
        // The low word is the command; the group commands carry an argument in the high word.
        const UINT cmd = static_cast<UINT>(wp & 0xFFFF);
        const size_t arg = static_cast<size_t>((wp >> 16) & 0xFFFF);
        const bool modal = cmd == ContextMenu::kChooseIcon || cmd == ContextMenu::kChooseImage || cmd == ContextMenu::kRenameGroup;
        if (index >= 0 && (cmd == ContextMenu::kGroupWith || cmd == ContextMenu::kMemberOut || cmd == ContextMenu::kRemoveMember)) {
            const auto& items = dock->Items();
            const AppKey key = items[static_cast<size_t>(index)].key;
            if (cmd == ContextMenu::kGroupWith) {
                if (arg < items.size()) GroupWith(*dock, key, items[arg].key);
            } else if (cmd == ContextMenu::kMemberOut) {
                MemberOut(*dock, key, arg, SIZE_MAX);
            } else {
                RemoveMember(*dock, key, arg);
            }
            result = 1;
            return true;
        }
        if (index >= 0 && modal && InSendMessage()) {
            // A modal dialog would block the sender until it closes: run it after the call.
            PostMessageW(dock->Hwnd(), msgItemCommand_, wp, static_cast<LPARAM>((dockIndex << 16) | static_cast<size_t>(index)));
        } else if (index >= 0 && (cmd == ContextMenu::kTrayClick || cmd == ContextMenu::kTrayRightClick)) {
            result = dock->ClickTrayIcon(index, arg, cmd == ContextMenu::kTrayRightClick) ? 1 : 0;
            return true;
        } else if (index >= 0 && cmd >= ContextMenu::kTrayPin && cmd <= ContextMenu::kTrayMove) {
            // The tray (8b): the drawer's icons of tray slot `index` by position.
            const DockItem& slot = dock->Items()[static_cast<size_t>(index)];
            const size_t cfg = dock->ConfigIndex();
            const std::vector<const TrayIcon*> icons = dock->TrayDrawerIcons(index);
            const size_t icon = cmd == ContextMenu::kTrayMove ? (arg & 0xFF) : arg;
            result = 0;
            if (cmd == ContextMenu::kTrayUnpin) {
                if (slot.kind == DockItem::Kind::TrayApp && slot.element < config_.docks[cfg].items.size()) {
                    UnpinTray(cfg, config_.docks[cfg].items[slot.element].id);
                    result = 1;
                }
            } else if (icon < icons.size()) {
                const std::wstring key = icons[icon]->key;
                if (cmd == ContextMenu::kTrayPin) {
                    PinTray(cfg, key, SIZE_MAX);
                } else if (cmd == ContextMenu::kTrayHide) {
                    const std::vector<std::wstring>& hidden = config_.docks[cfg].items[slot.element].hidden;
                    TrayHide(cfg, slot.element, key, std::find(hidden.begin(), hidden.end(), key) == hidden.end());
                } else {
                    const size_t before = (arg >> 8) & 0xFF;
                    TrayOrder(cfg, slot.element, key, before < icons.size() ? icons[before]->key : std::wstring());
                }
                result = 1;
            }
            return true;
        } else if (index >= 0 && cmd == ContextMenu::kClickSlot) {
            // By slot, not by key: an unstacked app has one slot per window.
            dock->ClickSlot(index);
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
            // A tray icon's menu is its app's (forwarded); Ctrl + right click is the dock's.
            if (dock->RightClickAt(client, (wp & MK_CONTROL) != 0)) return true;
            // An item's own menu, or the dock menu (no item there, or a widget).
            if (const int index = dock->MenuIndexAt(client); index >= 0) {
                ShowItemMenu(*dock, index, pt);
            } else {
                ShowMenu(dock, pt, dock->IndexAt(client));  // over a widget: its settings entry too
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
    if (msg == kConsoleClosed) {
        if (console_ && !console_->Hwnd()) console_.reset();
        return 0;
    }
    if (msg == kSettingsClosed) {
        // The window is gone (its WM_NCDESTROY posted this); a newly opened one is kept.
        // A closed window is gone (its WM_NCDESTROY posted this); open ones are kept.
        if (geocodeOwner_ && !geocodeOwner_->Hwnd()) {
            geocodeOwner_ = nullptr;
            geocodeDone_ = nullptr;
        }
        if (activeSettings_ && !activeSettings_->Hwnd()) activeSettings_ = nullptr;
        std::erase_if(settings_, [](const std::unique_ptr<SettingsWindow>& w) { return !w->Hwnd(); });
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
    if (msg == kRemoveFolderMessage) {
        RemoveDesktopEntry(static_cast<size_t>(wp));
        return 0;
    }
    if (msg == WeatherService::kGeocodeMessage) {
        // A place search of the settings window finished (posted by its worker; ours to free).
        std::unique_ptr<WeatherService::Response> response(reinterpret_cast<WeatherService::Response*>(lp));
        if (wp != geocodeTag_ || !geocodeDone_ || !geocodeOwner_ || !geocodeOwner_->Hwnd() || !response) return 0;  // superseded / closed
        std::vector<WeatherService::Place> places;
        std::wstring error;
        WeatherService::ParsePlaces(*response, places, error);
        auto done = std::move(geocodeDone_);
        geocodeDone_ = nullptr;
        geocodeOwner_ = nullptr;
        done(places, error);
        return 0;
    }
    if (msgTaskbarCreated_ && msg == msgTaskbarCreated_) {
        // Our own tray host's broadcast (taking the icons over or handing them back): nothing
        // changed for us (our tray icon stays with Explorer, which gets every message mirrored).
        if (docks_.TrayService().Broadcasting()) return 0;
        // Explorer restarted: the tray icon is gone and the new taskbar must be re-hidden.
        trayAdded_ = false;
        AddTrayIcon();
        // The new Explorer knows no app bars: register again once the taskbar is re-hidden.
        docks_.AllowAppBar(false);
        taskbar_.OnTaskbarCreated();
        docks_.AllowAppBar(!shutDown_);
        docks_.TrayService().OnExplorerRestarted();  // the apps registered with the new Explorer first
        docks_.OnExplorerRestarted();  // the folder windows go back above the new desktop window
        return 0;
    }

    switch (msg) {
        case WM_HOTKEY:
            if (wp != kDashboardHotkeyId) break;
            // A hotkey capture box (Settings > General) has the focus: it records the combination.
            if (HWND focus = GetFocus(); focus && GetPropW(focus, Hotkey::kCaptureProp)) {
                SendMessageW(focus, Hotkey::kCaptureMessage, LOWORD(lp), HIWORD(lp));
                return 0;
            }
            docks_.SetDashboard(!docks_.Dashboard());
            return 0;
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
