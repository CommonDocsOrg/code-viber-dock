// Round 12 phase 12G: Quick setup (see QuickSetup.h).

#include "QuickSetup.h"

#include <commctrl.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cwchar>

#include "SettingsIds.h"
#include "Theme.h"
#include "resource.h"

namespace {

using namespace SettingsIds;

// Client sizes, DIPs (the plan: 960 x 640 on displays wider than 1400 DIPs, else 760 x 540).
constexpr int kBigW = 960, kBigH = 640, kSmallW = 760, kSmallH = 540, kBigFrom = 1400;
constexpr int kMargin = 24;
constexpr int kStepsTop = 16, kStepsH = 36;
constexpr int kHeadingTop = 64, kHeadingH = 36, kIntroTop = 102, kIntroH = 40;
constexpr int kContentTop = 150;
constexpr int kButtonH = 32;
constexpr int kRowHeadH = 20, kRowH = 44, kRowCtlH = 28;
constexpr int kLabelW = 196, kControlW = 360;  // the design rules' row: label + control
constexpr int kDropH = 300;                    // a drop-down combo's height with its list

const wchar_t* const kStepNames[QuickSetup::kSteps] = {L"Your docks", L"Their looks", L"How it behaves", L"Done"};
const wchar_t* const kEdgeNames[] = {L"Bottom", L"Top", L"Left", L"Right"};  // DockEdge order
const wchar_t* const kCornerNames[] = {L"Square", L"Soft", L"Round"};
const wchar_t* const kAutoHideNames[] = {L"Off", L"Always", L"When a window overlaps"};  // AutoHideMode order
const wchar_t* const kAnimationNames[] = {L"Full", L"Reduced", L"Off"};

COLORREF Mix(COLORREF a, COLORREF b, int percentA) {
    return RGB((GetRValue(a) * percentA + GetRValue(b) * (100 - percentA)) / 100,
               (GetGValue(a) * percentA + GetGValue(b) * (100 - percentA)) / 100,
               (GetBValue(a) * percentA + GetBValue(b) * (100 - percentA)) / 100);
}

void FillRound(HDC dc, const RECT& r, COLORREF c, int radius) {
    HBRUSH b = CreateSolidBrush(c);
    HPEN p = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(p);
    DeleteObject(b);
}

bool Register(HINSTANCE instance, const wchar_t* name, WNDPROC proc, LPCWSTR cursor, bool icon) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, cursor);
    if (icon) wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    wc.lpszClassName = name;
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

int ComboSel(HWND combo) {
    const LRESULT s = combo ? SendMessageW(combo, CB_GETCURSEL, 0, 0) : CB_ERR;
    return s == CB_ERR ? -1 : static_cast<int>(s);
}

void SetComboSel(HWND combo, int sel) {
    if (combo && ComboSel(combo) != sel) SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(sel), 0);
}

void SetText(HWND h, const std::wstring& text) {
    if (!h) return;
    wchar_t cur[512]{};
    GetWindowTextW(h, cur, static_cast<int>(std::size(cur)));
    if (text != cur) SetWindowTextW(h, text.c_str());
}

bool Checked(HWND box) { return box && SendMessageW(box, BM_GETCHECK, 0, 0) == BST_CHECKED; }
void SetCheck(HWND box, bool on) {
    if (box && Checked(box) != on) SendMessageW(box, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
}

// The display combo: "Display 1 (main)", "Display 2" ... then "Every display" (docks[].monitor:
// 0 / primary, 1 ..., each).
int MonitorCount() { return std::max(1, GetSystemMetrics(SM_CMONITORS)); }
int DisplaySel(int monitor) {
    const int n = MonitorCount();
    if (monitor == DockConfig::kMonitorEach) return n;
    if (monitor == DockConfig::kMonitorPrimary || monitor == 0) return 0;
    return monitor > 0 && monitor < n ? monitor : -1;  // a display that is not connected: no row
}

}  // namespace

QuickSetup::QuickSetup(QuickSetupHost host, bool firstRun, bool startWithWindows)
    : host_(std::move(host)), firstRun_(firstRun), start_(startWithWindows) {
    if (host_.config) hideTaskbar_ = host_.config->hideTaskbar;
}

QuickSetup::~QuickSetup() {
    // Destroyed by App (its shutdown): a name edit's focus loss on the way must not edit anything.
    closing_ = done_ = true;
    if (hwnd_) DestroyWindow(hwnd_);
    for (HFONT f : {font_, fontBold_, fontHeading_}) {
        if (f) DeleteObject(f);
    }
    if (layerBrush_) DeleteObject(layerBrush_);
    if (controlBrush_) DeleteObject(controlBrush_);
}

const DockConfig* QuickSetup::DockOf(const std::wstring& id) const {
    if (!host_.config || id.empty()) return nullptr;
    for (const DockConfig& d : host_.config->docks) {
        if (d.id == id && d.IsDock()) return &d;
    }
    return nullptr;
}

std::wstring QuickSetup::NameOf(const std::wstring& id) const {
    const DockConfig* d = DockOf(id);
    return d ? d->DisplayName() : id;
}

