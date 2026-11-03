#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "AudioService.h"
#include "HttpClient.h"
#include "MediaService.h"

class WidgetInstance;

// The process-wide part of the widget runtime (owned by DockManager, bound to the app's hidden
// window like the other shared services): one timer scheduler for every widget's setTimeout /
// setInterval / tick (a single one-shot SetTimer for the earliest deadline; none while no widget
// waits for anything), the list of live widget instances (reload all, automation, the Console),
// the widgets' storage files (written debounced), their http requests (one short-lived worker
// thread per request, the result posted back), and the folder watcher of `watch` widgets (one
// thread blocked in ReadDirectoryChangesW while any watched widget exists; no polling). Every
// script runs on the UI thread; each widget instance has its own script runtime
// (WidgetInstance.h).
class WidgetRuntime {
public:
    static constexpr UINT_PTR kTimerId = 30;        // on the owner window: the next widget timer
    static constexpr UINT_PTR kSaveTimerId = 31;    // storage files, debounced (one-shot)
    static constexpr UINT_PTR kReloadTimerId = 32;  // watched folders changed: reload (debounced)
    static constexpr UINT kHttpMessage = WM_APP + 30;   // lParam: a finished request (HttpJob*)
    static constexpr UINT kWatchMessage = WM_APP + 31;  // wParam: folder index, lParam: generation
    static constexpr UINT kWatchSyncMessage = WM_APP + 32;  // posted: re-read the watched folders
    static constexpr DWORD kSaveDelayMs = 1000;
    static constexpr DWORD kReloadDelayMs = 250;
    static constexpr size_t kMaxStorageBytes = 1u << 20;  // per widget id
    static constexpr int kMaxRequests = 4;                // http requests in flight per widget

    WidgetRuntime() = default;
    WidgetRuntime(const WidgetRuntime&) = delete;
    WidgetRuntime& operator=(const WidgetRuntime&) = delete;
    ~WidgetRuntime() { Shutdown(); }

    void Initialize(HWND owner) { owner_ = owner; }
    // Writes pending storage, stops the watcher thread and the timers.
    void Shutdown();
    // The owner window's messages (timers, http results, watcher events). True if handled.
    bool HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

    // --- storage (%LocalAppData%\WinDock\widget-data\<id>.json, one object per widget id) ---
    bool StorageGet(const std::wstring& id, const std::string& key, std::string& json);
    bool StorageSet(const std::wstring& id, const std::string& key, const std::string& json, std::wstring& error);
    void StorageRemove(const std::wstring& id, const std::string& key);
    std::vector<std::string> StorageKeys(const std::wstring& id);
    static std::wstring StorageDirectory();
    // Writes every changed storage file now.
    void FlushStorage();

    // --- http ---
    // Runs `request` on a worker thread; the result comes back as WidgetInstance::OnHttpResult(id)
    // if `w` (with the same generation) still exists. False if the thread could not start.
    bool StartHttp(WidgetInstance* w, uint64_t generation, int id, Http::Request request);

    // --- watched folders ---
    // The set of watched widget folders may have changed (instances came / went / reloaded):
    // re-read it soon (posted, coalesced).
    void SyncWatch();
    size_t WatchedFolders() const { return watched_.size(); }

    // --- the Console ---
    // `log`: a widget wrote to its log; `changed`: instances came, went or changed state. Null to stop.
    void SetListeners(std::function<void(WidgetInstance*)> log, std::function<void()> changed) {
        logListener_ = std::move(log);
        changedListener_ = std::move(changed);
    }
    void NotifyLog(WidgetInstance* w) {
        if (logListener_) logListener_(w);
    }
    void NotifyChanged() {
        if (changedListener_) changedListener_();
    }

    // A timer of widget `w`: due in `ms` (repeat: every `ms`; aligned: at the next multiple of
    // `ms` of the local time of day, then every multiple). Replaces a timer with the same id.
    void Schedule(WidgetInstance* w, int id, uint32_t ms, bool repeat, bool aligned = false);
    void Cancel(WidgetInstance* w, int id);
    void CancelAll(WidgetInstance* w);
    size_t PendingTimers(const WidgetInstance* w) const;

