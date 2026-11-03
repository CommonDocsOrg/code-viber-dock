#include "DockManager.h"

#include <algorithm>

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
    tracker_.Start(owner_, [this](const WindowTracker::Snapshot& snap) {
        ForEach([&](DockWindow& w) { w.OnSnapshot(snap); });
    });
}

void DockManager::Shutdown() {
    if (!config_) return;
    for (Instance& i : instances_) {
        if (i.window) Close(std::move(i.window));
    }
    instances_.clear();
    watchers_.clear();
    watch_.Stop();
    tracker_.Stop();
    weather_.Stop();  // a fetch still in flight posts to a window that is gone (and frees it)
    graveyard_.clear();
    widgets_.Shutdown();  // every widget went with its window
    config_ = nullptr;
}

bool DockManager::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    if (tracker_.HandleMessage(msg, wp, lp)) return true;
    if (watch_.HandleMessage(msg, wp, lp)) return true;
    if (widgets_.HandleMessage(msg, wp, lp)) return true;  // the widget timers
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
    }
    old.clear();

    HRESULT first = S_OK;
    for (Instance& inst : instances_) {
        if (!inst.monitor) continue;  // parked
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
    syncing_ = false;
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
