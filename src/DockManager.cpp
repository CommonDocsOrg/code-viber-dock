#include "DockManager.h"

#include <algorithm>
#include <cmath>
#include <cwchar>

#include <dwmapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include "DesktopLayer.h"
#include "Launcher.h"
#include "ShellFiles.h"

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
    // Win+D raises Progman a moment after the desktop takes the foreground: the "desktop" docks and
    // widgets (not registered with the layer) are re-placed with the layer's late passes.
    DesktopLayer::SetLateCallback([this] { OnExplorerRestarted(); });
    widgets_.Initialize(owner_);
    // Script widgets use the service of their place (round 5: the item's own, else the dock's first
    // weather item's): created on first use; their first / last use and refresh() are posted here,
    // so services start, stop or fetch outside the script's call.
    widgets_.SetWeather([this](const WeatherConfig& place) { return WeatherFor(place); },
                        [this](WidgetRuntime::WeatherAction a, const WeatherConfig& place) {
                            if (a == WidgetRuntime::WeatherAction::Refresh) weatherRefresh_.push_back(place);
                            if (owner_) PostMessageW(owner_, kWeatherRequest, static_cast<WPARAM>(a), 0);
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
        for (Instance& i : instances_) {
            if (i.widget) i.widget->OnNotificationsChanged();  // an open centre over a standalone widget
        }
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
        for (Instance& i : instances_) {
            if (i.widget) i.widget->OnCalendarChanged();
        }
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

WidgetPreview::Result DockManager::RenderPreview(const std::wstring& id, float scale, UINT fitW, UINT fitH, float maxZoom) {
    if (!config_) return WidgetPreview::Result{nullptr, SIZE{}, L"no config", 0.0};
    const ScriptDockInfo dock = WidgetPreview::DockInfoFor(*config_, scale);
    if (fitW == 0) fitW = static_cast<UINT>(std::lround(1024.0f * dock.scale));
    if (fitH == 0) fitH = static_cast<UINT>(std::lround(512.0f * dock.scale));
    return previews_.Render(id, dock, config_->weather, fitW, fitH, maxZoom);
}

void DockManager::Shutdown() {
    if (!config_) return;
    DesktopLayer::SetLateCallback(nullptr);  // captured this
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
        if (i.widget) i.widget->Destroy();  // its widget unregisters from the runtime (still alive)
    }
    instances_.clear();
    folderGraveyard_.clear();
    widgetGraveyard_.clear();
    if (desktopNotify_) SHChangeNotifyDeregister(desktopNotify_);
    desktopNotify_ = 0;
    if (desktopTimer_ && owner_) KillTimer(owner_, kDesktopTimer);
    desktopTimer_ = false;
    if (folderRendererReady_) folderRenderer_.Shutdown();
    folderRendererReady_ = false;
    previews_.Release();
    watchers_.clear();
    dashboard_ = false;
    dashboardPrev_ = nullptr;
    fullscreenNow_.clear();
    abnMonitors_.clear();
    guardForce_ = -1;
    watch_.Stop();
    tracker_.Stop();
    widgets_.SetWeather(nullptr, nullptr);
    for (auto& w : weather_) {
        w->SetChangedCallback(nullptr);
        w->Stop();  // a fetch still in flight posts to a window that is gone (and frees it)
    }
    weather_.clear();
    weatherRefresh_.clear();
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
    widgetGraveyard_.clear();
    // The desktop windows' device goes with the last of them (tens of MB with a hardware device).
    if (folderRendererReady_ && FolderCount() == 0 && WidgetCount() == 0) {
        folderRenderer_.Shutdown();
        folderRendererReady_ = false;
    }
}

bool DockManager::EnsureDesktopRenderer() {
    if (folderRendererReady_) return true;
    const bool software = config_ && config_->renderer == L"software";
    if (FAILED(folderRenderer_.Initialize(owner_, software))) {
        folderRenderer_.Shutdown();
        return false;
    }
    folderRendererReady_ = true;
    return true;
}

void DockManager::RecoverDesktopDevice() {
    folderRecoveryPosted_ = false;
    if (!folderRendererReady_) return;
    for (Instance& i : instances_) {
        if (i.folder) i.folder->ReleaseDevice();
        if (i.widget) i.widget->ReleaseDevice();
    }
    if (FAILED(folderRenderer_.RecreateDevice())) {
        OutputDebugStringW(L"WinDock: the desktop windows' device could not be recreated\n");
        return;
    }
    for (Instance& i : instances_) {
        if (i.folder) i.folder->RestoreDevice();
        if (i.widget) i.widget->RestoreDevice();
    }
}

HRESULT DockManager::CreateWidgetFor(Instance& inst) {
    if (!EnsureDesktopRenderer()) {
        inst.failed = true;
        return E_FAIL;
    }
    auto widget = std::make_unique<WidgetWindow>();
    WidgetWindow::Host host;
    host.renderer = &folderRenderer_;
    host.shared = this;
    host.changed = [this] {
        if (callbacks_.widgetChanged) callbacks_.widgetChanged();
    };
    host.neighbours = [this](const WidgetWindow* self) { return FloatingRects(self); };
    host.deviceLost = [this] {
        if (!folderRecoveryPosted_) folderRecoveryPosted_ = PostMessageW(owner_, kFolderDeviceLost, 0, 0) != FALSE;
    };
    host.menu = [this](size_t cfg, POINT screen) {
        if (callbacks_.widgetMenu) callbacks_.widgetMenu(cfg, screen);
    };
    host.openSettings = [this](size_t cfg) {
        if (callbacks_.widgetSettings) callbacks_.widgetSettings(cfg);
    };
    const HRESULT hr = widget->Create(instance_, config_, inst.cfg, inst.monitor, std::move(host));
    if (FAILED(hr)) {
        widget->Destroy();
        inst.failed = true;
        return hr;
    }
    inst.failed = false;
    inst.widget = std::move(widget);
    return S_OK;
}

WidgetWindow* DockManager::WidgetOf(size_t cfg) {
    for (Instance& i : instances_) {
        if (i.widget && i.cfg == cfg) return i.widget.get();
    }
    return nullptr;
}

WidgetWindow* DockManager::WidgetByHwnd(HWND hwnd) {
    if (!hwnd) return nullptr;
    for (Instance& i : instances_) {
        if (i.widget && i.widget->Hwnd() == hwnd) return i.widget.get();
    }
    return nullptr;
}

size_t DockManager::WidgetCount() const {
    size_t n = 0;
    for (const Instance& i : instances_) n += i.widget ? 1 : 0;
    return n;
}

void DockManager::ApplyWidgetWindow(size_t cfg) {
    if (WidgetWindow* w = WidgetOf(cfg)) w->ApplyConfig();
}

bool DockManager::WidgetsWatching() const {
    for (const Instance& i : instances_) {
        if (i.widget && i.widget->WantsWatch()) return true;
    }
    return false;
}

HRESULT DockManager::CreateFolderFor(Instance& inst) {
    if (!EnsureDesktopRenderer()) {
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

void DockManager::OnExplorerRestarted() {
    DesktopLayer::Reassert();  // folder windows, "behind" docks and widgets (registered with the layer)
    ForEach([](DockWindow& w) { w.ReassertLayer(); });  // shown "desktop" docks
    for (Instance& i : instances_) {
        if (i.widget) i.widget->ReassertLayer();  // shown "desktop" standalone widgets
    }
}

std::wstring DockManager::DesktopFolder() {
    wchar_t buf[MAX_PATH * 2]{};
    const DWORD n = GetEnvironmentVariableW(L"WINDOCK_DESKTOP_DIR", buf, static_cast<DWORD>(std::size(buf)));
    if (n > 0 && n < std::size(buf)) return ShellFiles::Normalize(buf);
    PWSTR path = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &path)) && path) dir = path;
    if (path) CoTaskMemFree(path);
    return dir;
}

