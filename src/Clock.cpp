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

}  // namespace

void Clock::Configure(const std::wstring& format, bool showDate) {
    format_ = format;
    showDate_ = showDate;
    timeText_.clear();
    dateText_.clear();
    ReleaseLayout();
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
    if (FormatHasSeconds(format_)) {
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
    __time64_t now = _time64(nullptr);
    tm local{};
    if (_localtime64_s(&local, &now) != 0) return false;

    std::wstring time = Format(format_, local);
    std::wstring date = showDate_ ? Format(kDateFormat, local) : std::wstring();
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
    scale_ = 0.0f;
    size_ = {};
}

D2D1_SIZE_U Clock::BuildLayout(IDWriteFactory* dwrite, float scale) {
    size_ = {};
    timeHeight_ = 0.0f;
    if (!dwrite) return size_;

    if (scale != scale_) {
        timeLayout_ = nullptr;
        dateLayout_ = nullptr;
        timeFormat_ = nullptr;
        dateFormat_ = nullptr;
        scale_ = scale;
    }

    const float timeSize = (showDate_ ? 14.0f : 16.0f) * scale;
    if (!timeFormat_ &&
        FAILED(dwrite->CreateTextFormat(kFontFamily, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, timeSize, L"", timeFormat_.put()))) {
        return size_;
    }
    if (showDate_ && !dateFormat_ &&
        FAILED(dwrite->CreateTextFormat(kFontFamily, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, 11.0f * scale, L"", dateFormat_.put()))) {
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
    if (showDate_ && !dateLayout_ && !dateText_.empty()) {
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

void Clock::Draw(ID2D1DeviceContext* dc, float x, float y) const {
    winrt::com_ptr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(Theme::ToColor(textColor_), brush.put()))) return;
    if (timeLayout_) dc->DrawTextLayout(D2D1::Point2F(x, y), timeLayout_.get(), brush.get());
    if (dateLayout_) {
        brush->SetColor(Theme::ToColor(dateColor_));
        dc->DrawTextLayout(D2D1::Point2F(x, y + timeHeight_), dateLayout_.get(), brush.get());
    }
}
