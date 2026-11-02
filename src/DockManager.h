#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Config.h"
#include "DockWindow.h"
#include "ForegroundWatch.h"
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
    };

    struct Callbacks {
        DockWindow::MessageHook hook;                // every dock window's message hook (App)
        std::function<void(DockWindow&)> setup;      // a dock window was created (pin requests ...)
        std::function<void()> weatherChanged;        // the weather data / status changed (once per change)
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
    void Reap() { graveyard_.clear(); }
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
    WeatherService& WeatherData() override { return weather_; }
    const WeatherService& WeatherData() const { return weather_; }
    void ConfigureWeather() override;
    bool AnyWeatherItem() const override;
    ForegroundWatch& Watch() override { return watch_; }
    void SetWatching(DockWindow* dock, bool on) override;

private:
    HRESULT CreateWindowFor(Instance& inst);
    // Closes the window now (DestroyWindow) and keeps the object until Reap / the destructor.
    void Close(std::unique_ptr<DockWindow> window);

    HINSTANCE instance_ = nullptr;
    Config* config_ = nullptr;
    HWND owner_ = nullptr;
    Callbacks callbacks_;
    std::vector<HMONITOR> monitors_;
    std::vector<Instance> instances_;
    std::vector<std::unique_ptr<DockWindow>> graveyard_;  // closed windows, freed by Reap
    bool appBarAllowed_ = false;
    bool syncing_ = false;

    WindowTracker tracker_;
    WeatherService weather_;
    ForegroundWatch watch_;
    std::vector<DockWindow*> watchers_;  // docks subscribed to watch_ (autoHide "intelligent")
};
