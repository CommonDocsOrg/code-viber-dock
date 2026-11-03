#pragma once

#include <windows.h>

// The acrylic blur behind the bar: a second top-level window, exactly the size of the drawn bar,
// kept directly beneath the dock window in z-order. DWM fills it with a system backdrop
// (DWMWA_SYSTEMBACKDROP_TYPE); the dock window draws the tint, hairline and content on top.
// It never takes input: the dock window above it always covers it.
//
// Recipe that works on Windows 11 (verified on build 26300): WS_POPUP | WS_THICKFRAME with the
// non-client area removed (WM_NCCALCSIZE -> 0) and WS_EX_NOREDIRECTIONBITMAP (nothing paints the
// client area, so the backdrop shows through). DWM draws a system backdrop only for windows whose
// frame is "active", so the window answers every WM_NCACTIVATE as active. Without the frame style
// or the forced activation DWM shows the inactive fallback colour instead of the blur.
class Backdrop {
public:
    static constexpr wchar_t kClassName[] = L"WinDockBackdrop";

    Backdrop() = default;
    Backdrop(const Backdrop&) = delete;
    Backdrop& operator=(const Backdrop&) = delete;
    ~Backdrop() { Destroy(); }

    // Creates the (hidden) window and applies the system backdrop. Returns false if this build
    // of Windows does not support system backdrops (the window is not kept then).
    // `mouseTransparent`: layered + WS_EX_TRANSPARENT (fully opaque), so the mouse passes through
    // it to other apps' windows too (a mouse-transparent popup's blur).
    bool Create(HINSTANCE instance, bool mouseTransparent = false);
    void Destroy();
    bool Exists() const { return hwnd_ != nullptr; }
    HWND Hwnd() const { return hwnd_; }

    // Picks the DWM corner preference closest to (not above) `radiusDip` and returns the radius
    // in DIPs that DWM will round the blur with (0, 4 or 8); the bar tint should use the same.
    float SetCornerRadius(float radiusDip);

    // Shows the window at `rect` (screen) directly beneath `above`. An empty rect hides it.
    void Place(HWND above, const RECT& rect);
    // Re-asserts the position directly beneath `above` (after `above` changed z-order).
    void Restack(HWND above);
    void Hide();
    bool Visible() const { return visible_; }

    // True when Windows renders backdrops at all: "Transparency effects" on and no high contrast.
    static bool SystemAllowsTransparency();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    HWND hwnd_ = nullptr;
    RECT rect_{};
    bool visible_ = false;
    bool activated_ = false;
    int corner_ = -1;  // DWM_WINDOW_CORNER_PREFERENCE last applied
};
