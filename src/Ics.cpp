#include "Ics.h"

#include <algorithm>
#include <climits>
#include <cwctype>
#include <functional>
#include <set>

namespace Ics {

// --- dates ---------------------------------------------------------------------------------------

// H. Hinnant's days_from_civil / civil_from_days.
int64_t DaysFromCivil(int y, int m, int d) {
    y -= m <= 2 ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void CivilFromDays(int64_t z, int& y, int& m, int& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    y = static_cast<int>(yoe + era * 400 + (m <= 2 ? 1 : 0));
}

int DaysInMonth(int y, int m) {
    static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return kDays[(m - 1 + 12) % 12];
}

int WeekdayOf(int64_t days) {  // 1970-01-01 was a Thursday (3)
    return static_cast<int>(((days % 7) + 7 + 3) % 7);
}

std::wstring FormatDay(int64_t day) {
    int y = 0, m = 0, d = 0;
    CivilFromDays(day, y, m, d);
    wchar_t buf[16];
    swprintf_s(buf, L"%04d%02d%02d", y, m, d);
    return buf;
}

std::wstring FormatUtc(int64_t utc) {
    const int64_t day = DayOf(utc);
    const int64_t s = utc - day * 86400;
    wchar_t buf[32];
    swprintf_s(buf, L"%sT%02d%02d%02dZ", FormatDay(day).c_str(), static_cast<int>(s / 3600), static_cast<int>(s / 60 % 60),
               static_cast<int>(s % 60));
    return buf;
}

namespace {

constexpr int64_t kEpochToFileTime = 11644473600LL;  // seconds 1601-01-01 -> 1970-01-01
constexpr int64_t kMinSecs = -kEpochToFileTime + 2 * 86400;
constexpr int64_t kMaxSecs = 253402214400LL - 2 * 86400;  // ~9999-12-29

bool ToSystemTime(int64_t secs, SYSTEMTIME& st) {
    secs = std::clamp(secs, kMinSecs, kMaxSecs);
    const ULONGLONG ticks = static_cast<ULONGLONG>(secs + kEpochToFileTime) * 10000000ULL;
    FILETIME ft{static_cast<DWORD>(ticks & 0xFFFFFFFFu), static_cast<DWORD>(ticks >> 32)};
    return FileTimeToSystemTime(&ft, &st) != FALSE;
}

int64_t FromSystemTime(const SYSTEMTIME& st) {
    FILETIME ft{};
    if (!SystemTimeToFileTime(&st, &ft)) return 0;
    const ULONGLONG ticks = (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return static_cast<int64_t>(ticks / 10000000ULL) - kEpochToFileTime;
}

// --- zones ---------------------------------------------------------------------------------------

class FixedZone final : public Zone {
public:
    explicit FixedZone(int offset) : offset_(offset) {}
    int64_t ToUtc(int64_t wall) const override { return wall - offset_; }
    int64_t ToWall(int64_t utc) const override { return utc + offset_; }

private:
    int offset_;
};

// A Windows time zone (its dynamic DST rules, from the registry).
class WinZone final : public Zone {
public:
    explicit WinZone(const DYNAMIC_TIME_ZONE_INFORMATION& tz) : tz_(tz) {}
    int64_t ToUtc(int64_t wall) const override {
        SYSTEMTIME l{}, u{};
        if (!ToSystemTime(wall, l) || !TzSpecificLocalTimeToSystemTimeEx(&tz_, &l, &u)) return wall;
        return FromSystemTime(u) + (wall - std::clamp(wall, kMinSecs, kMaxSecs));
    }
    int64_t ToWall(int64_t utc) const override {
        SYSTEMTIME u{}, l{};
        if (!ToSystemTime(utc, u) || !SystemTimeToTzSpecificLocalTimeEx(&tz_, &u, &l)) return utc;
        return FromSystemTime(l) + (utc - std::clamp(utc, kMinSecs, kMaxSecs));
    }

private:
    DYNAMIC_TIME_ZONE_INFORMATION tz_;
};

// One STANDARD / DAYLIGHT observance of a VTIMEZONE.
struct Observance {
    int64_t start = 0;  // wall-clock, in the offset before it (TZOFFSETFROM)
    int from = 0, to = 0;
    bool hasRule = false;
    Rule rule;
    std::vector<int64_t> rdates;  // wall-clock
};

template <class F>
void ForEachInstance(const Rule& r, int64_t dtstart, int64_t winFrom, int64_t winTo, int64_t untilWall, F&& emit);

// A VTIMEZONE: the offset in effect at a UTC instant is the TZOFFSETTO of the latest onset at or
// before it (onsets in UTC = the observance's wall onset - its TZOFFSETFROM), cached per year.
class VZone final : public Zone {
public:
    explicit VZone(std::vector<Observance> obs) : obs_(std::move(obs)) {
        for (const Observance& o : obs_) {
            for (int off : {o.to, o.from}) {
                if (std::find(offsets_.begin(), offsets_.end(), off) == offsets_.end()) offsets_.push_back(off);
            }
        }
        std::sort(offsets_.begin(), offsets_.end(), std::greater<int>());
        if (!obs_.empty()) {
            const Observance* first = &obs_.front();
            for (const Observance& o : obs_) {
                if (o.start < first->start) first = &o;
            }
            before_ = first->from;
        }
    }

    int64_t ToWall(int64_t utc) const override { return utc + OffsetAt(utc); }
    int64_t ToUtc(int64_t wall) const override {
        if (obs_.empty()) return wall;
        for (int off : offsets_) {  // the largest offset first: the earlier of two ambiguous instants
            const int64_t u = wall - off;
            if (OffsetAt(u) == off) return u;
        }
        // In a gap (spring forward): the offset before it.
        return wall - OffsetAt(wall - offsets_.front());
    }

private:
    int OffsetAt(int64_t utc) const {
        int y = 0, m = 0, d = 0;
        CivilFromDays(DayOf(utc), y, m, d);
        int64_t best = INT64_MIN;
        int offset = before_;
        auto consider = [&](int64_t at, int to) {
            if (at <= utc && at > best) {
                best = at;
                offset = to;
            }
        };
        for (size_t i = 0; i < obs_.size(); ++i) {
            const Observance& o = obs_[i];
            consider(o.start - o.from, o.to);
            for (int64_t r : o.rdates) consider(r - o.from, o.to);
            if (!o.hasRule) continue;
            // The latest rule onset at or before `utc`: this year, else the years before (back to
            // the observance's start: a rule that ended long ago still decides the offset).
            int startYear = 0;
            CivilFromDays(DayOf(o.start), startYear, m, d);
            for (int year = y; year >= startYear && year >= y - 400; --year) {
                const std::vector<int64_t>& on = Onsets(i, year);
                auto it = std::upper_bound(on.begin(), on.end(), utc);
                if (it != on.begin()) {
                    consider(*(it - 1), o.to);
                    break;
                }
            }
        }
        return offset;
    }

    // The onsets (UTC, sorted) of observance i in `year`.
    const std::vector<int64_t>& Onsets(size_t i, int year) const {
        const auto key = std::make_pair(i, year);
        auto it = cache_.find(key);
        if (it != cache_.end()) return it->second;
        const Observance& o = obs_[i];
        std::vector<int64_t> on;
        const int64_t lo = DaysFromCivil(year, 1, 1) * 86400, hi = DaysFromCivil(year + 1, 1, 1) * 86400;
        if (o.start >= lo && o.start < hi) on.push_back(o.start - o.from);
        for (int64_t r : o.rdates) {
            if (r >= lo && r < hi) on.push_back(r - o.from);
        }
        if (o.hasRule) {
            int64_t until = INT64_MAX;
            if (o.rule.until.Valid()) {
                // UNTIL is UTC in a VTIMEZONE (RFC 5545 3.6.5); a floating / date one is wall-clock.
                until = o.rule.until.kind == Time::Kind::Utc ? o.rule.until.wall + o.from
                        : o.rule.until.IsDate()             ? o.rule.until.wall + 86399
                                                             : o.rule.until.wall;
            }
            ForEachInstance(o.rule, o.start, lo, hi, until, [&](int64_t wall) {
                on.push_back(wall - o.from);
                return true;
            });
        }
        std::sort(on.begin(), on.end());
        if (cache_.size() > 4096) cache_.clear();
        return cache_.emplace(key, std::move(on)).first->second;
    }

    std::vector<Observance> obs_;
    std::vector<int> offsets_;
    int before_ = 0;
    mutable std::map<std::pair<size_t, int>, std::vector<int64_t>> cache_;
};

SRWLOCK g_zoneLock = SRWLOCK_INIT;

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::wstring Upper(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towupper(c));
    return s;
}

// Common IANA zones -> Windows keys (CLDR windowsZones, the "001" territory entries plus the
// usual aliases). ICU covers the rest when icu.dll is there.
struct IanaEntry {
    const wchar_t* iana;
    const wchar_t* windows;
};
constexpr IanaEntry kIana[] = {
    {L"america/new_york", L"Eastern Standard Time"},
    {L"america/detroit", L"Eastern Standard Time"},
    {L"america/toronto", L"Eastern Standard Time"},
    {L"us/eastern", L"Eastern Standard Time"},
    {L"america/indiana/indianapolis", L"US Eastern Standard Time"},
    {L"america/chicago", L"Central Standard Time"},
    {L"america/winnipeg", L"Central Standard Time"},
    {L"us/central", L"Central Standard Time"},
    {L"america/mexico_city", L"Central Standard Time (Mexico)"},
    {L"america/denver", L"Mountain Standard Time"},
    {L"america/edmonton", L"Mountain Standard Time"},
    {L"us/mountain", L"Mountain Standard Time"},
    {L"america/phoenix", L"US Mountain Standard Time"},
    {L"america/los_angeles", L"Pacific Standard Time"},
    {L"america/vancouver", L"Pacific Standard Time"},
    {L"america/tijuana", L"Pacific Standard Time (Mexico)"},
    {L"us/pacific", L"Pacific Standard Time"},
    {L"america/anchorage", L"Alaskan Standard Time"},
    {L"pacific/honolulu", L"Hawaiian Standard Time"},
    {L"america/halifax", L"Atlantic Standard Time"},
    {L"america/st_johns", L"Newfoundland Standard Time"},
    {L"america/sao_paulo", L"E. South America Standard Time"},
    {L"america/argentina/buenos_aires", L"Argentina Standard Time"},
    {L"america/buenos_aires", L"Argentina Standard Time"},
    {L"america/bogota", L"SA Pacific Standard Time"},
    {L"america/lima", L"SA Pacific Standard Time"},
    {L"america/santiago", L"Pacific SA Standard Time"},
    {L"america/caracas", L"Venezuela Standard Time"},
    {L"europe/london", L"GMT Standard Time"},
    {L"europe/dublin", L"GMT Standard Time"},
    {L"europe/lisbon", L"GMT Standard Time"},
    {L"atlantic/reykjavik", L"Greenwich Standard Time"},
    {L"europe/berlin", L"W. Europe Standard Time"},
    {L"europe/amsterdam", L"W. Europe Standard Time"},
    {L"europe/rome", L"W. Europe Standard Time"},
    {L"europe/stockholm", L"W. Europe Standard Time"},
    {L"europe/vienna", L"W. Europe Standard Time"},
    {L"europe/zurich", L"W. Europe Standard Time"},
    {L"europe/oslo", L"W. Europe Standard Time"},
    {L"europe/paris", L"Romance Standard Time"},
    {L"europe/brussels", L"Romance Standard Time"},
    {L"europe/madrid", L"Romance Standard Time"},
    {L"europe/copenhagen", L"Romance Standard Time"},
    {L"europe/prague", L"Central Europe Standard Time"},
    {L"europe/budapest", L"Central Europe Standard Time"},
    {L"europe/belgrade", L"Central Europe Standard Time"},
    {L"europe/warsaw", L"Central European Standard Time"},
    {L"europe/zagreb", L"Central European Standard Time"},
    {L"europe/athens", L"GTB Standard Time"},
    {L"europe/bucharest", L"GTB Standard Time"},
    {L"europe/helsinki", L"FLE Standard Time"},
    {L"europe/kiev", L"FLE Standard Time"},
    {L"europe/kyiv", L"FLE Standard Time"},
    {L"europe/riga", L"FLE Standard Time"},
    {L"europe/tallinn", L"FLE Standard Time"},
    {L"europe/vilnius", L"FLE Standard Time"},
    {L"europe/sofia", L"FLE Standard Time"},
    {L"europe/istanbul", L"Turkey Standard Time"},
    {L"europe/moscow", L"Russian Standard Time"},
    {L"africa/cairo", L"Egypt Standard Time"},
    {L"africa/johannesburg", L"South Africa Standard Time"},
    {L"africa/lagos", L"W. Central Africa Standard Time"},
    {L"africa/nairobi", L"E. Africa Standard Time"},
    {L"asia/dubai", L"Arabian Standard Time"},
    {L"asia/karachi", L"Pakistan Standard Time"},
    {L"asia/kolkata", L"India Standard Time"},
    {L"asia/calcutta", L"India Standard Time"},
    {L"asia/dhaka", L"Bangladesh Standard Time"},
    {L"asia/bangkok", L"SE Asia Standard Time"},
    {L"asia/jakarta", L"SE Asia Standard Time"},
    {L"asia/shanghai", L"China Standard Time"},
    {L"asia/hong_kong", L"China Standard Time"},
    {L"asia/singapore", L"Singapore Standard Time"},
    {L"asia/kuala_lumpur", L"Singapore Standard Time"},
    {L"asia/taipei", L"Taipei Standard Time"},
    {L"asia/tokyo", L"Tokyo Standard Time"},
    {L"asia/seoul", L"Korea Standard Time"},
    {L"asia/jerusalem", L"Israel Standard Time"},
    {L"asia/tehran", L"Iran Standard Time"},
    {L"australia/sydney", L"AUS Eastern Standard Time"},
    {L"australia/melbourne", L"AUS Eastern Standard Time"},
    {L"australia/canberra", L"AUS Eastern Standard Time"},
    {L"australia/brisbane", L"E. Australia Standard Time"},
    {L"australia/adelaide", L"Cen. Australia Standard Time"},
    {L"australia/perth", L"W. Australia Standard Time"},
    {L"pacific/auckland", L"New Zealand Standard Time"},
};

// icu.dll (Windows 10 1903+): ucal_getWindowsTimeZoneID. Loaded once, never freed.
using IcuWindowsId = int32_t(__cdecl*)(const wchar_t* id, int32_t len, wchar_t* winid, int32_t capacity, int* status);
IcuWindowsId IcuFunction() {
    static IcuWindowsId fn = [] {
        HMODULE icu = LoadLibraryExW(L"icu.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return icu ? reinterpret_cast<IcuWindowsId>(GetProcAddress(icu, "ucal_getWindowsTimeZoneID")) : nullptr;
    }();
    return fn;
}

bool IsUtcName(const std::wstring& lower) {
    return lower == L"utc" || lower == L"gmt" || lower == L"z" || lower == L"etc/utc" || lower == L"etc/gmt" ||
           lower == L"etc/zulu" || lower == L"zulu" || lower == L"universal" || lower == L"etc/universal";
}

}  // namespace

std::shared_ptr<const Zone> UtcZone() {
    static const std::shared_ptr<const Zone> utc = std::make_shared<FixedZone>(0);
    return utc;
}

std::shared_ptr<const Zone> LocalZone() {
    DYNAMIC_TIME_ZONE_INFORMATION tz{};
    if (GetDynamicTimeZoneInformation(&tz) == TIME_ZONE_ID_INVALID) return UtcZone();
    return std::make_shared<WinZone>(tz);
}

std::shared_ptr<const Zone> WindowsZone(const std::wstring& key) {
    static std::map<std::wstring, std::shared_ptr<const Zone>>* cache = new std::map<std::wstring, std::shared_ptr<const Zone>>;
    const std::wstring lower = Lower(key);
    if (lower.empty()) return nullptr;
    AcquireSRWLockExclusive(&g_zoneLock);
    auto it = cache->find(lower);
    if (it != cache->end()) {
        auto z = it->second;
        ReleaseSRWLockExclusive(&g_zoneLock);
        return z;
    }
    std::shared_ptr<const Zone> found;
    DYNAMIC_TIME_ZONE_INFORMATION tz{};
    for (DWORD i = 0; EnumDynamicTimeZoneInformation(i, &tz) == ERROR_SUCCESS; ++i) {
        if (_wcsicmp(tz.TimeZoneKeyName, key.c_str()) == 0) {
            found = std::make_shared<WinZone>(tz);
            break;
        }
    }
    if (cache->size() < 512) (*cache)[lower] = found;
    ReleaseSRWLockExclusive(&g_zoneLock);
    return found;
}

std::wstring WindowsKeyForIana(const std::wstring& iana) {
    const std::wstring lower = Lower(iana);
    for (const IanaEntry& e : kIana) {
        if (lower == e.iana) return e.windows;
    }
    if (IcuWindowsId fn = IcuFunction(); fn && !iana.empty() && iana.size() < 128) {
        wchar_t buf[128]{};
        int status = 0;
        const int32_t n = fn(iana.c_str(), static_cast<int32_t>(iana.size()), buf, 127, &status);
        if (status <= 0 && n > 0 && n < 128) return std::wstring(buf, static_cast<size_t>(n));
    }
    return {};
}

std::shared_ptr<const Zone> ResolveZoneName(const std::wstring& tzid) {
    std::wstring name = tzid;
    while (!name.empty() && (name.front() == L'"' || name.front() == L' ')) name.erase(name.begin());
    while (!name.empty() && (name.back() == L'"' || name.back() == L' ')) name.pop_back();
    if (name.empty()) return nullptr;
    if (IsUtcName(Lower(name))) return UtcZone();
    if (auto z = WindowsZone(name)) return z;
    // "/mozilla.org/20050126_1/Europe/Berlin", "/softwarestudio.org/Olson_20011030_5/America/New_York":
    // try the trailing "Area/City" (and "Area/Sub/City").
    std::vector<std::wstring> candidates{name};
    std::vector<size_t> slashes;
    for (size_t i = 0; i < name.size(); ++i) {
        if (name[i] == L'/') slashes.push_back(i);
    }
    for (size_t k = slashes.size(); k-- > 0;) {
        if (slashes.size() - k >= 2 && slashes.size() - k <= 3) candidates.push_back(name.substr(slashes[k] + 1));
    }
    for (const std::wstring& c : candidates) {
        if (IsUtcName(Lower(c))) return UtcZone();
        const std::wstring key = WindowsKeyForIana(c);
        if (!key.empty()) {
            if (auto z = WindowsZone(key)) return z;
        }
    }
    return nullptr;
}

// --- RRULE ---------------------------------------------------------------------------------------

namespace {

int WeekdayCode(const std::wstring& s) {
    static const wchar_t* const kCodes[] = {L"MO", L"TU", L"WE", L"TH", L"FR", L"SA", L"SU"};
    for (int i = 0; i < 7; ++i) {
        if (s == kCodes[i]) return i;
    }
    return -1;
}

std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out;
    size_t at = 0;
    for (;;) {
        const size_t next = s.find(sep, at);
        out.push_back(s.substr(at, next == std::wstring::npos ? std::wstring::npos : next - at));
        if (next == std::wstring::npos) break;
        at = next + 1;
    }
    return out;
}

bool ToInt(const std::wstring& s, int& v) {
    if (s.empty() || s.size() > 9) return false;
    size_t i = 0;
    bool neg = false;
    if (s[0] == L'+' || s[0] == L'-') {
        neg = s[0] == L'-';
        i = 1;
    }
    if (i >= s.size()) return false;
    int n = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < L'0' || s[i] > L'9') return false;
        n = n * 10 + (s[i] - L'0');
    }
    v = neg ? -n : n;
    return true;
}

bool ParseTimeValue(const std::wstring& value, const std::wstring& tzid, bool dateParam, Time& out);

}  // namespace

