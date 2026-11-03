#include "Backdrop.h"

#include <dwmapi.h>

#include <initializer_list>

namespace {

constexpr DWORD kExStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP;
// The thick frame makes DWM treat the window as framed (needed for the backdrop to render); its
// non-client area is removed in WM_NCCALCSIZE, so the window rect is the blurred rect.
constexpr DWORD kStyle = WS_POPUP | WS_THICKFRAME;

}  // namespace

LRESULT CALLBACK Backdrop::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCCALCSIZE:
            if (wp) return 0;  // client area = window rect: no visible frame
            break;
        case WM_NCACTIVATE:
            // DWM draws the backdrop only for an "active" frame; this window is never really
            // activated, so it always paints itself active.
            return DefWindowProcW(hwnd, msg, TRUE, lp);
        case WM_NCHITTEST:
            return HTTRANSPARENT;  // the dock window above takes all input anyway
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_WINDOWPOSCHANGING:
            // DefWindowProc clamps a thick-frame window to a minimum size (14 px here); the blur
            // must be able to shrink to a sliver while the bar slides out at the screen edge.
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            ValidateRect(hwnd, nullptr);  // nothing to paint: the system backdrop shows through
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool Backdrop::Create(HINSTANCE instance, bool mouseTransparent) {
    if (hwnd_) return true;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &Backdrop::WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    const DWORD ex = kExStyle | (mouseTransparent ? WS_EX_TRANSPARENT | WS_EX_LAYERED : 0);
    hwnd_ = CreateWindowExW(ex, kClassName, L"WinDock backdrop", kStyle, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
    if (!hwnd_) return false;
    if (mouseTransparent) SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);

    // Acrylic first; older builds that only know Mica-style backdrops get those; builds without
    // system backdrops (before 22H2) reject the attribute and we fall back to the plain tint.
    bool supported = false;
    for (DWM_SYSTEMBACKDROP_TYPE type : {DWMSBT_TRANSIENTWINDOW, DWMSBT_TABBEDWINDOW, DWMSBT_MAINWINDOW}) {
        if (SUCCEEDED(DwmSetWindowAttribute(hwnd_, DWMWA_SYSTEMBACKDROP_TYPE, &type, sizeof(type)))) {
            supported = true;
            break;
        }
    }
    if (!supported) {
        OutputDebugStringW(L"WinDock: system backdrops unsupported; acrylic disabled\n");
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    // No DWM accent border: the dock draws its own hairline.
    const COLORREF border = DWMWA_COLOR_NONE;
    DwmSetWindowAttribute(hwnd_, DWMWA_BORDER_COLOR, &border, sizeof(border));
    const MARGINS margins{-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(hwnd_, &margins);
    rect_ = RECT{};
    visible_ = false;
    activated_ = false;
    corner_ = -1;
    return true;
}

void Backdrop::Destroy() {
    if (hwnd_) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    visible_ = false;
}

float Backdrop::SetCornerRadius(float radiusDip) {
    DWM_WINDOW_CORNER_PREFERENCE pref = DWMWCP_ROUND;  // 8 DIPs
    float radius = 8.0f;
    if (radiusDip < 4.0f) {
        pref = DWMWCP_DONOTROUND;
        radius = 0.0f;
    } else if (radiusDip < 8.0f) {
        pref = DWMWCP_ROUNDSMALL;  // 4 DIPs
        radius = 4.0f;
    }
    if (hwnd_ && corner_ != static_cast<int>(pref)) {
        DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
        corner_ = static_cast<int>(pref);
    }
    return radius;
}

void Backdrop::Place(HWND above, const RECT& r) {
    if (!hwnd_) return;
    if (r.right <= r.left || r.bottom <= r.top) {
        Hide();
        return;
    }
    if (visible_ && EqualRect(&r, &rect_) && GetWindow(above, GW_HWNDNEXT) == hwnd_) return;
    const bool show = !visible_;
    rect_ = r;
    visible_ = true;
    SetWindowPos(hwnd_, above, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | (show ? SWP_SHOWWINDOW : 0));
    if (!activated_) {
        SendMessageW(hwnd_, WM_NCACTIVATE, TRUE, 0);
        activated_ = true;
    }
}

void Backdrop::Restack(HWND above) {
    if (!hwnd_ || !visible_ || GetWindow(above, GW_HWNDNEXT) == hwnd_) return;
    SetWindowPos(hwnd_, above, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOMOVE | SWP_NOSIZE);
}

void Backdrop::Hide() {
    if (!hwnd_ || !visible_) return;
    visible_ = false;
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                 SWP_HIDEWINDOW | SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOMOVE | SWP_NOSIZE);
}

bool Backdrop::SystemAllowsTransparency() {
    HIGHCONTRASTW hc{sizeof(hc)};
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON)) {
        return false;
    }
    DWORD value = 1;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"EnableTransparency", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) {
        value = 1;  // not set: Windows' default is on
    }
    return value != 0;
}
