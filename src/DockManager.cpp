#include "DockManager.h"

#include <algorithm>

#include "DesktopLayer.h"
#include "Launcher.h"

namespace {

BOOL CALLBACK CollectMonitor(HMONITOR mon, HDC, LPRECT, LPARAM lp) {
    reinterpret_cast<std::vector<HMONITOR>*>(lp)->push_back(mon);
    return TRUE;
}

}  // namespace

std::vector<HMONITOR> DockManager::EnumerateMonitors() {
    // Index 0 = primary; 1..n = the other monitors in enumeration order.
    std::vector<HMONITOR> monitors;
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&monitors));
    HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    std::erase(monitors, primary);
    monitors.insert(monitors.begin(), primary);
    return monitors;
}

void DockManager::Initialize(HINSTANCE instance, Config* config, HWND owner, Callbacks callbacks) {
    instance_ = instance;
    config_ = config;
    owner_ = owner;
    callbacks_ = std::move(callbacks);
    monitors_ = EnumerateMonitors();
    widgets_.Initialize(owner_);
    weather_.SetChangedCallback([this] {
        ForEach([](DockWindow& w) { w.OnWeatherChanged(); });
        if (callbacks_.weatherChanged) callbacks_.weatherChanged();
    });
    // The window list for every dock (the initial enumeration runs now, before any dock exists).
    tracker_.SetTitleCallback([this](const std::vector<HWND>& windows) {
        ForEach([&](DockWindow& w) { w.OnWindowTitles(windows); });
    });
    tracker_.Start(owner_, [this](const WindowTracker::Snapshot& snap) {
        ForEach([&](DockWindow& w) { w.OnSnapshot(snap); });
    });
    // The tray: icon changes reach every dock (posted and coalesced by TrayHost); an app asking
    // where its icon is gets the first dock that shows it.
    tray_.SetChangedCallback([this] { ForEach([](DockWindow& w) { w.OnTrayChanged(); }); });
    // Notifications (phase 9): every change reaches every dock (badges, the centre element), the
    // widgets that use the provider, and the settings window; tray balloons join the list.
    notifications_.SetChanged([this] {
        ForEach([](DockWindow& w) { w.OnNotificationsChanged(); });
        widgets_.NotifyNotifications();
        if (callbacks_.notificationsChanged) callbacks_.notificationsChanged();
    });
    // Balloons join the list only while they are kept from Explorer (notifications.balloons):
    // otherwise Explorer shows them as toasts, which the listener reads like any other.
    tray_.SetBalloonSink([this](const TrayBalloon& b) {
        if (config_ && config_->notifications.balloons && tray_.StripBalloons()) notifications_.AddBalloon(b);
    });
    widgets_.SetNotifications(&notifications_, [this] { SyncNotifications(); });
    // Calendars (phase 13): every dock (the popup, the clock's dot) and the settings window.
    calendar_.SetChanged([this] {
        ForEach([](DockWindow& w) { w.OnCalendarChanged(); });
        widgets_.NotifyCalendar();
        if (callbacks_.calendarChanged) callbacks_.calendarChanged();
    });
    // Reminders (13b): the first dock's banner, and the widgets' onReminder.
    calendar_.SetReminderCallback([this](const CalendarService::ReminderInfo& r) {
        if (DockWindow* w = First()) w->ShowReminder(r);
        widgets_.NotifyReminder(r.json);
    });
    widgets_.SetCalendar(&calendar_);
    tray_.SetRectResolver([this](const TrayIcon& icon, RECT& r) {
        for (Instance& i : instances_) {
            if (i.window && i.window->TrayIconRect(icon.id, r)) return true;
        }
        return false;
    });
}