bool ParseRule(const std::wstring& value, Rule& out) {
    out = Rule{};
    for (const std::wstring& part : Split(Upper(value), L';')) {
        const size_t eq = part.find(L'=');
        if (eq == std::wstring::npos) continue;
        const std::wstring key = part.substr(0, eq), v = part.substr(eq + 1);
        auto ints = [&](std::vector<int>& list, int lo, int hi) {
            for (const std::wstring& x : Split(v, L',')) {
                int n = 0;
                if (ToInt(x, n) && n != 0 && n >= lo && n <= hi) list.push_back(n);
            }
        };
        if (key == L"FREQ") {
            out.freq = v == L"DAILY"     ? Rule::Freq::Daily
                       : v == L"WEEKLY"  ? Rule::Freq::Weekly
                       : v == L"MONTHLY" ? Rule::Freq::Monthly
                       : v == L"YEARLY"  ? Rule::Freq::Yearly
                                         : Rule::Freq::Unsupported;
        } else if (key == L"INTERVAL") {
            int n = 1;
            if (ToInt(v, n) && n >= 1) out.interval = std::min(n, 100000);
        } else if (key == L"COUNT") {
            int n = 0;
            if (ToInt(v, n) && n >= 1) out.count = n;
        } else if (key == L"UNTIL") {
            Time t;
            if (ParseTimeValue(v, L"", false, t)) out.until = t;
        } else if (key == L"WKST") {
            const int wd = WeekdayCode(v);
            if (wd >= 0) out.wkst = wd;
        } else if (key == L"BYDAY") {
            for (const std::wstring& x : Split(v, L',')) {
                if (x.size() < 2) continue;
                const int wd = WeekdayCode(x.substr(x.size() - 2));
                if (wd < 0) continue;
                Rule::Day d;
                d.weekday = wd;
                if (x.size() > 2 && !ToInt(x.substr(0, x.size() - 2), d.ordinal)) continue;
                if (d.ordinal < -53 || d.ordinal > 53) continue;
                out.byDay.push_back(d);
            }
        } else if (key == L"BYMONTHDAY") {
            ints(out.byMonthDay, -31, 31);
        } else if (key == L"BYMONTH") {
            ints(out.byMonth, 1, 12);
        } else if (key == L"BYYEARDAY") {
            ints(out.byYearDay, -366, 366);
        } else if (key == L"BYSETPOS") {
            ints(out.bySetPos, -366, 366);
        }
    }
    return out.freq != Rule::Freq::None;
}

