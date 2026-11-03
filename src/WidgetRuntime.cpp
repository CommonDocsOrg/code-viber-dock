#include "WidgetRuntime.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <iterator>
#include <new>

#include "Clock.h"
#include "Config.h"
#include "JsonUtil.h"
#include "WidgetInstance.h"

namespace WidgetSystem {

LocalTime Now() {
    LocalTime t;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    t.year = st.wYear;
    t.month = st.wMonth;
    t.day = st.wDay;
    t.hour = st.wHour;
    t.minute = st.wMinute;
    t.second = st.wSecond;
    t.millisecond = st.wMilliseconds;
    t.weekday = st.wDayOfWeek;
    static const int kCum[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    const bool leap = (t.year % 4 == 0 && t.year % 100 != 0) || t.year % 400 == 0;
    t.yearDay = kCum[std::clamp(t.month, 1, 12) - 1] + t.day + (leap && t.month > 2 ? 1 : 0);
    DYNAMIC_TIME_ZONE_INFORMATION tz{};
    const DWORD mode = GetDynamicTimeZoneInformation(&tz);
    t.dst = mode == TIME_ZONE_ID_DAYLIGHT;
    t.utcOffset = -static_cast<int>(tz.Bias + (t.dst ? tz.DaylightBias : tz.StandardBias));
    return t;
}

std::wstring Format(const std::wstring& format) {
    __time64_t now = _time64(nullptr);
    tm local{};
    if (_localtime64_s(&local, &now) != 0) return {};
    return Clock::Format(format, local);
}

std::wstring Locale() {
    wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
    return GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) ? name : L"en-US";
}

bool Hour12() {
    DWORD v = 0;
    if (!GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_ITIME | LOCALE_RETURN_NUMBER, reinterpret_cast<LPWSTR>(&v),
                         sizeof(v) / sizeof(wchar_t))) {
        return false;
    }
    return v == 0;  // 0: AM / PM 12-hour format
}

int FirstDayOfWeek() {
    DWORD v = 0;  // 0 = Monday ... 6 = Sunday
    if (!GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK | LOCALE_RETURN_NUMBER,
                         reinterpret_cast<LPWSTR>(&v), sizeof(v) / sizeof(wchar_t))) {
        return 0;
    }
    return static_cast<int>((v + 1) % 7);
}

namespace {
std::wstring LocaleString(LCTYPE type) {
    wchar_t buf[128]{};
    return GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, type, buf, static_cast<int>(std::size(buf))) ? buf : L"";
}
}  // namespace

std::vector<std::wstring> DayNames(bool abbreviated) {
    // LOCALE_SDAYNAME1 is Monday.
    const LCTYPE full[] = {LOCALE_SDAYNAME7, LOCALE_SDAYNAME1, LOCALE_SDAYNAME2, LOCALE_SDAYNAME3,
                           LOCALE_SDAYNAME4, LOCALE_SDAYNAME5, LOCALE_SDAYNAME6};
    const LCTYPE abbr[] = {LOCALE_SABBREVDAYNAME7, LOCALE_SABBREVDAYNAME1, LOCALE_SABBREVDAYNAME2,
                           LOCALE_SABBREVDAYNAME3, LOCALE_SABBREVDAYNAME4, LOCALE_SABBREVDAYNAME5,
                           LOCALE_SABBREVDAYNAME6};
    std::vector<std::wstring> out;
    for (int i = 0; i < 7; ++i) out.push_back(LocaleString(abbreviated ? abbr[i] : full[i]));
    return out;
}

std::vector<std::wstring> MonthNames(bool abbreviated) {
    std::vector<std::wstring> out;
    for (int i = 0; i < 12; ++i) {
        out.push_back(LocaleString(static_cast<LCTYPE>((abbreviated ? LOCALE_SABBREVMONTHNAME1 : LOCALE_SMONTHNAME1) + i)));
    }
    return out;
}

