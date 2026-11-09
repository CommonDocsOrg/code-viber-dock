#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "CalendarService.h"
#include "Config.h"
#include "DockWindow.h"
#include "FolderWindow.h"
#include "ForegroundWatch.h"
#include "NotificationService.h"
#include "TrayHost.h"
#include "Weather.h"
#include "WidgetPreview.h"
#include "WidgetWindow.h"
#include "WindowTracker.h"

// Owns every dock window and the services they share (one each per process): the window tracker,
// the weather services (one per place shown, round 5) and the foreground watch, all bound to the
// app's own hidden window
// (`owner`), which feeds them its messages (HandleMessage). The taskbar controller stays with App
// (it is not a dock service); each dock keeps its own renderer, icon cache and popups.
//
// Instances: one per config dock, in config order; a dock with monitor "each" has one per
// connected monitor (in monitor order), all showing the same config (per-instance state such as
// the running apps' drag order is session-only). A dock whose monitor is not connected is
// parked: it keeps its place in the list (automation index, QueryState 49) but has no window
// until the monitor returns. Sync() re-reads the config and the monitors and makes the windows
// match: instances are matched by dock id (and by monitor for "each"), so a reused window keeps
// its state.
class DockManager final : public DockShared {
public:
    struct Instance {
        std::wstring id;          // config dock id
        size_t cfg = 0;           // index into config->docks
        bool each = false;        // one of a monitor "each" dock's instances
        HMONITOR monitor = nullptr;  // the monitor it is on (null: parked)
        int monitorIndex = -1;    // index into Monitors() (-1: parked)
        bool failed = false;      // the window could not be created (counts as parked)
        std::unique_ptr<DockWindow> window;  // null while parked
        // A docks[] entry of kind "folderWindow" (round 4 phase 12): its window instead (null while
        // parked); `window` stays null. Automation addresses it by the same index.
        bool isFolder = false;
        std::unique_ptr<FolderWindow> folder;
        // Round 6 phase 6B: a docks[] entry of kind "widget" (a standalone widget): its window (null
        // while parked); addressed by the same index.
        bool isWidget = false;
        std::unique_ptr<WidgetWindow> widget;
        bool Live() const { return window || folder || widget; }
    };

    struct Callbacks {
        DockWindow::MessageHook hook;                // every dock window's message hook (App)
        std::function<void(DockWindow&)> setup;      // a dock window was created (pin requests ...)
        std::function<void()> weatherChanged;        // the weather data / status changed (once per change)
        std::function<void()> notificationsChanged;  // the notification list / service state changed
        std::function<void()> calendarChanged;       // the calendar feeds' data / status changed
        // Folder windows: one changed its config entry (moved, resized, collapsed, a menu
        // setting); asks to remove config dock `cfg`; "New folder window…" from its menu.
        std::function<void()> folderChanged;
        std::function<void(size_t cfg)> folderRemove;
        std::function<void()> folderNew;
        // Standalone widgets (round 6 phase 6B): one was moved / resized (save, refresh Settings);
        // its right-click menu (App runs it); its widget's settings (weather.openSettings()).
        std::function<void()> widgetChanged;
        std::function<void(size_t cfg, POINT screen)> widgetMenu;
        std::function<void(size_t cfg)> widgetSettings;
        // Round 12 phase 12I: a folder window or standalone widget moved, was resized or hidden /
        // shown (FolderWindow / WidgetWindow Host::moved): the settings panel beside it follows. May
        // come from inside Sync (SetMonitor): the receiver must not walk the instances synchronously.
        std::function<void()> desktopMoved;
    };

    DockManager() = default;
    DockManager(const DockManager&) = delete;
    DockManager& operator=(const DockManager&) = delete;
    ~DockManager() { Shutdown(); }

    // Starts the shared services on `owner` (the app's hidden window). Sync() creates the docks.
    void Initialize(HINSTANCE instance, Config* config, HWND owner, Callbacks callbacks);
    // Destroys every dock window, then stops the services.
    void Shutdown();
    // Re-enumerates the monitors and creates / parks / re-points / destroys dock windows to match
    // config->docks. Returns the first creation failure (S_OK if every window could be created).
    HRESULT Sync();
    // Frees the windows Sync / Shutdown closed (their C++ objects outlive their HWNDs until then:
    // a dock may be closed from inside one of its own messages, e.g. Reload config in its menu).
    // Call from a posted message.
    void Reap();
    // The owner window's messages for the shared services (tracker, watch, weather). True if handled.
    bool HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

