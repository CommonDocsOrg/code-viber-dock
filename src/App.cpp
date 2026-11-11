#include "App.h"
#include "ClockFaces.h"

#include "InputDialog.h"

#include <dbt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

#include "AppIdentity.h"
#include "ContextMenu.h"
#include "DarkMode.h"
#include "DesktopLayer.h"
#include "Hotkey.h"
#include "IconUtil.h"
#include "JsonUtil.h"
#include "MenuIcons.h"
#include "Presets.h"
#include "SettingsForm.h"  // round 13 phase 13C: ItemSettingsValues / StoreItemSettings (the Item window's write)
#include "SettingsIds.h"
#include "SettingsPreview.h"  // round 18 phase 18A: SettingsGallery::kMsgChoose (DockCommand 187)
#include "Theme.h"
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

// Round 12 phase 12A: words for the undo labels and the Keep / Revert bar.
const wchar_t* EdgeWord(DockEdge e) {
    switch (e) {
        case DockEdge::Top: return L"top";
        case DockEdge::Left: return L"left";
        case DockEdge::Right: return L"right";
        case DockEdge::Bottom:
        default: return L"bottom";
    }
}

std::wstring DisplayWords(int monitor) {
    if (monitor == DockConfig::kMonitorEach) return L"every display";
    if (monitor == DockConfig::kMonitorPrimary || monitor == 0) return L"the primary display";
    return L"display " + std::to_wstring(monitor + 1);
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

// --- round 12 phase 12G: the Run key for Quick setup (AppQuickSetup.cpp) ---
bool App::AutostartOn() { return AutostartEnabled(); }
bool App::SetAutostartOn(bool enable) { return SetAutostart(enable); }
// --- end round 12 phase 12G ---

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
    msgWelcome_ = RegisterWindowMessageW(AppIpc::kWelcomeMessage);
    msgQueryState_ = RegisterWindowMessageW(DockWindow::kQueryStateMessage);
    msgQueryItem_ = RegisterWindowMessageW(DockWindow::kQueryItemMessage);

    if (config_.Load()) {
        // First run: no config file yet. Seed the default pins and write the file; the welcome
        // dialog opens once the first dock is up (round 9 phase 9C; round 12 phase 12G: Quick setup's
        // first-run variant).
        firstRun_ = true;
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
    // Round 13 phase 13C: in dark mode the dock's menus are dark (13B found them light: dark popups
    // were on only while a settings window was open, and no menu owner was allowed). Once, before
    // the windows that own menus are made; each of them then allows itself (DarkMode::DarkMenusOn).
    DarkMode::EnableDarkMenus();
    if (!CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, AppIpc::kAppWindowClass, L"WinDock", WS_POPUP, 0, 0, 0, 0,
                         nullptr, nullptr, instance, this)) {
        return false;
    }
    // The app window owns the tray-icon menu when no dock is shown (App::ShowAppMenu).
    if (DarkMode::DarkMenusOn()) DarkMode::AllowDarkForWindow(hwnd_, true);

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
        NoteLabel(L"Changed a folder window");  // round 12 phase 12A
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
        NoteLabel(L"Moved a desktop widget");  // round 12 phase 12A
        RequestSave();
        RefreshSettings();
    };
    callbacks.widgetMenu = [this](size_t cfg, POINT pt) { ShowWidgetMenu(cfg, pt); };
    callbacks.widgetSettings = [this](size_t cfg) { OpenItemSettings(cfg, 0); };
    callbacks.widgetDockDrag = [this](size_t cfg, int phase, POINT screen) { return WidgetDockDrag(cfg, phase, screen); };  // round 16 phase 16G
    // Round 12 phase 12I: a folder window / standalone widget moved, was resized or hidden: the
    // settings panel beside it follows. Posted and coalesced (it can come from inside Sync's
    // SetMonitor, and a collapse plus its config change arrive together); only when a panel is open.
    callbacks.desktopMoved = [this] {
        if (panelAnchorPending_ || !hwnd_) return;
        const bool anyPanel = std::any_of(settings_.begin(), settings_.end(), [](const std::unique_ptr<SettingsWindow>& w) {
            return w->Hwnd() && w->IsPanel() && (w->IsFolderWindow() || w->IsWidgetWindow());
        });
        if (anyPanel) panelAnchorPending_ = PostMessageW(hwnd_, kPanelAnchorMoved, 0, 0) != FALSE;
    };
    startPlacer_.Initialize(hwnd_);  // round 16 phase 16A: its watch's timer is on the app window
    docks_.SetStartPlacer(&startPlacer_);
    docks_.Initialize(instance, &config_, hwnd_, std::move(callbacks));
    // Round 6 phase 6E: reloaded widget packages render their previews again.
    docks_.Widgets().SetPackagesChanged([this] { InvalidatePreviews(); });
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
    // Round 12 phase 12A: the timeline starts from the config as it is now; the top labels as the
    // window text of two hidden statics of the app window (automation).
    undoLabel_ = CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | SS_NOPREFIX, 0, 0, 0, 0, hwnd_,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(SettingsIds::kAppUndoLabel)), instance, nullptr);
    redoLabel_ = CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | SS_NOPREFIX, 0, 0, 0, 0, hwnd_,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(SettingsIds::kAppRedoLabel)), instance, nullptr);
    // Round 12 phase 12C: QueryState 303's text channel (a dock's display name), as the two above.
    nameText_ = CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | SS_NOPREFIX, 0, 0, 0, 0, hwnd_,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(SettingsIds::kAppNameText)), instance, nullptr);
    lastState_ = SnapshotText();
    lastPlacement_ = PlacementOf(config_);
    timelineReady_ = true;
    if (firstRun_) OpenWelcome();  // round 9 phase 9C: posted, after the docks are shown
    return true;
}

void App::OpenWelcome() {
    if (hwnd_ && !shutDown_) PostMessageW(hwnd_, kShowWelcome, 0, 0);
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
    requests.reorder = [this, d](const std::vector<AppKey>& order, const AppKey& key, std::optional<ItemAnchor> anchor) {
        ApplyPinOrder(*d, order, key, anchor);
    };
    requests.unpin = [this, d](const AppKey& key) {
        const int index = IndexOfKey(*d, key);
        if (index >= 0 && d->Items()[static_cast<size_t>(index)].pinned) TogglePin(*d, key);
    };
    requests.pinFiles = [this, d](const std::vector<std::wstring>& paths, size_t at) {
        PinFiles(d->ConfigIndex(), d->Pins(), paths, at);
    };
    requests.moveItem = [this, d](size_t from, size_t to, std::optional<ItemAnchor> anchor) {
        MoveItem(d->ConfigIndex(), from, to, anchor);
    };
    requests.widgetToDesktop = [this, d](size_t item, POINT screen) {  // round 16 phase 16E
        MoveWidgetToDesktop(d->ConfigIndex(), item, screen);
    };
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
        NoteLabel(L"Moved " + c.id);  // round 12 phase 12A
        NoteTipGesture(2);            // round 12 phase 12G: the third tip's gesture (a dock dragged)
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
        NoteLabel(c.edge != edge ? L"Moved " + c.DisplayName() + L" to the " + EdgeWord(edge) + L" edge" : L"Moved " + c.DisplayName());  // round 12 phase 12A
        NoteTipGesture(2);  // round 12 phase 12G: the third tip's gesture (a dock dragged)
        c.edge = edge;
        c.align = align;
        c.offset = offset;
        c.SortByGroup();  // round 7 phase 7B: items without an anchor follow the new align's group
        docks_.ForConfig(cfg, [](DockWindow& w) { w.ApplyConfig(); });
        RequestSave();
        RefreshSettings();
    };
    // Round 12 phase 12E: customize mode's drag along / off the dock's own edge.
    requests.moveInEdge = [this, d](int offset, int edgeGap) { MoveInEdge(*d, offset, edgeGap); };
    dock.SetPinRequests(std::move(requests));
    // Round 12 phase 12E: the settings panel beside this dock follows its resting bar.
    dock.SetRestMovedCallback([this, d] { ReanchorPanels(d, true); });
    // The docks' app bars (reserve screen space): the taskbar controller must not undo the work
    // areas they leave.
    dock.SetAppBarCallback([this](bool, const RECT&, const RECT&) { UpdateReservedMonitors(); });
    DockWindow::WeatherCallbacks weather;
    weather.openSettings = [this, d](size_t item) { OpenItemSettings(d->ConfigIndex(), item); };
    dock.SetWeatherCallbacks(std::move(weather));
    // Round 12 phase 12D: customize mode (App owns it; the dock asks).
    DockWindow::EditRequests edit;
    edit.enter = [this, d] { EnterEdit(d); };
    edit.leave = [this, d] {
        if (editDock_ == d) LeaveEdit();
    };
    edit.remove = [this, d](size_t item) {
        if (editDock_ == d) EditRemoveItem(item);
    };
    edit.anchor = [this, d](size_t item, ItemAnchor anchor) {
        if (editDock_ == d) EditAnchorItem(item, anchor);
    };
    edit.plus = [this, d](int) {
        if (editDock_ == d) ShowDrawer(true);
    };
    edit.moved = [this, d] {
        if (editDock_ == d) UpdateEditUi();
    };
    edit.ended = [this, d] {
        if (editDock_ != d) return;
        editDock_ = nullptr;  // the window is going away: LeaveEdit must not touch it
        LeaveEdit();
    };
    dock.SetEditRequests(std::move(edit));
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
    ScheduleCommit();  // round 12 phase 12A: the gesture becomes one timeline entry
    if (previewing_) {
        // A Keep / Cancel preview: nothing is written until Keep (Cancel restores the file's state).
        saveHeld_ = true;
        UpdateTimelineUi();
        return;
    }
    SetTimer(hwnd_, kSaveTimerId, kSaveDelayMs, nullptr);  // restarts the debounce window
    savePending_ = true;
    UpdateTimelineUi();  // "Saving…"
}

void App::FlushSave() {
    if (!savePending_) return;
    if (hwnd_) KillTimer(hwnd_, kSaveTimerId);
    savePending_ = false;
    SaveNow();
}

// --- the timeline (round 12 phase 12A) ---------------------------------------------------------------

void App::SaveNow() {
    DWORD error = 0;
    // A failed save keeps the config in memory; the next change saves again (and Retry does now).
    saveError_ = config_.Save(&error) ? 0 : (error ? error : static_cast<DWORD>(ERROR_GEN_FAILURE));
    if (saveError_) {
        wchar_t buf[96];
        swprintf_s(buf, L"WinDock: saving config.json failed (error %lu)\n", saveError_);
        OutputDebugStringW(buf);
    }
    UpdateTimelineUi();
}

bool App::RetrySave() {
    if (previewing_) return false;  // nothing is written during a preview
    if (hwnd_) KillTimer(hwnd_, kSaveTimerId);
    savePending_ = false;
    SaveNow();
    return saveError_ == 0;
}

int App::SaveStatus() const {
    if (savePending_ || previewing_) return 1;  // a preview holds the save until Keep
    return saveError_ ? 2 : 0;
}

int App::BarKind() const {
    if (previewing_) return 2;
    if (keepRevert_) return 1;
    return saveError_ && !savePending_ ? 3 : 0;
}

std::wstring App::SaveErrorText() const {
    switch (saveError_) {
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
        case ERROR_WRITE_PROTECT:
            return L"config.json is read-only or locked";
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL:
            return L"the disk is full";
        default: {
            wchar_t* text = nullptr;
            FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                           saveError_, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
            std::wstring s = text ? text : L"";
            if (text) LocalFree(text);
            while (!s.empty() && (iswspace(s.back()) || s.back() == L'.')) s.pop_back();
            if (s.empty()) s = L"error " + std::to_wstring(saveError_);
            return s;
        }
    }
}

std::wstring App::SnapshotText() const {
    try {
        return config_.ToJson(false);  // the settings windows' sizes are not a change
    } catch (...) {
        return {};
    }
}

std::vector<App::Placement> App::PlacementOf(const Config& c) {
    std::vector<Placement> out;
    for (const DockConfig& d : c.docks) {
        if (!d.IsDock()) continue;
        out.push_back(Placement{d.id, d.edge, d.Floating(), d.monitor, d.autoHide, d.layer, d.enabled});
    }
    return out;
}

std::wstring App::DescribePlacement(const std::vector<Placement>& before, const std::vector<Placement>& after, size_t& cfg) const {
    cfg = SIZE_MAX;
    for (const Placement& a : after) {
        const auto it = std::find_if(before.begin(), before.end(), [&](const Placement& b) { return b.id == a.id; });
        if (it == before.end()) continue;  // a new dock: added, not moved
        const Placement& b = *it;
        const std::wstring who = after.size() <= 1 ? std::wstring(L"Dock") : L"Dock " + config_.DockDisplayName(DockIndexOf(a.id));  // round 12 phase 12C: by name
        std::wstring text;
        if (b.enabled && !a.enabled) {
            text = who + L" is turned off.";
        } else if (b.floating != a.floating) {
            text = a.floating ? who + L" now floats on the desktop." : who + L" is back on the " + EdgeWord(a.edge) + L" edge.";
        } else if (b.edge != a.edge) {
            text = a.floating ? who + L" now faces " + EdgeWord(a.edge) + L"." : who + L" moved to the " + EdgeWord(a.edge) + L" edge.";
        } else if (b.monitor != a.monitor) {
            text = who + L" moved to " + DisplayWords(a.monitor) + L".";
        } else if (b.autoHide != a.autoHide) {
            text = a.autoHide == AutoHideMode::Off      ? who + L" no longer hides."
                   : a.autoHide == AutoHideMode::Always ? who + L" now hides until you point at its edge."
                                                        : who + L" now hides when a window overlaps it.";
        } else if (b.layer != a.layer) {
            text = a.layer == DockLayer::Normal    ? who + L" is now above windows."
                   : a.layer == DockLayer::Desktop ? who + L" now shows only on the desktop."
                                                   : who + L" is now behind windows.";
        }
        if (!text.empty()) {
            cfg = DockIndexOf(a.id);
            return text;
        }
    }
    return {};
}

void App::NoteLabel(const std::wstring& label) {
    if (label.empty() || !pendingLabel_.empty()) return;  // the first label of a gesture wins
    pendingLabel_ = label;
    ScheduleCommit();  // a label without a change is dropped by the commit
}

void App::ScheduleCommit() {
    if (commitPosted_ || !hwnd_ || shutDown_) return;
    commitPosted_ = PostMessageW(hwnd_, kTimelineCommit, 0, 0) != FALSE;
}

void App::CommitTimeline(bool allowBar) {
    commitPosted_ = false;
    std::wstring label = std::move(pendingLabel_);
    pendingLabel_.clear();
    if (!timelineReady_ || shutDown_ || previewing_) return;  // a preview: its Keep records one entry
    std::wstring now = SnapshotText();
    if (now.empty() || now == lastState_) return;
    std::vector<Placement> placement = PlacementOf(config_);
    size_t barCfg = SIZE_MAX;
    const std::wstring moved = DescribePlacement(lastPlacement_, placement, barCfg);
    if (label.empty() && !moved.empty()) label = moved.substr(0, moved.size() - 1);  // without the full stop
    if (label.empty()) label = L"Change";
    undo_.push_back(TimelineEntry{label, std::move(lastState_), nextSerial_++});
    if (undo_.size() > kTimelineMax) undo_.erase(undo_.begin());
    redo_.clear();
    lastState_ = std::move(now);
    lastPlacement_ = std::move(placement);
    if (!moved.empty() && allowBar && config_.settings.confirmPlacement) {
        // The change moved or hid a dock: Keep / Revert for 12 s (kept when left alone).
        keepRevert_ = true;
        keepText_ = moved;
        keepLabel_ = label;
        keepState_ = undo_.back().state;
        keepSeconds_ = kKeepSeconds;
        barDock_ = barCfg;
        SetTimer(hwnd_, kKeepTimerId, 1000, nullptr);
    } else if (keepRevert_) {
        CloseKeepRevert();  // another change: the move is kept (its Revert would undo this one too)
    }
    UpdateTimelineUi();
}

bool App::ApplySnapshot(const std::wstring& state) {
    if (shutDown_ || modalDepth_ > 0) {
        MessageBeep(MB_ICONWARNING);  // a menu or dialog of a dock is open: its dock must stay
        return false;
    }
    Config fresh;
    if (state.empty() || fresh.LoadJson(state) != Config::LoadResult::Ok) return false;
    for (int k = 0; k < SettingsUiConfig::kKinds; ++k) fresh.settings.windows[k] = config_.settings.windows[k];
    fresh.settings.tipsShown = config_.settings.tipsShown;  // round 12 phase 12G: UI state, not in the snapshot
    // As Reload config, but the taskbar and the Run key only when the snapshot changes them (a
    // "Show Windows taskbar temporarily" of this session stays).
    const bool taskbar = fresh.hideTaskbar != config_.hideTaskbar;
    const bool autostart = fresh.startWithWindows != config_.startWithWindows;
    config_ = std::move(fresh);
    if (config_.Main().AutoHideOn()) lastOnMode_ = config_.Main().autoHide;
    SyncDocks();
    docks_.ForEach([](DockWindow& w) {
        w.ApplyConfig();
        w.ReloadPins();
    });
    if (taskbar) SetTaskbarEnabled(config_.hideTaskbar);
    if (autostart && AutostartEnabled() != config_.startWithWindows) SetAutostart(config_.startWithWindows);
    ApplyHotkey();
    RefreshSettings();
    // The restored state is where the timeline is now (not a change of its own); saved as any change.
    lastState_ = SnapshotText();
    lastPlacement_ = PlacementOf(config_);
    pendingLabel_.clear();
    RequestSave();
    return true;
}

bool App::Undo() {
    if (shutDown_) return false;
    FlushTimeline();
    if (previewing_) return CancelBar();  // Ctrl+Z during a preview ends it
    if (undo_.empty() || modalDepth_ > 0) return false;
    TimelineEntry e = std::move(undo_.back());
    undo_.pop_back();
    std::wstring current = lastState_;
    if (!ApplySnapshot(e.state)) {
        undo_.push_back(std::move(e));
        return false;
    }
    redo_.push_back(TimelineEntry{std::move(e.label), std::move(current), e.serial});
    CloseKeepRevert();  // what it would revert is history now
    UpdateTimelineUi();
    return true;
}

bool App::Redo() {
    if (shutDown_) return false;
    FlushTimeline();
    if (previewing_ || redo_.empty() || modalDepth_ > 0) return false;
    TimelineEntry e = std::move(redo_.back());
    redo_.pop_back();
    std::wstring current = lastState_;
    if (!ApplySnapshot(e.state)) {
        redo_.push_back(std::move(e));
        return false;
    }
    undo_.push_back(TimelineEntry{std::move(e.label), std::move(current), e.serial});
    CloseKeepRevert();
    UpdateTimelineUi();
    return true;
}

bool App::RevertWindow(SettingsWindow* w) {
    if (!w || shutDown_) return false;
    FlushTimeline();
    if (previewing_ && !CancelBar()) return false;
    const auto mark = std::find_if(marks_.begin(), marks_.end(), [&](const WindowMark& m) { return m.window == w; });
    if (mark == marks_.end()) return false;
    const uint64_t since = mark->serial;
    const long long n = std::count_if(undo_.begin(), undo_.end(), [&](const TimelineEntry& e) { return e.serial > since; });
    if (n <= 0) return false;
    const std::wstring target = mark->state;
    std::wstring before = lastState_;
    const std::wstring name = w->SearchName();
    if (!ApplySnapshot(target)) return false;
    // One entry (Undo brings the changes back), and the window counts from here.
    undo_.push_back(TimelineEntry{L"Reverted " + std::to_wstring(n) + (n == 1 ? L" change" : L" changes") + L" in " + name,
                                  std::move(before), nextSerial_++});
    if (undo_.size() > kTimelineMax) undo_.erase(undo_.begin());
    redo_.clear();
    for (WindowMark& m : marks_) {
        if (m.window == w) m.serial = undo_.back().serial;
    }
    CloseKeepRevert();
    UpdateTimelineUi();
    return true;
}

void App::StartPreview(const std::wstring& label, const std::wstring& text, HWND host, size_t cfg) {
    FlushTimeline();  // what came before is an entry of its own
    if (!previewing_) {
        CloseKeepRevert();
        FlushSave();  // the file has everything before the preview: Cancel only has to restore memory
        previewBefore_ = lastState_;
        saveHeld_ = false;
        previewing_ = true;
    }
    previewLabel_ = label;  // a second template in the same preview: one entry, named for the last
    previewText_ = text;
    previewHost_ = host;
    barDock_ = cfg;
}

bool App::KeepBar() {
    FlushTimeline();
    if (previewing_) {
        // One entry for everything the preview changed, then saved now.
        previewing_ = false;
        previewHost_ = nullptr;
        pendingLabel_ = previewLabel_;
        CommitTimeline(false);
        saveHeld_ = false;
        RequestSave();
        FlushSave();
        UpdateTimelineUi();
        return true;
    }
    if (!keepRevert_) return false;
    CloseKeepRevert();
    UpdateTimelineUi();
    return true;
}

bool App::CancelBar() {
    FlushTimeline();
    if (previewing_) {
        previewing_ = false;
        const HWND host = previewHost_;
        previewHost_ = nullptr;
        const std::wstring before = previewBefore_;
        if (!ApplySnapshot(before)) {
            previewing_ = true;  // refused (a dock menu is open): the preview stays
            previewHost_ = host;
            return false;
        }
        saveHeld_ = false;
        UpdateTimelineUi();
        return true;
    }
    if (!keepRevert_ || modalDepth_ > 0) return false;  // a dock menu / dialog is open: ApplySnapshot refuses
    const std::wstring state = keepState_, label = keepLabel_;
    std::wstring before = lastState_;
    CloseKeepRevert();
    if (!ApplySnapshot(state)) return false;
    // Revert is a change of its own: Undo moves the dock again.
    undo_.push_back(TimelineEntry{L"Reverted \x201C" + label + L"\x201D", std::move(before), nextSerial_++});
    if (undo_.size() > kTimelineMax) undo_.erase(undo_.begin());
    redo_.clear();
    UpdateTimelineUi();
    return true;
}

void App::CloseKeepRevert() {
    if (!keepRevert_) return;
    keepRevert_ = false;
    if (hwnd_) KillTimer(hwnd_, kKeepTimerId);
    keepState_.clear();
    keepText_.clear();
    keepLabel_.clear();
    keepSeconds_ = 0;
}

SettingsStatus App::StatusFor(const SettingsWindow* w) const {
    SettingsStatus s;
    s.save = SaveStatus();
    s.bar = BarKind();
    switch (s.bar) {
        case 1:
            s.barText = keepText_;
            s.cancelText = L"Revert (" + std::to_wstring(keepSeconds_) + L" s)";
            break;
        case 2:
            s.barText = previewText_;
            s.cancelText = L"Cancel";
            break;
        case 3:
            s.barText = L"Couldn't save: " + SaveErrorText() + L".";
            break;
        default:
            break;
    }
    if (!undo_.empty()) s.undoLabel = undo_.back().label;
    if (!redo_.empty()) s.redoLabel = redo_.back().label;
    for (const WindowMark& m : marks_) {
        if (m.window != w) continue;
        s.sinceOpen = static_cast<int>(
            std::count_if(undo_.begin(), undo_.end(), [&](const TimelineEntry& e) { return e.serial > m.serial; }));
    }
    return s;
}

