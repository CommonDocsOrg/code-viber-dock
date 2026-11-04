#include "TextEditorWindow.h"

#include <commctrl.h>
#include <shellscalingapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <cwctype>
#include <map>
#include <utility>

#include "resource.h"

namespace {

// Layout (DIPs).
constexpr float kWidth = 420.0f;
constexpr float kHeight = 320.0f;        // multi-line
constexpr float kSingleHeight = 140.0f;  // single-line (fixed)
constexpr float kMinWidth = 280.0f;
constexpr float kMinHeight = 170.0f;
constexpr float kMargin = 12.0f;
constexpr float kGap = 8.0f;
constexpr float kButtonW = 88.0f;
constexpr float kButtonH = 28.0f;
constexpr float kSingleEditH = 28.0f;
constexpr float kStatusH = 20.0f;
constexpr float kAnchorGap = 8.0f;  // the widget's box -> the window

// The sizes the editors had when they closed (DIPs), per key, for this session.
std::map<std::wstring, std::pair<float, float>>& Sizes() {
    static std::map<std::wstring, std::pair<float, float>> sizes;
    return sizes;
}

std::wstring ToCrLf(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size() + s.size() / 16);
    for (wchar_t c : s) {
        if (c == L'\n') out.push_back(L'\r');
        if (c != L'\r') out.push_back(c);
    }
    return out;
}

std::wstring ToLf(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'\r') {
            if (i + 1 < s.size() && s[i + 1] == L'\n') continue;  // "\r\n" -> "\n"
            out.push_back(L'\n');
            continue;
        }
        out.push_back(s[i]);
    }
    return out;
}

bool CtrlDown() { return (GetKeyState(VK_CONTROL) & 0x8000) != 0; }
bool ShiftDown() { return (GetKeyState(VK_SHIFT) & 0x8000) != 0; }
bool AltDown() { return (GetKeyState(VK_MENU) & 0x8000) != 0; }

// The window's rect next to `a` (the widget's box): on `side` when it fits there, else the opposite
// side, else beside it; always inside the work area `wa`.
RECT PlaceNear(const RECT& a, int w, int h, TextEditorWindow::Side side, int gap, const RECT& wa) {
    using Side = TextEditorWindow::Side;
    w = std::min<int>(w, wa.right - wa.left);
    h = std::min<int>(h, wa.bottom - wa.top);
    const int cx = (a.left + a.right) / 2, cy = (a.top + a.bottom) / 2;
    auto at = [&](Side s, RECT& out) {
        int x = 0, y = 0;
        bool fits = false;
        switch (s) {
            case Side::Above: x = cx - w / 2; y = a.top - gap - h; fits = y >= wa.top; break;
            case Side::Below: x = cx - w / 2; y = a.bottom + gap; fits = y + h <= wa.bottom; break;
            case Side::Right: x = a.right + gap; y = cy - h / 2; fits = x + w <= wa.right; break;
            case Side::Left:
            default: x = a.left - gap - w; y = cy - h / 2; fits = x >= wa.left; break;
        }
        x = std::max<int>(wa.left, std::min<int>(x, wa.right - w));
        y = std::max<int>(wa.top, std::min<int>(y, wa.bottom - h));
        out = RECT{x, y, x + w, y + h};
        return fits;
    };
    Side order[4] = {side, side, side, side};
    switch (side) {
        case Side::Above: order[1] = Side::Below; order[2] = Side::Right; order[3] = Side::Left; break;
        case Side::Below: order[1] = Side::Above; order[2] = Side::Right; order[3] = Side::Left; break;
        case Side::Right: order[1] = Side::Left; order[2] = Side::Above; order[3] = Side::Below; break;
        case Side::Left:
        default: order[1] = Side::Right; order[2] = Side::Above; order[3] = Side::Below; break;
    }
    RECT first{};
    for (int i = 0; i < 4; ++i) {
        RECT r{};
        const bool fits = at(order[i], r);
        if (i == 0) first = r;
        if (fits) return r;
    }
    return first;  // nowhere free: on the preferred side, pushed onto the monitor
}

}  // namespace

