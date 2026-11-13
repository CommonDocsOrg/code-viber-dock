#include "WidgetSamples.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "Config.h"
#include "JsonUtil.h"
#include "WeatherGlyphs.h"

namespace WidgetSamples {

namespace {

namespace json = winrt::Windows::Data::Json;

constexpr double kPi = 3.14159265358979323846;
constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;

std::string Q(const std::wstring& s) { return JsonUtil::WideToUtf8(JsonUtil::Quote(s)); }

std::string N(double v) {
    if (!std::isfinite(v)) return "null";
    char buf[40];
    if (std::fabs(v) < 9.0e15 && v == std::floor(v)) {
        std::snprintf(buf, sizeof buf, "%.0f", v);
    } else {
        std::snprintf(buf, sizeof buf, "%.15g", v);
    }
    return buf;
}

const char* B(bool b) { return b ? "true" : "false"; }

// --- time ------------------------------------------------------------------------------------------

// Seconds since 1970 (UTC) of a FILETIME.
int64_t Unix(const FILETIME& ft) {
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<int64_t>((u.QuadPart - 116444736000000000ull) / 10000000ull);
}

int64_t NowMs() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<int64_t>((u.QuadPart - 116444736000000000ull) / 10000ull);
}

// Days since 1970-01-01 of a civil date (H. Hinnant's days_from_civil) and back.
int64_t DaysFromCivil(int y, int m, int d) {
    y -= m <= 2 ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void CivilFromDays(int64_t days, int& y, int& m, int& d) {
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const int64_t doe = days - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    y = static_cast<int>(yoe + era * 400 + (m <= 2 ? 1 : 0));
}

std::string DayText(int64_t days) {
    int y = 0, m = 0, d = 0;
    CivilFromDays(days, y, m, d);
    char buf[24];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", y, m, d);
    return buf;
}

int Weekday(int64_t days) { return static_cast<int>(((days % 7) + 11) % 7); }  // 1970-01-01 was a Thursday (4)

int64_t TodayLocal() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    return DaysFromCivil(st.wYear, st.wMonth, st.wDay);
}

// UTC seconds of local day `days` at hh:mm (Windows' own rules for the local zone).
int64_t LocalToUtc(int64_t days, int hour, int minute) {
    int y = 0, m = 0, d = 0;
    CivilFromDays(days, y, m, d);
    SYSTEMTIME local{};
    local.wYear = static_cast<WORD>(y);
    local.wMonth = static_cast<WORD>(m);
    local.wDay = static_cast<WORD>(d);
    local.wHour = static_cast<WORD>(hour);
    local.wMinute = static_cast<WORD>(minute);
    SYSTEMTIME utc{};
    FILETIME ft{};
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc) || !SystemTimeToFileTime(&utc, &ft)) {
        return (days * 86400) + hour * 3600 + minute * 60;
    }
    return Unix(ft);
}

// The locale's short time of a local hh:mm ("9:30 AM").
std::wstring ShortTime(int hour, int minute) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    st.wHour = static_cast<WORD>(hour);
    st.wMinute = static_cast<WORD>(minute);
    st.wSecond = 0;
    wchar_t buf[64]{};
    if (!GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, buf, static_cast<int>(std::size(buf)))) {
        std::swprintf(buf, std::size(buf), L"%02d:%02d", hour, minute);
    }
    return buf;
}

// The local wall-clock time of a UTC instant as days / hour.
void LocalOf(int64_t utcSeconds, int64_t& days, int& hour) {
    ULARGE_INTEGER u{};
    u.QuadPart = static_cast<uint64_t>(utcSeconds) * 10000000ull + 116444736000000000ull;
    FILETIME ft{u.LowPart, u.HighPart};
    SYSTEMTIME utc{}, local{};
    FileTimeToSystemTime(&ft, &utc);
    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
    days = DaysFromCivil(local.wYear, local.wMonth, local.wDay);
    hour = local.wHour;
}

// --- calendar --------------------------------------------------------------------------------------

struct SampleEvent {
    const wchar_t* title;
    const wchar_t* location;
    int h0, m0, h1, m1;
};
constexpr SampleEvent kEvents[] = {
    {L"Stand-up", L"Team room", 9, 30, 9, 45},
    {L"Lunch with Sam", L"Corner Café", 12, 30, 13, 30},
};

std::string ColorText(uint32_t argb) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "#%08X", argb);
    return buf;
}