    // The monitors, primary first, then the others in EnumDisplayMonitors order (the order of
    // docks[].monitor indexes).
    const std::vector<HMONITOR>& Monitors() const { return monitors_; }
    static std::vector<HMONITOR> EnumerateMonitors();
    // The monitor index a config dock (not "each") is on, or -1 when its monitor is not connected.
    int MonitorIndexFor(const DockConfig& d) const;

    const std::vector<Instance>& Instances() const { return instances_; }
    size_t Count() const { return instances_.size(); }
    // Automation dock index -> instance (null: no such dock).
    Instance* At(size_t index) { return index < instances_.size() ? &instances_[index] : nullptr; }
    DockWindow* WindowAt(size_t index) { return index < instances_.size() ? instances_[index].window.get() : nullptr; }
    DockWindow* ByHwnd(HWND hwnd);
    // The first live window of config dock `cfg` (null: every instance is parked).
    DockWindow* FirstOf(size_t cfg);
    // The first live window of any dock (null: all parked).
    DockWindow* First();
    // The automation index of a window (SIZE_MAX: not ours).
    size_t IndexOf(const DockWindow* window) const;
    // --- folder windows (round 4 phase 12) ---
    FolderWindow* FolderAt(size_t index) { return index < instances_.size() ? instances_[index].folder.get() : nullptr; }
    FolderWindow* FolderOf(size_t cfg);
    size_t FolderCount() const;
    // --- standalone widgets (round 6 phase 6B) ---
    WidgetWindow* WidgetAt(size_t index) { return index < instances_.size() ? instances_[index].widget.get() : nullptr; }
    WidgetWindow* WidgetOf(size_t cfg);
    WidgetWindow* WidgetByHwnd(HWND hwnd);
    size_t WidgetCount() const;
    // Config entry `cfg` (a widget entry) changed its widget or settings: its window follows.
    void ApplyWidgetWindow(size_t cfg);
    // Explorer restarted (TaskbarCreated): the desktop windows go back above the new desktop.
    void OnExplorerRestarted();
    // --- desktop rules (12b) ---
    // The Desktop's files (not folders, not hidden / system ones) matching some folder window
    // entry's `rules` move into its folder (docks[] order: the first match wins), renamed rather
    // than overwritten; one undoable IFileOperation per folder. Returns the files queued (0 while
    // an earlier sort's moves are still running: it sorts again when they end). `onlyAuto` (the
    // Desktop watch): only the windows with autoSort.
    size_t SortDesktop(bool onlyAuto = false);
    // Watches the Desktop (SHChangeNotifyRegister on the owner window, coalesced by a 500 ms
    // one-shot) while some folder window has rules with autoSort; nothing otherwise.
    void SyncDesktopRules();
    size_t RulesMoved() const { return rulesMoved_; }
    bool DesktopWatched() const { return desktopNotify_ != 0; }
    // The Desktop folder: the environment variable WINDOCK_DESKTOP_DIR (tests: a scratch folder,
    // never the real Desktop), else the user's Desktop (FOLDERID_Desktop).
    static std::wstring DesktopFolder();
    template <class F>
    void ForEach(F&& f) {
        for (Instance& i : instances_) {
            if (i.window) f(*i.window);
        }
    }
    template <class F>
    void ForConfig(size_t cfg, F&& f) {
        for (Instance& i : instances_) {
            if (i.window && i.cfg == cfg) f(*i.window);
        }
    }

    // Every dock's app bar: allowed / withdrawn (see DockWindow::AllowAppBar); new docks follow it.
    void AllowAppBar(bool allow);
    bool AppBarAllowed() const { return appBarAllowed_; }
    // The monitors (rcMonitor) of every dock that reserves space right now.
    std::vector<RECT> ReservedMonitors() const;

