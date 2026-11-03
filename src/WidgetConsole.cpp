#include "WidgetConsole.h"

#include <shellapi.h>
#include <shellscalingapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cwchar>
#include <iterator>

#include "JsonUtil.h"
#include "WidgetInstance.h"
#include "WidgetPackage.h"
#include "WidgetRuntime.h"
#include "resource.h"

namespace {

constexpr int kWidthDip = 760;
constexpr int kHeightDip = 440;
constexpr int kListWidthDip = 250;
constexpr int kMarginDip = 10;
constexpr int kButtonWDip = 96;
constexpr int kButtonHDip = 28;

const wchar_t* StateText(WidgetInstance::State s) {
    switch (s) {
        case WidgetInstance::State::Error: return L"error";
        case WidgetInstance::State::Suspended: return L"stopped";
        case WidgetInstance::State::Ok:
        default: return L"ok";
    }
}

}  // namespace

WidgetConsole::~WidgetConsole() {
    runtime_.SetListeners(nullptr, nullptr);
    if (hwnd_) DestroyWindow(hwnd_);
    for (HGDIOBJ o : {static_cast<HGDIOBJ>(font_), static_cast<HGDIOBJ>(mono_), static_cast<HGDIOBJ>(layerBrush_),
                      static_cast<HGDIOBJ>(controlBrush_)}) {
        if (o) DeleteObject(o);
    }
}

bool WidgetConsole::Create(HINSTANCE instance) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProcThunk;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    // Centred on the monitor under the cursor, at that monitor's DPI.
    POINT pt{};
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dx = 96, dy = 96;
    if (FAILED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy))) dx = 96;
    dpi_ = dx;
    const int w = Px(kWidthDip), h = Px(kHeightDip);
    const RECT& wa = mi.rcWork;
    hwnd_ = CreateWindowExW(0, kClassName, L"WinDock widget console", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 2, w, h, nullptr,
                            nullptr, instance, this);
    if (!hwnd_) return false;
    dpi_ = GetDpiForWindow(hwnd_);
    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, DWORD ex, int id) {
        return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, hwnd_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    };
    // Owner-drawn rows: the selection in the palette's colours (a plain list box's dark selection
    // is unreadable).
    list_ = make(L"LISTBOX", L"",
                 WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                 WS_EX_CLIENTEDGE, kList);
    log_ = make(L"EDIT", L"", WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL |
                                   ES_AUTOHSCROLL | ES_NOHIDESEL,
                WS_EX_CLIENTEDGE, kLog);
    reload_ = make(L"BUTTON", L"Reload", WS_TABSTOP | BS_PUSHBUTTON, 0, kReload);
    folder_ = make(L"BUTTON", L"Open folder", WS_TABSTOP | BS_PUSHBUTTON, 0, kFolder);
    clear_ = make(L"BUTTON", L"Clear", WS_TABSTOP | BS_PUSHBUTTON, 0, kClear);
    status_ = make(L"STATIC", L"", SS_LEFTNOWORDWRAP | SS_NOPREFIX, 0, kStatus);
    SendMessageW(log_, EM_SETLIMITTEXT, 0, 0);
    ApplyTheme();
    Layout();
    runtime_.SetListeners(
        [this](WidgetInstance* w) {
            if (w == shown_) PostRefresh(false);
        },
        [this] { PostRefresh(true); });
    FillList();
    FillLog();
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
    return true;
}

void WidgetConsole::Activate() {
    if (!hwnd_) return;
    if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
}