std::string EventJson(const SampleEvent& e, int64_t day, size_t index) {
    const int64_t start = LocalToUtc(day, e.h0, e.m0), end = LocalToUtc(day, e.h1, e.m1);
    const std::string id = "sample/" + std::to_string(index) + "/" + std::to_string(start);
    return "{\"id\":\"" + id + "\",\"feed\":\"Sample\",\"color\":\"" + ColorText(DefaultFeedColor(0)) +
           "\",\"title\":" + Q(e.title) + ",\"location\":" + Q(e.location) + ",\"description\":\"\",\"start\":" +
           std::to_string(start * 1000) + ",\"end\":" + std::to_string(end * 1000) +
           ",\"allDay\":false,\"url\":\"\",\"recurring\":true,\"startDay\":\"" + DayText(day) + "\",\"endDay\":\"" +
           DayText(day) + "\",\"startTime\":" + Q(ShortTime(e.h0, e.m0)) + ",\"endTime\":" + Q(ShortTime(e.h1, e.m1)) +
           ",\"reminder\":-1}";
}

// A date argument ("YYYY-MM-DD" or ms since 1970) -> its local day. False when absent / malformed.
bool DayArg(const json::JsonObject& o, const wchar_t* key, int64_t& day, int64_t& utcMs, bool& present) {
    present = false;
    const json::IJsonValue v = JsonUtil::Find(o, key);
    if (!v || v.ValueType() == json::JsonValueType::Null) return true;
    present = true;
    if (v.ValueType() == json::JsonValueType::Number) {
        const double ms = v.GetNumber();
        if (!std::isfinite(ms) || std::fabs(ms) > 8.0e15) return false;
        utcMs = static_cast<int64_t>(ms);
        int hour = 0;
        LocalOf(utcMs / 1000, day, hour);
        return true;
    }
    if (v.ValueType() != json::JsonValueType::String) return false;
    const std::wstring s(v.GetString());
    int y = 0, m = 0, d = 0;
    if (s.size() != 10 || swscanf_s(s.c_str(), L"%4d-%2d-%2d", &y, &m, &d) != 3 || m < 1 || m > 12 || d < 1 || d > 31) {
        return false;
    }
    day = DaysFromCivil(y, m, d);
    utcMs = LocalToUtc(day, 0, 0) * 1000;
    return true;
}

// --- perf ------------------------------------------------------------------------------------------

struct SampleMetric {
    const char* key;
    const wchar_t* name;
    const wchar_t* unit;
    const char* kind;
    const wchar_t* group;
    double min, max, value;
};