    // --- DockShared ---
    WindowTracker& Tracker() override { return tracker_; }
    CalendarService& Calendar() override { return calendar_; }
    const CalendarService& Calendar() const { return calendar_; }
    // Re-applies config.calendars (the service runs while some feed is enabled).
    void ConfigureCalendar();
    const WeatherService* FindWeather(const WeatherConfig& place) const override;
    void SyncWeather() override;
    void RefreshWeather(const WeatherConfig* place) override;
    // The service of `place`, created (not started: SyncWeather configures it) when there is none.
    WeatherService* WeatherFor(const WeatherConfig& place);
    // Weather services alive (QueryState 220).
    size_t WeatherServiceCount() const { return weather_.size(); }
    ForegroundWatch& Watch() override { return watch_; }
    void SetWatching(DockWindow* dock, bool on) override;
    WidgetRuntime& Widgets() override { return widgets_; }
    // Round 6 phase 6E: widget `id`'s live preview at `scale` (px per DIP) on the virtual dock
    // (WidgetPreview::DockInfoFor: the global theme, icon size 48), fitted into fitW x fitH px
    // (0: 1024 x 512 DIPs), drawn up to `maxZoom` times larger when it is smaller than that.
    WidgetPreview& Previews() { return previews_; }
    WidgetPreview::Result RenderPreview(const std::wstring& id, float scale, UINT fitW = 0, UINT fitH = 0, float maxZoom = 1.0f);
    TrayHost& TrayService() override { return tray_; }
    NotificationService& Notifications() override { return notifications_; }
    const NotificationService& Notifications() const { return notifications_; }
    // The app of notification `id` to the front (its slot on the first dock showing it, else a
    // launch of its AUMID / exe), its notifications marked read. False: unknown id.
    bool ActivateNotification(uint32_t id) override;
    // The centre popup opened / closed (it polls every second while open, and keeps the service
    // running even when nothing else needs it).
    void SetCentreOpen(bool open) override;
    // Round 6 phase 6B: every floating thing's rect (the shown floating docks' rest bars and the
    // shown standalone widgets' windows) but `self`'s; DockWindow and WidgetWindow snap to them.
    std::vector<RECT> FloatingNeighbours(const DockWindow* self) override;
    std::vector<RECT> FloatingRects(const void* self) const;
    bool DashboardOn() const override { return dashboard_; }
    void EndDashboard() override { SetDashboard(false); }

    // --- the dashboard (round 6 phase 6A) ---
    // On: every dock and standalone widget (6B) with layer "desktop" or "behind" is shown, revealed
    // and topmost (a dock's popups
    // work as usual); the first of them takes the foreground (the hotkey's input allows it), so
    // Escape reaches it and a click on any other window is a foreground change. Off again by the
    // hotkey (App), Escape in one of its docks (EndDashboard) or another app's window taking the
    // foreground (the shared foreground watch runs while the mode lasts; the desktop itself (a
    // click on the wallpaper) and WinDock's own windows keep it). Off: the foreground goes back to
    // the window that had it before (when one of our docks still has it), then each dock follows
    // its layer's rules again. Normal docks are never touched.
    void SetDashboard(bool on);
    bool Dashboard() const { return dashboard_; }

    // --- the full-screen guard (round 9 phase 9A) ---
    // With settings.fullscreen "hide" (the default) the foreground watch runs always (its hooks
    // only fire on events: nothing polls) and every evaluation decides, per monitor, whether a
    // full-screen app is in front there (FullscreenMonitors). While one is, every WinDock window on
    // that monitor in the topmost band (docks of any auto-hide mode, standalone widgets; not the
    // "desktop" / "behind" layers, not folder windows) is hidden without any z-order call
    // (DockWindow / WidgetWindow::SetFullscreenHidden) and comes back (topmost once) when the
    // monitor is no longer full screen. An app bar dock's ABN_FULLSCREENAPP feeds the same state
    // (FullscreenNotice). "ignore": nothing is hidden (an app bar dock steps below the app as before).
    bool FullscreenGuardOn() const { return config_ && config_->settings.fullscreenHide; }
    // The setting changed / Sync: the watch runs while the guard is on; then an evaluation.
    void SyncFullscreenGuard();
    // DockCommand 150 (tests): 1 every monitor counts as full screen, 0 none, until 2 ("release")
    // goes back to the real evaluation (and evaluates now). The guard must be on.
    void ForceFullscreen(int mode);
    // Monitors with a full-screen app in front at the last evaluation.
    const std::vector<HMONITOR>& FullscreenMonitorsNow() const { return fullscreenNow_; }
    bool FullscreenNotice(DockWindow* dock, bool on) override;

    // The notification service runs while it is wanted (NotificationsWanted): some shown dock has
    // badges on, a widget uses the provider, or the centre popup is open. Sync, the settings and
    // the widget runtime call it; it also applies notifications.pollS / .balloons.
    void SyncNotifications();
    bool NotificationsWanted() const;