namespace {

bool Contains(const std::vector<int>& v, int x) { return std::find(v.begin(), v.end(), x) != v.end(); }

// The days of month y-m a MONTHLY rule (or a YEARLY one with BYMONTH) gives, sorted (days since the
// epoch). `dtDom`: DTSTART's day of the month (used without BYMONTHDAY / BYDAY).
void MonthDays(const Rule& r, int y, int m, int dtDom, std::vector<int64_t>& out) {
    const int dim = DaysInMonth(y, m);
    const int64_t first = DaysFromCivil(y, m, 1);
    std::vector<int> set;
    bool have = false;
    if (!r.byMonthDay.empty()) {
        for (int v : r.byMonthDay) {
            const int dd = v > 0 ? v : dim + 1 + v;
            if (dd >= 1 && dd <= dim) set.push_back(dd);
        }
        have = true;
    }
    if (!r.byDay.empty()) {
        std::vector<int> bd;
        const int w1 = WeekdayOf(first);
        for (const Rule::Day& d : r.byDay) {
            const int firstOcc = 1 + (d.weekday - w1 + 7) % 7;
            if (d.ordinal == 0) {
                for (int dd = firstOcc; dd <= dim; dd += 7) bd.push_back(dd);
            } else if (d.ordinal > 0) {
                const int dd = firstOcc + 7 * (d.ordinal - 1);
                if (dd <= dim) bd.push_back(dd);
            } else {
                const int lastOcc = firstOcc + 7 * ((dim - firstOcc) / 7);
                const int dd = lastOcc - 7 * (-d.ordinal - 1);
                if (dd >= 1) bd.push_back(dd);
            }
        }
        if (have) {
            std::vector<int> both;
            for (int dd : set) {
                if (Contains(bd, dd)) both.push_back(dd);
            }
            set = std::move(both);
        } else {
            set = std::move(bd);
        }
        have = true;
    }
    if (!have && dtDom <= dim) set.push_back(dtDom);
    for (int dd : set) out.push_back(first + dd - 1);
}

// The days of year y a YEARLY rule gives.
void YearDays(const Rule& r, int y, int dtMonth, int dtDom, std::vector<int64_t>& out) {
    const int64_t jan1 = DaysFromCivil(y, 1, 1);
    const int diy = static_cast<int>(DaysFromCivil(y + 1, 1, 1) - jan1);
    if (!r.byMonth.empty()) {
        std::vector<int> months = r.byMonth;
        std::sort(months.begin(), months.end());
        for (int m : months) MonthDays(r, y, m, dtDom, out);
        if (!r.byYearDay.empty()) {
            std::erase_if(out, [&](int64_t day) {
                const int yd = static_cast<int>(day - jan1) + 1;
                return !Contains(r.byYearDay, yd) && !Contains(r.byYearDay, yd - diy - 1);
            });
        }
        return;
    }
    if (!r.byYearDay.empty()) {
        for (int v : r.byYearDay) {
            const int yd = v > 0 ? v : diy + 1 + v;
            if (yd < 1 || yd > diy) continue;
            const int64_t day = jan1 + yd - 1;
            if (!r.byDay.empty() && std::none_of(r.byDay.begin(), r.byDay.end(), [&](const Rule::Day& d) {
                    return d.weekday == WeekdayOf(day);
                })) {
                continue;
            }
            out.push_back(day);
        }
        return;
    }
    if (!r.byMonthDay.empty()) {
        for (int m = 1; m <= 12; ++m) MonthDays(r, y, m, dtDom, out);
        return;
    }
    if (!r.byDay.empty()) {  // ordinals within the year
        const int w1 = WeekdayOf(jan1);
        for (const Rule::Day& d : r.byDay) {
            const int firstOcc = 1 + (d.weekday - w1 + 7) % 7;
            if (d.ordinal == 0) {
                for (int dd = firstOcc; dd <= diy; dd += 7) out.push_back(jan1 + dd - 1);
            } else if (d.ordinal > 0) {
                const int dd = firstOcc + 7 * (d.ordinal - 1);
                if (dd <= diy) out.push_back(jan1 + dd - 1);
            } else {
                const int lastOcc = firstOcc + 7 * ((diy - firstOcc) / 7);
                const int dd = lastOcc - 7 * (-d.ordinal - 1);
                if (dd >= 1) out.push_back(jan1 + dd - 1);
            }
        }
        return;
    }
    if (dtDom <= DaysInMonth(y, dtMonth)) out.push_back(DaysFromCivil(y, dtMonth, dtDom));  // Feb 29: leap years only
}

bool DailyMatch(const Rule& r, int64_t day) {
    int y = 0, m = 0, d = 0;
    CivilFromDays(day, y, m, d);
    if (!r.byMonth.empty() && !Contains(r.byMonth, m)) return false;
    if (!r.byMonthDay.empty()) {
        const int dim = DaysInMonth(y, m);
        if (!Contains(r.byMonthDay, d) && !Contains(r.byMonthDay, d - dim - 1)) return false;
    }
    if (!r.byDay.empty()) {
        const int wd = WeekdayOf(day);
        if (std::none_of(r.byDay.begin(), r.byDay.end(), [&](const Rule::Day& x) { return x.weekday == wd; })) return false;
    }
    return true;
}

int64_t WeekStart(int64_t day, int wkst) { return day - (WeekdayOf(day) - wkst + 7) % 7; }

// Calls emit(wall) for the instances of rule `r` after `dtstart` (wall-clock seconds; DTSTART
// itself is the first instance and is not emitted) whose start lies in [winFrom, winTo), in order.
// COUNT counts from DTSTART; `untilWall` is inclusive (INT64_MAX: none). emit returns false to stop.
template <class F>
void ForEachInstance(const Rule& r, int64_t dtstart, int64_t winFrom, int64_t winTo, int64_t untilWall, F&& emit) {
    if (r.freq == Rule::Freq::None || r.freq == Rule::Freq::Unsupported) return;
    const int64_t dtDay = DayOf(dtstart);
    const int64_t tod = dtstart - dtDay * 86400;
    int y0 = 0, m0 = 0, d0 = 0;
    CivilFromDays(dtDay, y0, m0, d0);
    const int64_t interval = std::max(1, r.interval);
    const bool counting = r.count > 0;
    if (counting && r.count <= 1) return;
    int64_t produced = 1;
    int64_t k = 0;
    if (!counting && winFrom > dtstart) {  // skip the periods before the window
        const int64_t fromDay = DayOf(winFrom);
        int yf = 0, mf = 0, df = 0;
        CivilFromDays(fromDay, yf, mf, df);
        int64_t periods = 0;
        switch (r.freq) {
            case Rule::Freq::Daily: periods = (fromDay - dtDay) / interval; break;
            case Rule::Freq::Weekly: periods = (WeekStart(fromDay, r.wkst) - WeekStart(dtDay, r.wkst)) / 7 / interval; break;
            case Rule::Freq::Monthly: periods = ((static_cast<int64_t>(yf) * 12 + mf) - (static_cast<int64_t>(y0) * 12 + m0)) / interval; break;
            case Rule::Freq::Yearly: periods = (yf - y0) / interval; break;
            default: break;
        }
        k = std::max<int64_t>(0, periods - 1);
    }
    std::vector<int64_t> days;
    std::vector<int> weekdays;
    for (const Rule::Day& d : r.byDay) {
        if (!Contains(weekdays, d.weekday)) weekdays.push_back(d.weekday);
    }
    if (weekdays.empty()) weekdays.push_back(WeekdayOf(dtDay));
    for (int guard = 0; guard < 400000; ++guard, ++k) {
        days.clear();
        int64_t periodFirst = 0;
        switch (r.freq) {
            case Rule::Freq::Daily: {
                const int64_t d = dtDay + k * interval;
                periodFirst = d;
                if (DailyMatch(r, d)) days.push_back(d);
                break;
            }
            case Rule::Freq::Weekly: {
                const int64_t ws = WeekStart(dtDay, r.wkst) + k * 7 * interval;
                periodFirst = ws;
                for (int wd : weekdays) {
                    const int64_t d = ws + (wd - r.wkst + 7) % 7;
                    if (!r.byMonth.empty()) {
                        int y = 0, m = 0, dd = 0;
                        CivilFromDays(d, y, m, dd);
                        if (!Contains(r.byMonth, m)) continue;
                    }
                    days.push_back(d);
                }
                break;
            }
            case Rule::Freq::Monthly: {
                const int64_t idx = static_cast<int64_t>(y0) * 12 + (m0 - 1) + k * interval;
                const int y = static_cast<int>(FloorDiv(idx, 12)), m = static_cast<int>(idx - FloorDiv(idx, 12) * 12) + 1;
                periodFirst = DaysFromCivil(y, m, 1);
                if (r.byMonth.empty() || Contains(r.byMonth, m)) MonthDays(r, y, m, d0, days);
                break;
            }
            case Rule::Freq::Yearly: {
                const int y = static_cast<int>(y0 + k * interval);
                periodFirst = DaysFromCivil(y, 1, 1);
                YearDays(r, y, m0, d0, days);
                break;
            }
            default: return;
        }
        if (periodFirst * 86400 >= winTo) return;  // nothing later is inside the window
        if (untilWall != INT64_MAX && periodFirst * 86400 > untilWall) return;
        std::sort(days.begin(), days.end());
        days.erase(std::unique(days.begin(), days.end()), days.end());
        if (!r.bySetPos.empty() && !days.empty()) {
            std::vector<int64_t> picked;
            const int n = static_cast<int>(days.size());
            for (int pos : r.bySetPos) {
                const int i = pos > 0 ? pos - 1 : n + pos;
                if (i >= 0 && i < n) picked.push_back(days[static_cast<size_t>(i)]);
            }
            std::sort(picked.begin(), picked.end());
            picked.erase(std::unique(picked.begin(), picked.end()), picked.end());
            days = std::move(picked);
        }
        for (int64_t day : days) {
            const int64_t t = day * 86400 + tod;
            if (t <= dtstart) continue;
            if (t > untilWall) return;
            if (counting && ++produced > r.count) return;
            if (t >= winTo) return;
            if (t >= winFrom && !emit(t)) return;
        }
    }
}

// --- content lines -------------------------------------------------------------------------------

std::wstring Wide(const char* s, size_t n) {
    if (n == 0) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, s, static_cast<int>(n), nullptr, 0);
    if (len <= 0) {  // not UTF-8: Latin-1
        std::wstring w(n, L'\0');
        for (size_t i = 0; i < n; ++i) w[i] = static_cast<unsigned char>(s[i]);
        return w;
    }
    std::wstring w(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, static_cast<int>(n), w.data(), len);
    return w;
}