// --- lifetime ------------------------------------------------------------------------------------

TextEditorWindow* TextEditorWindow::Open(HWND owner, const RECT& anchor, bool topmost, Options options, Done done) {
    auto* w = new TextEditorWindow();
    w->options_ = std::move(options);
    w->done_ = std::move(done);
    if (!w->Create(owner, anchor, topmost)) {
        // Not self-deleting yet; `done` is not called (the caller reports the failure itself).
        w->finished_ = true;
        w->done_ = nullptr;
        if (w->hwnd_) DestroyWindow(w->hwnd_);
        delete w;
        return nullptr;
    }
    return w;
}

TextEditorWindow::~TextEditorWindow() {
    for (HGDIOBJ o : {static_cast<HGDIOBJ>(editFont_), static_cast<HGDIOBJ>(uiFont_), static_cast<HGDIOBJ>(layerBrush_),
                      static_cast<HGDIOBJ>(controlBrush_)}) {
        if (o) DeleteObject(o);
    }
}

bool TextEditorWindow::Create(HWND owner, const RECT& anchor, bool topmost) {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &TextEditorWindow::WndProcThunk;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    // The monitor of the widget's box, at its DPI.
    HMONITOR mon = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(mon, &mi)) return false;
    UINT dx = 96, dy = 96;
    if (SUCCEEDED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy))) dpi_ = dx;
    float wDip = kWidth, hDip = options_.multiline ? kHeight : kSingleHeight;
    if (auto it = Sizes().find(options_.sizeKey); it != Sizes().end() && !options_.sizeKey.empty()) {
        wDip = std::max(kMinWidth, it->second.first);
        if (options_.multiline) hDip = std::max(kMinHeight, it->second.second);
    }
    const RECT r = PlaceNear(anchor, Px(wDip), Px(hDip), options_.side, Px(kAnchorGap), mi.rcWork);
    const std::wstring title = options_.title.empty() ? std::wstring(L"Edit") : options_.title;
    hwnd_ = CreateWindowExW(topmost ? WS_EX_TOPMOST : 0, kClassName, title.c_str(),
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN, r.left, r.top,
                            r.right - r.left, r.bottom - r.top, owner, nullptr, instance, this);
    if (!hwnd_) return false;
    if (const UINT d = GetDpiForWindow(hwnd_)) dpi_ = d;

    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, DWORD ex, int id) {
        return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, hwnd_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    };
    const DWORD editStyle = options_.multiline
                                ? WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | ES_NOHIDESEL
                                : WS_TABSTOP | ES_AUTOHSCROLL | ES_NOHIDESEL;
    edit_ = make(L"EDIT", L"", editStyle, WS_EX_CLIENTEDGE, kEdit);
    ok_ = make(L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, 0, kOk);
    cancel_ = make(L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, 0, kCancel);
    status_ = make(L"STATIC", L"", SS_LEFTNOWORDWRAP | SS_NOPREFIX | SS_CENTERIMAGE, 0, kStatus);
    if (!edit_ || !ok_ || !cancel_ || !status_) return false;
    for (HWND c : {edit_, ok_, cancel_}) SetWindowSubclass(c, &TextEditorWindow::ChildProc, 1, reinterpret_cast<DWORD_PTR>(this));
    ApplyTheme();
    const std::wstring text = options_.multiline ? ToCrLf(options_.text) : options_.text;
    SetWindowTextW(edit_, text.c_str());
    if (!options_.multiline && !options_.placeholder.empty()) {
        SendMessageW(edit_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(options_.placeholder.c_str()));
    }
    SendMessageW(edit_, EM_EMPTYUNDOBUFFER, 0, 0);  // Ctrl+Z goes back to the text as given, no further
    if (options_.multiline) {
        const int end = GetWindowTextLengthW(edit_);
        SendMessageW(edit_, EM_SETSEL, static_cast<WPARAM>(end), end);  // the caret at the end
        SendMessageW(edit_, EM_SCROLLCARET, 0, 0);
    } else {
        SendMessageW(edit_, EM_SETSEL, 0, -1);
    }
    Layout();
    UpdateStatus();
    selfDelete_ = true;
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    SetFocus(edit_);
    return true;
}

