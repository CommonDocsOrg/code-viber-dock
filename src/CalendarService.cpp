#include "CalendarService.h"

#include <shlwapi.h>
#include <winhttp.h>

#include <algorithm>
#include <ctime>
#include <cwctype>
#include <iterator>
#include <new>

#include "HttpClient.h"

namespace {

constexpr DWORD kTimeoutMs = 15000;
constexpr size_t kMaxBody = 32u << 20;
constexpr UINT kFileDebounceMs = 300;

int64_t UnixNow() { return static_cast<int64_t>(_time64(nullptr)); }

std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(std::max(0, n)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(std::max(0, n)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

bool ReadWhole(const std::wstring& path, std::string& out, size_t cap, DWORD& error) {
    out.clear();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return false;
    }
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) != FALSE;
    if (ok && static_cast<uint64_t>(size.QuadPart) > cap) {
        error = ERROR_FILE_TOO_LARGE;
        ok = false;
    }
    if (ok) {
        try {
            out.resize(static_cast<size_t>(size.QuadPart));
        } catch (...) {
            error = ERROR_OUTOFMEMORY;
            ok = false;
        }
    }
    DWORD read = 0;
    if (ok && !out.empty() && (!ReadFile(f, out.data(), static_cast<DWORD>(out.size()), &read, nullptr))) {
        error = GetLastError();
        ok = false;
    }
    if (ok) out.resize(read);
    CloseHandle(f);
    return ok;
}

bool WriteAtomic(const std::wstring& path, const std::string& data) {
    const std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(f, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size();
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

uint64_t FileStamp(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return 0;
    const uint64_t t = (static_cast<uint64_t>(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
    return t ^ (static_cast<uint64_t>(a.nFileSizeLow) * 0x9E3779B97F4A7C15ull) ^ a.nFileSizeHigh;
}

std::wstring Hex64(uint64_t v) {
    wchar_t buf[24];
    swprintf_s(buf, L"%016llx", static_cast<unsigned long long>(v));
    return buf;
}

uint64_t Fnv1a(const std::wstring& s) {
    uint64_t h = 1469598103934665603ull;
    for (wchar_t c : s) {
        h ^= static_cast<uint16_t>(c);
        h *= 1099511628211ull;
    }
    return h;
}

std::wstring HttpErrorText(DWORD error, DWORD status) {
    switch (error) {
        case 0: return L"HTTP " + std::to_wstring(status);
        case ERROR_WINHTTP_TIMEOUT: return L"the request timed out";
        case ERROR_WINHTTP_NAME_NOT_RESOLVED: return L"no network (name not resolved)";
        case ERROR_WINHTTP_CANNOT_CONNECT:
        case ERROR_WINHTTP_CONNECTION_ERROR: return L"cannot connect";
        case ERROR_WINHTTP_SECURE_FAILURE: return L"secure connection failed";
        case ERROR_FILE_TOO_LARGE: return L"the calendar is larger than 32 MB";
        case ERROR_INVALID_PARAMETER: return L"not a valid URL";
        default: {
            wchar_t buf[48];
            swprintf_s(buf, error > 0xFFFF ? L"network error 0x%08lX" : L"network error %lu", error);
            return buf;
        }
    }
}

std::wstring FileErrorText(DWORD error) {
    switch (error) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND: return L"file not found";
        case ERROR_ACCESS_DENIED: return L"access denied";
        case ERROR_SHARING_VIOLATION: return L"the file is in use";
        case ERROR_FILE_TOO_LARGE: return L"the file is larger than 32 MB";
        default: return L"cannot read the file (error " + std::to_wstring(error) + L")";
    }
}

std::wstring FormatClock(int64_t wallSeconds) {
    const int64_t day = Ics::DayOf(wallSeconds);
    const int64_t s = wallSeconds - day * 86400;
    int y = 0, m = 0, d = 0;
    Ics::CivilFromDays(day, y, m, d);
    SYSTEMTIME st{};
    st.wYear = static_cast<WORD>(std::clamp(y, 1601, 30827));
    st.wMonth = static_cast<WORD>(m);
    st.wDay = static_cast<WORD>(d);
    st.wHour = static_cast<WORD>(s / 3600);
    st.wMinute = static_cast<WORD>(s / 60 % 60);
    wchar_t buf[64]{};
    if (!GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, buf, 64)) return {};
    return buf;
}

}  // namespace

// A worker's job: what to fetch / read, and what came back. Owned by the receiver of the post.
struct CalendarService::Job {
    enum Kind { Cache = 0, Web = 1, File = 2 };
    int kind = Cache;
    HWND owner = nullptr;
    uint64_t feedId = 0;
    uint64_t generation = 0;
    std::wstring location;   // URL / path
    std::wstring cachePath;  // web feeds: <hash>.ics (the meta file is <hash>.meta)
    std::wstring etag, lastModified;  // sent as validators (web)
    // Results.
    DWORD error = 0;
    DWORD status = 0;
    std::shared_ptr<Ics::Calendar> cal;
    std::wstring parseError;
    std::wstring newEtag, newLastModified;
    int64_t fetchedAt = 0;  // Cache: when the cached body was fetched
    uint64_t stamp = 0;     // File: its last write time / size
};

namespace {

std::wstring MetaPath(const std::wstring& icsPath) { return icsPath.substr(0, icsPath.size() - 4) + L".meta"; }

bool ParseBody(const std::string& body, CalendarService::Job& job) {
    auto cal = std::make_shared<Ics::Calendar>();
    std::wstring error;
    if (!Ics::Parse(body, *cal, &error)) {
        job.parseError = error.empty() ? L"not an iCalendar file" : error;
        return false;
    }
    job.cal = std::move(cal);
    return true;
}

DWORD WINAPI JobThread(LPVOID param) {
    auto* job = static_cast<CalendarService::Job*>(param);
    try {
        using Job = CalendarService::Job;
        if (job->kind == Job::Cache) {
            // <hash>.meta: url, ETag, Last-Modified, fetch time (one per line, UTF-8).
            std::string meta, body;
            DWORD err = 0;
            if (ReadWhole(MetaPath(job->cachePath), meta, 64 * 1024, err)) {
                std::vector<std::wstring> lines;
                size_t at = 0;
                const std::wstring text = Wide(meta);
                while (at <= text.size()) {
                    size_t end = text.find(L'\n', at);
                    if (end == std::wstring::npos) end = text.size();
                    std::wstring line = text.substr(at, end - at);
                    if (!line.empty() && line.back() == L'\r') line.pop_back();
                    lines.push_back(std::move(line));
                    at = end + 1;
                }
                if (lines.size() >= 4 && Lower(lines[0]) == Lower(job->location) &&
                    ReadWhole(job->cachePath, body, kMaxBody, err) && ParseBody(body, *job)) {
                    job->newEtag = lines[1];
                    job->newLastModified = lines[2];
                    job->fetchedAt = _wtoi64(lines[3].c_str());
                }
            }
        } else if (job->kind == Job::Web) {
            Http::Request rq;
            rq.url = job->location;
            rq.timeoutMs = kTimeoutMs;
            rq.maxBody = kMaxBody;
            rq.headers = L"Accept: text/calendar, */*\r\n";
            if (!job->etag.empty()) rq.headers += L"If-None-Match: " + job->etag + L"\r\n";
            if (!job->lastModified.empty()) rq.headers += L"If-Modified-Since: " + job->lastModified + L"\r\n";
            Http::Result r;
            Http::Perform(rq, r);
            job->error = r.error;
            job->status = r.status;
            job->newEtag = r.etag;
            job->newLastModified = r.lastModified;
            if (!r.error && r.status == 200 && ParseBody(r.body, *job)) {
                CreateDirectoryW(CalendarService::CacheDirectory().c_str(), nullptr);
                const std::wstring meta = job->location + L"\n" + r.etag + L"\n" + r.lastModified + L"\n" +
                                          std::to_wstring(UnixNow()) + L"\n";
                if (WriteAtomic(job->cachePath, r.body)) WriteAtomic(MetaPath(job->cachePath), Utf8(meta));
            }
        } else {
            std::string body;
            job->stamp = FileStamp(job->location);
            if (!ReadWhole(job->location, body, kMaxBody, job->error)) {
                if (!job->error) job->error = ERROR_READ_FAULT;
            } else {
                ParseBody(body, *job);
            }
        }
    } catch (...) {  // std::bad_alloc
        job->cal.reset();
        job->error = ERROR_OUTOFMEMORY;
    }
    if (!PostMessageW(job->owner, CalendarService::kResultMessage, 0, reinterpret_cast<LPARAM>(job))) delete job;
    return 0;
}

struct WatchArgs {
    HWND owner = nullptr;
    HANDLE stop = nullptr;
    LONG generation = 0;
    std::vector<std::wstring> folders;
    std::vector<std::vector<std::wstring>> names;  // per folder: the watched file names, lower case
};

// One thread: ReadDirectoryChangesW on each folder (not recursive, overlapped); a batch that names
// one of the watched files posts kFileMessage(generation, folder). Ends when `stop` is set.
DWORD WINAPI WatchThread(LPVOID param) {
    std::unique_ptr<WatchArgs> args(static_cast<WatchArgs*>(param));
    struct Dir {
        HANDLE handle = INVALID_HANDLE_VALUE;
        OVERLAPPED ov{};
        alignas(DWORD) BYTE buffer[8192];
        size_t index = 0;
    };
    constexpr DWORD kFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE;
    std::vector<std::unique_ptr<Dir>> dirs;
    std::vector<HANDLE> waits{args->stop};
    const size_t n = std::min<size_t>(args->folders.size(), MAXIMUM_WAIT_OBJECTS - 1);
    for (size_t i = 0; i < n; ++i) {
        auto d = std::make_unique<Dir>();
        d->index = i;
        d->handle = CreateFileW(args->folders[i].c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (d->handle == INVALID_HANDLE_VALUE) continue;
        d->ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!d->ov.hEvent ||
            !ReadDirectoryChangesW(d->handle, d->buffer, sizeof(d->buffer), FALSE, kFilter, nullptr, &d->ov, nullptr)) {
            if (d->ov.hEvent) CloseHandle(d->ov.hEvent);
            CloseHandle(d->handle);
            continue;
        }
        waits.push_back(d->ov.hEvent);
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
        bool hit = bytes == 0;  // the buffer overflowed: anything may have changed
        for (DWORD at = 0; !hit && at + sizeof(FILE_NOTIFY_INFORMATION) <= bytes;) {
            const auto* fni = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(d.buffer + at);
            const std::wstring name = Lower(std::wstring(fni->FileName, fni->FileNameLength / sizeof(wchar_t)));
            const auto& names = args->names[d.index];
            if (std::find(names.begin(), names.end(), name) != names.end()) hit = true;
            if (!fni->NextEntryOffset) break;
            at += fni->NextEntryOffset;
        }
        if (hit) PostMessageW(args->owner, CalendarService::kFileMessage, static_cast<WPARAM>(args->generation), d.index);
        if (!ReadDirectoryChangesW(d.handle, d.buffer, sizeof(d.buffer), FALSE, kFilter, nullptr, &d.ov, nullptr)) break;
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

// --- configuration -------------------------------------------------------------------------------

std::wstring CalendarService::CacheDirectory() { return Config::Directory() + L"\\calendar-cache"; }

bool CalendarService::Classify(const std::wstring& url, bool& file, std::wstring& location, std::wstring& problem) {
    file = false;
    location.clear();
    problem.clear();
    std::wstring u = url;
    while (!u.empty() && iswspace(u.front())) u.erase(u.begin());
    while (!u.empty() && iswspace(u.back())) u.pop_back();
    const std::wstring lower = Lower(u);
    if (lower.empty()) {
        problem = L"no URL";
        return false;
    }
    if (lower.rfind(L"webcal://", 0) == 0 || lower.rfind(L"webcals://", 0) == 0) {
        u = L"https://" + u.substr(u.find(L"://") + 3);
    }
    const std::wstring l2 = Lower(u);
    if (l2.rfind(L"http://", 0) == 0 || l2.rfind(L"https://", 0) == 0) {
        bool secure = false;
        std::wstring host, path;
        unsigned short port = 0;
        if (!Http::SplitUrl(u, secure, host, port, path)) {
            problem = L"not a valid URL";
            return false;
        }
        location = u;
        return true;
    }
    if (l2.rfind(L"file:", 0) == 0) {
        wchar_t path[MAX_PATH * 2]{};
        DWORD n = static_cast<DWORD>(std::size(path));
        if (FAILED(PathCreateFromUrlW(u.c_str(), path, &n, 0))) {
            problem = L"not a valid file URL";
            return false;
        }
        u = path;
    }
    wchar_t expanded[MAX_PATH * 2]{};
    if (ExpandEnvironmentStringsW(u.c_str(), expanded, static_cast<DWORD>(std::size(expanded)))) u = expanded;
    if (u.size() < 3 || !((u[1] == L':' && (u[2] == L'\\' || u[2] == L'/')) || (u[0] == L'\\' && u[1] == L'\\'))) {
        problem = L"not an http(s) / webcal URL or a full file path";
        return false;
    }
    std::replace(u.begin(), u.end(), L'/', L'\\');
    file = true;
    location = u;
    return true;
}

size_t CalendarService::EnabledFeeds() const {
    return static_cast<size_t>(std::count_if(feeds_.begin(), feeds_.end(), [](const Feed& f) {
        return f.cfg.enabled && f.problem.empty();
    }));
}

void CalendarService::Configure(const CalendarsConfig& cfg, HWND owner) {
    if (owner != owner_) Stop();
    owner_ = owner;
    cfg_ = cfg;
    std::vector<Feed> next;
    for (const CalendarFeed& c : cfg.feeds) {
        Feed f;
        f.cfg = c;
        Classify(c.url, f.file, f.location, f.problem);
        // The same source, still enabled: keep its data and schedule.
        auto old = std::find_if(feeds_.begin(), feeds_.end(), [&](const Feed& o) {
            return o.id != 0 && o.location == f.location && o.file == f.file && o.cfg.enabled && c.enabled && !f.location.empty();
        });
        if (old != feeds_.end() && active_) {
            Feed kept = std::move(*old);
            old->id = 0;  // taken
            const int previousRefresh = kept.cfg.refreshMin;
            kept.cfg = c;
            if (!kept.file && previousRefresh != c.refreshMin && kept.lastFetch > 0 && !kept.inFlight && kept.failures == 0) {
                kept.due = kept.lastFetch + static_cast<int64_t>(c.refreshMin) * 60;
            }
            next.push_back(std::move(kept));
            continue;
        }
        f.id = nextFeedId_++;
        f.status = c.enabled && f.problem.empty() ? FeedStatus::Loading : f.problem.empty() ? FeedStatus::Off : FeedStatus::Error;
        if (!f.problem.empty()) f.error = f.problem;
        if (!f.location.empty()) f.cacheKey = Hex64(Fnv1a(Lower(f.location)));  // also the reminder ids' source
        next.push_back(std::move(f));
    }
    feeds_ = std::move(next);
    const bool want = EnabledFeeds() > 0 && owner_;
    if (!want) {
        const bool was = active_ || !items_.empty();
        Stop();
        items_.clear();
        byDay_.clear();
        for (Feed& f : feeds_) {
            f.cal.reset();
            f.occurrences = 0;
            if (f.status != FeedStatus::Error) f.status = FeedStatus::Off;
        }
        if (was) Notify();
        return;
    }
    active_ = true;
    for (Feed& f : feeds_) {
        if (!f.cfg.enabled || !f.problem.empty() || f.cal || f.inFlight) continue;
        StartJob(static_cast<size_t>(&f - feeds_.data()), f.file ? Job::File : Job::Cache);
    }
    StartWatcher();
    Arm();
    Rebuild();
    Notify();
}

void CalendarService::Stop() {
    ++generation_;  // results in flight are dropped when they arrive
    if (owner_ && refreshArmed_) KillTimer(owner_, kRefreshTimerId);
    if (owner_ && fileTimerArmed_) KillTimer(owner_, kFileTimerId);
    if (owner_ && reminderArmed_) KillTimer(owner_, kReminderTimerId);
    refreshArmed_ = fileTimerArmed_ = reminderArmed_ = false;
    StopWatcher();
    for (Feed& f : feeds_) f.inFlight = false;
    active_ = false;
}

void CalendarService::RefreshNow() {
    if (!active_) return;
    const int64_t now = Now();
    for (size_t i = 0; i < feeds_.size(); ++i) {
        Feed& f = feeds_[i];
        if (!f.cfg.enabled || !f.problem.empty() || f.inFlight) continue;
        f.failures = 0;
        f.due = now;
        StartJob(i, f.file ? Job::File : Job::Web);
    }
    Arm();
    Notify();
}

void CalendarService::TimeChanged() {
    if (!active_) return;
    Rebuild();
    Notify();
}

// --- jobs ----------------------------------------------------------------------------------------

void CalendarService::StartJob(size_t index, int kind) {
    Feed& f = feeds_[index];
    auto* job = new (std::nothrow) Job;
    if (!job) return;
    job->kind = kind;
    job->owner = owner_;
    job->feedId = f.id;
    job->generation = generation_;
    job->location = f.location;
    if (!f.file) job->cachePath = CacheDirectory() + L"\\" + f.cacheKey + L".ics";
    if (kind == Job::Web && f.cal) {  // validators only when there is data to keep on a 304
        job->etag = f.etag;
        job->lastModified = f.lastModified;
    }
    HANDLE thread = CreateThread(nullptr, 256 * 1024, &JobThread, job, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!thread) {
        delete job;
        f.error = L"could not start the request";
        f.status = FeedStatus::Error;
        return;
    }
    CloseHandle(thread);
    f.inFlight = true;
}

void CalendarService::OnResult(Job* raw) {
    std::unique_ptr<Job> job(raw);
    if (job->generation != generation_ || !active_) return;
    auto it = std::find_if(feeds_.begin(), feeds_.end(), [&](const Feed& f) { return f.id == job->feedId; });
    if (it == feeds_.end()) return;
    Feed& f = *it;
    f.inFlight = false;
    const int64_t now = Now();
    bool dataChanged = false;
    const int64_t refresh = static_cast<int64_t>(f.cfg.refreshMin) * 60;
    auto fail = [&](std::wstring error) {
        f.error = std::move(error);
        f.status = FeedStatus::Error;
        if (!f.file) {
            ++f.failures;
            const int shift = std::min(f.failures - 1, 10);
            const int64_t minutes = std::min<int64_t>(int64_t{1} << shift, f.cfg.refreshMin);
            f.due = now + minutes * 60;
        }
        wchar_t buf[512];
        swprintf_s(buf, L"WinDock: calendar \"%s\": %s\n", f.cfg.name.c_str(), f.error.c_str());
        OutputDebugStringW(buf);
    };
    switch (job->kind) {
        case Job::Cache:
            if (job->cal) {
                f.cal = std::move(job->cal);
                f.etag = job->newEtag;
                f.lastModified = job->newLastModified;
                f.lastFetch = job->fetchedAt;
                f.fromCache = true;
                f.status = FeedStatus::Ok;
                dataChanged = true;
                // Due refreshMin after that fetch (now if that is past).
                f.due = std::max(now, job->fetchedAt + refresh);
            } else {
                f.due = now;
            }
            break;
        case Job::Web:
            ++fetches_;
            if (!job->error && job->status == 304 && f.cal) {
                ++notModified_;
                f.lastFetch = now;
                f.fromCache = false;
                f.failures = 0;
                f.error.clear();
                f.status = FeedStatus::Ok;
                f.due = now + refresh;
            } else if (!job->error && job->status == 200 && job->cal) {
                f.cal = std::move(job->cal);
                f.etag = job->newEtag;
                f.lastModified = job->newLastModified;
                f.lastFetch = now;
                f.fromCache = false;
                f.failures = 0;
                f.error.clear();
                f.status = FeedStatus::Ok;
                f.due = now + refresh;
                dataChanged = true;
            } else if (!job->error && job->status == 200) {
                fail(job->parseError.empty() ? std::wstring(L"not an iCalendar file") : job->parseError);
            } else {
                fail(HttpErrorText(job->error, job->status));
            }
            break;
        case Job::File:
        default:
            ++fileReads_;
            if (job->cal) {
                f.cal = std::move(job->cal);
                f.fileStamp = job->stamp;
                f.lastFetch = now;
                f.error.clear();
                f.status = FeedStatus::Ok;
                dataChanged = true;
            } else if (job->error) {
                f.fileStamp = job->stamp;
                fail(FileErrorText(job->error));
            } else {
                f.fileStamp = job->stamp;
                fail(job->parseError.empty() ? std::wstring(L"not an iCalendar file") : job->parseError);
            }
            break;
    }
    Arm();
    if (dataChanged) Rebuild();
    Notify();
}

// --- timers --------------------------------------------------------------------------------------

void CalendarService::Arm() {
    if (!owner_) return;
    int64_t due = INT64_MAX;
    if (active_) {
        for (const Feed& f : feeds_) {
            if (f.file || !f.cfg.enabled || !f.problem.empty() || f.inFlight || f.due <= 0) continue;
            due = std::min(due, f.due);
        }
    }
    if (due == INT64_MAX) {
        if (refreshArmed_) KillTimer(owner_, kRefreshTimerId);
        refreshArmed_ = false;
        return;
    }
    if (refreshArmed_ && armedDue_ == due) return;
    const int64_t ms = std::clamp<int64_t>((due - Now()) * 1000, USER_TIMER_MINIMUM, 0x7FFFFFF0);
    SetTimer(owner_, kRefreshTimerId, static_cast<UINT>(ms), nullptr);
    refreshArmed_ = true;
    armedDue_ = due;
}

void CalendarService::OnRefreshTimer() {
    KillTimer(owner_, kRefreshTimerId);
    refreshArmed_ = false;
    const int64_t now = Now();
    bool started = false;
    for (size_t i = 0; i < feeds_.size(); ++i) {
        Feed& f = feeds_[i];
        if (f.file || !f.cfg.enabled || !f.problem.empty() || f.inFlight || f.due <= 0 || f.due > now + 1) continue;
        StartJob(i, Job::Web);
        started = true;
    }
    Arm();
    if (started) Notify();
}

int64_t CalendarService::NextDueIn() const {
    if (!refreshArmed_) return -1;
    return std::max<int64_t>(0, armedDue_ - Now());
}

// --- file watching -------------------------------------------------------------------------------

void CalendarService::StartWatcher() {
    std::vector<std::wstring> folders;
    std::vector<std::vector<std::wstring>> names;
    for (const Feed& f : feeds_) {
        if (!f.file || !f.cfg.enabled || !f.problem.empty()) continue;
        const size_t cut = f.location.find_last_of(L'\\');
        if (cut == std::wstring::npos || cut == 0) continue;
        std::wstring folder = Lower(f.location.substr(0, cut));
        if (folder.size() == 2) folder += L'\\';  // "c:" -> "c:\"
        const std::wstring name = Lower(f.location.substr(cut + 1));
        auto at = std::find(folders.begin(), folders.end(), folder);
        if (at == folders.end()) {
            folders.push_back(folder);
            names.push_back({name});
        } else {
            names[static_cast<size_t>(at - folders.begin())].push_back(name);
        }
    }
    if (folders == watchedFolders_ && (watchThread_ != nullptr) == !folders.empty()) return;
    StopWatcher();
    watchedFolders_ = folders;
    folderDirty_.assign(folders.size(), false);
    if (folders.empty() || !owner_) return;
    auto* args = new (std::nothrow) WatchArgs;
    if (!args) return;
    watchStop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    args->owner = owner_;
    args->stop = watchStop_;
    args->generation = ++watchGeneration_;
    args->folders = std::move(folders);
    args->names = std::move(names);
    watchThread_ = watchStop_ ? CreateThread(nullptr, 64 * 1024, &WatchThread, args, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr)
                              : nullptr;
    if (!watchThread_) {
        delete args;
        if (watchStop_) CloseHandle(watchStop_);
        watchStop_ = nullptr;
    }
}

void CalendarService::StopWatcher() {
    ++watchGeneration_;
    if (watchThread_) {
        SetEvent(watchStop_);
        WaitForSingleObject(watchThread_, 2000);
        CloseHandle(watchThread_);
        watchThread_ = nullptr;
    }
    if (watchStop_) CloseHandle(watchStop_);
    watchStop_ = nullptr;
    watchedFolders_.clear();
    folderDirty_.clear();
}

void CalendarService::OnFileChanged(WPARAM generation, LPARAM folder) {
    if (static_cast<LONG>(generation) != watchGeneration_ || folder < 0 || static_cast<size_t>(folder) >= folderDirty_.size()) {
        return;
    }
    folderDirty_[static_cast<size_t>(folder)] = true;
    if (!fileTimerArmed_ && owner_) fileTimerArmed_ = SetTimer(owner_, kFileTimerId, kFileDebounceMs, nullptr) != 0;
}

void CalendarService::OnFileTimer() {
    KillTimer(owner_, kFileTimerId);
    fileTimerArmed_ = false;
    for (size_t i = 0; i < feeds_.size(); ++i) {
        Feed& f = feeds_[i];
        if (!f.file || !f.cfg.enabled || !f.problem.empty() || f.inFlight) continue;
        const size_t cut = f.location.find_last_of(L'\\');
        std::wstring folder = Lower(f.location.substr(0, cut));
        if (folder.size() == 2) folder += L'\\';
        const auto at = std::find(watchedFolders_.begin(), watchedFolders_.end(), folder);
        if (at == watchedFolders_.end() || !folderDirty_[static_cast<size_t>(at - watchedFolders_.begin())]) continue;
        if (FileStamp(f.location) == f.fileStamp && f.fileStamp != 0) continue;  // touched, not changed
        StartJob(i, Job::File);
    }
    std::fill(folderDirty_.begin(), folderDirty_.end(), false);
}

bool CalendarService::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == kResultMessage) {
        OnResult(reinterpret_cast<Job*>(lp));
        return true;
    }
    if (msg == kFileMessage) {
        OnFileChanged(wp, lp);
        return true;
    }
    if (msg == WM_TIMER && wp == kRefreshTimerId) {
        OnRefreshTimer();
        return true;
    }
    if (msg == WM_TIMER && wp == kFileTimerId) {
        OnFileTimer();
        return true;
    }
    if (msg == WM_TIMER && wp == kReminderTimerId) {
        OnReminderTimer();
        return true;
    }
    return false;
}

// --- the expansion -------------------------------------------------------------------------------

int64_t CalendarService::Now() { return UnixNow(); }

int64_t CalendarService::Today() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    return Ics::DaysFromCivil(st.wYear, st.wMonth, st.wDay);
}

void CalendarService::Rebuild() {
    items_.clear();
    byDay_.clear();
    local_ = Ics::LocalZone();
    const int64_t today = Today();
    winFrom_ = today - kDaysBack;
    winTo_ = today + kDaysAhead;
    if (reqTo_ >= reqFrom_) {
        winFrom_ = std::min(winFrom_, reqFrom_);
        winTo_ = std::max(winTo_, reqTo_);
    }
    if (!active_) {
        for (Feed& f : feeds_) f.occurrences = 0;
        ArmReminders();
        return;
    }
    const int64_t from = local_->ToUtc(winFrom_ * 86400), to = local_->ToUtc((winTo_ + 1) * 86400);
    for (size_t k = 0; k < feeds_.size(); ++k) {
        Feed& f = feeds_[k];
        f.occurrences = 0;
        if (!f.cfg.enabled || !f.cal) continue;
        const std::vector<Ics::Occurrence> occ = Ics::Expand(*f.cal, from, to, *local_);
        f.occurrences = occ.size();
        for (const Ics::Occurrence& o : occ) items_.push_back(Item{o, static_cast<uint32_t>(k)});
    }
    std::stable_sort(items_.begin(), items_.end(), [](const Item& a, const Item& b) {
        if (a.occ.start != b.occ.start) return a.occ.start < b.occ.start;
        if (a.occ.allDay != b.occ.allDay) return a.occ.allDay;
        return a.feed < b.feed;
    });
    for (size_t i = 0; i < items_.size(); ++i) {
        const Ics::Occurrence& o = items_[i].occ;
        const int64_t first = std::max(o.firstDay, winFrom_), last = std::min({o.lastDay, winTo_, first + 400});
        for (int64_t d = first; d <= last; ++d) byDay_[d].push_back(static_cast<uint32_t>(i));
    }
    for (auto& [day, list] : byDay_) {
        std::stable_sort(list.begin(), list.end(), [&](uint32_t a, uint32_t b) {
            const Item& x = items_[a];
            const Item& y = items_[b];
            const bool xa = x.occ.allDay || x.occ.firstDay < day, ya = y.occ.allDay || y.occ.firstDay < day;
            if (xa != ya) return xa;  // all-day (and continuing) first
            return x.occ.start < y.occ.start;
        });
    }
    ArmReminders();
}

void CalendarService::EnsureDays(int64_t from, int64_t to) {
    if (to < from) return;
    if (from >= winFrom_ && to <= winTo_) return;
    // Keep the window bounded (about three years): a request far away replaces the older one.
    if (reqTo_ >= reqFrom_ && std::max(reqTo_, to) - std::min(reqFrom_, from) <= 1100) {
        reqFrom_ = std::min(reqFrom_, from);
        reqTo_ = std::max(reqTo_, to);
    } else {
        reqFrom_ = from;
        reqTo_ = to;
    }
    Rebuild();
}

std::vector<size_t> CalendarService::On(int64_t day) const {
    std::vector<size_t> out;
    auto it = byDay_.find(day);
    if (it == byDay_.end()) return out;
    out.assign(it->second.begin(), it->second.end());
    return out;
}

size_t CalendarService::CountOn(int64_t day) const {
    auto it = byDay_.find(day);
    return it == byDay_.end() ? 0 : it->second.size();
}

// --- texts ---------------------------------------------------------------------------------------

std::wstring CalendarService::TimeText(int64_t utc) const { return FormatClock(LocalWall(utc)); }

std::wstring CalendarService::RangeText(const Item& item, int64_t day) const {
    const Ics::Occurrence& o = item.occ;
    if (o.allDay) return L"All day";
    const bool startsToday = o.firstDay == day, endsToday = o.lastDay == day;
    if (startsToday && endsToday) {
        if (o.end <= o.start) return TimeText(o.start);
        return TimeText(o.start) + L" \x2013 " + TimeText(o.end);
    }
    if (startsToday) return L"From " + TimeText(o.start);
    if (endsToday) return L"Until " + TimeText(o.end);
    return L"All day";
}

std::wstring CalendarService::StatusText(size_t index) const {
    if (index >= feeds_.size()) return {};
    const Feed& f = feeds_[index];
    if (!f.problem.empty()) return L"Error: " + f.problem;
    if (!f.cfg.enabled) return L"Off";
    std::wstring s;
    if (f.cal) {
        s = (f.lastFetch > 0 ? L"Updated " + FormatClock(LocalWall(f.lastFetch)) : std::wstring(L"Loaded")) + L", " +
            std::to_wstring(f.occurrences) + (f.occurrences == 1 ? L" event" : L" events");
        if (f.fromCache) s += L" (cached)";
    }
    if (f.inFlight) return s.empty() ? std::wstring(L"Loading\x2026") : s + L"; updating\x2026";
    if (!f.error.empty()) {
        if (!s.empty()) s += L"; ";
        s += L"error: " + f.error;
        if (!f.file && f.due > 0) {
            const int64_t mins = std::max<int64_t>(1, (f.due - Now() + 59) / 60);
            s += L"; retry in " + std::to_wstring(mins) + L" min";
        }
        if (!f.cal) s[0] = L'E';
        return s;
    }
    return s.empty() ? std::wstring(L"Waiting") : s;
}

// --- automation ----------------------------------------------------------------------------------

int CalendarService::State() const {
    if (!active_) return 0;
    for (const Feed& f : feeds_) {
        if (f.cfg.enabled && f.status == FeedStatus::Error) return 2;
    }
    return 1;
}

int64_t CalendarService::LastFetchAge() const {
    int64_t newest = 0;
    for (const Feed& f : feeds_) {
        if (f.cfg.enabled && !f.fromCache) newest = std::max(newest, f.lastFetch);
    }
    return newest > 0 ? std::max<int64_t>(0, Now() - newest) : -1;
}

// --- the widgets' provider (13b) -----------------------------------------------------------------

namespace {

std::string JsonText(const std::wstring& w) {
    const std::string s = Utf8(w);
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    sprintf_s(buf, "\\u%04x", c);
                    o += buf;
                } else {
                    o += static_cast<char>(c);
                }
        }
    }
    return o + "\"";
}