bool App::TimelineAction(SettingsWindow* w, SettingsAction action) {
    switch (action) {
        case SettingsAction::Undo: return Undo();
        case SettingsAction::Redo: return Redo();
        case SettingsAction::Keep: return KeepBar();
        case SettingsAction::Cancel: return CancelBar();
        case SettingsAction::Retry: return RetrySave();
        case SettingsAction::OpenFolder: OpenConfigFolder(); return true;
        case SettingsAction::RevertWindow: return RevertWindow(w);
    }
    return false;
}

void App::OpenConfigFolder() {
    // Explorer with config.json selected (the folder alone when the file is gone).
    const std::wstring path = Config::FilePath();
    AllowSetForegroundWindow(ASFW_ANY);
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        const std::wstring args = L"/select,\"" + path + L"\"";
        ShellExecuteW(nullptr, nullptr, L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    } else {
        ShellExecuteW(nullptr, L"open", Config::Directory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void App::OnBannerCommand(int id) {
    if (id == SettingsIds::kKeepBarKeep) {
        KeepBar();
    } else if (id == SettingsIds::kKeepBarRevert) {
        CancelBar();
    }
}

void App::UpdateTimelineUi() {
    if (shutDown_ || !hwnd_) return;
    bool anyShown = false;
    for (size_t i = 0; i < settings_.size(); ++i) {
        SettingsWindow* w = settings_[i].get();
        if (!w->Hwnd()) continue;
        w->StatusChanged();
        if (IsWindowVisible(w->Hwnd()) && !IsIconic(w->Hwnd()) && !IsSearchHidden(w)) anyShown = true;
    }
    // The label statics (automation reads the top labels there).
    auto setText = [](HWND h, const std::wstring& text) {
        if (!h) return;
        wchar_t cur[512]{};
        GetWindowTextW(h, cur, static_cast<int>(std::size(cur)));
        if (text != cur) SetWindowTextW(h, text.c_str());
    };
    setText(undoLabel_, undo_.empty() ? std::wstring() : undo_.back().label);
    setText(redoLabel_, redo_.empty() ? std::wstring() : redo_.back().label);
    // A Keep bar while no settings window shows it: the banner near the dock.
    const int bar = BarKind();
    // Round 12 phase 12G: not while Quick setup is open (its Finish / Cancel are the preview's Keep / Cancel).
    if ((bar == 1 || bar == 2) && !anyShown && !QuickSetupOpen()) {
        const SettingsStatus s = StatusFor(nullptr);
        DockWindow* d = barDock_ < config_.docks.size() ? docks_.FirstOf(barDock_) : nullptr;
        if (!d) d = docks_.First();
        RECT barRect{}, work{};
        DockEdge edge = DockEdge::Bottom;
        bool floating = false;
        UINT dpi = GetDpiForSystem();
        if (d) {
            barRect = d->RestBarRect();
            const size_t cfg = d->ConfigIndex();
            if (cfg < config_.docks.size()) {
                edge = config_.docks[cfg].edge;
                floating = config_.docks[cfg].Floating();
            }
            dpi = d->Dpi();
        }
        MONITORINFO mi{sizeof(mi)};
        HMONITOR mon = d ? MonitorFromRect(&barRect, MONITOR_DEFAULTTOPRIMARY) : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        GetMonitorInfoW(mon, &mi);
        work = mi.rcWork;
        banner_.Show(instance_, barRect, work, edge, floating, dpi, s.barText, s.cancelText);
    } else {
        banner_.Close();
    }
    if (editDock_) UpdateEditUi();  // round 12 phase 12D: the edit bar's save state and Undo
    PlaceTip();                     // round 12 phase 12G: a tip follows its dock (moved, resized)
}

std::wstring App::EntryName(size_t cfg, size_t item) const {
    if (cfg >= config_.docks.size() || item >= config_.docks[cfg].items.size()) return L"an item";
    const DockItemEntry& e = config_.docks[cfg].items[item];
    switch (e.Kind()) {
        case ItemKind::Clock: return L"Clock";
        case ItemKind::Weather: return L"Weather";
        case ItemKind::Widget: return Widgets::DisplayName(e.id);
        case ItemKind::Tray: return L"System tray";
        case ItemKind::Running: return L"Running apps";
        case ItemKind::Notifications: return L"Notification centre";
        case ItemKind::Separator: return L"a separator";
        case ItemKind::RecycleBin: return L"Recycle Bin";
        case ItemKind::Group: return e.name.empty() ? std::wstring(L"a group") : e.name;
        default: {
            const std::wstring& s = e.path.empty() ? e.aumid : e.path;
            const size_t slash = s.find_last_of(L"\\/");
            std::wstring name = slash == std::wstring::npos ? s : s.substr(slash + 1);
            if (const size_t dot = name.rfind(L'.'); dot != std::wstring::npos && dot > 0) name.resize(dot);
            return name.empty() ? std::wstring(L"an item") : name;
        }
    }
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
    if (cfg < config_.docks.size()) config_.docks[cfg].SortByGroup();  // round 7 phase 7B: group order
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
    // Round 12 phase 12A: the undo label ("Pinned Paint", "Unpinned Paint").
    const std::wstring name = item.kind == DockItem::Kind::Separator ? std::wstring(L"a separator")
                              : item.displayName.empty()             ? std::wstring(L"an app")
                                                                     : item.displayName;
    NoteLabel((item.pinned ? (item.kind == DockItem::Kind::Separator ? L"Removed " : L"Unpinned ") : L"Pinned ") + name);

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
        // Round 7 phase 7B: it stays in the group it shows in (the running item's).
        if (dc.Anchored()) e->anchor = dc.AnchorFor(dc.GroupOf(item.element));
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
        group.anchor = t->anchor;  // round 7 phase 7B: its place on the dock is the target's
        group.offset = t->offset;
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
    m.anchor = g->anchor;  // round 7 phase 7B: beside its group (a drop elsewhere joins that group below)
    g->members.erase(g->members.begin() + static_cast<ptrdiff_t>(member));
    if (at == SIZE_MAX) at = gi + 1;
    if (g->members.empty()) {
        dc.ErasePin(gi);  // the last member out: the group goes
        if (at > gi) --at;
    }
    dc.InsertPin(at, std::move(m));
    if (const std::vector<size_t> idx = dc.PinIndexes(); at < idx.size()) dc.JoinNeighbourGroup(idx[at]);
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
    const ItemAnchor anchor = g->anchor;  // round 7 phase 7B: the members stay in the group's group
    dc.ErasePin(gi);
    for (size_t k = 0; k < members.size(); ++k) {
        members[k].anchor = anchor;
        dc.InsertPin(gi + k, std::move(members[k]));
    }
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

void App::ApplyPinOrder(DockWindow& dock, const std::vector<AppKey>& order, const AppKey& anchorKey,
                        std::optional<ItemAnchor> anchor) {
    const auto& pins = dock.Pins();  // parallel to the pin items of the dock's config
    const size_t cfg = dock.ConfigIndex();
    // Round 12 phase 12A: one drag, one entry ("Moved Paint").
    if (const int dragged = IndexOfKey(dock, anchorKey); dragged >= 0 && !dock.Items()[static_cast<size_t>(dragged)].displayName.empty()) {
        NoteLabel(L"Moved " + dock.Items()[static_cast<size_t>(dragged)].displayName);
    } else {
        NoteLabel(L"Reordered " + config_.docks[cfg].DisplayName());
    }
    const std::vector<DockItemEntry> current = config_.docks[cfg].Pins();
    std::vector<bool> used(current.size(), false);
    std::vector<DockItemEntry> next;
    next.reserve(current.size() + 1);
    for (const AppKey& key : order) {
        bool found = false;
        const bool anchorIt = anchor && key == anchorKey;  // round 7 phase 7B: dropped into another group
        // Every entry resolving to this app moves along (duplicates stay together).
        for (size_t i = 0; i < current.size() && i < pins.size(); ++i) {
            if (!used[i] && pins[i].key == key) {
                next.push_back(current[i]);
                if (anchorIt) next.back().anchor = *anchor;
                used[i] = true;
                found = true;
            }
        }
        if (found) continue;
        const int index = IndexOfKey(dock, key);  // a running item dragged into the pinned section
        if (index < 0) continue;
        if (std::optional<DockItemEntry> e = EntryForItem(dock.Items()[static_cast<size_t>(index)])) {
            if (anchorIt) e->anchor = *anchor;
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
        dock.InsertPin(at, std::move(e));
        // Round 7 phase 7B: dropped among a group's items, it joins that group.
        if (const std::vector<size_t> idx = dock.PinIndexes(); at < idx.size()) dock.JoinNeighbourGroup(idx[at]);
        ++at;
        changed = true;
    }
    if (!changed) return;
    // Round 12 phase 12A: "Pinned Paint" (one file) / "Pinned 3 items".
    if (paths.size() == 1) {
        const size_t slash = paths[0].find_last_of(L"\\/");
        std::wstring name = slash == std::wstring::npos ? paths[0] : paths[0].substr(slash + 1);
        if (const size_t dot = name.rfind(L'.'); dot != std::wstring::npos && dot > 0) name.resize(dot);
        NoteLabel(L"Pinned " + name);
    } else {
        NoteLabel(L"Pinned " + std::to_wstring(paths.size()) + L" items");
    }
    ReloadPins(cfg);
    RequestSave();
    RefreshSettings();
}

void App::MoveItem(size_t cfg, size_t from, size_t to, std::optional<ItemAnchor> anchor) {
    if (cfg >= config_.docks.size() || from >= config_.docks[cfg].items.size()) return;
    NoteLabel(L"Moved " + EntryName(cfg, from));  // round 12 phase 12A
    DockConfig& d = config_.docks[cfg];
    bool changed = false;
    if (anchor && d.items[from].anchor != *anchor) {  // round 7 phase 7B: dropped into another group
        d.items[from].anchor = *anchor;
        changed = true;
    }
    if (d.MoveItem(from, to)) changed = true;
    if (!changed) return;
    ReloadPins(cfg);  // sorts into group order
    RequestSave();
    RefreshSettings();  // the Contents tab's strip
}

void App::SetItemAnchor(size_t cfg, size_t item, ItemAnchor anchor) {
    if (cfg >= config_.docks.size() || item >= config_.docks[cfg].items.size()) return;
    DockItemEntry& e = config_.docks[cfg].items[item];
    if (e.anchor == anchor) return;
    e.anchor = anchor;
    ReloadPins(cfg);  // sorts into group order
    RequestSave();
    RefreshSettings();
}

void App::SetItemOffset(size_t cfg, size_t item, int offset) {
    if (cfg >= config_.docks.size() || item >= config_.docks[cfg].items.size()) return;
    DockItemEntry& e = config_.docks[cfg].items[item];
    offset = std::clamp(offset, -DockConfig::kMaxItemOffset, DockConfig::kMaxItemOffset);
    if (e.offset == offset) return;
    e.offset = offset;
    ReloadPins(cfg);
    RequestSave();
    RefreshSettings();
}

void App::AddWidget(size_t cfg, ItemKind kind) {
    const size_t at = config_.docks[cfg].AddWidget(kind);
    if (at == SIZE_MAX) {
        MessageBeep(MB_ICONWARNING);  // a second running item
        return;
    }
    NoteLabel(L"Added " + EntryName(cfg, at) + L" to " + config_.docks[cfg].DisplayName());  // round 12 phase 12A
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
            if (index < 0) return;
            const size_t element = dock.Items()[static_cast<size_t>(index)].element;
            const size_t cfg = dock.ConfigIndex();
            // Round 10 phase 10A: the notification centre has nothing of its own per dock: its
            // "Notification settings…" is App settings > Notifications (as on the Contents tab).
            if (cfg < config_.docks.size() && element < config_.docks[cfg].items.size() &&
                config_.docks[cfg].items[element].Kind() == ItemKind::Notifications) {
                OpenSettings(SettingsIds::kPageNotifications);
                return;
            }
            OpenItemSettings(cfg, element);
            return;
        }
        case ContextMenu::kToggleCalendar:
        case ContextMenu::kToggleWeatherPopup:
        case ContextMenu::kToggleCentre: {
            // Round 10 phase 10A: from an item's menu (or ItemCommand on a slot of that kind) the
            // popup of that slot, not of the dock's first item of the kind (DockCommand 114 / 115 /
            // 133 keep doing that).
            const int index = IndexOfKey(dock, key);
            const DockItem::Kind want = cmd == ContextMenu::kToggleCalendar       ? DockItem::Kind::Clock
                                        : cmd == ContextMenu::kToggleWeatherPopup ? DockItem::Kind::Weather
                                                                                  : DockItem::Kind::Notifications;
            if (index >= 0 && dock.Items()[static_cast<size_t>(index)].kind == want) {
                dock.ToggleSlotPopup(index);
            } else {
                OnCommand(cmd, dock.ConfigIndex(), &dock);
            }
            return;
        }
        case ContextMenu::kRemoveFromDock: {
            // Round 10 phase 10A: a widget kind's config item goes, as the Contents tab's Remove does
            // (its Item window closes in RefreshSettings). Not while the dock is locked (greyed).
            const int index = IndexOfKey(dock, key);
            const size_t cfg = dock.ConfigIndex();
            if (index < 0 || cfg >= config_.docks.size()) return;
            const DockItem& slot = dock.Items()[static_cast<size_t>(index)];
            DockConfig& d = config_.docks[cfg];
            if (!slot.IsWidget() || slot.element >= d.items.size() || d.locked) {
                MessageBeep(MB_ICONWARNING);
                return;
            }
            NoteLabel(L"Removed " + EntryName(cfg, slot.element) + L" from " + d.DisplayName());  // round 12 phase 12A
            d.items.erase(d.items.begin() + static_cast<ptrdiff_t>(slot.element));
            ReloadPins(cfg);
            RequestSave();
            RefreshSettings();
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
    LeaveEdit();  // round 12 phase 12D: the edit bar, the drawer popup, the foreground hook
    // First: closing them may commit a pending edit (focus loss), which must still be saved.
    {
        presets_.reset();  // round 9 phase 9C: before its owner (its window re-enables it)
        quickSetup_.reset();  // round 12 phase 12G: its preview is cancelled by not saving it (below)
        tips_.Close();
        tipShown_ = -1;
        help_.reset();     // round 9 phase 9D
        std::vector<std::unique_ptr<SettingsSearch>> searches = std::move(searches_);  // round 9 phase 9B: before their owners
        searches_.clear();
        searches.clear();
        searchHidden_.clear();
        std::vector<std::unique_ptr<SettingsWindow>> windows = std::move(settings_);
        settings_.clear();
        windows.clear();
        marks_.clear();  // round 12 phase 12A: their windows are gone
    }
    activeSettings_ = nullptr;
    console_.reset();  // before the docks: it listens to the widget runtime
    geocodeDone_ = nullptr;
    geocodeOwner_ = nullptr;
    RemoveHotkey();
    // Round 16 phase 16A: a Start menu open beside a button: its host back at its natural place
    // (the next Windows key would open it shifted otherwise); the UI Automation client released.
    startPlacer_.Shutdown();
    shutDown_ = true;
    // Round 12 phase 12A: a preview still open is cancelled by not saving it (RequestSave held it;
    // the file has the state before it); an open Keep / Revert keeps its change.
    banner_.Close();
    if (hwnd_) KillTimer(hwnd_, kKeepTimerId);
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
    MenuIcons::Shutdown();  // round 13 phase 13B: the menus' cached bitmaps
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
    state.hasTray = d.Has(ItemKind::Tray);
    state.hasNotifications = d.Has(ItemKind::Notifications);
    state.locked = d.locked;
    state.hasRunning = d.Has(ItemKind::Running);
    state.edge = d.edge;
    state.dockCount = static_cast<int>(config_.docks.size());
    state.dashboard = docks_.Dashboard();
    // Round 12 phase 12D: "Customize this dock" / "Done customizing".
    state.canEdit = d.IsDock();
    state.editing = editDock_ && editDock_->ConfigIndex() == cfg;
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

void App::ShowMenu(DockWindow* dock, POINT pt) {
    // Round 10 phase 10A: only the dock's background (a widget slot has its own item menu).
    if (!dock) return;
    NoteTipGesture(0);  // round 12 phase 12G: the first tip's gesture
    const size_t cfg = dock->ConfigIndex();
    const HWND owner = dock->Hwnd();
    ContextMenu::TrayMenuState state = MenuState(cfg);
    FlushTimeline();  // round 12 phase 12A: Undo "<label>" at the top
    if (!previewing_ && !undo_.empty()) state.undoLabel = undo_.back().label;
    const UINT cmd = RunMenu(dock, [&] { return ContextMenu::ShowDockMenu(owner, pt, state); });
    if (cmd == ContextMenu::kNone || shutDown_) return;
    OnCommand(cmd, cfg, docks_.ByHwnd(owner));
}

void App::ShowAppMenu(POINT pt) {
    // Round 10 phase 10A: the notification-area icon's own menu (it showed the first dock's menu).
    // The first dock still owns it (a shown, activatable window, so the menu closes on a click
    // elsewhere; KB135788) and is held shown meanwhile, as before.
    DockWindow* dock = docks_.First();
    const HWND owner = dock ? dock->Hwnd() : hwnd_;
    ContextMenu::AppMenuState state;
    state.hideTaskbar = taskbar_.Enabled();
    state.startWithWindows = AutostartEnabled();
    state.dashboard = docks_.Dashboard();
    FlushTimeline();  // round 12 phase 12A: Undo "<label>" at the top
    if (!previewing_ && !undo_.empty()) state.undoLabel = undo_.back().label;
    std::vector<std::wstring> ids;  // the entries by docks[] index, to find them again afterwards
    for (const DockConfig& d : config_.docks) {
        if (d.Has(ItemKind::Widget)) state.hasWidgets = true;
        const std::wstring kind = d.IsFolderWindow() ? L" (folder window)" : d.IsWidgetWindow() ? L" (desktop widget)" : L"";
        state.docks.push_back(d.DisplayName() + kind + L" settings…");  // round 12 phase 12C: by name
        ids.push_back(d.id);
    }
    const UINT cmd = RunMenu(dock, [&] { return ContextMenu::ShowAppMenu(owner, pt, state); });
    if (cmd == ContextMenu::kNone || shutDown_) return;
    if (cmd >= ContextMenu::kDockSettingsBase && cmd - ContextMenu::kDockSettingsBase < ids.size()) {
        // The entries may have changed while the menu was open: the same entry by id.
        const size_t now = DockIndexOf(ids[cmd - ContextMenu::kDockSettingsBase]);
        if (now != SIZE_MAX) {
            OpenDockSettings(now);
        } else {
            MessageBeep(MB_ICONWARNING);
        }
        return;
    }
    OnCommand(cmd, 0, nullptr);  // app-wide commands only (none of them acts on a dock)
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
    // Round 10 phase 10A: a widget kind's own entries (its settings named, Refresh weather, Remove).
    itemState.locked = config_.docks[cfg].locked;
    // Round 13 phase 13B: the header names the widget ("Widget: <name>", the kind's glyph); the
    // settings entry reads "Widget settings…" for every widget kind.
    if (item.IsWidget() && item.element < config_.docks[cfg].items.size()) {
        const DockItemEntry& e = config_.docks[cfg].items[item.element];
        std::wstring widgetName;
        switch (e.Kind()) {
            case ItemKind::Clock: widgetName = L"Clock"; break;
            case ItemKind::Weather:
                widgetName = L"Weather";
                itemState.weatherConfigured = WeatherOf(config_, e).Configured();
                break;
            case ItemKind::Tray: widgetName = L"Tray"; break;
            case ItemKind::Notifications: widgetName = L"Notification centre"; break;  // settings: App settings > Notifications
            case ItemKind::Widget: widgetName = Widgets::DisplayName(e.id); break;
            default: break;
        }
        itemState.hasSettings = !widgetName.empty();
        if (!widgetName.empty()) itemState.headerText = L"Widget: " + widgetName;
        itemState.headerGlyph = ContextMenu::KindGlyph(item.kind);
        itemState.quick = QuickEntries(dock, index);  // round 13 phase 13C
    } else if (item.kind != DockItem::Kind::Separator) {
        // Apps, folders, groups, the Recycle Bin: the name with the item's own icon (the cheapest
        // HICON there is, MenuIcons::ItemIcon), else the kind's glyph.
        itemState.headerText = name;
        itemState.headerGlyph = ContextMenu::KindGlyph(item.kind);
    }
    // Round 13 phase 13D: an unpinned running app is a slot of the running element: its menu opens
    // the running item's settings too (kOpenItemSettings: the slot's element is that item).
    itemState.runningSettings = !item.pinned && item.kind == DockItem::Kind::App &&
                                item.element < config_.docks[cfg].items.size() &&
                                config_.docks[cfg].items[item.element].Kind() == ItemKind::Running;
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
    bool ownsIcon = false;
    if (!item.IsWidget() && item.kind != DockItem::Kind::Separator) {
        itemState.headerIcon = MenuIcons::ItemIcon(item, MenuIcons::SizeFor(GetDpiForWindow(owner)), itemState.binEmpty,
                                                   ownsIcon);
    }
    const UINT cmd =
        RunMenu(&dock, [&] { return ContextMenu::ShowItemMenu(owner, pt, name, titles, itemState, state); });
    if (itemState.headerIcon && ownsIcon) DestroyIcon(itemState.headerIcon);
    DockWindow* again = shutDown_ ? nullptr : docks_.ByHwnd(owner);
    if (!again) return;
    // Round 13 phase 13C: a quick entry (only a widget kind's menu has them, and none of those has
    // Close all windows, 900): the slot found again, its list built again (the same one unless the
    // item changed meanwhile), the entry applied as ItemCommand 866 does.
    if (!itemState.quick.empty() && cmd >= ContextMenu::kQuickBase && cmd - ContextMenu::kQuickBase < itemState.quick.size()) {
        const int now = IndexOfKey(*again, key);
        if (now < 0 || !ApplyQuick(*again, now, cmd - ContextMenu::kQuickBase)) MessageBeep(MB_ICONWARNING);
        return;
    }
    RunItemCommand(*again, cmd, key, windows);
}

// --- round 13 phase 13C: quick settings in a widget's menu ---
std::vector<ContextMenu::QuickEntry> App::QuickEntries(const DockWindow& dock, int slot) const {
    namespace json = JsonUtil::json;
    std::vector<ContextMenu::QuickEntry> out;
    const size_t cfg = dock.ConfigIndex();
    if (slot < 0 || static_cast<size_t>(slot) >= dock.Items().size() || cfg >= config_.docks.size()) return out;
    const DockItem& item = dock.Items()[static_cast<size_t>(slot)];
    if (!item.IsWidget() || item.element >= config_.docks[cfg].items.size()) return out;
    const DockItemEntry& e = config_.docks[cfg].items[item.element];
    // The fields: a script widget's marked "quick" (WidgetPackage keeps 8 at most, bools and
    // choices only); the clock (no manifest) two hand-picked plain bools of ClockConfig, with menu
    // labels of their own. "24-hour clock" is left out: the clock's 12 / 24 hours is its `format`
    // string, not a bool (a toggle would have to rewrite a format the user may have typed).
    std::vector<Widgets::SettingField> fields;
    std::shared_ptr<const Widgets::Package> pkg;
    if (e.Kind() == ItemKind::Clock) {
        for (Widgets::SettingField& f : Widgets::BuiltInSchema(ItemKind::Clock)) {
            if (f.key == L"showSeconds") {
                f.label = L"Show seconds";
            } else if (f.key == L"showDate") {
                f.label = L"Show date";
            } else {
                continue;
            }
            fields.push_back(std::move(f));
        }
    } else if (e.Kind() == ItemKind::Widget) {
        std::wstring error;
        pkg = Widgets::Load(e.id, error);
        if (!pkg) return out;
        for (const Widgets::SettingField& f : pkg->manifest.settings) {
            if (f.quick) fields.push_back(f);
        }
    }
    if (fields.empty()) return out;
    // The values the Item window shows (its own over the defaults).
    json::JsonObject values{nullptr};
    try {
        const std::wstring text = ItemSettingsValues(config_, e, pkg ? &pkg->manifest : nullptr);
        if (!json::JsonObject::TryParse(text, values)) values = nullptr;
    } catch (...) {
        values = nullptr;
    }
    for (const Widgets::SettingField& f : fields) {
        json::IJsonValue v = values ? JsonUtil::Find(values, f.key.c_str()) : nullptr;
        if (!v) {
            try {
                v = json::JsonValue::Parse(f.defaultJson);
            } catch (...) {
                v = nullptr;
            }
        }
        if (f.type == Widgets::SettingField::Type::Bool) {
            const bool on = v && v.ValueType() == json::JsonValueType::Boolean && v.GetBoolean();
            out.push_back({f.key, on ? L"false" : L"true", f.label, L"", f.label, on});
        } else if (f.type == Widgets::SettingField::Type::Choice) {
            const std::wstring current = v && v.ValueType() == json::JsonValueType::String ? std::wstring(v.GetString()) : L"";
            for (const auto& [value, label] : f.choices) {
                out.push_back({f.key, JsonUtil::Quote(value), label, f.label, f.label, value == current});
            }
        }
        if (out.size() >= ContextMenu::kQuickMax) {
            out.resize(ContextMenu::kQuickMax);
            break;
        }
    }
    return out;
}

bool App::ApplyQuick(DockWindow& dock, int slot, size_t entry) {
    const std::vector<ContextMenu::QuickEntry> entries = QuickEntries(dock, slot);
    if (entry >= entries.size()) return false;
    const ContextMenu::QuickEntry& q = entries[entry];
    const size_t cfg = dock.ConfigIndex();
    const size_t element = dock.Items()[static_cast<size_t>(slot)].element;
    // The Item window's write (StoreItemSettings, shared with the Contents inspector) and its host
    // apply (SettingsScope::Pins: the dock re-reads the item, a script widget gets onSettingsChanged,
    // the clock redraws; saved; the open settings windows follow). A choice already chosen: nothing.
    if (!StoreItemSettings(config_, config_.docks[cfg].items[element], {{q.key, q.valueJson}})) return true;
    NoteLabel(L"Changed " + q.field + L" of " + EntryName(cfg, element));
    OnSettingsChanged(SettingsScope::Pins, cfg, nullptr);
    return true;
}
// --- end round 13 phase 13C ---

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
            case ContextMenu::kAddFromDrawer:
            case ContextMenu::kCustomize:  // round 12 phase 12D
                return;
            default:
                break;
        }
    }
    // Round 12 phase 12A: the undo labels of the menu's changes ("Auto-hide: Always").
    auto autoHideLabel = [](AutoHideMode m) {
        return std::wstring(L"Auto-hide: ") + (m == AutoHideMode::Off      ? L"Off"
                                               : m == AutoHideMode::Always ? L"Always"
                                                                           : L"When a window overlaps");
    };
    switch (cmd) {
        case ContextMenu::kUndo:  // round 12 phase 12A: the dock / tray-icon menu's Undo "<label>"
            if (!Undo()) MessageBeep(MB_OK);
            break;
        case ContextMenu::kCustomize: {  // round 12 phase 12D: the dock menu's first entry, the header's Customize
            DockWindow* w = dock ? dock : docks_.FirstOf(cfg);
            if (editDock_ && editDock_ == w) {
                LeaveEdit();
            } else if (!w) {
                MessageBeep(MB_ICONWARNING);  // parked (its display is not connected)
            } else {
                EnterEdit(w);  // a locked dock asks first (cancelled: nothing changes)
            }
            break;
        }
        case ContextMenu::kAutoHide:
            NoteLabel(autoHideLabel(d.AutoHideOn() ? AutoHideMode::Off : lastOnMode_));
            SetAutoHide(cfg, d.AutoHideOn() ? AutoHideMode::Off : lastOnMode_);
            RefreshSettings();
            break;
        case ContextMenu::kAutoHideOff:
            NoteLabel(autoHideLabel(AutoHideMode::Off));
            SetAutoHide(cfg, AutoHideMode::Off);
            RefreshSettings();
            break;
        case ContextMenu::kAutoHideAlways:
            NoteLabel(autoHideLabel(AutoHideMode::Always));
            SetAutoHide(cfg, AutoHideMode::Always);
            RefreshSettings();
            break;
        case ContextMenu::kAutoHideIntelligent:
            NoteLabel(autoHideLabel(AutoHideMode::Intelligent));
            SetAutoHide(cfg, AutoHideMode::Intelligent);
            RefreshSettings();
            break;
        case ContextMenu::kHideTaskbar: {
            const bool hide = !taskbar_.Enabled();
            NoteLabel(hide ? L"Hide the Windows taskbar: On" : L"Hide the Windows taskbar: Off");
            config_.hideTaskbar = hide;
            SetTaskbarEnabled(hide);
            RequestSave();
            RefreshSettings();
            break;
        }
        case ContextMenu::kStartWithWindows: {
            const bool enable = !AutostartEnabled();
            if (SetAutostart(enable)) {
                NoteLabel(enable ? L"Start with Windows: On" : L"Start with Windows: Off");
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
        // --- round 12 phase 12G: the dock menu's App > Quick setup…, About's Show the tips again ---
        case ContextMenu::kQuickSetup:
            OpenQuickSetup(false);
            break;
        case ContextMenu::kShowTips:
            StartTips(true);
            break;
        // --- end round 12 phase 12G ---
        case ContextMenu::kDockSettings:
            panelFrom_ = dock ? dock->Hwnd() : nullptr;  // round 12 phase 12E: the panel beside this instance
            OpenDockSettings(cfg);
            break;
        // --- round 12 phase 12C: the dock menu's Rename dock… / Duplicate dock / Save dock as layout… ---
        case ContextMenu::kRenameDock:
            if (!d.IsDock() || !BeginRenameDock(cfg)) MessageBeep(MB_ICONWARNING);
            break;
        case ContextMenu::kDuplicateDock:
            if (!d.IsDock() || DuplicateDock(cfg) == SIZE_MAX) MessageBeep(MB_ICONWARNING);
            break;
        case ContextMenu::kSaveDockLayout:
            if (d.IsDock()) AskSaveLayout(cfg, dock ? dock->Hwnd() : nullptr);
            break;
        // --- end round 12 phase 12C ---
        case ContextMenu::kAddFromDrawer:  // round 10 phase 10A: Add > Apps, folders and more widgets…
            panelFrom_ = dock ? dock->Hwnd() : nullptr;  // round 12 phase 12E
            OpenDockSettings(cfg, SettingsIds::kTabContents);  // its add drawer is always shown
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
        case ContextMenu::kRerenderPreviews:  // round 6 phase 6E: every settings window's previews again
            InvalidatePreviews();
            break;
        case ContextMenu::kHelpMenu:  // round 9 phase 9D: the dock menu's Help…
            OpenHelp(L"quick-start.md");
            break;
        case ContextMenu::kTaskManager: {  // round 7 phase 7A: Windows' Task Manager (it elevates by itself)
            const auto rc = reinterpret_cast<INT_PTR>(
                ShellExecuteW(nullptr, L"open", L"taskmgr.exe", nullptr, nullptr, SW_SHOWNORMAL));
            if (rc <= 32) {
                wchar_t msg[96];
                swprintf_s(msg, L"WinDock: starting taskmgr.exe failed (%lld)\n", static_cast<long long>(rc));
                OutputDebugStringW(msg);
            }
            break;
        }
        case ContextMenu::kRefreshWeather:
            docks_.RefreshWeather(nullptr);  // every place
            break;
        case ContextMenu::kWeatherSettings: {
            // The dock's first weather item's settings window (none: the dock's Contents tab).
            const auto it = std::find_if(d.items.begin(), d.items.end(), [](const DockItemEntry& e) {
                return e.Kind() == ItemKind::Weather || (e.Kind() == ItemKind::Widget && e.id == L"weather");
            });
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
            NoteLabel((d.locked ? L"Unlocked " : L"Locked ") + d.DisplayName());
            d.locked = !d.locked;  // read by the dock when a drag / drop starts
            docks_.ApplyWidgetWindow(cfg);  // a standalone widget: dock.locked
            RequestSave();
            RefreshSettings();
            break;
        case ContextMenu::kLockAllDocks:
        case ContextMenu::kUnlockAllDocks:
            NoteLabel(cmd == ContextMenu::kLockAllDocks ? L"Locked all docks" : L"Unlocked all docks");
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
            NoteLabel(cmd == ContextMenu::kTrayEnable ? L"Tray icons on the dock: On" : L"Tray icons on the dock: Off");
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
        case ContextMenu::kToggleBadges: {
            // Round 13 phase 13D: badges are the running item's setting (no running item: nothing to
            // toggle; the dock shows no running apps of its own).
            RunningConfig runCfg = d.Running();
            if (!d.Has(ItemKind::Running)) break;
            NoteLabel(runCfg.badges ? L"Notification badges: Off" : L"Notification badges: On");
            runCfg.badges = !runCfg.badges;
            d.SetRunning(runCfg);
            docks_.SyncNotifications();  // starts / stops the service, redraws the badges
            RequestSave();
            RefreshSettings();
            break;
        }
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
            NoteLabel(L"Added a separator to " + d.DisplayName());
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
            NoteLabel(L"Moved " + d.DisplayName() + L" to the " + EdgeWord(edge) + L" edge");
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
    ReanchorPanels(nullptr, false);  // round 12 phase 12E: beside the docks as they are now (a parked one's closes)
    if (hwnd_) PostMessageW(hwnd_, kReapMessage, 0, 0);  // closed windows are freed outside their messages
    PlaceTip();  // round 12 phase 12G: the first dock may be on another display now
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
        SaveNow();  // the file was deleted: write the current settings back
        return;
    }
    // Round 12 phase 12A: what came before is an entry of its own; a preview open now ends (the
    // file never had it: the reload is its Cancel).
    FlushTimeline();
    if (previewing_) {
        previewing_ = false;
        saveHeld_ = false;
        previewHost_ = nullptr;
    }
    CloseKeepRevert();
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
    // Round 12 phase 12A: the file's state is one entry (Undo goes back to what was in memory).
    pendingLabel_ = L"Reloaded config.json";
    CommitTimeline(false);
    UpdateTimelineUi();
}

void App::RefreshSettings(SettingsWindow* except) {
    SyncSettingsTargets();
    // Index loop: nothing below adds or frees windows (closed ones are freed by kSettingsClosed).
    for (size_t i = 0; i < settings_.size(); ++i) {
        SettingsWindow* w = settings_[i].get();
        if (w == except || !w->Hwnd()) continue;
        w->Refresh();
    }
    if (QuickSetupOpen()) quickSetup_->Refresh();  // round 12 phase 12G (posted)
}

void App::InvalidatePreviews() {
    ++previewGeneration_;
    drawerPopup_.InvalidatePreviews();  // round 12 phase 12D
    for (size_t i = 0; i < settings_.size(); ++i) {
        if (SettingsWindow* w = settings_[i].get(); w->Hwnd()) w->InvalidatePreviews();
    }
}

bool App::PreTranslateMessage(MSG& msg) {
    // A window may open another one from inside its own message: the index loop and raw pointer
    // stay valid (the objects themselves never move).
    if (presets_ && presets_->Hwnd() && presets_->PreTranslateMessage(msg)) return true;  // round 9 phase 9C
    // Round 12 phase 12G: Quick setup's keys; Esc in any of our windows while a tip shows skips the
    // tips (the bubble never has the focus) and then does what it does there.
    if (QuickSetupOpen() && quickSetup_->PreTranslateMessage(msg)) return true;
    if (tipShown_ >= 0 && msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) OnTipCommand(SettingsIds::kTipSkip);
    // Round 12 phase 12H: Ctrl+Z / Ctrl+Y (and Ctrl+Shift+Z) in customize mode, as in the settings
    // windows (12A had them only there): on the dock being customized (it takes the keyboard when
    // the mode starts; with no focus window its keys come as WM_SYSKEYDOWN), its edit bar and its
    // drawer popup. One step per press (an auto-repeat does nothing); the drawer's search box keeps
    // its own Ctrl+Z while it can undo its typing, as an edit box in a settings window does.
    if (editDock_ && (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) && (msg.wParam == 'Z' || msg.wParam == 'Y') &&
        GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_MENU) >= 0 && msg.hwnd) {
        const HWND root = GetAncestor(msg.hwnd, GA_ROOT);
        if (root && (root == editDock_->Hwnd() || root == editBar_.Hwnd() || root == drawerPopup_.Hwnd())) {
            const bool redo = msg.wParam == 'Y' || GetKeyState(VK_SHIFT) < 0;
            wchar_t cls[16]{};
            const bool editUndo = !redo && GetClassNameW(msg.hwnd, cls, static_cast<int>(std::size(cls))) && _wcsicmp(cls, L"Edit") == 0 &&
                                  SendMessageW(msg.hwnd, EM_CANUNDO, 0, 0);
            if (!editUndo) {
                if (!(msg.lParam & (1 << 30)) && !(redo ? Redo() : Undo())) MessageBeep(MB_OK);
                return true;
            }
        }
    }
    // Round 12 phase 12D: the drawer popup's keyboard (Tab between its controls, Escape closes it).
    if (const HWND drawer = drawerPopup_.Hwnd(); drawer && (msg.hwnd == drawer || IsChild(drawer, msg.hwnd)) && IsDialogMessageW(drawer, &msg)) {
        return true;
    }
    if (help_ && help_->Hwnd() && help_->PreTranslateMessage(msg)) return true;           // round 9 phase 9D
    for (size_t i = 0; i < settings_.size(); ++i) {
        if (SettingsWindow* w = settings_[i].get(); w->Hwnd() && w->PreTranslateMessage(msg)) return true;
    }
    for (size_t i = 0; i < searches_.size(); ++i) {  // round 9 phase 9B: the search dialogs
        if (SettingsSearch* s = searches_[i].get(); s->Hwnd() && s->PreTranslateMessage(msg)) return true;
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
        if (w->Hwnd() && w->Kind() == kind && !IsSearchHidden(w.get())) ++n;  // round 9 phase 9B: not the search's hidden ones
    }
    return n;
}

bool App::KeepShownFor(size_t cfg) const {
    // The active settings window is a Dock / Item window of this dock (the Global one keeps nothing shown).
    if (activeSettings_ && activeSettings_->Hwnd() && activeSettings_->Kind() != SettingsKind::Global && activeSettings_->DockIndex() == cfg) {
        return true;
    }
    // Round 12 phase 12E: the dock's settings panel holds it shown for as long as it is open (it
    // sits beside the bar, active or not; the drawer's hold works the same way).
    for (const auto& w : settings_) {
        if (w->Hwnd() && w->IsPanel() && IsWindowVisible(w->Hwnd()) && w->DockIndex() == cfg) return true;
    }
    return false;
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
        case ItemKind::Running:  // round 13 phase 13D: RunningItemPage (was the dock's Running apps tab)
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
        if (config_.docks[d].IsWidgetWindow()) {
            DestroyWindow(w->Hwnd());  // round 8 phase 8C: a standalone widget has no Item window
            continue;
        }
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
    host.dockMonitor = [this, dockOf, w] {
        const size_t cfg = dockOf();
        if (DockWindow* d = cfg != SIZE_MAX ? PanelDockOf(w) : nullptr) return d->MonitorRect();  // 12E: the panel's instance
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
    host.dockDpi = [this, dockOf, w] {
        DockWindow* d = dockOf() != SIZE_MAX ? PanelDockOf(w) : nullptr;  // round 12 phase 12E: the panel's instance
        // Round 12 phase 12I: a folder window's / standalone widget's panel opens at its window's DPI.
        if (HWND h = nullptr; !d && dockOf() != SIZE_MAX && DesktopAnchor(dockOf(), nullptr, &h) && h) return GetDpiForWindow(h);
        if (!d) d = docks_.First();
        return d ? d->Dpi() : GetDpiForSystem();
    };
    host.activated = [this, w](bool active) { OnSettingsActivated(w, active); };
    host.dockBar = [this, dockOf, w] {
        // Round 12 phase 12I: a folder window's / standalone widget's panel sits beside that window.
        if (RECT r{}; dockOf() != SIZE_MAX && DesktopAnchor(dockOf(), &r, nullptr)) return r;
        if (WidgetWindow* wi = dockOf() != SIZE_MAX ? docks_.WidgetOf(dockOf()) : nullptr) return wi->ScreenRect();
        DockWindow* d = dockOf() != SIZE_MAX ? PanelDockOf(w) : nullptr;  // round 12 phase 12E: the panel's instance
        return d ? d->RestFootprintRect() : RECT{};  // round 12 phase 12H: with a title outside the bar
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
        if (SettingsWindow* d = FindSettings(SettingsKind::Dock, w->DockId()); d && !IsIconic(d->Hwnd()) && IsWindowVisible(d->Hwnd())) {
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
    host.perfMetrics = [this] {
        std::vector<SettingsPerfMetric> out;
        for (const PerfService::Metric& m : docks_.Widgets().PerfMetricList()) {
            out.push_back(SettingsPerfMetric{JsonUtil::Utf8ToWide(m.key), m.name, m.group, m.hwinfo});
        }
        return out;
    };
    // Round 6 phase 6E: the widget previews (DockManager's WidgetPreview).
    host.renderPreview = [this](const std::wstring& id, float scale, SIZE fit, float maxZoom) {
        const WidgetPreview::Result r = docks_.RenderPreview(id, scale, static_cast<UINT>(std::max<LONG>(1, fit.cx)),
                                                             static_cast<UINT>(std::max<LONG>(1, fit.cy)), maxZoom);
        return SettingsWidgetPreview{r.bitmap, r.px, r.error, r.ms};
    };
    host.previewStamp = [this] { return WidgetPreview::ThemeStamp(config_) ^ (previewGeneration_ * 0x9E3779B97F4A7C15ull); };
    host.previewPanel = [this] { return config_.theme.background; };
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
    host.openSearch = [this, w](const std::wstring& text) { OpenSearch(w, text); };  // round 9 phase 9B
    host.openPresets = [this, w](int mode, size_t dock) {  // round 9 phase 9C
        // Round 12 phase 12C: 3 a new dock from Mine (Dock from a layout…).
        if (mode == 1) {  // round 12 phase 12G: About's Run quick setup again
            OpenQuickSetup(false);
            return;
        }
        OpenPresets(mode == 1 ? PresetMode::Welcome : mode == 2 || mode == 3 ? PresetMode::NewDock : PresetMode::Apply, dock, w->Hwnd(),
                    mode == 3 ? PresetKind::Mine : PresetKind::Layouts);
    };
    // Round 12 phase 12C: Duplicate dock / Save dock as layout… / Rename dock of config dock `dock`.
    host.dockAction = [this, w](UINT command, size_t dock) {
        switch (command) {
            case ContextMenu::kDuplicateDock: return DuplicateDock(dock) != SIZE_MAX;
            case ContextMenu::kSaveDockLayout:
                // The name dialog owned by this window (disabled meanwhile, as its own dialogs are).
                if (dock >= config_.docks.size() || !config_.docks[dock].IsDock() || !w->Hwnd()) return false;
                AskSaveLayout(dock, w->Hwnd());
                return true;
            case ContextMenu::kRenameDock: return BeginRenameDock(dock);
            default: return false;
        }
    };
    host.openHelp = [this, w](const std::wstring& topic) { OpenHelp(topic, w->Hwnd()); };  // round 9 phase 9D
    host.modal = [this](bool open) {  // round 10 phase 10D: Reset to factory defaults…'s confirmation
        if (open) {
            ++modalDepth_;
        } else {
            EndModal();
        }
    };
    // Round 12 phase 12A: the timeline and the save state.
    host.status = [this, w] { return StatusFor(w); };
    host.timeline = [this, w](SettingsAction action) { return TimelineAction(w, action); };
    host.label = [this](const std::wstring& label) { NoteLabel(label); };
    host.preview = [this, w](const std::wstring& label, const std::wstring& text) {  // round 12 phase 12B
        StartPreview(label, text, w->Hwnd(), w->DockIndex() != SIZE_MAX ? w->DockIndex() : 0);
    };
    // Round 12 phase 12D: a Contents drawer tile dragged onto a dock or the desktop.
    host.drawerDrag = [this](int phase, POINT screen, const DockItemEntry* entry) { return DrawerDrag(phase, screen, entry); };
    host.editDocks = [this](SettingsDockEdit edit, size_t index) { return EditDocks(edit, index); };
    // Round 18 phase 18A: Docks and displays' Add a dock card.
    host.addLayoutDock = [this, w](bool mine, size_t index, bool copyPins, int monitor) {
        return AddDockFromCard(w, mine, index, copyPins, monitor);
    };
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

bool App::AddSettingsWindow(std::unique_ptr<SettingsWindow> w, bool show) {
    SettingsWindow* raw = w.get();
    raw->Host() = MakeSettingsHost(raw);
    if (!raw->Create(instance_, show)) {
        if (activeSettings_ == raw) activeSettings_ = nullptr;
        if (geocodeOwner_ == raw) {
            geocodeOwner_ = nullptr;
            geocodeDone_ = nullptr;
        }
        MessageBeep(MB_ICONWARNING);
        return false;
    }
    settings_.push_back(std::move(w));
    // Round 12 phase 12A: Revert this window goes back to the config as it is now.
    FlushTimeline();
    marks_.push_back(WindowMark{raw, nextSerial_ - 1, lastState_});
    UpdateTimelineUi();  // the banner gives way to the window's bar
    if (raw->IsPanel()) UpdateKeepShown();  // round 12 phase 12E: the panel holds its dock shown (also when not activated)
    return true;
}

void App::OpenSettings(int page) {
    if (shutDown_) return;
    if (SettingsWindow* w = FindSettings(SettingsKind::Global)) {
        ClaimSettings(w);
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
    // Round 12 phase 12E: the dock window this came from (an "each" dock's instance; set by the menu
    // and edit bar paths just before), taken once.
    const HWND from = panelFrom_;
    panelFrom_ = nullptr;
    if (shutDown_ || dock >= config_.docks.size()) return;
    const std::wstring id = config_.docks[dock].id;
    DockWindow* fromDock = from ? docks_.ByHwnd(from) : nullptr;
    if (fromDock && fromDock->ConfigIndex() != dock) fromDock = nullptr;
    if (SettingsWindow* w = FindSettings(SettingsKind::Dock, id)) {
        ClaimSettings(w);
        if (tab >= 0) w->ShowPage(tab);
        // Opened from another instance of the dock: the panel moves beside that one.
        if (w->IsPanel() && fromDock && fromDock->Hwnd() != w->AnchorDock()) {
            w->SetAnchorDock(fromDock->Hwnd());
            if (IsWindowVisible(w->Hwnd())) w->Reanchor(true);
        }
        w->Activate();
        return;
    }
    auto w = std::make_unique<SettingsWindow>(SettingsHost{}, SettingsKind::Dock, id);
    SettingsWindow* raw = w.get();
    if (fromDock) raw->SetAnchorDock(fromDock->Hwnd());  // (before Create: it places the panel)
    if (AddSettingsWindow(std::move(w)) && tab >= 0) raw->ShowPage(tab);
}

void App::OpenItemSettings(size_t dock, size_t item) {
    if (shutDown_ || dock >= config_.docks.size() || item >= config_.docks[dock].items.size()) return;
    // Round 8 phase 8C: a standalone widget's widget settings are the Widget tab of its own (Dock
    // kind) window, not an Item window (DockCommand 142, ItemCommand 854, its menu, the widget's
    // weather.openSettings()).
    if (config_.docks[dock].IsWidgetWindow()) {
        OpenDockSettings(dock, SettingsIds::kTabWidgetSettings);
        return;
    }
    // Round 5 phase B: the item's own window (an item without settings: its dock's window).
    if (!HasItemWindow(dock, item)) {
        OpenDockSettings(dock, SettingsIds::kTabContents);
        return;
    }
    const std::wstring id = config_.docks[dock].id;
    if (SettingsWindow* w = FindSettings(SettingsKind::Item, id, item)) {
        ClaimSettings(w);
        w->Activate();
        return;
    }
    auto w = std::make_unique<SettingsWindow>(SettingsHost{}, SettingsKind::Item, id, item);
    w->SetItemCopy(config_.docks[dock].items[item], config_.docks[dock].items.size());
    AddSettingsWindow(std::move(w));
}

// --- the settings search (round 9 phase 9B) ----------------------------------------------------

bool App::IsSearchHidden(const SettingsWindow* w) const {
    return std::find(searchHidden_.begin(), searchHidden_.end(), w) != searchHidden_.end();
}

void App::ClaimSettings(SettingsWindow* w) { std::erase(searchHidden_, w); }

SettingsSearch* App::SearchOf(const SettingsWindow* w) const {
    if (!w || !w->Hwnd()) return nullptr;
    for (const auto& s : searches_) {
        if (s->Hwnd() && s->Owner() == w) return s.get();
    }
    return nullptr;
}

SettingsWindow* App::CreateHiddenSettings(SettingsKind kind, const std::wstring& dock, size_t item) {
    if (shutDown_) return nullptr;
    auto w = std::make_unique<SettingsWindow>(SettingsHost{}, kind, dock, item);
    if (kind == SettingsKind::Item) {
        const size_t d = DockIndexOf(dock);
        if (d == SIZE_MAX || item >= config_.docks[d].items.size()) return nullptr;
        w->SetItemCopy(config_.docks[d].items[item], config_.docks[d].items.size());
    }
    SettingsWindow* raw = w.get();
    if (!AddSettingsWindow(std::move(w), false)) return nullptr;  // never shown, activated or focused
    searchHidden_.push_back(raw);
    return raw;
}

SettingsSearchHost App::MakeSearchHost() {
    SettingsSearchHost host;
    host.config = &config_;
    host.windows = [this] {
        // The Global window and every docks[] entry's Dock window (a dock, a standalone widget, a
        // folder window); those that are not open are created hidden.
        std::vector<SettingsWindow*> out;
        SettingsWindow* g = FindSettings(SettingsKind::Global);
        if (!g && !config_.docks.empty()) g = CreateHiddenSettings(SettingsKind::Global, config_.docks.front().id);
        if (g) out.push_back(g);
        for (size_t i = 0; i < config_.docks.size(); ++i) {
            const std::wstring id = config_.docks[i].id;
            SettingsWindow* w = FindSettings(SettingsKind::Dock, id);
            if (!w) w = CreateHiddenSettings(SettingsKind::Dock, id);
            if (w) out.push_back(w);
        }
        return out;
    };
    host.open = [this](SettingsKind kind, const std::wstring& dock, size_t item, int page) -> SettingsWindow* {
        // The paths of DockCommand 107 / 143 / 142 (a hidden window is shown by them).
        if (kind == SettingsKind::Global) {
            OpenSettings(page);
            return FindSettings(SettingsKind::Global);
        }
        const size_t d = DockIndexOf(dock);
        if (d == SIZE_MAX) return nullptr;
        if (kind == SettingsKind::Dock) {
            OpenDockSettings(d, page);
            return FindSettings(SettingsKind::Dock, dock);
        }
        OpenItemSettings(d, item);
        return FindSettings(SettingsKind::Item, dock, item);
    };
    host.itemWindow = [this](const std::wstring& dock, size_t item) -> SettingsWindow* {
        if (SettingsWindow* w = FindSettings(SettingsKind::Item, dock, item)) return w;
        const size_t d = DockIndexOf(dock);
        if (d == SIZE_MAX || config_.docks[d].IsWidgetWindow() || !HasItemWindow(d, item)) return nullptr;
        return CreateHiddenSettings(SettingsKind::Item, dock, item);
    };
    host.destroyed = [this] {
        if (hwnd_) PostMessageW(hwnd_, kSearchClosed, 0, 0);
    };
    return host;
}

void App::OpenSearch(SettingsWindow* owner, const std::wstring& text) {
    if (shutDown_ || !owner || !owner->Hwnd() || !IsWindowVisible(owner->Hwnd())) return;
    if (SettingsSearch* s = SearchOf(owner)) {
        s->Activate(text);
        return;
    }
    auto s = std::make_unique<SettingsSearch>(MakeSearchHost(), owner);
    SettingsSearch* raw = s.get();
    searches_.push_back(std::move(s));  // before Create: the index may open hidden windows
    if (!raw->Create(instance_, text)) {
        std::erase_if(searches_, [raw](const std::unique_ptr<SettingsSearch>& p) { return p.get() == raw; });
        MessageBeep(MB_ICONWARNING);
        if (hwnd_) PostMessageW(hwnd_, kSearchClosed, 0, 0);  // the hidden windows it made go
    }
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
            NoteLabel(L"Added dock " + d.id);  // round 12 phase 12A: the undo labels of these edits
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
            NoteLabel(L"Added floating dock " + d.id);
            list.push_back(std::move(d));
            break;
        }
        case SettingsDockEdit::Duplicate: {
            if (index >= list.size()) return false;
            // Round 12 phase 12C: a dock gets the dock menu's Duplicate dock (named, placed, appended).
            if (list[index].IsDock()) return DuplicateDock(index) != SIZE_MAX;
            DockConfig d = list[index];
            d.id = config_.UniqueDockId(d.id);
            d.color = 0;  // its own colour (the palette by index), not the copied dock's
            NoteLabel(L"Duplicated " + list[index].DisplayName());
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
            d.themeOverrides = list[index].themeOverrides;  // round 12 phase 12B: its own look too
            d.theme = list[index].theme;
            DockItemEntry tray{ToString(ItemKind::Tray)};
            auto& from = list[index].items;
            if (auto it = std::find_if(from.begin(), from.end(), [](const DockItemEntry& e) { return e.Kind() == ItemKind::Tray; });
                it != from.end()) {
                tray = *it;  // its rows, columns, hidden icons and order come along
                from.erase(it);
            }
            d.items = {std::move(tray)};
            config_.tray.enabled = true;
            NoteLabel(L"Put the tray of " + list[index].DisplayName() + L" in its own dock");
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
            NoteLabel(L"Removed " + list[index].DisplayName());
            list.erase(list.begin() + static_cast<ptrdiff_t>(index));
            break;
        }
        case SettingsDockEdit::ToggleEnabled: {
            // Round 11 phase 11D: off without losing the entry (Sync parks it), or back on. The
            // last enabled dock stays (the taskbar is hidden: something must be on screen).
            if (index >= list.size()) return false;
            DockConfig& d = list[index];
            if (d.enabled && d.IsDock() &&
                std::count_if(list.begin(), list.end(), [](const DockConfig& o) { return o.IsDock() && o.enabled; }) <= 1) {
                return false;
            }
            NoteLabel((d.enabled ? L"Turned off " : L"Turned on ") + d.DisplayName());
            d.enabled = !d.enabled;
            break;
        }
    }
    SyncDocks();
    RequestSave();
    RefreshSettings();  // a removed dock's windows close; every window's header (indexes and colours moved)
    return true;
}

// --- help (round 9 phase 9D) ---------------------------------------------------------------------

void App::OpenHelp(const std::wstring& topic, HWND nearWindow) {
    if (shutDown_) return;
    if (!help_) {
        help_ = std::make_unique<HelpWindow>([this] {
            if (hwnd_) PostMessageW(hwnd_, kHelpClosed, 0, 0);
        });
        if (!help_->Create(instance_, nearWindow)) {
            help_.reset();
            MessageBeep(MB_ICONWARNING);
            return;
        }
    }
    help_->Open(topic);
}

LRESULT App::CountUnexplained(LPARAM lp) {
    // LOWORD bit 0: the Global window, else the Dock window of dock HIWORD; bit 1: its Item windows
    // too; bit 2: the control id of gap number LOWORD >> 8 instead of the count (0: none). A window
    // that is not open is made hidden for the count and closed again (unless a search uses it).
    // Bit 3: the cards without a "?" button instead (bit 2: page index * 100 + card index).
    const LPARAM low = lp & 0xFFFF;
    std::vector<SettingsWindow*> windows, made;
    auto get = [&](SettingsKind kind, const std::wstring& dock, size_t item) -> SettingsWindow* {
        SettingsWindow* w = FindSettings(kind, dock, item);
        if (!w) {
            w = CreateHiddenSettings(kind, dock, item);
            if (w) made.push_back(w);
        }
        return w;
    };
    if (low & 1) {
        if (!config_.docks.empty()) {
            if (SettingsWindow* g = FindSettings(SettingsKind::Global)) {
                windows.push_back(g);
            } else if (SettingsWindow* h = get(SettingsKind::Global, config_.docks.front().id, SIZE_MAX)) {
                windows.push_back(h);
            }
        }
    } else {
        const DockManager::Instance* inst = docks_.At(DockOfParam(lp));
        if (!inst || inst->cfg >= config_.docks.size()) return -2;
        const size_t d = inst->cfg;
        const std::wstring id = config_.docks[d].id;
        if (SettingsWindow* w = get(SettingsKind::Dock, id, SIZE_MAX)) windows.push_back(w);
        if (low & 2) {
            for (size_t i = 0; i < config_.docks[d].items.size(); ++i) {
                if (config_.docks[d].IsWidgetWindow() || !HasItemWindow(d, i)) continue;
                if (SettingsWindow* w = get(SettingsKind::Item, id, i)) windows.push_back(w);
            }
        }
    }
    if (windows.empty()) return -2;
    std::vector<int> ids;
    int count = 0;
    for (SettingsWindow* w : windows) {
        if (low & 8) {  // bit 3: the cards without a "?" button instead
            count += w->CardsWithoutHelp(&ids);
            continue;
        }
        std::vector<std::wstring> gaps;
        count += w->UnexplainedInputs(&gaps, &ids);
        for (const std::wstring& g : gaps) OutputDebugStringW((L"WinDock 285: " + w->SearchName() + L" > " + g + L"\n").c_str());
    }
    if (searches_.empty()) {
        for (SettingsWindow* w : made) {
            std::erase(searchHidden_, w);
            if (w->Hwnd()) DestroyWindow(w->Hwnd());  // freed by kSettingsClosed
        }
    }
    if (low & 4) {
        const size_t k = static_cast<size_t>((low >> 8) & 0xFF);
        return k < ids.size() ? ids[k] : 0;
    }
    return count;
}

// --- dock presets (round 9 phase 9C) ------------------------------------------------------------

void App::OpenPresets(PresetMode mode, size_t cfg, HWND owner, PresetKind kind) {
    if (shutDown_) return;
    if (presets_ && presets_->Hwnd()) {
        presets_->Activate();  // one chooser at a time
        return;
    }
    if (Presets::All().empty() && mode != PresetMode::AppLook) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    // Welcome: the first dock (the one a first run made).
    if (mode == PresetMode::Welcome) {
        cfg = SIZE_MAX;
        for (size_t i = 0; i < config_.docks.size() && cfg == SIZE_MAX; ++i) {
            if (config_.docks[i].IsDock()) cfg = i;
        }
    }
    if (mode != PresetMode::NewDock && mode != PresetMode::AppLook && (cfg >= config_.docks.size() || !config_.docks[cfg].IsDock())) {
        MessageBeep(MB_ICONWARNING);  // a folder window / standalone widget takes no preset
        return;
    }
    const bool forDock = mode == PresetMode::Apply || mode == PresetMode::Welcome;
    const std::wstring id = forDock ? config_.docks[cfg].id : std::wstring();
    const std::wstring name = forDock ? config_.docks[cfg].DisplayName() : std::wstring();
    PresetDialogHost host;
    host.config = &config_;
    // Round 12 phase 12A: Apply / Add dock start a Keep / Cancel preview (the chooser closes; closing
    // the settings window it was opened from cancels it). The welcome variant applies at once.
    // Round 12 phase 12C: the kind (a built-in layout, a look, a saved layout) and Keep my current look.
    const HWND hostWindow = owner && owner != hwnd_ ? owner : nullptr;
    host.apply = [this, mode, id, hostWindow](PresetKind kind, size_t index, bool keepPins, bool keepLook) {
        if (kind == PresetKind::Looks) {
            if (mode == PresetMode::AppLook) return ApplyLook(index, SIZE_MAX, true, hostWindow);
            const size_t at = DockIndexOf(id);
            return at != SIZE_MAX && ApplyLook(index, at, true, hostWindow);
        }
        if (kind == PresetKind::Mine) {
            const Presets::MineList mine = Presets::Mine();  // read again: the same folder the chooser listed
            if (index >= mine.layouts.size()) return false;
            const Presets::Preset& p = mine.layouts[index];
            if (mode == PresetMode::NewDock) {
                return AddLayoutDock(p, L"from layout " + p.name, keepPins, true, hostWindow) != SIZE_MAX;
            }
            const size_t at = DockIndexOf(id);
            return at != SIZE_MAX && ApplyLayoutTo(p, L"Applied layout " + p.name, at, keepPins, keepLook, true, hostWindow);
        }
        if (mode == PresetMode::NewDock) return AddPresetDock(index, keepPins, true, hostWindow) != SIZE_MAX;
        const size_t at = DockIndexOf(id);  // by id: the docks may have moved while it was open
        return at != SIZE_MAX && ApplyPreset(index, at, keepPins, mode == PresetMode::Apply, hostWindow, keepLook);
    };
    host.destroyed = [this] {
        if (hwnd_) PostMessageW(hwnd_, kPresetClosed, 0, 0);
    };
    presets_ = std::make_unique<PresetDialog>(std::move(host), mode, owner ? owner : hwnd_, id, name, kind);
    if (!presets_->Create(instance_)) {
        presets_.reset();
        MessageBeep(MB_ICONWARNING);
    }
}

bool App::ApplyPreset(size_t index, size_t cfg, bool keepPins, bool preview, HWND host, bool keepLook) {
    const std::vector<Presets::Preset>& all = Presets::All();
    if (index >= all.size()) return false;
    return ApplyLayoutTo(all[index], L"Applied preset " + all[index].name, cfg, keepPins, keepLook, preview, host);
}

// Round 12 phase 12C: ApplyPreset's body, for a built-in preset or a saved layout.
bool App::ApplyLayoutTo(const Presets::Preset& p, const std::wstring& label, size_t cfg, bool keepPins, bool keepLook, bool preview,
                        HWND host) {
    if (shutDown_ || modalDepth_ > 0) return false;  // a dock's menu / dialog is open
    if (cfg >= config_.docks.size() || !config_.docks[cfg].IsDock()) return false;
    // Round 12 phase 12A: a preview (Keep / Cancel) or one timeline entry.
    if (preview) {
        StartPreview(label, L"Previewing " + p.name + L" on " + config_.docks[cfg].DisplayName() + L".", host, cfg);
    } else {
        NoteLabel(label);
    }
    DockConfig& d = config_.docks[cfg];
    const DockConfig before = d;
    d = Presets::Apply(p, config_, d, Presets::PinsOf(d), keepPins);
    if (keepLook) {  // round 12 phase 12C: "Keep my current look": the layout's look is not applied
        d.themePreset = before.themePreset;
        d.themeOverrides = before.themeOverrides;
        d.theme = before.theme;
    }
    // Round 11 phase 11C: a preset with the tray (Windows 7) asks for the notify icons, as adding the
    // tray item does (the dock menu's Add widget > Tray, the drawer): the takeover goes on (SyncDocks
    // starts the host). App settings > Tray icons turns it off again.
    if (d.Has(ItemKind::Tray)) config_.tray.enabled = true;
    config_.Sanitize();  // as a loaded file is (the preset's values are in range already)
    config_.ResolveDockThemes();
    if (cfg == 0 && config_.Main().AutoHideOn()) lastOnMode_ = config_.Main().autoHide;
    // As Reload config: the windows that must exist, then every key and item re-read.
    SyncDocks();
    docks_.ForConfig(cfg, [](DockWindow& w) {
        w.ApplyConfig();
        w.ReloadPins();
    });
    RequestSave();
    RefreshSettings();
    FlashDock(cfg);
    return true;
}

size_t App::AddPresetDock(size_t index, bool copyPins, bool preview, HWND host) {
    const std::vector<Presets::Preset>& all = Presets::All();
    if (index >= all.size()) return SIZE_MAX;
    return AddLayoutDock(all[index], L"from preset " + all[index].name, copyPins, preview, host);
}

// Round 12 phase 12C: AddPresetDock's body, for a built-in preset or a saved layout; `from` ends the
// undo label ("Added dock work from layout Work").
size_t App::AddLayoutDock(const Presets::Preset& p, const std::wstring& from, bool copyPins, bool preview, HWND host, int monitor,
                          bool placeFree) {
    if (shutDown_ || modalDepth_ > 0) return SIZE_MAX;
    std::vector<DockItemEntry> pins;
    if (copyPins && p.hasPins) {
        const auto first = std::find_if(config_.docks.begin(), config_.docks.end(), [](const DockConfig& d) { return d.IsDock(); });
        if (first != config_.docks.end()) pins = Presets::PinsOf(*first);
    }
    DockConfig made = Presets::MakeNew(p, config_, pins, monitor, placeFree);
    // Round 12 phase 12A: a preview (Keep / Cancel) or one timeline entry.
    const std::wstring label = L"Added dock " + made.DisplayName() + L" " + from;
    if (preview) {
        StartPreview(label, L"Previewing " + p.name + L" as the new dock " + made.DisplayName() + L".", host, config_.docks.size());
    } else {
        NoteLabel(label);
    }
    config_.docks.push_back(std::move(made));
    const size_t cfg = config_.docks.size() - 1;
    if (config_.docks[cfg].Has(ItemKind::Tray)) config_.tray.enabled = true;  // as ApplyPreset (round 11 phase 11C)
    config_.ResolveDockThemes();
    // The Global window's Docks and displays page selects the new dock.
    if (SettingsWindow* g = FindSettings(SettingsKind::Global)) g->SelectDock(config_.docks[cfg].id);
    SyncDocks();
    RequestSave();
    RefreshSettings();
    FlashDock(cfg);
    return cfg;
}

// --- round 18 phase 18A: the Add a dock card ---------------------------------------------------------

int App::AddDockFromCard(SettingsWindow* w, bool mine, size_t index, bool copyPins, int monitor) {
    if (shutDown_ || modalDepth_ > 0 || !w || !w->Hwnd()) return 0;
    const HWND host = w->Hwnd();
    if (previewing_) {
        // Only the card's own preview is cancelled for the next tile (the user flips through the
        // layouts on the live desktop); a look preview of the Themes page, the chooser's or Quick
        // setup's is not the card's to throw away. A look picked while the card's dock was previewed
        // extends that preview under the look's label (StartPreview), so it no longer counts as ours.
        const bool own = !cardPreviewLabel_.empty() && previewLabel_ == cardPreviewLabel_ && previewHost_ == host;
        if (!own) return 2;
        if (!CancelBar()) return 0;  // refused (a dock menu is open): the preview stays
    }
    Presets::Preset p;
    std::wstring from;
    if (mine) {
        Presets::MineList list = Presets::Mine();  // read again: the card listed the same folder at its Load
        if (index >= list.layouts.size()) return 0;
        p = std::move(list.layouts[index]);
        from = L"from layout " + p.name;
    } else {
        const std::vector<Presets::Preset>& all = Presets::All();
        if (index >= all.size()) return 0;
        p = all[index];
        from = L"from preset " + p.name;
    }
    // A display that is not connected (unplugged since the card listed it) parks the dock as any
    // config would; the card lists only connected ones.
    if (monitor != DockConfig::kMonitorEach && monitor != DockConfig::kMonitorPrimary && monitor < 0) monitor = DockConfig::kMonitorPrimary;
    const size_t made = AddLayoutDock(p, from, copyPins, true, host, monitor, true);
    if (made == SIZE_MAX) return 0;
    cardPreviewLabel_ = previewLabel_;
    ++cardAdds_;
    return 1;
}

// --- round 12 phase 12C: docks you keep ------------------------------------------------------------

bool App::ApplyLook(size_t look, size_t cfg, bool preview, HWND host) {
    if (shutDown_ || modalDepth_ > 0) return false;
    const std::vector<Presets::Look> looks = Presets::Looks();
    if (look >= looks.size()) return false;
    const Presets::Look& l = looks[look];
    const bool app = cfg == SIZE_MAX;
    if (!app && (cfg >= config_.docks.size() || !config_.docks[cfg].IsDock())) return false;
    const std::wstring who = app ? std::wstring() : config_.docks[cfg].DisplayName();
    const std::wstring label = app ? L"Applied the " + l.name + L" look" : L"Applied the " + l.name + L" look to " + who;
    if (preview) {
        StartPreview(label, app ? L"Previewing the " + l.name + L" look." : L"Previewing the " + l.name + L" look on " + who + L".", host,
                     app ? 0 : cfg);
    } else {
        NoteLabel(label);
    }
    // Only the look: the preset with no fields of its own (a dock: its own look from now on).
    if (app) {
        config_.themePreset = l.id;
        config_.themeOverrides = 0;
        config_.ResolveTheme();  // the docks that use the app look follow
    } else {
        DockConfig& d = config_.docks[cfg];
        d.themePreset = l.id;
        d.themeOverrides = 0;
        config_.ResolveDockThemes();
    }
    docks_.ForEach([](DockWindow& w) { w.ApplyConfig(); });
    RequestSave();
    RefreshSettings();
    if (!app) FlashDock(cfg);
    return true;
}

bool App::RenameDock(size_t cfg, const std::wstring& name) {
    if (shutDown_ || cfg >= config_.docks.size()) return false;
    DockConfig& d = config_.docks[cfg];
    std::wstring n;
    for (wchar_t c : name) {
        if (c >= 0x20 && c != 0x7F) n += c;
    }
    const size_t a = n.find_first_not_of(L' ');
    n = a == std::wstring::npos ? std::wstring() : n.substr(a, n.find_last_not_of(L' ') - a + 1);
    if (n.size() > 64) n.resize(64);
    if (n == d.id) n.clear();  // the id is what an empty name shows
    if (n == d.name) return true;
    const std::wstring before = d.DisplayName();
    d.name = n;
    NoteLabel(L"Renamed " + before + L" to " + d.DisplayName());
    docks_.ForConfig(cfg, [](DockWindow& w) { w.ApplyConfig(); });  // a title that follows the name
    RequestSave();
    RefreshSettings();
    return true;
}

bool App::BeginRenameDock(size_t cfg) {
    if (shutDown_ || cfg >= config_.docks.size() || !config_.docks[cfg].IsDock()) return false;
    OpenDockSettings(cfg);
    SettingsWindow* w = FindSettings(SettingsKind::Dock, config_.docks[cfg].id);
    return w && w->BeginRename();
}

size_t App::DuplicateDock(size_t cfg) {
    if (shutDown_ || modalDepth_ > 0) return SIZE_MAX;
    if (cfg >= config_.docks.size() || !config_.docks[cfg].IsDock()) return SIZE_MAX;
    const DockConfig& src = config_.docks[cfg];
    DockConfig d = src;  // every key and item (deep: value types)
    d.id = config_.UniqueDockId(src.id);
    d.name = src.DisplayName() + L" copy";
    if (d.name.size() > 64) d.name.resize(64);
    d.enabled = true;
    // The next palette colour: appended, the copy's index is the next one (no other dock's index
    // or colour moves; before 12C the copy went right after its source).
    d.color = 0;
    // Where: the next free edge of the same display (bottom, top, left, right); every edge taken:
    // the same edge, moved along it by the bar's length. A floating bar moves by its length; a ring
    // stays where its source is.
    auto display = [](int m) { return m == DockConfig::kMonitorPrimary ? 0 : m; };
    DockWindow* w = docks_.FirstOf(cfg);
    const RECT bar = w ? w->RestBarRect() : RECT{};
    const UINT dpi = w ? w->Dpi() : USER_DEFAULT_SCREEN_DPI;
    const bool vertical = IsVertical(src.edge);
    const LONG lengthPx = vertical ? bar.bottom - bar.top : bar.right - bar.left;
    const int lengthDip = lengthPx > 0 ? MulDiv(lengthPx, USER_DEFAULT_SCREEN_DPI, static_cast<int>(dpi))
                                       : (src.iconSize + 8) * static_cast<int>(std::max<size_t>(1, src.items.size()));
    if (src.shape == DockShape::Bar && !src.Floating()) {
        bool used[4] = {};
        for (const DockConfig& o : config_.docks) {
            if (!o.IsDock() || !o.enabled || o.Floating() || o.shape == DockShape::Circle) continue;
            if (display(o.monitor) != display(src.monitor) && !o.Each() && !src.Each()) continue;
            used[static_cast<int>(o.edge) & 3] = true;
        }
        const DockEdge order[] = {DockEdge::Bottom, DockEdge::Top, DockEdge::Left, DockEdge::Right};
        const auto freeEdge = std::find_if(std::begin(order), std::end(order), [&](DockEdge e) { return !used[static_cast<int>(e)]; });
        if (freeEdge != std::end(order)) {
            d.edge = *freeEdge;
            d.offset = 0;
        } else {
            const int step = lengthDip + 8;
            d.offset = std::clamp(src.offset + (src.align == DockAlign::End ? -step : step), -16000, 16000);
        }
    } else if (src.Floating() && w) {
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromRect(&bar, MONITOR_DEFAULTTONEAREST), &mi);
        const int x = MulDiv(bar.left - mi.rcWork.left, USER_DEFAULT_SCREEN_DPI, static_cast<int>(dpi));
        const int y = MulDiv(bar.top - mi.rcWork.top, USER_DEFAULT_SCREEN_DPI, static_cast<int>(dpi));
        d.x = std::clamp(vertical ? x : x + lengthDip + 8, -16000, 16000);
        d.y = std::clamp(vertical ? y + lengthDip + 8 : y, -16000, 16000);
    }
    NoteLabel(L"Duplicated " + src.DisplayName());
    config_.docks.push_back(std::move(d));
    const size_t made = config_.docks.size() - 1;
    config_.ResolveDockThemes();
    if (SettingsWindow* g = FindSettings(SettingsKind::Global)) g->SelectDock(config_.docks[made].id);
    SyncDocks();
    RequestSave();
    RefreshSettings();
    FlashDock(made);
    return made;
}

unsigned long App::SaveDockLayout(size_t cfg, const std::wstring& name, bool overwrite) {
    std::wstring id;
    return Presets::SaveLayout(config_, cfg, name, overwrite, id);
}

void App::AskSaveLayout(size_t cfg, HWND owner) {
    if (shutDown_ || modalDepth_ > 0 || cfg >= config_.docks.size() || !config_.docks[cfg].IsDock()) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    const std::wstring id = config_.docks[cfg].id;
    std::wstring name = config_.docks[cfg].DisplayName();
    // The owner: the dock it was chosen from (held shown meanwhile), else its Dock window or App
    // settings when one is shown, else the app window (the dialog is centred on the primary display).
    DockWindow* dock = owner ? docks_.ByHwnd(owner) : nullptr;
    const HWND dockHwnd = dock ? dock->Hwnd() : nullptr;  // looked up again afterwards (it may be gone)
    if (!owner) {
        SettingsWindow* w = FindSettings(SettingsKind::Dock, id);
        if (!w || !IsWindowVisible(w->Hwnd())) w = FindSettings(SettingsKind::Global);
        owner = w && IsWindowVisible(w->Hwnd()) ? w->Hwnd() : hwnd_;
    }
    if (dock) dock->SetHold(true);
    ++modalDepth_;
    HHOOK hook = nullptr;
    if (dock) {  // the dock's windows do not activate: the dialog gets the focus as RenameGroup's does
        g_activateTarget = hwnd_;
        g_activateMessage = kDialogActivated;
        hook = SetWindowsHookExW(WH_CBT, &ActivateHook, nullptr, GetCurrentThreadId());
    }
    bool ok = InputDialog::Ask(owner, L"Save dock as layout", L"Name of the layout:", name, 64);
    if (hook) UnhookWindowsHookEx(hook);
    g_activateTarget = nullptr;
    bool overwrite = false;
    if (ok) {
        const std::wstring path = Config::LayoutsDirectory() + L"\\" + Presets::LayoutStem(name) + L".json";
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            const std::wstring text = L"A layout named \x201C" + name + L"\x201D is saved already. Replace it?";
            overwrite = MessageBoxW(IsWindow(owner) ? owner : nullptr, text.c_str(), L"Save dock as layout", MB_YESNO | MB_ICONQUESTION) == IDYES;
            ok = overwrite;
        }
    }
    unsigned long error = 0;
    const size_t at = DockIndexOf(id);  // by id: the docks may have changed meanwhile
    if (ok) error = at != SIZE_MAX ? SaveDockLayout(at, name, overwrite) : ERROR_NOT_FOUND;
    if (ok && error != 0) {
        MessageBoxW(IsWindow(owner) ? owner : nullptr, L"The layout could not be saved (the layouts folder could not be written).",
                    L"Save dock as layout", MB_OK | MB_ICONWARNING);
    }
    if (DockWindow* again = dockHwnd && !shutDown_ ? docks_.ByHwnd(dockHwnd) : nullptr) again->SetHold(false);
    EndModal();
}

bool App::KeepCommand(UINT command, LPARAM lp) {
    if (shutDown_) return false;
    const size_t instance = DockOfParam(lp);
    const size_t low = static_cast<size_t>(lp & 0xFFFF);
    auto shownHost = [this](SettingsWindow* w) { return w && w->Hwnd() && IsWindowVisible(w->Hwnd()) ? w->Hwnd() : nullptr; };
    if (command == ContextMenu::kApplyLookPreset && instance == 0xFFFF) {
        return ApplyLook(low, SIZE_MAX, true, shownHost(FindSettings(SettingsKind::Global)));
    }
    const DockManager::Instance* inst = docks_.At(instance);
    if (!inst || inst->cfg >= config_.docks.size() || !config_.docks[inst->cfg].IsDock()) return false;
    const size_t cfg = inst->cfg;
    const HWND host = shownHost(FindSettings(SettingsKind::Dock, config_.docks[cfg].id));
    wchar_t buf[256]{};
    switch (command) {
        case ContextMenu::kRenameDock:
            if (low == 0) return BeginRenameDock(cfg);
            if (!GlobalGetAtomNameW(static_cast<ATOM>(low), buf, static_cast<int>(std::size(buf)))) return false;
            return RenameDock(cfg, buf);
        case ContextMenu::kDuplicateDock:
            return DuplicateDock(cfg) != SIZE_MAX;
        case ContextMenu::kApplySavedLayout: {
            // LOWORD & 0xFF the index, bit 8 keep the pins, bit 9 keep the look (as DockCommand 153).
            const Presets::MineList mine = Presets::Mine();
            const size_t index = low & 0xFF;
            if (index >= mine.layouts.size()) return false;
            return ApplyLayoutTo(mine.layouts[index], L"Applied layout " + mine.layouts[index].name, cfg, (low & 0x100) != 0,
                                 (low & 0x200) != 0, true, host);
        }
        case ContextMenu::kApplyLookPreset:
            return ApplyLook(low, cfg, true, host);
        case ContextMenu::kSaveDockLayout:
            if (low == 0) {
                // The name dialog is modal: posted, so a sender is not blocked (it runs once this
                // returned; LOWORD 0xFFFF marks the posted one).
                return PostMessageW(hwnd_, msgDockCommand_, ContextMenu::kSaveDockLayout, (lp & ~LPARAM{0xFFFF}) | 0xFFFF) != FALSE;
            }
            if (low == 0xFFFF) {
                AskSaveLayout(cfg, nullptr);
                return true;
            }
            if (!GlobalGetAtomNameW(static_cast<ATOM>(low), buf, static_cast<int>(std::size(buf)))) return false;
            return SaveDockLayout(cfg, buf, true) == 0;
        default:
            return false;
    }
}
// --- end round 12 phase 12C ---

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
    DockItemEntry e;
    e.kind = ToString(ItemKind::Widget);
    e.id = id;
    return AddWidgetWindow(std::move(e), nearDock, open);
}

bool App::CanBeStandalone(const DockItemEntry& e) {
    return e.Kind() == ItemKind::Widget && Widgets::ValidId(e.id) && Widgets::CanBeStandalone(e.id);
}

size_t App::AddWidgetWindow(DockItemEntry entry, size_t nearDock, bool open) {
    // Round 16 phase 16E: from a config item (a widget dragged off a dock keeps its entry: settings,
    // name). Only the dock-placement keys go: a standalone entry has no anchor / offset of its own.
    const std::wstring id = entry.id;
    if (shutDown_ || modalDepth_ > 0 || entry.Kind() != ItemKind::Widget || !Widgets::ValidId(id)) return SIZE_MAX;
    std::wstring error;
    const auto pkg = Widgets::Load(id, error);
    if (!pkg || !pkg->manifest.standalone.supported) return SIZE_MAX;  // round 16 phase 16E: needs a dock
    DockConfig d;
    d.dockKind = DockKind::Widget;
    d.id = config_.UniqueDockId(id);
    entry.anchor = ItemAnchor::None;
    entry.offset = 0;
    d.items = {std::move(entry)};
    pkg->manifest.DefaultStandaloneSize(d.widgetWindow.w, d.widgetWindow.h);
    d.layer = DockLayer::Behind;
    // The display of the dock it was added from ("each": the primary), centred on its work area.
    d.monitor = 0;
    if (nearDock < config_.docks.size() && !config_.docks[nearDock].Each()) {
        const int m = docks_.MonitorIndexFor(config_.docks[nearDock]);
        d.monitor = m >= 0 ? m : 0;
    }
    if (widgetAt_) {  // round 12 phase 12D: dropped on the desktop: the display under that point
        const HMONITOR under = MonitorFromPoint(*widgetAt_, MONITOR_DEFAULTTONEAREST);
        const std::vector<HMONITOR>& all = docks_.Monitors();
        for (size_t m = 0; m < all.size(); ++m) {
            if (all[m] == under) d.monitor = static_cast<int>(m);
        }
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
        if (widgetAt_) {  // round 12 phase 12D: centred on the drop point (DIPs from the work area's top-left)
            n.x = std::clamp(static_cast<int>(std::lround((widgetAt_->x - mi.rcWork.left) / s)) - n.widgetWindow.w / 2, 0,
                             std::max(0, workW - n.widgetWindow.w));
            n.y = std::clamp(static_cast<int>(std::lround((widgetAt_->y - mi.rcWork.top) / s)) - n.widgetWindow.h / 2, 0,
                             std::max(0, workH - n.widgetWindow.h));
        }
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
    // Round 16 phase 16G: Move to dock ▸ one entry per enabled dock (not the folder windows and
    // standalone widgets); a locked dock's entry is greyed (MoveWidgetToDock refuses it); none at
    // all: the submenu is greyed. The entries carry the dock ids in `moveTargets` (by index below).
    std::vector<std::wstring> moveTargets(config_.docks.size());
    if (HMENU docksMenu = CreatePopupMenu()) {
        UINT count = 0;
        for (size_t k = 0; k < config_.docks.size() && k < 200; ++k) {
            const DockConfig& d = config_.docks[k];
            if (!d.IsDock() || !d.enabled) continue;
            moveTargets[k] = d.id;
            AppendMenuW(docksMenu, MF_STRING | (d.locked ? MF_GRAYED : 0), ContextMenu::kMoveToDockBase + static_cast<UINT>(k),
                        (d.DisplayName() + (d.locked ? L" (locked)" : L"")).c_str());
            ++count;
        }
        AppendMenuW(menu, MF_POPUP | (count == 0 ? MF_GRAYED : 0), reinterpret_cast<UINT_PTR>(docksMenu), L"Move to dock");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    // Round 10 phase 10A: "App settings…" (was "Global settings…"), removal last.
    AppendMenuW(menu, MF_STRING, ContextMenu::kOpenSettings, L"App settings…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ContextMenu::kWidgetRemove, L"Remove from desktop");
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
        OpenDockSettings(now, SettingsIds::kTabWidgetWindow);  // round 8 phase 8C: its Window tab
    } else if (cmd == ContextMenu::kOpenSettings) {
        OpenSettings();
    } else if (cmd >= ContextMenu::kMoveToDockBase && cmd < ContextMenu::kMoveToDockBase + moveTargets.size()) {
        // Round 16 phase 16G: that dock by id (posted: this runs inside the widget window's
        // WM_RBUTTONUP, and the move destroys that window).
        const size_t dock = DockIndexOf(moveTargets[cmd - ContextMenu::kMoveToDockBase]);
        if (dock == SIZE_MAX) {
            MessageBeep(MB_ICONWARNING);
        } else {
            RequestWidgetToDock(now, dock, SIZE_MAX);  // beeps when refused
        }
    } else {
        WidgetEntryCommand(cmd, now);
    }
}

LRESULT App::WidgetEntryCommand(UINT cmd, size_t cfg, size_t arg) {
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
        case ContextMenu::kWidgetToDock:  // round 16 phase 16G: to the end of config dock `arg` (posted)
            return RequestWidgetToDock(cfg, arg, SIZE_MAX) ? 1 : 0;
        case ContextMenu::kEditorOk:
        case ContextMenu::kEditorCancel: {  // round 6 phase 6C: its widget's open text editor
            WidgetWindow* w = docks_.WidgetOf(cfg);
            return w && w->EditorCommand(cmd == ContextMenu::kEditorOk) ? 1 : 0;
        }
        default:
            return -1;
    }
}

void App::OnSettingsChanged(SettingsScope scope, size_t cfg, SettingsWindow* source) {
    if (shutDown_) return;
    if (scope == SettingsScope::WindowSize) {
        RequestSave();  // round 8 phase 8A: config.settings.windows; nothing else changes
        return;
    }
    if (cfg >= config_.docks.size()) cfg = 0;
    // An Item window edited its own item: what it is now is what to find again.
    if (source && source->Kind() == SettingsKind::Item) {
        if (const DockItemEntry* e = source->TargetItem()) source->SetItemCopy(*e, source->Dock().items.size());
    }
    bool resorted = false;
    switch (scope) {
        case SettingsScope::Appearance: {
            // Sizes, position, monitor, theme, clock: the theme may be per dock, the monitor
            // decides which windows exist.
            config_.ResolveDockThemes();
            // Round 7 phase 7B: a new align moves the items without an anchor to its group (the
            // source window's Contents strip then shows the new order too).
            resorted = config_.docks[cfg].SortByGroup();
            if (modalDepth_ == 0) {
                SyncDocks();  // folder windows and standalone widgets apply their entries in Sync
            } else {
                for (size_t k = 0; k < config_.docks.size(); ++k) docks_.ApplyWidgetWindow(k);
            }
            docks_.ForEach([](DockWindow& w) { w.ApplyConfig(); });
            break;
        }
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
        case SettingsScope::Fullscreen:
            docks_.SyncFullscreenGuard();  // round 9 phase 9A: the watch and an evaluation
            break;
        case SettingsScope::SaveOnly:
            docks_.ApplyWidgetWindow(cfg);  // a standalone widget's lock (its header button)
            break;
        case SettingsScope::WindowSize:  // handled above
            break;
    }
    RequestSave();
    // The other windows follow (their pages, headers; a removed item's window closes); the source
    // window keeps its controls (a half-typed value stays) and updates its header.
    RefreshSettings(source);
    if (source && resorted) source->Refresh();
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
            if (!w || IsSearchHidden(w)) return 0;  // round 9 phase 9B: one made hidden for a search is not open
            return arg == 1 ? reinterpret_cast<LRESULT>(w->Hwnd()) : 1;
        }
        case 229:    // round 5 phase C, the Dock window's Contents tab: a strip tile's centre (item LOWORD(lParam))
        case 230:    // a drawer row's centre (entry LOWORD(lParam))
        case 231:    // the strip's selected item
        case 232:    // the drawer is shown
        case 233:    // the strip's insertion marker (-1 none)
        case 277:    // round 8 phase 8B: the Position tab's placement kind (0 bar on an edge, 1 floating bar, 2 half circle, 3 circle)
        case 278:    // ... the Contents drawer's row count of group LOWORD (0 apps and folders, 1 built in, 2 script widgets)
        case 289:    // round 10 phase 10B: the Contents inspector's common-setting rows for the selection
        case 290:    // ... the control id of row LOWORD (its input; a read-only value's static; -1 none)
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
            return w && !IsSearchHidden(w) ? reinterpret_cast<LRESULT>(w->Hwnd()) : 0;
        }
        case 282:    // round 9 phase 9B: a search dialog is open for the Dock window of dock HIWORD (LOWORD 1: the Global window's): 1 / 0
        case 283: {  // ... its result count (the rows shown; -2: none open)
            SettingsWindow* w = arg == 1 ? FindSettings(SettingsKind::Global)
                                : inst && inst->cfg < config_.docks.size() ? FindSettings(SettingsKind::Dock, config_.docks[inst->cfg].id)
                                                                           : nullptr;
            SettingsSearch* s = SearchOf(w);
            if (wp == 282) return s ? 1 : 0;
            return s ? s->ResultCount() : -2;
        }
        case 264:    // round 6 phase 6E: the Dock window's drawer: entry LOWORD's preview size (MAKELONG(w, h) px; 0 none)
        case 267:    // ... its render time in microseconds (-1: not rendered)
        case 268: {  // the Contents tab's inspector shows a widget preview (1 / 0); -2 for each: no Dock window of that dock
            SettingsWindow* w = inst && inst->cfg < config_.docks.size() ? FindSettings(SettingsKind::Dock, config_.docks[inst->cfg].id) : nullptr;
            LRESULT out = -2;
            if (w && !w->QueryPages(static_cast<int>(wp), arg, out)) out = -2;
            return out;
        }
        case 263:    // round 6 phase 6E: previews rendered by the Dock window of dock HIWORD (LOWORD 1: by the Global window)
        case 265: {  // widget ids whose preview failed there; -2: no such window
            SettingsWindow* w = arg == 1 ? FindSettings(SettingsKind::Global)
                                : inst && inst->cfg < config_.docks.size() ? FindSettings(SettingsKind::Dock, config_.docks[inst->cfg].id)
                                                                           : nullptr;
            if (!w || !w->Hwnd()) return -2;
            return wp == 263 ? w->PreviewsRendered() : w->PreviewFailures();
        }
        case 275: {  // round 8 phase 8A: a settings window's client size MAKELONG(w, h) DIPs (LOWORD 1: the Global window, else the Dock window of dock HIWORD; -2 not open)
            SettingsWindow* w = arg == 1 ? FindSettings(SettingsKind::Global)
                                : inst && inst->cfg < config_.docks.size() ? FindSettings(SettingsKind::Dock, config_.docks[inst->cfg].id)
                                                                           : nullptr;
            if (!w || !w->Hwnd()) return -2;
            const SIZE s = w->ClientSizeDip();
            return static_cast<LRESULT>(MAKELONG(static_cast<WORD>(s.cx), static_cast<WORD>(s.cy)));
        }
        case 276: {  // 1 while expander LOWORD (its control id) is open: the Dock window of dock HIWORD, its Item windows, the Global window (-1 no such expander)
            std::vector<SettingsWindow*> look;
            if (inst && inst->cfg < config_.docks.size()) {
                const std::wstring& id = config_.docks[inst->cfg].id;
                if (SettingsWindow* d = FindSettings(SettingsKind::Dock, id)) look.push_back(d);
                for (const auto& s : settings_) {
                    if (s->Kind() == SettingsKind::Item && s->DockId() == id) look.push_back(s.get());
                }
            }
            if (SettingsWindow* g = FindSettings(SettingsKind::Global)) look.push_back(g);
            for (SettingsWindow* w : look) {
                if (!w->Hwnd()) continue;
                if (const int state = w->ExpanderState(static_cast<int>(arg)); state >= 0) return state;
            }
            return -1;
        }
        case 266:  // round 6 phase 6E: live widget instances in the runtime (previews never count: none outlives its render)
            return static_cast<LRESULT>(docks_.Widgets().Instances().size()) + docks_.Previews().Alive();
        case 291:    // round 10 phase 10C: the tabs of the Item window of item LOWORD of dock HIWORD (LOWORD 0xFFFF: of its Dock window); 0 no tab strip, -2 not open
        case 292: {  // ... the selected tab's place in the strip (0 the first; -1 no tab strip), -2 not open
            SettingsWindow* w = nullptr;
            if (inst && inst->cfg < config_.docks.size()) {
                const std::wstring& id = config_.docks[inst->cfg].id;
                w = arg == 0xFFFF ? FindSettings(SettingsKind::Dock, id) : arg >= 0 ? FindSettings(SettingsKind::Item, id, static_cast<size_t>(arg)) : nullptr;
            }
            if (!w || !w->Hwnd()) return -2;
            if (wp == 291) return w->TabCount();
            return w->TabCount() > 0 ? w->NavDisplayIndex(w->ActivePageIndex()) : -1;
        }
        case 279: {  // round 8 phase 8D: the Global window's nav row of the active page (0 General, 1 Docks and displays, ... 8 About; -2 no Global window)
            SettingsWindow* w = FindSettings(SettingsKind::Global);
            if (!w || !w->Hwnd()) return -2;
            return w->NavDisplayIndex(w->ActivePageIndex());
        }
        case 269: {  // round 6 phase 6E: Global > Widget library's preview panel: its bitmap size (MAKELONG px; 0 none; -2 no Global window)
            SettingsWindow* w = FindSettings(SettingsKind::Global);
            LRESULT out = -2;
            if (w && !w->QueryPages(269, arg, out)) out = -2;
            return out;
        }
        case 284:  // round 9 phase 9C: a preset / welcome chooser is open: 1 (LOWORD 2: its variant 0 apply, 1 welcome, 2 new dock; 3: the selected preset); -2 none
            if (!presets_ || !presets_->Hwnd()) return -2;
            if (arg == 2) return static_cast<LRESULT>(presets_->Mode());
            if (arg == 3) return presets_->Selected();
            return 1;
        case 285:  // round 9 phase 9D: inputs without a tooltip / hint or a label (LOWORD bit 0: the Global window, bit 1: with the Item windows, bit 2: the id of gap LOWORD >> 8 instead; bit 3: cards without a "?" instead)
            return CountUnexplained(lp);
        case 286:  // round 9 phase 9D: the Help window is shown (1 / 0)
            return help_ && help_->Hwnd() && IsWindowVisible(help_->Hwnd()) ? 1 : 0;
        case 287:  // ... its guide (0 quick start ... 5 the index; -1 none)
            return help_ && help_->Hwnd() ? help_->Guide() : -1;
        case 295:  // round 11 phase 11D: docks[] entry HIWORD (any kind) is enabled (1) / disabled (0); -2 no such entry
            if (!inst || inst->cfg >= config_.docks.size()) return -2;
            return config_.docks[inst->cfg].enabled ? 1 : 0;
        // Round 12 phase 12A: the timeline (the change just made is recorded first).
        case 296:  // undo depth (entries Undo can undo)
            FlushTimeline();
            return static_cast<LRESULT>(undo_.size());
        case 297:  // redo depth
            FlushTimeline();
            return static_cast<LRESULT>(redo_.size());
        case 298:  // the save: 0 saved, 1 pending (or held by a preview), 2 the last save failed
            FlushTimeline();
            return SaveStatus();
        case 299:  // a Keep / Cancel preview is open (1 / 0)
            FlushTimeline();
            return previewing_ ? 1 : 0;
        case 300:  // the bar: 0 none, 1 Keep / Revert, 2 Keep / Cancel, 3 a failed save (LOWORD 1: the banner is shown, 1 / 0)
            FlushTimeline();
            if (arg == 1) return banner_.Hwnd() && IsWindowVisible(banner_.Hwnd()) ? 1 : 0;
            return BarKind();
        case 301:    // round 12 phase 12B: the theme gallery's selected tile (LOWORD 1: App settings > Themes, else the Look tab of the Dock window of dock HIWORD)
        case 302: {  // ... its tile count; -2 for each: no such window
            SettingsWindow* w = arg == 1 ? FindSettings(SettingsKind::Global)
                                : inst && inst->cfg < config_.docks.size() ? FindSettings(SettingsKind::Dock, config_.docks[inst->cfg].id)
                                                                           : nullptr;
            LRESULT out = -2;
            if (!w || !w->Hwnd() || !w->QueryPages(static_cast<int>(wp), arg, out)) out = -2;
            return out;
        }
        // --- round 12 phase 12C ---
        case 303: {  // docks[] entry HIWORD's display name: written into the app window's hidden static 5690; its length (-2 no such entry)
            if (!inst || inst->cfg >= config_.docks.size()) return -2;
            const std::wstring name = config_.DockDisplayName(inst->cfg);
            if (nameText_) SetWindowTextW(nameText_, name.c_str());
            return static_cast<LRESULT>(name.size());
        }
        case 304:  // the open chooser's kind: 0 Layouts, 1 Looks, 2 Mine (-2 none open)
            return presets_ && presets_->Hwnd() ? static_cast<LRESULT>(presets_->Kind()) : -2;
        case 305:  // dock HIWORD's titleShow: 0 off, 1 on hover, 2 always (-2 no such dock)
        case 306:  // ... its titlePlace: 0 outside, 1 inside
            if (!inst || inst->cfg >= config_.docks.size() || !config_.docks[inst->cfg].IsDock()) return -2;
            return wp == 305 ? static_cast<LRESULT>(config_.docks[inst->cfg].titleShow) : static_cast<LRESULT>(config_.docks[inst->cfg].titlePlace);
        // --- end round 12 phase 12C ---
        case 317:  // round 12 phase 12G: Quick setup's step, 318 the tip shown, 319 tipsShown, 320 the held choices
        case 318:
        case 319:
        case 320:
            return QuickSetupState(wp, lp);
        // --- round 12 phase 12F: the settings window on a large display ---
        case 321:    // the active page's layout: 1 / 2 columns (LOWORD 0xFFFE the App window, 0xFFFF the Dock settings of dock HIWORD, else the Item window of item LOWORD of dock HIWORD); -2 not open
        case 323: {  // ... the active page's sentence (control 5800, its window text): its length (0 none); -2 not open
            SettingsWindow* w = nullptr;
            if (arg == 0xFFFE) {
                w = FindSettings(SettingsKind::Global);
            } else if (inst && inst->cfg < config_.docks.size()) {
                const std::wstring& id = config_.docks[inst->cfg].id;
                w = arg == 0xFFFF ? FindSettings(SettingsKind::Dock, id) : arg >= 0 ? FindSettings(SettingsKind::Item, id, static_cast<size_t>(arg)) : nullptr;
            }
            if (!w || !w->Hwnd()) return -2;
            if (wp == 323) return static_cast<LRESULT>(w->LeadText().size());
            layoutQueried_ = w->Hwnd();
            return w->PageColumns();
        }
        case 322: {  // the column of control LOWORD on the active page of the window 321 last answered for: 0 left (or one column), 1 right, 2 across both; -1 no such control, -2 that window is gone
            for (const auto& s : settings_) {
                if (s->Hwnd() && s->Hwnd() == layoutQueried_) return s->ControlColumn(static_cast<int>(arg));
            }
            return -2;
        }
        // --- end round 12 phase 12F ---
        case 280:  // round 9 phase 9A: dock HIWORD (a dock or a standalone widget) is hidden by the full-screen guard (1 / 0; -2 no such dock)
            if (!inst) return -2;
            if (inst->window) return inst->window->FullscreenHidden() ? 1 : 0;
            if (inst->widget) return inst->widget->FullscreenHidden() ? 1 : 0;
            return 0;
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
        case 257:  // round 6 phase 6D: the perf sampler runs (1 / 0)
            return docks_.Widgets().Perf().Active() ? 1 : 0;
        case 258:  // widgets using the perf provider
            return static_cast<LRESULT>(docks_.Widgets().PerfUsers());
        case 259:  // perf snapshots received since the dock started
            return static_cast<LRESULT>(docks_.Widgets().Perf().Snapshots());
        case 260:  // the sampler's interval in ms (0: not running)
            return docks_.Widgets().Perf().Active() ? static_cast<LRESULT>(docks_.Widgets().Perf().Interval()) : 0;
        case 261:  // metric keys in the latest snapshot (0: none / not running)
            return static_cast<LRESULT>(docks_.Widgets().Perf().MetricCount());
        case 262:  // HWiNFO's shared memory is readable and fresh (1 / 0)
            return docks_.Widgets().Perf().Hwinfo().available ? 1 : 0;
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
        // --- round 12 phase 12D: customize mode (dock HIWORD(lParam); -1 no such dock window) ---
        case 307:  // the dock is in customize mode (1 / 0)
            return inst && inst->window ? (inst->window->Editing() ? 1 : 0) : -1;
        case 308:  // its hovered item's remove badge is shown (1 / 0)
            return inst && inst->window ? (inst->window->EditBadgeSlot() >= 0 ? 1 : 0) : -1;
        case 309:  // its "+" slots laid out (0 - 2)
            return inst && inst->window ? inst->window->PlusCount() : -1;
        case 310:  // the edit bar is shown (any dock)
            return editBar_.Shown() ? 1 : 0;
        case 311:  // the drawer popup is shown (any dock)
            return drawerPopup_.Shown() ? 1 : 0;
        case 312:  // its tiles after the search and the chip (-1: not shown)
            return drawerPopup_.Shown() ? drawerPopup_.TileCount() : -1;
        // --- round 12 phase 12E ---
        // 313: the Dock settings window of dock HIWORD: 0 none open, 1 a panel beside the dock (2 a
        // captioned window: folder window / standalone widget entries until round 12 phase 12I;
        // never now, every Dock window is the panel, beside the folder window / widget).
        case 313:
        case 314: {  // ... the panel's side: 0 above the dock (window), 1 below it, 2 right of it, 3 left of it (-1: no panel)
            SettingsWindow* w = inst && inst->cfg < config_.docks.size() ? FindSettings(SettingsKind::Dock, config_.docks[inst->cfg].id) : nullptr;
            const bool open = w && w->Hwnd() && !IsSearchHidden(w) && IsWindowVisible(w->Hwnd());
            if (wp == 313) return !open ? 0 : w->IsPanel() ? 1 : 2;
            return open && w->IsPanel() ? w->PanelSide() : -1;
        }
        case 315:  // dock HIWORD's docks[].edgeGap (DIPs; -2 not a dock)
            if (!inst || inst->cfg >= config_.docks.size() || !config_.docks[inst->cfg].IsDock()) return -2;
            return config_.docks[inst->cfg].edgeGap;
        case 316:  // 1 while a drag of the whole dock is in progress (Ctrl+drag, customize mode's drag; -1 no such dock window)
            return inst && inst->window ? (inst->window->DockDragging() ? 1 : 0) : -1;
        // --- end round 12 phase 12E ---
        case 325:  // round 13 phase 13C: the quick entries of slot LOWORD(lParam) of dock HIWORD (-1 no such dock window)
            return inst && inst->window ? static_cast<LRESULT>(QuickEntries(*inst->window, static_cast<int>(arg)).size()) : -1;
        case 328:  // round 14 phase 14A: the resting bar's length | thickness << 16 (px; DockWindow), -2 not a dock
        case 330:  // round 14 phase 14D: a thumbnail tile's along | across << 16 (px; 0 no tile; DockWindow), -2 not a dock
            if (!inst || !inst->window) return -2;
            return inst->window->QueryState(wp, arg);
        case 329:  // round 14 phase 14B: settings.snapToScreen (0 / 1; app-wide)
            return config_.settings.snapToScreen ? 1 : 0;
        case 331:  // round 16 phase 16A: the Start menu placer: 0 no host window, 1 at its natural place, 2 shifted
            return startPlacer_.State();
        case 332:  // round 16 phase 16A: the last shift Place computed (a dry run too): MAKELONG(dx, dy), 0 before any
            return startPlacer_.LastShift();
        case 333: {  // round 18 phase 18A: the Add a dock gallery's tiles (LOWORD 1: the selected tile; 2: the canvas's ghost tile, -1 none); -2 no App settings window
            SettingsWindow* w = FindSettings(SettingsKind::Global);
            LRESULT out = -2;
            if (!w || !w->Hwnd() || !w->QueryPages(333, arg, out)) out = -2;
            return out;
        }
        default:
            break;
    }
    if (inst && inst->folder) return wp >= 185 ? inst->folder->QueryState(wp, arg) : -1;
    if (inst && inst->widget) return inst->widget->QueryState(wp, arg);  // a standalone widget (6B)
    if (inst && inst->isWidget) return -1;  // parked
    if (!inst || !inst->window) return -1;
    return inst->window->QueryState(wp, arg);
}

// Round 12 phase 12B: DockCommand 162 - 164, the look commands of the editors' ⋯ menu without their
// dialogs (ContextMenu.h). The source look: HIWORD(lParam) the dock by instance index, 0xFFFF the app's.
bool App::LookCommand(UINT command, LPARAM lp) {
    if (shutDown_ || modalDepth_ > 0) return false;
    const size_t instance = DockOfParam(lp);
    DockConfig* source = nullptr;
    if (instance != 0xFFFF) {
        const DockManager::Instance* inst = docks_.At(instance);
        if (!inst || inst->cfg >= config_.docks.size() || !config_.docks[inst->cfg].IsDock()) return false;
        source = &config_.docks[inst->cfg];
    }
    wchar_t buf[256]{};
    switch (command) {
        case ContextMenu::kSaveLookAsTheme: {
            if (!GlobalGetAtomNameW(static_cast<ATOM>(lp & 0xFFFF), buf, static_cast<int>(std::size(buf)))) return false;
            const std::wstring name = buf;
            std::wstring stem;
            if (Theme::SaveAs(name, source ? config_.ThemeOf(*source) : config_.theme, true, stem) != 0) return false;
            // As the menu's: the look's preset becomes the file (the values are the same); a dock
            // that uses the app look keeps it (the file only).
            if (!source || source->OwnLook()) {
                NoteLabel(L"Saved the look as theme \x201C" + name + L"\x201D");
                if (source) {
                    source->themePreset = stem;
                    source->themeOverrides = 0;
                } else {
                    config_.themePreset = stem;
                    config_.themeOverrides = 0;
                }
                config_.ResolveTheme();
                docks_.ForEach([](DockWindow& w) { w.ApplyConfig(); });
                RequestSave();
            }
            RefreshSettings();
            return true;
        }
        case ContextMenu::kImportThemeFile: {
            if (!GlobalGetAtomNameW(static_cast<ATOM>(lp & 0xFFFF), buf, static_cast<int>(std::size(buf)))) return false;
            std::wstring stem;
            const DWORD error = Theme::Import(buf, stem);
            if (error != 0) return false;
            RefreshSettings();
            return true;
        }
        case ContextMenu::kApplyLookToDocks: {
            const unsigned mask = static_cast<unsigned>(lp & 0xFFFF);
            std::vector<size_t> targets;
            for (size_t k = 0; k < config_.docks.size() && k < 16; ++k) {
                if ((mask & (1u << k)) && config_.docks[k].IsDock() && &config_.docks[k] != source) targets.push_back(k);
            }
            if (targets.empty()) return false;
            NoteLabel(targets.size() == 1 ? L"Applied the look to " + config_.docks[targets[0]].DisplayName()
                                          : L"Applied the look to " + std::to_wstring(targets.size()) + L" docks");
            for (size_t k : targets) config_.CopyLook(source, config_.docks[k]);
            config_.ResolveDockThemes();
            docks_.ForEach([](DockWindow& w) { w.ApplyConfig(); });
            RequestSave();
            RefreshSettings();
            return true;
        }
        default:
            return false;
    }
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
    if (msg == msgWelcome_) {  // round 9 phase 9C: `WinDock.exe --welcome` (round 12 phase 12G: Quick setup)
        OpenQuickSetup(false);
        return true;
    }
    // --- round 12 phase 12G: DockCommand 180 - 183 (Quick setup, the tips) ---
    if (msg == msgDockCommand_ && wp >= ContextMenu::kQuickSetup && wp <= ContextMenu::kSetupFinish) {
        result = QuickSetupCommand(static_cast<UINT>(wp), lp) ? 1 : 0;
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
    if (msg == msgDockCommand_ && wp == ContextMenu::kSetDockEnabled) {
        // Round 11 phase 11D: entry HIWORD(lParam) disabled (LOWORD 0), enabled (1) or toggled (2).
        // 1 done (also when it already was), 0 refused (no such entry, the last enabled dock).
        DockManager::Instance* inst = docks_.At(DockOfParam(lp));
        const LPARAM mode = lp & 0xFFFF;
        result = 0;
        if (inst && inst->cfg < config_.docks.size() && mode <= 2) {
            const bool on = config_.docks[inst->cfg].enabled;
            result = (mode == 2 || (mode == 1) != on) ? (EditDocks(SettingsDockEdit::ToggleEnabled, inst->cfg) ? 1 : 0) : 1;
        }
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
    if (msg == msgDockCommand_ && wp == ContextMenu::kFullscreenGuard) {
        // Round 9 phase 9A: LOWORD(lParam) 1 / 0 forces the full-screen guard on / off for every
        // monitor, 2 releases it (a real evaluation). 0: the guard is off (settings.fullscreen "ignore").
        const LPARAM mode = lp & 0xFFFF;
        result = docks_.FullscreenGuardOn() && mode <= 2 ? 1 : 0;
        if (result) docks_.ForceFullscreen(static_cast<int>(mode));
        return true;
    }
    if (msg == msgDockCommand_ && wp == ContextMenu::kFindSetting) {
        // Round 9 phase 9B: the settings search of the Dock window of dock HIWORD(lParam) (LOWORD 1:
        // of the Global window), that window opened first. 1: the dialog is open.
        SettingsWindow* w = nullptr;
        if ((lp & 0xFFFF) == 1) {
            OpenSettings();
            w = FindSettings(SettingsKind::Global);
        } else if (DockManager::Instance* inst = docks_.At(DockOfParam(lp)); inst && inst->cfg < config_.docks.size()) {
            OpenDockSettings(inst->cfg);
            w = FindSettings(SettingsKind::Dock, config_.docks[inst->cfg].id);
        }
        if (w) OpenSearch(w, L"");
        result = SearchOf(w) ? 1 : 0;
        return true;
    }
    if (msg == msgDockCommand_ && wp == ContextMenu::kPresetDialog) {
        // Round 9 phase 9C: the preset chooser for dock HIWORD(lParam); LOWORD 1 the welcome variant,
        // 2 the "new dock" one, else apply to that dock. Owned by the dock's (or for a new dock the
        // Global) settings window when it is open. Not modal for the sender: 1 when it is open.
        // Round 12 phase 12C: LOWORD 3 the new dock variant on Mine, 4 apply on Looks, 5 apply on
        // Mine, 6 the app look (Looks only, owned by App settings when it is shown).
        const LPARAM variant = lp & 0xFFFF;
        if (variant == 1) {  // round 12 phase 12G: the welcome variant is Quick setup now
            OpenQuickSetup(false);
            result = QuickSetupOpen() ? 1 : 0;
            return true;
        }
        const PresetMode mode = variant == 1                   ? PresetMode::Welcome
                                : variant == 2 || variant == 3 ? PresetMode::NewDock
                                : variant == 6                 ? PresetMode::AppLook
                                                               : PresetMode::Apply;
        const PresetKind kind = variant == 3 || variant == 5 ? PresetKind::Mine
                                : variant == 4 || variant == 6 ? PresetKind::Looks
                                                               : PresetKind::Layouts;
        DockManager::Instance* inst = docks_.At(DockOfParam(lp));
        const size_t cfg = inst ? inst->cfg : SIZE_MAX;
        SettingsWindow* w = mode == PresetMode::NewDock || mode == PresetMode::AppLook ? FindSettings(SettingsKind::Global)
                            : mode == PresetMode::Apply && cfg < config_.docks.size() ? FindSettings(SettingsKind::Dock, config_.docks[cfg].id)
                                                                                     : nullptr;
        const HWND owner = w && w->Hwnd() && IsWindowVisible(w->Hwnd()) ? w->Hwnd() : nullptr;
        OpenPresets(mode, cfg, owner, kind);
        result = presets_ && presets_->Hwnd() ? 1 : 0;
        return true;
    }
    if (msg == msgDockCommand_ && wp == ContextMenu::kApplyPreset) {
        // Preset LOWORD(lParam) & 0xFF (its index in the list) applied to dock HIWORD(lParam); bit 8:
        // keep the dock's pins at the preset's marker. 1 applied, 0 refused.
        DockManager::Instance* inst = docks_.At(DockOfParam(lp));
        // Round 12 phase 12A: a Keep / Cancel preview, as from the chooser (DockCommand 160 / 161 end
        // it); hosted by the dock's Dock window when it is shown, else the banner near the dock.
        SettingsWindow* w = inst && inst->cfg < config_.docks.size() ? FindSettings(SettingsKind::Dock, config_.docks[inst->cfg].id) : nullptr;
        const HWND host = w && IsWindowVisible(w->Hwnd()) ? w->Hwnd() : nullptr;
        result = inst && ApplyPreset(static_cast<size_t>(lp & 0xFF), inst->cfg, (lp & 0x100) != 0, true, host) ? 1 : 0;
        return true;
    }
    if (msg == msgDockCommand_ && wp >= ContextMenu::kSaveLookAsTheme && wp <= ContextMenu::kApplyLookToDocks) {
        result = LookCommand(static_cast<UINT>(wp), lp) ? 1 : 0;
        return true;
    }
    // --- round 12 phase 12C: DockCommand 165 - 169 (rename, duplicate, saved layout, look, save as layout) ---
    if (msg == msgDockCommand_ && wp >= ContextMenu::kRenameDock && wp <= ContextMenu::kSaveDockLayout) {
        result = KeepCommand(static_cast<UINT>(wp), lp) ? 1 : 0;
        return true;
    }
    if (msg == msgDockCommand_ && wp >= ContextMenu::kUndo && wp <= ContextMenu::kBarCancel) {
        // Round 12 phase 12A: the timeline. 157 Undo, 158 Redo, 159 Revert this window (the Dock window
        // of dock HIWORD(lParam); LOWORD 1: the App settings window), 160 Keep, 161 Revert / Cancel the
        // open bar. 1 done, 0 nothing to do (or refused).
        bool done = false;
        switch (wp) {
            case ContextMenu::kUndo: done = Undo(); break;
            case ContextMenu::kRedo: done = Redo(); break;
            case ContextMenu::kRevertWindow: {
                SettingsWindow* w = nullptr;
                if ((lp & 0xFFFF) == 1) {
                    w = FindSettings(SettingsKind::Global);
                } else if (DockManager::Instance* inst = docks_.At(DockOfParam(lp)); inst && inst->cfg < config_.docks.size()) {
                    w = FindSettings(SettingsKind::Dock, config_.docks[inst->cfg].id);
                }
                done = w && RevertWindow(w);
                break;
            }
            case ContextMenu::kBarKeep: done = KeepBar(); break;
            case ContextMenu::kBarCancel: done = CancelBar(); break;
            default: break;
        }
        result = done ? 1 : 0;
        return true;
    }
    if (msg == msgDockCommand_ && (wp == ContextMenu::kCustomize || wp == ContextMenu::kCustomizeDrawer)) {
        // Round 12 phase 12D: customize mode on dock HIWORD(lParam) (170: LOWORD 1 enter, 0 leave, 2
        // toggle; a locked dock's question is modal: posted when sent), its drawer popup (171: 1 show,
        // 0 hide, 2 toggle; only while that dock is customized). 1 done (also when already so), 0 refused.
        DockWindow* dock = docks_.WindowAt(DockOfParam(lp));
        const LPARAM mode = lp & 0xFFFF;
        result = 0;
        if (!dock || mode > 2) return true;
        if (wp == ContextMenu::kCustomize) {
            const bool on = editDock_ == dock;
            const bool want = mode == 2 ? !on : mode == 1;
            if (want == on) {
                result = 1;
            } else if (!want) {
                LeaveEdit();
                result = 1;
            } else if (dock->ConfigIndex() < config_.docks.size() && config_.docks[dock->ConfigIndex()].locked && InSendMessage()) {
                PostMessageW(hwnd_, msg, wp, (lp & ~static_cast<LPARAM>(0xFFFF)) | 1);  // the question after the call
                result = 1;
            } else {
                result = EnterEdit(dock) ? 1 : 0;
            }
        } else if (editDock_ == dock) {
            const bool shown = drawerPopup_.Shown();
            const bool want = mode == 2 ? !shown : mode == 1;
            if (want != shown) ShowDrawer(want);
            result = 1;
        }
        return true;
    }
    // --- round 12 phase 12E: DockCommand 176 close the panel, 177 / 178 the edge gap / offset ---
    // Round 14 phase 14A: 184 / 185 the bar's fixed length / thickness, the same way.
    if (msg == msgDockCommand_ && ((wp >= ContextMenu::kClosePanel && wp <= ContextMenu::kSetDockOffset) ||
                                   wp == ContextMenu::kSetDockLength || wp == ContextMenu::kSetDockThickness)) {
        result = PanelCommand(static_cast<UINT>(wp), lp) ? 1 : 0;
        return true;
    }
    // Round 14 phase 14B: DockCommand 186 settings.snapToScreen = LOWORD(lParam) != 0, as Global >
    // General's check box writes it (one undo entry; read at drag time, nothing to re-apply).
    if (msg == msgDockCommand_ && wp == ContextMenu::kSetSnapToScreen) {
        const bool on = (lp & 0xFFFF) != 0;
        if (!shutDown_ && config_.settings.snapToScreen != on) {
            NoteLabel(on ? L"Snap to the screen's corners and centre: On" : L"Snap to the screen's corners and centre: Off");
            config_.settings.snapToScreen = on;
            RequestSave();
            RefreshSettings();
        }
        result = 1;
        return true;
    }
    // Round 18 phase 18A: DockCommand 187 the App settings window's Add a dock tile LOWORD(lParam),
    // chosen as a click would (the gallery's kMsgChoose: the page's own path, refusals included).
    if (msg == msgDockCommand_ && wp == ContextMenu::kAddDockTile) {
        result = 0;
        SettingsWindow* g = FindSettings(SettingsKind::Global);
        if (!g || !g->Hwnd() || shutDown_) return true;
        struct Find {
            HWND found = nullptr;
            static BOOL CALLBACK Proc(HWND h, LPARAM lp) {
                auto* self = reinterpret_cast<Find*>(lp);
                if (GetDlgCtrlID(h) != SettingsIds::kAddDockGallery) return TRUE;
                self->found = h;
                return FALSE;
            }
        } find;
        EnumChildWindows(g->Hwnd(), &Find::Proc, reinterpret_cast<LPARAM>(&find));  // every descendant: the page's controls
        if (!find.found) return true;
        const uint64_t before = cardAdds_;
        SendMessageW(find.found, SettingsGallery::kMsgChoose, static_cast<WPARAM>(lp & 0xFFFF), 0);
        result = cardAdds_ != before ? 1 : 0;
        return true;
    }
    if (msg == msgItemCommand_ && (wp & 0xFFFF) >= ContextMenu::kClickPlus && (wp & 0xFFFF) <= ContextMenu::kEditRemove) {
        // Round 12 phase 12D, customize mode of dock HIWORD(lParam): 862 "+" slot LOWORD(lParam), 863
        // drawer entry LOWORD added at the end, 864 ... before config item HIWORD(wParam), 865 the
        // remove badge of slot LOWORD. 1 done, 0 refused (not customizing that dock, no such target).
        DockWindow* dock = docks_.WindowAt(DockOfParam(lp));
        const UINT cmd = static_cast<UINT>(wp & 0xFFFF);
        const int arg = static_cast<int>(ArgOfParam(lp));
        const size_t at = static_cast<size_t>((wp >> 16) & 0xFFFF);
        result = 0;
        if (!dock || dock != editDock_) return true;
        if ((cmd == ContextMenu::kAddTile || cmd == ContextMenu::kAddTileAt) &&
            (arg == StripUi::DrawerRefs::kApp || arg == StripUi::DrawerRefs::kFolder) && InSendMessage()) {
            PostMessageW(hwnd_, msg, wp, lp);  // their dialogs are modal: after the call
            result = 1;
            return true;
        }
        switch (cmd) {
            case ContextMenu::kClickPlus: result = dock->ClickPlus(arg) ? 1 : 0; break;
            case ContextMenu::kAddTile: result = AddDrawerEntry(arg, SIZE_MAX) ? 1 : 0; break;
            case ContextMenu::kAddTileAt: result = AddDrawerEntry(arg, at) ? 1 : 0; break;
            case ContextMenu::kEditRemove: result = dock->EditRemoveSlot(arg) ? 1 : 0; break;
            default: break;
        }
        return true;
    }
    if (msg == msgItemCommand_ && (wp & 0xFFFF) == ContextMenu::kQuickApply) {
        // Round 13 phase 13C: quick entry HIWORD(wParam) of slot LOWORD(lParam) of dock HIWORD(lParam),
        // applied as its menu entry is. 1 done, 0 no such slot / entry.
        DockWindow* dock = docks_.WindowAt(DockOfParam(lp));
        result = dock && ApplyQuick(*dock, static_cast<int>(ArgOfParam(lp)), static_cast<size_t>((wp >> 16) & 0xFFFF)) ? 1 : 0;
        return true;
    }
    if (msg == msgItemCommand_ && (wp & 0xFFFF) == ContextMenu::kWidgetToDesktop) {
        // Round 16 phase 16E: slot LOWORD(lParam) of dock HIWORD(lParam) moved to the desktop as a
        // drag off the dock would, centred on the work area of the dock window's monitor. 1 done,
        // 0 refused (no such slot, a locked dock, not a script widget, a dock-only widget).
        DockWindow* dock = docks_.WindowAt(DockOfParam(lp));
        const LPARAM slot = ArgOfParam(lp);
        result = 0;
        if (dock && slot >= 0 && slot < static_cast<LPARAM>(dock->Items().size())) {
            const DockItem& item = dock->Items()[static_cast<size_t>(slot)];
            std::optional<POINT> at;
            MONITORINFO mi{sizeof(mi)};
            if (dock->Hwnd() && GetMonitorInfoW(MonitorFromWindow(dock->Hwnd(), MONITOR_DEFAULTTONEAREST), &mi)) {
                at = POINT{(mi.rcWork.left + mi.rcWork.right) / 2, (mi.rcWork.top + mi.rcWork.bottom) / 2};
            }
            if (item.IsWidget()) result = MoveWidgetToDesktop(dock->ConfigIndex(), item.element, at) ? 1 : 0;
        }
        return true;
    }
    if (msg == msgDockCommand_ && wp == ContextMenu::kOpenHelp) {
        // Round 9 phase 9D: the Help window at guide LOWORD(lParam) (0 quick start, 1 settings, 2 docks,
        // 3 items, 4 troubleshooting, 5 the index). 1: it is shown.
        const int guide = static_cast<int>(lp & 0xFFFF);
        const auto& guides = HelpDoc::Guides();
        result = 0;
        if (guide >= 0 && guide < static_cast<int>(guides.size())) {
            OpenHelp(guides[static_cast<size_t>(guide)].file);
            result = help_ && help_->Hwnd() && IsWindowVisible(help_->Hwnd()) ? 1 : 0;
        }
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
        const bool known = inst && ((cmd >= ContextMenu::kAutoHide && cmd <= ContextMenu::kLastDockCommand) ||
                                    cmd == ContextMenu::kAddFromDrawer);
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
        result = WidgetEntryCommand(static_cast<UINT>(wp & 0xFFFF), docks_.At(DockOfParam(lp))->cfg, static_cast<size_t>((wp >> 16) & 0xFFFF));
        return true;
    }
    if (msg == msgItemCommand_ && ((wp & 0xFFFF) == ContextMenu::kEditorOk || (wp & 0xFFFF) == ContextMenu::kEditorCancel)) {
        // Round 6 phase 6C: the text editor of slot LOWORD(lParam)'s widget (0xFFFF: the open one).
        DockWindow* dock = docks_.WindowAt(DockOfParam(lp));
        result = dock && dock->EditorCommand(static_cast<int>(ArgOfParam(lp)), (wp & 0xFFFF) == ContextMenu::kEditorOk) ? 1 : 0;
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
        if (cmd == ContextMenu::kSetItemAnchor || cmd == ContextMenu::kSetItemOffset) {
            // Round 7 phase 7B: the slot's config item (the running apps: the running item).
            result = 0;
            if (index >= 0) {
                const size_t element = dock->Items()[static_cast<size_t>(index)].element;
                const size_t cfg = dock->ConfigIndex();
                if (cmd == ContextMenu::kSetItemAnchor && arg <= static_cast<size_t>(ItemAnchor::End)) {
                    SetItemAnchor(cfg, element, static_cast<ItemAnchor>(arg));
                    result = 1;
                } else if (cmd == ContextMenu::kSetItemOffset) {
                    SetItemOffset(cfg, element, static_cast<int16_t>(arg));
                    result = 1;
                }
            }
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
            // An item's own menu, or the dock menu (no item there). Round 10 phase 10A: a widget
            // slot (its element asks for the dock menu: RightClickAction::DockMenu) gets its own
            // item menu too, with the dock menu in its Dock submenu.
            int index = dock->MenuIndexAt(client);
            if (index < 0) {
                const int at = dock->IndexAt(client);
                if (at >= 0 && dock->Items()[static_cast<size_t>(at)].IsWidget()) index = at;
            }
            if (index >= 0) {
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
                ShowAppMenu(POINT{GET_X_LPARAM(wp), GET_Y_LPARAM(wp)});  // round 10 phase 10A: its own menu
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
    if (msg == kSearchClosed) {
        // Round 9 phase 9B: a search dialog closed (Esc, its owner closed). With the last one gone,
        // the windows made hidden for the index close too (those the user went to are shown: kept).
        std::erase_if(searches_, [](const std::unique_ptr<SettingsSearch>& s) { return !s->Hwnd(); });
        if (searches_.empty() && !searchHidden_.empty()) {
            const std::vector<SettingsWindow*> hidden = std::move(searchHidden_);
            searchHidden_.clear();
            for (SettingsWindow* w : hidden) {
                if (w->Hwnd() && !IsWindowVisible(w->Hwnd())) DestroyWindow(w->Hwnd());  // freed by kSettingsClosed
            }
        }
        return 0;
    }
    if (msg == kHelpClosed) {  // round 9 phase 9D
        if (help_ && !help_->Hwnd()) help_.reset();
        return 0;
    }
    if (msg == kPresetClosed) {  // round 9 phase 9C
        if (presets_ && !presets_->Hwnd()) presets_.reset();
        return 0;
    }
    if (msg == kTimelineCommit) {  // round 12 phase 12A: the gesture that just ran is one entry
        CommitTimeline();
        return 0;
    }
    if (msg == kEditLeaveMessage) {  // round 12 phase 12D: another app's window came to the front
        if (editDock_ && modalDepth_ == 0) LeaveEdit();
        return 0;
    }
    if (msg == kShowWelcome) {
        OpenQuickSetup(firstRun_);  // round 12 phase 12G: the first run's variant once
        return 0;
    }
    if (msg == kPanelAnchorMoved) {  // round 12 phase 12I: a folder window / standalone widget moved (coalesced)
        panelAnchorPending_ = false;
        ReanchorPanels(nullptr, true, true);
        return 0;
    }
    if (msg == kQuickSetupClosed) {  // round 12 phase 12G
        if (quickSetup_ && !quickSetup_->Hwnd()) quickSetup_.reset();
        if (previewing_ && previewHost_ && !IsWindow(previewHost_)) CancelBar();  // closed without Finish / Cancel
        UpdateTimelineUi();
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
        std::erase_if(searchHidden_, [](const SettingsWindow* w) { return !w->Hwnd(); });  // round 9 phase 9B
        // Round 12 phase 12A: a closed window's mark goes; closing the window a preview was chosen
        // from cancels it.
        std::erase_if(marks_, [](const WindowMark& m) { return !m.window->Hwnd(); });
        if (previewing_ && previewHost_ && !IsWindow(previewHost_)) CancelBar();
        std::erase_if(settings_, [](const std::unique_ptr<SettingsWindow>& w) { return !w->Hwnd(); });
        if (settings_.empty()) docks_.Previews().Release();  // round 6 phase 6E: its factories go with the last window
        UpdateKeepShown();   // round 12 phase 12E: a closed panel no longer holds its dock shown
        UpdateTimelineUi();  // the banner, when no window shows the bar any more
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
    if (msg == kMoveToDockMessage) {
        // Round 16 phase 16G: the moves RequestWidgetToDock queued, by id (an entry or dock that is
        // gone or changed meanwhile: refused with a beep).
        std::vector<PendingDockMove> moves = std::move(pendingDockMoves_);
        pendingDockMoves_.clear();
        for (const PendingDockMove& m : moves) {
            const size_t cfg = DockIndexOf(m.entry), dock = DockIndexOf(m.dock);
            if (cfg == SIZE_MAX || dock == SIZE_MAX) {
                MessageBeep(MB_ICONWARNING);
            } else {
                MoveWidgetToDock(cfg, dock, m.at);  // beeps when refused
            }
        }
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
            if (startPlacer_.OnTimer(wp)) return 0;  // round 16 phase 16A: the Start menu watch
            if (wp == kKeepTimerId) {
                // Round 12 phase 12A: the Keep / Revert bar counts down; left alone, the change is kept.
                if (--keepSeconds_ <= 0) CloseKeepRevert();
                UpdateTimelineUi();
                return 0;
            }
            if (wp != kSaveTimerId) break;
            KillTimer(hwnd_, kSaveTimerId);  // one-shot
            savePending_ = false;
            SaveNow();  // round 12 phase 12A: its result is the save state (Saved / Couldn't save)
            return 0;
        case WM_QUERYENDSESSION:
            startPlacer_.Restore();  // round 16 phase 16A
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

// --- round 12 phase 12D: customize mode on the dock ------------------------------------------------

namespace {
HWND g_editHookTarget = nullptr;  // the app window the customize mode's foreground hook posts to
}  // namespace

void CALLBACK App::OnEditForeground(HWINEVENTHOOK, DWORD, HWND hwnd, LONG idObject, LONG, DWORD, DWORD) {
    // WINEVENT_SKIPOWNPROCESS: every event here is another process's window coming to the front (a
    // click on the desktop, the taskbar, another app): customize mode ends, as the click popups close.
    if (idObject != OBJID_WINDOW || !hwnd || !g_editHookTarget) return;
    PostMessageW(g_editHookTarget, kEditLeaveMessage, 0, 0);
}

void App::HookEditForeground(bool on) {
    if (on == (editHook_ != nullptr)) return;
    if (on) {
        g_editHookTarget = hwnd_;
        // Out of context and only while customizing (PopupWindow's click model does the same).
        editHook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, &App::OnEditForeground, 0, 0,
                                    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        if (!editHook_) g_editHookTarget = nullptr;
    } else {
        UnhookWinEvent(editHook_);
        editHook_ = nullptr;
        g_editHookTarget = nullptr;
    }
}

bool App::EnterEdit(DockWindow* dock) {
    if (!dock || shutDown_ || docks_.IndexOf(dock) == SIZE_MAX) return false;
    size_t cfg = dock->ConfigIndex();
    if (cfg >= config_.docks.size() || !config_.docks[cfg].IsDock()) return false;
    if (editDock_ == dock) return true;
    if (editDock_) LeaveEdit();
    if (config_.docks[cfg].locked) {
        // A locked dock keeps its items where they are: ask first (Unlock is a change of its own,
        // undoable; Cancel leaves everything as it was).
        const HWND owner = dock->Hwnd();
        int button = IDCANCEL;
        RunMenu(dock, [&] {
            TASKDIALOG_BUTTON buttons[] = {{IDOK, L"Unlock"}};
            TASKDIALOGCONFIG tdc{};
            tdc.cbSize = sizeof(tdc);
            tdc.hwndParent = owner;
            tdc.hInstance = instance_;
            tdc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
            tdc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
            tdc.pszWindowTitle = L"Customize this dock";
            tdc.pszMainInstruction = L"Unlock this dock to customize it?";
            tdc.pszContent = L"A locked dock keeps its items where they are. Unlock it to add, move and remove them on the dock; "
                             L"Undo locks it again.";
            tdc.pButtons = buttons;
            tdc.cButtons = static_cast<UINT>(std::size(buttons));
            tdc.nDefaultButton = IDOK;
            if (FAILED(TaskDialogIndirect(&tdc, &button, nullptr, nullptr))) {
                button = MessageBoxW(owner, L"Unlock this dock to customize it?", L"Customize this dock", MB_OKCANCEL | MB_ICONQUESTION);
            }
            return 0u;
        });
        // The dock (or its entry) may be gone while the question was open.
        if (button != IDOK || shutDown_ || docks_.IndexOf(dock) == SIZE_MAX) return false;
        cfg = dock->ConfigIndex();
        if (cfg >= config_.docks.size() || !config_.docks[cfg].IsDock()) return false;
        DockConfig& d = config_.docks[cfg];
        if (d.locked) {
            NoteLabel(L"Unlocked " + d.id);
            d.locked = false;
            docks_.ApplyWidgetWindow(cfg);
            RequestSave();
            RefreshSettings();
        }
    }
    FlushTimeline();  // what came before (the unlock) is an entry of its own
    // Round 12 phase 12H: the window that was in front, to give the foreground back when the mode
    // ends (not one of ours: our own windows keep it as they are).
    {
        const HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        editPrevForeground_ = fg && pid != GetCurrentProcessId() ? fg : nullptr;
    }
    editDock_ = dock;
    dock->SetEditing(true);
    NoteTipGesture(1);  // round 12 phase 12G: the second tip's gesture
    HookEditForeground(true);
    // The dock takes the keyboard (Escape leaves): allowed, the user just pressed on it or chose a
    // menu entry (from automation it may be refused: Done and a click beside the bar still work).
    SetForegroundWindow(dock->Hwnd());
    UpdateEditUi();
    return true;
}

void App::LeaveEdit() {
    DockWindow* d = editDock_;
    editDock_ = nullptr;
    HookEditForeground(false);
    if (dragDock_) {
        if (docks_.IndexOf(dragDock_) != SIZE_MAX) dragDock_->ExternalItemLeave();
        dragDock_ = nullptr;
    }
    // The dock gets the foreground back from the drawer (not a dock window that is going away).
    drawerPopup_.SetReturnFocus(d && docks_.IndexOf(d) != SIZE_MAX ? d->Hwnd() : nullptr);
    drawerPopup_.Hide();
    editBar_.Close();
    if (d && docks_.IndexOf(d) != SIZE_MAX) {
        d->SetDrawerHold(false);
        d->SetEditing(false);
    }
    // Round 12 phase 12H: EnterEdit gave the dock the foreground (its Escape); left by Done or Esc it
    // kept it for good, and the dock is not a window to stay in front. Measured on this machine: an
    // accessibility client polls the foreground window with WM_GETOBJECT, about 320 a second for the
    // dock, 30 - 45 ms of the UI thread's time every second until another window was activated. The
    // window that was in front before gets it back (a click on another app already did that).
    const HWND prev = editPrevForeground_;
    editPrevForeground_ = nullptr;
    if (prev && !shutDown_ && IsWindow(prev) && IsWindowVisible(prev) && !IsIconic(prev)) {
        const HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        if (!fg || pid == GetCurrentProcessId()) SetForegroundWindow(prev);
    }
}

void App::UpdateEditUi() {
    DockWindow* d = editDock_;
    if (!d || shutDown_) return;
    const size_t cfg = d->ConfigIndex();
    if (cfg >= config_.docks.size() || !config_.docks[cfg].IsDock() || config_.docks[cfg].locked) {
        // Gone, or locked again (an Undo of "Unlocked main", the Lock button): customizing would do
        // nothing. Posted: this may run inside the dock's own layout pass.
        PostMessageW(hwnd_, kEditLeaveMessage, 0, 0);
        return;
    }
    const DockConfig& dc = config_.docks[cfg];
    EditBar::Content c;
    c.dock = config_.DockDisplayName(cfg);  // round 12 phase 12C's name, else the id (the 12C / 12D merge)
    c.dockArgb = config_.DockColor(cfg);
    // The 12A save state, worded as the settings windows' band words it.
    const SettingsStatus s = StatusFor(nullptr);
    c.status = s.save == 2   ? L"\x26A0 Not saved"
               : s.bar == 2  ? L"Not saved yet"
               : s.save == 1 ? L"Saving\x2026"
                             : L"\x2713 Saved";
    c.canUndo = previewing_ || !undo_.empty();
    c.drawerShown = drawerPopup_.Shown();
    const RECT frame = d->EditFrameRect();
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(MonitorFromRect(&frame, MONITOR_DEFAULTTONEAREST), &mi)) return;
    editBar_.Show(instance_, frame, mi.rcWork, dc.edge, dc.Floating(), d->Dpi(), c);
    if (drawerPopup_.Shown()) PlaceDrawer();
}

void App::PlaceDrawer() {
    DockWindow* d = editDock_;
    if (!d) return;
    const size_t cfg = d->ConfigIndex();
    if (cfg >= config_.docks.size()) return;
    const DockConfig& dc = config_.docks[cfg];
    const RECT frame = d->EditFrameRect();
    const RECT bar = editBar_.Shown() ? editBar_.Rect() : frame;
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(MonitorFromRect(&frame, MONITOR_DEFAULTTONEAREST), &mi)) return;
    // The room for it: beyond the edit bar (above a bottom dock's, under a top dock's) or, beside a
    // vertical dock, above the edit bar in its column; never over the dock.
    RECT avail = mi.rcWork;
    const int gap = MulDiv(8, static_cast<int>(d->Dpi()), USER_DEFAULT_SCREEN_DPI);
    DockEdge edge = dc.edge;
    if (dc.Floating() || !IsVertical(edge)) edge = bar.bottom <= frame.top ? DockEdge::Bottom : DockEdge::Top;
    int endAlong = bar.right;
    switch (edge) {
        case DockEdge::Top: avail.top = std::max(bar.bottom, frame.bottom) + gap; break;
        case DockEdge::Left:
            avail.left = std::max(bar.left, frame.right);
            avail.bottom = bar.top - gap;
            endAlong = bar.bottom;
            break;
        case DockEdge::Right:
            avail.right = std::min(bar.right, frame.left);
            avail.bottom = bar.top - gap;
            endAlong = bar.bottom;
            break;
        case DockEdge::Bottom:
        default: avail.bottom = std::min(bar.top, frame.top) - gap; break;
    }
    if (!drawerReady_) {
        drawerReady_ = true;
        DrawerPopup::Callbacks cb;
        cb.add = [this](int ref) { AddDrawerEntry(ref, SIZE_MAX); };
        cb.desktop = [this](int ref) {
            const StripUi::DrawerEntry* e = drawerPopup_.Entry(ref);
            if (!e || e->kind != ItemKind::Widget || !editDock_) return;
            NoteLabel(L"Added " + e->name + L" to the desktop");
            if (AddWidgetWindow(e->id, editDock_->ConfigIndex(), false) == SIZE_MAX) MessageBeep(MB_ICONWARNING);
        };
        cb.drag = [this](int phase, POINT screen, int ref) {
            if (phase == 1 || !editDock_ || editDock_->ConfigIndex() >= config_.docks.size()) return DrawerDrag(1, screen, nullptr);
            const std::optional<DockItemEntry> e = drawerPopup_.EntryFor(ref, config_.docks[editDock_->ConfigIndex()]);
            return DrawerDrag(phase, screen, e ? &*e : nullptr);
        };
        cb.closed = [this] {
            if (editDock_) editDock_->SetDrawerHold(false);
            UpdateEditUi();
        };
        cb.escape = [this] { LeaveEdit(); };  // Escape in a popup of customize mode leaves the mode
        cb.renderPreview = [this](const std::wstring& id, float scale, SIZE fit, float maxZoom) {
            const WidgetPreview::Result r = docks_.RenderPreview(id, scale, static_cast<UINT>(std::max<LONG>(1, fit.cx)),
                                                                 static_cast<UINT>(std::max<LONG>(1, fit.cy)), maxZoom);
            return SettingsWidgetPreview{r.bitmap, r.px, r.error, r.ms};
        };
        cb.previewStamp = [this] { return WidgetPreview::ThemeStamp(config_) ^ (previewGeneration_ * 0x9E3779B97F4A7C15ull); };
        cb.previewPanel = [this] { return config_.theme.background; };
        drawerPopup_.SetCallbacks(std::move(cb));
    }
    drawerPopup_.SetReturnFocus(d->Hwnd());
    const bool wasShown = drawerPopup_.Shown();
    drawerPopup_.Show(instance_, dc, config_.DockDisplayName(cfg), config_.DockColor(cfg), avail, edge, endAlong, d->Dpi());  // 12C's name (the merge)
    if (!wasShown && drawerPopup_.Shown()) d->SetDrawerHold(true);
}

void App::ShowDrawer(bool show) {
    DockWindow* d = editDock_;
    if (show && d) {
        PlaceDrawer();
    } else if (drawerPopup_.Shown()) {
        drawerPopup_.Hide();
        if (d) d->SetDrawerHold(false);
    }
    UpdateEditUi();  // "Add items" / "Hide items"
}

void App::OnEditBarCommand(int id) {
    DockWindow* d = editDock_;
    if (!d) return;
    switch (id) {
        case SettingsIds::kEditBarUndo:
            if (!Undo()) MessageBeep(MB_OK);
            break;
        case SettingsIds::kEditBarAdd: ShowDrawer(!drawerPopup_.Shown()); break;
        case SettingsIds::kEditBarSettings:  // = DockCommand 143 (12E: the panel, beside this instance)
            panelFrom_ = d->Hwnd();
            OpenDockSettings(d->ConfigIndex());
            break;
        case SettingsIds::kEditBarDone: {
            // Round 12 phase 12E: Done also closes the dock's settings panel (customizing is over).
            const size_t cfg = d->ConfigIndex();
            LeaveEdit();
            ClosePanel(cfg);
            break;
        }
        default: break;
    }
}

void App::EditRemoveItem(size_t item) {
    DockWindow* d = editDock_;
    if (!d) return;
    const size_t cfg = d->ConfigIndex();
    if (cfg >= config_.docks.size() || item >= config_.docks[cfg].items.size()) return;
    DockConfig& dc = config_.docks[cfg];
    NoteLabel(L"Removed " + EntryName(cfg, item) + L" from " + dc.id);
    dc.items.erase(dc.items.begin() + static_cast<ptrdiff_t>(item));
    ItemsChanged(cfg);
    if (drawerPopup_.Shown() && cfg < config_.docks.size()) drawerPopup_.Refresh(config_.docks[cfg]);  // a once-only kind is free again
}

void App::EditAnchorItem(size_t item, ItemAnchor anchor) {
    DockWindow* d = editDock_;
    if (!d) return;
    const size_t cfg = d->ConfigIndex();
    if (cfg >= config_.docks.size() || item >= config_.docks[cfg].items.size()) return;
    if (config_.docks[cfg].items[item].anchor == anchor) return;
    NoteLabel(L"Anchored " + EntryName(cfg, item) +
              (anchor == ItemAnchor::Start ? L" at the start" : anchor == ItemAnchor::Center ? L" in the centre" : L" at the end"));
    SetItemAnchor(cfg, item, anchor);  // ItemCommand 859's path: sorted into group order, reloaded, saved
}

bool App::InsertDockEntry(size_t cfg, DockItemEntry e, size_t at) {
    if (shutDown_ || cfg >= config_.docks.size() || !config_.docks[cfg].IsDock()) return false;
    DockConfig& d = config_.docks[cfg];
    const ItemKind k = e.Kind();
    const bool once = k == ItemKind::Running || k == ItemKind::Tray || k == ItemKind::Notifications || k == ItemKind::RecycleBin;
    if (d.locked || (once && d.Has(k))) {
        MessageBeep(MB_ICONWARNING);
        return false;
    }
    if (k == ItemKind::Tray) config_.tray.enabled = true;  // as the dock menu's Add > Tray
    at = std::min(at, d.items.size());
    d.items.insert(d.items.begin() + static_cast<ptrdiff_t>(at), std::move(e));
    // Round 7 phase 7B: it joins the group of its new neighbours, then the items are in group order.
    d.JoinNeighbourGroup(at);
    d.SortByGroup(&at);
    NoteLabel(L"Added " + EntryName(cfg, at) + L" to " + d.id);
    ItemsChanged(cfg);  // a clock / weather item gets its defaults (ReloadPins: EnsureItemSettings)
    if (editDock_ && drawerPopup_.Shown() && editDock_->ConfigIndex() < config_.docks.size()) {
        drawerPopup_.Refresh(config_.docks[editDock_->ConfigIndex()]);
    }
    return true;
}

bool App::AddDrawerEntry(int ref, size_t at) {
    DockWindow* d = editDock_;
    if (!d || shutDown_) return false;
    const size_t cfg = d->ConfigIndex();
    if (cfg >= config_.docks.size()) return false;
    if (ref == StripUi::DrawerRefs::kApp || ref == StripUi::DrawerRefs::kFolder) {
        // Their dialogs (owned by the drawer when it is open; it stays open meanwhile).
        const HWND owner = drawerPopup_.Shown() ? drawerPopup_.Hwnd() : d->Hwnd();
        std::vector<std::wstring> paths;
        drawerPopup_.SetModal(true);
        RunMenu(d, [&] {
            paths = PickPins(owner, ref == StripUi::DrawerRefs::kFolder);
            return 0u;
        });
        drawerPopup_.SetModal(false);
        if (paths.empty() || editDock_ != d || docks_.IndexOf(d) == SIZE_MAX) return false;
        const size_t c = d->ConfigIndex();
        if (c >= config_.docks.size()) return false;
        // After the last pin, or (ItemCommand 864) before config item `at`: the pins before it.
        size_t pinAt = config_.docks[c].PinCount();
        if (at != SIZE_MAX) {
            pinAt = 0;
            for (size_t i = 0; i < at && i < config_.docks[c].items.size(); ++i) {
                if (config_.docks[c].items[i].IsPin()) ++pinAt;
            }
        }
        PinFiles(c, d->Pins(), paths, pinAt);
        return true;
    }
    std::optional<DockItemEntry> e = drawerPopup_.EntryFor(ref, config_.docks[cfg]);
    if (!e) return false;
    // A click adds it at the end of the dock (ItemCommand 864: where a drop before item `at` would).
    return InsertDockEntry(cfg, std::move(*e), at == SIZE_MAX ? config_.docks[cfg].items.size() : at);
}

int App::DrawerDrag(int phase, POINT screen, const DockItemEntry* entry) {
    DockWindow* under = nullptr;
    HWND root = nullptr;
    if (phase != 1) {
        if (HWND h = WindowFromPoint(screen)) root = GetAncestor(h, GA_ROOT);
        under = root ? docks_.ByHwnd(root) : nullptr;
    }
    if (dragDock_ && dragDock_ != under) {
        if (docks_.IndexOf(dragDock_) != SIZE_MAX) dragDock_->ExternalItemLeave();
        dragDock_ = nullptr;
    }
    if (phase == 1) drawerStandaloneId_.clear();  // round 16 phase 16E (below)
    if (phase == 1 || !entry) return 0;
    if (under) {
        // A dock (any dock of this process): its marker shows where the tile lands.
        if (phase == 0) {
            if (!under->ExternalItemOver(screen)) return 0;
            dragDock_ = under;
            return 1;
        }
        const size_t at = under->ExternalItemDrop(screen);
        dragDock_ = nullptr;
        return at != SIZE_MAX && InsertDockEntry(under->ConfigIndex(), *entry, at) ? 1 : 0;
    }
    // Outside every dock and every window of WinDock (the desktop, as the old "Desktop" button): a
    // script widget becomes a standalone widget centred on the point.
    // Round 16 phase 16E: not a widget whose manifest says it needs a dock (CanBeStandalone, the
    // same decision as a widget dragged off a dock). Phase 0 comes with every mouse move: the
    // manifest's answer is kept per widget id for the drag (cleared by its drop).
    DWORD pid = 0;
    if (root) GetWindowThreadProcessId(root, &pid);
    bool standalone = false;
    if (entry->Kind() == ItemKind::Widget) {
        if (drawerStandaloneId_ != entry->id) {
            drawerStandaloneId_ = entry->id;
            drawerStandaloneOk_ = CanBeStandalone(*entry);
        }
        standalone = drawerStandaloneOk_;
    }
    if (phase != 0) drawerStandaloneId_.clear();
    const bool desktop = standalone && root && pid != GetCurrentProcessId();
    if (phase == 0 || !desktop) return desktop ? 2 : 0;
    return AddWidgetWindowAt(entry->id, screen) != SIZE_MAX ? 1 : 0;
}

size_t App::AddWidgetWindowAt(const std::wstring& id, POINT screen) {
    NoteLabel(L"Added " + Widgets::DisplayName(id) + L" to the desktop");
    widgetAt_ = screen;
    const size_t cfg = AddWidgetWindow(id, SIZE_MAX, false);
    widgetAt_.reset();
    return cfg;
}

bool App::MoveWidgetToDesktop(size_t cfg, size_t item, std::optional<POINT> screen) {
    // Round 16 phase 16E: refused on a locked dock, for anything but a script widget (the clock /
    // weather / tray / notification kinds and the running apps need a dock: AddWidgetWindow only
    // makes "widget" entries) and for a widget whose manifest says it needs a dock.
    if (shutDown_ || modalDepth_ > 0 || cfg >= config_.docks.size() || !config_.docks[cfg].IsDock() || config_.docks[cfg].locked ||
        item >= config_.docks[cfg].items.size() || !CanBeStandalone(config_.docks[cfg].items[item])) {
        MessageBeep(MB_ICONWARNING);
        return false;
    }
    // One undo entry for the gesture (the first label wins: AddWidgetWindowAt's is not used).
    NoteLabel(L"Moved " + EntryName(cfg, item) + L" to the desktop");
    // A copy: the erase invalidates the reference, and AddWidgetWindow's push_back onto
    // config_.docks may reallocate (no DockConfig& is held across it). The dock's widget instance
    // goes with the item (ReloadPins rebuilds its elements); the standalone window makes its own
    // (init runs again). storage is per widget id (WidgetRuntime), so the new one sees the same store.
    const DockItemEntry entry = config_.docks[cfg].items[item];
    config_.docks[cfg].items.erase(config_.docks[cfg].items.begin() + static_cast<ptrdiff_t>(item));
    ReloadPins(cfg);  // first: the dock's slot is gone before the new window shows
    if (screen) widgetAt_ = *screen;
    const size_t added = AddWidgetWindow(entry, cfg, false);  // syncs, saves, refreshes the settings windows
    widgetAt_.reset();
    if (added == SIZE_MAX) {
        // Refused after all (checked above, so unexpected): the item goes back where it was.
        auto& items = config_.docks[cfg].items;
        items.insert(items.begin() + static_cast<ptrdiff_t>(std::min(item, items.size())), entry);
        ReloadPins(cfg);
        MessageBeep(MB_ICONWARNING);
        return false;
    }
    return true;
}

// --- round 16 phase 16G: a standalone widget back onto a dock ---------------------------------

bool App::CanMoveWidgetToDock(size_t cfg, size_t dock) const {
    // Any script widget may live on a dock (standalone.supported only limits the other way). The
    // entry's own lock (no move / resize of the window) does not refuse it: the menu entry is a
    // deliberate choice, and a locked window cannot be dragged anyway.
    if (shutDown_ || modalDepth_ > 0 || cfg >= config_.docks.size() || dock >= config_.docks.size() || cfg == dock) return false;
    const DockConfig& entry = config_.docks[cfg];
    const DockConfig& target = config_.docks[dock];
    return entry.IsWidgetWindow() && entry.WidgetItem() && target.IsDock() && target.enabled && !target.locked;
}

bool App::RequestWidgetToDock(size_t cfg, size_t dock, size_t at) {
    if (!hwnd_ || !CanMoveWidgetToDock(cfg, dock)) {
        MessageBeep(MB_ICONWARNING);
        return false;
    }
    // By id: a sync or another posted removal may shift the indexes before the message runs.
    pendingDockMoves_.push_back(PendingDockMove{config_.docks[cfg].id, config_.docks[dock].id, at});
    if (pendingDockMoves_.size() == 1) PostMessageW(hwnd_, kMoveToDockMessage, 0, 0);  // one message drains them all
    return true;
}

bool App::MoveWidgetToDock(size_t cfg, size_t dock, size_t at) {
    if (!CanMoveWidgetToDock(cfg, dock)) {
        MessageBeep(MB_ICONWARNING);
        return false;
    }
    // One undo entry for the gesture: the first label wins (InsertDockEntry's "Added …" and
    // EditDocks' "Removed …" are not used). The timeline stores whole-config snapshots, so Undo
    // brings the standalone entry (and its window) back and takes the item off the dock.
    const std::wstring name = Widgets::DisplayName(config_.docks[cfg].WidgetItem()->id);
    NoteLabel(L"Moved " + name + L" to " + config_.docks[dock].DisplayName());
    // A copy of the item (kind, id, settings, name; its anchor / offset are none / 0 on a standalone
    // entry, and InsertDockEntry joins it to its new neighbours' group). The entry's x / y / size /
    // layer / lock stay behind with it. Storage is per widget id (WidgetRuntime): the dock's new
    // instance sees the same store; its init runs again.
    DockItemEntry item = *config_.docks[cfg].WidgetItem();
    const std::wstring entryId = config_.docks[cfg].id;
    // Into the dock first, then the entry goes: its erase shifts every index above `cfg` (no
    // DockConfig& is held across either call).
    if (!InsertDockEntry(dock, std::move(item), at == SIZE_MAX ? config_.docks[dock].items.size() : at)) return false;  // it beeped
    const size_t now = DockIndexOf(entryId);
    if (now == SIZE_MAX || !EditDocks(SettingsDockEdit::Remove, now)) {
        // Unexpected (checked above): both now exist; the user can remove the window by hand.
        MessageBeep(MB_ICONWARNING);
        return false;
    }
    return true;
}

int App::WidgetDockDrag(size_t cfg, int phase, POINT screen) {
    // As DrawerDrag's dock half. WindowFromPoint would answer the dragged window itself (it is under
    // the pointer, above the dock), so the dock is found by its rect instead: the first shown dock
    // window containing the point whose config dock may take the widget (an enabled, unlocked dock:
    // a locked one shows no marker at all). ExternalItemOver answers false off the bar and on a
    // hidden (auto-hidden) dock: a drop there simply does not land (not opened specially).
    DockWindow* under = nullptr;
    if (phase != 1 && cfg < config_.docks.size()) {
        docks_.ForEach([&](DockWindow& w) {
            if (under || !w.Hwnd() || !IsWindowVisible(w.Hwnd()) || !CanMoveWidgetToDock(cfg, w.ConfigIndex())) return;
            RECT r{};
            if (GetWindowRect(w.Hwnd(), &r) && PtInRect(&r, screen)) under = &w;
        });
    }
    if (widgetDragDock_ && widgetDragDock_ != under) {
        if (docks_.IndexOf(widgetDragDock_) != SIZE_MAX) widgetDragDock_->ExternalItemLeave();
        widgetDragDock_ = nullptr;
    }
    if (phase == 1 || !under) return 0;
    if (phase == 0) {
        if (!under->ExternalItemOver(screen)) return 0;
        widgetDragDock_ = under;
        return 1;
    }
    const size_t at = under->ExternalItemDrop(screen);
    widgetDragDock_ = nullptr;
    // Posted: this runs inside the widget window's WM_LBUTTONUP, and the move destroys that window.
    return at != SIZE_MAX && RequestWidgetToDock(cfg, under->ConfigIndex(), at) ? 1 : 0;
}

std::vector<std::wstring> App::PickPins(HWND owner, bool folder) {
    // As the Contents tab's Add app… / Add folder… (SettingsPagesContents.cpp).
    std::vector<std::wstring> paths;
    winrt::com_ptr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())))) {
        MessageBeep(MB_ICONERROR);
        return paths;
    }
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    if (folder) {
        dialog->SetTitle(L"Pin a folder to the dock");
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
    } else {
        const COMDLG_FILTERSPEC filters[] = {{L"Apps and shortcuts (*.lnk;*.exe)", L"*.lnk;*.exe"}, {L"All files", L"*.*"}};
        dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
        dialog->SetTitle(L"Pin apps to the dock");
        // NODEREFERENCELINKS: a chosen shortcut is pinned as the shortcut (keeps its arguments).
        dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM | FOS_NODEREFERENCELINKS |
                           FOS_NOCHANGEDIR);
        winrt::com_ptr<IShellItem> programs;
        if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_Programs, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(programs.put())))) {
            dialog->SetDefaultFolder(programs.get());
        }
    }
    if (FAILED(dialog->Show(owner))) return paths;  // cancelled
    winrt::com_ptr<IShellItemArray> items;
    DWORD count = 0;
    if (FAILED(dialog->GetResults(items.put())) || FAILED(items->GetCount(&count))) return paths;
    for (DWORD i = 0; i < count; ++i) {
        winrt::com_ptr<IShellItem> item;
        PWSTR raw = nullptr;
        if (SUCCEEDED(items->GetItemAt(i, item.put())) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) && raw) {
            paths.emplace_back(raw);
            CoTaskMemFree(raw);
        }
    }
    return paths;
}