void TextEditorWindow::Finish(bool ok) {
    if (finished_ || !hwnd_) return;
    finished_ = true;
    const std::wstring text = ok ? Text() : std::wstring();
    Done done = std::move(done_);
    done_ = nullptr;
    HWND h = hwnd_;
    if (done) done(ok, text);  // the widget's promise (it forgets this editor first)
    if (IsWindow(h)) DestroyWindow(h);  // deletes the object (WM_NCDESTROY)
}

void TextEditorWindow::Abandon() {
    finished_ = true;
    done_ = nullptr;
    if (hwnd_) DestroyWindow(hwnd_);
}

// --- look ----------------------------------------------------------------------------------------

void TextEditorWindow::ApplyTheme() {
    palette_ = DarkMode::CurrentPalette();
    const bool dark = palette_.dark;
    DarkMode::SetDarkCaption(hwnd_, dark);
    DarkMode::ThemeControl(ok_, dark);
    DarkMode::ThemeControl(cancel_, dark);
    if (options_.multiline) {
        // The scroll bar in the Explorer dark style (the edit-box style leaves it light).
        SetWindowTheme(edit_, dark ? L"DarkMode_Explorer" : nullptr, nullptr);
    } else {
        DarkMode::ThemeControl(edit_, dark);
    }
    if (layerBrush_) DeleteObject(layerBrush_);
    if (controlBrush_) DeleteObject(controlBrush_);
    layerBrush_ = CreateSolidBrush(palette_.layer);
    controlBrush_ = CreateSolidBrush(palette_.control);
    if (editFont_) DeleteObject(editFont_);
    editFont_ = CreateFontW(-MulDiv(10, static_cast<int>(dpi_), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    LOGFONTW lf{};
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi_)) lf = ncm.lfMessageFont;
    if (uiFont_) DeleteObject(uiFont_);
    uiFont_ = CreateFontIndirectW(&lf);
    SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(editFont_), TRUE);
    for (HWND c : {ok_, cancel_, status_}) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);
    SendMessageW(edit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(Px(4), Px(4)));
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void TextEditorWindow::Layout() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int m = Px(kMargin), gap = Px(kGap), bw = Px(kButtonW), bh = Px(kButtonH);
    const int rowY = std::max<int>(m, rc.bottom - m - bh);
    const int cancelX = rc.right - m - bw, okX = cancelX - gap - bw;
    MoveWindow(cancel_, cancelX, rowY, bw, bh, TRUE);
    MoveWindow(ok_, okX, rowY, bw, bh, TRUE);
    const int sh = Px(kStatusH);
    MoveWindow(status_, m, rowY + (bh - sh) / 2, std::max(0, okX - gap - m), sh, TRUE);
    const int editH = options_.multiline ? std::max(0, rowY - gap - m) : Px(kSingleEditH);
    MoveWindow(edit_, m, m, std::max(0, static_cast<int>(rc.right) - 2 * m), editH, TRUE);
}

std::wstring TextEditorWindow::Text() const {
    const int n = GetWindowTextLengthW(edit_);
    std::wstring t(static_cast<size_t>(std::max(0, n)) + 1, L'\0');
    const int got = n > 0 ? GetWindowTextW(edit_, t.data(), n + 1) : 0;
    t.resize(static_cast<size_t>(std::max(0, got)));
    return ToLf(t);
}

