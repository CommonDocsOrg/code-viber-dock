#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "CalendarService.h"
#include "ItemGridView.h"
#include "PopupWindow.h"
#include "Renderer.h"

// A month calendar above the clock: a click popup (PopupWindow, class WinDockCalendar). Header:
// today's long date (the locale's format), then "Month YYYY" with previous / next arrows; the
// weekday initials from the locale (LOCALE_SABBREVDAYNAME*, starting on LOCALE_IFIRSTDAYOFWEEK);
// a 6-week grid with the days of the neighbouring months dimmed and today on an accent disc.
// Clicking the month title, or Home, returns to the current month; Left / Right and Page Up /
// Page Down change the month. The grid always has 6 rows, so a month change is a redraw in place.
// With a footer text (clock.clickTarget is set) a button under the grid opens the target.
//
// Events (round 4 phase 13), while the calendar service runs (some feed is enabled): days with
// events get up to three dots in their feeds' colours under the number (calendars.showDots);
// clicking a day selects it (a day of a neighbouring month shows that month) and its events are
// listed under the grid (an ItemGridView in list mode: the feed's colour bar, the title, the start
// time or "All day"; all-day and continuing events first), today's when the popup opens; the
// popup grows with the list (up to six rows, then it scrolls) and shrinks again. Hovering or
// selecting (Up / Down) an event shows its time, feed, location and description in a strip under
// the list; a click or Enter opens the event's URL (http / https only), else nothing. Without a
// running service the popup is exactly the month grid it was before.
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
        std::wstring footer;  // the footer button's text (empty: no footer)
        bool dots = true;     // calendars.showDots
        bool reduceMotion = false;
    };

    struct Callbacks {
        std::function<void()> closed;  // closed by itself (deactivated, Escape)
        std::function<void()> footer;  // the footer button
        std::function<void(const std::wstring& url)> openUrl;  // an event with a URL was clicked
    };

    CalendarPopup() : PopupWindow(kClassName, L"WinDock calendar", Mode::Click) {}
    ~CalendarPopup() override { Destroy(); }

    void Init(HINSTANCE instance, Renderer* renderer, Callbacks callbacks);
    // Shows the month of `day` (local days since 1970; INT64_MIN: today) at `anchor`, that day
    // selected. `service` may be null (no events).
    bool Show(const Style& style, const PopupAnchor& anchor, CalendarService* service, int64_t day = INT64_MIN);
    // The service's data changed: dots and the list follow (the popup may resize).
    void Update();
    // Months between the month shown and the current one (0 while hidden).
    int MonthOffset() const;

    // --- automation ---
    // Selects `day` (shows its month). False while hidden.
    bool SelectDay(int64_t day);
    int64_t SelectedDay() const { return OnScreen() && Events() ? selected_ : INT64_MIN; }
    size_t Rows() const { return rows_.size(); }  // events listed for the selected day
    int DotsDrawn() const { return dotsDrawn_; }  // dots in the grid at the last paint
    int HotRow() const { return view_.Hot(); }
    int DetailShown() const { return lastDetail_; }  // the row the strip describes (-1 none)

protected:
    SIZE Measure() override;
    void Paint(ID2D1DeviceContext* dc) override;
    int HitAt(POINT pt, int* part) const override;
    void OnClick(int index, int part, Button button) override;
    void OnHidden() override;
    void OnDestroying() override;
    bool OnMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) override;

private:
    static constexpr int kHitPrev = 1;
    static constexpr int kHitNext = 2;
    static constexpr int kHitMonth = 3;
    static constexpr int kHitFooter = 4;
    static constexpr int kHitDayBase = 100;  // + grid cell 0 - 41
    static constexpr UINT_PTR kViewTimerBase = 100;

    // Moves the month shown by `months` (0: back to the current month, today selected) and redraws.
    void Shift(int months);
    void EnsureFormats();
    bool Events() const { return service_ && service_->Active(); }
    // The first day (local days) of the 6-week grid of the month shown.
    int64_t GridFirst() const;
    // Refills the list for the selected day; `relayout`: measure and place again (size changes).
    void Fill();
    void Relayout();
    ItemGridView::Style ViewStyle() const;
    ID2D1Bitmap* BarBitmap(uint32_t color);
    int DetailRow() const;  // the row the strip describes (-1 none)

    Style style_;
    Frame frame_;
    PopupAnchor anchor_{};
    Callbacks callbacks_;
    CalendarService* service_ = nullptr;
    SYSTEMTIME today_{};
    int year_ = 2000;
    int month_ = 1;      // 1 - 12
    int firstDay_ = 0;   // first day of the week, 0 = Monday (LOCALE_IFIRSTDAYOFWEEK)
    int64_t selected_ = INT64_MIN;
    int dotsDrawn_ = 0;

    RECT prevRect_{};
    RECT nextRect_{};
    RECT monthRect_{};
    RECT footerRect_{};  // empty without a footer
    RECT dateRect_{};    // the selected day's header (events only)
    RECT viewRect_{};    // the list
    RECT emptyRect_{};   // "No events"
    RECT detailRect_{};  // the strip under the list

    ItemGridView view_;
    bool viewReady_ = false;
    std::vector<size_t> rows_;  // indexes into service_->Items()
    int lastDetail_ = -1;
    std::map<uint32_t, winrt::com_ptr<ID2D1Bitmap>> bars_;

    float formatScale_ = 0.0f;
    winrt::com_ptr<IDWriteTextFormat> titleFormat_;  // today's date
    winrt::com_ptr<IDWriteTextFormat> monthFormat_;  // "November 2026"
    winrt::com_ptr<IDWriteTextFormat> dayFormat_;    // day numbers
    winrt::com_ptr<IDWriteTextFormat> headFormat_;   // weekday names
    winrt::com_ptr<IDWriteTextFormat> noteFormat_;   // the date header, "No events", the strip
};