struct Line {
    std::wstring name;  // upper case
    std::vector<std::pair<std::wstring, std::wstring>> params;  // name upper case, value unquoted
    std::wstring value;
    std::wstring Param(const wchar_t* key) const {
        for (const auto& [k, v] : params) {
            if (k == key) return v;
        }
        return {};
    }
};

bool ParseLine(const std::wstring& raw, Line& out) {
    out = Line{};
    size_t i = 0;
    while (i < raw.size() && raw[i] != L';' && raw[i] != L':') ++i;
    if (i == 0 || i >= raw.size()) return false;
    out.name = Upper(raw.substr(0, i));
    while (i < raw.size() && raw[i] == L';') {
        ++i;
        size_t eq = i;
        while (eq < raw.size() && raw[eq] != L'=' && raw[eq] != L':' && raw[eq] != L';') ++eq;
        if (eq >= raw.size()) return false;
        std::wstring key = Upper(raw.substr(i, eq - i));
        std::wstring val;
        i = eq;
        if (raw[i] == L'=') {
            ++i;
            // One or more values, comma separated, each maybe quoted: kept as one text.
            for (;;) {
                if (i < raw.size() && raw[i] == L'"') {
                    const size_t close = raw.find(L'"', i + 1);
                    if (close == std::wstring::npos) return false;
                    val += raw.substr(i + 1, close - i - 1);
                    i = close + 1;
                } else {
                    const size_t start = i;
                    while (i < raw.size() && raw[i] != L';' && raw[i] != L':' && raw[i] != L',') ++i;
                    val += raw.substr(start, i - start);
                }
                if (i < raw.size() && raw[i] == L',') {
                    val += L',';
                    ++i;
                    continue;
                }
                break;
            }
        }
        out.params.emplace_back(std::move(key), std::move(val));
    }
    if (i >= raw.size() || raw[i] != L':') return false;
    out.value = raw.substr(i + 1);
    return true;
}