bool DockManager::SyncTray() {
    bool wanted = config_ && config_->tray.enabled;
    if (wanted) {
        wanted = std::any_of(config_->docks.begin(), config_->docks.end(), [](const DockConfig& d) {
            return d.Has(ItemKind::Tray) || d.Has(ItemKind::TrayApp);
        });
    }
    if (wanted && !tray_.Running() && !trayStartPosted_) {
        trayStartPosted_ = PostMessageW(owner_, kTrayStartMessage, 0, 0) != FALSE;
    } else if (!wanted && tray_.Running()) {
        tray_.Stop();  // tells the docks (their grids empty)
    }
    return wanted;
}

void DockManager::Shutdown() {
    if (!config_) return;
    // The icons go back to Explorer first (no dock redraws for it).
    tray_.SetChangedCallback(nullptr);
    tray_.SetBalloonSink(nullptr);
    tray_.Stop();
    notifications_.SetChanged(nullptr);
    widgets_.SetNotifications(nullptr, nullptr);  // the widgets going below must not restart it
    notifications_.Stop();
    for (Instance& i : instances_) {
        if (i.window) Close(std::move(i.window));
        if (i.folder) i.folder->Destroy();
    }
    instances_.clear();
    folderGraveyard_.clear();
    if (folderRendererReady_) folderRenderer_.Shutdown();
    folderRendererReady_ = false;
    watchers_.clear();
    watch_.Stop();
    tracker_.Stop();
    weather_.Stop();  // a fetch still in flight posts to a window that is gone (and frees it)
    calendar_.SetChanged(nullptr);
    calendar_.SetReminderCallback(nullptr);
    calendar_.Stop();
    widgets_.SetCalendar(nullptr);
    graveyard_.clear();
    widgets_.Shutdown();  // every widget went with its window
    config_ = nullptr;
}

void DockManager::Reap() {
    graveyard_.clear();
    folderGraveyard_.clear();
    // The folder windows' device goes with the last of them (tens of MB with a hardware device).
    if (folderRendererReady_ && FolderCount() == 0) {
        folderRenderer_.Shutdown();
        folderRendererReady_ = false;
    }
}

bool DockManager::EnsureFolderRenderer() {
    if (folderRendererReady_) return true;
    const bool software = config_ && config_->renderer == L"software";
    if (FAILED(folderRenderer_.Initialize(owner_, software))) {
        folderRenderer_.Shutdown();
        return false;
    }
    folderRendererReady_ = true;
    return true;
}

void DockManager::RecoverFolderDevice() {
    folderRecoveryPosted_ = false;
    if (!folderRendererReady_) return;
    for (Instance& i : instances_) {
        if (i.folder) i.folder->ReleaseDevice();
    }
    if (FAILED(folderRenderer_.RecreateDevice())) {
        OutputDebugStringW(L"WinDock: the folder windows' device could not be recreated\n");
        return;
    }
    for (Instance& i : instances_) {
        if (i.folder) i.folder->RestoreDevice();
    }
}

HRESULT DockManager::CreateFolderFor(Instance& inst) {
    if (!EnsureFolderRenderer()) {
        inst.failed = true;
        return E_FAIL;
    }
    auto folder = std::make_unique<FolderWindow>();
    FolderWindow::Host host;
    host.renderer = &folderRenderer_;
    host.changed = [this] {
        if (callbacks_.folderChanged) callbacks_.folderChanged();
    };
    host.neighbours = [this](const FolderWindow* self) {
        std::vector<RECT> rects;
        for (const Instance& i : instances_) {
            if (i.folder && i.folder.get() != self && i.folder->Hwnd()) rects.push_back(i.folder->ScreenRect());
        }
        return rects;
    };
    host.deviceLost = [this] {
        if (!folderRecoveryPosted_) folderRecoveryPosted_ = PostMessageW(owner_, kFolderDeviceLost, 0, 0) != FALSE;
    };
    host.remove = [this](size_t cfg) {
        if (callbacks_.folderRemove) callbacks_.folderRemove(cfg);
    };
    host.newWindow = [this] {
        if (callbacks_.folderNew) callbacks_.folderNew();
    };
    const HRESULT hr = folder->Create(instance_, config_, inst.cfg, inst.monitor, std::move(host));
    if (FAILED(hr)) {
        folder->Destroy();
        inst.failed = true;
        return hr;
    }
    inst.failed = false;
    inst.folder = std::move(folder);
    return S_OK;
}

