#include "WidgetRuntime.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <iterator>

#include "Clock.h"
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
    return static_cast<uint32_t>(delay + 15u);  // just after the boundary, never just before it
}

void WidgetRuntime::Shutdown() {
    if (owner_ && armed_) KillTimer(owner_, kTimerId);
    armed_ = false;
    entries_.clear();
    owner_ = nullptr;
}

bool WidgetRuntime::HandleMessage(UINT msg, WPARAM wp, LPARAM) {
    if (msg != WM_TIMER || wp != kTimerId) return false;
    KillTimer(owner_, kTimerId);  // one-shot; re-armed for the next deadline
    armed_ = false;
    ++wakeups_;
    OnTimer();
    return true;
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

void WidgetRuntime::Add(WidgetInstance* w) { instances_.push_back(w); }

void WidgetRuntime::Remove(WidgetInstance* w) {
    std::erase(instances_, w);
    CancelAll(w);
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
    const double now = WidgetNowMs() + 1.0;  // a timer may fire up to a tick early
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