std::wstring FormatDate(bool longFormat) {
    wchar_t buf[128]{};
    return GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, longFormat ? DATE_LONGDATE : DATE_SHORTDATE, nullptr, nullptr, buf,
                           static_cast<int>(std::size(buf)), nullptr)
               ? buf
               : L"";
}

std::wstring FormatTime(bool seconds) {
    wchar_t buf[64]{};
    return GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, seconds ? 0 : TIME_NOSECONDS, nullptr, nullptr, buf,
                           static_cast<int>(std::size(buf)))
               ? buf
               : L"";
}

}  // namespace WidgetSystem

double WidgetNowMs() {
    static const double freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / freq;
}

uint32_t WidgetAlignedDelay(uint32_t period) {
    if (period == 0) return 0;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    const uint64_t tod = ((static_cast<uint64_t>(st.wHour) * 60u + st.wMinute) * 60u + st.wSecond) * 1000u + st.wMilliseconds;
    const uint64_t p = std::min<uint64_t>(period, 86400000u);
    const uint64_t delay = p - tod % p;
    // Just after the boundary, never before it, also when the timer fires a little early (OnTimer).
    return static_cast<uint32_t>(delay + 30u);
}

void WidgetRuntime::Shutdown() {
    audioUsers_.clear();
    mediaUsers_.clear();
    notificationUsers_.clear();
    calendarUsers_.clear();
    notificationsDemand_ = nullptr;
    weatherRequest_ = nullptr;
    audio_.Stop();
    media_.Stop();
    FlushStorage();
    StopWatcher();
    watched_.clear();
    if (owner_) {
        if (armed_) KillTimer(owner_, kTimerId);
        if (savePending_) KillTimer(owner_, kSaveTimerId);
        if (reloadPending_) KillTimer(owner_, kReloadTimerId);
    }
    armed_ = savePending_ = reloadPending_ = false;
    entries_.clear();
    reloadIds_.clear();
    owner_ = nullptr;
}

bool WidgetRuntime::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TIMER:
            if (wp == kTimerId) {
                KillTimer(owner_, kTimerId);  // one-shot; re-armed for the next deadline
                armed_ = false;
                ++wakeups_;
                OnTimer();
                return true;
            }
            if (wp == kSaveTimerId) {
                KillTimer(owner_, kSaveTimerId);
                savePending_ = false;
                FlushStorage();
                return true;
            }
            if (wp == kReloadTimerId) {
                KillTimer(owner_, kReloadTimerId);
                reloadPending_ = false;
                const std::set<std::wstring> ids = std::move(reloadIds_);
                reloadIds_.clear();
                const std::vector<WidgetInstance*> all = instances_;
                for (WidgetInstance* w : all) {
                    if (Alive(w) && ids.count(w->Id())) {
                        w->Note(L"files changed: reloading");
                        w->Reload();
                    }
                }
                return true;
            }
            return false;
        case kHttpMessage:
            OnHttpResult(lp);
            return true;
        case kWatchMessage:
            OnWatchEvent(wp, lp);
            return true;
        case kWatchSyncMessage:
            watchSyncPosted_ = false;
            DoSyncWatch();
            return true;
        case AudioService::kMessage:
            return audio_.HandleMessage(msg, wp, lp);
        case MediaService::kMessage:
            return media_.HandleMessage(msg, wp, lp);
        default:
            return false;
    }
}

// --- storage -----------------------------------------------------------------------------------

std::wstring WidgetRuntime::StorageDirectory() { return Config::Directory() + L"\\widget-data"; }

WidgetRuntime::Store& WidgetRuntime::StoreFor(const std::wstring& id) {
    Store& s = stores_[id];
    if (s.loaded) return s;
    s.loaded = true;
    JsonUtil::json::JsonObject o{nullptr};
    if (JsonUtil::ReadObjectFile(StorageDirectory() + L"\\" + id + L".json", o) && o) {
        try {
            for (const auto& kv : o) {
                std::string key = JsonUtil::WideToUtf8(std::wstring(kv.Key()));
                std::string value = JsonUtil::WideToUtf8(std::wstring(kv.Value().Stringify()));
                s.bytes += key.size() + value.size();
                s.values[std::move(key)] = std::move(value);
            }
        } catch (...) {
        }
    }
    return s;
}