void DockManager::SyncDesktopRules() {
    const bool want = config_ && std::any_of(config_->docks.begin(), config_->docks.end(), [](const DockConfig& d) {
                          return d.IsFolderWindow() && d.folder.autoSort && d.folder.RuleCount() > 0;
                      });
    if (want && !desktopNotify_) {
        desktopDir_ = DesktopFolder();
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (!desktopDir_.empty() && SUCCEEDED(SHParseDisplayName(desktopDir_.c_str(), nullptr, &pidl, 0, nullptr)) && pidl) {
            SHChangeNotifyEntry entry{pidl, FALSE};
            desktopNotify_ = SHChangeNotifyRegister(owner_, SHCNRF_InterruptLevel | SHCNRF_ShellLevel | SHCNRF_NewDelivery,
                                                    SHCNE_CREATE | SHCNE_RENAMEITEM | SHCNE_UPDATEDIR, kDesktopChange, 1, &entry);
            CoTaskMemFree(pidl);
        }
        // Files already there when the watch starts follow the rules too.
        if (desktopNotify_ && !desktopTimer_) desktopTimer_ = SetTimer(owner_, kDesktopTimer, 500, nullptr) != 0;
    } else if (!want && desktopNotify_) {
        SHChangeNotifyDeregister(desktopNotify_);
        desktopNotify_ = 0;
        if (desktopTimer_) KillTimer(owner_, kDesktopTimer);
        desktopTimer_ = false;
    }
}

