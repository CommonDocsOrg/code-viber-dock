#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "Config.h"
#include "Ics.h"

// The calendar feeds (round 4 phase 13): ICS files fetched over WinHTTP or read from disk, parsed
// (Ics.h) and expanded into occurrences for the calendar popup, the clock's dot, the `calendar`
// provider of widgets and reminders. Owned by DockManager, bound to the app's hidden window.
//
// Cost: with no enabled feed nothing exists (no timer, no thread). Otherwise:
//  - web feeds (http, https, webcal = https): one short-lived worker thread per fetch (Http::Perform,
//    15 s timeouts, 32 MB cap) that also parses the result; ETag / Last-Modified are sent back as
//    If-None-Match / If-Modified-Since (a 304 keeps the data). One coalesced one-shot timer
//    (kRefreshTimerId) for the earliest due feed: each feed is due refreshMin after its last fetch;
//    after a failure 1, 2, 4 ... minutes (capped at refreshMin), like the weather service. The body
//    is cached in %LocalAppData%\WinDock\calendar-cache\<hash>.ics (+ <hash>.meta: url, ETag,
//    Last-Modified, fetch time) and loaded at start, so the dock shows events before the first
//    fetch and a restart within the interval does not fetch again;
//  - file feeds: read (and parsed) on a worker thread at start and whenever the file changes: one
//    watcher thread blocks in ReadDirectoryChangesW on the files' folders (no polling); changes
//    are debounced by a one-shot timer (kFileTimerId, 300 ms).
// Occurrences are expanded on the UI thread into a window of local days (by default 31 days back
// to 92 ahead of today), extended on demand (EnsureDays: the popup navigating further).
class CalendarService {
public:
    static constexpr UINT kResultMessage = WM_APP + 50;  // lParam: a finished job (owned by the receiver)
    static constexpr UINT kFileMessage = WM_APP + 51;    // wParam: watcher generation, lParam: folder index
    static constexpr UINT_PTR kRefreshTimerId = 40;      // on the owner: the next feed that is due
    static constexpr UINT_PTR kFileTimerId = 41;         // a watched file changed (debounce)
    static constexpr UINT_PTR kReminderTimerId = 42;     // the next reminder that is due (13b)
    static constexpr int64_t kReminderGraceS = 300;      // a reminder still fires up to 5 min after the start
    static constexpr int kDaysBack = 31, kDaysAhead = 92;

    enum class FeedStatus { Off = 0, Loading = 1, Ok = 2, Error = 3 };
    struct Feed {
        uint64_t id = 0;          // jobs find their feed by it (indexes move when feeds change)
        CalendarFeed cfg;
        bool file = false;        // a file feed (else web)
        std::wstring location;    // the http(s) URL, or the file's full path
        std::wstring problem;     // the URL / path cannot be used ("" ok)
        std::wstring cacheKey;    // the cache file stem (web feeds)
        FeedStatus status = FeedStatus::Off;
        std::wstring error;       // the last failure ("" none)
        int64_t lastFetch = 0;    // Unix seconds of the last good fetch / read (0 never)
        int64_t due = 0;          // Unix seconds the next fetch is due (0: not scheduled)
        int failures = 0;
        bool inFlight = false;
        bool fromCache = false;   // the data came from the cache (no fetch yet this session)
        std::wstring etag, lastModified;
        std::shared_ptr<const Ics::Calendar> cal;
        size_t occurrences = 0;   // in the expansion window
        uint64_t fileStamp = 0;   // file feeds: last write time, to skip unchanged notifications
    };
    // One occurrence in the window.
    struct Item {
        Ics::Occurrence occ;
        uint32_t feed = 0;
    };
    // A reminder that fired (13b): for the dock's banner and the widgets' onReminder.
    struct ReminderInfo {
        std::wstring id;        // IdOf(item)
        std::wstring title, location, when;  // when: "10:00 AM" (the start)
        int64_t start = 0;      // UTC seconds
        int64_t day = 0;        // its local day
        uint32_t color = 0;
        std::string json;       // EventJson(item)
    };

    struct Job;  // a worker's job and its result (CalendarService.cpp)

    CalendarService() = default;
    CalendarService(const CalendarService&) = delete;
    CalendarService& operator=(const CalendarService&) = delete;
    ~CalendarService() { Stop(); }

    // Applies config.calendars: starts (some feed enabled), stops, or re-reads changed feeds.
    void Configure(const CalendarsConfig& cfg, HWND owner);
    // Stops everything: timers, the watcher, results in flight (ignored when they arrive).
    void Stop();
    // Every web feed fetches now, every file feed is read again.
    void RefreshNow();
    // The time zone or the clock changed (WM_TIMECHANGE / resume): re-expand.
    void TimeChanged();
    bool HandleMessage(UINT msg, WPARAM wp, LPARAM lp);
    // After the data or a feed's status changed (once per change).
    void SetChanged(std::function<void()> cb) { changed_ = std::move(cb); }

