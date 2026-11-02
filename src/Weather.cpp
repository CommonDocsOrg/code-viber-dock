#include "Weather.h"

#include <winhttp.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <limits>
#include <memory>
#include <new>

#include "JsonUtil.h"

using namespace JsonUtil;

namespace {

constexpr wchar_t kForecastHost[] = L"api.open-meteo.com";
constexpr wchar_t kGeocodeHost[] = L"geocoding-api.open-meteo.com";
constexpr wchar_t kUserAgent[] = L"WinDock/0.1";
constexpr int kTimeoutMs = 5000;  // resolve, connect, send and receive, each
constexpr UINT kMinuteMs = 60u * 1000u;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

struct FetchJob {
    HWND hwnd = nullptr;
    UINT msg = 0;
    WPARAM tag = 0;
    std::wstring host;
    std::wstring path;
};

// One synchronous HTTPS GET. Runs on the worker thread: WinHTTP only.
void HttpGet(const std::wstring& host, const std::wstring& path, WeatherService::Response& r) {
    HINTERNET session = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        r.error = GetLastError();
        return;
    }
    WinHttpSetTimeouts(session, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);
    // No WINHTTP_OPTION_DECOMPRESSION: on Windows 11 build 26300 it makes WinHttpQueryDataAvailable
    // fail with E_ABORT in synchronous mode (the responses are a few KB anyway).
    HINTERNET connect = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                : nullptr;
    bool ok = request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0,
                                            0) &&
              WinHttpReceiveResponse(request, nullptr);
    if (ok) {
        DWORD status = 0, size = sizeof(status);
        ok = WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) != FALSE;
        r.status = status;
    }
    while (ok) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            ok = false;
            break;
        }
        if (available == 0) break;
        if (r.body.size() + available > WeatherService::kMaxBody) {
            r.error = ERROR_FILE_TOO_LARGE;
            r.body.clear();
            break;
        }
        const size_t at = r.body.size();
        r.body.resize(at + available);
        DWORD read = 0;
        if (!WinHttpReadData(request, r.body.data() + at, available, &read)) {
            ok = false;
            break;
        }
        r.body.resize(at + read);
    }
    if (!ok && !r.error) r.error = GetLastError() ? GetLastError() : ERROR_GEN_FAILURE;
    if (r.error) r.body.clear();
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
}

DWORD WINAPI FetchThread(LPVOID param) {
    std::unique_ptr<FetchJob> job(static_cast<FetchJob*>(param));
    auto* response = new (std::nothrow) WeatherService::Response;
    if (!response) return 0;
    try {
        HttpGet(job->host, job->path, *response);
    } catch (...) {  // std::bad_alloc from the body
        response->body.clear();
        response->error = ERROR_OUTOFMEMORY;
    }
    // The receiver owns the response; a window that is gone cannot, so free it here.
    if (!PostMessageW(job->hwnd, job->msg, job->tag, reinterpret_cast<LPARAM>(response))) delete response;
    return 0;
}

bool StartFetch(HWND hwnd, UINT msg, WPARAM tag, const wchar_t* host, std::wstring path) {
    if (!hwnd) return false;
    auto* job = new (std::nothrow) FetchJob{hwnd, msg, tag, host, std::move(path)};
    if (!job) return false;
    HANDLE thread = CreateThread(nullptr, 64 * 1024, &FetchThread, job, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!thread) {
        delete job;
        return false;
    }
    CloseHandle(thread);  // the thread ends by itself after posting
    return true;
}

std::wstring UrlEncode(const std::wstring& text) {
    const std::string utf8 = WideToUtf8(text);
    std::wstring out;
    for (unsigned char c : utf8) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out += static_cast<wchar_t>(c);
        } else {
            wchar_t hex[4];
            swprintf_s(hex, L"%%%02X", c);
            out += hex;
        }
    }
    return out;
}

std::wstring CoordText(double v) {
    wchar_t buf[32];
    swprintf_s(buf, L"%.4f", v);
    return buf;
}

// Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant's days_from_civil).
int64_t DaysFromCivil(int y, int m, int d) {
    y -= m <= 2 ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// Reads `digits` decimal digits at s[at] (advancing `at`). False if they are not all digits.
bool Digits(const std::wstring& s, size_t& at, size_t digits, int& value) {
    if (at + digits > s.size()) return false;
    value = 0;
    for (size_t i = 0; i < digits; ++i) {
        const wchar_t c = s[at + i];
        if (c < L'0' || c > L'9') return false;
        value = value * 10 + (c - L'0');
    }
    at += digits;
    return true;
}

bool Expect(const std::wstring& s, size_t& at, wchar_t c) {
    if (at >= s.size() || s[at] != c) return false;
    ++at;
    return true;
}

// "2026-11-02" (Open-Meteo's ISO 8601 dates) -> days since the epoch. False if malformed.
bool ParseDate(const std::wstring& s, int64_t& days, size_t* end = nullptr) {
    size_t at = 0;
    int y = 0, m = 0, d = 0;
    if (!Digits(s, at, 4, y) || !Expect(s, at, L'-') || !Digits(s, at, 2, m) || !Expect(s, at, L'-') ||
        !Digits(s, at, 2, d) || m < 1 || m > 12 || d < 1 || d > 31) {
        return false;
    }
    days = DaysFromCivil(y, m, d);
    if (end) *end = at;
    return true;
}

// "2026-11-02T10:00" -> minutes since the epoch, as if UTC (the location's local time).
bool ParseDateTime(const std::wstring& s, int64_t& minutes, int& hour) {
    int64_t days = 0;
    size_t at = 0;
    int h = 0, mi = 0;
    if (!ParseDate(s, days, &at) || !Expect(s, at, L'T') || !Digits(s, at, 2, h) || !Expect(s, at, L':') ||
        !Digits(s, at, 2, mi) || h > 23 || mi > 59) {
        return false;
    }
    minutes = days * 1440 + h * 60 + mi;
    hour = h;
    return true;
}

int Weekday(int64_t days) {  // 1970-01-01 was a Thursday; 0 = Monday
    return static_cast<int>(((days % 7) + 7 + 3) % 7);
}

double At(const std::vector<double>& v, size_t i) { return i < v.size() ? v[i] : kNaN; }
int CodeAt(const std::vector<double>& v, size_t i) {
    const double c = At(v, i);
    return std::isfinite(c) ? static_cast<int>(c) : -1;
}

// The Open-Meteo forecast response -> Forecast. False if the essentials are missing.
bool ParseForecast(const json::JsonObject& root, WeatherService::Forecast& out) {
    WeatherService::Forecast f;
    double offset = 0.0;
    ReadNumber(root, L"utc_offset_seconds", offset);
    f.utcOffset = std::isfinite(offset) ? static_cast<int>(offset) : 0;

    const json::JsonObject current = ReadObject(root, L"current");
    if (!current) return false;
    double temp = kNaN, code = kNaN, isDay = 1.0;
    f.wind = kNaN;
    f.humidity = kNaN;
    ReadNumber(current, L"temperature_2m", temp);
    ReadNumber(current, L"weather_code", code);
    ReadNumber(current, L"is_day", isDay);
    ReadNumber(current, L"wind_speed_10m", f.wind);
    ReadNumber(current, L"relative_humidity_2m", f.humidity);
    if (!std::isfinite(temp) || !std::isfinite(code)) return false;
    f.temp = temp;
    f.code = static_cast<int>(code);
    f.isDay = isDay != 0.0;

    if (const json::JsonObject hourly = ReadObject(root, L"hourly")) {
        std::vector<std::wstring> times;
        std::vector<double> temps, codes, precip, day;
        ReadStrings(hourly, L"time", times);
        ReadNumbers(hourly, L"temperature_2m", temps);
        ReadNumbers(hourly, L"weather_code", codes);
        ReadNumbers(hourly, L"precipitation_probability", precip);
        ReadNumbers(hourly, L"is_day", day);
        f.hourly.reserve(times.size());
        for (size_t i = 0; i < times.size(); ++i) {
            WeatherService::Hour h;
            if (!ParseDateTime(times[i], h.key, h.hour)) continue;
            h.temp = At(temps, i);
            h.code = CodeAt(codes, i);
            h.precip = At(precip, i);
            const double d = At(day, i);
            h.isDay = std::isfinite(d) ? d != 0.0 : (h.hour >= 6 && h.hour < 20);
            f.hourly.push_back(h);
        }
    }
    if (const json::JsonObject daily = ReadObject(root, L"daily")) {
        std::vector<std::wstring> times;
        std::vector<double> codes, tmax, tmin;
        ReadStrings(daily, L"time", times);
        ReadNumbers(daily, L"weather_code", codes);
        ReadNumbers(daily, L"temperature_2m_max", tmax);
        ReadNumbers(daily, L"temperature_2m_min", tmin);
        f.daily.reserve(times.size());
        for (size_t i = 0; i < times.size(); ++i) {
            WeatherService::Day d;
            if (!ParseDate(times[i], d.day)) continue;
            d.weekday = Weekday(d.day);
            d.code = CodeAt(codes, i);
            d.tmax = At(tmax, i);
            d.tmin = At(tmin, i);
            f.daily.push_back(d);
        }
    }
    f.valid = true;
    out = std::move(f);
    return true;
}

bool ParseBody(const std::string& body, json::JsonObject& root) {
    root = nullptr;
    try {
        json::JsonObject parsed{nullptr};
        if (!json::JsonObject::TryParse(Utf8ToWide(body), parsed) || !parsed) return false;
        root = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

int64_t UnixNow() { return static_cast<int64_t>(_time64(nullptr)); }

}  // namespace

std::wstring WeatherService::Place::Label() const {
    std::wstring s = name;
    for (const std::wstring* part : {&admin1, &country}) {
        if (part->empty() || *part == name) continue;
        if (!s.empty()) s += L", ";
        s += *part;
    }
    return s;
}

std::wstring WeatherService::CachePath() { return Config::Directory() + L"\\weather-cache.json"; }

std::wstring WeatherService::ForecastPath() const {
    std::wstring p = L"/v1/forecast?latitude=" + CoordText(cfg_.latitude) + L"&longitude=" + CoordText(cfg_.longitude) +
                     L"&current=temperature_2m,weather_code,is_day,wind_speed_10m,relative_humidity_2m"
                     L"&hourly=temperature_2m,weather_code,precipitation_probability,is_day"
                     L"&daily=weather_code,temperature_2m_max,temperature_2m_min&forecast_days=7&timezone=auto";
    if (cfg_.units == Units::Imperial) p += L"&temperature_unit=fahrenheit&wind_speed_unit=mph";
    return p;
}

void WeatherService::Configure(const WeatherConfig& cfg, bool shown, HWND hwnd) {
    const bool place = !configured_ || hwnd != hwnd_ || cfg.latitude != cfg_.latitude ||
                       cfg.longitude != cfg_.longitude || cfg.units != cfg_.units;
    const bool interval = cfg.refreshMinutes != cfg_.refreshMinutes;
    const bool wasActive = Active();
    hwnd_ = hwnd;
    cfg_ = cfg;
    shown_ = shown;
    configured_ = true;
    if (!Active()) {
        Stop();
        if (place) {
            data_ = Forecast{};
            status_ = Status::None;
            error_.clear();
            failures_ = 0;
        }
        Notify();
        return;
    }
    if (place) {
        Stop();  // a result in flight is for the old place
        data_ = Forecast{};
        status_ = Status::None;
        error_.clear();
        failures_ = 0;
        if (LoadCache()) status_ = Status::Stale;
        Fetch();
        Notify();
        return;
    }
    if (!wasActive) {
        failures_ = 0;
        Fetch();
        Notify();
        return;
    }
    if (interval && timerPending_ && status_ == Status::Fresh) {
        // Next refresh: refreshMinutes after the last fetch (at least a minute from now).
        const int64_t due = data_.fetchedAt + static_cast<int64_t>(cfg_.refreshMinutes) * 60 - UnixNow();
        Schedule(static_cast<UINT>(std::clamp<int64_t>(due, 60, static_cast<int64_t>(cfg_.refreshMinutes) * 60)) *
                 1000u);
    }
}

void WeatherService::Stop() {
    CancelTimer();
    ++generation_;  // a fetch in flight is ignored when it arrives
}

void WeatherService::RefreshNow() {
    if (!Active()) return;
    failures_ = 0;
    Fetch();
    Notify();
}

void WeatherService::CancelTimer() {
    if (timerPending_ && hwnd_) KillTimer(hwnd_, kTimerId);
    timerPending_ = false;
}

void WeatherService::Schedule(UINT ms) {
    if (!hwnd_) return;
    SetTimer(hwnd_, kTimerId, std::max<UINT>(ms, USER_TIMER_MINIMUM), nullptr);  // replaces a pending one
    timerPending_ = true;
}

void WeatherService::Fetch() {
    CancelTimer();
    if (!Active() || Busy()) return;  // one request at a time
    if (!StartFetch(hwnd_, kResultMessage, generation_, kForecastHost, ForecastPath())) {
        OnFailure(L"could not start the request");
        return;
    }
    inFlight_ = true;
    inFlightGen_ = generation_;
}

bool WeatherService::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == kResultMessage) {
        OnResponse(wp, reinterpret_cast<Response*>(lp));
        return true;
    }
    if (msg == WM_TIMER && wp == kTimerId) {
        KillTimer(hwnd_, kTimerId);  // one-shot
        timerPending_ = false;
        Fetch();
        Notify();
        return true;
    }
    return false;
}