size_t DockManager::SortDesktop(bool onlyAuto) {
    if (!config_) return 0;
    if (rulesJobs_ > 0) {
        sortAgain_ = true;  // the files still moving would be found again
        return 0;
    }
    const std::wstring dir = desktopNotify_ ? desktopDir_ : DesktopFolder();
    if (dir.empty()) return 0;
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileExW(ShellFiles::Join(dir, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (find == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        names.emplace_back(fd.cFileName);
    } while (names.size() < 20000 && FindNextFileW(find, &fd));
    FindClose(find);
    size_t queued = 0;
    for (const DockConfig& d : config_->docks) {
        if (!d.IsFolderWindow() || d.folder.RuleCount() == 0 || names.empty() || (onlyAuto && !d.folder.autoSort)) continue;
        wchar_t buf[MAX_PATH * 2];
        const DWORD n = ExpandEnvironmentStringsW(d.folder.path.c_str(), buf, static_cast<DWORD>(std::size(buf)));
        const std::wstring dest = ShellFiles::Normalize(n > 0 && n <= std::size(buf) ? std::wstring(buf) : d.folder.path);
        const DWORD attr = GetFileAttributesW(dest.c_str());
        if (dest.empty() || attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY) || ShellFiles::SamePath(dest, dir)) {
            continue;  // no folder, or the Desktop itself
        }
        ShellFiles::Job job;
        job.op = ShellFiles::Op::Move;
        job.dest = dest;
        job.notify = owner_;
        job.message = kRulesJobDone;
        for (auto it = names.begin(); it != names.end();) {
            if (PathMatchSpecExW(it->c_str(), d.folder.rules.c_str(), PMSF_MULTIPLE) == S_OK) {
                job.files.push_back(ShellFiles::Join(dir, *it));
                // Never over a file of the same name: "name (2).ext" (none of this batch's names either).
                job.newNames.push_back(ShellFiles::FreeName(dest, *it, job.newNames));
                it = names.erase(it);  // the first matching window takes it
            } else {
                ++it;
            }
        }
        if (job.files.empty()) continue;
        const size_t count = job.files.size();
        if (ShellFiles::Start(std::move(job))) {
            ++rulesJobs_;
            queued += count;
            rulesMoved_ += count;
        }
    }
    return queued;
}

bool DockManager::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == kFolderDeviceLost) {
        RecoverDesktopDevice();
        return true;
    }
    if (msg == kDesktopChange) {
        PIDLIST_ABSOLUTE* pidls = nullptr;
        LONG event = 0;
        if (HANDLE lock = SHChangeNotification_Lock(reinterpret_cast<HANDLE>(wp), static_cast<DWORD>(lp), &pidls, &event)) {
            SHChangeNotification_Unlock(lock);
        }
        if (desktopNotify_ && !desktopTimer_) desktopTimer_ = SetTimer(owner_, kDesktopTimer, 500, nullptr) != 0;
        return true;
    }
    if (msg == WM_TIMER && wp == kDesktopTimer) {
        KillTimer(owner_, kDesktopTimer);  // one-shot
        desktopTimer_ = false;
        SortDesktop(true);
        return true;
    }
    if (msg == kRulesJobDone) {
        if (rulesJobs_ > 0) --rulesJobs_;
        if (rulesJobs_ == 0 && sortAgain_) {
            sortAgain_ = false;
            if (!desktopTimer_) desktopTimer_ = SetTimer(owner_, kDesktopTimer, 500, nullptr) != 0;
        }
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
    if (msg == kWeatherRequest) {
        if (static_cast<WidgetRuntime::WeatherAction>(wp) == WidgetRuntime::WeatherAction::Refresh) {
            const std::vector<WeatherConfig> places = std::move(weatherRefresh_);
            weatherRefresh_.clear();
            for (const WeatherConfig& p : places) RefreshWeather(&p);
        } else {
            SyncWeather();
        }
        return true;
    }
    if (msg == WeatherService::kResultMessage) {
        // A fetch finished: the service whose fetch it is (generations are process-wide), else
        // nobody wants it any more (the service was dropped meanwhile) and it is freed here.
        for (auto& w : weather_) {
            if (w->HandleMessage(msg, wp, lp)) return true;
        }
        delete reinterpret_cast<WeatherService::Response*>(lp);
        return true;
    }
    if (msg == WM_TIMER && wp >= kWeatherTimerBase && wp < kWeatherTimerBase + 256) {
        for (auto& w : weather_) {
            if (w->HandleMessage(msg, wp, lp)) return true;
        }
        KillTimer(owner_, wp);  // a dropped service's
        return true;
    }
    return false;
}