std::string DayText(int64_t day) {
    int y = 0, m = 0, d = 0;
    Ics::CivilFromDays(day, y, m, d);
    char buf[16];
    sprintf_s(buf, "%04d-%02d-%02d", y, m, d);
    return buf;
}

std::string ColorText(uint32_t argb) {
    char buf[16];
    sprintf_s(buf, "#%08X", argb);
    return buf;
}

}  // namespace

std::wstring CalendarService::IdOf(const Item& item) const {
    const Feed& f = feeds_[item.feed];
    const Ics::Event& ev = EventOf(item);
    return f.cacheKey + L"|" + ev.uid + L"|" + std::to_wstring(item.occ.originalStart);
}

std::string CalendarService::EventJson(const Item& item) const {
    const Feed& f = feeds_[item.feed];
    const Ics::Event& ev = EventOf(item);
    const Ics::Occurrence& o = item.occ;
    std::string j = "{\"id\":" + JsonText(IdOf(item)) + ",\"feed\":" + JsonText(f.cfg.name) + ",\"color\":\"" +
                    ColorText(f.cfg.color) + "\",\"title\":" + JsonText(ev.summary) + ",\"location\":" +
                    JsonText(ev.location) + ",\"description\":" + JsonText(ev.description) +
                    ",\"start\":" + std::to_string(o.start * 1000) + ",\"end\":" + std::to_string(o.end * 1000) +
                    ",\"allDay\":" + (o.allDay ? "true" : "false") + ",\"url\":" + JsonText(ev.url) +
                    ",\"recurring\":" + (o.recurring ? "true" : "false") + ",\"startDay\":\"" + DayText(o.firstDay) +
                    "\",\"endDay\":\"" + DayText(o.lastDay) + "\",\"startTime\":" +
                    JsonText(o.allDay ? std::wstring() : TimeText(o.start)) + ",\"endTime\":" +
                    JsonText(o.allDay ? std::wstring() : TimeText(o.end)) + ",\"reminder\":" +
                    std::to_string(ev.reminderMin) + "}";
    return j;
}

