// The Global settings window's navigation column (WinDock.SettingsNav): one row per page, painted
// with per-pixel alpha so the Mica backdrop of the extended frame shows through. And a Dock window's
// tab strip (WinDock.SettingsTabs, round 5 phase B): one tab per page, the selected one underlined
// in the dock's colour.

#include <windows.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "SettingsIds.h"
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

std::vector<SettingsWindow::NavSlot> SettingsWindow::NavOrder() const {
    std::vector<NavSlot> order;
    const int n = static_cast<int>(pages_.size());
    auto listed = [&](int i) { return i >= 0 && i < n && pages_[static_cast<size_t>(i)]->InNav(); };
    if (kind_ == SettingsKind::Global) {
        // Round 8 phase 8D: grouped by purpose; the page indexes (kPage*) do not change.
        static constexpr struct {
            int page;
            const wchar_t* caption;
            bool gap;
        } kGlobal[] = {
            {SettingsIds::kPageGeneral, L"GENERAL", false},   {SettingsIds::kPageDocks, nullptr, false},
            {SettingsIds::kPageThemes, L"LOOK", false},       {SettingsIds::kPageAnimation, nullptr, false},
            {SettingsIds::kPageCalendar, L"DATA", false},     {SettingsIds::kPageNotifications, nullptr, false},
            {SettingsIds::kPageTray, nullptr, false},         {SettingsIds::kPageWidgets, L"WIDGETS", false},
            {SettingsIds::kPageAbout, nullptr, true},
        };
        const wchar_t* pendingCaption = nullptr;  // a caption whose page is not listed goes to the next
        bool pendingGap = false;
        for (const auto& s : kGlobal) {
            if (s.caption) pendingCaption = s.caption;
            pendingGap = pendingGap || s.gap;
            if (!listed(s.page)) continue;
            order.push_back(NavSlot{s.page, pendingCaption, pendingGap});
            pendingCaption = nullptr;
            pendingGap = false;
        }
        // A page the table does not name (none today) at the end.
        for (int i = 0; i < n; ++i) {
            if (!listed(i)) continue;
            if (std::none_of(order.begin(), order.end(), [i](const NavSlot& s) { return s.page == i; })) order.push_back(NavSlot{i, nullptr, false});
        }
        // Round 9 phase 9D: the Help row (not a page: a click opens the Help window at the quick
        // start; the arrow keys skip it, F1 is its key).
        order.push_back(NavSlot{SettingsIds::kNavHelp, nullptr, false});
        return order;
    }
    for (int i = 0; i < n; ++i) {
        if (listed(i)) order.push_back(NavSlot{i, nullptr, false});
    }
    return order;
}

int SettingsWindow::NavDisplayIndex(int index) const {
    const std::vector<NavSlot> order = NavOrder();
    for (size_t k = 0; k < order.size(); ++k) {
        if (order[k].page == index) return static_cast<int>(k);
    }
    return -1;
}

int SettingsWindow::NavItemDip(const std::vector<NavSlot>& order) const {
    // 36 DIPs, less when the rows, captions and gaps would not fit the window's height (at least 24).
    int captions = 0, gaps = 0;
    for (const NavSlot& s : order) {
        captions += s.caption ? 1 : 0;
        gaps += s.gap ? 1 : 0;
    }
    const int room = static_cast<int>(ClientDip().cy) - (find_ ? kNavFindDip : 0) - kNavTopDip - 4 - captions * kNavCaptionDip -
                     gaps * kNavGapDip;
    return std::clamp(room / std::max(1, static_cast<int>(order.size())), 24, kNavItemDip);
}

RECT SettingsWindow::NavCaptionRect(size_t k) const {
    const std::vector<NavSlot> order = NavOrder();
    if (k >= order.size() || !order[k].caption) return RECT{};
    const int itemDip = NavItemDip(order);
    int y = Px(kNavTopDip);
    for (size_t i = 0; i < order.size(); ++i) {
        if (order[i].gap) y += Px(kNavGapDip);
        if (order[i].caption) {
            if (i == k) return RECT{Px(18), y, Px(kNavWidthDip) - Px(8), y + Px(kNavCaptionDip)};
            y += Px(kNavCaptionDip);
        }
        y += Px(itemDip);
    }
    return RECT{};
}