std::unique_ptr<WeatherService> DockManager::MakeWeather(const WeatherConfig& place) {
    auto service = std::make_unique<WeatherService>();
    service->SetTimerId(FreeWeatherTimer());
    // Not started (shown = false): it answers for this place (not configured / no data) until
    // SyncWeather configures it. Before the callback: WeatherFor may run inside a script call.
    service->Configure(place, false, owner_);
    WeatherService* raw = service.get();
    service->SetChangedCallback([this, raw] {
        ForEach([](DockWindow& w) { w.OnWeatherChanged(); });
        widgets_.NotifyWeather(raw->Settings());  // the widgets of this place (never inside a script call)
        if (callbacks_.weatherChanged) callbacks_.weatherChanged();
    });
    return service;
}

UINT_PTR DockManager::FreeWeatherTimer() const {
    for (UINT_PTR id = kWeatherTimerBase; id < kWeatherTimerBase + 256; ++id) {
        if (std::none_of(weather_.begin(), weather_.end(), [&](const auto& w) { return w->TimerId() == id; })) return id;
    }
    return kWeatherTimerBase + 255;  // 256 places at once: they share the last id (not expected)
}

const WeatherService* DockManager::FindWeather(const WeatherConfig& place) const {
    for (const auto& w : weather_) {
        if (WeatherService::SamePlace(w->Settings(), place)) return w.get();
    }
    return nullptr;
}

WeatherService* DockManager::WeatherFor(const WeatherConfig& place) {
    for (auto& w : weather_) {
        if (WeatherService::SamePlace(w->Settings(), place)) return w.get();
    }
    if (!config_) return nullptr;
    // The next SyncWeather (posted by a widget's first use; run by the dock after an item change)
    // starts it.
    weather_.push_back(MakeWeather(place));
    return weather_.back().get();
}