void TextEditorWindow::UpdateStatus() {
    const int n = GetWindowTextLengthW(edit_);
    std::wstring raw(static_cast<size_t>(std::max(0, n)) + 1, L'\0');
    const int got = n > 0 ? GetWindowTextW(edit_, raw.data(), n + 1) : 0;
    raw.resize(static_cast<size_t>(std::max(0, got)));
    // The count is the script's (a line break is one character); the edit counts "\r\n" as two, so
    // its limit grows by one per line break.
    const size_t breaks = static_cast<size_t>(std::count(raw.begin(), raw.end(), L'\r'));
    const size_t count = raw.size() - breaks;
    SendMessageW(edit_, EM_SETLIMITTEXT, static_cast<WPARAM>(options_.maxLength + breaks), 0);
    atLimit_ = count >= options_.maxLength;
    wchar_t buf[128];
    if (atLimit_) {
        swprintf_s(buf, L"%zu characters (the limit)", count);
    } else {
        swprintf_s(buf, L"%zu character%s", count, count == 1 ? L"" : L"s");
    }
    SetWindowTextW(status_, buf);
    InvalidateRect(status_, nullptr, TRUE);
}

// --- keyboard ------------------------------------------------------------------------------------

void TextEditorWindow::FocusNext(HWND from, bool back) {
    HWND order[3] = {edit_, ok_, cancel_};
    int i = 0;
    while (i < 3 && order[i] != from) ++i;
    if (i == 3) i = 0;
    i = (i + (back ? 2 : 1)) % 3;
    SetFocus(order[i]);
    if (order[i] == edit_ && !options_.multiline) SendMessageW(edit_, EM_SETSEL, 0, -1);
}

void TextEditorWindow::DeleteWordBack() {
    DWORD start = 0, end = 0;
    SendMessageW(edit_, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
    if (start == end) {
        const int n = GetWindowTextLengthW(edit_);
        std::wstring t(static_cast<size_t>(std::max(0, n)) + 1, L'\0');
        const int got = n > 0 ? GetWindowTextW(edit_, t.data(), n + 1) : 0;
        t.resize(static_cast<size_t>(std::max(0, got)));
        size_t p = std::min<size_t>(start, t.size());
        while (p > 0 && std::iswspace(t[p - 1])) --p;   // the spaces (and line breaks) before the caret
        while (p > 0 && !std::iswspace(t[p - 1])) --p;  // then the word
        start = static_cast<DWORD>(p);
        SendMessageW(edit_, EM_SETSEL, start, end);
    }
    SendMessageW(edit_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L""));
}

bool TextEditorWindow::OnKey(HWND from, UINT msg, WPARAM wp) {
    const bool isEdit = from == edit_;
    if (msg == WM_KEYDOWN) {
        switch (wp) {
            case VK_ESCAPE:
                Finish(false);
                return true;
            case VK_RETURN:
                if (CtrlDown() || (isEdit && !options_.multiline)) {
                    Finish(true);
                    return true;
                }
                if (!isEdit) {  // a focused button: pressed
                    SendMessageW(hwnd_, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(from), BN_CLICKED), reinterpret_cast<LPARAM>(from));
                    return true;
                }
                return false;  // a line break
            case VK_TAB:
                if (CtrlDown() || AltDown()) return false;
                FocusNext(from, ShiftDown());
                return true;
            case 'A':
                if (isEdit && CtrlDown() && !AltDown()) {
                    SendMessageW(edit_, EM_SETSEL, 0, -1);
                    return true;
                }
                return false;
            case VK_BACK:
                if (isEdit && CtrlDown() && !AltDown()) {
                    DeleteWordBack();
                    return true;
                }
                return false;
            default:
                return false;
        }
    }
    if (msg == WM_CHAR) {
        // The characters of the keys handled above (and Ctrl+Enter's line feed) never reach the text.
        switch (wp) {
            case 0x1B:  // Escape
            case L'\t':
            case 0x0A:  // Ctrl+Enter
            case 0x01:  // Ctrl+A
            case 0x7F:  // Ctrl+Backspace
                return true;
            case L'\r':
                return !isEdit || !options_.multiline;
            default:
                return false;
        }
    }
    return false;
}