// --- round 12 phase 12E: the Dock settings panel beside its dock, Where on the screen ---------------

DockWindow* App::PanelDockOf(const SettingsWindow* w) {
    const size_t cfg = w ? w->DockIndex() : SIZE_MAX;
    if (cfg >= config_.docks.size()) return nullptr;
    // The instance it was opened from while it still shows this dock (an "each" dock's display may
    // have gone: then the dock's first window).
    if (const HWND a = w->AnchorDock()) {
        if (DockWindow* d = docks_.ByHwnd(a); d && d->ConfigIndex() == cfg) return d;
    }
    return docks_.FirstOf(cfg);
}

bool App::DesktopAnchor(size_t cfg, RECT* rect, HWND* hwnd) {
    if (cfg >= config_.docks.size()) return false;
    // A folder window has no hidden state of its own (collapsed is still shown); a standalone
    // widget is hidden by the "desktop" layer's rule and the full-screen guard (Shown()).
    if (FolderWindow* f = docks_.FolderOf(cfg); f && f->Hwnd()) {
        if (rect) *rect = f->ScreenRect();
        if (hwnd) *hwnd = f->Hwnd();
        return IsWindowVisible(f->Hwnd()) != FALSE;
    }
    if (WidgetWindow* wi = docks_.WidgetOf(cfg); wi && wi->Hwnd()) {
        if (rect) *rect = wi->ScreenRect();
        if (hwnd) *hwnd = wi->Hwnd();
        return wi->Shown();
    }
    return false;  // parked: its display is gone
}