bool WidgetRuntime::StorageGet(const std::wstring& id, const std::string& key, std::string& json) {
    Store& s = StoreFor(id);
    auto it = s.values.find(key);
    if (it == s.values.end()) return false;
    json = it->second;
    return true;
}

bool WidgetRuntime::StorageSet(const std::wstring& id, const std::string& key, const std::string& json,
                               std::wstring& error) {
    Store& s = StoreFor(id);
    auto it = s.values.find(key);
    const size_t old = it == s.values.end() ? 0 : key.size() + it->second.size();
    if (s.bytes - old + key.size() + json.size() > kMaxStorageBytes) {
        error = L"storage is full (1 MB per widget)";
        return false;
    }
    if (it != s.values.end() && it->second == json) return true;
    s.bytes = s.bytes - old + key.size() + json.size();
    s.values[key] = json;
    s.dirty = true;
    ScheduleSave();
    return true;
}

void WidgetRuntime::StorageRemove(const std::wstring& id, const std::string& key) {
    Store& s = StoreFor(id);
    auto it = s.values.find(key);
    if (it == s.values.end()) return;
    s.bytes -= key.size() + it->second.size();
    s.values.erase(it);
    s.dirty = true;
    ScheduleSave();
}

std::vector<std::string> WidgetRuntime::StorageKeys(const std::wstring& id) {
    std::vector<std::string> keys;
    for (const auto& kv : StoreFor(id).values) keys.push_back(kv.first);
    return keys;
}

void WidgetRuntime::ScheduleSave() {
    if (savePending_ || !owner_) return;
    savePending_ = SetTimer(owner_, kSaveTimerId, kSaveDelayMs, nullptr) != 0;
    if (!savePending_) FlushStorage();
}

void WidgetRuntime::FlushStorage() {
    bool dirAttempted = false;
    for (auto& [id, s] : stores_) {
        if (!s.dirty) continue;
        if (!dirAttempted) {
            CreateDirectoryW(Config::Directory().c_str(), nullptr);
            CreateDirectoryW(StorageDirectory().c_str(), nullptr);
            dirAttempted = true;
        }
        std::string out = "{";
        bool first = true;
        for (const auto& [key, value] : s.values) {
            out += first ? "\n  " : ",\n  ";
            first = false;
            out += JsonUtil::WideToUtf8(JsonUtil::Quote(JsonUtil::Utf8ToWide(key))) + ": " + value;
        }
        out += first ? "}\n" : "\n}\n";
        const std::wstring path = StorageDirectory() + L"\\" + id + L".json";
        if (s.values.empty()) {
            DeleteFileW(path.c_str());
            s.dirty = false;
        } else if (JsonUtil::WriteFileAtomic(path, out)) {
            s.dirty = false;
        }
    }
}

// --- http ----------------------------------------------------------------------------------------

namespace {

struct HttpJob {
    HWND owner = nullptr;
    WidgetInstance* widget = nullptr;
    uint64_t generation = 0;
    int id = 0;
    Http::Request request;
    Http::Result result;
};

DWORD WINAPI HttpThread(LPVOID param) {
    auto* job = static_cast<HttpJob*>(param);
    try {
        Http::Perform(job->request, job->result);
    } catch (...) {
        job->result.body.clear();
        job->result.error = ERROR_OUTOFMEMORY;
    }
    // The owner window frees it; a window that is gone cannot.
    if (!PostMessageW(job->owner, WidgetRuntime::kHttpMessage, 0, reinterpret_cast<LPARAM>(job))) delete job;
    return 0;
}

}  // namespace