const std::vector<SampleMetric>& Metrics() {
    static const std::vector<SampleMetric> list = [] {
        const double total = 16.0 * kGiB, used = std::round(9.3 * kGiB);
        std::vector<SampleMetric> m = {
            {"cpu.total", L"CPU total", L"%", "percent", L"CPU", 0, 100, 37},
            {"cpu.frequency", L"CPU frequency", L"MHz", "clock", L"CPU", 0, 6000, 4180},
            {"cpu.processes", L"Processes", L"", "number", L"CPU", 0, 500, 312},
            {"cpu.threads", L"Threads", L"", "number", L"CPU", 0, 6000, 4096},
        };
        static const double cores[] = {41, 33, 52, 28, 39, 30, 45, 27};
        static const char* const coreKeys[] = {"cpu.core.0", "cpu.core.1", "cpu.core.2", "cpu.core.3",
                                               "cpu.core.4", "cpu.core.5", "cpu.core.6", "cpu.core.7"};
        static const wchar_t* const coreNames[] = {L"CPU core 0", L"CPU core 1", L"CPU core 2", L"CPU core 3",
                                                   L"CPU core 4", L"CPU core 5", L"CPU core 6", L"CPU core 7"};
        for (size_t i = 0; i < std::size(cores); ++i) m.push_back({coreKeys[i], coreNames[i], L"%", "percent", L"CPU", 0, 100, cores[i]});
        const std::vector<SampleMetric> rest = {
            {"memory.used", L"Memory used", L"B", "bytes", L"Memory", 0, total, used},
            {"memory.available", L"Memory available", L"B", "bytes", L"Memory", 0, total, total - used},
            {"memory.total", L"Memory total", L"B", "bytes", L"Memory", 0, total, total},
            {"memory.percent", L"Memory used (percent)", L"%", "percent", L"Memory", 0, 100, 58},
            {"memory.committed", L"Memory committed", L"B", "bytes", L"Memory", 0, 24.0 * kGiB, std::round(11.2 * kGiB)},
            {"memory.commitLimit", L"Commit limit", L"B", "bytes", L"Memory", 0, 24.0 * kGiB, 24.0 * kGiB},
            {"gpu.total", L"GPU (3D)", L"%", "percent", L"GPU", 0, 100, 12},
            {"gpu.max", L"GPU (busiest engine)", L"%", "percent", L"GPU", 0, 100, 15},
            {"gpu.engine.3D", L"GPU 3D engine", L"%", "percent", L"GPU", 0, 100, 12},
            {"gpu.engine.Copy", L"GPU copy engine", L"%", "percent", L"GPU", 0, 100, 2},
            {"gpu.engine.VideoDecode", L"GPU video decode", L"%", "percent", L"GPU", 0, 100, 15},
            {"gpu.memory.used", L"GPU memory used", L"B", "bytes", L"GPU", 0, 8.0 * kGiB, std::round(2.4 * kGiB)},
            {"gpu.memory.total", L"GPU memory total", L"B", "bytes", L"GPU", 0, 8.0 * kGiB, 8.0 * kGiB},
            {"disk.total", L"Disk active time", L"%", "percent", L"Disk", 0, 100, 4},
            {"disk.read", L"Disk read", L"B/s", "bytesPerSecond", L"Disk", 0, 1e9, 1572864},
            {"disk.write", L"Disk write", L"B/s", "bytesPerSecond", L"Disk", 0, 1e9, 655360},
            {"network.down", L"Network download", L"B/s", "bytesPerSecond", L"Network", 0, 125e6, 122880},
            {"network.up", L"Network upload", L"B/s", "bytesPerSecond", L"Network", 0, 125e6, 18432},
            // Round 7 phase 7C: a laptop on battery (the Battery widget's preview).
            {"battery.percent", L"Battery", L"%", "percent", L"Battery", 0, 100, 74},
            {"battery.charging", L"Battery charging", L"", "bool", L"Battery", 0, 1, 0},
            {"battery.acLine", L"On mains power", L"", "bool", L"Battery", 0, 1, 0},
            {"battery.timeLeft", L"Battery time left", L"s", "seconds", L"Battery", 0, 36000, 11400},
            {"system.uptime", L"Uptime", L"s", "seconds", L"System", 0, 604800, 273600},
        };
        m.insert(m.end(), rest.begin(), rest.end());
        return m;
    }();
    return list;
}

const SampleMetric* FindMetric(const std::string& key) {
    for (const SampleMetric& m : Metrics()) {
        if (key == m.key) return &m;
    }
    return nullptr;
}

// A deterministic value in [-1, 1] for (key, i).
double Noise(const std::string& key, size_t i) {
    uint32_t h = 2166136261u;
    for (char c : key) h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
    h = (h ^ static_cast<uint32_t>(i * 2654435761u)) * 16777619u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return static_cast<double>(h % 20001u) / 10000.0 - 1.0;
}

}  // namespace

// --- audio / media / notifications ------------------------------------------------------------------

std::string AudioJson() {
    return "{\"available\":true,\"volume\":0.62,\"muted\":false,\"device\":\"Speakers (Sample)\",\"deviceId\":\"sample:speakers\","
           "\"mic\":true,\"micVolume\":0.8,\"micMuted\":false,\"micName\":\"Microphone (Sample)\",\"devices\":["
           "{\"id\":\"sample:speakers\",\"name\":\"Speakers (Sample)\",\"default\":true},"
           "{\"id\":\"sample:headphones\",\"name\":\"Headphones (Sample)\",\"default\":false}]}";
}

std::string MediaJson() {
    return "{\"available\":true,\"session\":true,\"title\":\"Sample Song\",\"artist\":\"Sample Artist\",\"album\":\"Sample Album\","
           "\"app\":\"Sample.Player\",\"status\":\"playing\",\"hasArt\":true,\"position\":73,\"duration\":214,\"canPlay\":true,"
           "\"canPause\":true,\"canToggle\":true,\"canNext\":true,\"canPrevious\":true,\"canSeek\":true}";
}