LRESULT CALLBACK TextEditorWindow::ChildProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    auto* self = reinterpret_cast<TextEditorWindow*>(ref);
    switch (msg) {
        case WM_KEYDOWN:
        case WM_CHAR:
            if (self && self->OnKey(hwnd, msg, wp)) return 0;  // (the object may be gone now)
            break;
        case WM_PAINT:
            // The placeholder of a multi-line editor (EM_SETCUEBANNER is single-line only).
            if (self && hwnd == self->edit_ && self->options_.multiline && !self->options_.placeholder.empty() &&
                GetWindowTextLengthW(hwnd) == 0) {
                const LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
                HDC dc = GetDC(hwnd);
                if (dc) {
                    RECT fr{};
                    SendMessageW(hwnd, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&fr));
                    HGDIOBJ old = SelectObject(dc, self->editFont_);
                    SetBkMode(dc, TRANSPARENT);
                    SetTextColor(dc, self->palette_.textDisabled);
                    DrawTextW(dc, self->options_.placeholder.c_str(), -1, &fr, DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
                    SelectObject(dc, old);
                    ReleaseDC(hwnd, dc);
                }
                return r;
            }
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, &TextEditorWindow::ChildProc, id);
            break;
        default:
            break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// --- window --------------------------------------------------------------------------------------

LRESULT CALLBACK TextEditorWindow::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    TextEditorWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<TextEditorWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        if (self) self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<TextEditorWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    const LRESULT r = self->WndProc(msg, wp, lp);
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        self->hwnd_ = nullptr;
        if (self->selfDelete_) delete self;
    }
    return r;
}

LRESULT TextEditorWindow::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND: {
            const int id = LOWORD(wp), code = HIWORD(wp);
            if (id == kEdit && code == EN_CHANGE) {
                UpdateStatus();
                return 0;
            }
            if (code == BN_CLICKED && (id == kOk || id == kCancel)) {
                Finish(id == kOk);
                return 0;
            }
            break;
        }
        case WM_CLOSE:
            Finish(false);
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) break;
            [[fallthrough]];
        case WM_SETFOCUS: {
            HWND f = GetFocus();
            if (f != edit_ && f != ok_ && f != cancel_ && edit_) SetFocus(edit_);
            return 0;
        }
        case WM_SIZE:
            if (edit_) Layout();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize = POINT{Px(kMinWidth), Px(options_.multiline ? kMinHeight : kSingleHeight)};
            if (!options_.multiline) mmi->ptMaxTrackSize.y = Px(kSingleHeight);
            return 0;
        }
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp);
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            ApplyTheme();
            Layout();
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (lp && CompareStringOrdinal(reinterpret_cast<const wchar_t*>(lp), -1, L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL) {
                ApplyTheme();
            }
            return 0;
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, palette_.text);
            SetBkColor(dc, palette_.control);
            return reinterpret_cast<LRESULT>(controlBrush_);
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wp);
            const bool warn = reinterpret_cast<HWND>(lp) == status_ && atLimit_;
            SetTextColor(dc, warn ? (palette_.dark ? RGB(0xFF, 0xB0, 0x4A) : RGB(0xB0, 0x4A, 0x00)) : palette_.textSecondary);
            SetBkColor(dc, palette_.layer);
            return reinterpret_cast<LRESULT>(layerBrush_);
        }
        case WM_CTLCOLORBTN:
            return reinterpret_cast<LRESULT>(layerBrush_);
        case WM_ERASEBKGND: {
            RECT rc{};
            GetClientRect(hwnd_, &rc);
            FillRect(reinterpret_cast<HDC>(wp), &rc, layerBrush_);
            return 1;
        }
        case WM_DESTROY: {
            // The size for the next editor of this widget (this session).
            RECT r{};
            if (!options_.sizeKey.empty() && GetWindowRect(hwnd_, &r) && !IsIconic(hwnd_)) {
                const float s = static_cast<float>(dpi_) / 96.0f;
                Sizes()[options_.sizeKey] = {static_cast<float>(r.right - r.left) / s, static_cast<float>(r.bottom - r.top) / s};
            }
            if (!finished_) {  // destroyed with its owner (the dock went): cancelled
                finished_ = true;
                Done done = std::move(done_);
                done_ = nullptr;
                if (done) done(false, std::wstring());
            }
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