void WeatherService::OnResponse(WPARAM generation, Response* raw) {
    std::unique_ptr<Response> response(raw);
    if (generation == inFlightGen_) inFlight_ = false;
    if (generation != generation_ || !Active() || !response) return;  // stale: dropped (and freed)
    if (response->error || response->status != 200) {
        OnFailure(ErrorText(*response));
        return;
    }
    json::JsonObject root{nullptr};
    Forecast parsed;
    bool ok = false;
    try {
        ok = ParseBody(response->body, root) && ParseForecast(root, parsed);
    } catch (...) {
        ok = false;
    }
    if (!ok) {
        OnFailure(L"unexpected response");
        return;
    }
    parsed.fetchedAt = UnixNow();
    data_ = std::move(parsed);
    status_ = Status::Fresh;
    error_.clear();
    failures_ = 0;
    SaveCache(response->body);
    Schedule(static_cast<UINT>(cfg_.refreshMinutes) * kMinuteMs);
    Notify();
}

void WeatherService::OnFailure(std::wstring error) {
    error_ = std::move(error);
    ++failures_;
    status_ = data_.valid ? Status::Stale : Status::Error;
    // 1, 2, 4 ... minutes, never longer than the refresh interval.
    const int shift = std::min(failures_ - 1, 9);
    const UINT minutes = std::min<UINT>(1u << shift, static_cast<UINT>(cfg_.refreshMinutes));
    Schedule(minutes * kMinuteMs);
    wchar_t buf[256];
    swprintf_s(buf, L"WinDock: weather fetch failed (%s); retry in %u min\n", error_.c_str(), minutes);
    OutputDebugStringW(buf);
    Notify();
}