RECT SettingsWindow::NavItemRect(int index) const {
    // 36 DIPs a page (round 8 phase 8D: under the group captions, About after a gap), less once
    // the pages would not fit the window's height. Pages that are not listed (InNav) take no row.
    const std::vector<NavSlot> order = NavOrder();
    const int itemDip = NavItemDip(order);
    int y = Px(kNavTopDip);
    for (const NavSlot& s : order) {
        if (s.gap) y += Px(kNavGapDip);
        if (s.caption) y += Px(kNavCaptionDip);
        if (s.page == index) return RECT{Px(6), y + Px(2), Px(kNavWidthDip) - Px(6), y + Px(itemDip) - Px(2)};
        y += Px(itemDip);
    }
    return RECT{};
}

int SettingsWindow::NavStep(int from, int step) const {
    // In display order (round 8 phase 8D); `from` before the first (-1) or after the last
    // (pages_.size()) for Home / End.
    std::vector<NavSlot> order = NavOrder();
    std::erase_if(order, [](const NavSlot& s) { return s.page == SettingsIds::kNavHelp; });  // round 9 phase 9D: not a page
    if (order.empty()) return from;
    if (from < 0) return step > 0 ? order.front().page : from;
    if (from >= static_cast<int>(pages_.size())) return step < 0 ? order.back().page : from;
    for (size_t k = 0; k < order.size(); ++k) {
        if (order[k].page != from) continue;
        const int next = static_cast<int>(k) + step;
        return next >= 0 && next < static_cast<int>(order.size()) ? order[static_cast<size_t>(next)].page : from;
    }
    return order.front().page;  // `from` is not listed
}