bool WidgetRuntime::StartHttp(WidgetInstance* w, uint64_t generation, int id, Http::Request request) {
    if (!owner_) return false;
    auto* job = new (std::nothrow) HttpJob;
    if (!job) return false;
    job->owner = owner_;
    job->widget = w;
    job->generation = generation;
    job->id = id;
    job->request = std::move(request);
    HANDLE thread = CreateThread(nullptr, 64 * 1024, &HttpThread, job, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!thread) {
        delete job;
        return false;
    }
    CloseHandle(thread);  // it ends by itself after posting
    return true;
}

void WidgetRuntime::OnHttpResult(LPARAM lp) {
    std::unique_ptr<HttpJob> job(reinterpret_cast<HttpJob*>(lp));
    if (!job || !Alive(job->widget)) return;
    job->widget->OnHttpResult(job->generation, job->id, job->result);
}

// --- watched folders -----------------------------------------------------------------------------

namespace {

struct WatchThreadArgs {
    HWND owner = nullptr;
    HANDLE stop = nullptr;
    LONG generation = 0;
    std::vector<std::wstring> folders;
};

// Blocks in ReadDirectoryChangesW on every watched folder (overlapped, one event each) and posts
// kWatchMessage(folder index) for each batch of changes. Ends when `stop` is set.
DWORD WINAPI WatchThread(LPVOID param) {
    std::unique_ptr<WatchThreadArgs> args(static_cast<WatchThreadArgs*>(param));
    struct Dir {
        HANDLE handle = INVALID_HANDLE_VALUE;
        OVERLAPPED ov{};
        alignas(DWORD) BYTE buffer[4096];
    };
    const size_t n = std::min<size_t>(args->folders.size(), MAXIMUM_WAIT_OBJECTS - 1);
    std::vector<std::unique_ptr<Dir>> dirs;
    std::vector<HANDLE> waits{args->stop};
    std::vector<size_t> index;  // waits[k + 1] -> folder index
    constexpr DWORD kFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                              FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE;
    for (size_t i = 0; i < n; ++i) {
        auto d = std::make_unique<Dir>();
        d->handle = CreateFileW(args->folders[i].c_str(), FILE_LIST_DIRECTORY,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (d->handle == INVALID_HANDLE_VALUE) continue;
        d->ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!d->ov.hEvent || !ReadDirectoryChangesW(d->handle, d->buffer, sizeof(d->buffer), TRUE, kFilter, nullptr,
                                                    &d->ov, nullptr)) {
            if (d->ov.hEvent) CloseHandle(d->ov.hEvent);
            CloseHandle(d->handle);
            continue;
        }
        waits.push_back(d->ov.hEvent);
        index.push_back(i);
        dirs.push_back(std::move(d));
    }
    for (;;) {
        const DWORD r = WaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(), FALSE, INFINITE);
        if (r == WAIT_OBJECT_0 || r == WAIT_FAILED) break;
        const size_t k = r - WAIT_OBJECT_0 - 1;
        if (k >= dirs.size()) break;
        Dir& d = *dirs[k];
        DWORD bytes = 0;
        GetOverlappedResult(d.handle, &d.ov, &bytes, FALSE);
        ResetEvent(d.ov.hEvent);
        PostMessageW(args->owner, WidgetRuntime::kWatchMessage, index[k], args->generation);
        if (!ReadDirectoryChangesW(d.handle, d.buffer, sizeof(d.buffer), TRUE, kFilter, nullptr, &d.ov, nullptr)) {
            break;  // the folder went away: stop watching (a reload re-syncs)
        }
    }
    for (auto& d : dirs) {
        CancelIoEx(d->handle, &d->ov);
        DWORD bytes = 0;
        GetOverlappedResult(d->handle, &d->ov, &bytes, TRUE);
        CloseHandle(d->ov.hEvent);
        CloseHandle(d->handle);
    }
    return 0;
}

}  // namespace

void WidgetRuntime::SyncWatch() {
    if (watchSyncPosted_ || !owner_) return;
    watchSyncPosted_ = PostMessageW(owner_, kWatchSyncMessage, 0, 0) != FALSE;
}