const std::string& MediaArtBytes() {
    static const std::string bmp = [] {
        constexpr int W = 64, H = 64;
        constexpr int stride = W * 3;  // 192: a multiple of 4
        constexpr uint32_t pixels = stride * H;
        BITMAPFILEHEADER fh{};
        BITMAPINFOHEADER ih{};
        fh.bfType = 0x4D42;  // "BM"
        fh.bfOffBits = sizeof(fh) + sizeof(ih);
        fh.bfSize = fh.bfOffBits + pixels;
        ih.biSize = sizeof(ih);
        ih.biWidth = W;
        ih.biHeight = H;  // bottom-up
        ih.biPlanes = 1;
        ih.biBitCount = 24;
        ih.biCompression = BI_RGB;
        ih.biSizeImage = pixels;
        std::string out(fh.bfSize, '\0');
        std::memcpy(out.data(), &fh, sizeof(fh));
        std::memcpy(out.data() + sizeof(fh), &ih, sizeof(ih));
        uint8_t* p = reinterpret_cast<uint8_t*>(out.data() + fh.bfOffBits);
        for (int row = 0; row < H; ++row) {
            const int y = H - 1 - row;  // top-down position of this bottom-up row
            for (int x = 0; x < W; ++x) {
                // A diagonal gradient, violet (top-left) to orange (bottom-right), with a soft disc.
                const double t = (x + y) / 126.0;
                double r = 0x6A + (0xFF - 0x6A) * t, g = 0x3C + (0x9A - 0x3C) * t, b = 0xD8 + (0x3C - 0xD8) * t;
                const double dx = x - 40.0, dy = y - 24.0, disc = std::exp(-(dx * dx + dy * dy) / 260.0) * 0.35;
                r += (255 - r) * disc;
                g += (255 - g) * disc;
                b += (255 - b) * disc;
                uint8_t* px = p + row * stride + x * 3;
                px[0] = static_cast<uint8_t>(std::clamp(b, 0.0, 255.0));
                px[1] = static_cast<uint8_t>(std::clamp(g, 0.0, 255.0));
                px[2] = static_cast<uint8_t>(std::clamp(r, 0.0, 255.0));
            }
        }
        return out;
    }();
    return bmp;
}

std::string NotificationsJson() {
    const int64_t now = NowMs();
    struct Item {
        int id;
        const wchar_t* app;
        const char* key;
        const wchar_t* title;
        const wchar_t* body;
        int64_t ago;  // minutes
    };
    static const Item kItems[] = {
        {3, L"Mail", "sample.mail", L"Sam Rivera", L"Lunch at 12:30? I booked the corner table.", 5},
        {2, L"Mail", "sample.mail", L"Weekly report", L"The numbers for this week are in.", 20},
        {1, L"Calendar", "sample.calendar", L"Stand-up", L"9:30 AM, Team room", 60},
    };
    std::string j = "{\"state\":\"polling\",\"reason\":\"\",\"unread\":3,\"items\":[";
    for (size_t i = 0; i < std::size(kItems); ++i) {
        const Item& it = kItems[i];
        if (i) j += ",";
        j += "{\"id\":" + std::to_string(it.id) + ",\"app\":" + Q(it.app) + ",\"appKey\":\"" + it.key + "\",\"aumid\":\"" +
             std::string(it.key) + "\",\"title\":" + Q(it.title) + ",\"body\":" + Q(it.body) +
             ",\"time\":" + std::to_string(now - it.ago * 60000) + ",\"unread\":true,\"balloon\":false}";
    }
    return j + "]}";
}

// --- calendar ------------------------------------------------------------------------------------

