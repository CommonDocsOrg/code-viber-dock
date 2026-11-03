// The settings window's navigation column (WinDock.SettingsNav): one row per page, painted with
// per-pixel alpha so the Mica backdrop of the extended frame shows through.

#include <windows.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include "SettingsWindow.h"

namespace {

// Composites a rounded rectangle (or, with ring > 0, only its outline) of `color` at `alpha`
// over a premultiplied top-down 32 bpp buffer, anti-aliased with a signed distance per pixel.
void BlendRoundRect(RGBQUAD* bits, int stride, int width, int height, const RECT& r, float radius,
                    COLORREF color, BYTE alpha, float ring = 0.0f) {
    if (!bits || alpha == 0 || r.right <= r.left || r.bottom <= r.top) return;
    const float cx = (static_cast<float>(r.left) + static_cast<float>(r.right)) / 2.0f;
    const float cy = (static_cast<float>(r.top) + static_cast<float>(r.bottom)) / 2.0f;
    const float hx = static_cast<float>(r.right - r.left) / 2.0f;
    const float hy = static_cast<float>(r.bottom - r.top) / 2.0f;
    radius = std::min({radius, hx, hy});
    const float a0 = static_cast<float>(alpha) / 255.0f;
    const float red = GetRValue(color), green = GetGValue(color), blue = GetBValue(color);
    const int y0 = std::max<int>(r.top, 0), y1 = std::min<int>(r.bottom, height);
    const int x0 = std::max<int>(r.left, 0), x1 = std::min<int>(r.right, width);
    for (int y = y0; y < y1; ++y) {
        RGBQUAD* row = bits + static_cast<ptrdiff_t>(y) * stride;
        for (int x = x0; x < x1; ++x) {
            const float qx = std::fabs(static_cast<float>(x) + 0.5f - cx) - (hx - radius);
            const float qy = std::fabs(static_cast<float>(y) + 0.5f - cy) - (hy - radius);
            const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
            const float d = std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - radius;
            float cov = std::clamp(0.5f - d, 0.0f, 1.0f);
            if (ring > 0.0f) cov = std::max(0.0f, cov - std::clamp(0.5f - (d + ring), 0.0f, 1.0f));
            const float a = a0 * cov;
            if (a <= 0.0f) continue;
            RGBQUAD& p = row[x];
            const float k = 1.0f - a;
            p.rgbRed = static_cast<BYTE>(std::lround(red * a + p.rgbRed * k));
            p.rgbGreen = static_cast<BYTE>(std::lround(green * a + p.rgbGreen * k));
            p.rgbBlue = static_cast<BYTE>(std::lround(blue * a + p.rgbBlue * k));
            p.rgbReserved = static_cast<BYTE>(std::lround(255.0f * a + p.rgbReserved * k));
        }
    }
}

}  // namespace

// --- nav pane --------------------------------------------------------------------------------

LRESULT CALLBACK SettingsWindow::NavProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SettingsWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<SettingsWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == WM_NCDESTROY) SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    return self->NavProc(hwnd, msg, wp, lp);
}

RECT SettingsWindow::NavItemRect(int index) const {
    // 36 DIPs a page, less once the pages would not fit the window's height (16 pages: 34).
    const int count = std::max(1, static_cast<int>(pages_.size()));
    const int itemDip = std::min(kNavItemDip, (kHeightDip - kNavTopDip - 4) / count);
    const int top = Px(kNavTopDip) + index * Px(itemDip);
    return RECT{Px(6), top + Px(2), Px(kNavWidthDip) - Px(6), top + Px(itemDip) - Px(2)};
}

int SettingsWindow::NavHitTest(POINT pt) const {
    for (int i = 0; i < static_cast<int>(pages_.size()); ++i) {
        RECT r = NavItemRect(i);
        r.top -= Px(2);
        r.bottom += Px(2);
        if (PtInRect(&r, pt)) return i;
    }
    return -1;
}