std::wstring Unescape(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'\\' && i + 1 < s.size()) {
            const wchar_t n = s[++i];
            out += (n == L'n' || n == L'N') ? L'\n' : n;
        } else {
            out += s[i];
        }
    }
    return out;
}

bool Digits(const std::wstring& s, size_t at, size_t n, int& v) {
    if (at + n > s.size()) return false;
    v = 0;
    for (size_t i = 0; i < n; ++i) {
        const wchar_t c = s[at + i];
        if (c < L'0' || c > L'9') return false;
        v = v * 10 + (c - L'0');
    }
    return true;
}

bool ParseTimeValue(const std::wstring& raw, const std::wstring& tzid, bool dateParam, Time& out) {
    std::wstring v = raw;
    while (!v.empty() && (v.back() == L' ' || v.back() == L'\t')) v.pop_back();
    int y = 0, m = 0, d = 0;
    if (!Digits(v, 0, 4, y) || !Digits(v, 4, 2, m) || !Digits(v, 6, 2, d) || m < 1 || m > 12 || d < 1 ||
        d > DaysInMonth(y, m)) {
        return false;
    }
    const int64_t day = DaysFromCivil(y, m, d);
    if (v.size() == 8 || dateParam) {
        out.kind = Time::Kind::Date;
        out.wall = day * 86400;
        out.tzid.clear();
        return true;
    }
    int hh = 0, mm = 0, ss = 0;
    if (v.size() < 15 || (v[8] != L'T' && v[8] != L't') || !Digits(v, 9, 2, hh) || !Digits(v, 11, 2, mm) ||
        !Digits(v, 13, 2, ss) || hh > 23 || mm > 59 || ss > 60) {
        return false;
    }
    out.wall = day * 86400 + hh * 3600 + mm * 60 + std::min(ss, 59);
    if (v.size() >= 16 && (v[15] == L'Z' || v[15] == L'z')) {
        out.kind = Time::Kind::Utc;
        out.tzid.clear();
    } else if (!tzid.empty()) {
        out.kind = Time::Kind::Zoned;
        out.tzid = tzid;
    } else {
        out.kind = Time::Kind::Floating;
        out.tzid.clear();
    }
    return true;
}