void WidgetRuntime::DoSyncWatch() {
    std::vector<Watched> want;
    for (WidgetInstance* w : instances_) {
        Watched x;
        if (!w->WatchFolder(x.id, x.folder)) continue;
        if (std::find(want.begin(), want.end(), x) == want.end()) want.push_back(std::move(x));
    }
    if (want == watched_ && (watchThread_ != nullptr) == !want.empty()) return;
    StopWatcher();
    watched_ = std::move(want);
    if (watched_.empty()) return;
    auto* args = new (std::nothrow) WatchThreadArgs;
    if (!args) return;
    watchStop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    args->owner = owner_;
    args->stop = watchStop_;
    args->generation = ++watchGeneration_;
    for (const Watched& x : watched_) args->folders.push_back(x.folder);
    watchThread_ = watchStop_ ? CreateThread(nullptr, 64 * 1024, &WatchThread, args, STACK_SIZE_PARAM_IS_A_RESERVATION,
                                             nullptr)
                              : nullptr;
    if (!watchThread_) {
        delete args;
        if (watchStop_) CloseHandle(watchStop_);
        watchStop_ = nullptr;
    }
}

void WidgetRuntime::StopWatcher() {
    ++watchGeneration_;  // events already posted by the old thread are ignored
    if (watchThread_) {
        SetEvent(watchStop_);
        WaitForSingleObject(watchThread_, 2000);
        CloseHandle(watchThread_);
        watchThread_ = nullptr;
    }
    if (watchStop_) CloseHandle(watchStop_);
    watchStop_ = nullptr;
}

void WidgetRuntime::OnWatchEvent(WPARAM index, LPARAM generation) {
    if (static_cast<LONG>(generation) != watchGeneration_ || index >= watched_.size()) return;
    reloadIds_.insert(watched_[index].id);
    // Editors write a file in several steps: wait for them to settle.
    if (reloadPending_ || !owner_) return;
    reloadPending_ = SetTimer(owner_, kReloadTimerId, kReloadDelayMs, nullptr) != 0;
}

void WidgetRuntime::Schedule(WidgetInstance* w, int id, uint32_t ms, bool repeat, bool aligned) {
    Cancel(w, id);
    Entry e;
    e.w = w;
    e.id = id;
    e.period = repeat ? std::max<uint32_t>(ms, 1u) : 0u;
    e.aligned = aligned && repeat;
    e.due = WidgetNowMs() + static_cast<double>(e.aligned ? WidgetAlignedDelay(e.period) : ms);
    entries_.push_back(e);
    Arm();
}

void WidgetRuntime::Cancel(WidgetInstance* w, int id) {
    std::erase_if(entries_, [&](const Entry& e) { return e.w == w && e.id == id; });
    Arm();
}

void WidgetRuntime::CancelAll(WidgetInstance* w) {
    std::erase_if(entries_, [&](const Entry& e) { return e.w == w; });
    Arm();
}

size_t WidgetRuntime::PendingTimers(const WidgetInstance* w) const {
    return static_cast<size_t>(std::count_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.w == w; }));
}

void WidgetRuntime::Add(WidgetInstance* w) {
    instances_.push_back(w);
    NotifyChanged();
}

void WidgetRuntime::Remove(WidgetInstance* w) {
    ReleaseProviders(w);
    std::erase(instances_, w);
    CancelAll(w);
    SyncWatch();
    NotifyChanged();
}

AudioService* WidgetRuntime::UseAudio(WidgetInstance* w) {
    if (!audio_.Active()) {
        if (!owner_ || !audio_.Start(owner_)) return nullptr;
        audio_.SetChanged([this] { NotifyProvider(audioUsers_, "audio"); });
    }
    audioUsers_.insert(w);
    return &audio_;
}

MediaService* WidgetRuntime::UseMedia(WidgetInstance* w) {
    if (!media_.Active()) {
        if (!owner_ || !media_.Start(owner_)) return nullptr;
        media_.SetChanged([this] { NotifyProvider(mediaUsers_, "media"); });
    }
    mediaUsers_.insert(w);
    return &media_;
}