std::wstring WeatherService::ErrorText(const Response& r) {
    switch (r.error) {
        case 0:
            break;
        case ERROR_WINHTTP_TIMEOUT:
            return L"the request timed out";
        case ERROR_WINHTTP_NAME_NOT_RESOLVED:
            return L"no network (name not resolved)";
        case ERROR_WINHTTP_CANNOT_CONNECT:
        case ERROR_WINHTTP_CONNECTION_ERROR:
            return L"cannot connect";
        case ERROR_WINHTTP_SECURE_FAILURE:
            return L"secure connection failed";
        case ERROR_FILE_TOO_LARGE:
            return L"response too large";
        default: {
            wchar_t buf[48];
            swprintf_s(buf, r.error > 0xFFFF ? L"network error 0x%08lX" : L"network error %lu", r.error);
            return buf;
        }
    }
    std::wstring text = L"HTTP " + std::to_wstring(r.status);
    json::JsonObject root{nullptr};
    if (r.status != 200 && ParseBody(r.body, root)) {
        std::wstring reason;
        ReadString(root, L"reason", reason);
        if (reason.size() > 120) reason.resize(120);
        if (!reason.empty()) text += L": " + reason;
    }
    return text;
}

int64_t WeatherService::Age() const {
    if (!data_.valid || data_.fetchedAt <= 0) return -1;
    return std::max<int64_t>(0, UnixNow() - data_.fetchedAt);
}

