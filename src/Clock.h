#pragma once

#include <Unknwn.h>
#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>

#include <winrt/base.h>

#include <cstdint>
#include <ctime>
#include <string>

// Formats the current time with a strftime-style format and lays it out with DirectWrite.
// Timer scheduling is a one-shot SetTimer aligned to the next minute (or second) boundary.
class Clock {
public:
    void Configure(const std::wstring& format, bool showDate);
    // ARGB colours of the time and date lines (theme.clockText / clockDateText).
    void SetColors(uint32_t text, uint32_t date) {
        textColor_ = text;
        dateColor_ = date;
    }

    // Starts/restarts the aligned one-shot timer on hwnd. Call again from WM_TIMER.
    void ScheduleNextTick(HWND hwnd, UINT_PTR timerId) const;
    static void Stop(HWND hwnd, UINT_PTR timerId);

    // Re-formats the text. Returns true if it changed (layout must be rebuilt).
    bool UpdateText();

    // Builds text layouts for the current text at the given DPI scale. Returns the
    // pixel size of the text block (0x0 if the text is empty or on failure).
    D2D1_SIZE_U BuildLayout(IDWriteFactory* dwrite, float scale);
    void Draw(ID2D1DeviceContext* dc, float x, float y) const;

    void ReleaseLayout();

    // strftime-style formatting that never terminates on a bad specifier (falls back to %H:%M).
    static std::wstring Format(const std::wstring& format, const tm& t);
    // The format shows seconds (%S, %T, %X, %c, %r): the clock ticks every second.
    static bool FormatHasSeconds(const std::wstring& format);
    // clock.format with clock.showSeconds applied: ":%S" right after the last minutes specifier
    // (%M, %#M, %R; none: at the end), unless the format shows seconds already.
    static std::wstring EffectiveFormat(const std::wstring& format, bool showSeconds);

private:

    std::wstring format_ = L"%H:%M";
    bool showDate_ = false;
    uint32_t textColor_ = 0xF2FFFFFF;
    uint32_t dateColor_ = 0xB6FFFFFF;
    std::wstring timeText_;
    std::wstring dateText_;

    float scale_ = 0.0f;
    winrt::com_ptr<IDWriteTextFormat> timeFormat_;
    winrt::com_ptr<IDWriteTextFormat> dateFormat_;
    winrt::com_ptr<IDWriteTextLayout> timeLayout_;
    winrt::com_ptr<IDWriteTextLayout> dateLayout_;
    D2D1_SIZE_U size_{};
    float timeHeight_ = 0.0f;
};
