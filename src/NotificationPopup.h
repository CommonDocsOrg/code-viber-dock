#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ItemGridView.h"
#include "NotificationService.h"
#include "PopupWindow.h"
#include "Renderer.h"

// The notification centre (round 4 phase 9b): a click popup (PopupWindow, class
// WinDockNotifications) over the dock's `notifications` element (or a widget that calls
// notifications.openCentre()). A header ("Notifications", the unread count, "Clear all"), then one
// row per notification of the NotificationService's list, newest first, in an ItemGridView in list
// mode with detail rows: the app's icon (the dock's icon cache), the title, two lines of text, the
// relative time; the hovered row shows a close glyph. A click on a row activates its app and
// dismisses it (the dock: Callbacks::activate), the close glyph / Delete dismisses it, "Clear all"
// dismisses every listed one. With no notifications: "No notifications"; with access denied: the
// reason and an "Open Windows settings" button. Escape / another window coming forward close it;
// Up / Down / Home / End select, Enter activates. The rows follow the list while it is open
// (Update, from the dock's change handler). Do not disturb: Windows has no public API to read or
// set it, so there is no toggle.
class NotificationPopup : public PopupWindow {
public:
    static constexpr wchar_t kClassName[] = L"WinDockNotifications";

    struct Style {
        float scale = 1.0f;
        bool acrylic = false;
        uint32_t tint = 0xCC202020;
        uint32_t text = 0xFFFFFFFF;
        uint32_t secondary = 0xB6FFFFFF;
        uint32_t border = 0x24FFFFFF;
        uint32_t hotBackground = 0x1AFFFFFF;
        uint32_t hotBorder = 0x33FFFFFF;
        uint32_t accent = 0xFF4CC2FF;
        float cornerRadiusDip = 8.0f;
        RECT monitor{};
        bool reduceMotion = false;
    };

    struct Callbacks {
        std::function<void()> closed;                  // closed by itself (deactivated, Escape)
        std::function<void(uint32_t id)> activate;     // a row was clicked / Enter
        std::function<void(uint32_t id)> dismiss;      // its close glyph / Delete
        std::function<void()> clearAll;                // "Clear all"
        std::function<void()> openSettings;            // "Open Windows settings" (access denied)
        // The app's icon for a row, `px` square (null: none); a bitmap of the dock's device.
        std::function<ID2D1Bitmap*(const NotificationItem& item, UINT px)> icon;
    };

    NotificationPopup() : PopupWindow(kClassName, L"WinDock notifications", Mode::Click) {}
    ~NotificationPopup() override { Destroy(); }

    void Init(HINSTANCE instance, Renderer* renderer, Callbacks callbacks);
    // Shows the list at `anchor` (beside the dock, away from its edge).
    bool Show(const Style& style, const PopupAnchor& anchor, const NotificationService& service);
    // The list changed: refills the rows in place (keeps the scroll position), resizing the popup.
    void Update(const NotificationService& service);

    // --- automation ---
    size_t Rows() const { return ids_.size(); }
    uint32_t IdAt(size_t row) const { return row < ids_.size() ? ids_[row] : 0; }
    bool DismissRow(size_t row);
    bool ActivateRow(size_t row);
    bool ClearAll();
    int Selected() const { return view_.Selected(); }
    // 0: rows, 1: the empty text, 2: the denied text.
    int Content() const { return message_; }

protected:
    SIZE Measure() override;
    void Paint(ID2D1DeviceContext* dc) override;
    int HitAt(POINT pt, int* part) const override;
    void OnClick(int index, int part, Button button) override;
    void OnHidden() override;
    void OnDestroying() override;
    bool OnMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) override;

private:
    static constexpr int kHitClear = 1;
    static constexpr int kHitSettings = 2;
    static constexpr UINT_PTR kViewTimerBase = 100;

    void Fill(const NotificationService& service);
    ItemGridView::Style ViewStyle() const;
    void EnsureFormats();
    static std::wstring RelativeTime(int64_t unixMs);

    Callbacks callbacks_;
    Style style_;
    Frame frame_;
    PopupAnchor anchor_{};
    ItemGridView view_;
    bool viewReady_ = false;
    std::vector<uint32_t> ids_;            // the rows' notification ids
    std::vector<NotificationItem> items_;  // a copy of the rows' notifications (icons resolve from it)
    size_t unread_ = 0;
    int message_ = 1;                      // see Content()
    std::wstring reason_;
    RECT headerRect_{};
    RECT clearRect_{};
    RECT viewRect_{};
    RECT messageRect_{};
    RECT settingsRect_{};
    float clearTextW_ = 0.0f;
    float settingsTextW_ = 0.0f;
    float formatScale_ = 0.0f;
    winrt::com_ptr<IDWriteTextFormat> titleFormat_;
    winrt::com_ptr<IDWriteTextFormat> buttonFormat_;
    winrt::com_ptr<IDWriteTextFormat> messageFormat_;
};