int SettingsWindow::NavHitTest(POINT pt) const {
    for (const NavSlot& s : NavOrder()) {
        RECT r = NavItemRect(s.page);
        r.top -= Px(2);
        r.bottom += Px(2);
        if (PtInRect(&r, pt)) return s.page;
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
                    SelectPage(NavStep(active_, -1));
                    return 0;
                case VK_DOWN:
                case VK_RIGHT:
                    SelectPage(NavStep(active_, 1));
                    return 0;
                case VK_HOME:
                    SelectPage(NavStep(-1, 1));
                    return 0;
                case VK_END:
                    SelectPage(NavStep(static_cast<int>(pages_.size()), -1));
                    return 0;
                default:
                    break;
            }
            break;
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            const int i = NavHitTest(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            if (i == SettingsIds::kNavHelp) {
                OpenHelp(L"quick-start.md");  // round 9 phase 9D: the Help row
            } else if (i >= 0) {
                SelectPage(i);
            }
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
    auto drawText = [&](HFONT font, const wchar_t* text, int len, RECT tr, COLORREF color, DWORD flags) {
        if (len == 0 || !font) return;
        HGDIOBJ old = SelectObject(dc, font);
        if (theme && buffer) {
            DTTOPTS opts{sizeof(opts)};
            opts.dwFlags = DTT_COMPOSITED | DTT_TEXTCOLOR;
            opts.crText = color;
            DrawThemeTextEx(theme, dc, 0, 0, text, len, flags, &tr, &opts);
        } else {
            SetTextColor(dc, color);
            DrawTextW(dc, text, len, &tr, flags);
        }
        SelectObject(dc, old);
    };
    const std::vector<NavSlot> order = NavOrder();
    // Round 8 phase 8D: the group captions, small and muted.
    for (size_t k = 0; k < order.size(); ++k) {
        if (!order[k].caption) continue;
        RECT cr = NavCaptionRect(k);
        cr.top += Px(4);  // closer to their rows than to the row above
        drawText(fontCaption_ ? fontCaption_ : font_, order[k].caption, static_cast<int>(wcslen(order[k].caption)), cr,
                 palette_.textSecondary, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    for (const NavSlot& slot : order) {
        const int i = slot.page;
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
        // Round 9 phase 9D: the Help row has no page (Segoe Fluent Icons' Help glyph).
        static const wchar_t kHelpGlyph = L'\xE897';
        static const std::wstring kHelpName = L"Help";
        const bool helpRow = i == SettingsIds::kNavHelp;
        if (!helpRow && (i < 0 || i >= static_cast<int>(pages_.size()))) continue;
        const wchar_t* glyph = helpRow ? &kHelpGlyph : &pages_[static_cast<size_t>(i)]->glyph_;
        const std::wstring& name = helpRow ? kHelpName : pages_[static_cast<size_t>(i)]->name_;
        const int textLeft = fontIcons_ ? Px(44) : Px(18);
        const struct {
            HFONT font;
            const wchar_t* text;
            int len;
            RECT rect;
        } parts[] = {
            {fontIcons_, glyph, *glyph && fontIcons_ ? 1 : 0, RECT{r.left + Px(12), r.top, r.left + Px(40), r.bottom}},
            {font_, name.c_str(), static_cast<int>(name.size()), RECT{textLeft, r.top, r.right - Px(8), r.bottom}},
        };
        for (const auto& part : parts) {
            drawText(part.font, part.text, part.len, part.rect, palette_.text,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
    }
    if (theme) CloseThemeData(theme);
    if (buffer) EndBufferedPaint(buffer, TRUE);
}

// --- the Find a setting… box (round 9 phase 9B) ------------------------------------------------
// A box at the top of the nav column that looks like a search box but only opens the settings
// search dialog: a click, Enter, Space or Down opens it, a typed character opens it with that
// character in its box. Painted like the nav (per-pixel alpha over the Mica column).

LRESULT CALLBACK SettingsWindow::FindProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SettingsWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<SettingsWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == WM_NCDESTROY) SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    return self->FindProc(hwnd, msg, wp, lp);
}

LRESULT SettingsWindow::FindProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_GETDLGCODE: {
            // Typed characters, and Enter (which the dialog manager would take as the default button).
            const MSG* m = reinterpret_cast<const MSG*>(lp);
            if (m && m->message == WM_KEYDOWN && m->wParam == VK_RETURN) return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
            return DLGC_WANTCHARS | DLGC_WANTARROWS;
        }
        case WM_KEYDOWN:
            if (wp == VK_RETURN || wp == VK_SPACE || wp == VK_DOWN) {
                OpenSearch(L"");
                return 0;
            }
            break;
        case WM_CHAR:
            if (wp > L' ' && wp != 0x7F) OpenSearch(std::wstring(1, static_cast<wchar_t>(wp)));
            return 0;
        case WM_LBUTTONDOWN:
            SetFocus(hwnd);
            OpenSearch(L"");
            return 0;
        case WM_MOUSEMOVE:
            if (!findTracking_) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                findTracking_ = TrackMouseEvent(&tme) != FALSE;
            }
            if (!findHot_) {
                findHot_ = true;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_MOUSELEAVE:
            findTracking_ = false;
            if (findHot_) {
                findHot_ = false;
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
            PaintFind(hwnd, dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:
            PaintFind(hwnd, reinterpret_cast<HDC>(wp));
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void SettingsWindow::PaintFind(HWND hwnd, HDC hdc) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    BP_PAINTPARAMS params{sizeof(params), BPPF_ERASE, nullptr, nullptr};
    HDC dc = nullptr;
    HPAINTBUFFER buffer = BeginBufferedPaint(hdc, &rc, BPBF_TOPDOWNDIB, &params, &dc);
    if (!buffer) {
        FillRect(hdc, &rc, baseBrush_);
        dc = hdc;
    }
    RGBQUAD* bits = nullptr;
    int stride = 0;
    if (buffer && SUCCEEDED(GetBufferedPaintBits(buffer, &bits, &stride)) && bits && !mica_) {
        const RGBQUAD base{GetBValue(palette_.base), GetGValue(palette_.base), GetRValue(palette_.base), 255};
        for (int y = 0; y < rc.bottom; ++y) std::fill_n(bits + static_cast<ptrdiff_t>(y) * stride, rc.right, base);
    }
    const float radius = static_cast<float>(Px(4));
    const bool focused = GetFocus() == hwnd;
    // The field: the control colour, a hairline border (stronger on hover), the accent under it
    // while it has the focus (as a text box does). High contrast: the system colours, opaque.
    const BYTE fill = palette_.highContrast ? 255 : palette_.dark ? (findHot_ ? 0xD8 : 0xB8) : (findHot_ ? 0xFF : 0xE8);
    BlendRoundRect(bits, stride, rc.right, rc.bottom, rc, radius, palette_.control, fill);
    BlendRoundRect(bits, stride, rc.right, rc.bottom, rc, radius, palette_.highContrast ? palette_.text : palette_.textSecondary,
                   palette_.highContrast ? 255 : static_cast<BYTE>(findHot_ ? 0x70 : 0x48), 1.0f);
    if (focused) {
        const RECT bar{rc.left + Px(2), rc.bottom - Px(2), rc.right - Px(2), rc.bottom};
        BlendRoundRect(bits, stride, rc.right, rc.bottom, bar, static_cast<float>(Px(1)), palette_.accent, 255);
    }
    if (!bits && !buffer) {
        // No buffered paint: plain GDI.
        HBRUSH b = CreateSolidBrush(palette_.control);
        FillRect(dc, &rc, b);
        DeleteObject(b);
    }
    HTHEME theme = OpenThemeData(hwnd, L"WINDOW");
    SetBkMode(dc, TRANSPARENT);
    auto drawText = [&](HFONT font, const wchar_t* text, int len, RECT tr, COLORREF color) {
        if (len == 0 || !font) return;
        HGDIOBJ old = SelectObject(dc, font);
        const DWORD flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS;
        if (theme && buffer) {
            DTTOPTS opts{sizeof(opts)};
            opts.dwFlags = DTT_COMPOSITED | DTT_TEXTCOLOR;
            opts.crText = color;
            DrawThemeTextEx(theme, dc, 0, 0, text, len, flags, &tr, &opts);
        } else {
            SetTextColor(dc, color);
            DrawTextW(dc, text, len, &tr, flags);
        }
        SelectObject(dc, old);
    };
    int textLeft = Px(10);
    if (fontIcons_) {
        static const wchar_t kGlyph = L'\xE721';  // Search (Segoe Fluent Icons / MDL2 Assets)
        drawText(fontIcons_, &kGlyph, 1, RECT{Px(9), rc.top, Px(30), rc.bottom}, palette_.textSecondary);
        textLeft = Px(32);
    }
    static const wchar_t kText[] = L"Find a setting\x2026";
    drawText(font_, kText, static_cast<int>(wcslen(kText)), RECT{textLeft, rc.top, rc.right - Px(8), rc.bottom}, palette_.textSecondary);
    if (focused && !FocusCuesHidden(hwnd) && palette_.highContrast) {
        RECT fr = rc;
        InflateRect(&fr, -Px(2), -Px(2));
        SetTextColor(dc, palette_.text);
        DrawFocusRect(dc, &fr);
    }
    if (theme) CloseThemeData(theme);
    if (buffer) EndBufferedPaint(buffer, TRUE);
}

// --- tab strip -------------------------------------------------------------------------------

LRESULT CALLBACK SettingsWindow::TabsProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SettingsWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<SettingsWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == WM_NCDESTROY) SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    return self->TabsProc(hwnd, msg, wp, lp);
}

RECT SettingsWindow::TabRect(int index) const {
    // Left to right from the margin, each as wide as its name plus padding.
    if (!tabs_ || index < 0 || index >= static_cast<int>(pages_.size())) return RECT{};
    RECT client{};
    GetClientRect(tabs_, &client);
    HDC dc = GetDC(tabs_);
    HGDIOBJ old = dc ? SelectObject(dc, font_) : nullptr;
    int x = Px(16);
    RECT r{};
    for (int i = 0; i <= index; ++i) {
        const std::wstring& name = pages_[static_cast<size_t>(i)]->Name();
        SIZE ext{};
        if (dc) GetTextExtentPoint32W(dc, name.c_str(), static_cast<int>(name.size()), &ext);
        const int w = ext.cx + Px(28);
        r = RECT{x, Px(4), x + w, client.bottom - Px(2)};
        x += w + Px(4);
    }
    if (dc) {
        SelectObject(dc, old);
        ReleaseDC(tabs_, dc);
    }
    return r;
}

int SettingsWindow::TabHitTest(POINT pt) const {
    for (int i = 0; i < static_cast<int>(pages_.size()); ++i) {
        const RECT r = TabRect(i);
        if (PtInRect(&r, pt)) return i;
    }
    return -1;
}

LRESULT SettingsWindow::TabsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS;
        case WM_KEYDOWN:
            switch (wp) {
                case VK_LEFT:
                    SelectPage(NavStep(active_, -1));
                    return 0;
                case VK_RIGHT:
                    SelectPage(NavStep(active_, 1));
                    return 0;
                case VK_HOME:
                    SelectPage(NavStep(-1, 1));
                    return 0;
                case VK_END:
                    SelectPage(NavStep(static_cast<int>(pages_.size()), -1));
                    return 0;
                default:
                    break;
            }
            break;
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            const int i = TabHitTest(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            if (i >= 0) SelectPage(i);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!tabsTracking_) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                tabsTracking_ = TrackMouseEvent(&tme) != FALSE;
            }
            const int hot = TabHitTest(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            if (hot != tabHot_) {
                tabHot_ = hot;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            tabsTracking_ = false;
            if (tabHot_ != -1) {
                tabHot_ = -1;
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
            PaintTabs(hwnd, dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:
            PaintTabs(hwnd, reinterpret_cast<HDC>(wp));
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void SettingsWindow::PaintTabs(HWND hwnd, HDC hdc) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    // Double-buffered GDI on the content layer (no alpha: nothing shows through here).
    HDC dc = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom));
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    FillRect(dc, &rc, layerBrush_);
    auto mix = [](COLORREF a, COLORREF b, int pa) {
        auto ch = [&](BYTE x, BYTE y) { return static_cast<BYTE>((x * pa + y * (100 - pa)) / 100); };
        return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)), ch(GetBValue(a), GetBValue(b)));
    };
    // A hairline under the strip.
    {
        HBRUSH line = CreateSolidBrush(mix(palette_.text, palette_.layer, 14));
        RECT r{0, rc.bottom - std::max(1, Px(1)), rc.right, rc.bottom};
        FillRect(dc, &r, line);
        DeleteObject(line);
    }
    const COLORREF accent = palette_.highContrast ? palette_.accent : DockColor();
    const bool focused = GetFocus() == hwnd && !FocusCuesHidden(hwnd);
    HGDIOBJ oldFont = SelectObject(dc, font_);
    SetBkMode(dc, TRANSPARENT);
    for (int i = 0; i < static_cast<int>(pages_.size()); ++i) {
        const RECT r = TabRect(i);
        const bool selected = i == active_;
        if (selected || i == tabHot_) {
            HBRUSH pill = CreateSolidBrush(mix(palette_.text, palette_.layer, selected ? 9 : 5));
            HPEN pen = CreatePen(PS_SOLID, 1, mix(palette_.text, palette_.layer, selected ? 9 : 5));
            HGDIOBJ ob = SelectObject(dc, pill);
            HGDIOBJ op = SelectObject(dc, pen);
            RoundRect(dc, r.left, r.top, r.right, r.bottom - Px(4), Px(8), Px(8));
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(pen);
            DeleteObject(pill);
        }
        if (selected) {
            HBRUSH bar = CreateSolidBrush(accent);
            RECT u{r.left + Px(10), r.bottom - Px(4), r.right - Px(10), r.bottom - Px(1)};
            FillRect(dc, &u, bar);
            DeleteObject(bar);
        }
        const std::wstring& name = pages_[static_cast<size_t>(i)]->Name();
        SetTextColor(dc, selected ? palette_.text : palette_.textSecondary);
        RECT tr{r.left, r.top, r.right, r.bottom - Px(4)};
        DrawTextW(dc, name.c_str(), static_cast<int>(name.size()), &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (selected && focused) {
            RECT fr{r.left + Px(2), r.top + Px(2), r.right - Px(2), r.bottom - Px(6)};
            SetTextColor(dc, palette_.text);
            SetBkColor(dc, palette_.layer);
            DrawFocusRect(dc, &fr);
        }
    }
    SelectObject(dc, oldFont);
    BitBlt(hdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