std::string CalendarService::FeedsJson() const {
    static const char* const kStatus[] = {"off", "loading", "ok", "error"};
    std::string j = "[";
    for (size_t i = 0; i < feeds_.size(); ++i) {
        const Feed& f = feeds_[i];
        if (i) j += ",";
        j += "{\"name\":" + JsonText(f.cfg.name) + ",\"color\":\"" + ColorText(f.cfg.color) + "\",\"enabled\":" +
             (f.cfg.enabled ? "true" : "false") + ",\"file\":" + (f.file ? "true" : "false") + ",\"status\":\"" +
             kStatus[static_cast<int>(f.status)] + "\",\"error\":" + JsonText(f.error) + ",\"lastFetch\":" +
             std::to_string(f.lastFetch * 1000) + ",\"events\":" + std::to_string(f.occurrences) + "}";
    }
    return j + "]";
}

// --- reminders (13b) -----------------------------------------------------------------------------

int CalendarService::ReminderMinutes(const Feed& f) const {
    return f.cfg.reminderMin > 0 ? f.cfg.reminderMin : cfg_.defaultReminderMin;
}

void CalendarService::ArmReminders() {
    if (!owner_) return;
    int64_t next = INT64_MAX;
    const int64_t now = Now();
    if (active_) {
        for (const Item& it : items_) {
            const int minutes = ReminderMinutes(feeds_[it.feed]);
            if (minutes <= 0 || it.occ.allDay || now >= it.occ.start + kReminderGraceS) continue;
            const int64_t due = it.occ.start - static_cast<int64_t>(minutes) * 60;
            if (due >= next) continue;  // items_ is sorted by start, not by due: no early exit
            if (fired_.count(IdOf(it))) continue;
            next = due;
        }
        for (const auto& [id, until] : snoozed_) next = std::min(next, until);
    }
    if (next == INT64_MAX) {
        if (reminderArmed_) KillTimer(owner_, kReminderTimerId);
        reminderArmed_ = false;
        return;
    }
    next = std::max(next, now);
    if (reminderArmed_ && reminderDue_ == next) return;
    const int64_t ms = std::clamp<int64_t>((next - now) * 1000 + 20, USER_TIMER_MINIMUM, 0x7FFFFFF0);
    SetTimer(owner_, kReminderTimerId, static_cast<UINT>(ms), nullptr);
    reminderArmed_ = true;
    reminderDue_ = next;
}

