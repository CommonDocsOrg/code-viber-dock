#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>

#include "CalendarService.h"
#include "PopupWindow.h"
#include "Renderer.h"

// A calendar reminder (round 4 phase 13b): a banner beside the dock (PopupWindow in the hover
// model: it never takes the focus or the foreground, so it does not disturb what the user types),
// class WinDockReminder. The feed's colour bar, the event's title, "In 15 min · 10:00 AM" (or
// "Now"), the location, "+n more" when more reminders wait, and two buttons: Snooze 5 min and
// Dismiss. A click elsewhere on it dismisses it and opens the month calendar on the event's day.
// It stays until a button is pressed; nothing animates or ticks while it is shown.
class ReminderBanner : public PopupWindow {
public:
    static constexpr wchar_t kClassName[] = L"WinDockReminder";

    struct Style {
        float scale = 1.0f;
        bool acrylic = false;
        uint32_t tint = 0xCC202020;
        uint32_t text = 0xFFFFFFFF;
        uint32_t secondary = 0xB6FFFFFF;
        uint32_t border = 0x24FFFFFF;
        uint32_t hotBackground = 0x1AFFFFFF;
        uint32_t hotBorder = 0x33FFFFFF;
        float cornerRadiusDip = 8.0f;
        RECT monitor{};
    };
    struct Callbacks {
        std::function<void()> snooze;
        std::function<void()> dismiss;
        std::function<void()> open;  // the banner's body
    };

    ReminderBanner() : PopupWindow(kClassName, L"WinDock reminder", Mode::Hover) {}
    ~ReminderBanner() override { Destroy(); }

    void Init(HINSTANCE instance, Renderer* renderer, Callbacks callbacks);
    // Shows (or replaces) the reminder; `more`: how many wait behind it.
    bool Show(const Style& style, const PopupAnchor& anchor, const CalendarService::ReminderInfo& reminder, size_t more);
    const CalendarService::ReminderInfo& Current() const { return info_; }

protected:
    SIZE Measure() override;
    void Paint(ID2D1DeviceContext* dc) override;
    int HitAt(POINT pt, int* part) const override;
    void OnClick(int index, int part, Button button) override;
    void OnDestroying() override;

private:
    static constexpr int kHitBody = 1;
    static constexpr int kHitSnooze = 2;
    static constexpr int kHitDismiss = 3;
    void EnsureFormats();

    Style style_;
    Frame frame_;
    Callbacks callbacks_;
    CalendarService::ReminderInfo info_;
    std::wstring when_;
    size_t more_ = 0;
    RECT snoozeRect_{}, dismissRect_{};
    float formatScale_ = 0.0f;
    winrt::com_ptr<IDWriteTextFormat> titleFormat_, textFormat_, buttonFormat_;
};