NotificationService* WidgetRuntime::UseNotifications(WidgetInstance* w) {
    if (!notifications_) return nullptr;
    if (notificationUsers_.insert(w).second && notificationUsers_.size() == 1 && notificationsDemand_) {
        notificationsDemand_();  // the first user: the service starts
    }
    return notifications_;
}

CalendarService* WidgetRuntime::UseCalendar(WidgetInstance* w) {
    if (!calendar_) return nullptr;
    calendarUsers_.insert(w);
    return calendar_;
}

WeatherService* WidgetRuntime::UseWeather(WidgetInstance* w) {
    if (!weather_) return nullptr;
    if (weatherUsers_.insert(w).second && weatherUsers_.size() == 1) WeatherRequest(WeatherAction::Sync);
    return weather_;
}

void WidgetRuntime::ReleaseProviders(WidgetInstance* w) {
    calendarUsers_.erase(w);
    if (weatherUsers_.erase(w) && weatherUsers_.empty()) WeatherRequest(WeatherAction::Sync);
    if (audioUsers_.erase(w) && audioUsers_.empty()) audio_.Stop();
    if (mediaUsers_.erase(w) && mediaUsers_.empty()) media_.Stop();
    if (notificationUsers_.erase(w) && notificationUsers_.empty() && notificationsDemand_) notificationsDemand_();
}

void WidgetRuntime::NotifyProvider(std::set<WidgetInstance*>& users, const char* channel, const std::string& payload) {
    const std::vector<WidgetInstance*> all(users.begin(), users.end());  // a callback may reload a widget
    for (WidgetInstance* w : all) {
        if (users.count(w) && Alive(w)) w->ProviderChanged(channel, payload);
    }
}

bool WidgetRuntime::Alive(const WidgetInstance* w) const {
    return std::find(instances_.begin(), instances_.end(), w) != instances_.end();
}

void WidgetRuntime::ReloadAll() {
    const std::vector<WidgetInstance*> all = instances_;  // a reload never adds or removes instances
    for (WidgetInstance* w : all) {
        if (Alive(w)) w->Reload();
    }
}

void WidgetRuntime::Arm() {
    if (firing_ || !owner_) return;  // OnTimer arms once at its end
    if (entries_.empty()) {
        if (armed_) KillTimer(owner_, kTimerId);
        armed_ = false;
        return;
    }
    double due = entries_.front().due;
    for (const Entry& e : entries_) due = std::min(due, e.due);
    if (armed_ && std::fabs(due - armedDue_) < 1.0) return;
    const double delay = std::max(0.0, due - WidgetNowMs());
    SetTimer(owner_, kTimerId, static_cast<UINT>(std::clamp(delay, static_cast<double>(USER_TIMER_MINIMUM), 2.0e9)),
             nullptr);
    armed_ = true;
    armedDue_ = due;
}

void WidgetRuntime::OnTimer() {
    firing_ = true;
    // A USER timer may fire a little before its time: what is due within 12 ms runs now (no
    // second wake-up for it; aligned refreshes keep 30 ms of slack for this).
    const double now = WidgetNowMs() + 12.0;
    // The due ones, earliest first; repeating ones are rescheduled before they run (a callback
    // may cancel or replace its own timer).
    std::vector<Entry> due;
    for (Entry& e : entries_) {
        if (e.due <= now) due.push_back(e);
    }
    std::sort(due.begin(), due.end(), [](const Entry& a, const Entry& b) { return a.due < b.due; });
    for (const Entry& d : due) {
        auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.w == d.w && e.id == d.id; });
        if (it == entries_.end() || it->due > now) continue;  // cancelled / replaced by an earlier callback
        if (it->period == 0) {
            entries_.erase(it);
        } else if (it->aligned) {
            it->due = WidgetNowMs() + WidgetAlignedDelay(it->period);
        } else {
            it->due = std::max(it->due + it->period, WidgetNowMs() + 1.0);
        }
        if (Alive(d.w)) d.w->OnTimer(d.id);
    }
    firing_ = false;
    armed_ = false;
    Arm();
}
