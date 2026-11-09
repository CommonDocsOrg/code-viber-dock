// Round 12 phase 12G: the tips on the dock (CoachMarks.h).

#include "CoachMarks.h"

#include <commctrl.h>

#include <algorithm>

#include "SettingsIds.h"

namespace {

bool RegisterTipClass(HINSTANCE instance, WNDPROC proc) {
    static bool registered = false;
    if (registered) return true;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = CoachMark::kClassName;
    registered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return registered;
}

// Layout of the bubble, DIPs: a 4-DIP accent edge at the left (as the Keep banner), 16 padding,
// the sentence, then a row with "Tip n of 3" at the left and the two buttons at the right.
constexpr int kPad = 16, kEdge = 4, kButtonH = 28, kSkipW = 84, kNextW = 72, kGap = 8, kRowGap = 12;

}  // namespace

CoachMark::~CoachMark() {
    Close();
    for (HFONT f : {font_, fontSmall_}) {
        if (f) DeleteObject(f);
    }
    if (brush_) DeleteObject(brush_);
}

const wchar_t* CoachMark::Text(int index) {
    switch (index) {
        case 0: return L"Right-click an empty part of the dock for its menu and settings.";
        case 1: return L"Choose Customize this dock to add, move and remove items. The drawer opens beside the dock.";
        case 2: return L"While customizing, drag an empty part of the dock to move it. It snaps to the centre.";
        default: return L"";
    }
}

void CoachMark::Close() {
    if (hwnd_) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    text_ = count_ = skip_ = next_ = nullptr;
    shownIndex_ = -1;
}

void CoachMark::ApplyStyle() {
    palette_ = DarkMode::CurrentPalette();
    if (brush_) DeleteObject(brush_);
    brush_ = CreateSolidBrush(palette_.layer);
    if (fontDpi_ != dpi_ || !font_) {
        NONCLIENTMETRICSW ncm{sizeof(ncm)};
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi_);
        for (HFONT f : {font_, fontSmall_}) {
            if (f) DeleteObject(f);
        }
        font_ = CreateFontIndirectW(&ncm.lfMessageFont);
        LOGFONTW minor = ncm.lfMessageFont;
        minor.lfHeight = minor.lfHeight * 9 / 10;
        fontSmall_ = CreateFontIndirectW(&minor);
        fontDpi_ = dpi_;
    }
    for (HWND c : {text_, count_, skip_, next_}) {
        if (!c) continue;
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(c == count_ ? fontSmall_ : font_), FALSE);
        if (c == skip_ || c == next_) DarkMode::ThemeControl(c, palette_.dark);
    }
}

