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
#include "Config.h"
#include "HttpClient.h"
#include "MediaService.h"
#include "PerfService.h"

class NotificationService;
class CalendarService;
class WeatherService;

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
    static constexpr UINT kStorageMessage = WM_APP + 36;    // posted: storage.onChange deliveries (coalesced)
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
    // `writer` (round 6 phase 6C): the instance that changed the key; the other instances of the
    // same widget id hear storage.onChange({ key }) shortly after (posted), only when the value
    // really changed.
    bool StorageSet(const std::wstring& id, const std::string& key, const std::string& json, std::wstring& error,
                    WidgetInstance* writer = nullptr);
    void StorageRemove(const std::wstring& id, const std::string& key, WidgetInstance* writer = nullptr);
    uint64_t StorageNotices() const { return storageNotices_; }  // storage.onChange deliveries
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
    void Schedule(WidgetInstance* w, int id, uint32_t ms, bool repeat, bool aligned = false, const std::wstring& zone = {});
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
    // `perf` (round 6 phase 6D): one sampler for every widget that uses it, started by the first
    // and stopped with the last; its period is the fastest any live user asked for
    // (perf.setInterval, default 1000 ms). Each snapshot reaches the users as perf.onChange.
    PerfService* UsePerf(WidgetInstance* w);
    // `w`'s own period (0: the default); the service re-times to the fastest of its users.
    void SetPerfInterval(WidgetInstance* w, uint32_t ms);
    PerfService& Perf() { return perf_; }
    const PerfService& Perf() const { return perf_; }
    size_t PerfUsers() const { return perfUsers_.size(); }
    // The metrics for the settings page's "choiceFrom": "perf" (the running service's, else a short
    // synchronous sample, PerfService::ListOnce).
    std::vector<PerfService::Metric> PerfMetricList() const;
    // `notifications` (phase 9): the service is DockManager's; a widget that uses it counts as a
    // consumer (the manager starts the service while there is one: `demand` asks it to re-check).
    void SetNotifications(NotificationService* service, std::function<void()> demand) {
        notifications_ = service;
        notificationsDemand_ = std::move(demand);
    }
    NotificationService* UseNotifications(WidgetInstance* w);
    NotificationService* Notifications() const { return notifications_; }
    size_t NotificationUsers() const { return notificationUsers_.size(); }
    // The notification list changed: the widgets that used the provider hear onChange.
    void NotifyNotifications() { NotifyProvider(notificationUsers_, "notifications"); }
    // `calendar` (phase 13): DockManager's service (it runs while some feed is enabled, whatever
    // the widgets do); its users hear onChange, and onReminder with the event (JSON).
    void SetCalendar(CalendarService* service) { calendar_ = service; }
    CalendarService* UseCalendar(WidgetInstance* w);
    size_t CalendarUsers() const { return calendarUsers_.size(); }
    void NotifyCalendar() { NotifyProvider(calendarUsers_, "calendar"); }
    void NotifyReminder(const std::string& eventJson) { NotifyProvider(calendarUsers_, "calendarReminder", eventJson); }
    // `weather` (phase 14; per place since round 5): DockManager's WeatherService of the widget's
    // place (WidgetInstance::WeatherCfg; `resolve` creates it on first use). The services run for
    // the shown docks' weather items and the widgets using the provider: `request` (posted by the
    // manager, never inside a script call) re-checks that when a user comes / goes or its place
    // changes, and carries weather.refresh() for that place.
    enum class WeatherAction { Sync, Refresh };
    void SetWeather(std::function<WeatherService*(const WeatherConfig&)> resolve,
                    std::function<void(WeatherAction, const WeatherConfig&)> request) {
        weatherResolve_ = std::move(resolve);
        weatherRequest_ = std::move(request);
    }
    WeatherService* UseWeather(WidgetInstance* w);
    size_t WeatherUsers() const { return weatherUsers_.size(); }
    // The places the users want (one per user; the manager makes them unique).
    std::vector<WeatherConfig> WeatherUserConfigs() const;
    void WeatherRequest(WeatherAction a, const WeatherConfig& place) {
        if (weatherRequest_) weatherRequest_(a, place);
    }
    // Widget `w`'s place changed (its settings, or its dock's weather item): its service follows.
    void WeatherPlaceChanged(WidgetInstance* w);
    // The forecast or the status of `place` changed: its widgets hear onChange.
    void NotifyWeather(const WeatherConfig& place);
    uint64_t WeatherNotices() const { return weatherNotices_; }  // onChange deliveries (QueryState 215)