bool ParseTime(const Line& l, Time& out) {
    return ParseTimeValue(l.value, l.Param(L"TZID"), Upper(l.Param(L"VALUE")) == L"DATE", out);
}

void ParseTimeList(const Line& l, std::vector<Time>& out) {
    const std::wstring tzid = l.Param(L"TZID");
    const bool date = Upper(l.Param(L"VALUE")) == L"DATE";
    for (std::wstring v : Split(l.value, L',')) {
        const size_t slash = v.find(L'/');  // PERIOD: its start
        if (slash != std::wstring::npos) v.resize(slash);
        Time t;
        if (ParseTimeValue(v, tzid, date, t)) out.push_back(t);
    }
}

// "[+-]P[nW][nD][T[nH][nM][nS]]" -> nominal days, exact seconds (negative for "-").
bool ParseDuration(const std::wstring& raw, int64_t& days, int64_t& secs) {
    std::wstring v = Upper(raw);
    days = secs = 0;
    size_t i = 0;
    bool neg = false;
    if (i < v.size() && (v[i] == L'+' || v[i] == L'-')) neg = v[i++] == L'-';
    if (i >= v.size() || v[i++] != L'P') return false;
    bool time = false, any = false;
    int64_t n = 0;
    bool digits = false;
    for (; i < v.size(); ++i) {
        const wchar_t c = v[i];
        if (c >= L'0' && c <= L'9') {
            n = n * 10 + (c - L'0');
            if (n > 100000000) return false;
            digits = true;
            continue;
        }
        if (c == L'T') {
            time = true;
            continue;
        }
        if (!digits) return false;
        if (c == L'W' && !time) days += n * 7;
        else if (c == L'D' && !time) days += n;
        else if (c == L'H' && time) secs += n * 3600;
        else if (c == L'M' && time) secs += n * 60;
        else if (c == L'S' && time) secs += n;
        else return false;
        n = 0;
        digits = false;
        any = true;
    }
    if (!any || digits) return false;
    if (neg) {
        days = -days;
        secs = -secs;
    }
    return true;
}

bool ParseOffset(const std::wstring& raw, int& out) {
    std::wstring v = raw;
    while (!v.empty() && v.back() == L' ') v.pop_back();
    if (v.size() < 5 || (v[0] != L'+' && v[0] != L'-')) return false;
    int hh = 0, mm = 0, ss = 0;
    if (!Digits(v, 1, 2, hh) || !Digits(v, 3, 2, mm)) return false;
    if (v.size() >= 7) Digits(v, 5, 2, ss);
    out = (hh * 3600 + mm * 60 + ss) * (v[0] == L'-' ? -1 : 1);
    return true;
}

}  // namespace