int CoachMark::Measure(int width) {
    // The sentence wrapped at the text column's width, then the button row.
    const std::wstring text = shownIndex_ >= 0 ? Text(shownIndex_) : L"";
    RECT r{0, 0, std::max(1, width - Px(kEdge + kPad + kPad)), 0};
    HDC dc = GetDC(hwnd_);
    HGDIOBJ old = SelectObject(dc, font_);
    DrawTextW(dc, text.c_str(), -1, &r, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
    SelectObject(dc, old);
    ReleaseDC(hwnd_, dc);
    return Px(kPad) + static_cast<int>(r.bottom - r.top) + Px(kRowGap) + Px(kButtonH) + Px(kPad - 4);
}

bool CoachMark::Show(HINSTANCE instance, int index, int count, bool last, const RECT& bar, const RECT& work, DockEdge edge,
                     bool floating, UINT dpi) {
    if (index < 0 || !RegisterTipClass(instance, &CoachMark::WndProcThunk)) return false;
    dpi_ = std::max<UINT>(dpi, USER_DEFAULT_SCREEN_DPI / 2);
    if (!hwnd_) {
        // Topmost (the dock is topmost too), never activated: a click on a button still works.
        CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClassName, L"WinDock tip", WS_POPUP | WS_CLIPCHILDREN, 0, 0, 0,
                        0, nullptr, nullptr, instance, this);
        if (!hwnd_) return false;
        auto make = [&](const wchar_t* cls, DWORD style, int id) {
            return CreateWindowExW(0, cls, L"", WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, hwnd_,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        };
        text_ = make(WC_STATICW, SS_LEFT | SS_NOPREFIX, SettingsIds::kTipText);
        count_ = make(WC_STATICW, SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE, SettingsIds::kTipCount);
        skip_ = make(WC_BUTTONW, BS_PUSHBUTTON, SettingsIds::kTipSkip);
        next_ = make(WC_BUTTONW, BS_PUSHBUTTON, SettingsIds::kTipNext);
        SetWindowTextW(skip_, L"Skip tips");
        fontDpi_ = 0;
        ApplyStyle();
    } else if (fontDpi_ != dpi_) {
        ApplyStyle();
    }
    const bool changed = index != shownIndex_ || last != shownLast_;
    shownIndex_ = index;
    shownLast_ = last;
    if (changed) {
        const std::wstring of = L"Tip " + std::to_wstring(index + 1) + L" of " + std::to_wstring(std::max(count, index + 1));
        SetWindowTextW(text_, Text(index));
        SetWindowTextW(count_, of.c_str());
        SetWindowTextW(next_, last ? L"Done" : L"Next");
        SetWindowTextW(hwnd_, (of + L": " + Text(index)).c_str());  // what it says (automation)
    }
    // The size: 320 DIPs (less on a narrow display), the height its sentence needs.
    const int w = std::min(Px(kWidthDip), std::max<int>(Px(200), work.right - work.left - Px(32)));
    const int h = Measure(w), gap = Px(12);
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
    if (changed) InvalidateRect(hwnd_, nullptr, TRUE);
    return true;
}

void CoachMark::Layout() {
    if (!hwnd_) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int left = Px(kEdge + kPad), right = rc.right - Px(kPad);
    const int by = rc.bottom - Px(kPad - 4) - Px(kButtonH);
    SetWindowPos(text_, nullptr, left, Px(kPad), std::max(0, right - left), std::max(0, by - Px(kRowGap) - Px(kPad)),
                 SWP_NOZORDER | SWP_NOACTIVATE);
    int x = right - Px(kNextW);
    SetWindowPos(next_, nullptr, x, by, Px(kNextW), Px(kButtonH), SWP_NOZORDER | SWP_NOACTIVATE);
    x -= Px(kGap) + Px(kSkipW);
    SetWindowPos(skip_, nullptr, x, by, Px(kSkipW), Px(kButtonH), SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(count_, nullptr, left, by, std::max(0, x - Px(kGap) - left), Px(kButtonH), SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT CALLBACK CoachMark::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    CoachMark* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<CoachMark*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<CoachMark*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        if (self->hwnd_ == hwnd) {
            self->hwnd_ = nullptr;
            self->text_ = self->count_ = self->skip_ = self->next_ = nullptr;
            self->shownIndex_ = -1;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return self->WndProc(msg, wp, lp);
}

LRESULT CoachMark::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;  // the buttons take the click, the foreground stays where it is
        case WM_ERASEBKGND: {
            HDC dc = reinterpret_cast<HDC>(wp);
            RECT rc{};
            GetClientRect(hwnd_, &rc);
            FillRect(dc, &rc, brush_);
            // An accent edge at the left and a hairline border, as the Keep banner.
            HBRUSH accent = CreateSolidBrush(palette_.highContrast ? palette_.text : palette_.accent);
            RECT edge{0, 0, Px(kEdge), rc.bottom};
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
            SetTextColor(dc, reinterpret_cast<HWND>(lp) == count_ ? palette_.textSecondary : palette_.text);
            SetBkColor(dc, palette_.layer);
            return reinterpret_cast<LRESULT>(brush_);
        }
        case WM_COMMAND:
            if (HIWORD(wp) == BN_CLICKED && command_) {
                // The handler may close (destroy) this bubble: nothing of it is used afterwards.
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