void WidgetConsole::ApplyTheme() {
    palette_ = DarkMode::CurrentPalette();
    DarkMode::SetDarkCaption(hwnd_, palette_.dark);
    for (HWND c : {list_, reload_, folder_, clear_}) DarkMode::ThemeControl(c, palette_.dark);
    // The log's scroll bars: the Explorer dark style (the edit-box style leaves them light).
    SetWindowTheme(log_, palette_.dark ? L"DarkMode_Explorer" : nullptr, nullptr);
    if (layerBrush_) DeleteObject(layerBrush_);
    if (controlBrush_) DeleteObject(controlBrush_);
    layerBrush_ = CreateSolidBrush(palette_.layer);
    controlBrush_ = CreateSolidBrush(palette_.control);
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    LOGFONTW lf{};
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi_)) lf = ncm.lfMessageFont;
    if (font_) DeleteObject(font_);
    font_ = CreateFontIndirectW(&lf);
    LOGFONTW mono = lf;
    wcscpy_s(mono.lfFaceName, L"Cascadia Mono");
    if (mono_) DeleteObject(mono_);
    mono_ = CreateFontIndirectW(&mono);
    for (HWND c : {list_, reload_, folder_, clear_, status_}) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    SendMessageW(log_, WM_SETFONT, reinterpret_cast<WPARAM>(mono_ ? mono_ : font_), TRUE);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void WidgetConsole::Layout() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int m = Px(kMarginDip), lw = Px(kListWidthDip), bw = Px(kButtonWDip), bh = Px(kButtonHDip);
    const int bottom = rc.bottom - m - bh;
    MoveWindow(list_, m, m, lw, std::max(0, bottom - 2 * m), TRUE);
    MoveWindow(log_, 2 * m + lw, m, std::max(0, static_cast<int>(rc.right) - 3 * m - lw), std::max(0, bottom - 2 * m), TRUE);
    MoveWindow(reload_, m, bottom, bw, bh, TRUE);
    MoveWindow(folder_, 2 * m + bw, bottom, bw, bh, TRUE);
    MoveWindow(clear_, 3 * m + 2 * bw, bottom, bw, bh, TRUE);
    MoveWindow(status_, 4 * m + 3 * bw, bottom + Px(5), std::max(0, static_cast<int>(rc.right) - 5 * m - 3 * bw), bh, TRUE);
}

WidgetInstance* WidgetConsole::Selected() const {
    const int sel = static_cast<int>(SendMessageW(list_, LB_GETCURSEL, 0, 0));
    if (sel < 0 || sel >= static_cast<int>(rows_.size())) return nullptr;
    WidgetInstance* w = rows_[static_cast<size_t>(sel)];
    return runtime_.Alive(w) ? w : nullptr;
}

void WidgetConsole::FillList() {
    listDirty_ = false;
    WidgetInstance* keep = shown_ && runtime_.Alive(shown_) ? shown_ : nullptr;
    rows_ = runtime_.Instances();
    SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
    SendMessageW(list_, LB_RESETCONTENT, 0, 0);
    int sel = rows_.empty() ? -1 : 0;
    for (size_t i = 0; i < rows_.size(); ++i) {
        const WidgetInstance* w = rows_[i];
        const std::wstring dock = JsonUtil::Utf8ToWide(w->DockInfo().dockId);
        const std::wstring row = (dock.empty() ? L"" : dock + L" · ") + w->Name() + L" (" + w->Key() + L") · " +
                                 StateText(w->GetState());
        SendMessageW(list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(row.c_str()));
        if (w == keep) sel = static_cast<int>(i);
    }
    SendMessageW(list_, LB_SETCURSEL, static_cast<WPARAM>(sel), 0);
    SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list_, nullptr, TRUE);
    wchar_t status[96];
    swprintf_s(status, L"%zu widget%s", rows_.size(), rows_.size() == 1 ? L"" : L"s");
    SetWindowTextW(status_, status);
}

void WidgetConsole::FillLog() {
    WidgetInstance* w = Selected();
    if (w != shown_) clearedAt_ = 0;
    shown_ = w;
    std::wstring text;
    int lines = 0;
    if (w) {
        const auto& log = w->LogLines();
        const uint64_t first = w->LogTotal() - log.size();  // total index of log.front()
        for (size_t i = 0; i < log.size(); ++i) {
            if (first + i < clearedAt_) continue;
            const WidgetInstance::LogLine& l = log[i];
            wchar_t head[48];
            swprintf_s(head, L"%02u:%02u:%02u.%03u %-5s ", l.time.wHour, l.time.wMinute, l.time.wSecond,
                       l.time.wMilliseconds,
                       l.level == ScriptLogLevel::Error  ? L"error"
                       : l.level == ScriptLogLevel::Warn ? L"warn"
                                                         : L"log");
            std::wstring body = l.text;
            // Continuation lines (stacks) indented under the text.
            for (size_t p = body.find(L'\n'); p != std::wstring::npos; p = body.find(L'\n', p + 21)) {
                body.replace(p, 1, L"\r\n                   ");
            }
            text += head + body + L"\r\n";
            ++lines;
        }
    }
    shownLines_ = lines;
    SetWindowTextW(log_, text.c_str());
    // Scroll to the end (the newest line).
    const int len = GetWindowTextLengthW(log_);
    SendMessageW(log_, EM_SETSEL, static_cast<WPARAM>(len), len);
    SendMessageW(log_, EM_SCROLLCARET, 0, 0);
    EnableWindow(reload_, w != nullptr);
    EnableWindow(clear_, w != nullptr);
}