void App::ReanchorPanels(DockWindow* moved, bool animate, bool desktopOnly) {
    if (shutDown_) return;
    // Index loop: closing only posts (the objects are freed by kSettingsClosed).
    for (size_t i = 0; i < settings_.size(); ++i) {
        SettingsWindow* w = settings_[i].get();
        if (!w->Hwnd() || !w->IsPanel() || IsSearchHidden(w)) continue;
        if (w->IsFolderWindow() || w->IsWidgetWindow()) {
            // Round 12 phase 12I: beside its window on the desktop; gone (parked) or hidden: closes,
            // as a parked dock's panel does. A dock that moved concerns none of these.
            if (!DesktopAnchor(w->DockIndex(), nullptr, nullptr)) {
                PostMessageW(w->Hwnd(), WM_CLOSE, 0, 0);
                continue;
            }
            if (!moved) w->Reanchor(animate);
            continue;
        }
        if (desktopOnly) continue;
        DockWindow* d = PanelDockOf(w);
        if (!d) {
            // Its dock has no window (parked: its display is gone; disabled): a panel beside nothing
            // closes, as the dock's window did when its dock went away.
            PostMessageW(w->Hwnd(), WM_CLOSE, 0, 0);
            continue;
        }
        if (moved && d != moved) continue;
        w->Reanchor(animate);
    }
}