    bool Active() const { return active_; }
    const CalendarsConfig& Settings() const { return cfg_; }
    const std::vector<Feed>& Feeds() const { return feeds_; }
    size_t EnabledFeeds() const;
    // Occurrences in the window, sorted by start (all-day first).
    const std::vector<Item>& Items() const { return items_; }
    const Ics::Event& EventOf(const Item& item) const { return feeds_[item.feed].cal->events[item.occ.event]; }
    uint32_t ColorOf(const Item& item) const { return feeds_[item.feed].cfg.color; }
    // The occurrences touching local day `day` (indexes into Items()): all-day first, then by start.
    std::vector<size_t> On(int64_t day) const;
    size_t CountOn(int64_t day) const;
    // The window covers [from, to] (local days): expands further if it does not.
    void EnsureDays(int64_t from, int64_t to);
    int64_t WindowFrom() const { return winFrom_; }
    int64_t WindowTo() const { return winTo_; }
    // Today (local) and now.
    static int64_t Today();
    static int64_t Now();
    // Wall-clock seconds of a UTC instant in the local zone.
    int64_t LocalWall(int64_t utc) const { return local_ ? local_->ToWall(utc) : utc; }
    int64_t LocalToUtc(int64_t wall) const { return local_ ? local_->ToUtc(wall) : wall; }
    // "10:15 AM" (locale short time) of a UTC instant; "All day" style texts for the popup.
    std::wstring TimeText(int64_t utc) const;
    // "9:00 – 10:00 AM", "All day", "Until 11:00" (for day `day`).
    std::wstring RangeText(const Item& item, int64_t day) const;
    // A feed's status line for Settings ("Updated 10:15, 42 events", "Error: HTTP 404; retry in 2 min").
    std::wstring StatusText(size_t feed) const;

    // --- the widgets' `calendar` provider (13b) ---
    // A stable id of an occurrence: the feed's source, the event's UID and its original start.
    std::wstring IdOf(const Item& item) const;
    // { id, feed, color, title, location, description, start, end (ms since 1970), allDay, url,
    // recurring, startDay / endDay ("YYYY-MM-DD", local; endDay the last day), startTime / endTime
    // (locale short time, "" all-day), reminder (the event's VALARM minutes, -1 none) }.
    std::string EventJson(const Item& item) const;
    // [{ name, color, enabled, file, status ("off" | "loading" | "ok" | "error"), error,
    // lastFetch (ms, 0 never), events }].
    std::string FeedsJson() const;

    // --- reminders (13b): one timer for the earliest due one ---
    // The reminder minutes of a feed (its reminderMin, else calendars.defaultReminderMin; 0 off).
    int ReminderMinutes(const Feed& f) const;
    void SetReminderCallback(std::function<void(const ReminderInfo&)> cb) { reminder_ = std::move(cb); }
    size_t PendingReminders() const;      // not fired yet (timed events still ahead) + snoozed
    int64_t NextReminderIn() const;       // seconds until the reminder timer fires (-1 none)
    uint64_t RemindersFired() const { return remindersFired_; }
    // Automation: the earliest pending reminder (else the earliest snoozed one) fires now.
    bool FireNextReminder();
    // The reminder `id` fires again in `seconds`.
    void Snooze(const std::wstring& id, int seconds);

    // --- automation ---
    int State() const;                 // 0 off, 1 running, 2 a feed has an error
    int64_t LastFetchAge() const;      // seconds since the newest good fetch / read (-1 none)
    uint64_t Fetches() const { return fetches_; }          // web responses received
    uint64_t NotModified() const { return notModified_; }  // ... of which 304
    uint64_t FileReads() const { return fileReads_; }      // file feeds read
    int PendingTimers() const { return (refreshArmed_ ? 1 : 0) + (fileTimerArmed_ ? 1 : 0) + (reminderArmed_ ? 1 : 0); }
    int64_t NextDueIn() const;         // seconds until the refresh timer fires (-1 none)
    bool Watching() const { return watchThread_ != nullptr; }
    // The web feed's URL for a config URL (webcal -> https) or the file path (file:/// too). False
    // with `problem` when it is neither.
    static bool Classify(const std::wstring& url, bool& file, std::wstring& location, std::wstring& problem);
    static std::wstring CacheDirectory();

private:
    void StartJob(size_t feed, int kind);
    void OnResult(Job* job);
    void Arm();
    void OnRefreshTimer();
    void Rebuild();
    void StartWatcher();
    void StopWatcher();
    void OnFileChanged(WPARAM generation, LPARAM folder);
    void OnFileTimer();
    void ArmReminders();
    void OnReminderTimer();
    void Fire(const Item& item, const std::wstring& id);
    void Notify() {
        if (changed_) changed_();
    }

    HWND owner_ = nullptr;
    CalendarsConfig cfg_;
    bool active_ = false;
    std::vector<Feed> feeds_;
    uint64_t nextFeedId_ = 1;
    uint64_t generation_ = 1;  // bumped by Stop: results in flight are dropped
    int64_t reqFrom_ = 0, reqTo_ = -1;  // days EnsureDays asked for (kept across rebuilds)
    std::vector<Item> items_;
    std::map<int64_t, std::vector<uint32_t>> byDay_;  // local day -> indexes into items_
    int64_t winFrom_ = 0, winTo_ = -1;  // local days of the expansion, inclusive
    std::shared_ptr<const Ics::Zone> local_;
    bool refreshArmed_ = false;
    int64_t armedDue_ = 0;
    bool fileTimerArmed_ = false;
    std::vector<std::wstring> watchedFolders_;  // lower-case, no trailing backslash
    std::vector<bool> folderDirty_;
    HANDLE watchThread_ = nullptr;
    HANDLE watchStop_ = nullptr;
    LONG watchGeneration_ = 0;
    uint64_t fetches_ = 0, notModified_ = 0, fileReads_ = 0;
    std::function<void()> changed_;
    // Reminders (session only): ids that fired, snoozed ids -> when they fire again.
    std::set<std::wstring> fired_;
    std::map<std::wstring, int64_t> snoozed_;
    bool reminderArmed_ = false;
    int64_t reminderDue_ = 0;
    uint64_t remindersFired_ = 0;
    std::function<void(const ReminderInfo&)> reminder_;
};