bool QuickSetup::Create(HINSTANCE instance, HWND owner) {
    instance_ = instance;
    if (!host_.config || !Register(instance, kClassName, WndProcThunk, IDC_ARROW, true) ||
        !Register(instance, kStepsClass, StepsProc, IDC_HAND, false) || !Register(instance, kRowsClass, RowsProc, IDC_ARROW, false) ||
        !SettingsGallery::Register(instance)) {
        return false;
    }
    // The first row: the first dock (the one the first run made).
    for (const DockConfig& d : host_.config->docks) {
        if (!d.IsDock()) continue;
        Row row;
        row.id = d.id;
        row.keepEntry = !firstRun_;  // a dock that was set up before: "As it is now" first
        if (firstRun_) {
            const size_t basic = Presets::IndexOf(L"basic");
            row.layoutSel = basic == SIZE_MAX ? 0 : static_cast<int>(basic);
        }
        rows_.push_back(std::move(row));
        break;
    }
    if (rows_.empty()) return false;
    looks_ = Presets::Looks();

    // The primary display (the app window is hidden): its DPI and size decide the window's.
    HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    UINT dx = USER_DEFAULT_SCREEN_DPI, dy = USER_DEFAULT_SCREEN_DPI;
    if (FAILED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy)) || dx == 0) dx = USER_DEFAULT_SCREEN_DPI;
    dpi_ = dx;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    const int workDip = MulDiv(mi.rcWork.right - mi.rcWork.left, USER_DEFAULT_SCREEN_DPI, static_cast<int>(dpi_));
    const bool big = workDip > kBigFrom;
    widthDip_ = big ? kBigW : kSmallW;
    heightDip_ = big ? kBigH : kSmallH;
    constexpr DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    constexpr DWORD exStyle = WS_EX_CONTROLPARENT | WS_EX_DLGMODALFRAME | WS_EX_APPWINDOW;
    auto rectFor = [&](UINT dpi) {
        RECT r{0, 0, MulDiv(widthDip_, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI), MulDiv(heightDip_, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI)};
        AdjustWindowRectExForDpi(&r, style, FALSE, exStyle, dpi);
        const int w = r.right - r.left, h = r.bottom - r.top;
        int x = (mi.rcWork.left + mi.rcWork.right - w) / 2, y = (mi.rcWork.top + mi.rcWork.bottom - h) / 2;
        x = std::max(static_cast<int>(mi.rcWork.left), std::min(x, static_cast<int>(mi.rcWork.right) - w));
        y = std::max(static_cast<int>(mi.rcWork.top), std::min(y, static_cast<int>(mi.rcWork.bottom) - h));
        return RECT{x, y, x + w, y + h};
    };
    RECT r = rectFor(dpi_);
    CreateWindowExW(exStyle, kClassName, firstRun_ ? L"Welcome to WinDock" : L"Quick setup", style, r.left, r.top, r.right - r.left,
                    r.bottom - r.top, owner, nullptr, instance, this);
    if (!hwnd_) return false;
    if (const UINT actual = GetDpiForWindow(hwnd_); actual && actual != dpi_) {
        dpi_ = actual;
        r = rectFor(dpi_);
        SetWindowPos(hwnd_, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    CreateFonts();
    auto make = [&](HWND parent, const wchar_t* cls, const wchar_t* text, DWORD s, int id, DWORD ex = 0) {
        return CreateWindowExW(ex, cls, text, WS_CHILD | s, 0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                               instance, nullptr);
    };
    // The tab order is the creation order: the steps, the step's controls, then the buttons.
    steps_ = make(hwnd_, kStepsClass, L"", WS_VISIBLE | WS_TABSTOP, kSetupSteps);
    if (steps_) SetWindowLongPtrW(steps_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    heading_ = make(hwnd_, WC_STATICW, L"", WS_VISIBLE | SS_LEFT | SS_NOPREFIX, kSetupHeading);
    intro_ = make(hwnd_, WC_STATICW, L"", WS_VISIBLE | SS_LEFT | SS_NOPREFIX, kSetupIntro);
    // Step 1: the rows (a container: its children's ids are 10 * row + part), Add a dock, the notes.
    rowsBox_ = make(hwnd_, kRowsClass, L"", WS_CLIPCHILDREN, kSetupRows, WS_EX_CONTROLPARENT);
    if (rowsBox_) SetWindowLongPtrW(rowsBox_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    const wchar_t* heads[4] = {L"Name", L"Layout", L"Display", L"Edge"};
    for (int k = 0; k < 4; ++k) rowHeads_[k] = make(rowsBox_, WC_STATICW, heads[k], WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 100 + k);
    for (size_t i = 0; i < rows_.size(); ++i) CreateRow(i);
    addDock_ = make(hwnd_, WC_BUTTONW, L"Add a dock", WS_TABSTOP | BS_PUSHBUTTON, kSetupAddDock);
    addNote_ = make(hwnd_, WC_STATICW, L"Up to three docks here. Add more later in App settings.", SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE,
                    kSetupAddNote);
    layoutNote_ = make(hwnd_, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX, kSetupLayoutNote);
    // Step 2: same look, the "Look of" combo, a gallery, Transparency and Corners per row.
    same_ = make(hwnd_, WC_BUTTONW, L"Every dock uses the same look", WS_TABSTOP | BS_AUTOCHECKBOX, kSetupSameLook);
    sameCaption_ = make(hwnd_, WC_STATICW, L"Every dock uses the same look", SS_LEFT | SS_NOPREFIX | SS_NOTIFY | SS_CENTERIMAGE,
                        kSetupSameLookCaption);
    lookDock_ = make(hwnd_, WC_COMBOBOXW, L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, kSetupLookDock);
    for (int k = 0; k < kMaxDocks; ++k) {
        HWND g = make(hwnd_, SettingsGallery::kClassName, L"Looks", WS_TABSTOP, kSetupGallery + k);
        galleries_[k].Attach(g);
    }
    alphaLabel_ = make(hwnd_, WC_STATICW, L"Transparency", SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE, kSetupTransparencyLabel);
    for (int k = 0; k < kMaxDocks; ++k) {
        alpha_[k] = make(hwnd_, TRACKBAR_CLASSW, L"Transparency", WS_TABSTOP | TBS_HORZ | TBS_NOTICKS, kSetupTransparency + k);
        if (alpha_[k]) {
            SendMessageW(alpha_[k], TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
            SendMessageW(alpha_[k], TBM_SETPAGESIZE, 0, 10);
        }
    }
    cornersLabel_ = make(hwnd_, WC_STATICW, L"Corners", SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE, kSetupCornersLabel);
    for (int k = 0; k < kMaxDocks; ++k) {
        corners_[k] = make(hwnd_, WC_COMBOBOXW, L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, kSetupCorners + k);
        for (const wchar_t* n : kCornerNames) SendMessageW(corners_[k], CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(n));
    }
    // Step 3. The check boxes' own text is the caption beside them (a dark check box draws its text
    // black: the chooser's way).
    startBox_ = make(hwnd_, WC_BUTTONW, L"Start WinDock when Windows starts", WS_TABSTOP | BS_AUTOCHECKBOX, kSetupStartWithWindows);
    startCaption_ = make(hwnd_, WC_STATICW, L"Start WinDock when Windows starts", SS_LEFT | SS_NOPREFIX | SS_NOTIFY | SS_CENTERIMAGE,
                         kSetupStartCaption);
    taskbarBox_ = make(hwnd_, WC_BUTTONW, L"Hide the Windows taskbar", WS_TABSTOP | BS_AUTOCHECKBOX, kSetupHideTaskbar);
    taskbarCaption_ = make(hwnd_, WC_STATICW, L"Hide the Windows taskbar while WinDock runs", SS_LEFT | SS_NOPREFIX | SS_NOTIFY | SS_CENTERIMAGE,
                           kSetupTaskbarCaption);
    behaviourNote_ = make(hwnd_, WC_STATICW, L"These two take effect when you choose Finish.", SS_LEFT | SS_NOPREFIX, kSetupBehaviourNote);
    autoHideLabel_ = make(hwnd_, WC_STATICW, L"Auto-hide", SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE | SS_ENDELLIPSIS, kSetupAutoHideLabel);
    autoHide_ = make(hwnd_, WC_COMBOBOXW, L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, kSetupAutoHide);
    for (const wchar_t* n : kAutoHideNames) SendMessageW(autoHide_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(n));
    animationLabel_ = make(hwnd_, WC_STATICW, L"Animation", SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE, kSetupAnimationLabel);
    animation_ = make(hwnd_, WC_COMBOBOXW, L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, kSetupAnimation);
    for (const wchar_t* n : kAnimationNames) SendMessageW(animation_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(n));
    animationNote_ = make(hwnd_, WC_STATICW,
                          L"Full: every animation, with less motion while Windows asks for it. Reduced: less motion always. Off: no "
                          L"animation at all.",
                          SS_LEFT | SS_NOPREFIX, -1);
    // Step 4.
    doneNote_ = make(hwnd_, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX, -1);
    // The buttons (every step).
    back_ = make(hwnd_, WC_BUTTONW, L"Back", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kSetupBack);
    showAround_ = make(hwnd_, WC_BUTTONW, L"Show me around", WS_TABSTOP | BS_PUSHBUTTON, kSetupShowAround);
    next_ = make(hwnd_, WC_BUTTONW, L"Next", WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, kSetupNext);
    finish_ = make(hwnd_, WC_BUTTONW, L"Finish", WS_TABSTOP | BS_PUSHBUTTON, kSetupFinish);
    cancel_ = make(hwnd_, WC_BUTTONW, L"Cancel", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kSetupCancel);
    if (!steps_ || !rowsBox_ || !next_ || !cancel_ || !finish_) {
        DestroyWindow(hwnd_);
        return false;
    }
    SetCheck(startBox_, start_);
    SetCheck(taskbarBox_, hideTaskbar_);
    SetFonts();
    ApplyTheme();
    Layout();  // WM_SIZE came before the children
    Sync();
    GoTo(0);
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
    if (!rows_.empty() && rows_.front().layout) SetFocus(rows_.front().layout);
    return true;
}

void QuickSetup::Activate() {
    if (!hwnd_) return;
    if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
}

void QuickSetup::CreateFonts() {
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    LOGFONTW lf{};
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi_)) {
        lf = ncm.lfMessageFont;
    } else {
        lf.lfHeight = -MulDiv(9, static_cast<int>(dpi_), 72);
        lf.lfWeight = FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        wcscpy_s(lf.lfFaceName, L"Segoe UI");
    }
    for (HFONT f : {font_, fontBold_, fontHeading_}) {
        if (f) DeleteObject(f);
    }
    font_ = CreateFontIndirectW(&lf);
    LOGFONTW b = lf;
    b.lfWeight = FW_SEMIBOLD;
    fontBold_ = CreateFontIndirectW(&b);
    LOGFONTW h = b;
    h.lfHeight = lf.lfHeight * 5 / 3;
    fontHeading_ = CreateFontIndirectW(&h);
}

void QuickSetup::SetFonts() {
    auto set = [&](HWND c, HFONT f) {
        if (c) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
    };
    for (HWND c : {intro_, addDock_, addNote_, layoutNote_, same_, sameCaption_, lookDock_, alphaLabel_, cornersLabel_, startBox_,
                   startCaption_, taskbarBox_, taskbarCaption_, behaviourNote_, autoHideLabel_, autoHide_, animationLabel_, animation_,
                   animationNote_, doneNote_, back_, showAround_, next_, finish_, cancel_}) {
        set(c, font_);
    }
    set(heading_, fontHeading_);
    for (HWND c : rowHeads_) set(c, fontBold_);
    for (int k = 0; k < kMaxDocks; ++k) {
        set(alpha_[k], font_);
        set(corners_[k], font_);
    }
    for (const Row& r : rows_) {
        for (HWND c : {r.name, r.layout, r.display, r.edge, r.remove}) set(c, font_);
    }
}

void QuickSetup::ApplyTheme() {
    palette_ = DarkMode::CurrentPalette();
    const bool dark = palette_.dark;
    DarkMode::SetDarkCaption(hwnd_, dark);
    DarkMode::AllowDarkForWindow(hwnd_, dark);
    auto theme = [&](HWND c) {
        if (!c) return;
        DarkMode::AllowDarkForWindow(c, dark);
        DarkMode::ThemeControl(c, dark);
    };
    for (HWND c : {addDock_, same_, lookDock_, startBox_, taskbarBox_, autoHide_, animation_, back_, showAround_, next_, finish_, cancel_}) {
        theme(c);
    }
    for (int k = 0; k < kMaxDocks; ++k) {
        theme(corners_[k]);
        theme(alpha_[k]);
        galleries_[k].SetColors(palette_.layer, palette_.text, palette_.textSecondary, palette_.accent);
    }
    for (const Row& r : rows_) {
        for (HWND c : {r.name, r.layout, r.display, r.edge, r.remove}) theme(c);
    }
    if (layerBrush_) DeleteObject(layerBrush_);
    if (controlBrush_) DeleteObject(controlBrush_);
    layerBrush_ = CreateSolidBrush(palette_.layer);
    controlBrush_ = CreateSolidBrush(palette_.control);
    if (hwnd_) RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

// --- layout ----------------------------------------------------------------------------------------

void QuickSetup::Layout() {
    if (!hwnd_) return;
    const int W = widthDip_, H = heightDip_, cw = W - 2 * kMargin;
    auto place = [&](HWND h, int x, int y, int w, int hh) {
        if (h) SetWindowPos(h, nullptr, Px(x), Px(y), Px(std::max(0, w)), Px(std::max(0, hh)), SWP_NOZORDER | SWP_NOACTIVATE);
    };
    place(steps_, kMargin, kStepsTop, cw, kStepsH);
    place(heading_, kMargin, kHeadingTop, cw, kHeadingH);
    place(intro_, kMargin, kIntroTop, cw, kIntroH);
    const int contentBottom = H - 16 - kButtonH - 16;
    // Step 1.
    const int rowsH = kRowHeadH + 4 + kMaxDocks * kRowH;
    place(rowsBox_, kMargin, kContentTop, cw, rowsH);
    LayoutRows();
    const int addY = kContentTop + rowsH + 8;
    place(addDock_, kMargin, addY, 140, 30);
    place(addNote_, kMargin + 152, addY, cw - 152, 30);
    place(layoutNote_, kMargin, addY + 30 + 16, std::min(cw, 720), contentBottom - (addY + 46));
    // Step 2.
    int y = kContentTop;
    place(same_, kMargin, y, 15, 24);
    place(sameCaption_, kMargin + 20, y, 320, 24);
    place(lookDock_, W - kMargin - 260, y - 2, 260, kDropH);
    y += 36;
    const int sliderRowH = 36;
    const int count = std::max(1, static_cast<int>(looks_.size()));
    const int galleryH = std::max(SettingsGallery::kTileH, std::min(SettingsGallery::HeightDip(count, cw), contentBottom - sliderRowH - 12 - y));
    for (int k = 0; k < kMaxDocks; ++k) place(galleries_[k].Hwnd(), kMargin, y, cw, galleryH);
    y += galleryH + 12;
    place(alphaLabel_, kMargin, y, 150, 30);
    for (int k = 0; k < kMaxDocks; ++k) place(alpha_[k], kMargin + 150, y, 240, 30);
    const int cx = kMargin + 150 + 240 + 24;
    place(cornersLabel_, cx, y, 70, 30);
    for (int k = 0; k < kMaxDocks; ++k) place(corners_[k], cx + 70, y + 1, 150, kDropH);
    // Step 3: label 196 + control 360.
    y = kContentTop;
    place(startBox_, kMargin, y, 15, 24);
    place(startCaption_, kMargin + 20, y, kLabelW + kControlW - 20, 24);
    y += 30;
    place(taskbarBox_, kMargin, y, 15, 24);
    place(taskbarCaption_, kMargin + 20, y, kLabelW + kControlW - 20, 24);
    y += 26;
    place(behaviourNote_, kMargin + 20, y, cw - 20, 20);
    y += 36;
    place(autoHideLabel_, kMargin, y, kLabelW - 8, 28);
    place(autoHide_, kMargin + kLabelW, y, 300, kDropH);
    y += 40;
    place(animationLabel_, kMargin, y, kLabelW - 8, 28);
    place(animation_, kMargin + kLabelW, y, 300, kDropH);
    y += 36;
    place(animationNote_, kMargin + kLabelW, y, std::min(cw - kLabelW, kControlW + 160), 48);
    // Step 4.
    place(doneNote_, kMargin, kContentTop, std::min(cw, 720), contentBottom - kContentTop);
    // The buttons, right-aligned: (Show me around) Back, Next / Finish, Cancel.
    const int by = H - 16 - kButtonH;
    int x = W - kMargin - 100;
    place(cancel_, x, by, 100, kButtonH);
    x -= 8 + 110;
    place(next_, x, by, 110, kButtonH);
    place(finish_, x, by, 110, kButtonH);
    x -= 8 + 100;
    place(back_, x, by, 100, kButtonH);
    x -= 8 + 160;
    place(showAround_, x, by, 160, kButtonH);
}

void QuickSetup::LayoutRows() {
    if (!rowsBox_) return;
    const int cw = widthDip_ - 2 * kMargin;
    // name, layout, display, edge, Remove (no control wider than 360 DIPs).
    const int removeW = 80, gap = 8;
    const int avail = cw - removeW - 4 * gap;
    const int nameW = std::min(kControlW, avail * 22 / 100), layoutW = std::min(kControlW, avail * 34 / 100),
              displayW = std::min(kControlW, avail * 26 / 100), edgeW = std::min(kControlW, avail - nameW - layoutW - displayW);
    const int xs[5] = {0, nameW + gap, nameW + gap + layoutW + gap, nameW + gap + layoutW + gap + displayW + gap,
                       nameW + gap + layoutW + gap + displayW + gap + edgeW + gap};
    auto place = [&](HWND h, int x, int y, int w, int hh) {
        if (h) SetWindowPos(h, nullptr, Px(x), Px(y), Px(w), Px(hh), SWP_NOZORDER | SWP_NOACTIVATE);
    };
    for (int k = 0; k < 4; ++k) place(rowHeads_[k], xs[k], 0, k == 0 ? nameW : k == 1 ? layoutW : k == 2 ? displayW : edgeW, kRowHeadH);
    for (size_t r = 0; r < rows_.size(); ++r) {
        const int y = kRowHeadH + 4 + static_cast<int>(r) * kRowH;
        const Row& row = rows_[r];
        place(row.name, xs[0], y, nameW, kRowCtlH);
        place(row.layout, xs[1], y, layoutW, kDropH);
        place(row.display, xs[2], y, displayW, kDropH);
        place(row.edge, xs[3], y, edgeW, kDropH);
        place(row.remove, xs[4], y, removeW, kRowCtlH);
    }
}

// --- the step indicator --------------------------------------------------------------------------------

RECT QuickSetup::StepRect(int step) const {
    RECT rc{};
    if (steps_) GetClientRect(steps_, &rc);
    const LONG w = rc.right / kSteps;
    return RECT{step * w, 0, step + 1 == kSteps ? rc.right : (step + 1) * w, rc.bottom};
}

void QuickSetup::PaintSteps(HWND hwnd, HDC hdc) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    HDC dc = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom));
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    FillRect(dc, &rc, layerBrush_);
    SetBkMode(dc, TRANSPARENT);
    const COLORREF accent = palette_.highContrast ? GetSysColor(COLOR_HIGHLIGHT) : palette_.accent;
    const COLORREF accentText = palette_.highContrast ? GetSysColor(COLOR_HIGHLIGHTTEXT)
                                : (GetRValue(accent) * 299 + GetGValue(accent) * 587 + GetBValue(accent) * 114) / 1000 > 150 ? RGB(0, 0, 0)
                                                                                                                        : RGB(0xFF, 0xFF, 0xFF);
    const COLORREF line = Mix(palette_.text, palette_.layer, 22);
    const bool focused = GetFocus() == hwnd;
    for (int s = 0; s < kSteps; ++s) {
        const RECT r = StepRect(s);
        const int d = Px(24), cy = (r.top + r.bottom) / 2;
        const RECT circle{r.left, cy - d / 2, r.left + d, cy + d / 2};
        const bool current = s == step_, done = s < step_;
        if (current || done) {
            FillRound(dc, circle, current ? accent : Mix(accent, palette_.layer, 45), d);
        } else {
            FillRound(dc, circle, line, d);
            FillRound(dc, RECT{circle.left + 1, circle.top + 1, circle.right - 1, circle.bottom - 1}, palette_.layer, d - 2);
        }
        HGDIOBJ oldFont = SelectObject(dc, current ? fontBold_ : font_);
        SetTextColor(dc, current || done ? accentText : palette_.textSecondary);
        wchar_t num[4];
        swprintf_s(num, L"%d", s + 1);
        RECT n = circle;
        DrawTextW(dc, num, -1, &n, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SetTextColor(dc, current ? palette_.text : palette_.textSecondary);
        RECT t{circle.right + Px(8), r.top, r.right - Px(8), r.bottom};
        DrawTextW(dc, kStepNames[s], -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        if (current && focused && !(SendMessageW(hwnd_, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
            RECT f{r.left - Px(2), r.top + Px(2), r.right - Px(4), r.bottom - Px(2)};
            DrawFocusRect(dc, &f);
        }
        SelectObject(dc, oldFont);
    }
    BitBlt(hdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

LRESULT CALLBACK QuickSetup::StepsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<QuickSetup*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            self->PaintSteps(hwnd, dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:
            self->PaintSteps(hwnd, reinterpret_cast<HDC>(wp));
            return 0;
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            for (int s = 0; s < kSteps; ++s) {
                const RECT r = self->StepRect(s);
                if (PtInRect(&r, pt)) {
                    self->GoTo(s);
                    break;
                }
            }
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_LEFT && self->step_ > 0) self->GoTo(self->step_ - 1);
            if (wp == VK_RIGHT && self->step_ + 1 < kSteps) self->GoTo(self->step_ + 1);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void QuickSetup::GoTo(int step) {
    if (!hwnd_) return;
    step = std::clamp(step, 0, kSteps - 1);
    if (step != step_) CommitNames();  // a name typed and not yet committed
    step_ = step;
    SetWindowTextW(steps_, (L"Step " + std::to_wstring(step_ + 1) + L" of 4: " + kStepNames[step_]).c_str());
    InvalidateRect(steps_, nullptr, FALSE);
    Sync();
    ShowStep();
}

void QuickSetup::ShowStep() {
    auto show = [](HWND h, bool on) {
        if (h) ShowWindow(h, on ? SW_SHOWNA : SW_HIDE);
    };
    const bool s1 = step_ == 0, s2 = step_ == 1, s3 = step_ == 2, s4 = step_ == 3;
    // Headings: the first step welcomes (the first run) or names Quick setup (run again).
    std::wstring heading, intro;
    switch (step_) {
        case 0:
            heading = firstRun_ ? L"Welcome to WinDock" : L"Quick setup";
            intro = L"Pick your docks: a layout for each, the display and the edge it sits on. The desktop behind this window shows "
                    L"each choice at once.";
            break;
        case 1:
            heading = L"Their looks";
            intro = L"Pick a look, then set how see-through the bar is and how round its corners are.";
            break;
        case 2:
            heading = L"How it behaves";
            intro = L"When WinDock starts, whether it stands in for the Windows taskbar, and how much it moves.";
            break;
        default:
            heading = DoneHeading();
            intro = L"Right-click an empty part of a dock any time for its menu: customize it, open its settings, or run Quick setup "
                    L"again.";
            break;
    }
    SetText(heading_, heading);
    SetText(intro_, intro);
    show(rowsBox_, s1);
    show(addDock_, s1);
    show(addNote_, s1);
    show(layoutNote_, s1);
    // Step 2: the same-look row only with two or more docks; one look row shown.
    const bool several = rows_.size() > 1;
    show(same_, s2 && several);
    show(sameCaption_, s2 && several);
    show(lookDock_, s2 && several && !SameLook());
    const size_t shown = LookShown();
    for (int k = 0; k < kMaxDocks; ++k) {
        const bool on = s2 && static_cast<size_t>(k) == shown;
        show(galleries_[k].Hwnd(), on);
        show(alpha_[k], on);
        show(corners_[k], on);
    }
    show(alphaLabel_, s2);
    show(cornersLabel_, s2);
    for (HWND h : {startBox_, startCaption_, taskbarBox_, taskbarCaption_, behaviourNote_, autoHideLabel_, autoHide_, animationLabel_,
                   animation_, animationNote_}) {
        show(h, s3);
    }
    show(doneNote_, s4);
    // The buttons: Back from step 2, Next until step 3, Finish and Show me around on the last.
    EnableWindow(back_, step_ > 0);
    show(next_, !s4);
    show(finish_, s4);
    show(showAround_, s4);
    SendMessageW(next_, BM_SETSTYLE, s4 ? BS_PUSHBUTTON : BS_DEFPUSHBUTTON, TRUE);
    SendMessageW(finish_, BM_SETSTYLE, s4 ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON, TRUE);
    // The focus: never left on a control that went away (only while this window is the active one:
    // a refresh from a settings window must not pull the activation here).
    HWND f = GetFocus();
    if (GetActiveWindow() == hwnd_ && (!f || !IsWindowVisible(f) || !IsWindowEnabled(f))) {
        HWND first = s1 && !rows_.empty() ? rows_.front().layout
                     : s2                 ? galleries_[shown < static_cast<size_t>(kMaxDocks) ? shown : 0].Hwnd()
                     : s3                 ? startBox_
                                          : finish_;
        if (first && IsWindowVisible(first)) SetFocus(first);
    }
}

std::wstring QuickSetup::DoneHeading() const {
    int n = 0;
    if (host_.config) {
        for (const DockConfig& d : host_.config->docks) n += d.IsDock() && d.enabled ? 1 : 0;
    }
    return n <= 1 ? std::wstring(L"Your dock is ready.") : L"Your " + std::to_wstring(n) + L" docks are ready.";
}

// --- the rows (step 1) ---------------------------------------------------------------------------------

void QuickSetup::CreateRow(size_t r) {
    if (r >= rows_.size() || !rowsBox_) return;
    Row& row = rows_[r];
    const int base = 10 * static_cast<int>(r);
    auto make = [&](const wchar_t* cls, DWORD s, int part, DWORD ex = 0) {
        return CreateWindowExW(ex, cls, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | s, 0, 0, 0, 0, rowsBox_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(base + part)), instance_, nullptr);
    };
    row.name = make(WC_EDITW, ES_AUTOHSCROLL, kRowName, WS_EX_CLIENTEDGE);
    if (row.name) SendMessageW(row.name, EM_LIMITTEXT, 64, 0);
    row.layout = make(WC_COMBOBOXW, WS_VSCROLL | CBS_DROPDOWNLIST, kRowLayout);
    row.display = make(WC_COMBOBOXW, WS_VSCROLL | CBS_DROPDOWNLIST, kRowDisplay);
    row.edge = make(WC_COMBOBOXW, WS_VSCROLL | CBS_DROPDOWNLIST, kRowEdge);
    // The first dock stays (Quick setup sets up at least one); the others can go again.
    row.remove = r > 0 ? make(WC_BUTTONW, BS_PUSHBUTTON, kRowRemove) : nullptr;
    if (row.remove) SetWindowTextW(row.remove, L"Remove");
    FillRowCombos(r);
    for (HWND c : {row.name, row.layout, row.display, row.edge, row.remove}) {
        if (!c) continue;
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
        DarkMode::AllowDarkForWindow(c, palette_.dark);
        DarkMode::ThemeControl(c, palette_.dark);
    }
    SyncRow(r);
}

void QuickSetup::FillRowCombos(size_t r) {
    Row& row = rows_[r];
    SendMessageW(row.layout, CB_RESETCONTENT, 0, 0);
    if (row.keepEntry) SendMessageW(row.layout, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"As it is now"));
    for (const Presets::Preset& p : Presets::All()) SendMessageW(row.layout, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(p.name.c_str()));
    SendMessageW(row.layout, CB_SETCURSEL, static_cast<WPARAM>(row.layoutSel), 0);
    SendMessageW(row.display, CB_RESETCONTENT, 0, 0);
    const int n = MonitorCount();
    for (int m = 0; m < n; ++m) {
        const std::wstring text = m == 0 ? std::wstring(L"Display 1 (main)") : L"Display " + std::to_wstring(m + 1);
        SendMessageW(row.display, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
    SendMessageW(row.display, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Every display"));
    SendMessageW(row.edge, CB_RESETCONTENT, 0, 0);
    for (const wchar_t* e : kEdgeNames) SendMessageW(row.edge, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(e));
}

void QuickSetup::SyncRow(size_t r) {
    if (r >= rows_.size()) return;
    Row& row = rows_[r];
    const DockConfig* d = DockOf(row.id);
    if (!d) return;
    if (row.name && GetFocus() != row.name) SetText(row.name, d->DisplayName());
    SetComboSel(row.display, DisplaySel(d->monitor));
    SetComboSel(row.edge, static_cast<int>(d->edge));
    SetComboSel(row.layout, row.layoutSel);
}

void QuickSetup::DestroyRows() {
    for (Row& row : rows_) {
        for (HWND c : {row.name, row.layout, row.display, row.edge, row.remove}) {
            if (c) DestroyWindow(c);
        }
        row.name = row.layout = row.display = row.edge = row.remove = nullptr;
    }
}

void QuickSetup::RebuildRows() {
    // The ids say the row (10 * row + part): after a row went, every row is made again.
    const HWND focus = GetFocus();
    const bool hadFocus = focus && rowsBox_ && IsChild(rowsBox_, focus);
    DestroyRows();
    for (size_t r = 0; r < rows_.size(); ++r) CreateRow(r);
    LayoutRows();
    if (hadFocus && !rows_.empty() && rows_.back().layout) SetFocus(rows_.back().layout);
}

void QuickSetup::CommitNames() {
    for (size_t r = 0; r < rows_.size(); ++r) CommitName(r);
}

void QuickSetup::CommitName(size_t r) {
    if (r >= rows_.size() || !rows_[r].name) return;
    const DockConfig* d = DockOf(rows_[r].id);
    if (!d) return;
    wchar_t buf[80]{};
    GetWindowTextW(rows_[r].name, buf, static_cast<int>(std::size(buf)));
    if (d->DisplayName() == buf) return;
    QuickSetupChange c;
    c.kind = QuickSetupChange::Kind::Rename;
    c.dock = rows_[r].id;
    c.text = buf;
    Apply(c);
}

void QuickSetup::OnRowCommand(int id, int code, HWND control) {
    const size_t r = static_cast<size_t>(id / 10);
    const int part = id % 10;
    if (r >= rows_.size() || syncing_) return;
    Row& row = rows_[r];
    QuickSetupChange c;
    c.dock = row.id;
    switch (part) {
        case kRowName:
            if (code == EN_KILLFOCUS) CommitName(r);
            return;
        case kRowLayout: {
            if (code != CBN_SELCHANGE) return;
            const int sel = ComboSel(control);
            if (sel < 0) return;
            const int preset = row.keepEntry ? sel - 1 : sel;
            c.kind = QuickSetupChange::Kind::Layout;
            c.value = preset;
            if (!Apply(c)) {
                SetComboSel(control, row.layoutSel);
                return;
            }
            row.layoutSel = sel;
            // What the layout is, under the rows (its description).
            const auto& all = Presets::All();
            const std::wstring note = preset >= 0 && static_cast<size_t>(preset) < all.size()
                                          ? all[static_cast<size_t>(preset)].name + L": " + all[static_cast<size_t>(preset)].description
                                          : NameOf(row.id) + L": as it was when Quick setup opened.";
            SetText(layoutNote_, note);
            return;
        }
        case kRowDisplay: {
            if (code != CBN_SELCHANGE) return;
            const int sel = ComboSel(control), n = MonitorCount();
            const DockConfig* d = DockOf(row.id);
            if (sel < 0 || !d || sel == DisplaySel(d->monitor)) return;
            c.kind = QuickSetupChange::Kind::Display;
            c.value = sel >= n ? DockConfig::kMonitorEach : sel;
            Apply(c);
            return;
        }
        case kRowEdge: {
            if (code != CBN_SELCHANGE) return;
            const int sel = ComboSel(control);
            if (sel < 0 || sel > 3) return;
            c.kind = QuickSetupChange::Kind::Edge;
            c.value = sel;
            Apply(c);
            return;
        }
        case kRowRemove:
            // Posted: the button must not go away inside its own click.
            if (code == BN_CLICKED) PostMessageW(hwnd_, kMsgRemoveRow, r, 0);
            return;
        default:
            return;
    }
}

void QuickSetup::RemoveRow(size_t r) {
    if (r == 0 || r >= rows_.size()) return;
    QuickSetupChange c;
    c.kind = QuickSetupChange::Kind::RemoveDock;
    c.dock = rows_[r].id;
    if (!Apply(c)) return;
    // Apply's Refresh drops the row (its dock is gone); now, so the rows are right at once.
    Sync();
}

void QuickSetup::AddDock() {
    if (rows_.size() >= static_cast<size_t>(kMaxDocks)) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    QuickSetupChange c;
    c.kind = QuickSetupChange::Kind::AddDock;
    if (!Apply(c) || c.text.empty()) return;
    Row row;
    row.id = c.text;
    const size_t basic = Presets::IndexOf(L"basic");
    row.layoutSel = basic == SIZE_MAX ? 0 : static_cast<int>(basic);
    rows_.push_back(std::move(row));
    CreateRow(rows_.size() - 1);
    LayoutRows();
    Sync();
    if (rows_.back().name) SetFocus(rows_.back().name);
}

// --- the looks (step 2) --------------------------------------------------------------------------------

bool QuickSetup::SameLook() const {
    // Derived from the config: every row's dock uses the app look.
    for (const Row& r : rows_) {
        if (const DockConfig* d = DockOf(r.id); d && d->OwnLook()) return false;
    }
    return true;
}

std::wstring QuickSetup::LookTarget(size_t r) const {
    if (rows_.empty()) return {};
    if (rows_.size() == 1) {
        // One dock: its own look when it has one (a layout like Modern gives it one), else the app's.
        const DockConfig* d = DockOf(rows_.front().id);
        return d && d->OwnLook() ? d->id : std::wstring();
    }
    if (SameLook()) return {};
    return r < rows_.size() ? rows_[r].id : std::wstring();
}

size_t QuickSetup::LookShown() const {
    if (rows_.size() <= 1 || SameLook()) return 0;
    return std::min(lookShown_, rows_.size() - 1);
}

void QuickSetup::SyncLooks() {
    if (!host_.config) return;
    const Config& cfg = *host_.config;
    const bool same = SameLook();
    SetCheck(same_, same);
    // "Look of <name>" per row.
    SendMessageW(lookDock_, CB_RESETCONTENT, 0, 0);
    for (const Row& r : rows_) {
        const std::wstring text = L"Look of " + NameOf(r.id);
        SendMessageW(lookDock_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
    SetComboSel(lookDock_, static_cast<int>(LookShown()));
    for (size_t k = 0; k < static_cast<size_t>(kMaxDocks); ++k) {
        const std::wstring target = LookTarget(k);
        const DockConfig* d = DockOf(target);
        const bool own = d && d->OwnLook();
        const ThemeConfig& look = own ? d->theme : cfg.theme;
        const std::wstring& preset = own ? d->themePreset : cfg.themePreset;
        const uint64_t mask = own ? d->themeOverrides : cfg.themeOverrides;
        int changed = 0;
        for (uint64_t m = mask; m; m &= m - 1) ++changed;
        std::vector<SettingsGallery::Tile> tiles;
        int selected = 0;
        for (size_t i = 0; i < looks_.size(); ++i) {
            SettingsGallery::Tile t;
            t.name = looks_[i].name;
            t.theme = looks_[i].values;
            t.current = _wcsicmp(looks_[i].id.c_str(), preset.c_str()) == 0;
            if (t.current) {
                t.detail = changed ? L"Current \x00B7 " + std::to_wstring(changed) + L" changed" : std::wstring(L"Current");
                selected = static_cast<int>(i);
            }
            tiles.push_back(std::move(t));
        }
        galleries_[k].SetTiles(std::move(tiles), selected);
        const int alpha = std::clamp(Theme::Derived(Theme::Quick::Transparency).get(look), 0, 100);
        // Not while the thumb is dragged (the value read back may differ by one).
        if (alpha_[k] && GetCapture() != alpha_[k]) SendMessageW(alpha_[k], TBM_SETPOS, TRUE, alpha);
        SetComboSel(corners_[k], Theme::Derived(Theme::Quick::Corners).get(look));  // -1 (Mixed): no selection
        if (k == LookShown()) SetText(alphaLabel_, L"Transparency: " + std::to_wstring(alpha) + L" %");
    }
}

void QuickSetup::OnLookCommand(int id, int code) {
    if (syncing_) return;
    if (id == kSetupSameLook || id == kSetupSameLookCaption) {
        if (id == kSetupSameLookCaption && code != STN_CLICKED) return;
        if (id == kSetupSameLook && code != BN_CLICKED) return;
        const bool want = !SameLook();
        QuickSetupChange c;
        c.kind = QuickSetupChange::Kind::SameLook;
        c.value = want ? 1 : 0;
        for (const Row& r : rows_) c.docks.push_back(r.id);
        Apply(c);
        Sync();
        ShowStep();
        return;
    }
    if (id == kSetupLookDock) {
        if (code != CBN_SELCHANGE) return;
        const int sel = ComboSel(lookDock_);
        if (sel >= 0) lookShown_ = static_cast<size_t>(sel);
        Sync();
        ShowStep();
        return;
    }
    if (id >= kSetupGallery && id < kSetupGallery + kMaxDocks) {
        if (code != SettingsGallery::kChosen) return;
        const size_t k = static_cast<size_t>(id - kSetupGallery);
        const int sel = galleries_[k].Selected();
        if (sel < 0 || static_cast<size_t>(sel) >= looks_.size()) return;
        QuickSetupChange c;
        c.kind = QuickSetupChange::Kind::Look;
        c.dock = LookTarget(k);
        c.value = sel;
        Apply(c);
        return;
    }
    if (id >= kSetupCorners && id < kSetupCorners + kMaxDocks) {
        if (code != CBN_SELCHANGE) return;
        const size_t k = static_cast<size_t>(id - kSetupCorners);
        const int sel = ComboSel(corners_[k]);
        if (sel < 0) return;
        QuickSetupChange c;
        c.kind = QuickSetupChange::Kind::Quick;
        c.dock = LookTarget(k);
        c.quick = static_cast<int>(Theme::Quick::Corners);
        c.value = sel;
        Apply(c);
    }
}

void QuickSetup::OnAlpha(size_t k) {
    if (k >= static_cast<size_t>(kMaxDocks) || !alpha_[k] || syncing_ || !host_.config) return;
    const int pos = static_cast<int>(SendMessageW(alpha_[k], TBM_GETPOS, 0, 0));
    SetText(alphaLabel_, L"Transparency: " + std::to_wstring(pos) + L" %");
    const std::wstring target = LookTarget(k);
    const DockConfig* d = DockOf(target);
    const ThemeConfig& look = d && d->OwnLook() ? d->theme : host_.config->theme;
    if (Theme::Derived(Theme::Quick::Transparency).get(look) == pos) return;  // nothing to write
    QuickSetupChange c;
    c.kind = QuickSetupChange::Kind::Quick;
    c.dock = target;
    c.quick = static_cast<int>(Theme::Quick::Transparency);
    c.value = pos;
    Apply(c);
}

// --- behaviour (step 3) --------------------------------------------------------------------------------

void QuickSetup::SyncBehaviour() {
    if (!host_.config) return;
    SetCheck(startBox_, start_);
    SetCheck(taskbarBox_, hideTaskbar_);
    const DockConfig* first = rows_.empty() ? nullptr : DockOf(rows_.front().id);
    if (first) {
        SetComboSel(autoHide_, static_cast<int>(first->autoHide));
        SetText(autoHideLabel_, L"Auto-hide of " + first->DisplayName());
        // A floating bar never hides (its layer decides): the reason beside the control.
        EnableWindow(autoHide_, !first->Floating());
    }
    const AnimationConfig& a = host_.config->animation;
    SetComboSel(animation_, a.speed == 0.0 ? 2 : a.reduceMotion == ReduceMotion::On ? 1 : 0);
}

// --- the whole window ----------------------------------------------------------------------------------

void QuickSetup::Refresh() {
    if (!hwnd_ || refreshPosted_) return;
    refreshPosted_ = PostMessageW(hwnd_, kMsgRefresh, 0, 0) != FALSE;
}

void QuickSetup::Sync() {
    if (!hwnd_ || !host_.config || syncing_) return;
    syncing_ = true;  // setting the controls sends no edits back
    // Rows whose dock went away (Undo, a settings window): dropped; the first row follows the first dock.
    bool rebuild = false;
    for (size_t r = rows_.size(); r-- > 0;) {
        if (DockOf(rows_[r].id)) continue;
        if (r == 0) {
            const auto it = std::find_if(host_.config->docks.begin(), host_.config->docks.end(), [](const DockConfig& d) { return d.IsDock(); });
            if (it != host_.config->docks.end() &&
                std::none_of(rows_.begin() + 1, rows_.end(), [&](const Row& o) { return o.id == it->id; })) {
                rows_[0].id = it->id;
                rows_[0].keepEntry = true;
                rows_[0].layoutSel = 0;
                rebuild = true;
                continue;
            }
        }
        if (r > 0 || rows_.size() > 1) {
            rows_.erase(rows_.begin() + static_cast<ptrdiff_t>(r));
            rebuild = true;
        }
    }
    if (rebuild) RebuildRows();
    for (size_t r = 0; r < rows_.size(); ++r) SyncRow(r);
    EnableWindow(addDock_, rows_.size() < static_cast<size_t>(kMaxDocks));
    SyncLooks();
    SyncBehaviour();
    // Step 4's words.
    std::wstring note = L"Finish keeps everything you chose as one change: Undo in the dock menu brings back what was there before. "
                        L"Show me around also shows three short tips on the dock.\n\n";
    note += start_ ? L"WinDock starts when Windows starts." : L"WinDock does not start by itself; start it from its folder.";
    note += hideTaskbar_ ? L" The Windows taskbar stays hidden while WinDock runs." : L" The Windows taskbar stays beside the dock.";
    SetText(doneNote_, note);
    if (step_ == 3) SetText(heading_, DoneHeading());
    syncing_ = false;
}

bool QuickSetup::Apply(QuickSetupChange& c) {
    // Finished or cancelled: a name edit's focus loss while the window goes must not edit again.
    if (done_ || closing_) return false;
    if (!host_.change || !host_.change(c)) {
        MessageBeep(MB_ICONWARNING);
        Refresh();  // the control goes back to the config's value
        return false;
    }
    return true;
}

bool QuickSetup::Next() {
    if (!hwnd_ || step_ + 1 >= kSteps) return false;
    GoTo(step_ + 1);
    return true;
}

bool QuickSetup::Finish(bool tips) {
    if (!hwnd_ || closing_) return false;
    CommitNames();
    done_ = true;  // from here no control edits anything (the window's focus goes as it closes)
    if (!host_.finish || !host_.finish(start_, hideTaskbar_, tips)) {
        done_ = false;
        MessageBeep(MB_ICONWARNING);
        return false;
    }
    Close();
    return true;
}

bool QuickSetup::Cancel() {
    if (!hwnd_ || closing_) return false;
    done_ = true;
    if (host_.cancel && !host_.cancel()) {
        done_ = false;
        MessageBeep(MB_ICONWARNING);  // a dock menu is open: the preview stays, and so does the window
        return false;
    }
    Close();
    return true;
}

void QuickSetup::Close() {
    if (closing_ || !hwnd_) return;
    closing_ = true;
    DestroyWindow(hwnd_);
}

bool QuickSetup::PreTranslateMessage(MSG& msg) {
    if (!hwnd_ || !msg.hwnd || (msg.hwnd != hwnd_ && !IsChild(hwnd_, msg.hwnd))) return false;
    return IsDialogMessageW(hwnd_, &msg) != FALSE;
}

LRESULT CALLBACK QuickSetup::RowsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<QuickSetup*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
        case WM_ERASEBKGND: {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wp), &rc, self->layerBrush_);
            return 1;
        }
        // The rows' notifications and colours are the window's (it tells a row's control by its parent).
        case WM_COMMAND:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORBTN:
        case WM_NOTIFY:
            return self->hwnd_ ? SendMessageW(self->hwnd_, msg, wp, lp) : 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK QuickSetup::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    QuickSetup* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<QuickSetup*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<QuickSetup*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    const LRESULT r = self->WndProc(msg, wp, lp);
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        if (self->steps_) SetWindowLongPtrW(self->steps_, GWLP_USERDATA, 0);
        self->hwnd_ = nullptr;
        self->steps_ = self->heading_ = self->intro_ = self->back_ = self->next_ = self->finish_ = self->cancel_ = self->showAround_ = nullptr;
        self->rowsBox_ = self->addDock_ = self->addNote_ = self->layoutNote_ = nullptr;
        for (HWND& h : self->rowHeads_) h = nullptr;
        for (Row& row : self->rows_) row.name = row.layout = row.display = row.edge = row.remove = nullptr;
        self->same_ = self->sameCaption_ = self->lookDock_ = self->alphaLabel_ = self->cornersLabel_ = nullptr;
        for (int k = 0; k < kMaxDocks; ++k) self->alpha_[k] = self->corners_[k] = nullptr;
        self->startBox_ = self->startCaption_ = self->taskbarBox_ = self->taskbarCaption_ = self->behaviourNote_ = nullptr;
        self->autoHideLabel_ = self->autoHide_ = self->animationLabel_ = self->animation_ = self->animationNote_ = self->doneNote_ = nullptr;
        if (self->host_.destroyed) self->host_.destroyed();  // the app frees the object later (posted)
    }
    return r;
}

LRESULT QuickSetup::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case DM_GETDEFID:
            return MAKELRESULT(step_ + 1 >= kSteps ? kSetupFinish : kSetupNext, DC_HASDEFID);
        case WM_NEXTDLGCTL:  // as a dialog does (automation: WM_NEXTDLGCTL(control, TRUE) focuses it)
            if (LOWORD(lp) && wp && IsChild(hwnd_, reinterpret_cast<HWND>(wp))) {
                SetFocus(reinterpret_cast<HWND>(wp));
            } else if (!LOWORD(lp)) {
                if (HWND next = GetNextDlgTabItem(hwnd_, GetFocus(), wp != 0)) SetFocus(next);
            }
            return 0;
        case kMsgRefresh:
            refreshPosted_ = false;
            Sync();
            ShowStep();
            return 0;
        case kMsgRemoveRow:
            RemoveRow(static_cast<size_t>(wp));
            ShowStep();
            return 0;
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp);
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            CreateFonts();
            SetFonts();
            Layout();
            InvalidateRect(hwnd_, nullptr, TRUE);
            return 0;
        }
        case WM_SIZE:
            Layout();
            return 0;
        case WM_SETTINGCHANGE:
            if (wp == SPI_SETHIGHCONTRAST ||
                (lp && CompareStringOrdinal(reinterpret_cast<const wchar_t*>(lp), -1, L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL)) {
                ApplyTheme();
            }
            return 0;
        case WM_THEMECHANGED:
        case WM_SYSCOLORCHANGE:
            ApplyTheme();
            return 0;
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, palette_.text);
            SetBkColor(dc, palette_.control);
            return reinterpret_cast<LRESULT>(controlBrush_);
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wp);
            const HWND c = reinterpret_cast<HWND>(lp);
            const bool muted = c == intro_ || c == addNote_ || c == layoutNote_ || c == behaviourNote_ || c == animationNote_ ||
                               std::find(std::begin(rowHeads_), std::end(rowHeads_), c) != std::end(rowHeads_);
            SetTextColor(dc, muted ? palette_.textSecondary : IsWindowEnabled(c) ? palette_.text : palette_.textDisabled);
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
        case WM_HSCROLL:
            for (size_t k = 0; k < static_cast<size_t>(kMaxDocks); ++k) {
                if (reinterpret_cast<HWND>(lp) == alpha_[k]) OnAlpha(k);
            }
            return 0;
        case WM_COMMAND: {
            const int id = LOWORD(wp), code = HIWORD(wp);
            const HWND control = reinterpret_cast<HWND>(lp);
            // A row's control (a child of the rows container): its id is 10 * row + part.
            if (control && rowsBox_ && GetParent(control) == rowsBox_) {
                OnRowCommand(id, code, control);
                return 0;
            }
            const bool click = code == BN_CLICKED || lp == 0;
            switch (id) {
                case IDOK:  // Enter: the default button
                    if (step_ + 1 >= kSteps) {
                        Finish(false);
                    } else {
                        Next();
                    }
                    return 0;
                case IDCANCEL:
                    Cancel();
                    return 0;
                case kSetupNext:
                    if (click) Next();
                    return 0;
                case kSetupBack:
                    if (click && step_ > 0) GoTo(step_ - 1);
                    return 0;
                case kSetupFinish:
                    if (click) Finish(false);
                    return 0;
                case kSetupShowAround:
                    if (click) Finish(true);
                    return 0;
                case kSetupCancel:
                    if (click) Cancel();
                    return 0;
                case kSetupAddDock:
                    if (click) AddDock();
                    ShowStep();
                    return 0;
                case kSetupStartWithWindows:
                case kSetupStartCaption:
                case kSetupHideTaskbar:
                case kSetupTaskbarCaption: {
                    // Held until Finish. A caption click or WM_COMMAND(id, 0) toggles the box first.
                    const bool start = id == kSetupStartWithWindows || id == kSetupStartCaption;
                    HWND box = start ? startBox_ : taskbarBox_;
                    const bool caption = id == kSetupStartCaption || id == kSetupTaskbarCaption;
                    if (caption ? code != STN_CLICKED && lp != 0 : !click) return 0;
                    if (caption || lp == 0) SetCheck(box, !Checked(box));
                    (start ? start_ : hideTaskbar_) = Checked(box);
                    if (caption) SetFocus(box);
                    Sync();
                    return 0;
                }
                case kSetupAutoHide:
                case kSetupAnimation: {
                    if (code != CBN_SELCHANGE || syncing_) return 0;
                    const int sel = ComboSel(id == kSetupAutoHide ? autoHide_ : animation_);
                    if (sel < 0 || sel > 2) return 0;
                    QuickSetupChange c;
                    c.kind = id == kSetupAutoHide ? QuickSetupChange::Kind::AutoHide : QuickSetupChange::Kind::Animation;
                    c.value = sel;
                    Apply(c);
                    return 0;
                }
                default:
                    if (id == kSetupSameLookCaption && lp != 0 && code == STN_CLICKED) {
                        SetFocus(same_);
                    }
                    if ((id >= kSetupGallery && id < kSetupGallery + kMaxDocks) || (id >= kSetupCorners && id < kSetupCorners + kMaxDocks) ||
                        id == kSetupSameLook || id == kSetupSameLookCaption || id == kSetupLookDock) {
                        OnLookCommand(id, lp == 0 && (id == kSetupSameLook || id == kSetupSameLookCaption)
                                              ? (id == kSetupSameLook ? BN_CLICKED : STN_CLICKED)
                                              : code);
                    }
                    return 0;
            }
        }
        case WM_CLOSE:
            Cancel();
            return 0;
        case WM_DESTROY:
            // Destroyed without Finish or Cancel (App's shutdown): nothing is asked of App here: the
            // preview is never saved then (App::Shutdown).
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