bool Parse(const std::string& text, Calendar& out, std::wstring* error) {
    out = Calendar{};
    try {
        // Unfold into logical lines (CRLF or LF; a line starting with a space or tab continues the
        // previous one).
        std::vector<std::string> lines;
        size_t at = 0;
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
            static_cast<unsigned char>(text[2]) == 0xBF) {
            at = 3;
        }
        while (at < text.size()) {
            size_t end = text.find('\n', at);
            if (end == std::string::npos) end = text.size();
            size_t len = end - at;
            if (len > 0 && text[at + len - 1] == '\r') --len;
            if (len > 0 && (text[at] == ' ' || text[at] == '\t') && !lines.empty()) {
                lines.back().append(text, at + 1, len - 1);
            } else if (len > 0) {
                lines.emplace_back(text, at, len);
            }
            at = end + 1;
        }

        enum class Comp { Other, Calendar, Event, Alarm, Zone, Observance };
        std::vector<Comp> stack;
        bool sawCalendar = false;
        Event ev;
        bool alarmTriggered = false;
        std::wstring zoneId;
        std::vector<Observance> observances;
        Observance obs;
        bool obsValid = true;
        Line l;
        for (const std::string& raw : lines) {
            if (!ParseLine(Wide(raw.data(), raw.size()), l)) {
                ++out.skipped;
                continue;
            }
            if (l.name == L"BEGIN") {
                const std::wstring what = Upper(l.value);
                const Comp parent = stack.empty() ? Comp::Other : stack.back();
                Comp c = Comp::Other;
                if (what == L"VCALENDAR" && stack.empty()) {
                    c = Comp::Calendar;
                    sawCalendar = true;
                } else if (what == L"VEVENT" && parent == Comp::Calendar) {
                    c = Comp::Event;
                    ev = Event{};
                    alarmTriggered = false;
                } else if (what == L"VALARM" && parent == Comp::Event) {
                    c = Comp::Alarm;
                } else if (what == L"VTIMEZONE" && parent == Comp::Calendar) {
                    c = Comp::Zone;
                    zoneId.clear();
                    observances.clear();
                } else if ((what == L"STANDARD" || what == L"DAYLIGHT") && parent == Comp::Zone) {
                    c = Comp::Observance;
                    obs = Observance{};
                    obsValid = true;
                }
                stack.push_back(c);
                continue;
            }
            if (l.name == L"END") {
                if (stack.empty()) continue;
                const Comp c = stack.back();
                stack.pop_back();
                if (c == Comp::Event) {
                    if (ev.start.Valid()) {
                        out.events.push_back(std::move(ev));
                    } else {
                        ++out.skipped;
                    }
                    ev = Event{};
                } else if (c == Comp::Observance) {
                    if (obsValid) observances.push_back(obs);
                } else if (c == Comp::Zone) {
                    if (!zoneId.empty() && !observances.empty()) out.zones[zoneId] = std::make_shared<VZone>(observances);
                }
                continue;
            }
            const Comp c = stack.empty() ? Comp::Other : stack.back();
            switch (c) {
                case Comp::Calendar:
                    if (l.name == L"X-WR-CALNAME") out.name = Unescape(l.value);
                    if (l.name == L"X-WR-TIMEZONE") out.floatingZone = l.value;
                    break;
                case Comp::Event:
                    if (l.name == L"UID") {
                        ev.uid = l.value;
                    } else if (l.name == L"SUMMARY") {
                        ev.summary = Unescape(l.value);
                    } else if (l.name == L"DESCRIPTION") {
                        ev.description = Unescape(l.value);
                    } else if (l.name == L"LOCATION") {
                        ev.location = Unescape(l.value);
                    } else if (l.name == L"URL") {
                        ev.url = l.value;
                    } else if (l.name == L"DTSTART") {
                        if (!ParseTime(l, ev.start)) ++out.skipped;
                    } else if (l.name == L"DTEND") {
                        if (!ParseTime(l, ev.end)) ++out.skipped;
                    } else if (l.name == L"DURATION") {
                        ev.hasDuration = ParseDuration(l.value, ev.durationDays, ev.durationSecs);
                        if (!ev.hasDuration) ++out.skipped;
                    } else if (l.name == L"RRULE") {
                        if (!ev.hasRule) ev.hasRule = ParseRule(l.value, ev.rule);  // the first RRULE
                    } else if (l.name == L"EXDATE") {
                        ParseTimeList(l, ev.exdates);
                    } else if (l.name == L"RDATE") {
                        ParseTimeList(l, ev.rdates);
                    } else if (l.name == L"RECURRENCE-ID") {
                        if (!ParseTime(l, ev.recurrenceId)) ++out.skipped;
                    } else if (l.name == L"STATUS") {
                        ev.cancelled = Upper(l.value) == L"CANCELLED";
                    }
                    break;
                case Comp::Alarm:
                    if (l.name == L"TRIGGER" && !alarmTriggered) {
                        alarmTriggered = true;
                        int64_t days = 0, secs = 0;
                        const std::wstring related = Upper(l.Param(L"RELATED"));
                        if (Upper(l.Param(L"VALUE")) != L"DATE-TIME" && related != L"END" &&
                            ParseDuration(l.value, days, secs)) {
                            const int64_t before = -(days * 86400 + secs);
                            if (before >= 0 && before <= 31LL * 86400) ev.reminderMin = static_cast<int>(before / 60);
                        }
                    }
                    break;
                case Comp::Zone:
                    if (l.name == L"TZID") zoneId = l.value;
                    break;
                case Comp::Observance:
                    if (l.name == L"DTSTART") {
                        Time t;
                        if (ParseTime(l, t)) {
                            obs.start = t.wall;
                        } else {
                            obsValid = false;
                        }
                    } else if (l.name == L"TZOFFSETFROM") {
                        if (!ParseOffset(l.value, obs.from)) obsValid = false;
                    } else if (l.name == L"TZOFFSETTO") {
                        if (!ParseOffset(l.value, obs.to)) obsValid = false;
                    } else if (l.name == L"RRULE") {
                        obs.hasRule = ParseRule(l.value, obs.rule);
                    } else if (l.name == L"RDATE") {
                        std::vector<Time> list;
                        ParseTimeList(l, list);
                        for (const Time& t : list) obs.rdates.push_back(t.wall);
                    }
                    break;
                default:
                    break;
            }
        }
        if (!sawCalendar) {
            if (error) *error = L"not an iCalendar file (no VCALENDAR)";
            out = Calendar{};
            return false;
        }
        // Every TZID used without a VTIMEZONE: the Windows zone of that name (or the local zone).
        std::set<std::wstring> used;
        for (const Event& e : out.events) {
            for (const Time* t : {&e.start, &e.end, &e.recurrenceId}) {
                if (t->kind == Time::Kind::Zoned) used.insert(t->tzid);
            }
            for (const auto* list : {&e.exdates, &e.rdates}) {
                for (const Time& t : *list) {
                    if (t.kind == Time::Kind::Zoned) used.insert(t.tzid);
                }
            }
        }
        for (const std::wstring& tz : used) {
            if (out.zones.count(tz)) continue;
            if (auto z = ResolveZoneName(tz)) {
                out.zones[tz] = z;
                out.fallbackZones.push_back(tz);
            } else {
                out.zones[tz] = nullptr;
                out.unknownZones.push_back(tz);
            }
        }
        if (!out.floatingZone.empty()) {
            auto it = out.zones.find(out.floatingZone);
            out.floating = it != out.zones.end() && it->second ? it->second : ResolveZoneName(out.floatingZone);
        }
        return true;
    } catch (...) {  // std::bad_alloc
        if (error) *error = L"out of memory";
        out = Calendar{};
        return false;
    }
}

// --- expansion -----------------------------------------------------------------------------------

