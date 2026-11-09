// Round 12 phase 12A: the Keep / Revert, Keep / Cancel banner near the dock (KeepBanner.h).

#include "KeepBanner.h"

#include <commctrl.h>

#include <algorithm>

#include "SettingsIds.h"

namespace {

bool RegisterBannerClass(HINSTANCE instance, WNDPROC proc) {
    static bool registered = false;
    if (registered) return true;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = KeepBanner::kClassName;
    registered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return registered;
}

}  // namespace

KeepBanner::~KeepBanner() {
    Close();
    if (font_) DeleteObject(font_);
    if (brush_) DeleteObject(brush_);
}

void KeepBanner::Close() {
    if (hwnd_) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    text_ = keep_ = cancel_ = nullptr;
    shownText_.clear();
    shownCancel_.clear();
}

void KeepBanner::ApplyStyle() {
    palette_ = DarkMode::CurrentPalette();
    if (brush_) DeleteObject(brush_);
    brush_ = CreateSolidBrush(palette_.layer);
    if (fontDpi_ != dpi_ || !font_) {
        NONCLIENTMETRICSW ncm{sizeof(ncm)};
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi_);
        if (font_) DeleteObject(font_);
        font_ = CreateFontIndirectW(&ncm.lfMessageFont);
        fontDpi_ = dpi_;
    }
    for (HWND c : {text_, keep_, cancel_}) {
        if (!c) continue;
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
        if (c != text_) DarkMode::ThemeControl(c, palette_.dark);
    }
}

bool KeepBanner::Show(HINSTANCE instance, const RECT& bar, const RECT& work, DockEdge edge, bool floating, UINT dpi,
                      const std::wstring& text, const std::wstring& cancel) {
    if (!RegisterBannerClass(instance, &KeepBanner::WndProcThunk)) return false;
    dpi_ = std::max<UINT>(dpi, USER_DEFAULT_SCREEN_DPI / 2);
    if (!hwnd_) {
        // Topmost (the dock is topmost too), never activated: a click on a button still works.
        CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClassName, L"WinDock", WS_POPUP | WS_CLIPCHILDREN,
                        0, 0, 0, 0, nullptr, nullptr, instance, this);
        if (!hwnd_) return false;
        text_ = CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX | SS_ENDELLIPSIS, 0, 0,
                                0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(SettingsIds::kKeepBarText)), instance, nullptr);
        keep_ = CreateWindowExW(0, WC_BUTTONW, L"Keep", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd_,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(SettingsIds::kKeepBarKeep)), instance, nullptr);
        cancel_ = CreateWindowExW(0, WC_BUTTONW, L"", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd_,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(SettingsIds::kKeepBarRevert)), instance, nullptr);
        fontDpi_ = 0;
        ApplyStyle();
    } else if (fontDpi_ != dpi_) {
        ApplyStyle();
    }
    const bool sameText = text == shownText_ && cancel == shownCancel_;
    if (!sameText) {
        SetWindowTextW(text_, text.c_str());
        SetWindowTextW(cancel_, cancel.c_str());
        SetWindowTextW(hwnd_, (text + L" Keep \x00B7 " + cancel).c_str());  // what it says (automation)
        shownText_ = text;
        shownCancel_ = cancel;
    }
    // The size: the text's width plus the buttons, within the display.
    HDC dc = GetDC(hwnd_);
    HGDIOBJ old = SelectObject(dc, font_);
    SIZE ext{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &ext);
    SelectObject(dc, old);
    ReleaseDC(hwnd_, dc);
    const int workW = std::max<int>(Px(200), work.right - work.left - Px(32));
    const int w = std::clamp(static_cast<int>(ext.cx) + Px(20 + 16 + 72 + 8 + 112 + 12), Px(360), workW);
    const int h = Px(44), gap = Px(12);
    // Beside the bar, on the screen's side of it; a floating bar: above it (below near the top).
    const bool hasBar = bar.right > bar.left && bar.bottom > bar.top;
    const int cx = hasBar ? (bar.left + bar.right) / 2 : (work.left + work.right) / 2;
    const int cy = hasBar ? (bar.top + bar.bottom) / 2 : work.bottom - h;
    int x = cx - w / 2, y = hasBar ? bar.top - gap - h : work.bottom - gap - h;
    if (hasBar && !floating) {
        switch (edge) {
            case DockEdge::Top: y = bar.bottom + gap; break;
            case DockEdge::Left:
                x = bar.right + gap;
                y = cy - h / 2;
                break;
            case DockEdge::Right:
                x = bar.left - gap - w;
                y = cy - h / 2;
                break;
            case DockEdge::Bottom:
            default: break;
        }
    } else if (hasBar && y < work.top) {
        y = bar.bottom + gap;
    }
    x = std::clamp(x, static_cast<int>(work.left), std::max(static_cast<int>(work.left), static_cast<int>(work.right) - w));
    y = std::clamp(y, static_cast<int>(work.top), std::max(static_cast<int>(work.top), static_cast<int>(work.bottom) - h));
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    Layout();
    if (!sameText) InvalidateRect(hwnd_, nullptr, TRUE);
    return true;
}

void KeepBanner::Layout() {
    if (!hwnd_) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int bh = Px(28), by = (rc.bottom - bh) / 2;
    int x = rc.right - Px(12) - Px(112);
    SetWindowPos(cancel_, nullptr, x, by, Px(112), bh, SWP_NOZORDER | SWP_NOACTIVATE);
    x -= Px(8) + Px(72);
    SetWindowPos(keep_, nullptr, x, by, Px(72), bh, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(text_, nullptr, Px(20), 0, std::max(0, x - Px(16) - Px(20)), rc.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT CALLBACK KeepBanner::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    KeepBanner* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<KeepBanner*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<KeepBanner*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        if (self->hwnd_ == hwnd) self->hwnd_ = nullptr;
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return self->WndProc(msg, wp, lp);
}

LRESULT KeepBanner::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;  // the buttons take the click, the foreground stays where it is
        case WM_ERASEBKGND: {
            HDC dc = reinterpret_cast<HDC>(wp);
            RECT rc{};
            GetClientRect(hwnd_, &rc);
            FillRect(dc, &rc, brush_);
            // An accent edge at the left and a hairline border, as the settings windows' bar.
            HBRUSH accent = CreateSolidBrush(palette_.highContrast ? palette_.text : palette_.accent);
            RECT edge{0, 0, Px(4), rc.bottom};
            FillRect(dc, &edge, accent);
            DeleteObject(accent);
            HBRUSH hair = CreateSolidBrush(palette_.textDisabled);
            FrameRect(dc, &rc, hair);
            DeleteObject(hair);
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, palette_.text);
            SetBkColor(dc, palette_.layer);
            return reinterpret_cast<LRESULT>(brush_);
        }
        case WM_COMMAND:
            if (HIWORD(wp) == BN_CLICKED && command_) {
                // The handler may close (destroy) this banner: nothing of it is used afterwards.
                const int id = LOWORD(wp);
                auto command = command_;
                command(id);
            }
            return 0;
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            ApplyStyle();
            InvalidateRect(hwnd_, nullptr, TRUE);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