    void Add(WidgetInstance* w);
    void Remove(WidgetInstance* w);
    const std::vector<WidgetInstance*>& Instances() const { return instances_; }
    bool Alive(const WidgetInstance* w) const;
    // Every widget instance reloads its package and script.
    void ReloadAll();
    // How often the scheduler's timer fired (automation: idle widgets never wake the process).
    uint64_t Wakeups() const { return wakeups_; }

    // --- providers (audio, media): started for the first widget that uses one, stopped when the
    // last such widget goes or reloads; their changes reach every widget that used them
    // (WidgetInstance::ProviderChanged). Null if the provider cannot start.
    AudioService* UseAudio(WidgetInstance* w);
    MediaService* UseMedia(WidgetInstance* w);
    // `w` no longer uses the providers (it reloads, failed or goes).
    void ReleaseProviders(WidgetInstance* w);
    const AudioService& Audio() const { return audio_; }
    const MediaService& Media() const { return media_; }

private:
    struct Entry {
        WidgetInstance* w = nullptr;
        int id = 0;
        double due = 0.0;  // NowMs
        uint32_t period = 0;  // 0: one-shot
        bool aligned = false;
    };
    void Arm();
    void OnTimer();

    struct Store {
        bool loaded = false;
        bool dirty = false;
        std::map<std::string, std::string> values;  // key -> JSON text
        size_t bytes = 0;
    };
    Store& StoreFor(const std::wstring& id);
    void ScheduleSave();
    void OnHttpResult(LPARAM lp);
    void DoSyncWatch();
    void StopWatcher();
    void OnWatchEvent(WPARAM index, LPARAM generation);

    HWND owner_ = nullptr;
    std::vector<Entry> entries_;
    bool armed_ = false;
    double armedDue_ = 0.0;
    bool firing_ = false;
    uint64_t wakeups_ = 0;
    std::vector<WidgetInstance*> instances_;
    std::map<std::wstring, Store> stores_;
    bool savePending_ = false;
    // The watcher: the folders it watches (index = wParam of kWatchMessage) and their widget ids.
    struct Watched {
        std::wstring id;
        std::wstring folder;
        bool operator==(const Watched&) const = default;
    };
    std::vector<Watched> watched_;
    HANDLE watchThread_ = nullptr;
    HANDLE watchStop_ = nullptr;  // manual-reset event: the thread ends
    LONG watchGeneration_ = 0;
    bool watchSyncPosted_ = false;
    std::set<std::wstring> reloadIds_;  // changed folders waiting for kReloadTimerId
    bool reloadPending_ = false;
    std::function<void(WidgetInstance*)> logListener_;
    std::function<void()> changedListener_;
    AudioService audio_;
    MediaService media_;
    std::set<WidgetInstance*> audioUsers_, mediaUsers_;
    void NotifyProvider(std::set<WidgetInstance*>& users, const char* channel);
};

// The `system` namespace of the widget API: local date / time and locale strings computed by the
// host (not by the script engine: QuickJS's Date applies the current DST offset to every date on
// Windows), in the user's locale.
namespace WidgetSystem {
struct LocalTime {
    int year = 0, month = 0, day = 0;  // month 1 - 12
    int hour = 0, minute = 0, second = 0, millisecond = 0;
    int weekday = 0;   // 0 = Sunday
    int yearDay = 0;   // 1 - 366
    bool dst = false;
    int utcOffset = 0;  // minutes east of UTC
};
LocalTime Now();
// strftime (wcsftime) of the local time now; a bad specifier falls back to "%H:%M" (Clock::Format).
std::wstring Format(const std::wstring& format);
std::wstring Locale();  // "en-US"
bool Hour12();          // the user's time format uses a 12-hour clock
int FirstDayOfWeek();   // 0 = Sunday
// Sunday first / January first.
std::vector<std::wstring> DayNames(bool abbreviated);
std::vector<std::wstring> MonthNames(bool abbreviated);
// The user's long / short date and time formats (GetDateFormatEx / GetTimeFormatEx) for now.
std::wstring FormatDate(bool longFormat);
std::wstring FormatTime(bool seconds);
}  // namespace WidgetSystem

// Monotonic milliseconds (QueryPerformanceCounter).
double WidgetNowMs();
// Milliseconds from now to the next multiple of `period` ms of the local time of day (+30 ms, so
// it lands just after the boundary).
uint32_t WidgetAlignedDelay(uint32_t period);