std::vector<Occurrence> Expand(const Calendar& cal, int64_t from, int64_t to, const Zone& local, size_t limit) {
    std::vector<Occurrence> out;
    const Zone& utc = *UtcZone();
    const Zone& floating = cal.floating ? *cal.floating : local;
    auto zoneOf = [&](const Time& t) -> const Zone& {
        switch (t.kind) {
            case Time::Kind::Utc: return utc;
            case Time::Kind::Zoned: {
                auto it = cal.zones.find(t.tzid);
                return it != cal.zones.end() && it->second ? *it->second : floating;
            }
            case Time::Kind::Floating: return floating;
            case Time::Kind::Date:
            default: return local;
        }
    };
    auto toUtc = [&](const Time& t) { return t.kind == Time::Kind::Utc ? t.wall : zoneOf(t).ToUtc(t.wall); };

    // Adds an occurrence if it overlaps the window. All-day: [startDay, endDay) local dates.
    auto addTimed = [&](uint32_t index, int64_t s, int64_t e, bool recurring, int64_t original) {
        if (e < s) e = s;
        const bool inside = e > s ? (e > from && s < to) : (s >= from && s < to);
        if (!inside) return;
        Occurrence o;
        o.start = s;
        o.end = e;
        o.firstDay = DayOf(local.ToWall(s));
        o.lastDay = e > s ? DayOf(local.ToWall(e - 1)) : o.firstDay;
        o.originalStart = original;
        o.event = index;
        o.recurring = recurring;
        out.push_back(o);
    };
    auto addAllDay = [&](uint32_t index, int64_t startDay, int64_t endDay, bool recurring, int64_t original) {
        if (endDay <= startDay) endDay = startDay + 1;
        const int64_t s = local.ToUtc(startDay * 86400), e = local.ToUtc(endDay * 86400);
        if (!(e > from && s < to)) return;
        Occurrence o;
        o.start = s;
        o.end = e;
        o.firstDay = startDay;
        o.lastDay = endDay - 1;
        o.originalStart = original;
        o.event = index;
        o.allDay = true;
        o.recurring = recurring;
        out.push_back(o);
    };

    // Overrides (RECURRENCE-ID) by UID: their original instants (UTC; all-day: day * 86400).
    std::map<std::wstring, std::vector<int64_t>> overridden;
    for (const Event& e : cal.events) {
        if (!e.IsOverride()) continue;
        overridden[e.uid].push_back(e.recurrenceId.IsDate() ? e.recurrenceId.wall : toUtc(e.recurrenceId));
    }

    std::vector<int64_t> instances;
    for (uint32_t i = 0; i < cal.events.size() && out.size() < limit * 2; ++i) {
        const Event& ev = cal.events[i];
        if (!ev.start.Valid()) continue;
        const bool allDay = ev.start.IsDate();
        if (ev.IsOverride()) {  // shown at its own time (or not at all when cancelled)
            if (ev.cancelled) continue;
            const int64_t original = ev.recurrenceId.IsDate() ? ev.recurrenceId.wall : toUtc(ev.recurrenceId);
            if (allDay) {
                int64_t endDay = ev.end.IsDate() ? DayOf(ev.end.wall)
                                 : ev.hasDuration ? DayOf(ev.start.wall) + ev.durationDays + ev.durationSecs / 86400
                                                  : DayOf(ev.start.wall) + 1;
                addAllDay(i, DayOf(ev.start.wall), endDay, true, original);
            } else {
                const int64_t s = toUtc(ev.start);
                const int64_t e = ev.end.Valid() ? toUtc(ev.end)
                                  : ev.hasDuration ? zoneOf(ev.start).ToUtc(ev.start.wall + ev.durationDays * 86400) + ev.durationSecs
                                                   : s;
                addTimed(i, s, e, true, original);
            }
            continue;
        }
        if (ev.cancelled) continue;
        const Zone& z = zoneOf(ev.start);
        const int64_t startWall = ev.start.wall;  // Utc: UTC seconds = wall-clock of the UTC zone
        const int64_t tod = startWall - DayOf(startWall) * 86400;
        // The duration: wall-clock when start and end are in the same zone (an hour-long meeting stays
        // an hour of wall-clock time across a DST change), exact otherwise.
        int64_t wallDur = 0, exactDur = 0, dayDur = 1;
        if (allDay) {
            if (ev.end.Valid()) {
                dayDur = DayOf(ev.end.wall) - DayOf(ev.start.wall);
            } else if (ev.hasDuration) {
                dayDur = ev.durationDays + ev.durationSecs / 86400;
            }
            if (dayDur < 1) dayDur = 1;
        } else if (ev.end.Valid()) {
            if (ev.end.kind == ev.start.kind && ev.end.tzid == ev.start.tzid && !ev.end.IsDate()) {
                wallDur = ev.end.wall - ev.start.wall;
            } else {
                exactDur = toUtc(ev.end) - toUtc(ev.start);
            }
        } else if (ev.hasDuration) {
            wallDur = ev.durationDays * 86400;
            exactDur = ev.durationSecs;
        }
        const bool recurring = ev.hasRule || !ev.rdates.empty();

        instances.clear();
        instances.push_back(startWall);
        if (recurring) {
            const int64_t span = allDay ? dayDur * 86400 : std::max<int64_t>(0, wallDur + exactDur);
            const Zone& wz = allDay ? local : z;
            const int64_t winFrom = wz.ToWall(from) - span - 2 * 86400;
            const int64_t winTo = wz.ToWall(to) + 2 * 86400;
            if (ev.hasRule) {
                int64_t until = INT64_MAX;
                const Time& u = ev.rule.until;
                if (u.Valid()) {
                    if (u.IsDate()) {
                        until = u.wall + 86399;
                    } else if (u.kind == Time::Kind::Utc) {
                        until = allDay ? u.wall : z.ToWall(u.wall);
                    } else {
                        until = u.wall;  // floating: the event's own wall-clock
                    }
                }
                ForEachInstance(ev.rule, startWall, winFrom, winTo, until, [&](int64_t wall) {
                    instances.push_back(wall);
                    return instances.size() < 50000;
                });
            }
            for (const Time& r : ev.rdates) {
                int64_t wall = 0;
                if (r.IsDate()) {
                    wall = r.wall + (allDay ? 0 : tod);
                } else if (allDay) {
                    wall = DayOf(r.wall) * 86400;
                } else {
                    wall = z.ToWall(toUtc(r));
                }
                if (wall >= winFrom && wall < winTo) instances.push_back(wall);
            }
            std::sort(instances.begin(), instances.end());
            instances.erase(std::unique(instances.begin(), instances.end()), instances.end());
        }
        // EXDATE: instants (UTC) and dates.
        std::vector<int64_t> exUtc, exDays;
        for (const Time& x : ev.exdates) {
            if (x.IsDate() || allDay) {
                exDays.push_back(DayOf(x.kind == Time::Kind::Utc && !allDay ? z.ToWall(x.wall) : x.wall));
            } else {
                exUtc.push_back(toUtc(x));
            }
        }
        const auto ov = overridden.find(ev.uid);
        for (int64_t wall : instances) {
            const int64_t day = DayOf(wall);
            if (std::find(exDays.begin(), exDays.end(), day) != exDays.end()) continue;
            if (allDay) {
                if (ov != overridden.end() &&
                    std::find(ov->second.begin(), ov->second.end(), day * 86400) != ov->second.end()) {
                    continue;
                }
                addAllDay(i, day, day + dayDur, recurring, day * 86400);
            } else {
                const int64_t s = z.ToUtc(wall);
                if (std::find(exUtc.begin(), exUtc.end(), s) != exUtc.end()) continue;
                if (ov != overridden.end() && std::find(ov->second.begin(), ov->second.end(), s) != ov->second.end()) continue;
                const int64_t e = (wallDur ? z.ToUtc(wall + wallDur) : s) + exactDur;
                addTimed(i, s, e, recurring, s);
            }
        }
    }
    std::sort(out.begin(), out.end(), [&](const Occurrence& a, const Occurrence& b) {
        if (a.start != b.start) return a.start < b.start;
        if (a.allDay != b.allDay) return a.allDay;
        if (a.end != b.end) return a.end < b.end;
        return a.event < b.event;
    });
    if (out.size() > limit) out.resize(limit);
    return out;
}

}  // namespace Ics
