#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>

#include "PopupWindow.h"
#include "Renderer.h"

// A month calendar above the clock: a click popup (PopupWindow, class WinDockCalendar). Header:
// today's long date (the locale's format), then "Month YYYY" with previous / next arrows; the
// weekday initials from the locale (LOCALE_SABBREVDAYNAME*, starting on LOCALE_IFIRSTDAYOFWEEK);
// a 6-week grid with the days of the neighbouring months dimmed and today on an accent disc.
// Clicking the month title, or Home, returns to the current month; Left / Right and Page Up /
// Page Down change the month. No events. The grid always has 6 rows, so a month change is a
// redraw in place (no resize).
class CalendarPopup : public PopupWindow {
public:
    static constexpr wchar_t kClassName[] = L"WinDockCalendar";

    struct Style {
        float scale = 1.0f;
        bool acrylic = false;
        uint32_t tint = 0xCC202020;
        uint32_t text = 0xFFFFFFFF;
        uint32_t secondary = 0xB6FFFFFF;
        uint32_t border = 0x24FFFFFF;
        uint32_t hotBackground = 0x1AFFFFFF;
        uint32_t hotBorder = 0x33FFFFFF;
        uint32_t accent = 0xFF4CC2FF;  // today
        float cornerRadiusDip = 8.0f;
        RECT monitor{};
    };

    CalendarPopup() : PopupWindow(kClassName, L"WinDock calendar", Mode::Click) {}
    ~CalendarPopup() override { Destroy(); }

    // `closed`: the popup closed by itself (deactivated, Escape).
    void Init(HINSTANCE instance, Renderer* renderer, std::function<void()> closed);
    // Shows the current month centred on screen x `anchorX`, bottom edge at screen y `bottom`.
    bool Show(const Style& style, int anchorX, int bottom);
    // Months between the month shown and the current one (0 while hidden).
    int MonthOffset() const;

protected:
    SIZE Measure() override;
    void Paint(ID2D1DeviceContext* dc) override;
    int HitAt(POINT pt, int* part) const override;
    void OnClick(int index, int part, Button button) override;
    void OnDestroying() override;
    bool OnMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) override;

private:
    static constexpr int kHitPrev = 1;
    static constexpr int kHitNext = 2;
    static constexpr int kHitMonth = 3;

    // Moves the month shown by `months` (0: back to the current month) and redraws.
    void Shift(int months);
    void EnsureFormats();

    Style style_;
    Frame frame_;
    SYSTEMTIME today_{};
    int year_ = 2000;
    int month_ = 1;      // 1 - 12
    int firstDay_ = 0;   // first day of the week, 0 = Monday (LOCALE_IFIRSTDAYOFWEEK)

    RECT prevRect_{};
    RECT nextRect_{};
    RECT monthRect_{};

    float formatScale_ = 0.0f;
    winrt::com_ptr<IDWriteTextFormat> titleFormat_;  // today's date
    winrt::com_ptr<IDWriteTextFormat> monthFormat_;  // "November 2026"
    winrt::com_ptr<IDWriteTextFormat> dayFormat_;    // day numbers
    winrt::com_ptr<IDWriteTextFormat> headFormat_;   // weekday names
};
