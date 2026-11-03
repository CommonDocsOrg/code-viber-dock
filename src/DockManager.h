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
#include "WindowTracker.h"

// Owns every dock window and the services they share (one each per process): the window tracker,
// the weather service and the foreground watch, all bound to the app's own hidden window
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
        bool Live() const { return window || folder; }
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
    WeatherService& WeatherData() override { return weather_; }
    const WeatherService& WeatherData() const { return weather_; }
    void ConfigureWeather() override;
    bool AnyWeatherItem() const override;
    ForegroundWatch& Watch() override { return watch_; }
    void SetWatching(DockWindow* dock, bool on) override;
    WidgetRuntime& Widgets() override { return widgets_; }
    TrayHost& TrayService() override { return tray_; }
    NotificationService& Notifications() override { return notifications_; }
    const NotificationService& Notifications() const { return notifications_; }
    // The app of notification `id` to the front (its slot on the first dock showing it, else a
    // launch of its AUMID / exe), its notifications marked read. False: unknown id.
    bool ActivateNotification(uint32_t id) override;
    // The centre popup opened / closed (it polls every second while open, and keeps the service
    // running even when nothing else needs it).
    void SetCentreOpen(bool open) override;

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
    // The folder windows' shared device: made with the first folder window, released after the last.
    bool EnsureFolderRenderer();
    void RecoverFolderDevice();
    static constexpr UINT kFolderDeviceLost = WM_APP + 52;  // posted to the owner window
    static constexpr UINT kDesktopChange = WM_APP + 53;     // SHChangeNotify on the Desktop (rules)
    static constexpr UINT kRulesJobDone = WM_APP + 54;      // a rules move finished
    static constexpr UINT kWeatherRequest = WM_APP + 55;    // wParam: WidgetRuntime::WeatherAction
    static constexpr UINT_PTR kDesktopTimer = 43;           // owner timer: coalesces Desktop changes
    // Closes the window now (DestroyWindow) and keeps the object until Reap / the destructor.
    void Close(std::unique_ptr<DockWindow> window);

    HINSTANCE instance_ = nullptr;
    Config* config_ = nullptr;
    HWND owner_ = nullptr;
    Callbacks callbacks_;
    // Before the windows: their script widgets unregister from it when they go.
    WidgetRuntime widgets_;
    std::vector<HMONITOR> monitors_;
    std::vector<Instance> instances_;
    std::vector<std::unique_ptr<DockWindow>> graveyard_;  // closed windows, freed by Reap
    std::vector<std::unique_ptr<FolderWindow>> folderGraveyard_;
    Renderer folderRenderer_;   // every folder window's composition device (on the owner window)
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
    WeatherService weather_;
    TrayHost tray_;
    NotificationService notifications_;
    CalendarService calendar_;
    bool centreOpen_ = false;
    ForegroundWatch watch_;
    std::vector<DockWindow*> watchers_;  // docks subscribed to watch_ (autoHide "intelligent")
};