bool CalendarCall(const std::string& what, const std::string& args, std::string& out, std::wstring& error) {
    json::JsonObject o{nullptr};
    try {
        if (!json::JsonObject::TryParse(JsonUtil::Utf8ToWide(args), o) || !o) o = json::JsonObject();
    } catch (...) {
        o = json::JsonObject();
    }
    const int64_t today = TodayLocal();
    if (what == "feeds") {
        out = "[{\"name\":\"Sample\",\"color\":\"" + ColorText(DefaultFeedColor(0)) +
              "\",\"enabled\":true,\"file\":false,\"status\":\"ok\",\"error\":\"\",\"lastFetch\":" + std::to_string(NowMs() - 300000) +
              ",\"events\":" + std::to_string(std::size(kEvents)) + "}]";
        return true;
    }
    if (what == "refresh" || what == "openPopup") {
        out = "null";
        return true;
    }
    // The occurrences on `day` overlapping [from, to) (ms; 0 / INT64_MAX: open).
    auto collect = [&](int64_t day, int64_t from, int64_t to, std::vector<std::string>& list, size_t limit) {
        for (size_t i = 0; i < std::size(kEvents) && list.size() < limit; ++i) {
            const int64_t s = LocalToUtc(day, kEvents[i].h0, kEvents[i].m0) * 1000, e = LocalToUtc(day, kEvents[i].h1, kEvents[i].m1) * 1000;
            if (e > from && s < to) list.push_back(EventJson(kEvents[i], day, i));
        }
    };
    auto join = [&](const std::vector<std::string>& list) {
        out = "[";
        for (size_t i = 0; i < list.size(); ++i) out += (i ? "," : "") + list[i];
        out += "]";
    };
    std::vector<std::string> list;
    if (what == "eventsOn") {
        int64_t day = today, ms = 0;
        bool present = false;
        if (!DayArg(o, L"date", day, ms, present)) {
            error = L"calendar.eventsOn: the date must be a Date, ms since 1970 or \"YYYY-MM-DD\"";
            return false;
        }
        if (day == today) collect(today, INT64_MIN, INT64_MAX, list, 100);
        join(list);
        return true;
    }
    if (what == "next") {
        double n = 5.0;
        JsonUtil::ReadNumber(o, L"n", n);
        const size_t count = static_cast<size_t>(std::clamp(std::isfinite(n) ? n : 5.0, 1.0, 100.0));
        const int64_t now = NowMs();
        collect(today, now, INT64_MAX, list, count);
        if (list.empty()) collect(today + 1, now, INT64_MAX, list, count);  // today's are over: tomorrow's copies
        join(list);
        return true;
    }
    if (what == "events") {
        int64_t fromDay = today, toDay = today, from = NowMs(), to = INT64_MAX;
        bool present = false;
        if (!DayArg(o, L"from", fromDay, from, present)) {
            error = L"calendar.events: from must be a Date, ms since 1970 or \"YYYY-MM-DD\"";
            return false;
        }
        if (!present) from = NowMs();
        if (!DayArg(o, L"to", toDay, to, present)) {
            error = L"calendar.events: to must be a Date, ms since 1970 or \"YYYY-MM-DD\"";
            return false;
        }
        if (!present) to = INT64_MAX;
        collect(today, from, to, list, 2000);
        collect(today + 1, from, to, list, 2000);
        join(list);
        return true;
    }
    error = L"calendar: unknown call";
    return false;
}

// --- weather -----------------------------------------------------------------------------------------