void CalendarService::Fire(const Item& it, const std::wstring& id) {
    fired_.insert(id);
    ++remindersFired_;
    if (!reminder_) return;
    ReminderInfo r;
    r.id = id;
    const Ics::Event& ev = EventOf(it);
    r.title = ev.summary.empty() ? std::wstring(L"(No title)") : ev.summary;
    r.location = ev.location;
    r.when = TimeText(it.occ.start);
    r.start = it.occ.start;
    r.day = it.occ.firstDay;
    r.color = feeds_[it.feed].cfg.color;
    r.json = EventJson(it);
    reminder_(r);
}

void CalendarService::OnReminderTimer() {
    KillTimer(owner_, kReminderTimerId);
    reminderArmed_ = false;
    if (!active_) return;
    const int64_t now = Now() + 1;
    // Collected first: a callback may reconfigure (it must not see half-fired state).
    std::vector<std::pair<size_t, std::wstring>> due;
    for (size_t i = 0; i < items_.size(); ++i) {
        const Item& it = items_[i];
        const int minutes = ReminderMinutes(feeds_[it.feed]);
        if (minutes <= 0 || it.occ.allDay || now >= it.occ.start + kReminderGraceS) continue;
        if (it.occ.start - static_cast<int64_t>(minutes) * 60 > now) continue;
        std::wstring id = IdOf(it);
        if (!fired_.count(id)) due.emplace_back(i, std::move(id));
    }
    for (auto s = snoozed_.begin(); s != snoozed_.end();) {
        if (s->second > now) {
            ++s;
            continue;
        }
        const std::wstring id = s->first;
        s = snoozed_.erase(s);
        for (size_t i = 0; i < items_.size(); ++i) {
            if (IdOf(items_[i]) == id) {
                fired_.erase(id);
                due.emplace_back(i, id);
                break;
            }
        }
    }
    const std::vector<Item> snapshot = items_;
    for (const auto& [index, id] : due) Fire(snapshot[index], id);
    ArmReminders();
}

