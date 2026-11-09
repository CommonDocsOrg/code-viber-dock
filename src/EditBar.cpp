// Round 12 phase 12D: the edit bar of customize mode (EditBar.h). Built like the 12A KeepBanner.

#include "EditBar.h"

#include <commctrl.h>

#include <algorithm>

#include "SettingsIds.h"

namespace {

bool RegisterEditBarClass(HINSTANCE instance, WNDPROC proc) {
    static bool registered = false;
    if (registered) return true;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = EditBar::kClassName;
    registered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return registered;
}

// The buttons, right to left as they are laid out (id, caption, width in DIPs).
struct ButtonSpec {
    int id;
    const wchar_t* text;
    int w;
};
constexpr ButtonSpec kButtons[] = {{SettingsIds::kEditBarDone, L"Done", 64},
                                   {SettingsIds::kEditBarSettings, L"Settings", 80},
                                   {SettingsIds::kEditBarAdd, L"Add items", 88},
                                   {SettingsIds::kEditBarUndo, L"Undo", 64}};
constexpr int kStatusW = 96;   // DIPs: the save state ("Not saved yet")
constexpr int kBarH = 44;      // DIPs
constexpr int kDockGap = 44;   // DIPs between the bar's outer side and the strip: the anchor targets stay free

COLORREF FromArgb(uint32_t argb) { return RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF); }

}  // namespace

EditBar::~EditBar() {
    Close();
    for (HGDIOBJ o : {static_cast<HGDIOBJ>(font_), static_cast<HGDIOBJ>(bold_), static_cast<HGDIOBJ>(brush_)}) {
        if (o) DeleteObject(o);
    }
}

void EditBar::Close() {
    if (hwnd_) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    title_ = status_ = nullptr;
    hasShown_ = false;
}

RECT EditBar::Rect() const {
    RECT r{};
    if (hwnd_) GetWindowRect(hwnd_, &r);
    return r;
}

void EditBar::ApplyStyle() {
    palette_ = DarkMode::CurrentPalette();
    if (brush_) DeleteObject(brush_);
    brush_ = CreateSolidBrush(palette_.layer);
    if (fontDpi_ != dpi_ || !font_) {
        NONCLIENTMETRICSW ncm{sizeof(ncm)};
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi_);
        if (font_) DeleteObject(font_);
        if (bold_) DeleteObject(bold_);
        font_ = CreateFontIndirectW(&ncm.lfMessageFont);
        LOGFONTW b = ncm.lfMessageFont;
        b.lfWeight = FW_SEMIBOLD;
        bold_ = CreateFontIndirectW(&b);
        fontDpi_ = dpi_;
    }
    if (!hwnd_) return;
    for (HWND c = GetWindow(hwnd_, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(c == title_ ? bold_ : font_), FALSE);
        if (c != title_ && c != status_) DarkMode::ThemeControl(c, palette_.dark);
    }
}