    // The tray takeover runs while config tray.enabled is on and any dock has a "tray" or
    // "trayApp" item (Sync and every item change call it). Starting is posted (kTrayStartMessage):
    // the apps answer the TaskbarCreated broadcast at once and their Shell_NotifyIcon calls must
    // find this thread pumping messages, not busy starting up (they fail on a timeout otherwise).
    // Stopping is immediate. Returns whether the takeover is wanted.
    bool SyncTray();
    static constexpr UINT kTrayStartMessage = WM_APP + 45;  // posted to the owner window

private:
    HRESULT CreateWindowFor(Instance& inst);
    HRESULT CreateFolderFor(Instance& inst);
    HRESULT CreateWidgetFor(Instance& inst);
    // The desktop windows' shared device (the folder windows' and, since round 6 phase 6B, the
    // standalone widgets'): made with the first such window, released after the last.
    bool EnsureDesktopRenderer();
    void RecoverDesktopDevice();
    // The foreground watch is wanted by a standalone widget on the "desktop" layer.
    bool WidgetsWatching() const;
    static constexpr UINT kFolderDeviceLost = WM_APP + 52;  // posted to the owner window
    static constexpr UINT kDesktopChange = WM_APP + 53;     // SHChangeNotify on the Desktop (rules)
    static constexpr UINT kRulesJobDone = WM_APP + 54;      // a rules move finished
    static constexpr UINT kWeatherRequest = WM_APP + 55;    // wParam: WidgetRuntime::WeatherAction
    static constexpr UINT_PTR kDesktopTimer = 43;           // owner timer: coalesces Desktop changes
    // The weather services' refresh timers on the owner window: kWeatherTimerBase + n (n < 256).
    static constexpr UINT_PTR kWeatherTimerBase = 0x5700;
    // The places shown now (unique by WeatherService::SamePlace; the first wanter's names).
    std::vector<WeatherConfig> WantedWeather() const;
    UINT_PTR FreeWeatherTimer() const;
    std::unique_ptr<WeatherService> MakeWeather(const WeatherConfig& place);
    // Closes the window now (DestroyWindow) and keeps the object until Reap / the destructor.
    void Close(std::unique_ptr<DockWindow> window);

    HINSTANCE instance_ = nullptr;
    Config* config_ = nullptr;
    HWND owner_ = nullptr;
    Callbacks callbacks_;
    // Before the windows: their script widgets unregister from it when they go.
    WidgetRuntime widgets_;
    // Round 6 phase 6E: the settings windows' widget previews (reads widgets_' storage).
    WidgetPreview previews_{widgets_};
    std::vector<HMONITOR> monitors_;
    std::vector<Instance> instances_;
    std::vector<std::unique_ptr<DockWindow>> graveyard_;  // closed windows, freed by Reap
    std::vector<std::unique_ptr<FolderWindow>> folderGraveyard_;
    std::vector<std::unique_ptr<WidgetWindow>> widgetGraveyard_;
    Renderer folderRenderer_;   // every desktop window's composition device (folder windows, standalone widgets)
    bool folderRendererReady_ = false;
    bool folderRecoveryPosted_ = false;
    ULONG desktopNotify_ = 0;   // the Desktop watch (rules with autoSort)
    std::wstring desktopDir_;
    bool desktopTimer_ = false;
    size_t rulesMoved_ = 0;     // files moved by rules this session (QueryState 207)
    LONG rulesJobs_ = 0;
    bool sortAgain_ = false;
    bool appBarAllowed_ = false;
    bool syncing_ = false;
    bool trayStartPosted_ = false;

    WindowTracker tracker_;
    // The weather services (round 5 phase A), one per place; see SyncWeather.
    std::vector<std::unique_ptr<WeatherService>> weather_;
    std::vector<WeatherConfig> weatherRefresh_;  // widgets' refresh() calls waiting for kWeatherRequest
    bool weatherSyncing_ = false;
    TrayHost tray_;
    NotificationService notifications_;
    CalendarService calendar_;
    bool centreOpen_ = false;
    ForegroundWatch watch_;
    HWND watchFg_ = nullptr;             // the foreground window at the last watch evaluation
    std::vector<DockWindow*> watchers_;  // docks subscribed to watch_ (autoHide "intelligent")
    // Starts watch_ (a running watch only takes the callback): the watchers and the dashboard.
    void StartWatch();
    // A foreground change during dashboard mode: another app's window ends it.
    void CheckDashboardForeground();
    bool dashboard_ = false;
    HWND dashboardPrev_ = nullptr;  // the window that had the foreground when the mode began
    // Some client needs watch_: intelligent docks, the dashboard, "desktop" widgets, the guard.
    bool WatchWanted() const;
    // The full-screen guard (round 9 phase 9A): the monitors with a full-screen app in front now,
    // then every topmost window hidden / shown to match.
    std::vector<HMONITOR> FullscreenMonitors() const;
    void EvaluateFullscreen();
    std::vector<HMONITOR> fullscreenNow_;  // the last evaluation's
    // Monitors whose app bar dock got ABN_FULLSCREENAPP (on), with the window in front then.
    std::vector<std::pair<HMONITOR, HWND>> abnMonitors_;
    int guardForce_ = -1;                  // DockCommand 150: -1 real, 0 none, 1 every monitor
};