FolderWindow* DockManager::FolderOf(size_t cfg) {
    for (Instance& i : instances_) {
        if (i.folder && i.cfg == cfg) return i.folder.get();
    }
    return nullptr;
}

size_t DockManager::FolderCount() const {
    size_t n = 0;
    for (const Instance& i : instances_) n += i.folder ? 1 : 0;
    return n;
}

void DockManager::OnExplorerRestarted() { DesktopLayer::Reassert(); }

bool DockManager::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == kFolderDeviceLost) {
        RecoverFolderDevice();
        return true;
    }
    if (msg == kTrayStartMessage) {
        trayStartPosted_ = false;
        // Still wanted (the config may have changed meanwhile)?
        const bool wanted = config_ && config_->tray.enabled &&
                            std::any_of(config_->docks.begin(), config_->docks.end(), [](const DockConfig& d) {
                                return d.Has(ItemKind::Tray) || d.Has(ItemKind::TrayApp);
                            });
        if (wanted && !tray_.Running()) {
            if (!tray_.Start(instance_)) OutputDebugStringW(L"WinDock: the tray host could not start\n");
            ForEach([](DockWindow& w) { w.OnTrayChanged(); });
        }
        return true;
    }
    if (tracker_.HandleMessage(msg, wp, lp)) return true;
    if (watch_.HandleMessage(msg, wp, lp)) return true;
    if (widgets_.HandleMessage(msg, wp, lp)) return true;  // the widget timers
    if (notifications_.HandleMessage(msg, wp, lp)) return true;
    if (calendar_.HandleMessage(msg, wp, lp)) return true;  // feed results, file changes, the refresh timer
    if (msg == WM_TIMECHANGE) calendar_.TimeChanged();     // (not consumed)
    return weather_.HandleMessage(msg, wp, lp);  // fetch results, the refresh timer
}

int DockManager::MonitorIndexFor(const DockConfig& d) const {
    if (d.monitor == DockConfig::kMonitorPrimary) return 0;
    if (d.monitor < 0) return -1;
    return d.monitor < static_cast<int>(monitors_.size()) ? d.monitor : -1;
}

HRESULT DockManager::CreateWindowFor(Instance& inst) {
    auto window = std::make_unique<DockWindow>();
    const HRESULT hr = window->Create(instance_, config_, inst.cfg, inst.monitor, this, callbacks_.hook);
    if (FAILED(hr)) {
        Close(std::move(window));
        inst.failed = true;
        return hr;
    }
    inst.failed = false;
    if (callbacks_.setup) callbacks_.setup(*window);
    inst.window = std::move(window);
    return S_OK;
}