bool EditBar::Show(HINSTANCE instance, const RECT& bar, const RECT& work, DockEdge edge, bool floating, UINT dpi, const Content& content) {
    if (!RegisterEditBarClass(instance, &EditBar::WndProcThunk)) return false;
    dpi_ = std::max<UINT>(dpi, USER_DEFAULT_SCREEN_DPI / 2);
    if (!hwnd_) {
        // Topmost (the dock is topmost too), never activated: a click on a button still works.
        CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClassName, L"WinDock", WS_POPUP | WS_CLIPCHILDREN, 0, 0, 0,
                        0, nullptr, nullptr, instance, this);
        if (!hwnd_) return false;
        auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
            return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, hwnd_,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        };
        title_ = make(WC_STATICW, L"", SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX | SS_ENDELLIPSIS, SettingsIds::kEditBarTitle);
        status_ = make(WC_STATICW, L"", SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX | SS_ENDELLIPSIS, SettingsIds::kEditBarStatus);
        for (const ButtonSpec& b : kButtons) make(WC_BUTTONW, b.text, BS_PUSHBUTTON, b.id);
        fontDpi_ = 0;
        ApplyStyle();
        hasShown_ = false;
    } else if (fontDpi_ != dpi_) {
        ApplyStyle();
    }
    const bool same = hasShown_ && content.dock == shown_.dock && content.dockArgb == shown_.dockArgb &&
                      content.status == shown_.status && content.canUndo == shown_.canUndo && content.drawerShown == shown_.drawerShown;
    if (!same) {
        dockColor_ = FromArgb(content.dockArgb);
        const std::wstring title = L"Customizing " + content.dock;
        SetWindowTextW(title_, title.c_str());
        SetWindowTextW(status_, content.status.c_str());
        if (HWND add = GetDlgItem(hwnd_, SettingsIds::kEditBarAdd)) SetWindowTextW(add, content.drawerShown ? L"Hide items" : L"Add items");
        if (HWND undo = GetDlgItem(hwnd_, SettingsIds::kEditBarUndo)) EnableWindow(undo, content.canUndo);
        // What it says (automation, accessibility).
        SetWindowTextW(hwnd_, (title + L" \x00B7 " + content.status).c_str());
        HDC dc = GetDC(hwnd_);
        HGDIOBJ old = SelectObject(dc, bold_);
        SIZE ext{};
        GetTextExtentPoint32W(dc, title.c_str(), static_cast<int>(title.size()), &ext);
        SelectObject(dc, old);
        ReleaseDC(hwnd_, dc);
        titleW_ = ext.cx;
        shown_ = content;
        hasShown_ = true;
    }
    int buttonsW = 0;
    for (const ButtonSpec& b : kButtons) buttonsW += Px(b.w) + Px(8);
    const int workW = std::max<int>(Px(240), work.right - work.left - Px(16));
    // Round 12 phase 12H: Layout keeps Px(8) at the right end, not Px(4): the title had 4 px less
    // than its text and read "Customizing m…" for a dock named "main" (measured, 100 %); 4 px more
    // for the static's own rounding.
    const int w = std::min(std::max(Px(16) + std::min(titleW_, Px(220)) + Px(12) + Px(kStatusW) + buttonsW + Px(8) + Px(4), Px(420)), workW);
    const int h = Px(kBarH), gap = Px(kDockGap);
    // Beside the dock, on the screen's side of it, at its end (a floating bar: above it, below it
    // near the top of the screen).
    int x = bar.right - w, y = bar.top - gap - h;
    if (!floating) {
        switch (edge) {
            case DockEdge::Top: y = bar.bottom + gap; break;
            case DockEdge::Left:
                x = bar.right + gap;
                y = bar.bottom - h;
                break;
            case DockEdge::Right:
                x = bar.left - gap - w;
                y = bar.bottom - h;
                break;
            case DockEdge::Bottom:
            default: break;
        }
    } else if (y < work.top) {
        y = bar.bottom + gap;
    }
    x = std::clamp(x, static_cast<int>(work.left), std::max(static_cast<int>(work.left), static_cast<int>(work.right) - w));
    y = std::clamp(y, static_cast<int>(work.top), std::max(static_cast<int>(work.top), static_cast<int>(work.bottom) - h));
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    Layout();
    if (!same) InvalidateRect(hwnd_, nullptr, TRUE);
    return true;
}

void EditBar::Layout() {
    if (!hwnd_) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int bh = Px(28), by = (rc.bottom - bh) / 2;
    int x = rc.right - Px(8);
    for (const ButtonSpec& b : kButtons) {
        x -= Px(b.w);
        if (HWND c = GetDlgItem(hwnd_, b.id)) SetWindowPos(c, nullptr, x, by, Px(b.w), bh, SWP_NOZORDER | SWP_NOACTIVATE);
        x -= Px(8);
    }
    x -= Px(kStatusW);
    SetWindowPos(status_, nullptr, x, 0, Px(kStatusW), rc.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(title_, nullptr, Px(16), 0, std::max(0, x - Px(12) - Px(16)), rc.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT CALLBACK EditBar::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    EditBar* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<EditBar*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<EditBar*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        if (self->hwnd_ == hwnd) self->hwnd_ = nullptr;
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return self->WndProc(msg, wp, lp);
}

LRESULT EditBar::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;  // the buttons take the click, the foreground (the dock) stays
        case WM_ERASEBKGND: {
            HDC dc = reinterpret_cast<HDC>(wp);
            RECT rc{};
            GetClientRect(hwnd_, &rc);
            FillRect(dc, &rc, brush_);
            // The dock colour's edge at the left (its identity, as the settings header shows it) and a
            // hairline border.
            HBRUSH edge = CreateSolidBrush(palette_.highContrast ? palette_.text : dockColor_);
            RECT band{0, 0, Px(4), rc.bottom};
            FillRect(dc, &band, edge);
            DeleteObject(edge);
            HBRUSH hair = CreateSolidBrush(palette_.textDisabled);
            FrameRect(dc, &rc, hair);
            DeleteObject(hair);
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HDC dc = reinterpret_cast<HDC>(wp);
            const HWND c = reinterpret_cast<HWND>(lp);
            SetTextColor(dc, c == title_ && !palette_.highContrast ? dockColor_ : c == status_ ? palette_.textSecondary : palette_.text);
            SetBkColor(dc, palette_.layer);
            return reinterpret_cast<LRESULT>(brush_);
        }
        case WM_COMMAND:
            if (HIWORD(wp) == BN_CLICKED && command_) {
                // The handler may close (destroy) this bar: nothing of it is used afterwards.
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