bool App::ClosePanel(size_t cfg) {
    if (cfg >= config_.docks.size()) return false;
    SettingsWindow* w = FindSettings(SettingsKind::Dock, config_.docks[cfg].id);
    if (!w || !w->IsPanel() || IsSearchHidden(w) || !IsWindowVisible(w->Hwnd())) return false;
    DestroyWindow(w->Hwnd());  // freed by kSettingsClosed
    return true;
}

void App::MoveInEdge(DockWindow& dock, int offset, int edgeGap) {
    const size_t cfg = dock.ConfigIndex();
    if (shutDown_ || cfg >= config_.docks.size()) return;
    DockConfig& c = config_.docks[cfg];
    offset = std::clamp(offset, -16000, 16000);
    edgeGap = std::clamp(edgeGap, 0, DockConfig::kMaxEdgeGap);
    if (c.offset == offset && c.edgeGap == edgeGap) {
        dock.Relayout();  // back to its own place
        return;
    }
    // One gesture, one timeline entry: "Moved main (+120 px, 24 px from the edge)". Neither value
    // hides the dock or puts it on another edge or display: no Keep / Revert (DescribePlacement).
    const std::wstring along = offset == 0 ? std::wstring(c.align == DockAlign::Start ? L"at the start"
                                                          : c.align == DockAlign::End ? L"at the end"
                                                                                      : L"centred")
                                           : std::wstring(offset > 0 ? L"+" : L"\x2212") + std::to_wstring(std::abs(offset)) + L" px";
    NoteLabel(L"Moved " + c.DisplayName() + L" (" + along + L", " + std::to_wstring(edgeGap) + L" px from the edge)");
    NoteTipGesture(2);  // round 12 phase 12G: the third tip's gesture (a dock dragged; the 12E / 12G merge)
    c.offset = offset;
    c.edgeGap = edgeGap;
    docks_.ForConfig(cfg, [](DockWindow& w) { w.ApplyConfig(); });
    RequestSave();
    RefreshSettings();
}