HRESULT DockManager::Sync() {
    if (!config_ || syncing_) return S_OK;
    syncing_ = true;
    monitors_ = EnumerateMonitors();

    // What should exist: per config dock one instance, or one per monitor ("each").
    std::vector<Instance> next;
    for (size_t k = 0; k < config_->docks.size(); ++k) {
        const DockConfig& d = config_->docks[k];
        if (d.IsFolderWindow()) {
            Instance inst;
            inst.id = d.id;
            inst.cfg = k;
            inst.isFolder = true;
            inst.monitorIndex = MonitorIndexFor(d);
            inst.monitor = inst.monitorIndex >= 0 ? monitors_[static_cast<size_t>(inst.monitorIndex)] : nullptr;
            next.push_back(std::move(inst));
            continue;
        }
        if (d.Each()) {
            for (size_t m = 0; m < monitors_.size(); ++m) {
                Instance inst;
                inst.id = d.id;
                inst.cfg = k;
                inst.each = true;
                inst.monitor = monitors_[m];
                inst.monitorIndex = static_cast<int>(m);
                next.push_back(std::move(inst));
            }
        } else {
            Instance inst;
            inst.id = d.id;
            inst.cfg = k;
            inst.monitorIndex = MonitorIndexFor(d);
            inst.monitor = inst.monitorIndex >= 0 ? monitors_[static_cast<size_t>(inst.monitorIndex)] : nullptr;
            next.push_back(std::move(inst));
        }
    }

    // Reuse the windows of matching instances (same dock id; "each": the same monitor too).
    for (Instance& want : next) {
        if (want.isFolder) {
            for (Instance& old : instances_) {
                if (!old.folder || old.id != want.id || !want.monitor) continue;
                want.folder = std::move(old.folder);
                want.folder->SetConfigIndex(want.cfg);
                break;
            }
            continue;
        }
        for (Instance& old : instances_) {
            if (!old.window || old.id != want.id || old.each != want.each) continue;
            if (want.each && old.monitor != want.monitor) continue;
            if (!want.monitor) break;  // parked: the old window goes below
            want.window = std::move(old.window);
            want.window->SetConfigIndex(want.cfg);
            break;
        }
    }
    // Windows nobody took (removed docks, parked docks, monitors that went away) close; the list
    // is swapped in first so their app bar callbacks see the new state.
    std::vector<Instance> old = std::move(instances_);
    instances_ = std::move(next);
    for (Instance& o : old) {
        if (o.window) Close(std::move(o.window));
        if (o.folder) {
            o.folder->Destroy();
            folderGraveyard_.push_back(std::move(o.folder));
        }
    }
    old.clear();

    HRESULT first = S_OK;
    for (Instance& inst : instances_) {
        if (!inst.monitor) continue;  // parked
        if (inst.isFolder) {
            if (inst.folder) {
                inst.folder->SetMonitor(inst.monitor);
                inst.folder->ApplyConfig();  // whatever changed in its entry (cheap when nothing did)
            } else {
                CreateFolderFor(inst);  // a failure leaves it parked (it does not stop the docks)
            }
            continue;
        }
        if (inst.window) {
            inst.window->SetMonitor(inst.monitor);  // relayouts when it changed
            continue;
        }
        const HRESULT hr = CreateWindowFor(inst);
        if (FAILED(hr)) {
            if (SUCCEEDED(first)) first = hr;
            continue;
        }
        if (appBarAllowed_) inst.window->AllowAppBar(true);
    }
    // The weather service follows the docks that are shown (a parked weather dock does not fetch).
    ConfigureWeather();
    ConfigureCalendar();
    syncing_ = false;
    SyncTray();
    SyncNotifications();
    return first;
}

void DockManager::Close(std::unique_ptr<DockWindow> window) {
    if (!window) return;
    window->Destroy();
    graveyard_.push_back(std::move(window));
}

DockWindow* DockManager::ByHwnd(HWND hwnd) {
    if (!hwnd) return nullptr;
    for (Instance& i : instances_) {
        if (i.window && i.window->Hwnd() == hwnd) return i.window.get();
    }
    return nullptr;
}

DockWindow* DockManager::FirstOf(size_t cfg) {
    for (Instance& i : instances_) {
        if (i.window && i.cfg == cfg) return i.window.get();
    }
    return nullptr;
}

DockWindow* DockManager::First() {
    for (Instance& i : instances_) {
        if (i.window) return i.window.get();
    }
    return nullptr;
}

size_t DockManager::IndexOf(const DockWindow* window) const {
    for (size_t i = 0; i < instances_.size(); ++i) {
        if (instances_[i].window.get() == window) return i;
    }
    return SIZE_MAX;
}

void DockManager::AllowAppBar(bool allow) {
    appBarAllowed_ = allow;
    ForEach([&](DockWindow& w) { w.AllowAppBar(allow); });
}

