#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>

#include "PopupWindow.h"
#include "Renderer.h"

// A popup a script widget draws (popup.open / drawPopup, docs/widgets.md): a click popup
// (PopupWindow, class WinDockWidgetPopup) of the size the script asked for, anchored on the
// widget's slot beside the dock like the dock's own popups, with the theme's panel behind the
// script's drawing. Clicks go back to the script (onPopupClick) with DIP coordinates; it closes on
// Escape, when another window comes forward, on a dock click and by popup.close().
// The passive kind (popup.open(w, h, { passive: true }), class WinDockWidgetPassivePopup) is the
// same drawing in a hover-mode window made mouse-transparent: it never takes the focus and never
// sees the mouse (a level bar beside the dock while the wheel turns); the dock alone closes it.
class ScriptPopup : public PopupWindow {
public:
    static constexpr wchar_t kClassName[] = L"WinDockWidgetPopup";
    static constexpr wchar_t kPassiveClassName[] = L"WinDockWidgetPassivePopup";

    struct Callbacks {
        // Draw the script's content (DIPs w x h at `scale`; the panel is drawn already).
        std::function<void(ID2D1DeviceContext* dc, float w, float h, float scale)> paint;
        std::function<void(float x, float y, int button)> click;  // button: 0 left, 1 middle, 2 right
        std::function<void()> closed;                              // closed by itself (Escape, deactivated)
    };
    struct Style {
        float scale = 1.0f;
        bool acrylic = false;
        uint32_t tint = 0xCC202020;
        uint32_t border = 0x24FFFFFF;
        float cornerRadiusDip = 8.0f;
        RECT monitor{};
    };

    explicit ScriptPopup(Mode mode = Mode::Click, const wchar_t* className = kClassName)
        : PopupWindow(className, mode == Mode::Click ? L"WinDock widget popup" : L"WinDock widget passive popup", mode) {}
    ~ScriptPopup() override { Destroy(); }

    void Init(HINSTANCE instance, Renderer* renderer, Callbacks callbacks);
    // Shows (or resizes) the popup: w x h DIPs at `anchor`.
    bool Show(float w, float h, const Style& style, const PopupAnchor& anchor);
    // Draws the content again (popup.redraw()).
    void Repaint();
    float WidthDip() const { return w_; }
    float HeightDip() const { return h_; }

protected:
    SIZE Measure() override;
    void Paint(ID2D1DeviceContext* dc) override;
    int HitAt(POINT pt, int* part) const override;
    void OnClick(int index, int part, Button button) override;
    void OnHotChanged() override {}  // one item, nothing to highlight

private:
    Callbacks callbacks_;
    Style style_;
    float w_ = 0.0f, h_ = 0.0f;
};