void WidgetConsole::PostRefresh(bool list) {
    if (list) listDirty_ = true;
    if (refreshPosted_ || !hwnd_) return;
    refreshPosted_ = PostMessageW(hwnd_, kRefreshMessage, 0, 0) != FALSE;
}

LRESULT CALLBACK WidgetConsole::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    WidgetConsole* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<WidgetConsole*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<WidgetConsole*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    const LRESULT r = self->WndProc(msg, wp, lp);
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        self->hwnd_ = nullptr;
        if (self->destroyed_) self->destroyed_();  // the owner frees the object later (posted)
    }
    return r;
}

LRESULT WidgetConsole::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case kRefreshMessage:
            refreshPosted_ = false;
            if (listDirty_) FillList();
            FillLog();
            return 0;
        case WM_SIZE:
            Layout();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize = POINT{Px(520), Px(260)};
            return 0;
        }
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp);
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            ApplyTheme();
            Layout();
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (lp && CompareStringOrdinal(reinterpret_cast<const wchar_t*>(lp), -1, L"ImmersiveColorSet", -1, TRUE) ==
                          CSTR_EQUAL) {
                ApplyTheme();
            }
            return 0;
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wp);
            const bool control = msg != WM_CTLCOLORSTATIC || reinterpret_cast<HWND>(lp) == log_;
            SetTextColor(dc, palette_.text);
            SetBkColor(dc, control ? palette_.control : palette_.layer);
            return reinterpret_cast<LRESULT>(control ? controlBrush_ : layerBrush_);
        }
        case WM_MEASUREITEM: {
            auto* mi = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
            if (mi->CtlID != kList) break;
            mi->itemHeight = static_cast<UINT>(Px(22));
            return TRUE;
        }
        case WM_DRAWITEM: {
            const auto* di = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
            if (di->CtlID != kList) break;
            const bool selected = (di->itemState & ODS_SELECTED) != 0;
            // Selected: the accent mixed into the control colour (readable in light and dark).
            auto mix = [](COLORREF a, COLORREF b, int pct) {
                return RGB((GetRValue(a) * (100 - pct) + GetRValue(b) * pct) / 100,
                           (GetGValue(a) * (100 - pct) + GetGValue(b) * pct) / 100,
                           (GetBValue(a) * (100 - pct) + GetBValue(b) * pct) / 100);
            };
            const COLORREF bg = selected ? mix(palette_.control, palette_.accent, 35) : palette_.control;
            HBRUSH brush = CreateSolidBrush(bg);
            FillRect(di->hDC, &di->rcItem, brush);
            DeleteObject(brush);
            if (di->itemID != static_cast<UINT>(-1)) {
                wchar_t text[512]{};
                if (SendMessageW(list_, LB_GETTEXTLEN, di->itemID, 0) < static_cast<LRESULT>(std::size(text))) {
                    SendMessageW(list_, LB_GETTEXT, di->itemID, reinterpret_cast<LPARAM>(text));
                }
                SetBkMode(di->hDC, TRANSPARENT);
                SetTextColor(di->hDC, palette_.text);
                HGDIOBJ old = SelectObject(di->hDC, font_);
                RECT r = di->rcItem;
                r.left += Px(6);
                DrawTextW(di->hDC, text, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
                SelectObject(di->hDC, old);
            }
            if (di->itemState & ODS_FOCUS) DrawFocusRect(di->hDC, &di->rcItem);
            return TRUE;
        }
        case WM_CTLCOLORBTN:
            return reinterpret_cast<LRESULT>(layerBrush_);
        case WM_ERASEBKGND: {
            RECT rc{};
            GetClientRect(hwnd_, &rc);
            FillRect(reinterpret_cast<HDC>(wp), &rc, layerBrush_);
            return 1;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wp), code = HIWORD(wp);
            if (id == kList && code == LBN_SELCHANGE) {
                FillLog();
                return 0;
            }
            if (code != BN_CLICKED) break;
            WidgetInstance* w = Selected();
            if (id == kReload && w) {
                w->Note(L"reloaded from the Console");
                w->Reload();  // the runtime notifies the list and the log
            } else if (id == kClear && w) {
                clearedAt_ = w->LogTotal();
                FillLog();
            } else if (id == kFolder) {
                std::wstring folder = Widgets::Directory();
                if (w && w->Package() && w->Package()->source == Widgets::Package::Source::Folder) {
                    folder = w->Package()->folder;
                }
                CreateDirectoryW(folder.c_str(), nullptr);
                ShellExecuteW(hwnd_, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) DestroyWindow(hwnd_);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            runtime_.SetListeners(nullptr, nullptr);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