std::vector<RECT> DockManager::ReservedMonitors() const {
    std::vector<RECT> rects;
    for (const Instance& i : instances_) {
        if (i.window && i.window->AppBarRegistered()) rects.push_back(i.window->MonitorRect());
    }
    return rects;
}

bool DockManager::AnyWeatherItem() const {
    if (!config_) return false;
    for (const DockConfig& d : config_->docks) {
        if (!d.Has(ItemKind::Weather)) continue;
        if (d.Each() || MonitorIndexFor(d) >= 0) return true;  // shown (not parked)
    }
    return false;
}

void DockManager::ConfigureCalendar() {
    if (config_) calendar_.Configure(config_->calendars, owner_);
}

void DockManager::ConfigureWeather() {
    if (!config_) return;
    weather_.Configure(config_->weather, AnyWeatherItem(), owner_);
}

void DockManager::SetWatching(DockWindow* dock, bool on) {
    const auto it = std::find(watchers_.begin(), watchers_.end(), dock);
    if (on) {
        if (it == watchers_.end()) watchers_.push_back(dock);
        // A running watch only takes the callback (no hooks installed twice).
        watch_.Start(owner_, [this] {
            const std::vector<DockWindow*> watchers = watchers_;  // a check may change subscriptions
            for (DockWindow* w : watchers) {
                if (std::find(watchers_.begin(), watchers_.end(), w) != watchers_.end()) w->OnWatchEvent();
            }
        });
        return;
    }
    if (it != watchers_.end()) watchers_.erase(it);
    if (watchers_.empty()) watch_.Stop();  // no dock hides intelligently: no hooks at all
}

bool DockManager::NotificationsWanted() const {
    if (!config_) return false;
    if (centreOpen_ || widgets_.NotificationUsers() > 0) return true;
    for (const Instance& i : instances_) {
        if (!i.window || i.cfg >= config_->docks.size()) continue;  // a parked dock shows nothing
        const DockConfig& d = config_->docks[i.cfg];
        if (d.badges || d.Has(ItemKind::Notifications)) return true;  // badges, the centre's bell
    }
    return false;
}

void DockManager::SyncNotifications() {
    if (!config_ || syncing_) return;
    const bool wanted = NotificationsWanted();
    if (wanted && !notifications_.Active()) {
        if (!notifications_.Start(owner_, config_->notifications.pollS)) {
            OutputDebugStringW(L"WinDock: the notification service could not start\n");
        }
    } else if (!wanted && notifications_.Active()) {
        notifications_.Stop();  // tells the docks (their badges go)
    }
    notifications_.SetPollSeconds(config_->notifications.pollS);
    notifications_.SetFast(centreOpen_);
    // Balloons go to the centre only while the service runs to take them.
    tray_.SetStripBalloons(notifications_.Active() && config_->notifications.balloons);
    ForEach([](DockWindow& w) { w.OnNotificationsChanged(); });  // badges on / off, exclusions
}

void DockManager::SetCentreOpen(bool open) {
    if (centreOpen_ == open) return;
    centreOpen_ = open;
    SyncNotifications();
    if (open) notifications_.PollNow();
}

bool DockManager::ActivateNotification(uint32_t id) {
    const NotificationItem* n = notifications_.ById(id);
    if (!n) return false;
    const AppKey key = n->key;
    notifications_.MarkRead(id);
    if (key.id.empty()) return true;  // an app we cannot name: read, nothing to bring up
    for (Instance& i : instances_) {
        if (i.window && i.window->ActivateKey(key)) {
            notifications_.MarkRead(key);
            return true;
        }
    }
    // Not on any dock: start it (an AUMID through shell:AppsFolder, an exe by its path).
    DockItem item;
    item.key = key;
    if (!Launcher::Launch(item)) MessageBeep(MB_ICONWARNING);
    notifications_.MarkRead(key);
    return true;
}