std::wstring WeatherService::FormatTime(int64_t unixSeconds) {
    const ULONGLONG ticks = (static_cast<ULONGLONG>(unixSeconds) + 11644473600ull) * 10000000ull;
    FILETIME ft{static_cast<DWORD>(ticks & 0xFFFFFFFFu), static_cast<DWORD>(ticks >> 32)};
    SYSTEMTIME utc{}, local{};
    if (!FileTimeToSystemTime(&ft, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return {};
    wchar_t buf[64]{};
    if (!GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &local, nullptr, buf, 64)) return {};
    return buf;
}

std::wstring WeatherService::StatusText() const {
    if (!cfg_.Configured()) return L"Not configured";
    if (!shown_) return L"Not shown on the dock";
    std::wstring s;
    if (data_.valid) {
        s = L"Updated " + FormatTime(data_.fetchedAt);
        if (status_ == Status::Stale) s += L" (stale)";
        if (Busy()) s += L"; updating…";
        else if (!error_.empty()) s += L"; error: " + error_;
        return s;
    }
    if (Busy()) return L"Updating…";
    if (!error_.empty()) return L"Error: " + error_;
    return L"Waiting for the first update";
}

std::vector<WeatherService::Hour> WeatherService::HourlyWindow(size_t n) const {
    std::vector<Hour> out;
    if (!data_.valid || data_.hourly.empty()) return out;
    const int64_t now = (UnixNow() + data_.utcOffset) / 60;  // location-local minutes
    size_t first = data_.hourly.size();
    for (size_t i = 0; i < data_.hourly.size(); ++i) {
        if (data_.hourly[i].key + 60 > now) {  // the hour containing `now`, or the next one
            first = i;
            break;
        }
    }
    for (size_t i = first; i < data_.hourly.size() && out.size() < n; ++i) out.push_back(data_.hourly[i]);
    return out;
}

std::vector<WeatherService::Day> WeatherService::DailyWindow(size_t n) const {
    std::vector<Day> out;
    if (!data_.valid) return out;
    const int64_t today = ((UnixNow() + data_.utcOffset) / 60) / 1440;
    for (const Day& d : data_.daily) {
        if (d.day >= today && out.size() < n) out.push_back(d);
    }
    return out;
}

bool WeatherService::LoadCache() {
    json::JsonObject root{nullptr};
    if (!ReadObjectFile(CachePath(), root)) return false;
    try {
        double fetchedAt = 0.0, lat = kNaN, lon = kNaN;
        std::wstring units;
        ReadNumber(root, L"fetchedAt", fetchedAt);
        ReadNumber(root, L"latitude", lat);
        ReadNumber(root, L"longitude", lon);
        ReadString(root, L"units", units);
        Units u = Units::Metric;
        if (!(fetchedAt > 0.0) || !(std::fabs(lat - cfg_.latitude) < 1e-5) || !(std::fabs(lon - cfg_.longitude) < 1e-5) ||
            !FromString(units, u) || u != cfg_.units) {
            return false;
        }
        const json::JsonObject body = ReadObject(root, L"body");
        Forecast f;
        if (!body || !ParseForecast(body, f)) return false;
        f.fetchedAt = static_cast<int64_t>(fetchedAt);
        data_ = std::move(f);
        return true;
    } catch (...) {
        return false;
    }
}

void WeatherService::SaveCache(const std::string& body) const {
    wchar_t head[256];
    swprintf_s(head, L"{\"fetchedAt\": %lld, \"latitude\": %.6f, \"longitude\": %.6f, \"units\": \"%s\",\n\"body\": ",
               static_cast<long long>(data_.fetchedAt), cfg_.latitude, cfg_.longitude, ToString(cfg_.units));
    CreateDirectoryW(Config::Directory().c_str(), nullptr);
    WriteFileAtomic(CachePath(), WideToUtf8(head) + body + "}\n");  // the body parsed as an object
}

bool WeatherService::Geocode(const std::wstring& query, HWND hwnd, UINT msg, WPARAM tag) {
    const std::wstring path = L"/v1/search?name=" + UrlEncode(query) + L"&count=10&language=en&format=json";
    return StartFetch(hwnd, msg, tag, kGeocodeHost, path);
}

bool WeatherService::ParsePlaces(const Response& response, std::vector<Place>& places, std::wstring& error) {
    places.clear();
    if (response.error || response.status != 200) {
        error = ErrorText(response);
        return false;
    }
    json::JsonObject root{nullptr};
    if (!ParseBody(response.body, root)) {
        error = L"unexpected response";
        return false;
    }
    try {
        const json::JsonArray results = ReadArray(root, L"results");  // missing: nothing found
        if (!results) return true;
        for (const auto& item : results) {
            if (item.ValueType() != json::JsonValueType::Object) continue;
            const json::JsonObject o = item.GetObject();
            Place p;
            p.latitude = kNaN;
            p.longitude = kNaN;
            ReadString(o, L"name", p.name);
            ReadString(o, L"admin1", p.admin1);
            ReadString(o, L"country", p.country);
            ReadNumber(o, L"latitude", p.latitude);
            ReadNumber(o, L"longitude", p.longitude);
            if (p.name.empty() || !(p.latitude >= -90.0 && p.latitude <= 90.0) ||
                !(p.longitude >= -180.0 && p.longitude <= 180.0)) {
                continue;
            }
            places.push_back(std::move(p));
        }
    } catch (...) {
        error = L"unexpected response";
        return false;
    }
    return true;
}