size_t CalendarService::PendingReminders() const {
    if (!active_) return 0;
    const int64_t now = Now();
    size_t n = snoozed_.size();
    for (const Item& it : items_) {
        if (ReminderMinutes(feeds_[it.feed]) <= 0 || it.occ.allDay || now >= it.occ.start + kReminderGraceS) continue;
        if (!fired_.count(IdOf(it))) ++n;
    }
    return n;
}

int64_t CalendarService::NextReminderIn() const {
    if (!reminderArmed_) return -1;
    return std::max<int64_t>(0, reminderDue_ - Now());
}

bool CalendarService::FireNextReminder() {
    if (!active_) return false;
    const int64_t now = Now();
    const Item* best = nullptr;
    int64_t bestDue = INT64_MAX;
    std::wstring bestId;
    for (const Item& it : items_) {
        const int minutes = ReminderMinutes(feeds_[it.feed]);
        if (minutes <= 0 || it.occ.allDay || now >= it.occ.start + kReminderGraceS) continue;
        const int64_t due = it.occ.start - static_cast<int64_t>(minutes) * 60;
        if (due >= bestDue) continue;
        std::wstring id = IdOf(it);
        if (fired_.count(id)) continue;
        best = &it;
        bestDue = due;
        bestId = std::move(id);
    }
    if (!best && !snoozed_.empty()) {
        auto s = std::min_element(snoozed_.begin(), snoozed_.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
        for (const Item& it : items_) {
            if (IdOf(it) == s->first) {
                best = &it;
                bestId = s->first;
                break;
            }
        }
        snoozed_.erase(s);
        if (best) fired_.erase(bestId);
    }
    if (!best) return false;
    const Item copy = *best;
    Fire(copy, bestId);
    ArmReminders();
    return true;
}

void CalendarService::Snooze(const std::wstring& id, int seconds) {
    if (!active_ || id.empty()) return;
    snoozed_[id] = Now() + std::max(1, seconds);
    ArmReminders();
}
