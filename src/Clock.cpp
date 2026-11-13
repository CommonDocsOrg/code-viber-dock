#include "Clock.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <iterator>

#include "Theme.h"

namespace {

constexpr wchar_t kDateFormat[] = L"%a %d %b";
constexpr wchar_t kFallbackFormat[] = L"%H:%M";
constexpr wchar_t kFontFamily[] = L"Segoe UI";

// wcsftime reports bad conversion specifiers through the CRT invalid-parameter handler,
// which terminates the process by default. Swallow it for user-supplied formats.
void __cdecl IgnoreInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {}

// wcsftime without Format's fallback: "" for an empty result (a "%p" in a 24-hour locale).
std::wstring FormatRaw(const std::wstring& format, const tm& t) {
    if (format.empty()) return {};
    wchar_t buf[256];
    auto previous = _set_thread_local_invalid_parameter_handler(IgnoreInvalidParameter);
    const size_t n = wcsftime(buf, std::size(buf), format.c_str(), &t);
    _set_thread_local_invalid_parameter_handler(previous);
    return std::wstring(buf, n);
}

// Just past the last minutes specifier (%M, %#M, %R), npos without one (Clock::EffectiveFormat).
size_t MinutesEnd(const std::wstring& format) {
    size_t after = std::wstring::npos;
    for (size_t i = 0; i + 1 < format.size(); ++i) {
        if (format[i] != L'%') continue;
        size_t j = i + 1;
        if (format[j] == L'#' && j + 1 < format.size()) ++j;
        if (format[j] == L'M' || format[j] == L'R') after = j + 1;
        i = j;
    }
    return after;
}

std::wstring ApplyCase(std::wstring s, ClockFace::Case c) {
    if (c == ClockFace::Case::AsIs || s.empty()) return s;
    const DWORD flags = (c == ClockFace::Case::Upper ? LCMAP_UPPERCASE : LCMAP_LOWERCASE) | LCMAP_LINGUISTIC_CASING;
    std::wstring out(s.size() + 8, L'\0');
    const int n = LCMapStringEx(LOCALE_NAME_USER_DEFAULT, flags, s.c_str(), static_cast<int>(s.size()), out.data(),
                                static_cast<int>(out.size()), nullptr, nullptr, 0);
    if (n <= 0) return s;
    out.resize(static_cast<size_t>(n));
    return out;
}

void ReplaceAll(std::wstring& s, wchar_t what, const std::wstring& with) {
    for (size_t at = s.find(what); at != std::wstring::npos; at = s.find(what, at + with.size())) s.replace(at, 1, with);
}

DWRITE_TEXT_ALIGNMENT AlignOf(ClockFace::Align a) {
    return a == ClockFace::Align::Start ? DWRITE_TEXT_ALIGNMENT_LEADING
           : a == ClockFace::Align::End ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                        : DWRITE_TEXT_ALIGNMENT_CENTER;
}

bool IsLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

// A wall-clock SYSTEMTIME as the tm wcsftime formats (weekday and day of the year computed: they
// do not depend on the zone's own data).
tm TmFrom(const SYSTEMTIME& s) {
    static constexpr int kBefore[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    tm t{};
    t.tm_year = s.wYear - 1900;
    t.tm_mon = std::clamp<int>(s.wMonth, 1, 12) - 1;
    t.tm_mday = s.wDay;
    t.tm_hour = s.wHour;
    t.tm_min = s.wMinute;
    t.tm_sec = s.wSecond;
    t.tm_yday = kBefore[t.tm_mon] + s.wDay - 1 + (t.tm_mon > 1 && IsLeap(s.wYear) ? 1 : 0);
    // Sakamoto's method (0 = Sunday).
    static constexpr int kOffsets[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int y = s.wYear - (s.wMonth < 3 ? 1 : 0);
    t.tm_wday = (y + y / 4 - y / 100 + y / 400 + kOffsets[t.tm_mon] + s.wDay) % 7;
    t.tm_isdst = 0;
    return t;
}

int64_t Minutes(const SYSTEMTIME& s) {
    FILETIME ft{};
    if (!SystemTimeToFileTime(&s, &ft)) return 0;
    const int64_t ticks = static_cast<int64_t>((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
    return ticks / 600000000LL;  // 100 ns -> minutes
}

// Minutes zone `zone` (null: the local zone) is ahead of UTC at UTC time `utc`.
bool ZoneMinutes(const DYNAMIC_TIME_ZONE_INFORMATION* zone, const SYSTEMTIME& utc, int64_t& minutes) {
    SYSTEMTIME local{};
    if (!SystemTimeToTzSpecificLocalTimeEx(zone, &utc, &local)) return false;
    minutes = Minutes(local) - Minutes(utc);
    return true;
}

}  // namespace

std::vector<Clock::TimeZoneInfo> Clock::TimeZones() {
    // Round 18 phase 18D: cached for ten minutes. The list (about 140 zones, a registry read each)
    // was enumerated by every settings window with a time zone field (one per clock widget: a
    // dozen windows for the settings search's index) and by the index itself; the zones of a
    // Windows change only with an update, so a fresh read every so often is enough.
    static std::vector<TimeZoneInfo> cached;
    static ULONGLONG cachedAt = 0;
    const ULONGLONG now = GetTickCount64();
    if (!cached.empty() && now - cachedAt < 10 * 60 * 1000) return cached;
    std::vector<TimeZoneInfo> zones;
    DYNAMIC_TIME_ZONE_INFORMATION z{};
    for (DWORD i = 0; EnumDynamicTimeZoneInformation(i, &z) == ERROR_SUCCESS && i < 1000; ++i) {
        // The display name is not part of DYNAMIC_TIME_ZONE_INFORMATION: "Display" of the zone's
        // registry key (what Windows' own lists show).
        std::wstring display;
        wchar_t buf[256]{};
        DWORD size = sizeof(buf);
        const std::wstring key = std::wstring(L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones\\") + z.TimeZoneKeyName;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"Display", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS) {
            display = buf;
        }
        if (display.empty()) display = z.StandardName[0] ? z.StandardName : z.TimeZoneKeyName;
        zones.push_back(TimeZoneInfo{z.TimeZoneKeyName, display, z.Bias});
    }
    std::sort(zones.begin(), zones.end(), [](const TimeZoneInfo& a, const TimeZoneInfo& b) {
        if (a.bias != b.bias) return a.bias > b.bias;  // UTC-12 first (the largest bias)
        return _wcsicmp(a.display.c_str(), b.display.c_str()) < 0;
    });
    cached = zones;
    cachedAt = now;
    return zones;
}

bool Clock::FindTimeZone(const std::wstring& key, DYNAMIC_TIME_ZONE_INFORMATION& out) {
    if (key.empty()) return false;
    DYNAMIC_TIME_ZONE_INFORMATION z{};
    for (DWORD i = 0; EnumDynamicTimeZoneInformation(i, &z) == ERROR_SUCCESS && i < 1000; ++i) {
        if (_wcsicmp(z.TimeZoneKeyName, key.c_str()) == 0) {
            out = z;
            return true;
        }
    }
    return false;
}

int Clock::ZoneOffsetMinutes(const std::wstring& key) {
    DYNAMIC_TIME_ZONE_INFORMATION z{};
    if (!FindTimeZone(key, z)) return 0;
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    int64_t there = 0, here = 0;
    if (!ZoneMinutes(&z, utc, there) || !ZoneMinutes(nullptr, utc, here)) return 0;
    return static_cast<int>(there - here);
}

void Clock::SetTimeZone(const std::wstring& key) {
    zone_ = DYNAMIC_TIME_ZONE_INFORMATION{};
    zoned_ = FindTimeZone(key, zone_);
    timeText_.clear();
    dateText_.clear();
    faceLines_.clear();
}

void Clock::SetLabel(const std::wstring& label) {
    if (label == label_) return;
    label_ = label;
    timeText_.clear();
    dateText_.clear();
    faceLines_.clear();
    ReleaseLayout();  // the time's size depends on a second line
}

int Clock::OffsetMinutes() const {
    if (!zoned_) return 0;
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    int64_t there = 0, here = 0;
    if (!ZoneMinutes(&zone_, utc, there) || !ZoneMinutes(nullptr, utc, here)) return 0;
    return static_cast<int>(there - here);
}

bool Clock::Now(tm& out) const {
    if (zoned_) {
        SYSTEMTIME utc{}, there{};
        GetSystemTime(&utc);
        if (SystemTimeToTzSpecificLocalTimeEx(&zone_, &utc, &there)) {
            out = TmFrom(there);
            return true;
        }
    }
    __time64_t now = _time64(nullptr);
    return _localtime64_s(&out, &now) == 0;
}

std::wstring Clock::DateLine(const std::wstring& date) const {
    if (label_.empty()) return date;
    if (date.empty()) return label_;
    return label_ + L" · " + date;
}

void Clock::Configure(const std::wstring& format, bool showDate) {
    format_ = format;
    showDate_ = showDate;
    timeText_.clear();
    dateText_.clear();
    faceLines_.clear();
    ReleaseLayout();
}

void Clock::SetFace(std::shared_ptr<const ClockFace> face, const ClockFaceColors& colors, const std::wstring& rawFormat,
                    bool showSeconds) {
    face_ = std::move(face);
    faceColors_ = colors;
    rawFormat_ = rawFormat;
    rawSeconds_ = showSeconds;
    faceLines_.clear();
    timeText_.clear();
    dateText_.clear();
    ReleaseLayout();
}

void Clock::SetLook(const Look& look) {
    if (look == look_) return;
    look_ = look;
    // The texts stay (the look changes no text): the digital layouts are rebuilt from them by the
    // next BuildLayout without an UpdateText, which matters when ClockElement::Measure sets a
    // smaller look to fit the bar after the dock's Update() of that pass (found by the smoke test:
    // clearing them left an empty block until the next tick). A face's lines carry the look's
    // fonts and colours, so they are made again (BuildFaceLayout calls UpdateText when they are gone).
    faceLines_.clear();
    ReleaseLayout();
}

bool Clock::FaceLine::SameText(const FaceLine& o) const {
    if (text != o.text || font != o.font || weight != o.weight || size != o.size || color != o.color ||
        ranges.size() != o.ranges.size()) {
        return false;
    }
    for (size_t i = 0; i < ranges.size(); ++i) {
        const FaceRange& a = ranges[i];
        const FaceRange& b = o.ranges[i];
        if (a.start != b.start || a.length != b.length || a.font != b.font || a.weight != b.weight || a.size != b.size ||
            a.color != b.color) {
            return false;
        }
    }
    return true;
}

// The face's lines for local time `t`: texts, base styles (DIPs) and the styled ranges (small
// seconds, the suffix, the date on an inline line). Layouts come later (BuildFaceLayout).
std::vector<Clock::FaceLine> Clock::FaceTexts(const tm& t) const {
    const ClockFace& f = *face_;
    // Round 20 phase 20A: the item's look over the face's (Look): sizes, one font for every line
    // (the ranges' own fonts dropped), the time's weight, the time line's and the date line's colours.
    const float timeDip = (SecondLine() ? 14.0f : 16.0f) * f.time.size * look_.scale;
    const float dateDip = 11.0f * f.date.size * look_.scale;
    auto pick = [](const std::wstring& a, const std::wstring& b) { return a.empty() ? b : a; };
    auto font = [this, &pick](const std::wstring& own) { return look_.font.empty() ? own : look_.font; };
    auto rangeFont = [this](const std::wstring& own) { return look_.font.empty() ? own : std::wstring(); };
    const uint32_t timeColor = look_.timeColor ? look_.timeColor : faceColors_.time;
    const uint32_t secondsColor = look_.timeColor ? look_.timeColor : faceColors_.seconds;
    const uint32_t suffixColor = look_.timeColor ? look_.timeColor : faceColors_.suffix;
    const uint32_t dateColor = look_.dateColor ? look_.dateColor : faceColors_.date;
    const int timeWeight = look_.weight ? look_.weight : f.time.weight;

    // The time: before / the seconds / after the minutes, so small seconds are their own range.
    const bool formatSeconds = FormatHasSeconds(rawFormat_);
    std::wstring a, sec, b, secLine;
    if (rawSeconds_ && !formatSeconds && f.seconds != ClockFace::Seconds::Inline) {
        size_t after = MinutesEnd(rawFormat_);
        if (after == std::wstring::npos) after = rawFormat_.size();
        a = FormatRaw(rawFormat_.substr(0, after), t);
        b = FormatRaw(rawFormat_.substr(after), t);
        if (f.seconds == ClockFace::Seconds::Small) sec = FormatRaw(L":%S", t);
        if (f.seconds == ClockFace::Seconds::Line) secLine = FormatRaw(L"%S", t);
    } else {
        a = FormatRaw(rawSeconds_ && !formatSeconds ? EffectiveFormat(rawFormat_, true) : rawFormat_, t);
    }
    if (a.empty() && b.empty()) a = Format(L"%H:%M", t);
    if (!f.timeSeparator.empty()) {
        ReplaceAll(a, L':', f.timeSeparator);
        ReplaceAll(sec, L':', f.timeSeparator);
        ReplaceAll(b, L':', f.timeSeparator);
    }
    std::wstring time = ApplyCase(a + sec + b, f.time.textCase);
    size_t secStart = a.size(), secLen = sec.size();
    // The 12-hour suffix (the locale's AM / PM designator, as %p formats it).
    size_t sufStart = std::wstring::npos, sufLen = 0;
    const std::wstring p = ApplyCase(FormatRaw(L"%p", t), f.time.textCase);
    if (!p.empty() && f.suffix != ClockFace::Suffix::AsIs) {
        const size_t at = time.rfind(p);
        if (at != std::wstring::npos && (at >= secStart + secLen || at + p.size() <= secStart)) {
            if (f.suffix == ClockFace::Suffix::None) {
                size_t e0 = at, e1 = at + p.size();
                if (e0 > 0 && time[e0 - 1] == L' ') {
                    --e0;
                } else if (e1 < time.size() && time[e1] == L' ') {
                    ++e1;
                }
                time.erase(e0, e1 - e0);
                if (e1 <= secStart) secStart -= e1 - e0;
            } else if (f.suffix == ClockFace::Suffix::Lower) {
                time.replace(at, p.size(), ApplyCase(p, ClockFace::Case::Lower));
            } else {
                sufStart = at;
                sufLen = p.size();
            }
        }
    }
    FaceLine timeLine;
    timeLine.text = time;
    timeLine.font = font(f.time.font);
    timeLine.weight = timeWeight;
    timeLine.size = timeDip;
    timeLine.color = timeColor;
    if (secLen) {
        timeLine.ranges.push_back(FaceRange{static_cast<UINT32>(secStart), static_cast<UINT32>(secLen), rangeFont(f.secondsText.font),
                                            f.secondsText.weight, timeDip * f.secondsText.size, secondsColor});
    }
    if (sufLen) {
        timeLine.ranges.push_back(FaceRange{static_cast<UINT32>(sufStart), static_cast<UINT32>(sufLen), rangeFont(f.suffixText.font),
                                            f.suffixText.weight, timeDip * f.suffixText.size, suffixColor});
    }

    FaceLine dateLine;
    if (SecondLine()) {
        dateLine.text = DateLine(showDate_ ? ApplyCase(FormatRaw(f.dateFormat, t), f.date.textCase) : std::wstring());
        dateLine.font = font(f.date.font);
        dateLine.weight = f.date.weight;
        dateLine.size = dateDip;
        dateLine.color = dateColor;
    }
    FaceLine secondsLine;
    if (!secLine.empty()) {
        secondsLine.text = secLine;
        secondsLine.font = font(pick(f.secondsText.font, f.time.font));
        secondsLine.weight = f.secondsText.weight ? f.secondsText.weight : timeWeight;
        secondsLine.size = timeDip * f.secondsText.size;
        secondsLine.color = secondsColor;
    }

    std::vector<FaceLine> lines;
    // On a vertical dock an inline face stacks (its words go one per line anyway).
    const ClockFace::Layout layout = stacked_ && f.layout == ClockFace::Layout::Inline ? ClockFace::Layout::Stacked : f.layout;
    if (layout == ClockFace::Layout::Inline && !dateLine.text.empty()) {
        const UINT32 at = static_cast<UINT32>(timeLine.text.size());
        timeLine.text += f.separator + dateLine.text;
        timeLine.ranges.push_back(FaceRange{at, static_cast<UINT32>(f.separator.size() + dateLine.text.size()), rangeFont(f.date.font),
                                            f.date.weight, dateDip, dateColor});
        dateLine.text.clear();
    }
    if (layout == ClockFace::Layout::DateFirst && !dateLine.text.empty()) lines.push_back(dateLine);
    lines.push_back(timeLine);
    if (!secondsLine.text.empty()) lines.push_back(secondsLine);
    if (layout != ClockFace::Layout::DateFirst && !dateLine.text.empty()) lines.push_back(dateLine);
    return lines;
}

std::wstring Clock::Format(const std::wstring& format, const tm& t) {
    if (format.empty()) return {};
    wchar_t buf[256];
    auto previous = _set_thread_local_invalid_parameter_handler(IgnoreInvalidParameter);
    size_t n = wcsftime(buf, std::size(buf), format.c_str(), &t);
    _set_thread_local_invalid_parameter_handler(previous);
    if (n == 0) {
        // Invalid specifier or overflow: fall back to the default rather than show nothing.
        n = wcsftime(buf, std::size(buf), kFallbackFormat, &t);
    }
    return std::wstring(buf, n);
}

bool Clock::FormatHasSeconds(const std::wstring& format) {
    // Any specifier whose output includes seconds needs a 1 s tick.
    for (size_t i = 0; i + 1 < format.size(); ++i) {
        if (format[i] != L'%') continue;
        size_t j = i + 1;
        if (format[j] == L'#' && j + 1 < format.size()) ++j;  // MSVC '#' flag
        switch (format[j]) {
            case L'S': case L'T': case L'X': case L'c': case L'r': return true;
            default: break;
        }
        i = j;  // also skips "%%"
    }
    return false;
}

std::wstring Clock::EffectiveFormat(const std::wstring& format, bool showSeconds) {
    if (!showSeconds || format.empty() || FormatHasSeconds(format)) return format;
    size_t after = std::wstring::npos;  // just past the last minutes specifier
    for (size_t i = 0; i + 1 < format.size(); ++i) {
        if (format[i] != L'%') continue;
        size_t j = i + 1;
        if (format[j] == L'#' && j + 1 < format.size()) ++j;
        if (format[j] == L'M' || format[j] == L'R') after = j + 1;
        i = j;
    }
    if (after == std::wstring::npos) return format + L":%S";
    return format.substr(0, after) + L":%S" + format.substr(after);
}

void Clock::ScheduleNextTick(HWND hwnd, UINT_PTR timerId) const {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    UINT delay;
    // A face that hides the seconds ticks once a minute (unless clock.format itself shows them).
    const bool seconds = face_ && face_->seconds == ClockFace::Seconds::Hidden ? FormatHasSeconds(rawFormat_)
                                                                                : FormatHasSeconds(format_);
    if (seconds) {
        delay = 1000u - st.wMilliseconds;
    } else {
        delay = (59u - st.wSecond) * 1000u + (1000u - st.wMilliseconds);
    }
    // Small slack so the tick lands just after the boundary, never just before it.
    delay += 15;
    SetTimer(hwnd, timerId, delay, nullptr);
}

void Clock::Stop(HWND hwnd, UINT_PTR timerId) { KillTimer(hwnd, timerId); }

bool Clock::UpdateText() {
    tm local{};
    if (!Now(local)) return false;

    if (face_) {
        std::vector<FaceLine> lines = FaceTexts(local);
        bool same = lines.size() == faceLines_.size();
        for (size_t i = 0; same && i < lines.size(); ++i) same = lines[i].SameText(faceLines_[i]);
        if (same) return false;
        // Keep the text formats (same fonts and sizes): only the layouts are rebuilt.
        for (size_t i = 0; i < lines.size() && i < faceLines_.size(); ++i) {
            lines[i].format = std::move(faceLines_[i].format);
            lines[i].formatKey = std::move(faceLines_[i].formatKey);
        }
        faceLines_ = std::move(lines);
        return true;
    }

    std::wstring time = Format(format_, local);
    std::wstring date = SecondLine() ? DateLine(showDate_ ? Format(kDateFormat, local) : std::wstring()) : std::wstring();
    if (time == timeText_ && date == dateText_) return false;
    timeText_ = std::move(time);
    dateText_ = std::move(date);
    timeLayout_ = nullptr;
    dateLayout_ = nullptr;
    return true;
}

void Clock::ReleaseLayout() {
    timeLayout_ = nullptr;
    dateLayout_ = nullptr;
    timeFormat_ = nullptr;
    dateFormat_ = nullptr;
    for (FaceLine& l : faceLines_) {
        l.layout = nullptr;
        l.format = nullptr;
    }
    scale_ = 0.0f;
    size_ = {};
}

D2D1_SIZE_U Clock::BuildLayout(IDWriteFactory* dwrite, float scale) {
    size_ = {};
    timeHeight_ = 0.0f;
    if (!dwrite) return size_;
    if (face_) return BuildFaceLayout(dwrite, scale);

    if (scale != scale_) {
        timeLayout_ = nullptr;
        dateLayout_ = nullptr;
        timeFormat_ = nullptr;
        dateFormat_ = nullptr;
        scale_ = scale;
    }

    // Round 20 phase 20A: the item's look (Look) over the digital look: its size factor, font
    // family and the time's weight.
    const float timeSize = (SecondLine() ? 14.0f : 16.0f) * scale * look_.scale;
    const wchar_t* family = look_.font.empty() ? kFontFamily : look_.font.c_str();
    const DWRITE_FONT_WEIGHT timeWeight = look_.weight ? static_cast<DWRITE_FONT_WEIGHT>(look_.weight) : DWRITE_FONT_WEIGHT_SEMI_BOLD;
    if (!timeFormat_ &&
        FAILED(dwrite->CreateTextFormat(family, nullptr, timeWeight, DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, timeSize, L"", timeFormat_.put()))) {
        return size_;
    }
    if (SecondLine() && !dateFormat_ &&
        FAILED(dwrite->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, 11.0f * scale * look_.scale, L"", dateFormat_.put()))) {
        return size_;
    }

    // Stacked (vertical docks): one word per line.
    auto shown = [this](const std::wstring& text) {
        std::wstring out = text;
        if (stacked_) std::replace(out.begin(), out.end(), L' ', L'\n');
        return out;
    };
    if (!timeLayout_ && !timeText_.empty()) {
        const std::wstring text = shown(timeText_);
        if (FAILED(dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), timeFormat_.get(), 4096.0f,
                                            4096.0f, timeLayout_.put()))) {
            timeLayout_ = nullptr;
        }
    }
    if (SecondLine() && !dateLayout_ && !dateText_.empty()) {
        const std::wstring text = shown(dateText_);
        if (FAILED(dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), dateFormat_.get(), 4096.0f,
                                            4096.0f, dateLayout_.put()))) {
            dateLayout_ = nullptr;
        }
    }

    float width = 0.0f, height = 0.0f;
    DWRITE_TEXT_METRICS m{};
    if (timeLayout_ && SUCCEEDED(timeLayout_->GetMetrics(&m))) {
        width = std::max(width, m.widthIncludingTrailingWhitespace);
        timeHeight_ = m.height;
        height += m.height;
    }
    if (dateLayout_ && SUCCEEDED(dateLayout_->GetMetrics(&m))) {
        width = std::max(width, m.widthIncludingTrailingWhitespace);
        height += m.height;
    }

    // Center each line within the block.
    const float blockWidth = std::ceil(width);
    if (timeLayout_) {
        timeLayout_->SetMaxWidth(blockWidth);
        timeLayout_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    if (dateLayout_) {
        dateLayout_->SetMaxWidth(blockWidth);
        dateLayout_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    size_ = D2D1::SizeU(static_cast<UINT32>(blockWidth), static_cast<UINT32>(std::ceil(height)));
    return size_;
}

D2D1_SIZE_U Clock::BuildFaceLayout(IDWriteFactory* dwrite, float scale) {
    const ClockFace& f = *face_;
    if (scale != scale_) {
        for (FaceLine& l : faceLines_) {
            l.layout = nullptr;
            l.format = nullptr;
        }
        scale_ = scale;
    }
    if (faceLines_.empty()) UpdateText();  // SetStacked dropped them
    const bool stack = stacked_ && f.verticalStack;
    const DWRITE_TEXT_ALIGNMENT align = AlignOf(stacked_ ? f.verticalAlign : f.align);
    float width = 0.0f;
    for (FaceLine& l : faceLines_) {
        const std::wstring key = l.font + L"|" + std::to_wstring(l.weight) + L"|" + std::to_wstring(l.size * scale);
        if (!l.format || l.formatKey != key) {
            l.format = nullptr;
            l.layout = nullptr;
            if (FAILED(dwrite->CreateTextFormat(l.font.c_str(), nullptr, static_cast<DWRITE_FONT_WEIGHT>(l.weight),
                                                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, l.size * scale, L"",
                                                l.format.put()))) {
                l.format = nullptr;
                continue;
            }
            l.formatKey = key;
        }
        if (!l.layout && !l.text.empty()) {
            std::wstring text = l.text;  // one character for one: the ranges stay valid
            if (stack) std::replace(text.begin(), text.end(), L' ', L'\n');
            if (FAILED(dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), l.format.get(), 4096.0f,
                                                4096.0f, l.layout.put()))) {
                l.layout = nullptr;
                continue;
            }
            for (const FaceRange& r : l.ranges) {
                const DWRITE_TEXT_RANGE range{r.start, r.length};
                if (!r.font.empty()) l.layout->SetFontFamilyName(r.font.c_str(), range);
                if (r.weight) l.layout->SetFontWeight(static_cast<DWRITE_FONT_WEIGHT>(r.weight), range);
                if (r.size > 0.0f) l.layout->SetFontSize(r.size * scale, range);
            }
        }
        DWRITE_TEXT_METRICS m{};
        if (l.layout && SUCCEEDED(l.layout->GetMetrics(&m))) {
            width = std::max(width, m.widthIncludingTrailingWhitespace);
            l.height = m.height;
        } else {
            l.height = 0.0f;
        }
    }
    // The lines one under the other (lineGap between them), aligned inside the widest.
    const float blockWidth = std::ceil(width);
    float y = 0.0f;
    const FaceLine* last = nullptr;
    const float k = scale * look_.scale;  // round 20 phase 20A: the look's size factor on the face's DIPs
    for (FaceLine& l : faceLines_) {
        if (!l.layout) continue;
        if (last) y += f.lineGap * k;
        l.layout->SetMaxWidth(blockWidth);
        l.layout->SetTextAlignment(align);
        l.y = y;
        y += l.height;
        last = &l;
    }
    padX_ = f.background ? std::round(f.padX * k) : 0.0f;
    padY_ = f.background ? std::round(f.padY * k) : 0.0f;
    faceLastCentre_ = padY_ + (last ? last->y + last->height / 2.0f : 0.0f);
    if (blockWidth <= 0.0f) return size_;
    size_ = D2D1::SizeU(static_cast<UINT32>(blockWidth + 2.0f * padX_), static_cast<UINT32>(std::ceil(y) + 2.0f * padY_));
    return size_;
}