std::vector<WeatherConfig> DockManager::WantedWeather() const {
    std::vector<WeatherConfig> wanted;
    if (!config_) return wanted;
    auto add = [&](const WeatherConfig& w) {
        if (std::none_of(wanted.begin(), wanted.end(), [&](const WeatherConfig& o) { return WeatherService::SamePlace(o, w); })) {
            wanted.push_back(w);
        }
    };
    for (const DockConfig& d : config_->docks) {
        if (!d.IsDock() || !(d.Each() || MonitorIndexFor(d) >= 0)) continue;  // parked: nothing fetches
        for (const DockItemEntry& e : d.items) {
            if (e.Kind() != ItemKind::Weather) continue;
            const WeatherConfig w = WeatherOf(*config_, e);
            if (w.Configured()) add(w);  // without a place the item shows the map pin: no service
        }
    }
    for (const WeatherConfig& w : widgets_.WeatherUserConfigs()) add(w);  // also unconfigured: get() answers
    return wanted;
}

void DockManager::SyncWeather() {
    if (!config_) return;
    if (weatherSyncing_) {  // a change notification asked again: once this one is done
        if (owner_) PostMessageW(owner_, kWeatherRequest, static_cast<WPARAM>(WidgetRuntime::WeatherAction::Sync), 0);
        return;
    }
    weatherSyncing_ = true;
    const std::vector<WeatherConfig> wanted = WantedWeather();
    // The services nobody wants go (stopped first, freed after the notifications below).
    std::vector<std::unique_ptr<WeatherService>> dropped;
    for (auto it = weather_.begin(); it != weather_.end();) {
        const bool keep = std::any_of(wanted.begin(), wanted.end(),
                                      [&](const WeatherConfig& w) { return WeatherService::SamePlace(w, (*it)->Settings()); });
        if (keep) {
            ++it;
            continue;
        }
        (*it)->SetChangedCallback(nullptr);
        (*it)->Stop();
        dropped.push_back(std::move(*it));
        it = weather_.erase(it);
    }
    // Then every wanted place runs (a new one loads its cache and fetches; a running one only takes
    // the new name). Pointers first: a notification may add a service (a widget's first call).
    std::vector<std::pair<WeatherService*, WeatherConfig>> run;
    for (const WeatherConfig& w : wanted) run.emplace_back(WeatherFor(w), w);
    for (auto& [service, w] : run) {
        if (service) service->Configure(w, true, owner_);
    }
    if (!dropped.empty()) ForEach([](DockWindow& w) { w.OnWeatherChanged(); });  // elements whose service went
    dropped.clear();
    weatherSyncing_ = false;
}