std::string WeatherJson(bool imperial, const std::wstring& label) {
    auto temp = [&](double c) { return imperial ? std::round(c * 9.0 / 5.0 + 32.0) : c; };
    const int64_t now = NowMs();
    const int64_t fetched = now - 300000;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::wstring updated;
    {
        FILETIME ft{};
        ULARGE_INTEGER u{};
        u.QuadPart = static_cast<uint64_t>(fetched) * 10000ull + 116444736000000000ull;
        ft.dwLowDateTime = u.LowPart;
        ft.dwHighDateTime = u.HighPart;
        SYSTEMTIME utc{}, local{};
        FileTimeToSystemTime(&ft, &utc);
        SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
        updated = ShortTime(local.wHour, local.wMinute);
    }
    std::string o = "{\"state\":\"fresh\",\"configured\":true,\"place\":\"Sample City\",\"label\":" + Q(label) +
                    ",\"latitude\":47.61,\"longitude\":-122.33";
    o += std::string(",\"units\":\"") + (imperial ? "imperial" : "metric") + "\"";
    o += std::string(",\"temperatureUnit\":\"") + (imperial ? "F" : "C") + "\"";
    o += std::string(",\"windUnit\":\"") + (imperial ? "mph" : "km/h") + "\"";
    o += ",\"refreshMinutes\":30,\"busy\":false,\"error\":\"\",\"status\":\"Sample forecast (preview)\",\"age\":300";
    o += ",\"fetchedAt\":" + std::to_string(fetched) + ",\"updated\":" + Q(updated);
    o += ",\"current\":{\"temp\":" + N(temp(21)) + ",\"code\":0,\"isDay\":true,\"description\":" + Q(WeatherGlyphs::Describe(0)) +
         ",\"glyph\":\"clear\",\"wind\":" + N(imperial ? 7 : 12) + ",\"humidity\":45}";
    // 24 hours from the current one (local hours), a mild day / night curve.
    o += ",\"hourly\":[";
    const int64_t hourStart = LocalToUtc(TodayLocal(), st.wHour, 0);
    for (int i = 0; i < 24; ++i) {
        const int hour = (st.wHour + i) % 24;
        const double c = std::round((15.0 + 6.5 * std::cos(2.0 * kPi * (hour - 15) / 24.0)) * 10.0) / 10.0;
        const bool day = hour >= 7 && hour < 19;
        const int code = day ? (hour >= 16 && hour < 18 ? 2 : (hour % 5 == 0 ? 1 : 0)) : (hour % 4 == 0 ? 1 : 0);
        const int precip = hour >= 16 && hour < 19 ? 10 : (hour % 3 == 0 ? 5 : 0);
        o += (i ? ",{" : "{") + std::string("\"time\":") + std::to_string((hourStart + i * 3600) * 1000) +
             ",\"hour\":" + std::to_string(hour) + ",\"temp\":" + N(temp(c)) + ",\"code\":" + std::to_string(code) +
             ",\"isDay\":" + B(day) + ",\"precip\":" + std::to_string(precip) + "}";
    }
    // 7 days from today.
    o += "],\"daily\":[";
    static const int kCodes[] = {0, 1, 2, 3, 61, 2, 0};
    static const double kMax[] = {23, 22, 20, 18, 16, 19, 22};
    static const double kMin[] = {12, 13, 12, 11, 10, 11, 12};
    const int64_t today = TodayLocal();
    for (int i = 0; i < 7; ++i) {
        o += (i ? ",{" : "{") + std::string("\"day\":\"") + DayText(today + i) + "\",\"weekday\":" + std::to_string(Weekday(today + i)) +
             ",\"code\":" + std::to_string(kCodes[i]) + ",\"description\":" + Q(WeatherGlyphs::Describe(kCodes[i])) +
             ",\"min\":" + N(temp(kMin[i])) + ",\"max\":" + N(temp(kMax[i])) + "}";
    }
    o += "]}";
    return o;
}

// --- perf ----------------------------------------------------------------------------------------

std::string PerfSnapshotJson() {
    std::string o = "{\"time\":" + std::to_string(NowMs()) + ",\"interval\":1000,\"serial\":1,\"metrics\":{";
    bool first = true;
    for (const SampleMetric& m : Metrics()) {
        o += (first ? "\"" : ",\"") + std::string(m.key) + "\":" + N(m.value);
        first = false;
    }
    return o + "}}";
}

std::string PerfMetricsJson() {
    std::string o = "{\"serial\":1,\"list\":[";
    bool first = true;
    for (const SampleMetric& m : Metrics()) {
        o += (first ? "{" : ",{") + std::string("\"key\":\"") + m.key + "\",\"name\":" + Q(m.name) + ",\"unit\":" + Q(m.unit) +
             ",\"min\":" + N(m.min) + ",\"max\":" + N(m.max) + ",\"kind\":\"" + m.kind + "\",\"group\":" + Q(m.group) +
             ",\"hwinfo\":false}";
        first = false;
    }
    return o + "]}";
}

std::string PerfHwinfoJson() {
    return "{\"available\":false,\"reason\":\"preview\",\"version\":\"\",\"pollTime\":0,\"sensors\":[],\"readingCount\":0}";
}

void PerfSeries(const std::string& key, size_t n, std::vector<double>& out) {
    out.clear();
    const SampleMetric* m = FindMetric(key);
    if (!m) return;
    constexpr size_t kSamples = 60;
    n = std::min(n, kSamples);
    const double v = m->value;
    // Percentages swing by up to 12 points; other kinds by a quarter of the value.
    const double amp = std::string(m->kind) == "percent" ? std::min(12.0, std::max(2.0, v * 0.4)) : v * 0.25;
    double phase = 0.0;
    for (char c : key) phase += static_cast<uint8_t>(c) * 0.37;
    for (size_t k = 0; k < n; ++k) {
        const size_t i = kSamples - n + k;  // the newest sample is i = 59
        double x = v;
        if (i + 1 < kSamples) {
            const double t = static_cast<double>(kSamples - 1 - i);
            x = v + amp * std::sin(2.0 * kPi * t / 23.0 + phase) * (0.6 + 0.4 * std::cos(t / 9.0)) + amp * 0.3 * Noise(key, i);
        }
        out.push_back(std::clamp(x, m->min, m->max));
    }
}

}  // namespace WidgetSamples