bool App::PanelCommand(UINT command, LPARAM lp) {
    if (shutDown_) return false;
    DockManager::Instance* inst = docks_.At(DockOfParam(lp));
    if (!inst || inst->cfg >= config_.docks.size()) return false;
    const size_t cfg = inst->cfg;
    if (command == ContextMenu::kClosePanel) return ClosePanel(cfg);
    DockConfig& d = config_.docks[cfg];
    if (!d.IsDock()) return false;
    if (command == ContextMenu::kSetEdgeGap) {
        const int v = std::clamp(static_cast<int>(lp & 0xFFFF), 0, DockConfig::kMaxEdgeGap);
        if (d.edgeGap != v) {
            NoteLabel(L"Distance from the screen edge " + std::to_wstring(d.edgeGap) + L" \x2192 " + std::to_wstring(v));
            d.edgeGap = v;
        }
    } else if (command == ContextMenu::kSetDockOffset) {
        const int v = static_cast<int16_t>(static_cast<uint16_t>(lp & 0xFFFF));
        if (d.offset != v) {
            NoteLabel(L"Distance along the edge " + std::to_wstring(d.offset) + L" \x2192 " + std::to_wstring(v));
            d.offset = v;
        }
    } else if (command == ContextMenu::kSetDockLength || command == ContextMenu::kSetDockThickness) {
        // Round 14 phase 14A: docks[].length / thickness (0 automatic; else Config::Sanitize's range).
        const bool length = command == ContextMenu::kSetDockLength;
        const int raw = static_cast<int>(lp & 0xFFFF);
        const int v = raw <= 0 ? 0 : length ? std::clamp(raw, 96, 8000) : std::clamp(raw, 24, 1000);
        int& field = length ? d.length : d.thickness;
        if (field != v) {
            NoteLabel(std::wstring(length ? L"Changed the bar's length of " : L"Changed the bar's thickness of ") + d.DisplayName());
            field = v;
        }
    } else {
        return false;
    }
    docks_.ForConfig(cfg, [](DockWindow& w) { w.ApplyConfig(); });
    RequestSave();
    RefreshSettings();
    return true;
}
// --- end round 12 phase 12E ---