void DockManager::RefreshWeather(const WeatherConfig* place) {
    for (auto& w : weather_) {
        if (!place || WeatherService::SamePlace(w->Settings(), *place)) w->RefreshNow();
    }
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
        if (d.IsFolderWindow() || d.IsWidgetWindow()) {
            Instance inst;
            inst.id = d.id;
            inst.cfg = k;
            inst.isFolder = d.IsFolderWindow();
            inst.isWidget = d.IsWidgetWindow();
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
        if (want.isWidget) {
            for (Instance& old : instances_) {
                if (!old.widget || old.id != want.id || !want.monitor) continue;
                want.widget = std::move(old.widget);
                want.widget->SetConfigIndex(want.cfg);
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
        if (o.widget) {
            o.widget->Destroy();
            widgetGraveyard_.push_back(std::move(o.widget));
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
        if (inst.isWidget) {
            if (inst.widget) {
                inst.widget->SetMonitor(inst.monitor);
                inst.widget->ApplyConfig();
            } else {
                CreateWidgetFor(inst);  // a failure leaves it parked
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
    // The weather services follow the docks that are shown (a parked weather dock does not fetch).
    SyncWeather();
    ConfigureCalendar();
    syncing_ = false;
    SyncTray();
    SyncNotifications();
    SyncDesktopRules();
    // A "desktop" standalone widget follows the foreground (the shared watch), so does the
    // full-screen guard (round 9 phase 9A); none: the watch stops unless docks or the dashboard
    // need it. The new windows are then hidden when a full-screen app is in front on their monitor.
    SyncFullscreenGuard();
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

void DockManager::ConfigureCalendar() {
    if (config_) calendar_.Configure(config_->calendars, owner_);
}

void DockManager::StartWatch() {
    // A running watch only takes the callback (no hooks installed twice).
    watch_.Start(owner_, [this] {
        if (dashboard_) CheckDashboardForeground();  // first: leaving re-places the docks below
        // Round 9 phase 9A: before the docks' own rules (a dock shown again re-tests its overlap).
        EvaluateFullscreen();
        const std::vector<DockWindow*> watchers = watchers_;  // a check may change subscriptions
        for (DockWindow* w : watchers) {
            if (std::find(watchers_.begin(), watchers_.end(), w) != watchers_.end()) w->OnWatchEvent();
        }
        for (Instance& i : instances_) {
            if (i.widget) i.widget->OnWatchEvent();  // the "desktop" layer's rule
        }
        // The desktop came forward (Win+D): Progman rises above our windows a moment later. The
        // layer's own hook covers its registered windows only while one exists; this covers the
        // "desktop" docks and widgets (and Place is a no-op for the registered ones).
        if (watchFg_ != GetForegroundWindow()) {
            watchFg_ = GetForegroundWindow();
            if (DesktopLayer::DesktopForeground() && watchFg_ && !DesktopLayer::LatePending()) DesktopLayer::DesktopShown();
        }
    });
}

void DockManager::SetWatching(DockWindow* dock, bool on) {
    const auto it = std::find(watchers_.begin(), watchers_.end(), dock);
    if (on) {
        if (it == watchers_.end()) watchers_.push_back(dock);
        StartWatch();
        return;
    }
    if (it != watchers_.end()) watchers_.erase(it);
    if (!WatchWanted()) watch_.Stop();  // nobody needs it: no hooks at all
}

bool DockManager::WatchWanted() const {
    return !watchers_.empty() || dashboard_ || WidgetsWatching() || FullscreenGuardOn() || guardForce_ >= 0;
}

std::vector<RECT> DockManager::FloatingNeighbours(const DockWindow* self) { return FloatingRects(self); }

std::vector<RECT> DockManager::FloatingRects(const void* self) const {
    std::vector<RECT> rects;
    for (const Instance& i : instances_) {
        if (i.window && i.window.get() != self && i.window->FloatingShown()) rects.push_back(i.window->RestBarRect());
        if (i.widget && i.widget.get() != self && i.widget->Shown()) rects.push_back(i.widget->ScreenRect());
    }
    return rects;
}

namespace {

bool OwnWindow(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid == GetCurrentProcessId();
}

// The window to give the foreground back to when dashboard mode ends: the foreground window, or
// (one of ours has it: the tray menu, a settings window) the first visible app window of another
// process from the top of the z-order.
HWND ForegroundToRestore() {
    HWND fg = GetForegroundWindow();
    if (fg && !OwnWindow(fg)) return fg;
    for (HWND w = GetTopWindow(nullptr); w; w = GetWindow(w, GW_HWNDNEXT)) {
        if (!IsWindowVisible(w) || IsIconic(w) || OwnWindow(w)) continue;
        const LONG_PTR ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
        if (ex & (WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE)) continue;
        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) continue;
        RECT r{};
        if (!GetWindowRect(w, &r) || r.right <= r.left || r.bottom <= r.top) continue;
        return w;
    }
    return nullptr;
}

}  // namespace

void DockManager::SetDashboard(bool on) {
    if (on == dashboard_ || !config_) return;
    if (on) {
        dashboardPrev_ = ForegroundToRestore();
        dashboard_ = true;
        HWND first = nullptr;
        for (Instance& i : instances_) {
            if (!i.window || i.cfg >= config_->docks.size() || config_->docks[i.cfg].layer == DockLayer::Normal) continue;
            i.window->OnDashboardChanged();
            if (!first) first = i.window->Hwnd();
        }
        // Round 6 phase 6B: the standalone widgets on those layers too (after the docks: a dock, when
        // there is one, takes the keyboard).
        for (Instance& i : instances_) {
            if (!i.widget || i.cfg >= config_->docks.size() || config_->docks[i.cfg].layer == DockLayer::Normal) continue;
            i.widget->OnDashboardChanged();
            if (!first) first = i.widget->Hwnd();
        }
        // The keyboard to the first of them (Escape ends the mode there); WM_HOTKEY's input lets this
        // process take the foreground. A click on another window then is a foreground change.
        if (first && SetForegroundWindow(first)) SetFocus(first);
        StartWatch();
        return;
    }
    dashboard_ = false;
    // One of our docks has the foreground (Escape, the hotkey): back to the window that had it, so a
    // "desktop" dock hides again unless that was the desktop.
    HWND fg = GetForegroundWindow();
    if (fg && (ByHwnd(fg) || WidgetByHwnd(fg)) && dashboardPrev_ && IsWindow(dashboardPrev_) && IsWindowVisible(dashboardPrev_)) {
        SetForegroundWindow(dashboardPrev_);
    }
    dashboardPrev_ = nullptr;
    ForEach([](DockWindow& w) { w.OnDashboardChanged(); });
    for (Instance& i : instances_) {
        if (i.widget) i.widget->OnDashboardChanged();
    }
    if (!WatchWanted()) watch_.Stop();
    EvaluateFullscreen();  // a dock that left the topmost band is not the guard's any more
}

void DockManager::CheckDashboardForeground() {
    HWND fg = GetForegroundWindow();
    if (!fg || OwnWindow(fg)) return;  // none, a dock, a popup, a settings window
    HWND root = GetAncestor(fg, GA_ROOT);
    wchar_t cls[32]{};
    if (root && GetClassNameW(root, cls, 32) && (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0)) {
        return;  // the desktop (a click on the wallpaper): the docks stay up
    }
    SetDashboard(false);
}

// --- the full-screen guard (round 9 phase 9A) -------------------------------------------------------

void DockManager::SyncFullscreenGuard() {
    if (!config_) return;
    if (!FullscreenGuardOn()) {
        guardForce_ = -1;  // "ignore": a forced state (DockCommand 150) ends with the guard
        abnMonitors_.clear();
    }
    if (WatchWanted()) {
        StartWatch();  // a running watch only takes the callback
    } else {
        watch_.Stop();
    }
    EvaluateFullscreen();
}

void DockManager::ForceFullscreen(int mode) {
    guardForce_ = mode == 0 || mode == 1 ? mode : -1;  // 2 (or anything else): the real evaluation again
    SyncFullscreenGuard();
}

bool DockManager::FullscreenNotice(DockWindow* dock, bool on) {
    if (!FullscreenGuardOn() || !dock) return false;
    const size_t index = IndexOf(dock);
    HMONITOR monitor = index < instances_.size() ? instances_[index].monitor : nullptr;
    if (!monitor) monitor = MonitorFromWindow(dock->Hwnd(), MONITOR_DEFAULTTONEAREST);
    std::erase_if(abnMonitors_, [&](const auto& a) { return a.first == monitor; });
    // The notice names no window: it counts for the window in front when it came (the full-screen
    // one), so a notice whose "closed" never arrives (the app bar went away meanwhile) cannot hide
    // the docks for another window later.
    HWND fg = GetForegroundWindow();
    if (HWND root = fg ? GetAncestor(fg, GA_ROOT) : nullptr) fg = root;
    if (on && fg && !OwnWindow(fg)) abnMonitors_.emplace_back(monitor, fg);
    EvaluateFullscreen();
    return true;
}

std::vector<HMONITOR> DockManager::FullscreenMonitors() const {
    std::vector<HMONITOR> full;
    if (!FullscreenGuardOn()) return full;
    if (guardForce_ == 1) return monitors_;
    if (guardForce_ == 0) return full;
    // Only the foreground window can be a full-screen app in front: not one of ours (a settings
    // window, a popup), not the desktop / a taskbar / Start (WindowTracker::IsExcludedClass), shown,
    // not cloaked (another virtual desktop), not minimized.
    HWND fg = GetForegroundWindow();
    if (!fg) return full;
    if (HWND root = GetAncestor(fg, GA_ROOT)) fg = root;
    if (OwnWindow(fg) || WindowTracker::IsExcludedClass(fg) || !IsWindowVisible(fg) || IsIconic(fg)) return full;
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(fg, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return full;
    const HMONITOR fgMonitor = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    // 1. Its visible frame covers a whole monitor, and it is not an ordinary maximized window (a
    //    captioned window maximized while the taskbar is hidden covers the monitor too).
    RECT r{};
    const bool haveRect = SUCCEEDED(DwmGetWindowAttribute(fg, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))) || GetWindowRect(fg, &r);
    const bool captionedMax = IsZoomed(fg) && (GetWindowLongW(fg, GWL_STYLE) & WS_CAPTION) == WS_CAPTION;
    if (haveRect && !captionedMax) {
        for (HMONITOR m : monitors_) {
            MONITORINFO mi{sizeof(mi)};
            if (!GetMonitorInfoW(m, &mi)) continue;
            const RECT& mr = mi.rcMonitor;
            if (r.left <= mr.left && r.top <= mr.top && r.right >= mr.right && r.bottom >= mr.bottom) full.push_back(m);
        }
    }
    auto add = [&](HMONITOR m) {
        if (m && std::find(full.begin(), full.end(), m) == full.end()) full.push_back(m);
    };
    // 2. The shell says a Direct3D full-screen app or presentation mode runs: the foreground
    //    window's monitor.
    if (std::find(full.begin(), full.end(), fgMonitor) == full.end()) {
        QUERY_USER_NOTIFICATION_STATE quns{};
        if (SUCCEEDED(SHQueryUserNotificationState(&quns)) &&
            (quns == QUNS_RUNNING_D3D_FULL_SCREEN || quns == QUNS_PRESENTATION_MODE)) {
            add(fgMonitor);
        }
    }
    // 3. An app bar dock on the foreground window's monitor was told a full-screen app opened there
    //    (ABN_FULLSCREENAPP) while this window was in front, and the shell has not said it closed.
    for (const auto& [m, w] : abnMonitors_) {
        if (m == fgMonitor && w == fg) add(fgMonitor);
    }
    return full;
}

void DockManager::EvaluateFullscreen() {
    if (!config_) return;
    std::vector<HMONITOR> full = FullscreenMonitors();
    const bool changed = full != fullscreenNow_;
    fullscreenNow_ = std::move(full);
    auto onFull = [&](HMONITOR m) {
        return m && std::find(fullscreenNow_.begin(), fullscreenNow_.end(), m) != fullscreenNow_.end();
    };
    // Every live window follows (also when nothing changed: a new window, a layer change).
    for (Instance& i : instances_) {
        if (i.window) i.window->SetFullscreenHidden(onFull(i.monitor) && i.window->TopmostBand());
        if (i.widget) i.widget->SetFullscreenHidden(onFull(i.monitor) && i.widget->TopmostBand());
    }
    if (changed) {
        wchar_t buf[96];
        swprintf_s(buf, L"WinDock: full-screen guard: %zu monitor(s) full screen\n", fullscreenNow_.size());
        OutputDebugStringW(buf);
    }
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