private:
    struct Entry {
        WidgetInstance* w = nullptr;
        int id = 0;
        double due = 0.0;  // NowMs
        uint32_t period = 0;  // 0: one-shot
        bool aligned = false;
        std::wstring zone;  // aligned: to the time of day of this zone ("" local)
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
    // A key of widget `id` changed by `writer`: the other instances of `id` hear it (posted).
    void StorageChanged(WidgetInstance* writer, const std::wstring& id, const std::string& key);
    struct StorageNote {
        WidgetInstance* writer = nullptr;  // compared only (it may be gone by delivery)
        std::wstring id;
        std::string key;
        bool operator==(const StorageNote&) const = default;
    };
    std::vector<StorageNote> storageNotes_;
    bool storagePosted_ = false;
    uint64_t storageNotices_ = 0;
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
    PerfService perf_;
    std::set<WidgetInstance*> audioUsers_, mediaUsers_, notificationUsers_, calendarUsers_, weatherUsers_, perfUsers_;
    std::map<WidgetInstance*, uint32_t> perfIntervals_;  // what each user asked for (perf.setInterval)
    void SyncPerfInterval();
    CalendarService* calendar_ = nullptr;
    std::function<WeatherService*(const WeatherConfig&)> weatherResolve_;
    std::function<void(WeatherAction, const WeatherConfig&)> weatherRequest_;
    uint64_t weatherNotices_ = 0;
    NotificationService* notifications_ = nullptr;
    std::function<void()> notificationsDemand_;
    void NotifyProvider(std::set<WidgetInstance*>& users, const char* channel, const std::string& payload = {});
};

// The `system` namespace of the widget API: local date / time and locale strings computed by the
// host (not by the script engine: QuickJS's Date applies the current DST offset to every date on
// Windows), in the user's locale. Round 5 phase D: every time call takes a Windows time zone key
// name ("Tokyo Standard Time"; "" or an unknown key = the local time), converted with
// SystemTimeToTzSpecificLocalTimeEx (the zone's own daylight-saving rules).
namespace WidgetSystem {
struct LocalTime {
    int year = 0, month = 0, day = 0;  // month 1 - 12
    int hour = 0, minute = 0, second = 0, millisecond = 0;
    int weekday = 0;   // 0 = Sunday
    int yearDay = 0;   // 1 - 366
    bool dst = false;
    int utcOffset = 0;  // minutes east of UTC
    std::wstring timeZone;  // the zone's key name ("" = the local time: no key, or an unknown one)
};
LocalTime Now(const std::wstring& timeZone = {});
// strftime (wcsftime) of the time now in the zone; a bad specifier falls back to "%H:%M" (Clock::Format).
std::wstring Format(const std::wstring& format, const std::wstring& timeZone = {});
std::wstring Locale();  // "en-US"
bool Hour12();          // the user's time format uses a 12-hour clock
int FirstDayOfWeek();   // 0 = Sunday
// Sunday first / January first.
std::vector<std::wstring> DayNames(bool abbreviated);
std::vector<std::wstring> MonthNames(bool abbreviated);
// The user's long / short date and time formats (GetDateFormatEx / GetTimeFormatEx) for now.
std::wstring FormatDate(bool longFormat, const std::wstring& timeZone = {});
std::wstring FormatTime(bool seconds, const std::wstring& timeZone = {});
// The zones of this Windows (Clock::TimeZones: by UTC offset, then name).
struct Zone {
    std::wstring key, name;
    int standardOffset = 0;  // minutes east of UTC without daylight saving
};
std::vector<Zone> TimeZones();
// Forget the zone lookups (the time zone data or the clock changed).
void ForgetZones();
}  // namespace WidgetSystem

// Monotonic milliseconds (QueryPerformanceCounter).
double WidgetNowMs();
// Milliseconds from now to the next multiple of `period` ms of the time of day in `timeZone` ("":
// the local time; +30 ms, so it lands just after the boundary). Every Windows zone is a whole number
// of minutes from UTC, so periods up to a minute fall on the same instants in every zone; longer
// ones (an hour, a day) follow the zone's own hours and midnight.
uint32_t WidgetAlignedDelay(uint32_t period, const std::wstring& timeZone = {});
