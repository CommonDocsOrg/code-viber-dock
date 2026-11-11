// Round 9 phase 9C: the preset chooser (see PresetDialog.h).

#include "PresetDialog.h"

#include <commctrl.h>
#include <shellscalingapi.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <winrt/base.h>

#include <algorithm>
#include <cwchar>

#include "InputDialog.h"
#include "Presets.h"
#include "SettingsIds.h"
#include "Theme.h"
#include "resource.h"

namespace {

using namespace SettingsIds;

// Client size and layout, DIPs. Round 12 phase 12C: 504 high (was 480): the second check box row.
constexpr int kWidthDip = 720, kHeightDip = 504;
constexpr int kMargin = 16;
constexpr int kListW = 232;
constexpr int kRowDip = 48;           // a list row: name + tagline
constexpr int kRightX = kMargin + kListW + kMargin;
constexpr int kRightW = kWidthDip - kRightX - kMargin;
constexpr int kPreviewH = 150;
constexpr int kSilhouetteW = 156;     // the preview's left part
// Round 12 phase 12C: the kind row over the list and the Mine buttons under it.
constexpr int kKindsTop = 14, kKindsH = 30;
constexpr int kMineButtonsH = 28;

COLORREF Mix(COLORREF a, COLORREF b, int percentA) {
    return RGB((GetRValue(a) * percentA + GetRValue(b) * (100 - percentA)) / 100,
               (GetGValue(a) * percentA + GetGValue(b) * (100 - percentA)) / 100,
               (GetBValue(a) * percentA + GetBValue(b) * (100 - percentA)) / 100);
}

void Fill(HDC dc, const RECT& r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
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

// An ARGB colour over `under` (its alpha mixed in).
COLORREF OverArgb(uint32_t argb, COLORREF under) {
    const int a = static_cast<int>((argb >> 24) & 0xFF);
    const COLORREF c = RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
    return Mix(c, under, a * 100 / 255);
}

const wchar_t* KindName(PresetKind k) {
    return k == PresetKind::Looks ? L"Looks" : k == PresetKind::Mine ? L"Mine" : L"Layouts";
}

// Round 18 phase 18A: WhereText moved to Presets (the Add a dock gallery shows it too).
using Presets::WhereText;

}  // namespace

PresetDialog::PresetDialog(PresetDialogHost host, PresetMode mode, HWND owner, std::wstring dockId, std::wstring dockName,
                           PresetKind kind)
    : host_(std::move(host)), mode_(mode), owner_(owner), dockId_(std::move(dockId)), dockName_(std::move(dockName)), kind_(kind) {
    if (dockName_.empty()) dockName_ = dockId_;
    // Round 12 phase 12C: a kind this variant does not offer falls back to its first.
    const std::vector<PresetKind> kinds = Kinds();
    if (std::find(kinds.begin(), kinds.end(), kind_) == kinds.end()) kind_ = kinds.front();
}

PresetDialog::~PresetDialog() {
    if (hwnd_) DestroyWindow(hwnd_);
    for (HFONT f : {font_, fontBold_, fontName_, fontHeading_}) {
        if (f) DeleteObject(f);
    }
    if (layerBrush_) DeleteObject(layerBrush_);
    if (controlBrush_) DeleteObject(controlBrush_);
}

std::vector<PresetKind> PresetDialog::Kinds() const {
    switch (mode_) {
        case PresetMode::Welcome: return {PresetKind::Layouts};  // 12G reworks the first run
        case PresetMode::NewDock: return {PresetKind::Layouts, PresetKind::Mine};  // a look is not a dock
        case PresetMode::AppLook: return {PresetKind::Looks};
        case PresetMode::Apply:
        default: return {PresetKind::Layouts, PresetKind::Looks, PresetKind::Mine};
    }
}

int PresetDialog::Selected() const {
    if (!list_) return -1;
    const LRESULT sel = SendMessageW(list_, LB_GETCURSEL, 0, 0);
    return sel == LB_ERR ? -1 : static_cast<int>(sel);
}

const Presets::Preset* PresetDialog::CurrentLayout() const {
    const int i = Selected();
    if (i < 0) return nullptr;
    const size_t k = static_cast<size_t>(i);
    if (kind_ == PresetKind::Layouts) return k < Presets::All().size() ? &Presets::All()[k] : nullptr;
    if (kind_ == PresetKind::Mine) return k < mine_.layouts.size() ? &mine_.layouts[k] : nullptr;
    return nullptr;
}

const Presets::Look* PresetDialog::CurrentLook() const {
    const int i = Selected();
    if (kind_ != PresetKind::Looks || i < 0 || static_cast<size_t>(i) >= looks_.size()) return nullptr;
    return &looks_[static_cast<size_t>(i)];
}

bool PresetDialog::KeepPins() const {
    const Presets::Preset* p = CurrentLayout();
    return p && p->hasPins && check_ && SendMessageW(check_, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

bool PresetDialog::KeepLook() const {
    // The style bit, not IsWindowVisible: the dialog itself may not be shown yet.
    return CurrentLayout() && keepLook_ && (GetWindowLongW(keepLook_, GWL_STYLE) & WS_VISIBLE) &&
           SendMessageW(keepLook_, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

bool PresetDialog::Create(HINSTANCE instance) {
    instance_ = instance;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProcThunk;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    WNDCLASSEXW pc{sizeof(pc)};
    pc.lpfnWndProc = PreviewProc;
    pc.hInstance = instance;
    pc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    pc.lpszClassName = kPreviewClass;
    if (!RegisterClassExW(&pc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    WNDCLASSEXW kc{sizeof(kc)};  // round 12 phase 12C: the kind row
    kc.lpfnWndProc = KindsProc;
    kc.hInstance = instance;
    kc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    kc.lpszClassName = kKindsClass;
    if (!RegisterClassExW(&kc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    // The owner's display (the app window is hidden: the primary one).
    const bool visibleOwner = owner_ && IsWindowVisible(owner_);
    HMONITOR mon = visibleOwner ? MonitorFromWindow(owner_, MONITOR_DEFAULTTONEAREST) : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    UINT dx = USER_DEFAULT_SCREEN_DPI, dy = USER_DEFAULT_SCREEN_DPI;
    if (FAILED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy)) || dx == 0) dx = USER_DEFAULT_SCREEN_DPI;
    dpi_ = dx;
    constexpr DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    const DWORD exStyle = WS_EX_CONTROLPARENT | WS_EX_DLGMODALFRAME | (mode_ == PresetMode::Welcome ? WS_EX_APPWINDOW : 0);
    auto rectFor = [&](UINT dpi) {
        RECT r{0, 0, MulDiv(kWidthDip, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI),
               MulDiv(kHeightDip, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI)};
        AdjustWindowRectExForDpi(&r, style, FALSE, exStyle, dpi);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(mon, &mi);
        RECT o = mi.rcWork;
        if (visibleOwner) GetWindowRect(owner_, &o);  // centred over its owner, inside the work area
        const int w = r.right - r.left, h = r.bottom - r.top;
        int x = (o.left + o.right - w) / 2, y = (o.top + o.bottom - h) / 2;
        x = std::max(static_cast<int>(mi.rcWork.left), std::min(x, static_cast<int>(mi.rcWork.right) - w));
        y = std::max(static_cast<int>(mi.rcWork.top), std::min(y, static_cast<int>(mi.rcWork.bottom) - h));
        return RECT{x, y, x + w, y + h};
    };
    // Round 12 phase 12C: the dock by its display name; layouts and looks.
    std::wstring title = mode_ == PresetMode::Welcome   ? std::wstring(L"Welcome to WinDock")
                         : mode_ == PresetMode::NewDock ? std::wstring(L"New dock from a layout")
                         : mode_ == PresetMode::AppLook ? std::wstring(L"Apply a look to the app look")
                                                        : L"Apply a layout or look to " + dockName_;
    RECT r = rectFor(dpi_);
    CreateWindowExW(exStyle, kClassName, title.c_str(), style, r.left, r.top, r.right - r.left, r.bottom - r.top, owner_, nullptr,
                    instance, this);
    if (!hwnd_) return false;
    if (const UINT actual = GetDpiForWindow(hwnd_); actual && actual != dpi_) {
        dpi_ = actual;
        r = rectFor(dpi_);
        SetWindowPos(hwnd_, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD s, int id) {
        return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | s, 0, 0, 0, 0, hwnd_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    };
    CreateFonts();  // WM_MEASUREITEM of the list needs the DPI and the fonts
    if (mode_ == PresetMode::Welcome) {
        heading_ = make(WC_STATICW, L"Welcome to WinDock", SS_LEFT | SS_NOPREFIX, kPresetHeading);
        intro_ = make(WC_STATICW,
                      L"Pick a dock to start with. You can change everything later in its settings, or apply another "
                      L"layout or look from the \x22EF menu there.",
                      SS_LEFT | SS_NOPREFIX, kPresetIntro);
    }
    // The tab order is the creation order of the tab stops: (the kind row,) the list, (the Mine
    // buttons,) the check boxes, the buttons.
    if (Kinds().size() > 1) {
        kinds_ = CreateWindowExW(0, kKindsClass, KindName(kind_), WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, hwnd_,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPresetKind)), instance, nullptr);
        if (kinds_) SetWindowLongPtrW(kinds_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    }
    list_ = make(WC_LISTBOXW, L"Presets",
                 WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT, kPresetList);
    empty_ = make(WC_STATICW, L"Nothing saved yet. Use Save dock as layout\x2026 in the dock menu.", SS_CENTER | SS_NOPREFIX, kMineEmpty);
    mineRename_ = make(WC_BUTTONW, L"Rename\x2026", WS_TABSTOP | BS_PUSHBUTTON, kMineRename);
    mineDelete_ = make(WC_BUTTONW, L"Delete", WS_TABSTOP | BS_PUSHBUTTON, kMineDelete);
    mineExport_ = make(WC_BUTTONW, L"Export\x2026", WS_TABSTOP | BS_PUSHBUTTON, kMineExport);
    name_ = make(WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, kPresetName);
    description_ = make(WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_EDITCONTROL, kPresetDescription);
    preview_ = CreateWindowExW(0, kPreviewClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPresetPreview)), instance, nullptr);
    if (preview_) SetWindowLongPtrW(preview_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    lookCaption_ = make(WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, kPresetLookCaption);
    const wchar_t* keepText = mode_ == PresetMode::NewDock ? L"Copy my pinned apps from the first dock" : L"Keep my pinned apps and folders";
    check_ = make(WC_BUTTONW, keepText, WS_TABSTOP | BS_AUTOCHECKBOX, kPresetKeep);
    checkCaption_ = make(WC_STATICW, keepText, SS_LEFT | SS_NOPREFIX | SS_NOTIFY | SS_CENTERIMAGE, kPresetKeepCaption);
    note_ = make(WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS | SS_CENTERIMAGE, kPresetNote);
    // Round 12 phase 12C: the layout's look or the dock's own (a new dock has no look to keep).
    if (mode_ == PresetMode::Apply) {
        keepLook_ = make(WC_BUTTONW, L"Keep my current look", WS_TABSTOP | BS_AUTOCHECKBOX, kPresetKeepLook);
        keepLookCaption_ = make(WC_STATICW, L"Keep my current look", SS_LEFT | SS_NOPREFIX | SS_NOTIFY | SS_CENTERIMAGE, kPresetKeepLookCaption);
    }
    status_ = make(WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS | SS_CENTERIMAGE, kPresetStatus);
    const wchar_t* applyText = mode_ == PresetMode::Welcome ? L"Use this dock" : mode_ == PresetMode::NewDock ? L"Add dock" : L"Apply";
    apply_ = make(WC_BUTTONW, applyText, WS_TABSTOP | BS_DEFPUSHBUTTON, kPresetApply);
    cancel_ = make(WC_BUTTONW, mode_ == PresetMode::Welcome ? L"Skip (basic dock)" : L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, kPresetCancel);
    if (!list_ || !name_ || !description_ || !preview_ || !check_ || !apply_ || !cancel_) {
        DestroyWindow(hwnd_);
        return false;
    }
    // The strip: a private Config (the user's global theme and defaults; the preset's dock as docks[0]).
    if (host_.config) previewConfig_ = *host_.config;
    previewConfig_.docks.resize(1);
    strip_.Create(instance, hwnd_, kPresetLook, &previewConfig_, dpi_);
    strip_.SetShowPopup(false);  // the narrow strip: the bar gets the whole width
    strip_.SetDock([this]() -> const DockConfig* { return previewConfig_.docks.empty() ? nullptr : &previewConfig_.docks.front(); }, false);
    for (HWND c : {heading_, intro_, list_, name_, description_, lookCaption_, check_, checkCaption_, note_, apply_, cancel_, empty_,
                   mineRename_, mineDelete_, mineExport_, keepLook_, keepLookCaption_, status_, kinds_}) {
        if (c) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(c == name_ ? fontName_ : c == heading_ ? fontHeading_ : font_), FALSE);
    }
    SendMessageW(check_, BM_SETCHECK, BST_CHECKED, 0);
    if (keepLook_) SendMessageW(keepLook_, BM_SETCHECK, BST_UNCHECKED, 0);
    // Round 12 phase 12C: the looks and the saved layouts, read now.
    if (mode_ == PresetMode::Apply || mode_ == PresetMode::AppLook) looks_ = Presets::Looks();
    if (mode_ == PresetMode::Apply || mode_ == PresetMode::NewDock) mine_ = Presets::Mine();
    ApplyTheme();
    SetKind(kind_, false);  // fills the list, lays out, selects the first row
    // Modal to a settings window: it is disabled while the chooser is open (the app window is hidden:
    // nothing to disable).
    if (visibleOwner && IsWindowEnabled(owner_)) {
        EnableWindow(owner_, FALSE);
        ownerDisabled_ = true;
    }
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
    SetFocus(IsWindowVisible(list_) ? list_ : kinds_ ? kinds_ : cancel_);
    return true;
}

void PresetDialog::Activate() {
    if (!hwnd_) return;
    if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
}

void PresetDialog::CreateFonts() {
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
    for (HFONT f : {font_, fontBold_, fontName_, fontHeading_}) {
        if (f) DeleteObject(f);
    }
    font_ = CreateFontIndirectW(&lf);
    LOGFONTW b = lf;
    b.lfWeight = FW_SEMIBOLD;
    fontBold_ = CreateFontIndirectW(&b);
    LOGFONTW n = b;
    n.lfHeight = lf.lfHeight * 4 / 3;
    fontName_ = CreateFontIndirectW(&n);
    LOGFONTW h = b;
    h.lfHeight = lf.lfHeight * 5 / 3;
    fontHeading_ = CreateFontIndirectW(&h);
}

void PresetDialog::ApplyTheme() {
    palette_ = DarkMode::CurrentPalette();
    const bool dark = palette_.dark;
    DarkMode::SetDarkCaption(hwnd_, dark);
    DarkMode::AllowDarkForWindow(hwnd_, dark);
    for (HWND c : {list_, check_, apply_, cancel_, keepLook_, mineRename_, mineDelete_, mineExport_}) {
        if (!c) continue;
        DarkMode::AllowDarkForWindow(c, dark);
        DarkMode::ThemeControl(c, dark);
    }
    if (layerBrush_) DeleteObject(layerBrush_);
    if (controlBrush_) DeleteObject(controlBrush_);
    layerBrush_ = CreateSolidBrush(palette_.layer);
    controlBrush_ = CreateSolidBrush(palette_.control);
    strip_.SetColors(palette_.layer, Mix(palette_.text, palette_.layer, 14));
    if (hwnd_) RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

void PresetDialog::Layout() {
    if (!hwnd_ || !list_) return;
    HDWP dwp = BeginDeferWindowPos(24);
    auto place = [&](HWND h, int x, int y, int w, int hh) {
        if (h && dwp) dwp = DeferWindowPos(dwp, h, nullptr, Px(x), Px(y), Px(w), Px(hh), SWP_NOZORDER | SWP_NOACTIVATE);
    };
    // The left column: (welcome: the heading and its line; else the kind row, round 12 phase 12C,)
    // then the list (Mine: its buttons under it).
    int listTop = kMargin;
    if (mode_ == PresetMode::Welcome) {
        place(heading_, kMargin, 14, kListW, 32);
        place(intro_, kMargin, 50, kListW, 64);
        listTop = 118;
    } else if (kinds_) {
        place(kinds_, kMargin, kKindsTop, kListW, kKindsH);
        listTop = kKindsTop + kKindsH + 8;
    }
    const bool mine = kind_ == PresetKind::Mine;
    const int listBottom = kHeightDip - 14 - (mine ? kMineButtonsH + 8 : 0);
    place(list_, kMargin, listTop, kListW, listBottom - listTop);
    place(empty_, kMargin + 8, listTop + 24, kListW - 16, 80);
    const int mineY = kHeightDip - 14 - kMineButtonsH;
    place(mineRename_, kMargin, mineY, 76, kMineButtonsH);
    place(mineDelete_, kMargin + 80, mineY, 70, kMineButtonsH);
    place(mineExport_, kMargin + 154, mineY, kListW - 154, kMineButtonsH);
    // The right column.
    const int buttonsTop = kHeightDip - 14 - 30;
    place(name_, kRightX, 14, kRightW, 30);
    place(description_, kRightX, 48, kRightW, 68);
    place(preview_, kRightX, 122, kRightW, kPreviewH);
    place(lookCaption_, kRightX, 280, kRightW, 20);
    place(strip_.Hwnd(), kRightX, 300, kRightW, 92);
    place(check_, kRightX, 400, 15, 24);  // the box only: its own text is the caption beside it
    place(checkCaption_, kRightX + 20, 400, kRightW - 20, 24);
    place(note_, kRightX, 400, kRightW, 24);
    place(keepLook_, kRightX, 426, 15, 24);  // round 12 phase 12C
    place(keepLookCaption_, kRightX + 20, 426, kRightW - 20, 24);
    const int applyW = mode_ == PresetMode::Welcome ? 124 : 100, cancelW = mode_ == PresetMode::Welcome ? 140 : 100;
    place(cancel_, kWidthDip - kMargin - cancelW, buttonsTop, cancelW, 30);
    place(apply_, kWidthDip - kMargin - cancelW - 8 - applyW, buttonsTop, applyW, 30);
    place(status_, kRightX, buttonsTop, kRightW - cancelW - applyW - 16 - 8, 30);
    if (dwp) EndDeferWindowPos(dwp);
}

// --- round 12 phase 12C: the kinds -----------------------------------------------------------------

void PresetDialog::SetKind(PresetKind kind, bool focusList) {
    const std::vector<PresetKind> kinds = Kinds();
    if (std::find(kinds.begin(), kinds.end(), kind) == kinds.end()) return;
    kind_ = kind;
    if (kinds_) {
        SetWindowTextW(kinds_, KindName(kind_));  // accessibility / automation: the kind shown
        InvalidateRect(kinds_, nullptr, FALSE);
    }
    const bool mine = kind_ == PresetKind::Mine;
    for (HWND b : {mineRename_, mineDelete_, mineExport_}) {
        if (b) ShowWindow(b, mine ? SW_SHOWNA : SW_HIDE);
    }
    std::wstring status;
    if (mine && !mine_.skipped.empty()) {
        status = std::to_wstring(mine_.skipped.size()) + (mine_.skipped.size() == 1 ? L" file skipped: " : L" files skipped: ") + mine_.skipped.front();
    }
    if (status_) {
        SetWindowTextW(status_, status.c_str());
        ShowWindow(status_, status.empty() ? SW_HIDE : SW_SHOWNA);
    }
    FillList();
    Layout();
    UpdateSelection();
    if (focusList && IsWindowVisible(list_)) SetFocus(list_);
}

void PresetDialog::FillList() {
    if (!list_) return;
    SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
    SendMessageW(list_, LB_RESETCONTENT, 0, 0);
    size_t n = 0;
    if (kind_ == PresetKind::Layouts) {
        for (const Presets::Preset& p : Presets::All()) SendMessageW(list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(p.name.c_str()));
        n = Presets::All().size();
    } else if (kind_ == PresetKind::Mine) {
        for (const Presets::Preset& p : mine_.layouts) SendMessageW(list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(p.name.c_str()));
        n = mine_.layouts.size();
    } else {
        for (const Presets::Look& l : looks_) SendMessageW(list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(l.name.c_str()));
        n = looks_.size();
    }
    SendMessageW(list_, LB_SETITEMHEIGHT, 0, Px(kRowDip));
    if (n > 0) SendMessageW(list_, LB_SETCURSEL, 0, 0);  // the first
    SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list_, nullptr, TRUE);
    // An empty Mine: the line saying how to save one instead of the list.
    const bool empty = n == 0;
    ShowWindow(list_, empty ? SW_HIDE : SW_SHOWNA);
    if (empty_) ShowWindow(empty_, empty && kind_ == PresetKind::Mine ? SW_SHOWNA : SW_HIDE);
    for (HWND b : {mineRename_, mineDelete_, mineExport_}) {
        if (b) EnableWindow(b, !empty);
    }
    EnableWindow(apply_, !empty);
    if (empty && GetFocus() == list_ && kinds_) SetFocus(kinds_);
}

void PresetDialog::ReloadMine(const std::wstring& selectId) {
    mine_ = Presets::Mine();
    SetKind(PresetKind::Mine, false);
    for (size_t i = 0; i < mine_.layouts.size(); ++i) {
        if (_wcsicmp(mine_.layouts[i].id.c_str(), selectId.c_str()) == 0) {
            SendMessageW(list_, LB_SETCURSEL, i, 0);
            UpdateSelection();
            break;
        }
    }
}

RECT PresetDialog::KindRect(size_t slot) const {
    RECT rc{};
    if (kinds_) GetClientRect(kinds_, &rc);
    const size_t n = std::max<size_t>(1, Kinds().size());
    const LONG w = rc.right / static_cast<LONG>(n);
    const LONG left = static_cast<LONG>(slot) * w;
    return RECT{left, 0, slot + 1 == n ? rc.right : left + w, rc.bottom};
}

void PresetDialog::PaintKinds(HWND hwnd, HDC hdc) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    HDC dc = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom));
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    Fill(dc, rc, palette_.layer);
    // A segmented control: one rounded frame, the chosen segment filled with the accent.
    const COLORREF frame = palette_.highContrast ? palette_.text : Mix(palette_.text, palette_.layer, 22);
    FillRound(dc, rc, frame, Px(8));
    FillRound(dc, RECT{1, 1, rc.right - 1, rc.bottom - 1}, palette_.control, Px(7));
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, font_);
    const std::vector<PresetKind> kinds = Kinds();
    const bool focused = GetFocus() == hwnd;
    for (size_t i = 0; i < kinds.size(); ++i) {
        RECT s = KindRect(i);
        const bool on = kinds[i] == kind_;
        if (on) {
            const COLORREF fill = palette_.highContrast ? GetSysColor(COLOR_HIGHLIGHT) : palette_.accent;
            FillRound(dc, RECT{s.left + Px(3), s.top + Px(3), s.right - Px(3), s.bottom - Px(3)}, fill, Px(6));
        } else if (static_cast<int>(i) == kindHot_) {
            FillRound(dc, RECT{s.left + Px(3), s.top + Px(3), s.right - Px(3), s.bottom - Px(3)}, Mix(palette_.text, palette_.control, 8), Px(6));
        }
        const COLORREF accentText = palette_.highContrast ? GetSysColor(COLOR_HIGHLIGHTTEXT)
                                    : (GetRValue(palette_.accent) * 299 + GetGValue(palette_.accent) * 587 + GetBValue(palette_.accent) * 114) / 1000 > 150
                                        ? RGB(0, 0, 0)
                                        : RGB(0xFF, 0xFF, 0xFF);
        SetTextColor(dc, on ? accentText : palette_.text);
        SelectObject(dc, on ? fontBold_ : font_);
        DrawTextW(dc, KindName(kinds[i]), -1, &s, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (on && focused && !(SendMessageW(hwnd_, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
            RECT f{s.left + Px(5), s.top + Px(5), s.right - Px(5), s.bottom - Px(5)};
            DrawFocusRect(dc, &f);
        }
    }
    SelectObject(dc, oldFont);
    BitBlt(hdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

LRESULT CALLBACK PresetDialog::KindsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<PresetDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    const std::vector<PresetKind> kinds = self->Kinds();
    auto slotAt = [&](POINT pt) -> int {
        for (size_t i = 0; i < kinds.size(); ++i) {
            const RECT s = self->KindRect(i);
            if (PtInRect(&s, pt)) return static_cast<int>(i);
        }
        return -1;
    };
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            self->PaintKinds(hwnd, dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:
            self->PaintKinds(hwnd, reinterpret_cast<HDC>(wp));
            return 0;
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            const int slot = slotAt(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            if (slot >= 0) self->SetKind(kinds[static_cast<size_t>(slot)], false);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!self->kindTracking_) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                self->kindTracking_ = TrackMouseEvent(&tme) != FALSE;
            }
            const int slot = slotAt(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            if (slot != self->kindHot_) {
                self->kindHot_ = slot;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            self->kindTracking_ = false;
            self->kindHot_ = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_KEYDOWN: {
            if (wp != VK_LEFT && wp != VK_RIGHT && wp != VK_HOME && wp != VK_END) break;
            size_t at = 0;
            for (size_t i = 0; i < kinds.size(); ++i) {
                if (kinds[i] == self->kind_) at = i;
            }
            if (wp == VK_LEFT && at > 0) --at;
            if (wp == VK_RIGHT && at + 1 < kinds.size()) ++at;
            if (wp == VK_HOME) at = 0;
            if (wp == VK_END) at = kinds.size() - 1;
            if (kinds[at] != self->kind_) self->SetKind(kinds[at], false);
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void PresetDialog::OnMineCommand(int id) {
    const Presets::Preset* p = kind_ == PresetKind::Mine ? CurrentLayout() : nullptr;
    if (!p) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    const Presets::Preset layout = *p;  // the list is read again below
    if (id == kMineRename) {
        std::wstring name = layout.name;
        if (!InputDialog::Ask(hwnd_, L"Rename layout", L"Name:", name, 64) || !hwnd_) return;
        if (name == layout.name) return;
        if (Presets::RenameLayout(layout, name) != 0) {
            MessageBoxW(hwnd_, L"The layout could not be renamed (its file could not be written).", L"Rename layout", MB_OK | MB_ICONWARNING);
        }
        ReloadMine(layout.id);
        return;
    }
    if (id == kMineDelete) {
        const std::wstring text = L"Delete the layout \x201C" + layout.name + L"\x201D?\n\nIts file in the layouts folder is deleted. Docks that were made from it keep their settings.";
        if (MessageBoxW(hwnd_, text.c_str(), L"Delete layout", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES || !hwnd_) return;
        if (Presets::DeleteLayout(layout) != 0) {
            MessageBoxW(hwnd_, L"The layout's file could not be deleted.", L"Delete layout", MB_OK | MB_ICONWARNING);
        }
        ReloadMine(L"");
        return;
    }
    if (id == kMineExport) {
        // A copy of the file where the user wants it (it can be put back into the layouts folder,
        // or shared).
        winrt::com_ptr<IFileSaveDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())))) return;
        const COMDLG_FILTERSPEC filter[] = {{L"WinDock layout (*.json)", L"*.json"}};
        dialog->SetFileTypes(1, filter);
        dialog->SetDefaultExtension(L"json");
        dialog->SetFileName((layout.id + L".json").c_str());
        dialog->SetTitle(L"Export layout");
        if (FAILED(dialog->Show(hwnd_))) return;
        winrt::com_ptr<IShellItem> item;
        PWSTR path = nullptr;
        if (FAILED(dialog->GetResult(item.put())) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) return;
        const std::wstring dest = path;
        CoTaskMemFree(path);
        if (!CopyFileW(layout.path.c_str(), dest.c_str(), FALSE) && hwnd_) {
            MessageBoxW(hwnd_, L"The layout could not be exported (the file could not be written there).", L"Export layout", MB_OK | MB_ICONWARNING);
        }
    }
}

// --- the selection -----------------------------------------------------------------------------------

void PresetDialog::UpdateSelection() {
    // Round 12 phase 12C: a look (Looks), else a layout (Layouts / Mine).
    const Presets::Look* look = CurrentLook();
    const Presets::Preset* p = look ? nullptr : CurrentLayout();
    const bool layout = p != nullptr;
    for (HWND c : {check_, checkCaption_, note_}) ShowWindow(c, SW_HIDE);
    if (keepLook_) ShowWindow(keepLook_, layout ? SW_SHOWNA : SW_HIDE);
    if (keepLookCaption_) ShowWindow(keepLookCaption_, layout ? SW_SHOWNA : SW_HIDE);
    if (!look && !p) {
        SetWindowTextW(name_, L"");
        SetWindowTextW(description_, L"");
        SetWindowTextW(lookCaption_, L"");
        SetWindowTextW(preview_, L"");
        InvalidateRect(preview_, nullptr, FALSE);
        return;
    }
    // The dock the strip draws: the target dock (a look) or the layout's (with or without its look).
    const DockConfig* target = nullptr;
    if (host_.config) {
        for (const DockConfig& d : host_.config->docks) {
            if (d.id == dockId_) target = &d;
        }
    }
    if (look) {
        SetWindowTextW(name_, look->name.c_str());
        const std::wstring who = mode_ == PresetMode::AppLook ? std::wstring(L"the app look (every dock that uses it)") : dockName_;
        const std::wstring text = look->tagline + L"\n\nChanges only the look of " + who +
                                  L": its colours, transparency, corners and icon effects. Where it sits and what is on it stay as they are.";
        SetWindowTextW(description_, text.c_str());
        if (!previewConfig_.docks.empty() && host_.config) {
            if (mode_ == PresetMode::AppLook) {
                previewConfig_.themePreset = look->id;
                previewConfig_.themeOverrides = 0;
                previewConfig_.docks.front() = target ? *target : host_.config->docks.front();
                Config::UseAppLook(previewConfig_.docks.front());
                previewConfig_.ResolveTheme();
            } else {
                previewConfig_.docks.front() = target ? *target : DockConfig{};
                previewConfig_.docks.front().themePreset = look->id;
                previewConfig_.docks.front().themeOverrides = 0;
                previewConfig_.ResolveDockThemes();
            }
        }
        SetWindowTextW(lookCaption_, (L"Look (sample icons): " + dockName_ + L" in the " + look->name + L" look").c_str());
        SetWindowTextW(preview_, (L"Look: " + look->tagline).c_str());
        InvalidateRect(preview_, nullptr, FALSE);
        strip_.Invalidate();
        return;
    }
    SetWindowTextW(name_, p->name.c_str());
    SetWindowTextW(description_, p->description.c_str());
    // The pins: the check box, or (no marker) a line saying they are not kept.
    ShowWindow(check_, p->hasPins ? SW_SHOWNA : SW_HIDE);
    ShowWindow(checkCaption_, p->hasPins ? SW_SHOWNA : SW_HIDE);
    ShowWindow(note_, p->hasPins ? SW_HIDE : SW_SHOWNA);
    const wchar_t* note = mode_ == PresetMode::NewDock ? L"This setup has no pinned apps."
                                                        : L"This setup has no pinned apps: this dock's apps and folders are removed.";
    SetWindowTextW(note_, note);
    // The preview strip: the preset's dock (its theme preset over the user's global inline fields;
    // round 12 phase 12C: "Keep my current look" shows the target's look instead).
    const bool keepLook = KeepLook();
    if (!previewConfig_.docks.empty()) {
        if (host_.config) {
            previewConfig_.themePreset = host_.config->themePreset;
            previewConfig_.themeOverrides = host_.config->themeOverrides;
            previewConfig_.theme = host_.config->theme;
        }
        previewConfig_.docks.front() = p->dock;
        if (keepLook && target) {
            DockConfig& d = previewConfig_.docks.front();
            d.themePreset = target->themePreset;
            d.themeOverrides = target->themeOverrides;
            d.theme = target->theme;
        }
        previewConfig_.ResolveDockThemes();
    }
    std::wstring theme = L"your theme";
    const DockConfig& shown = previewConfig_.docks.empty() ? p->dock : previewConfig_.docks.front();
    if (keepLook) {
        theme = L"this dock's current look";
    } else if (!shown.themePreset.empty()) {
        theme = shown.themePreset + L" theme";
        if (const Theme::Preset* t = Theme::FindBuiltIn(shown.themePreset)) theme = std::wstring(t->name) + L" theme";
    }
    wchar_t mag[64];
    if (p->dock.magnification > 1.001) {
        swprintf_s(mag, L"icons grow to %.1f\x00D7", p->dock.magnification);
    } else {
        wcscpy_s(mag, L"no magnification");
    }
    const std::wstring lookText = L"Look (sample icons): " + theme + L" \x00B7 " + std::to_wstring(p->dock.iconSize) + L" px icons \x00B7 " + mag;
    SetWindowTextW(lookCaption_, lookText.c_str());
    // The preview's window text: what it shows (automation, accessibility).
    std::wstring items;
    for (const std::wstring& n : Presets::ItemNames(*p, KeepPins())) items += (items.empty() ? L"" : L" \x00B7 ") + n;
    const std::wstring text = WhereText(p->dock) + L"\nOn the dock: " + items;
    SetWindowTextW(preview_, text.c_str());
    InvalidateRect(preview_, nullptr, FALSE);
    strip_.Invalidate();
}

void PresetDialog::DrawListItem(const DRAWITEMSTRUCT& di) {
    // Round 12 phase 12C: the row of the shown kind (a layout's or a look's name and tagline).
    std::wstring name, tagline;
    const size_t i = static_cast<size_t>(di.itemID);
    if (kind_ == PresetKind::Looks) {
        if (i < looks_.size()) {
            name = looks_[i].name;
            tagline = looks_[i].tagline;
            // The name leads the tagline (LookTagline): shown once.
            if (const size_t dot = tagline.find(L" \x00B7 "); dot != std::wstring::npos) tagline = tagline.substr(dot + 3);
        }
    } else {
        const std::vector<Presets::Preset>& list = kind_ == PresetKind::Mine ? mine_.layouts : Presets::All();
        if (i < list.size()) {
            name = list[i].name;
            tagline = list[i].tagline;
        }
    }
    HDC dc = di.hDC;
    RECT r = di.rcItem;
    const bool selected = (di.itemState & ODS_SELECTED) != 0;
    const bool focused = GetFocus() == list_;
    COLORREF back = palette_.control;
    if (selected) back = palette_.highContrast ? GetSysColor(COLOR_HIGHLIGHT) : Mix(palette_.accent, palette_.control, focused ? 38 : 26);
    Fill(dc, r, back);
    if (name.empty() && tagline.empty()) return;
    SetBkMode(dc, TRANSPARENT);
    const COLORREF text = selected && palette_.highContrast ? GetSysColor(COLOR_HIGHLIGHTTEXT) : palette_.text;
    const COLORREF second = selected && palette_.highContrast ? GetSysColor(COLOR_HIGHLIGHTTEXT) : palette_.textSecondary;
    RECT t{r.left + Px(10), r.top + Px(6), r.right - Px(8), r.top + Px(26)};
    // A look: a swatch of its bar colour first.
    if (kind_ == PresetKind::Looks && i < looks_.size()) {
        const ThemeConfig& v = looks_[i].values;
        const RECT sw{t.left, r.top + Px(10), t.left + Px(22), r.bottom - Px(10)};
        FillRound(dc, sw, OverArgb(v.background, back), Px(6));
        t.left += Px(30);
    }
    HGDIOBJ old = SelectObject(dc, fontBold_);
    SetTextColor(dc, text);
    DrawTextW(dc, name.c_str(), -1, &t, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, font_);
    SetTextColor(dc, second);
    RECT g{t.left, r.top + Px(25), t.right, r.bottom - Px(4)};
    DrawTextW(dc, tagline.c_str(), -1, &g, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, old);
    if ((di.itemState & ODS_FOCUS) && !(di.itemState & ODS_NOFOCUSRECT)) DrawFocusRect(dc, &r);
}

void PresetDialog::PaintPreview(HWND hwnd, HDC hdc) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    HDC dc = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom));
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    Fill(dc, rc, palette_.layer);
    // A card like the settings windows' (the layer mixed 4 % toward the text, a 14 % border).
    const COLORREF card = palette_.highContrast ? palette_.layer : Mix(palette_.text, palette_.layer, 4);
    FillRound(dc, rc, palette_.highContrast ? palette_.text : Mix(palette_.text, palette_.layer, 14), Px(12));
    FillRound(dc, RECT{rc.left + 1, rc.top + 1, rc.right - 1, rc.bottom - 1}, card, Px(11));
    SetBkMode(dc, TRANSPARENT);
    RECT t{rc.left + Px(kSilhouetteW + 8), rc.top + Px(10), rc.right - Px(12), rc.bottom - Px(8)};
    auto para = [&](const std::wstring& s, HFONT font, COLORREF color, int gapAfter) {
        HGDIOBJ old = SelectObject(dc, font);
        SetTextColor(dc, color);
        RECT m = t;
        DrawTextW(dc, s.c_str(), -1, &m, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
        RECT draw{t.left, t.top, t.right, std::min(t.bottom, m.bottom)};
        DrawTextW(dc, s.c_str(), -1, &draw, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
        t.top = std::min(t.bottom, m.bottom + Px(gapAfter));
        SelectObject(dc, old);
    };
    // Round 12 phase 12C: a look: a sample bar in its colours (fill, border, dots), then its Quick
    // look values in words.
    if (const Presets::Look* look = CurrentLook()) {
        const ThemeConfig& v = look->values;
        const int corners = Theme::Derived(Theme::Quick::Corners).get(v);
        const int radius = Px(corners == 0 ? 0 : corners == 1 ? 8 : 16);
        const RECT bar{rc.left + Px(18), rc.top + (rc.bottom - rc.top) / 2 - Px(20), rc.left + Px(kSilhouetteW - 6),
                       rc.top + (rc.bottom - rc.top) / 2 + Px(20)};
        FillRound(dc, bar, OverArgb(v.barBorder, card), radius + Px(1));
        FillRound(dc, RECT{bar.left + 1, bar.top + 1, bar.right - 1, bar.bottom - 1}, OverArgb(v.background, card), radius);
        const int dot = Px(6), y = bar.bottom - Px(8);
        for (int k = 0; k < 3; ++k) {
            const int x = bar.left + (bar.right - bar.left) * (k + 1) / 4;
            FillRound(dc, RECT{x - dot / 2, y - dot / 2, x + dot / 2, y + dot / 2}, OverArgb(k == 1 ? v.dotActive : v.dotRunning, OverArgb(v.background, card)), dot);
        }
        para(L"What it changes", fontBold_, palette_.text, 2);
        std::wstring words = look->tagline;
        if (const size_t dot2 = words.find(L" \x00B7 "); dot2 != std::wstring::npos) words = words.substr(dot2 + 3);
        para(words, font_, palette_.textSecondary, 8);
        para(L"What stays", fontBold_, palette_.text, 2);
        para(L"Where the dock sits, how it behaves and what is on it.", font_, palette_.textSecondary, 0);
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldBmp);
        DeleteObject(bmp);
        DeleteDC(dc);
        return;
    }
    const Presets::Preset* p = CurrentLayout();
    if (p) {
        const DockConfig& d = p->dock;
        const COLORREF bar = palette_.accent;
        // --- the silhouette: a monitor with the dock where it sits ---
        const int sw = Px(132), sh = Px(78);
        const int sx = rc.left + (Px(kSilhouetteW) - sw) / 2 + Px(4), sy = rc.top + (rc.bottom - rc.top - sh - Px(16)) / 2;
        const RECT screen{sx, sy, sx + sw, sy + sh};
        FillRound(dc, RECT{screen.left - Px(2), screen.top - Px(2), screen.right + Px(2), screen.bottom + Px(2)}, Mix(palette_.text, card, 34),
                  Px(8));
        FillRound(dc, screen, Mix(palette_.text, card, 8), Px(6));
        const int mid = (screen.left + screen.right) / 2;
        Fill(dc, RECT{mid - Px(12), screen.bottom + Px(2), mid + Px(12), screen.bottom + Px(10)}, Mix(palette_.text, card, 34));
        FillRound(dc, RECT{mid - Px(26), screen.bottom + Px(9), mid + Px(26), screen.bottom + Px(14)}, Mix(palette_.text, card, 34), Px(3));
        const int inset = Px(5), thick = Px(7);
        const RECT in{screen.left + inset, screen.top + inset, screen.right - inset, screen.bottom - inset};
        // Round 18 phase 18A: the bar's place from Presets::SilhouetteBar (the gallery draws the same).
        FillRound(dc, Presets::SilhouetteBar(d, in, thick), bar, Px(d.Floating() ? 3 : 4));
        // --- the text: where, then what is on the dock ---
        para(L"Where", fontBold_, palette_.text, 2);
        para(WhereText(d), font_, palette_.textSecondary, 8);
        para(L"On the dock, in order", fontBold_, palette_.text, 2);
        std::wstring items;
        for (const std::wstring& n : Presets::ItemNames(*p, KeepPins())) items += (items.empty() ? L"" : L" \x00B7 ") + n;
        para(items, font_, palette_.textSecondary, 0);
    }
    BitBlt(hdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

void PresetDialog::OnApply() {
    const int sel = Selected();
    if (sel < 0 || !host_.apply || !host_.apply(kind_, static_cast<size_t>(sel), KeepPins(), KeepLook())) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    Close();
}

void PresetDialog::OnCancel() {
    // The welcome variant's Skip: the basic dock (with the pins the first run gave it).
    if (mode_ == PresetMode::Welcome && host_.apply) {
        const size_t basic = Presets::IndexOf(L"basic");
        if (basic != SIZE_MAX) host_.apply(PresetKind::Layouts, basic, true, false);
    }
    Close();
}

void PresetDialog::Close() {
    if (closing_ || !hwnd_) return;
    closing_ = true;
    // The owner first, so the activation goes back to it.
    if (ownerDisabled_ && owner_ && IsWindow(owner_)) EnableWindow(owner_, TRUE);
    ownerDisabled_ = false;
    DestroyWindow(hwnd_);
}

bool PresetDialog::PreTranslateMessage(MSG& msg) {
    if (!hwnd_ || !msg.hwnd || (msg.hwnd != hwnd_ && !IsChild(hwnd_, msg.hwnd))) return false;
    return IsDialogMessageW(hwnd_, &msg) != FALSE;
}

LRESULT CALLBACK PresetDialog::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PresetDialog* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<PresetDialog*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<PresetDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    const LRESULT r = self->WndProc(msg, wp, lp);
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        self->hwnd_ = nullptr;
        self->list_ = self->name_ = self->description_ = self->preview_ = self->lookCaption_ = nullptr;
        self->check_ = self->checkCaption_ = self->note_ = self->apply_ = self->cancel_ = self->heading_ = self->intro_ = nullptr;
        self->kinds_ = self->keepLook_ = self->keepLookCaption_ = self->mineRename_ = self->mineDelete_ = self->mineExport_ = nullptr;
        self->empty_ = self->status_ = nullptr;
        if (self->host_.destroyed) self->host_.destroyed();  // the app frees the object later (posted)
    }
    return r;
}

LRESULT CALLBACK PresetDialog::PreviewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<PresetDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            if (self) self->PaintPreview(hwnd, dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:
            if (self) self->PaintPreview(hwnd, reinterpret_cast<HDC>(wp));
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT PresetDialog::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case DM_GETDEFID:
            return MAKELRESULT(kPresetApply, DC_HASDEFID);
        case WM_NEXTDLGCTL:  // as a dialog does (automation: WM_NEXTDLGCTL(control, TRUE) focuses it)
            if (LOWORD(lp) && wp && IsChild(hwnd_, reinterpret_cast<HWND>(wp))) {
                SetFocus(reinterpret_cast<HWND>(wp));
            } else if (!LOWORD(lp)) {
                if (HWND next = GetNextDlgTabItem(hwnd_, GetFocus(), wp != 0)) SetFocus(next);
            }
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) != WA_INACTIVE && !HIWORD(wp) && list_) {
                if (HWND f = GetFocus(); !f || !IsChild(hwnd_, f)) SetFocus(IsWindowVisible(list_) ? list_ : kinds_ ? kinds_ : cancel_);
            }
            if (list_) InvalidateRect(list_, nullptr, FALSE);  // the selection's focused / unfocused colour
            return 0;
        case WM_MEASUREITEM: {
            auto* mi = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
            if (mi->CtlID == static_cast<UINT>(kPresetList)) {
                mi->itemHeight = static_cast<UINT>(Px(kRowDip));
                return TRUE;
            }
            break;
        }
        case WM_DRAWITEM: {
            const auto* di = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
            if (di->CtlID == static_cast<UINT>(kPresetList)) {
                DrawListItem(*di);
                return TRUE;
            }
            break;
        }
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp);
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            CreateFonts();
            for (HWND c : {heading_, intro_, list_, name_, description_, lookCaption_, check_, checkCaption_, note_, apply_, cancel_, empty_,
                           mineRename_, mineDelete_, mineExport_, keepLook_, keepLookCaption_, status_, kinds_}) {
                if (c) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(c == name_ ? fontName_ : c == heading_ ? fontHeading_ : font_), TRUE);
            }
            if (list_) SendMessageW(list_, LB_SETITEMHEIGHT, 0, Px(kRowDip));
            strip_.SetDpi(dpi_);
            Layout();
            InvalidateRect(hwnd_, nullptr, TRUE);
            return 0;
        }
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
        case WM_CTLCOLORLISTBOX: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, palette_.text);
            SetBkColor(dc, palette_.control);
            return reinterpret_cast<LRESULT>(controlBrush_);
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wp);
            const HWND c = reinterpret_cast<HWND>(lp);
            const bool muted = c == intro_ || c == lookCaption_ || c == note_ || c == empty_ || c == status_;
            SetTextColor(dc, muted ? palette_.textSecondary : palette_.text);
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
        case WM_COMMAND: {
            const int id = LOWORD(wp), code = HIWORD(wp);
            // Round 12 phase 12C: automation: WM_COMMAND(MAKEWPARAM(kPresetKind, kind), 0) shows that kind.
            if (id == kPresetKind && lp == 0) {
                if (code <= 2) SetKind(static_cast<PresetKind>(code), false);
                return 0;
            }
            if (id == kMineRename || id == kMineDelete || id == kMineExport) {
                if (code == BN_CLICKED || lp == 0) OnMineCommand(id);
                return 0;
            }
            if (id == kPresetKeepLook && (code == BN_CLICKED || lp == 0)) {
                UpdateSelection();  // the strip with / without the layout's look
                return 0;
            }
            if (id == kPresetKeepLookCaption && code == STN_CLICKED) {
                if (keepLook_ && IsWindowVisible(keepLook_)) {
                    const bool on = SendMessageW(keepLook_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    SendMessageW(keepLook_, BM_SETCHECK, on ? BST_UNCHECKED : BST_CHECKED, 0);
                    SetFocus(keepLook_);
                    UpdateSelection();
                }
                return 0;
            }
            if (id == kPresetList && code == LBN_SELCHANGE) {
                UpdateSelection();
                return 0;
            }
            if (id == kPresetList && code == LBN_DBLCLK) {
                OnApply();
                return 0;
            }
            if (id == kPresetKeep && (code == BN_CLICKED || lp == 0)) {
                UpdateSelection();  // the item list with / without the pins
                return 0;
            }
            if (id == kPresetKeepCaption && code == STN_CLICKED) {
                if (IsWindowVisible(check_)) {
                    const bool on = SendMessageW(check_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    SendMessageW(check_, BM_SETCHECK, on ? BST_UNCHECKED : BST_CHECKED, 0);
                    SetFocus(check_);
                    UpdateSelection();
                }
                return 0;
            }
            if (id == IDOK || (id == kPresetApply && (code == BN_CLICKED || lp == 0))) {
                OnApply();
                return 0;
            }
            if (id == IDCANCEL || (id == kPresetCancel && (code == BN_CLICKED || lp == 0))) {
                OnCancel();
                return 0;
            }
            break;
        }
        case WM_CLOSE:
            OnCancel();
            return 0;
        case WM_DESTROY:
            if (ownerDisabled_ && owner_ && IsWindow(owner_)) EnableWindow(owner_, TRUE);
            ownerDisabled_ = false;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