LRESULT SettingsWindow::NavProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS;
        case WM_KEYDOWN:
            switch (wp) {
                case VK_UP:
                case VK_LEFT:
                    SelectPage(active_ - 1);
                    return 0;
                case VK_DOWN:
                case VK_RIGHT:
                    SelectPage(active_ + 1);
                    return 0;
                case VK_HOME:
                    SelectPage(0);
                    return 0;
                case VK_END:
                    SelectPage(static_cast<int>(pages_.size()) - 1);
                    return 0;
                default:
                    break;
            }
            break;
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            const int i = NavHitTest(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            if (i >= 0) SelectPage(i);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!navTracking_) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                navTracking_ = TrackMouseEvent(&tme) != FALSE;
            }
            const int hot = NavHitTest(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            if (hot != navHot_) {
                navHot_ = hot;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            navTracking_ = false;
            if (navHot_ != -1) {
                navHot_ = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_UPDATEUISTATE: {
            const LRESULT r = DefWindowProcW(hwnd, msg, wp, lp);
            InvalidateRect(hwnd, nullptr, FALSE);
            return r;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            PaintNav(hwnd, dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:
            PaintNav(hwnd, reinterpret_cast<HDC>(wp));
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void SettingsWindow::PaintNav(HWND hwnd, HDC hdc) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    BP_PAINTPARAMS params{sizeof(params), BPPF_ERASE, nullptr, nullptr};
    HDC dc = nullptr;
    HPAINTBUFFER buffer = BeginBufferedPaint(hdc, &rc, BPBF_TOPDOWNDIB, &params, &dc);
    if (!buffer) {
        // No buffered paint: plain opaque GDI (only correct without Mica, which is then off).
        FillRect(hdc, &rc, baseBrush_);
        dc = hdc;
    }
    RGBQUAD* bits = nullptr;
    int stride = 0;
    if (buffer && SUCCEEDED(GetBufferedPaintBits(buffer, &bits, &stride)) && bits && !mica_) {
        // Solid nav column: opaque base colour.
        const RGBQUAD base{GetBValue(palette_.base), GetGValue(palette_.base), GetRValue(palette_.base), 255};
        for (int y = 0; y < rc.bottom; ++y) {
            std::fill_n(bits + static_cast<ptrdiff_t>(y) * stride, rc.right, base);
        }
    }
    const float radius = static_cast<float>(Px(4));
    const bool focused = GetFocus() == hwnd && !FocusCuesHidden(hwnd);
    HTHEME theme = OpenThemeData(hwnd, L"WINDOW");
    SetBkMode(dc, TRANSPARENT);
    for (int i = 0; i < static_cast<int>(pages_.size()); ++i) {
        const RECT r = NavItemRect(i);
        if (i == active_) {
            BlendRoundRect(bits, stride, rc.right, rc.bottom, r, radius, palette_.pill, palette_.pillSelectedAlpha);
            const int cy = (r.top + r.bottom) / 2;
            const RECT bar{r.left, cy - Px(8), r.left + Px(3), cy + Px(8)};
            BlendRoundRect(bits, stride, rc.right, rc.bottom, bar, static_cast<float>(Px(3)) / 2.0f, palette_.accent, 255);
            if (focused) {
                BlendRoundRect(bits, stride, rc.right, rc.bottom, r, radius, palette_.text, 255,
                               static_cast<float>(std::max(1, Px(2))));
            }
        } else if (i == navHot_) {
            BlendRoundRect(bits, stride, rc.right, rc.bottom, r, radius, palette_.pill, palette_.pillHoverAlpha);
        }
        const SettingsPage& page = *pages_[static_cast<size_t>(i)];
        const int textLeft = fontIcons_ ? Px(44) : Px(18);
        const struct {
            HFONT font;
            const wchar_t* text;
            int len;
            RECT rect;
        } parts[] = {
            {fontIcons_, &page.glyph_, page.glyph_ && fontIcons_ ? 1 : 0, RECT{r.left + Px(12), r.top, r.left + Px(40), r.bottom}},
            {font_, page.name_.c_str(), static_cast<int>(page.name_.size()), RECT{textLeft, r.top, r.right - Px(8), r.bottom}},
        };
        for (const auto& part : parts) {
            if (part.len == 0 || !part.font) continue;
            HGDIOBJ old = SelectObject(dc, part.font);
            RECT tr = part.rect;
            const DWORD flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS;
            if (theme && buffer) {
                DTTOPTS opts{sizeof(opts)};
                opts.dwFlags = DTT_COMPOSITED | DTT_TEXTCOLOR;
                opts.crText = palette_.text;
                DrawThemeTextEx(theme, dc, 0, 0, part.text, part.len, flags, &tr, &opts);
            } else {
                SetTextColor(dc, palette_.text);
                DrawTextW(dc, part.text, part.len, &tr, flags);
            }
            SelectObject(dc, old);
        }
    }
    if (theme) CloseThemeData(theme);
    if (buffer) EndBufferedPaint(buffer, TRUE);
}