void Clock::DrawFace(ID2D1DeviceContext* dc, float x, float y) const {
    const ClockFace& f = *face_;
    winrt::com_ptr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(Theme::ToColor(faceColors_.time), brush.put()))) return;
    if (f.background) {
        const float bw = (faceColors_.border >> 24) ? std::max(0.0f, std::round(f.borderWidth * scale_)) : 0.0f;
        const float r = f.radius * scale_ * look_.scale;  // round 20 phase 20A: the pill's corners grow with the text
        const D2D1_RECT_F box = D2D1::RectF(x + bw / 2.0f, y + bw / 2.0f, x + static_cast<float>(size_.width) - bw / 2.0f,
                                            y + static_cast<float>(size_.height) - bw / 2.0f);
        brush->SetColor(Theme::ToColor(faceColors_.fill));
        dc->FillRoundedRectangle(D2D1::RoundedRect(box, r, r), brush.get());
        if (bw > 0.0f) {
            brush->SetColor(Theme::ToColor(faceColors_.border));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(box, r, r), brush.get(), bw);
        }
    }
    for (const FaceLine& l : faceLines_) {
        if (!l.layout) continue;
        // Ranges in another colour: a brush as the range's drawing effect (cleared again after).
        std::vector<winrt::com_ptr<ID2D1SolidColorBrush>> effects;
        for (const FaceRange& r : l.ranges) {
            if (r.color == l.color) continue;
            winrt::com_ptr<ID2D1SolidColorBrush> b;
            if (FAILED(dc->CreateSolidColorBrush(Theme::ToColor(r.color), b.put()))) continue;
            l.layout->SetDrawingEffect(b.get(), DWRITE_TEXT_RANGE{r.start, r.length});
            effects.push_back(std::move(b));
        }
        brush->SetColor(Theme::ToColor(l.color));
        dc->DrawTextLayout(D2D1::Point2F(x + padX_, y + padY_ + l.y), l.layout.get(), brush.get());
        if (!effects.empty()) {
            for (const FaceRange& r : l.ranges) l.layout->SetDrawingEffect(nullptr, DWRITE_TEXT_RANGE{r.start, r.length});
        }
    }
}

void Clock::Draw(ID2D1DeviceContext* dc, float x, float y) const {
    if (face_) {
        DrawFace(dc, x, y);
        return;
    }
    winrt::com_ptr<ID2D1SolidColorBrush> brush;
    // Round 20 phase 20A: the look's colours over the theme's.
    if (FAILED(dc->CreateSolidColorBrush(Theme::ToColor(look_.timeColor ? look_.timeColor : textColor_), brush.put()))) return;
    if (timeLayout_) dc->DrawTextLayout(D2D1::Point2F(x, y), timeLayout_.get(), brush.get());
    if (dateLayout_) {
        brush->SetColor(Theme::ToColor(look_.dateColor ? look_.dateColor : dateColor_));
        dc->DrawTextLayout(D2D1::Point2F(x, y + timeHeight_), dateLayout_.get(), brush.get());
    }
}
