// Round 12 phase 12G: Quick setup (see QuickSetup.h).

#include "QuickSetup.h"

#include <shellscalingapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <utility>

#include "SettingsIds.h"
#include "Theme.h"
#include "resource.h"

namespace {

using namespace SettingsIds;
using QsUi::Glyph;

// Client sizes, DIPs (the plan: 960 x 640 on displays wider than 1400 DIPs, else 760 x 540).
constexpr int kBigW = 960, kBigH = 640, kSmallW = 760, kSmallH = 540, kBigFrom = 1400;
// Round 13 phase 13H: the prototype's column (heading 24 DIP at 20, its sentence under it, the
// body 18 below; the footer's buttons 18 from the bottom with a hairline 14 above them).
constexpr int kHeadingTop = 20, kHeadingH = 32, kIntroTop = 58;
constexpr int kButtonH = 34;
constexpr int kRailStepsTop = 68, kRailStepH = 52, kRailStepGap = 2;
// Step 1: a card per dock: two rows of (small label, field), the monitor picture on the left.
constexpr int kRowCardH = 126, kCardGap = 10, kAddH = 48;
constexpr int kFieldH = 28, kSegH = 30;
constexpr int kDropH = 300;  // a drop-down combo's height with its list
constexpr int kCardRadius = 10;

const wchar_t* const kStepNames[QuickSetup::kSteps] = {L"Docks", L"Look", L"Behaviour", L"Review"};
const wchar_t* const kEdgeNames[] = {L"Bottom", L"Top", L"Left", L"Right"};  // DockEdge order
// The edge choice's segments: Top, Bottom, Left, Right (the prototype's order) as DockEdge values.
constexpr int kSegEdge[4] = {1, 0, 2, 3};
const wchar_t* const kCornerNames[] = {L"Square", L"Soft", L"Round"};
const wchar_t* const kAutoHideNames[] = {L"Never", L"Always", L"When a window overlaps"};  // AutoHideMode order
const wchar_t* const kAutoHideDetails[] = {L"The dock stays on screen.", L"Slides away, back when the pointer touches the edge.",
                                           L"Hides only while a window covers its place."};
const wchar_t* const kAnimationNames[] = {L"Full", L"Reduced", L"Off"};
const wchar_t* const kAnimationNotes[] = {L"Every animation, with less motion while Windows asks for it.", L"Less motion, always.",
                                          L"No animation at all."};
const wchar_t* const kHeadings[QuickSetup::kSteps] = {L"Where should your docks live?", L"Pick a look", L"How should it behave?",
                                                     L"Everything is in place"};
const wchar_t* const kIntros[QuickSetup::kSteps] = {
    L"Start with one. Add up to three, each on its own edge or screen.",
    L"Colour, glass and corners. Fine-tune below, or change it any time from the dock menu.",
    L"Sensible defaults are already on. Everything here lives in Settings too.",
    L"Finish keeps all of this as one change. Undo in the dock menu brings back what was there before."};
constexpr wchar_t kInfo[] = L"Your desktop follows along as you choose. Cancel puts everything back.";
const wchar_t* const kStartHints[2] = {L"Start it yourself from the Start menu or its folder.", L"WinDock opens when you sign in."};
const wchar_t* const kTaskbarHints[2] = {L"Both stay on screen. The dock sits above the taskbar.",
                                         L"The dock stands in for it. It comes back as soon as WinDock quits."};

// The gallery's second line: what each built-in look is, in a few words (the prototype's). A theme
// file has no words of its own.
std::wstring ShortTagline(const Presets::Look& look) {
    static const struct {
        const wchar_t* id;
        const wchar_t* words;
    } kWords[] = {{L"dark", L"Soft, translucent charcoal"}, {L"light", L"Frosted white"},
                  {L"glass", L"See-through, blurred"},      {L"solid", L"Opaque, no transparency"},
                  {L"shelf", L"A thin lit ledge"},          {L"leopard", L"Tilted, with reflections"},
                  {L"contrast", L"Black, white outline"},   {L"aero", L"The Windows 7 blue"}};
    if (!look.user) {
        for (const auto& w : kWords) {
            if (_wcsicmp(w.id, look.id.c_str()) == 0) return w.words;
        }
    }
    return look.user ? L"Your theme file" : L"";
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

std::wstring TextOf(HWND h) {
    wchar_t buf[256]{};
    if (h) GetWindowTextW(h, buf, static_cast<int>(std::size(buf)));
    return buf;
}

bool Checked(HWND box) { return box && SendMessageW(box, BM_GETCHECK, 0, 0) == BST_CHECKED; }
void SetCheck(HWND box, bool on) {
    if (box && Checked(box) != on) SendMessageW(box, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
}

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

std::wstring Upper(std::wstring s) {
    if (!s.empty()) CharUpperBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
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

int SegOfEdge(int edge) {
    for (int i = 0; i < 4; ++i) {
        if (kSegEdge[i] == edge) return i;
    }
    return -1;
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
    for (HFONT f : {font_, fontBold_, fontHeading_, fontSmall_, fontLabel_}) {
        if (f) DeleteObject(f);
    }
    for (HBRUSH b : {baseBrush_, controlBrush_, cardBrush_}) {
        if (b) DeleteObject(b);
    }
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
        !SettingsGallery::Register(instance) || !QsUi::Choice::Register(instance)) {
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
    palette_ = DarkMode::CurrentPalette();
    colors_ = QsUi::ColorsFor(palette_);
    auto make = [&](HWND parent, const wchar_t* cls, const wchar_t* text, DWORD s, int id, DWORD ex = 0) {
        return CreateWindowExW(ex, cls, text, WS_CHILD | s, 0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                               instance, nullptr);
    };
    auto choice = [&](int id, QsUi::Choice::Style s, const wchar_t* name) { return QsUi::Choice::Create(hwnd_, id, s, name, instance); };
    // The tab order is the creation order: the steps, the step's controls, then the buttons.
    steps_ = make(hwnd_, kStepsClass, L"", WS_VISIBLE | WS_TABSTOP, kSetupSteps);
    if (steps_) SetWindowLongPtrW(steps_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    heading_ = make(hwnd_, WC_STATICW, L"", WS_VISIBLE | SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, kSetupHeading);
    intro_ = make(hwnd_, WC_STATICW, L"", WS_VISIBLE | SS_LEFT | SS_NOPREFIX, kSetupIntro);
    // Step 1: the rows (a container: its children's ids are 10 * row + part), Add a dock, the notes.
    rowsBox_ = make(hwnd_, kRowsClass, L"", WS_CLIPCHILDREN, kSetupRows, WS_EX_CONTROLPARENT);
    if (rowsBox_) SetWindowLongPtrW(rowsBox_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    for (size_t i = 0; i < rows_.size(); ++i) CreateRow(i);
    addDock_ = make(hwnd_, WC_BUTTONW, L"Add a dock", WS_TABSTOP | BS_PUSHBUTTON, kSetupAddDock);
    // 13H: the note's words are the button's own text at three docks; the static stays for its id.
    addNote_ = make(hwnd_, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE, kSetupAddNote);
    layoutNote_ = make(hwnd_, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX, kSetupLayoutNote);
    // Step 2: a gallery per row, then the card: same look, Transparency and Corners per row.
    lookDock_ = make(hwnd_, WC_COMBOBOXW, L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, kSetupLookDock);
    for (int k = 0; k < kMaxDocks; ++k) {
        HWND g = make(hwnd_, SettingsGallery::kClassName, L"Looks", WS_TABSTOP, kSetupGallery + k);
        galleries_[k].Attach(g);
    }
    same_ = make(hwnd_, WC_BUTTONW, L"Same look for every dock", WS_TABSTOP | BS_AUTOCHECKBOX, kSetupSameLook);
    sameCaption_ = make(hwnd_, WC_STATICW, L"Same look for every dock", SS_LEFT | SS_NOPREFIX | SS_NOTIFY | SS_CENTERIMAGE | SS_ENDELLIPSIS,
                        kSetupSameLookCaption);
    alphaLabel_ = make(hwnd_, WC_STATICW, L"Transparency", SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE, kSetupTransparencyLabel);
    alphaValue_ = make(hwnd_, WC_STATICW, L"", SS_RIGHT | SS_NOPREFIX | SS_CENTERIMAGE, kSetupTransparencyValue);
    for (int k = 0; k < kMaxDocks; ++k) {
        alpha_[k] = make(hwnd_, TRACKBAR_CLASSW, L"Transparency", WS_TABSTOP | TBS_HORZ | TBS_NOTICKS, kSetupTransparency + k);
        if (alpha_[k]) {
            SendMessageW(alpha_[k], TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
            SendMessageW(alpha_[k], TBM_SETPAGESIZE, 0, 10);
        }
    }
    cornersLabel_ = make(hwnd_, WC_STATICW, L"Corners", SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE, kSetupCornersLabel);
    for (int k = 0; k < kMaxDocks; ++k) {
        // The combo: the automation's state holder, never shown (13H); the choice is what is seen.
        corners_[k] = make(hwnd_, WC_COMBOBOXW, L"", WS_VSCROLL | CBS_DROPDOWNLIST, kSetupCorners + k);
        for (const wchar_t* n : kCornerNames) SendMessageW(corners_[k], CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(n));
        cornersChoice_[k] = choice(kSetupCornersChoice + k, QsUi::Choice::Style::Segments, L"Corners");
        std::vector<QsUi::Choice::Option> opts;
        for (const wchar_t* n : kCornerNames) opts.push_back(QsUi::Choice::Option{n, L"", Glyph::None, n});
        QsUi::Choice::SetOptions(cornersChoice_[k], std::move(opts));
    }
    // Step 3: the two switch cards (the check boxes painted as switches; their captions the cards'
    // titles), the auto-hide tiles, the Animation card.
    startCaption_ = make(hwnd_, WC_STATICW, L"Start with Windows", SS_LEFT | SS_NOPREFIX | SS_NOTIFY | SS_CENTERIMAGE | SS_ENDELLIPSIS,
                         kSetupStartCaption);
    startHint_ = make(hwnd_, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX, -1);
    startBox_ = make(hwnd_, WC_BUTTONW, L"Start with Windows", WS_TABSTOP | BS_AUTOCHECKBOX, kSetupStartWithWindows);
    taskbarCaption_ = make(hwnd_, WC_STATICW, L"Hide the Windows taskbar", SS_LEFT | SS_NOPREFIX | SS_NOTIFY | SS_CENTERIMAGE | SS_ENDELLIPSIS,
                           kSetupTaskbarCaption);
    taskbarHint_ = make(hwnd_, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX, -1);
    taskbarBox_ = make(hwnd_, WC_BUTTONW, L"Hide the Windows taskbar", WS_TABSTOP | BS_AUTOCHECKBOX, kSetupHideTaskbar);
    behaviourNote_ = make(hwnd_, WC_STATICW, L"These two take effect when you choose Finish.", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS,
                          kSetupBehaviourNote);
    autoHideLabel_ = make(hwnd_, WC_STATICW, L"AUTO-HIDE", SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE | SS_ENDELLIPSIS, kSetupAutoHideLabel);
    autoHideHint_ = make(hwnd_, WC_STATICW, L"A floating dock never hides.", SS_RIGHT | SS_NOPREFIX | SS_CENTERIMAGE | SS_ENDELLIPSIS, -1);
    autoHide_ = make(hwnd_, WC_COMBOBOXW, L"", WS_VSCROLL | CBS_DROPDOWNLIST, kSetupAutoHide);
    for (const wchar_t* n : kAutoHideNames) SendMessageW(autoHide_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(n));
    autoHideTiles_ = choice(kSetupAutoHideTiles, QsUi::Choice::Style::Tiles, L"Auto-hide");
    {
        const Glyph glyphs[] = {Glyph::HideNever, Glyph::HideAlways, Glyph::HideOverlap};
        std::vector<QsUi::Choice::Option> opts;
        for (int i = 0; i < 3; ++i) opts.push_back(QsUi::Choice::Option{kAutoHideNames[i], kAutoHideDetails[i], glyphs[i], kAutoHideNames[i]});
        QsUi::Choice::SetOptions(autoHideTiles_, std::move(opts));
    }
    animationLabel_ = make(hwnd_, WC_STATICW, L"Animation", SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE, kSetupAnimationLabel);
    animationNote_ = make(hwnd_, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, -1);
    animation_ = make(hwnd_, WC_COMBOBOXW, L"", WS_VSCROLL | CBS_DROPDOWNLIST, kSetupAnimation);
    for (const wchar_t* n : kAnimationNames) SendMessageW(animation_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(n));
    animationChoice_ = choice(kSetupAnimationChoice, QsUi::Choice::Style::Segments, L"Animation");
    {
        std::vector<QsUi::Choice::Option> opts;
        for (const wchar_t* n : kAnimationNames) opts.push_back(QsUi::Choice::Option{n, L"", Glyph::None, n});
        QsUi::Choice::SetOptions(animationChoice_, std::move(opts));
    }
    // Step 4: the note under the picture and an Edit per summary line.
    doneNote_ = make(hwnd_, WC_STATICW, L"Show me around finishes too, then shows three short tips on the dock.", SS_LEFT | SS_NOPREFIX, -1);
    for (int k = 0; k < kSummaryMax; ++k) edits_[k] = make(hwnd_, WC_BUTTONW, L"Edit", WS_TABSTOP | BS_PUSHBUTTON, kSetupEdit + k);
    // The buttons (every step): Cancel on the left, then Back, Show me around, Next / Finish.
    cancel_ = make(hwnd_, WC_BUTTONW, L"Cancel", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kSetupCancel);
    back_ = make(hwnd_, WC_BUTTONW, L"Back", WS_TABSTOP | BS_PUSHBUTTON, kSetupBack);
    showAround_ = make(hwnd_, WC_BUTTONW, L"Show me around", WS_TABSTOP | BS_PUSHBUTTON, kSetupShowAround);
    next_ = make(hwnd_, WC_BUTTONW, L"Next", WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, kSetupNext);
    finish_ = make(hwnd_, WC_BUTTONW, L"Finish", WS_TABSTOP | BS_PUSHBUTTON, kSetupFinish);
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
        lf.lfCharSet = DEFAULT_CHARSET;
        wcscpy_s(lf.lfFaceName, L"Segoe UI");
    }
    for (HFONT f : {font_, fontBold_, fontHeading_, fontSmall_, fontLabel_}) {
        if (f) DeleteObject(f);
    }
    // 13H: the prototype's sizes in DIPs (14 body, 12 captions, 11 small capitals, 24 the heading),
    // on the system's message font face.
    auto make = [&](int dip, int weight) {
        LOGFONTW f = lf;
        f.lfHeight = -MulDiv(dip, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI);
        f.lfWidth = 0;
        f.lfWeight = weight;
        return CreateFontIndirectW(&f);
    };
    font_ = make(14, FW_NORMAL);
    fontBold_ = make(14, FW_SEMIBOLD);
    fontHeading_ = make(24, FW_SEMIBOLD);
    fontSmall_ = make(12, FW_NORMAL);
    fontLabel_ = make(11, FW_SEMIBOLD);
}

void QuickSetup::SetFonts() {
    auto set = [&](HWND c, HFONT f) {
        if (c) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
    };
    for (HWND c : {intro_, addDock_, addNote_, lookDock_, same_, sameCaption_, startBox_, taskbarBox_, autoHide_, animation_, back_,
                   showAround_, next_, finish_, cancel_}) {
        set(c, font_);
    }
    for (HWND c : {layoutNote_, alphaLabel_, alphaValue_, cornersLabel_, startHint_, taskbarHint_, behaviourNote_, autoHideHint_,
                   animationNote_, doneNote_}) {
        set(c, fontSmall_);
    }
    for (HWND c : edits_) set(c, font_);
    set(heading_, fontHeading_);
    set(autoHideLabel_, fontLabel_);
    // The switch cards' and the Animation card's titles.
    for (HWND c : {startCaption_, taskbarCaption_, animationLabel_}) set(c, fontBold_);
    for (int k = 0; k < kMaxDocks; ++k) {
        set(alpha_[k], font_);
        set(corners_[k], font_);
    }
    // The combos' selection fields as tall as the name edit (the prototype's 28 DIP fields).
    auto fieldHeight = [&](HWND combo) {
        if (combo) SendMessageW(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), std::max(8, Px(kFieldH) - 6));
    };
    fieldHeight(lookDock_);
    for (const Row& r : rows_) {
        for (HWND c : {r.name, r.layout, r.display, r.edge, r.remove}) set(c, font_);
        fieldHeight(r.layout);
        fieldHeight(r.display);
    }
    ApplyChoiceLooks();
}

void QuickSetup::ApplyChoiceLooks() {
    QsUi::Choice::Look look;
    look.colors = colors_;
    look.font = font_;
    look.bold = fontBold_;
    look.smallFont = fontSmall_;
    look.compactTiles = m_.narrow;
    look.back = colors_.card;  // the segmented controls sit on cards
    for (HWND c : cornersChoice_) QsUi::Choice::SetLook(c, look);
    QsUi::Choice::SetLook(animationChoice_, look);
    for (const Row& r : rows_) QsUi::Choice::SetLook(r.edgeChoice, look);
    look.back = colors_.base;  // the tiles sit on the window
    QsUi::Choice::SetLook(autoHideTiles_, look);
}

void QuickSetup::ApplyTheme() {
    palette_ = DarkMode::CurrentPalette();
    colors_ = QsUi::ColorsFor(palette_);
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
    for (HWND c : edits_) theme(c);
    for (int k = 0; k < kMaxDocks; ++k) {
        theme(corners_[k]);
        // A trackbar caches its background brush: a theme reset makes it ask WM_CTLCOLORSTATIC again
        // (it is custom-drawn anyway: QsUi::DrawSlider).
        if (alpha_[k]) SetWindowTheme(alpha_[k], nullptr, nullptr);
        galleries_[k].SetColors(colors_.base, colors_.text, colors_.muted, colors_.accent);
    }
    for (const Row& r : rows_) {
        for (HWND c : {r.name, r.layout, r.display, r.edge, r.remove}) theme(c);
    }
    if (rowsBox_) DarkMode::ThemeScrollBars(rowsBox_, dark);
    for (HBRUSH b : {baseBrush_, controlBrush_, cardBrush_}) {
        if (b) DeleteObject(b);
    }
    baseBrush_ = CreateSolidBrush(colors_.base);
    controlBrush_ = CreateSolidBrush(palette_.control);
    cardBrush_ = CreateSolidBrush(colors_.card);
    ApplyChoiceLooks();
    if (hwnd_) RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

// --- layout ----------------------------------------------------------------------------------------

int QuickSetup::TextHeight(HFONT font, const std::wstring& text, int widthDip) const {
    if (!hwnd_ || text.empty() || widthDip <= 0) return 0;
    HDC dc = GetDC(hwnd_);
    HGDIOBJ old = SelectObject(dc, font);
    RECT r{0, 0, Px(widthDip), 0};
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &r, DT_LEFT | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(hwnd_, dc);
    return (static_cast<int>(r.bottom) * USER_DEFAULT_SCREEN_DPI + static_cast<int>(dpi_) - 1) / static_cast<int>(dpi_);
}

void QuickSetup::Layout() {
    if (!hwnd_) return;
    const int W = widthDip_, H = heightDip_;
    Metrics& m = m_;
    m.narrow = W < kBigW;
    m.rail = m.narrow ? 200 : 236;
    m.pad = m.narrow ? 28 : 36;
    m.x0 = m.rail + m.pad;
    m.x1 = W - m.pad;
    m.cw = m.x1 - m.x0;
    m.buttonsTop = H - 18 - kButtonH;
    m.footerLine = m.buttonsTop - 14;
    m.bodyBottom = m.footerLine - 14;
    for (int s = 0; s < kSteps; ++s) {
        m.introH[s] = std::max(18, TextHeight(font_, kIntros[s], m.cw));
        m.bodyTop[s] = kIntroTop + m.introH[s] + 18;
    }
    auto place = [&](HWND h, int x, int y, int w, int hh) {
        if (h) SetWindowPos(h, nullptr, Px(x), Px(y), Px(std::max(0, w)), Px(std::max(0, hh)), SWP_NOZORDER | SWP_NOACTIVATE);
    };
    cards_.clear();
    // The rail: the app mark (painted), the steps, the info box at its foot.
    place(steps_, 12, kRailStepsTop, m.rail - 24, kSteps * kRailStepH + (kSteps - 1) * kRailStepGap);
    {
        const int infoH = TextHeight(fontSmall_, kInfo, m.rail - 24 - 50) + 24;
        infoDip_ = RECT{12, H - 20 - infoH, m.rail - 12, H - 20};
    }
    // The heading and the sentence (ShowStep sizes them for the step).
    place(heading_, m.x0, kHeadingTop, m.cw, kHeadingH);
    place(intro_, m.x0, kIntroTop, m.cw, m.introH[0]);

    // Step 1: the rows container as tall as its cards, or scrolling when they do not fit; Add a
    // dock right under it, the layout's description under that when there is room.
    {
        const int top = m.bodyTop[0];
        const int n = static_cast<int>(rows_.size());
        rowsContentDip_ = n * kRowCardH + std::max(0, n - 1) * kCardGap;
        const int avail = std::max(kRowCardH, m.bodyBottom - top - kAddH - 10);
        rowsVisibleDip_ = std::min(rowsContentDip_, avail);
        place(rowsBox_, m.x0, top, m.cw, rowsVisibleDip_);
        const bool scroll = rowsContentDip_ > rowsVisibleDip_;
        if (rowsBox_) {
            ShowScrollBar(rowsBox_, SB_VERT, scroll);
            SCROLLINFO si{sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS};
            si.nMin = 0;
            si.nMax = scroll ? Px(rowsContentDip_) - 1 : 0;
            si.nPage = scroll ? static_cast<UINT>(Px(rowsVisibleDip_)) : 0;
            rowsScroll_ = scroll ? std::clamp(rowsScroll_, 0, Px(rowsContentDip_) - Px(rowsVisibleDip_)) : 0;
            si.nPos = rowsScroll_;
            SetScrollInfo(rowsBox_, SB_VERT, &si, TRUE);
        }
        LayoutRows();
        const int addY = top + rowsVisibleDip_ + 10;
        place(addDock_, m.x0, addY, m.cw, kAddH);
        const int noteY = addY + kAddH + 12;
        layoutNoteFits_ = m.bodyBottom - noteY >= 34;
        place(layoutNote_, m.x0, noteY, m.cw, std::max(0, m.bodyBottom - noteY));
        place(addNote_, m.x0, noteY, m.cw, 18);
    }

    // Step 2: the look grid (four columns), then one card with Same look, Transparency, Corners.
    {
        const int top = m.bodyTop[1];
        const bool several = rows_.size() > 1;
        const bool twoRows = several && m.narrow;  // the small window: the switch on a row of its own
        const int cardH = twoRows ? 114 : 78;
        const int avail = std::max(96, m.bodyBottom - top - 16 - cardH);
        const int count = std::max(1, static_cast<int>(looks_.size()));
        const int gridRows = (count + 3) / 4;
        const int gap = SettingsGallery::kWideGap;
        const int tileH = std::clamp((avail - (gridRows - 1) * gap) / gridRows, 84, 134);
        const int galleryH = std::min(avail, gridRows * tileH + (gridRows - 1) * gap);
        for (int k = 0; k < kMaxDocks; ++k) {
            galleries_[k].SetWide(4, tileH);
            place(galleries_[k].Hwnd(), m.x0, top, m.cw, galleryH);
        }
        const int cardTop = top + galleryH + 16;
        cards_.push_back(Card{1, RECT{m.x0, cardTop, m.x1, cardTop + cardH}});
        const int cx = m.x0 + 18, cr = m.x1 - 18;
        if (several) {
            const int sy = twoRows ? cardTop + 12 : cardTop + (cardH - 24) / 2;
            place(same_, cx, sy, 44, 24);
            place(sameCaption_, cx + 56, sy, 176, 24);
        }
        const int rowTop = twoRows ? cardTop + 48 : cardTop + 12;
        const int cornersW = 190, cornersX = cr - cornersW;
        place(cornersLabel_, cornersX, rowTop, cornersW, 18);
        for (int k = 0; k < kMaxDocks; ++k) {
            place(cornersChoice_[k], cornersX, rowTop + 24, cornersW, kSegH);
            place(corners_[k], cornersX, rowTop + 24, cornersW, kDropH);
        }
        // Transparency takes what is left between the switch and Corners ("Transparency" needs ~80 DIP
        // of the 12 DIP font beside its value: 146 here with two docks in the big window).
        const int tx = several && !twoRows ? cx + 56 + 176 + 24 : cx;
        const int tw = std::max(60, cornersX - 24 - tx);
        place(alphaLabel_, tx, rowTop, tw - 46, 18);
        place(alphaValue_, tx + tw - 46, rowTop, 46, 18);
        for (int k = 0; k < kMaxDocks; ++k) place(alpha_[k], tx - 6, rowTop + 24, tw + 12, kSegH);
        place(lookDock_, m.x1 - 220, kHeadingTop + 2, 220, kDropH);
    }

    // Step 3: two switch cards side by side, the auto-hide tiles, the Animation card.
    {
        const int top = m.bodyTop[2];
        const int cardW = (m.cw - 10) / 2;
        // Paddings 14, a 32 DIP icon square, 12 to the text and to the switch. The prototype's 36 / 16
        // left the bold "Hide the Windows taskbar" 181 DIP: an ellipsis (seen 2026-11-10); the titles
        // are measured, and the icon squares go when a title would still not fit (the small window).
        int titleW = 0;
        if (HDC dc = GetDC(hwnd_)) {
            HGDIOBJ old = SelectObject(dc, fontBold_);
            for (const wchar_t* t : {L"Start with Windows", L"Hide the Windows taskbar"}) {
                SIZE sz{};
                GetTextExtentPoint32W(dc, t, static_cast<int>(wcslen(t)), &sz);
                titleW = std::max(titleW, MulDiv(sz.cx, USER_DEFAULT_SCREEN_DPI, static_cast<int>(dpi_)) + 2);
            }
            SelectObject(dc, old);
            ReleaseDC(hwnd_, dc);
        }
        const int bare = cardW - 14 - 12 - 44 - 14;
        const bool icons = !m.narrow && bare - (32 + 12) >= titleW;
        const int textW = bare - (icons ? 32 + 12 : 0);
        int hintH = 0;
        for (const wchar_t* h : {kStartHints[0], kStartHints[1], kTaskbarHints[0], kTaskbarHints[1]}) {
            hintH = std::max(hintH, TextHeight(fontSmall_, h, textW));
        }
        const int cardH = std::max(72, 14 + 20 + 2 + hintH + 14);
        const Glyph cardIcons[2] = {Glyph::StartUp, Glyph::Taskbar};
        HWND captions[2] = {startCaption_, taskbarCaption_}, hints[2] = {startHint_, taskbarHint_}, boxes[2] = {startBox_, taskbarBox_};
        for (int c = 0; c < 2; ++c) {
            const int left = m.x0 + c * (cardW + 10);
            Card card{2, RECT{left, top, left + cardW, top + cardH}};
            if (icons) {
                card.icon = cardIcons[c];
                card.iconDip = RECT{left + 14, top + (cardH - 32) / 2, left + 46, top + (cardH - 32) / 2 + 32};
            }
            cards_.push_back(card);
            const int tx = left + 14 + (icons ? 44 : 0);
            place(captions[c], tx, top + 14, textW, 20);
            place(hints[c], tx, top + 36, textW, hintH);
            place(boxes[c], left + cardW - 14 - 44, top + (cardH - 24) / 2, 44, 24);
        }
        // The rest from the note down; the note is left out if the step would not fit (the small window).
        auto rest = [&](bool note, bool commit) {
            int y = top + cardH + 8;
            if (note) {
                if (commit) place(behaviourNote_, m.x0, y, m.cw, 16);
                y += 16;
            }
            y += 12;
            if (commit) {
                place(autoHideLabel_, m.x0, y, m.cw / 2, 16);
                place(autoHideHint_, m.x0 + m.cw / 2, y, m.cw / 2, 16);
            }
            y += 16 + 8;
            place(autoHideTiles_, m.x0, y, m.cw, 100);
            const int tilesH = std::max(64, MulDiv(QsUi::Choice::TilesHeight(autoHideTiles_), USER_DEFAULT_SCREEN_DPI, static_cast<int>(dpi_)) + 1);
            if (commit) {
                place(autoHideTiles_, m.x0, y, m.cw, tilesH);
                place(autoHide_, m.x0, y, 300, kDropH);
            }
            y += tilesH + 14;
            if (commit) {
                cards_.push_back(Card{2, RECT{m.x0, y, m.x1, y + 66}});
                const int segW = 220;
                place(animationLabel_, m.x0 + 18, y + 13, m.cw - 36 - segW - 16, 20);
                place(animationNote_, m.x0 + 18, y + 35, m.cw - 36 - segW - 16, 17);
                place(animationChoice_, m.x1 - 18 - segW, y + 18, segW, kSegH);
                place(animation_, m.x1 - 18 - segW, y + 18, segW, kDropH);
            }
            return y + 66;
        };
        ApplyChoiceLooks();  // compact tiles follow the window size before they are measured
        behaviourNoteFits_ = rest(true, false) <= m.bodyBottom;
        rest(behaviourNoteFits_, true);
    }

    // Step 4: the desktop picture and its caption on the left, the summary card on the right.
    {
        const int top = m.bodyTop[3];
        const int pw = m.narrow ? 220 : 320, ph = pw * 200 / 320;
        previewDip_ = RECT{m.x0, top, m.x0 + pw, top + ph};
        captionDip_ = RECT{m.x0, top + ph + 8, m.x0 + pw, top + ph + 26};
        place(doneNote_, m.x0, top + ph + 32, pw, std::max(0, m.bodyBottom - (top + ph + 32)));
        summaryDip_ = RECT{m.x0 + pw + (m.narrow ? 16 : 20), top, m.x1, top};
        cards_.push_back(Card{3, summaryDip_});  // LayoutSummary sets its height
        LayoutSummary();
    }

    // The footer: Cancel at the content's left edge (its text lines up with the column), the others
    // right-aligned (ShowStep places Back: it moves with Show me around).
    place(cancel_, m.x0 - 12, m.buttonsTop, 92, kButtonH);
    place(next_, m.x1 - 104, m.buttonsTop, 104, kButtonH);
    place(finish_, m.x1 - 104, m.buttonsTop, 104, kButtonH);
    place(showAround_, m.x1 - 104 - 8 - 150, m.buttonsTop, 150, kButtonH);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

// Step 4's summary card: a line per dock, then look, start, taskbar, auto-hide, animation; each line
// the label over its value, the value wrapping to a second line when it needs one (the summary card
// is the narrow half of the column: "Modern · bottom edge of main display" does not fit one line).
// When the lines do not fit the column (the small window, three docks) they share the height and
// put label and value on one line. Called from Layout and from Sync (the values change the height).
void QuickSetup::LayoutSummary() {
    if (!hwnd_) return;
    constexpr int kPad = 9, kLabelH = 19, kValueLineH = 16, kEditW = 44;
    const std::vector<SummaryLine> lines = SummaryLines();
    const int n = static_cast<int>(lines.size());
    const int top = m_.bodyTop[3], avail = m_.bodyBottom - top - 8;
    const int valueW = (summaryDip_.right - summaryDip_.left) - 18 - 30 - (kEditW + 18);
    std::vector<int> heights(static_cast<size_t>(n));
    int total = 0;
    for (int k = 0; k < n; ++k) {
        const int vh = std::clamp(TextHeight(fontSmall_, lines[static_cast<size_t>(k)].value, valueW), kValueLineH, 2 * kValueLineH);
        heights[static_cast<size_t>(k)] = kPad + kLabelH + vh + kPad;
        total += heights[static_cast<size_t>(k)];
    }
    twoLine_ = total <= avail;
    if (!twoLine_) {
        // One value line each first, then one line for label and value together.
        total = 0;
        for (int& h : heights) total += (h = kPad + kLabelH + kValueLineH + kPad);
        twoLine_ = total <= avail;
        if (!twoLine_) {
            const int h = n ? std::max(28, avail / n) : 28;
            for (int& x : heights) x = h;
        }
    }
    lineTops_.assign(1, top + 4);
    for (int h : heights) lineTops_.push_back(lineTops_.back() + h);
    summaryDip_.bottom = lineTops_.back() + 4;
    for (Card& c : cards_) {
        if (c.step == 3) c.dip = summaryDip_;
    }
    for (int k = 0; k < kSummaryMax; ++k) {
        if (!edits_[k]) continue;
        if (k >= n) {
            ShowWindow(edits_[k], SW_HIDE);
            continue;
        }
        // Beside the label (the prototype's flex-start), or centred on a one-line row.
        const int t = lineTops_[static_cast<size_t>(k)];
        const int y = twoLine_ ? t + kPad + kLabelH / 2 - 13 : t + (lineTops_[static_cast<size_t>(k) + 1] - t - 26) / 2;
        SetWindowPos(edits_[k], nullptr, Px(m_.x1 - 10 - kEditW), Px(y), Px(kEditW), Px(26), SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(edits_[k], step_ == 3 ? SW_SHOWNA : SW_HIDE);
    }
}

// The columns of a dock card (DIPs from the card's left): the monitor picture (not in the small
// window), then two columns of fields, the remove button in the top right corner.
namespace {
struct CardColumns {
    int monitorX, fx, colW, c2, removeX;
    bool monitor;
};
CardColumns ColumnsFor(int cardW, bool narrow) {
    CardColumns c{};
    c.monitor = !narrow;
    c.monitorX = 16;
    c.fx = c.monitor ? 16 + 112 + 18 : 16;
    const int fr = cardW - 12 - 34 - 10;
    c.colW = std::max(60, (fr - c.fx - 10) / 2);
    c.c2 = c.fx + c.colW + 10;
    c.removeX = cardW - 12 - 34;
    return c;
}
}  // namespace

void QuickSetup::LayoutRows() {
    if (!rowsBox_) return;
    RECT client{};
    GetClientRect(rowsBox_, &client);
    const int cardW = MulDiv(client.right, USER_DEFAULT_SCREEN_DPI, static_cast<int>(dpi_));
    const CardColumns col = ColumnsFor(cardW, m_.narrow);
    auto place = [&](HWND h, int x, int y, int w, int hh) {
        // y is the card's DIPs; the scroll is in px.
        if (h) SetWindowPos(h, nullptr, Px(x), Px(y) - rowsScroll_, Px(w), Px(hh), SWP_NOZORDER | SWP_NOACTIVATE);
    };
    for (size_t r = 0; r < rows_.size(); ++r) {
        const int top = static_cast<int>(r) * (kRowCardH + kCardGap);
        const Row& row = rows_[r];
        place(row.name, col.fx, top + 30, col.colW, kFieldH);
        place(row.layout, col.c2, top + 30, col.colW, kDropH);
        place(row.display, col.fx, top + 85, col.colW, kDropH);
        place(row.edgeChoice, col.c2, top + 84, std::min(col.colW, 176), kSegH);
        place(row.edge, col.c2, top + 85, col.colW, kDropH);
        place(row.remove, col.removeX, top + 12, 34, 34);
    }
    InvalidateRect(rowsBox_, nullptr, TRUE);
}

void QuickSetup::ScrollRowsTo(int px) {
    if (!rowsBox_) return;
    const int max = std::max(0, Px(rowsContentDip_) - Px(rowsVisibleDip_));
    px = std::clamp(px, 0, max);
    if (px == rowsScroll_) return;
    rowsScroll_ = px;
    SCROLLINFO si{sizeof(si), SIF_POS};
    si.nPos = px;
    SetScrollInfo(rowsBox_, SB_VERT, &si, TRUE);
    LayoutRows();
}

void QuickSetup::EnsureRowVisible(size_t r) {
    if (rowsContentDip_ <= rowsVisibleDip_ || r >= rows_.size()) return;
    const int top = Px(static_cast<int>(r) * (kRowCardH + kCardGap)), bottom = top + Px(kRowCardH);
    if (top < rowsScroll_) {
        ScrollRowsTo(top);
    } else if (bottom > rowsScroll_ + Px(rowsVisibleDip_)) {
        ScrollRowsTo(bottom - Px(rowsVisibleDip_));
    }
}

std::wstring QuickSetup::LayoutName(size_t r) const {
    if (r >= rows_.size()) return {};
    const Row& row = rows_[r];
    const int preset = row.keepEntry ? row.layoutSel - 1 : row.layoutSel;
    const auto& all = Presets::All();
    return preset >= 0 && static_cast<size_t>(preset) < all.size() ? all[static_cast<size_t>(preset)].name : std::wstring(L"As it is now");
}

std::wstring QuickSetup::LookName(size_t r) const {
    if (!host_.config) return {};
    const DockConfig* d = DockOf(LookTarget(r));
    const std::wstring& id = d && d->OwnLook() ? d->themePreset : host_.config->themePreset;
    std::wstring name = id.empty() ? std::wstring(L"Default") : id;
    for (const Presets::Look& l : looks_) {
        if (_wcsicmp(l.id.c_str(), id.c_str()) == 0) name = l.name;
    }
    return name;
}

// --- the step rail ---------------------------------------------------------------------------------

RECT QuickSetup::StepRect(int step) const {
    RECT rc{};
    if (steps_) GetClientRect(steps_, &rc);
    const int top = Px(step * (kRailStepH + kRailStepGap));
    return RECT{0, top, rc.right, top + Px(kRailStepH)};
}

std::wstring QuickSetup::StepSub(int step) const {
    switch (step) {
        case 0:
            return rows_.size() == 1 ? std::wstring(L"One dock") : std::to_wstring(rows_.size()) + L" docks";
        case 1: {
            const std::wstring name = LookName(0);
            return rows_.size() > 1 && !SameLook() ? name + L", each dock its own" : name;
        }
        case 2: {
            const DockConfig* first = rows_.empty() ? nullptr : DockOf(rows_.front().id);
            const int ah = first && !first->Floating() ? std::clamp(static_cast<int>(first->autoHide), 0, 2) : 0;
            return std::wstring(start_ ? L"Starts with Windows" : L"Started by you") + L" \x00B7 " + Lower(kAutoHideNames[ah]);
        }
        default:
            return step_ == 3 ? L"Ready to finish" : L"Check and finish";
    }
}

void QuickSetup::PaintSteps(HWND hwnd, HDC hdc) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    QsUi::Buffer buf(hdc, rc);
    HDC dc = buf.Dc() ? buf.Dc() : hdc;
    const QsUi::Colors& k = colors_;
    HBRUSH rail = CreateSolidBrush(k.rail);
    FillRect(dc, &rc, rail);
    DeleteObject(rail);
    const float s = Scale();
    const bool focused = GetFocus() == hwnd && !(SendMessageW(hwnd_, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
    {
        QsUi::Canvas c(dc, buf.W(), buf.H());
        for (int i = 0; i < kSteps; ++i) {
            const RECT r = StepRect(i);
            const bool current = i == step_, done = i < step_;
            if (current || i == stepsHot_) {
                c.FillRound(QsUi::F(r), 8 * s, k.highContrast ? k.rail : QsUi::Tint(k.text, k.rail, current ? 7 : 5));
                if (k.highContrast) c.StrokeRound(QsUi::F(r), 8 * s, current ? k.accent : k.text, std::max(1.0f, std::round(s)));
            }
            const float cx = static_cast<float>(Px(12 + 14)), cy = (r.top + r.bottom) / 2.0f, rad = 14.0f * s;
            if (current) {
                c.FillCircle(cx, cy, rad, k.accent);
            } else if (done) {
                c.FillCircle(cx, cy, rad, k.accent, k.highContrast ? 1.0f : 0.18f);
                const float g = std::round(13.0f * s);
                QsUi::DrawGlyph(c, Glyph::Check, D2D1::RectF(cx - g / 2, cy - g / 2, cx + g / 2, cy + g / 2), k.highContrast ? k.accentText : k.accent);
            } else {
                c.StrokeCircle(cx, cy, rad - 0.75f * s, k.highContrast ? k.text : QsUi::Tint(k.text, k.rail, 22), 1.5f * s);
            }
        }
    }
    SetBkMode(dc, TRANSPARENT);
    for (int i = 0; i < kSteps; ++i) {
        const RECT r = StepRect(i);
        const bool current = i == step_, done = i < step_;
        if (!done) {
            wchar_t num[4];
            swprintf_s(num, L"%d", i + 1);
            RECT n{Px(12), r.top, Px(12 + 28), r.bottom};
            HGDIOBJ old = SelectObject(dc, fontBold_);
            SetTextColor(dc, current ? k.accentText : k.muted);
            DrawTextW(dc, num, -1, &n, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, old);
        }
        const int tx = Px(12 + 28 + 12);
        RECT name{tx, r.top + Px(8), r.right - Px(8), r.top + Px(8 + 19)};
        HGDIOBJ old = SelectObject(dc, current ? fontBold_ : font_);
        SetTextColor(dc, k.text);
        DrawTextW(dc, kStepNames[i], -1, &name, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(dc, fontSmall_);
        SetTextColor(dc, k.muted);
        const std::wstring sub = StepSub(i);
        RECT subRect{tx, name.bottom + Px(1), r.right - Px(8), r.bottom - Px(4)};
        DrawTextW(dc, sub.c_str(), -1, &subRect, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(dc, old);
        if (current && focused) {
            RECT f = r;
            InflateRect(&f, -Px(2), -Px(2));
            SetTextColor(dc, RGB(0, 0, 0));
            SetBkColor(dc, RGB(0xFF, 0xFF, 0xFF));
            DrawFocusRect(dc, &f);
        }
    }
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
        case WM_MOUSEMOVE: {
            if (!self->stepsTracking_) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                self->stepsTracking_ = TrackMouseEvent(&tme) != FALSE;
            }
            const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int hot = -1;
            for (int s = 0; s < kSteps; ++s) {
                const RECT r = self->StepRect(s);
                if (PtInRect(&r, pt)) hot = s;
            }
            if (hot != self->stepsHot_) {
                self->stepsHot_ = hot;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            self->stepsTracking_ = false;
            if (self->stepsHot_ >= 0) {
                self->stepsHot_ = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
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
            // The rail is a column now: up / down walk it (left / right still do, as 12G's track did).
            if ((wp == VK_UP || wp == VK_LEFT) && self->step_ > 0) self->GoTo(self->step_ - 1);
            if ((wp == VK_DOWN || wp == VK_RIGHT) && self->step_ + 1 < kSteps) self->GoTo(self->step_ + 1);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// --- painting --------------------------------------------------------------------------------------

bool QuickSetup::OnCard(HWND control) const {
    if (!control || !hwnd_ || GetParent(control) != hwnd_) return false;
    RECT r{};
    GetWindowRect(control, &r);
    MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&r), 2);
    const POINT c{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
    for (const Card& k : cards_) {
        if (k.step != step_) continue;
        const RECT px = PxRect(k.dip);
        if (PtInRect(&px, c)) return true;
    }
    return false;
}

COLORREF QuickSetup::BackOf(HWND control) const {
    if (control && rowsBox_ && GetParent(control) == rowsBox_) return colors_.card;  // a dock card's
    return OnCard(control) ? colors_.card : colors_.base;
}

void QuickSetup::PaintRows(HWND hwnd, HDC hdc) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    QsUi::Buffer buf(hdc, rc);
    HDC dc = buf.Dc() ? buf.Dc() : hdc;
    FillRect(dc, &rc, baseBrush_);
    const QsUi::Colors& k = colors_;
    const float s = Scale();
    const int cardW = MulDiv(rc.right, USER_DEFAULT_SCREEN_DPI, static_cast<int>(dpi_));
    const CardColumns col = ColumnsFor(cardW, m_.narrow);
    auto y = [&](int dip) { return static_cast<float>(Px(dip) - rowsScroll_); };
    {
        QsUi::Canvas c(dc, buf.W(), buf.H());
        for (size_t r = 0; r < rows_.size(); ++r) {
            const int top = static_cast<int>(r) * (kRowCardH + kCardGap);
            const D2D1_RECT_F card = D2D1::RectF(0.0f, y(top), static_cast<float>(rc.right), y(top + kRowCardH));
            c.FillRound(card, kCardRadius * s, k.card);
            c.StrokeRound(card, kCardRadius * s, k.border, std::max(1.0f, std::round(s)));
            if (!col.monitor) continue;
            // The dock's monitor: the wallpaper and a short accent bar on its edge.
            const DockConfig* d = DockOf(rows_[r].id);
            const float mx = static_cast<float>(Px(col.monitorX)), my = y(top + (kRowCardH - 70) / 2);
            const D2D1_RECT_F mon = D2D1::RectF(mx, my, mx + Px(112), my + Px(70));
            c.FillWallpaper(mon, 6 * s);
            c.StrokeRound(mon, 6 * s, RGB(0xFF, 0xFF, 0xFF), std::max(1.0f, std::round(s)), 0.10f);
            const int edge = d ? static_cast<int>(d->edge) : 0;
            const float w = mon.right - mon.left, h = mon.bottom - mon.top, inset = 5 * s, thick = 8 * s;
            D2D1_RECT_F bar{};
            switch (edge) {
                case 1: bar = D2D1::RectF(mon.left + w * 0.24f, mon.top + inset, mon.right - w * 0.24f, mon.top + inset + thick); break;
                case 2: bar = D2D1::RectF(mon.left + inset, mon.top + h * 0.24f, mon.left + inset + thick, mon.bottom - h * 0.24f); break;
                case 3: bar = D2D1::RectF(mon.right - inset - thick, mon.top + h * 0.24f, mon.right - inset, mon.bottom - h * 0.24f); break;
                default: bar = D2D1::RectF(mon.left + w * 0.24f, mon.bottom - inset - thick, mon.right - w * 0.24f, mon.bottom - inset); break;
            }
            c.FillRound(bar, 3 * s, k.accent, 0.9f);
        }
    }
    // The small capitals over each field.
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ old = SelectObject(dc, fontLabel_);
    SetTextColor(dc, k.muted);
    for (size_t r = 0; r < rows_.size(); ++r) {
        const int top = static_cast<int>(r) * (kRowCardH + kCardGap);
        const struct {
            int x, y;
            const wchar_t* text;
        } labels[] = {{col.fx, top + 12, L"NAME"}, {col.c2, top + 12, L"LAYOUT"}, {col.fx, top + 66, L"DISPLAY"}, {col.c2, top + 66, L"EDGE"}};
        for (const auto& l : labels) {
            RECT t{Px(l.x), Px(l.y) - rowsScroll_, Px(l.x + col.colW), Px(l.y + 16) - rowsScroll_};
            DrawTextW(dc, l.text, -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    }
    SelectObject(dc, old);
}

void QuickSetup::PaintPreview(QsUi::Canvas& c) {
    if (!host_.config) return;
    const float s = Scale();
    const D2D1_RECT_F pr = QsUi::F(PxRect(previewDip_));
    c.FillWallpaper(pr, 10 * s);
    c.StrokeRound(pr, 10 * s, RGB(0xFF, 0xFF, 0xFF), std::max(1.0f, std::round(s)), 0.10f);
    // Every dock on its edge in its look, at the picture's scale (the prototype: a 24 DIP bar half
    // the side long, 10 from the edge, on a 320 DIP desktop), four icons each (the gallery's colours).
    const float u = (pr.right - pr.left) / 320.0f;
    constexpr uint32_t kIconColors[4] = {0xFF3A86FF, 0xFF34C759, 0xFFFF9F0A, 0xFFAF52DE};
    const float w = pr.right - pr.left, h = pr.bottom - pr.top, thick = std::round(24 * u), inset = std::round(10 * u);
    for (const Row& row : rows_) {
        const DockConfig* d = DockOf(row.id);
        if (!d) continue;
        const ThemeConfig& t = d->OwnLook() ? d->theme : host_.config->theme;
        const int edge = static_cast<int>(d->edge);
        const bool vertical = edge == 2 || edge == 3;
        D2D1_RECT_F bar{};
        switch (edge) {
            case 1: bar = D2D1::RectF(pr.left + w * 0.25f, pr.top + inset, pr.right - w * 0.25f, pr.top + inset + thick); break;
            case 2: bar = D2D1::RectF(pr.left + inset, pr.top + h * 0.25f, pr.left + inset + thick, pr.bottom - h * 0.25f); break;
            case 3: bar = D2D1::RectF(pr.right - inset - thick, pr.top + h * 0.25f, pr.right - inset, pr.bottom - h * 0.25f); break;
            default: bar = D2D1::RectF(pr.left + w * 0.25f, pr.bottom - inset - thick, pr.right - w * 0.25f, pr.bottom - inset); break;
        }
        const float radius = std::min(t.cornerRadius * 0.5f * u, thick / 2.0f);
        const bool acrylic = t.acrylic && !t.shelf && !t.perspective;
        if (acrylic) {
            // The blur behind the bar, then its tint (as the gallery's mini dock stands it in).
            c.FillRound(bar, radius, RGB(0x2A, 0x3D, 0x63), 0.85f);
            c.FillArgb(bar, radius, t.acrylicBackground);
        } else {
            c.FillArgb(bar, radius, t.background);
        }
        c.StrokeArgb(bar, radius, t.barBorder, std::max(1.0f, std::round(s)));
        const float icon = std::round(9 * u), gap = std::round(5 * u), run = 4 * icon + 3 * gap;
        for (int i = 0; i < 4; ++i) {
            const float along = (vertical ? (bar.top + bar.bottom) : (bar.left + bar.right)) / 2.0f - run / 2.0f + static_cast<float>(i) * (icon + gap);
            const float across = (vertical ? (bar.left + bar.right) : (bar.top + bar.bottom)) / 2.0f - icon / 2.0f;
            const D2D1_RECT_F ir = vertical ? D2D1::RectF(across, along, across + icon, along + icon) : D2D1::RectF(along, across, along + icon, across + icon);
            c.FillArgb(ir, 2 * u, kIconColors[i]);
        }
    }
}

std::vector<QuickSetup::SummaryLine> QuickSetup::SummaryLines() const {
    std::vector<SummaryLine> lines;
    if (!host_.config) return lines;
    const Config& cfg = *host_.config;
    for (size_t r = 0; r < rows_.size(); ++r) {
        const DockConfig* d = DockOf(rows_[r].id);
        if (!d) {
            lines.push_back(SummaryLine{rows_[r].id, L"", 0});
            continue;
        }
        std::wstring words = LayoutName(r);
        const std::wstring edge = Lower(kEdgeNames[static_cast<int>(d->edge) & 3]);
        const int sel = DisplaySel(d->monitor);
        // The prototype's words ("bottom edge of main display"): shorter than the combo's "Display 1 (main)".
        const std::wstring display = d->monitor == DockConfig::kMonitorEach ? L"every display"
                                     : sel <= 0                              ? L"main display"
                                                                             : L"display " + std::to_wstring(sel + 1);
        words += L" \x00B7 " + edge + L" edge of " + display;
        lines.push_back(SummaryLine{d->DisplayName(), words, 0});
    }
    std::wstring look;
    if (rows_.size() > 1 && !SameLook()) {
        look = L"Each dock its own look";
    } else {
        const DockConfig* d = DockOf(LookTarget(0));
        const ThemeConfig& values = d && d->OwnLook() ? d->theme : cfg.theme;
        look = LookName(0);
        if (rows_.size() > 1) look += L" for every dock";
        const int alpha = std::clamp(Theme::Derived(Theme::Quick::Transparency).get(values), 0, 100);
        const int corners = Theme::Derived(Theme::Quick::Corners).get(values);
        look += L" \x00B7 " + std::to_wstring(alpha) + L" % transparent \x00B7 " +
                (corners >= 0 && corners <= 2 ? Lower(kCornerNames[corners]) : std::wstring(L"mixed")) + L" corners";
    }
    lines.push_back(SummaryLine{L"Look", look, 1});
    lines.push_back(SummaryLine{L"Starts with Windows", start_ ? L"Yes" : L"No, you start it yourself", 2});
    lines.push_back(SummaryLine{L"Windows taskbar", hideTaskbar_ ? L"Hidden while WinDock runs" : L"Stays beside the dock", 2});
    const DockConfig* first = rows_.empty() ? nullptr : DockOf(rows_.front().id);
    lines.push_back(SummaryLine{L"Auto-hide",
                                !first             ? L"Never"
                                : first->Floating() ? L"Never (a floating dock)"
                                                    : kAutoHideNames[std::clamp(static_cast<int>(first->autoHide), 0, 2)],
                                2});
    const AnimationConfig& a = cfg.animation;
    lines.push_back(SummaryLine{L"Animation", kAnimationNames[a.speed == 0.0 ? 2 : a.reduceMotion == ReduceMotion::On ? 1 : 0], 2});
    return lines;
}

void QuickSetup::PaintContent(HDC hdc) {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    QsUi::Buffer buf(hdc, rc);
    HDC dc = buf.Dc() ? buf.Dc() : hdc;
    const QsUi::Colors& k = colors_;
    const float s = Scale();
    const float one = std::max(1.0f, std::round(s));
    FillRect(dc, &rc, baseBrush_);
    // The rail (full height, a hairline on its right) and the footer's hairline over the column.
    {
        HBRUSH rail = CreateSolidBrush(k.rail);
        const RECT r{0, 0, Px(m_.rail), rc.bottom};
        FillRect(dc, &r, rail);
        DeleteObject(rail);
        HBRUSH hair = CreateSolidBrush(k.highContrast ? k.text : QsUi::Tint(k.text, k.rail, 7));
        const RECT edge{Px(m_.rail) - static_cast<int>(one), 0, Px(m_.rail), rc.bottom};
        FillRect(dc, &edge, hair);
        DeleteObject(hair);
        HBRUSH foot = CreateSolidBrush(k.hair);
        const RECT f{Px(m_.rail), Px(m_.footerLine), rc.right, Px(m_.footerLine) + static_cast<int>(one)};
        FillRect(dc, &f, foot);
        DeleteObject(foot);
    }
    const std::vector<SummaryLine> lines = step_ == 3 ? SummaryLines() : std::vector<SummaryLine>{};
    const RECT info = PxRect(infoDip_);
    {
        QsUi::Canvas c(dc, buf.W(), buf.H());
        // The app mark: a rounded square in the accent (lighter to darker) with the dock glyph.
        const D2D1_RECT_F mark = QsUi::F(PxRect(RECT{24, 16, 54, 46}));
        c.FillGradient(mark, 8 * s, k.accent, k.highContrast ? k.accent : QsUi::Mix(RGB(0, 0, 0), k.accent, 40));
        const float g = std::round(18 * s), mx = (mark.left + mark.right) / 2, my = (mark.top + mark.bottom) / 2;
        QsUi::DrawGlyph(c, Glyph::DockMark, D2D1::RectF(mx - g / 2, my - g / 2, mx + g / 2, my + g / 2), k.accentText);
        // The info box: accent-tinted, an info glyph beside its sentence.
        if (k.highContrast) {
            c.StrokeRound(QsUi::F(info), 8 * s, k.text, one);
        } else {
            c.FillRound(QsUi::F(info), 8 * s, k.accent, 0.07f);
        }
        const float ig = std::round(16 * s);
        QsUi::DrawGlyph(c, Glyph::Info, D2D1::RectF(static_cast<float>(info.left + Px(12)), static_cast<float>(info.top + Px(13)), static_cast<float>(info.left + Px(12)) + ig, static_cast<float>(info.top + Px(13)) + ig),
                        k.highContrast ? k.text : k.accent);
        // The step's cards (an icon in an accent-tinted square on step 3's switch cards).
        for (const Card& card : cards_) {
            if (card.step != step_) continue;
            const D2D1_RECT_F r = QsUi::F(PxRect(card.dip));
            c.FillRound(r, kCardRadius * s, k.card);
            c.StrokeRound(r, kCardRadius * s, k.border, one);
            if (card.icon != Glyph::None) {
                const D2D1_RECT_F ir = QsUi::F(PxRect(card.iconDip));
                if (k.highContrast) {
                    c.StrokeRound(ir, 8 * s, k.text, one);
                } else {
                    c.FillRound(ir, 8 * s, k.accent, 0.12f);
                }
                const float cx = (ir.left + ir.right) / 2, cy = (ir.top + ir.bottom) / 2, ig2 = std::round(18 * s);
                QsUi::DrawGlyph(c, card.icon, D2D1::RectF(cx - ig2 / 2, cy - ig2 / 2, cx + ig2 / 2, cy + ig2 / 2), k.highContrast ? k.text : k.accent);
            }
        }
        // Step 4: the desktop picture, then the summary's check circles and dividers.
        if (step_ == 3) {
            PaintPreview(c);
            const int left = summaryDip_.left + 18, right = summaryDip_.right - 18;
            for (size_t i = 0; i < lines.size() && i + 1 < lineTops_.size(); ++i) {
                const int top = lineTops_[i], bottom = lineTops_[i + 1];
                const int labelTop = twoLine_ ? top + 9 : top + (bottom - top - 19) / 2;
                const float cy = static_cast<float>(Px(labelTop + 10)), cx = static_cast<float>(Px(left + 9));
                c.FillCircle(cx, cy, 9 * s, k.accent, k.highContrast ? 1.0f : 0.18f);
                const float cg = std::round(10 * s);
                QsUi::DrawGlyph(c, Glyph::Check, D2D1::RectF(cx - cg / 2, cy - cg / 2, cx + cg / 2, cy + cg / 2), k.highContrast ? k.accentText : k.accent);
                if (i + 1 < lines.size()) {
                    const float ly = static_cast<float>(Px(bottom)) - one / 2;
                    c.Line(static_cast<float>(Px(left)), ly, static_cast<float>(Px(right)), ly, k.highContrast ? k.text : QsUi::Tint(k.text, k.card, 7), one);
                }
            }
        }
    }
    // The text over the shapes (GDI: the system controls' fonts).
    SetBkMode(dc, TRANSPARENT);
    auto text = [&](const std::wstring& t, RECT r, HFONT f, COLORREF color, UINT flags) {
        HGDIOBJ old = SelectObject(dc, f);
        SetTextColor(dc, color);
        DrawTextW(dc, t.c_str(), static_cast<int>(t.size()), &r, flags | DT_NOPREFIX);
        SelectObject(dc, old);
    };
    text(L"WinDock", PxRect(RECT{64, 13, m_.rail - 12, 31}), fontBold_, k.text, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    text(firstRun_ ? L"Welcome! Set up in four steps" : L"Set up in four steps", PxRect(RECT{64, 31, m_.rail - 12, 49}), fontSmall_, k.muted,
         DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    text(kInfo, RECT{info.left + Px(12 + 16 + 10), info.top + Px(12), info.right - Px(12), info.bottom - Px(8)}, fontSmall_, k.muted,
         DT_LEFT | DT_TOP | DT_WORDBREAK);
    if (step_ == 3) {
        std::wstring caption;
        if (rows_.size() == 1 && !lines.empty()) {
            caption = lines.front().label + L" \x00B7 " + Lower(lines.front().value);
        } else {
            caption = std::to_wstring(rows_.size()) + L" docks on your desktop";
        }
        text(caption, PxRect(captionDip_), fontSmall_, k.faint, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        const int left = summaryDip_.left + 18 + 30, right = summaryDip_.right - 18 - 44;
        for (size_t i = 0; i < lines.size() && i + 1 < lineTops_.size(); ++i) {
            const int top = lineTops_[i], bottom = lineTops_[i + 1];
            if (twoLine_) {
                text(lines[i].label, PxRect(RECT{left, top + 9, right, top + 9 + 19}), fontBold_, k.text,
                     DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
                // Wrapped (LayoutSummary gave the line the value's height, two lines at most).
                text(lines[i].value, PxRect(RECT{left, top + 9 + 19, right, bottom - 7}), fontSmall_, k.muted,
                     DT_LEFT | DT_TOP | DT_WORDBREAK | DT_EDITCONTROL | DT_END_ELLIPSIS);
            } else {
                // A short window: label and value on one line.
                const RECT t = PxRect(RECT{left, top, right, bottom});
                HGDIOBJ old = SelectObject(dc, fontBold_);
                SIZE sz{};
                GetTextExtentPoint32W(dc, lines[i].label.c_str(), static_cast<int>(lines[i].label.size()), &sz);
                SelectObject(dc, old);
                text(lines[i].label, t, fontBold_, k.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                RECT v = t;
                v.left = std::min(v.right, v.left + sz.cx + Px(10));
                text(lines[i].value, v, fontSmall_, k.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
        }
    }
}

LRESULT QuickSetup::OnCustomDraw(NMCUSTOMDRAW& cd) {
    HWND h = cd.hdr.hwndFrom;
    wchar_t cls[32]{};
    GetClassNameW(h, cls, static_cast<int>(std::size(cls)));
    const float s = Scale();
    if (_wcsicmp(cls, TRACKBAR_CLASSW) == 0) return QsUi::DrawSlider(cd, colors_, BackOf(h), s);
    if (_wcsicmp(cls, WC_BUTTONW) != 0 || cd.dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
    UINT state = cd.uItemState;
    // The cues as the window has them (a button's own bit lags a keyboard first use).
    if (SendMessageW(hwnd_, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) {
        state &= ~static_cast<UINT>(CDIS_SHOWKEYBOARDCUES);
    } else {
        state |= CDIS_SHOWKEYBOARDCUES;
    }
    if (!IsWindowEnabled(h)) state |= CDIS_DISABLED;
    if (GetFocus() == h) state |= CDIS_FOCUS;
    if (h == same_ || h == startBox_ || h == taskbarBox_) {
        QsUi::DrawSwitch(cd.hdc, cd.rc, Checked(h), state, colors_, BackOf(h), s);
        return CDRF_SKIPDEFAULT;
    }
    QsUi::ButtonKind kind = QsUi::ButtonKind::Standard;
    Glyph glyph = Glyph::None;
    HFONT font = font_;
    std::wstring label = TextOf(h);
    if (h == next_ || h == finish_) {
        kind = QsUi::ButtonKind::Primary;
        font = fontBold_;
    } else if (h == cancel_) {
        kind = QsUi::ButtonKind::Ghost;
    } else if (h == addDock_) {
        kind = QsUi::ButtonKind::Dashed;
        if (IsWindowEnabled(h)) glyph = Glyph::Add;
    } else if (std::find(std::begin(edits_), std::end(edits_), h) != std::end(edits_)) {
        kind = QsUi::ButtonKind::Link;
    } else {
        for (const Row& r : rows_) {
            if (h == r.remove) {
                kind = QsUi::ButtonKind::Icon;
                glyph = Glyph::Close;
                label.clear();  // its text "Remove" is its accessible name
            }
        }
    }
    QsUi::DrawButton(cd.hdc, cd.rc, kind, label, glyph, state, font, colors_, BackOf(h), s);
    return CDRF_SKIPDEFAULT;
}

// --- steps -----------------------------------------------------------------------------------------

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
    auto place = [&](HWND h, int x, int y, int w, int hh) {
        if (h) SetWindowPos(h, nullptr, Px(x), Px(y), Px(std::max(0, w)), Px(std::max(0, hh)), SWP_NOZORDER | SWP_NOACTIVATE);
    };
    const bool s1 = step_ == 0, s2 = step_ == 1, s3 = step_ == 2, s4 = step_ == 3;
    const bool several = rows_.size() > 1;
    const bool lookOf = s2 && several && !SameLook();
    SetText(heading_, kHeadings[step_]);
    SetText(intro_, kIntros[step_]);
    // The heading leaves room for "Look of" beside it; the sentence is as tall as the step's.
    place(heading_, m_.x0, kHeadingTop, m_.cw - (lookOf ? 232 : 0), kHeadingH);
    place(intro_, m_.x0, kIntroTop, m_.cw, m_.introH[step_]);
    show(rowsBox_, s1);
    show(addDock_, s1);
    show(addNote_, false);
    show(layoutNote_, s1 && layoutNoteFits_);
    for (const Row& r : rows_) show(r.edge, false);  // the hidden state holder of the edge choice
    // Step 2: one look row shown; Same look only with two or more docks.
    show(same_, s2 && several);
    show(sameCaption_, s2 && several);
    show(lookDock_, lookOf);
    const size_t shown = LookShown();
    for (int k = 0; k < kMaxDocks; ++k) {
        const bool on = s2 && static_cast<size_t>(k) == shown;
        show(galleries_[k].Hwnd(), on);
        show(alpha_[k], on);
        show(cornersChoice_[k], on);
        show(corners_[k], false);
    }
    show(alphaLabel_, s2);
    show(alphaValue_, s2);
    show(cornersLabel_, s2);
    for (HWND h : {startBox_, startCaption_, startHint_, taskbarBox_, taskbarCaption_, taskbarHint_, autoHideLabel_, autoHideTiles_,
                   animationLabel_, animationChoice_, animationNote_}) {
        show(h, s3);
    }
    show(behaviourNote_, s3 && behaviourNoteFits_);
    show(autoHideHint_, s3 && firstFloating_);
    show(autoHide_, false);
    show(animation_, false);
    show(doneNote_, s4);
    const int lines = static_cast<int>(rows_.size()) + 5;
    for (int k = 0; k < kSummaryMax; ++k) show(edits_[k], s4 && k < lines);
    // The footer: Back from step 2 on, Next until step 3 ("Review" on it), Finish and Show me
    // around on the last.
    show(back_, step_ > 0);
    EnableWindow(back_, step_ > 0);
    show(next_, !s4);
    show(finish_, s4);
    show(showAround_, s4);
    SetText(next_, step_ == 2 ? L"Review" : L"Next");
    place(back_, s4 ? m_.x1 - 104 - 8 - 150 - 8 - 90 : m_.x1 - 104 - 8 - 90, m_.buttonsTop, 90, kButtonH);
    SendMessageW(next_, BM_SETSTYLE, s4 ? BS_PUSHBUTTON : BS_DEFPUSHBUTTON, TRUE);
    SendMessageW(finish_, BM_SETSTYLE, s4 ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON, TRUE);
    InvalidateRect(hwnd_, nullptr, TRUE);  // the step's cards
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

// --- the rows (step 1) ---------------------------------------------------------------------------------

void QuickSetup::CreateRow(size_t r) {
    if (r >= rows_.size() || !rowsBox_) return;
    Row& row = rows_[r];
    const int base = 10 * static_cast<int>(r);
    auto make = [&](const wchar_t* cls, DWORD s, int part, DWORD ex = 0) {
        return CreateWindowExW(ex, cls, L"", WS_CHILD | WS_TABSTOP | s, 0, 0, 0, 0, rowsBox_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(base + part)), instance_, nullptr);
    };
    row.name = make(WC_EDITW, WS_VISIBLE | ES_AUTOHSCROLL, kRowName, WS_EX_CLIENTEDGE);
    if (row.name) SendMessageW(row.name, EM_LIMITTEXT, 64, 0);
    row.layout = make(WC_COMBOBOXW, WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST, kRowLayout);
    row.display = make(WC_COMBOBOXW, WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST, kRowDisplay);
    // 13H: the edge combo is the hidden state holder; the segmented control after it is what is seen.
    row.edge = make(WC_COMBOBOXW, WS_VSCROLL | CBS_DROPDOWNLIST, kRowEdge);
    row.edgeChoice = QsUi::Choice::Create(rowsBox_, base + kRowEdgeChoice, QsUi::Choice::Style::Segments, L"Edge", instance_);
    if (row.edgeChoice) {
        const Glyph glyphs[4] = {Glyph::EdgeTop, Glyph::EdgeBottom, Glyph::EdgeLeft, Glyph::EdgeRight};
        const wchar_t* names[4] = {L"Top", L"Bottom", L"Left", L"Right"};
        std::vector<QsUi::Choice::Option> opts;
        for (int i = 0; i < 4; ++i) opts.push_back(QsUi::Choice::Option{L"", L"", glyphs[i], names[i]});
        QsUi::Choice::SetOptions(row.edgeChoice, std::move(opts));
        ShowWindow(row.edgeChoice, SW_SHOWNA);
    }
    // The first dock stays (Quick setup sets up at least one); the others can go again. BS_NOTIFY:
    // its focus scrolls the cards (BN_SETFOCUS).
    row.remove = r > 0 ? make(WC_BUTTONW, WS_VISIBLE | BS_PUSHBUTTON | BS_NOTIFY, kRowRemove) : nullptr;
    if (row.remove) SetWindowTextW(row.remove, L"Remove");
    FillRowCombos(r);
    for (HWND c : {row.name, row.layout, row.display, row.edge, row.remove}) {
        if (!c) continue;
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
        DarkMode::AllowDarkForWindow(c, palette_.dark);
        DarkMode::ThemeControl(c, palette_.dark);
    }
    for (HWND c : {row.layout, row.display}) {
        if (c) SendMessageW(c, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), std::max(8, Px(kFieldH) - 6));
    }
    if (row.name) SendMessageW(row.name, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(Px(6), Px(6)));
    ApplyChoiceLooks();
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
    QsUi::Choice::SetSelected(row.edgeChoice, SegOfEdge(static_cast<int>(d->edge)));
    SetComboSel(row.layout, row.layoutSel);
}

void QuickSetup::DestroyRows() {
    for (Row& row : rows_) {
        for (HWND c : {row.name, row.layout, row.display, row.edge, row.remove, row.edgeChoice}) {
            if (c) DestroyWindow(c);
        }
        row.name = row.layout = row.display = row.edge = row.remove = row.edgeChoice = nullptr;
    }
}

void QuickSetup::RebuildRows() {
    // The ids say the row (10 * row + part): after a row went, every row is made again.
    const HWND focus = GetFocus();
    const bool hadFocus = focus && rowsBox_ && IsChild(rowsBox_, focus);
    DestroyRows();
    for (size_t r = 0; r < rows_.size(); ++r) CreateRow(r);
    Layout();  // the rows container's height and Add a dock follow the row count
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
    if (r >= rows_.size()) return;
    // A row control got the focus (Tab into a card scrolled out of view): the card comes into view.
    if ((part == kRowName && code == EN_SETFOCUS) || ((part == kRowLayout || part == kRowDisplay) && code == CBN_SETFOCUS) ||
        (part == kRowRemove && code == BN_SETFOCUS) || (part == kRowEdgeChoice && code == QsUi::Choice::kFocused)) {
        EnsureRowVisible(r);
        return;
    }
    if (syncing_) return;
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
            // What the layout is, under the cards (its description).
            const auto& all = Presets::All();
            const std::wstring note = preset >= 0 && static_cast<size_t>(preset) < all.size()
                                          ? all[static_cast<size_t>(preset)].name + L": " + all[static_cast<size_t>(preset)].description
                                          : NameOf(row.id) + L": as it was when Quick setup opened.";
            SetText(layoutNote_, note);
            if (steps_) InvalidateRect(steps_, nullptr, FALSE);
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
            if (rowsBox_) InvalidateRect(rowsBox_, nullptr, FALSE);  // the card's monitor
            return;
        }
        case kRowEdgeChoice: {
            // 13H: the segmented control sets the hidden combo and runs its handler (one path).
            if (code != QsUi::Choice::kChosen) return;
            const int seg = QsUi::Choice::Selected(control);
            if (seg < 0 || seg > 3 || !row.edge) return;
            SetComboSel(row.edge, kSegEdge[seg]);
            OnRowCommand(10 * static_cast<int>(r) + kRowEdge, CBN_SELCHANGE, row.edge);
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
    Layout();
    Sync();
    EnsureRowVisible(rows_.size() - 1);
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
            t.detail = ShortTagline(looks_[i]);
            t.current = _wcsicmp(looks_[i].id.c_str(), preset.c_str()) == 0;
            if (t.current) {
                // The ring and the badge say "current"; the line says what was changed on it.
                if (changed) t.detail = std::to_wstring(changed) + L" changed \x00B7 " + t.detail;
                selected = static_cast<int>(i);
            }
            tiles.push_back(std::move(t));
        }
        galleries_[k].SetTiles(std::move(tiles), selected);
        const int alpha = std::clamp(Theme::Derived(Theme::Quick::Transparency).get(look), 0, 100);
        // Not while the thumb is dragged (the value read back may differ by one).
        if (alpha_[k] && GetCapture() != alpha_[k]) SendMessageW(alpha_[k], TBM_SETPOS, TRUE, alpha);
        const int corners = Theme::Derived(Theme::Quick::Corners).get(look);  // -1 (Mixed): no selection
        SetComboSel(corners_[k], corners);
        QsUi::Choice::SetSelected(cornersChoice_[k], corners);
        if (k == LookShown()) SetText(alphaValue_, std::to_wstring(alpha) + L" %");
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
    if (id >= kSetupCornersChoice && id < kSetupCornersChoice + kMaxDocks) {
        // 13H: the segmented control sets the hidden combo and runs its handler.
        if (code != QsUi::Choice::kChosen) return;
        const int k = id - kSetupCornersChoice;
        const int sel = QsUi::Choice::Selected(cornersChoice_[k]);
        if (sel < 0) return;
        SetComboSel(corners_[k], sel);
        OnLookCommand(kSetupCorners + k, CBN_SELCHANGE);
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
    SetText(alphaValue_, std::to_wstring(pos) + L" %");
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
    // The hints say what the switch's position means (they change with it).
    SetText(startHint_, kStartHints[start_ ? 1 : 0]);
    SetText(taskbarHint_, kTaskbarHints[hideTaskbar_ ? 1 : 0]);
    if (startBox_) InvalidateRect(startBox_, nullptr, FALSE);
    if (taskbarBox_) InvalidateRect(taskbarBox_, nullptr, FALSE);
    const DockConfig* first = rows_.empty() ? nullptr : DockOf(rows_.front().id);
    firstFloating_ = first && first->Floating();
    if (first) {
        const int ah = static_cast<int>(first->autoHide);
        SetComboSel(autoHide_, ah);
        QsUi::Choice::SetSelected(autoHideTiles_, ah);
        SetText(autoHideLabel_, rows_.size() > 1 ? L"AUTO-HIDE OF " + Upper(first->DisplayName()) : std::wstring(L"AUTO-HIDE"));
        // A floating bar never hides (its layer decides): the reason beside the label.
        EnableWindow(autoHide_, !firstFloating_);
        EnableWindow(autoHideTiles_, !firstFloating_);
    }
    const AnimationConfig& a = host_.config->animation;
    const int anim = a.speed == 0.0 ? 2 : a.reduceMotion == ReduceMotion::On ? 1 : 0;
    SetComboSel(animation_, anim);
    QsUi::Choice::SetSelected(animationChoice_, anim);
    SetText(animationNote_, kAnimationNotes[anim]);
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
    const bool room = rows_.size() < static_cast<size_t>(kMaxDocks);
    EnableWindow(addDock_, room);
    SetText(addDock_, room ? L"Add a dock" : L"Three docks is the most here. Add more later in App settings");
    SetText(addNote_, room ? L"Up to three docks here. Add more later in App settings."
                           : L"Three docks is the most here. Add more later in App settings.");
    if (rowsBox_) InvalidateRect(rowsBox_, nullptr, TRUE);
    SyncLooks();
    SyncBehaviour();
    if (steps_) InvalidateRect(steps_, nullptr, FALSE);  // the rail's captions
    if (step_ == 3) InvalidateRect(hwnd_, nullptr, TRUE);  // the picture and the summary
    for (int k = 0; k < kSummaryMax; ++k) editStep_[k] = 2;
    {
        const std::vector<SummaryLine> lines = SummaryLines();
        for (size_t k = 0; k < lines.size() && k < static_cast<size_t>(kSummaryMax); ++k) {
            editStep_[k] = lines[k].step;
            if (edits_[k]) SetText(edits_[k], L"Edit");
        }
    }
    LayoutSummary();  // a longer value can take a second line
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
        case WM_ERASEBKGND:
            return 1;  // PaintRows fills everything
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            self->PaintRows(hwnd, dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:
            self->PaintRows(hwnd, reinterpret_cast<HDC>(wp));
            return 0;
        case WM_SIZE:
            self->LayoutRows();  // the scroll bar came or went: the cards follow the client width
            return 0;
        case WM_VSCROLL: {
            SCROLLINFO si{sizeof(si), SIF_ALL};
            GetScrollInfo(hwnd, SB_VERT, &si);
            int pos = self->rowsScroll_;
            switch (LOWORD(wp)) {
                case SB_LINEUP: pos -= self->Px(32); break;
                case SB_LINEDOWN: pos += self->Px(32); break;
                case SB_PAGEUP: pos -= static_cast<int>(si.nPage); break;
                case SB_PAGEDOWN: pos += static_cast<int>(si.nPage); break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: pos = si.nTrackPos; break;
                case SB_TOP: pos = 0; break;
                case SB_BOTTOM: pos = si.nMax; break;
                default: break;
            }
            self->ScrollRowsTo(pos);
            return 0;
        }
        case WM_MOUSEWHEEL:
            if (self->rowsContentDip_ > self->rowsVisibleDip_) {
                self->ScrollRowsTo(self->rowsScroll_ - MulDiv(GET_WHEEL_DELTA_WPARAM(wp), self->Px(48), WHEEL_DELTA));
                return 0;
            }
            break;
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
        if (self->rowsBox_) SetWindowLongPtrW(self->rowsBox_, GWLP_USERDATA, 0);
        self->hwnd_ = nullptr;
        self->steps_ = self->heading_ = self->intro_ = self->back_ = self->next_ = self->finish_ = self->cancel_ = self->showAround_ = nullptr;
        self->rowsBox_ = self->addDock_ = self->addNote_ = self->layoutNote_ = nullptr;
        self->startHint_ = self->taskbarHint_ = self->autoHideHint_ = nullptr;
        for (Row& row : self->rows_) row.name = row.layout = row.display = row.edge = row.remove = row.edgeChoice = nullptr;
        self->same_ = self->sameCaption_ = self->lookDock_ = self->alphaLabel_ = self->alphaValue_ = self->cornersLabel_ = nullptr;
        for (int k = 0; k < kMaxDocks; ++k) self->alpha_[k] = self->corners_[k] = self->cornersChoice_[k] = nullptr;
        self->startBox_ = self->startCaption_ = self->taskbarBox_ = self->taskbarCaption_ = self->behaviourNote_ = nullptr;
        self->autoHideLabel_ = self->autoHide_ = self->animationLabel_ = self->animation_ = self->animationNote_ = self->doneNote_ = nullptr;
        self->autoHideTiles_ = self->animationChoice_ = nullptr;
        for (HWND& e : self->edits_) e = nullptr;
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
            ShowStep();
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
            const bool muted = c == intro_ || c == addNote_ || c == layoutNote_ || c == animationNote_ || c == startHint_ || c == taskbarHint_ ||
                               c == autoHideHint_ || c == alphaLabel_ || c == cornersLabel_ || c == autoHideLabel_ || c == doneNote_;
            const bool faint = c == behaviourNote_;
            SetTextColor(dc, faint ? colors_.faint : muted ? colors_.muted : IsWindowEnabled(c) ? colors_.text : colors_.disabled);
            const bool card = OnCard(c);  // a static (or a trackbar) on a card
            SetBkColor(dc, card ? colors_.card : colors_.base);
            return reinterpret_cast<LRESULT>(card ? cardBrush_ : baseBrush_);
        }
        case WM_CTLCOLORBTN:
            return reinterpret_cast<LRESULT>(BackOf(reinterpret_cast<HWND>(lp)) == colors_.card ? cardBrush_ : baseBrush_);
        case WM_NOTIFY: {
            auto* nm = reinterpret_cast<NMHDR*>(lp);
            if (nm && nm->code == NM_CUSTOMDRAW) return OnCustomDraw(*reinterpret_cast<NMCUSTOMDRAW*>(lp));
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;  // PaintContent fills everything (the children are clipped out)
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd_, &ps);
            PaintContent(dc);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:
            PaintContent(reinterpret_cast<HDC>(wp));
            return 0;
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
            if (id >= kSetupEdit && id < kSetupEdit + kSummaryMax) {
                // 13H: a summary line's Edit goes to the step that owns it.
                if (click) GoTo(editStep_[id - kSetupEdit]);
                return 0;
            }
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
                case kSetupAutoHideTiles:
                case kSetupAnimationChoice: {
                    // 13H: the tiles / segments set the hidden combo and run its handler.
                    if (code != QsUi::Choice::kChosen) return 0;
                    const bool ah = id == kSetupAutoHideTiles;
                    HWND combo = ah ? autoHide_ : animation_;
                    const int sel = QsUi::Choice::Selected(control);
                    if (sel < 0 || !combo) return 0;
                    SetComboSel(combo, sel);
                    SendMessageW(hwnd_, WM_COMMAND, MAKEWPARAM(ah ? kSetupAutoHide : kSetupAnimation, CBN_SELCHANGE), reinterpret_cast<LPARAM>(combo));
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
                        (id >= kSetupCornersChoice && id < kSetupCornersChoice + kMaxDocks) || id == kSetupSameLook ||
                        id == kSetupSameLookCaption || id == kSetupLookDock) {
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
