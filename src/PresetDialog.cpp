// Round 9 phase 9C: the preset chooser (see PresetDialog.h).

#include "PresetDialog.h"

#include <commctrl.h>
#include <shellscalingapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cwchar>

#include "Presets.h"
#include "SettingsIds.h"
#include "Theme.h"
#include "resource.h"

namespace {

using namespace SettingsIds;

// Client size and layout, DIPs.
constexpr int kWidthDip = 720, kHeightDip = 480;
constexpr int kMargin = 16;
constexpr int kListW = 232;
constexpr int kRowDip = 48;           // a list row: name + tagline
constexpr int kRightX = kMargin + kListW + kMargin;
constexpr int kRightW = kWidthDip - kRightX - kMargin;
constexpr int kPreviewH = 150;
constexpr int kSilhouetteW = 156;     // the preview's left part

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

const Presets::Preset* PresetAt(int index) {
    const auto& all = Presets::All();
    return index >= 0 && static_cast<size_t>(index) < all.size() ? &all[static_cast<size_t>(index)] : nullptr;
}

// "Bottom edge, centred." / "Floating at the top right." / "Left edge, full length."
std::wstring WhereText(const DockConfig& d) {
    std::wstring s;
    if (d.Floating()) {
        const bool cx = d.x == DockConfig::kCentred, cy = d.y == DockConfig::kCentred;
        if (cx && cy) {
            s = L"Floating in the middle of the screen.";
        } else {
            const std::wstring v = cy ? L"middle" : d.y < 400 ? L"top" : L"bottom";
            const std::wstring h = cx ? L"" : d.x > 1000 ? L" right" : L" left";
            s = L"Floating at the " + v + h + L" of the screen.";
        }
    } else {
        s = std::wstring(d.edge == DockEdge::Top    ? L"Top"
                         : d.edge == DockEdge::Left ? L"Left"
                         : d.edge == DockEdge::Right ? L"Right"
                                                     : L"Bottom") +
            L" edge";
        const bool vertical = IsVertical(d.edge);
        if (d.fullWidth) {
            s += L", full length.";
        } else if (d.align == DockAlign::Start) {
            s += vertical ? L", at the top." : L", at the left.";
        } else if (d.align == DockAlign::End) {
            s += vertical ? L", at the bottom." : L", at the right.";
        } else {
            s += L", centred.";
        }
    }
    switch (d.layer) {
        case DockLayer::Behind: s += L" On the desktop, under your windows."; break;
        case DockLayer::Desktop: s += L" Shown while the desktop is showing."; break;
        default:
            s += !d.AutoHideOn()                           ? L" Always shown, above your windows."
                 : d.autoHide == AutoHideMode::Intelligent ? L" Hides while a window covers it."
                                                           : L" Hides when the pointer leaves it.";
            break;
    }
    if (d.locked) s += L" Locked.";
    return s;
}

}  // namespace

PresetDialog::PresetDialog(PresetDialogHost host, PresetMode mode, HWND owner, std::wstring dockId)
    : host_(std::move(host)), mode_(mode), owner_(owner), dockId_(std::move(dockId)) {}

PresetDialog::~PresetDialog() {
    if (hwnd_) DestroyWindow(hwnd_);
    for (HFONT f : {font_, fontBold_, fontName_, fontHeading_}) {
        if (f) DeleteObject(f);
    }
    if (layerBrush_) DeleteObject(layerBrush_);
    if (controlBrush_) DeleteObject(controlBrush_);
}

int PresetDialog::Selected() const {
    if (!list_) return -1;
    const LRESULT sel = SendMessageW(list_, LB_GETCURSEL, 0, 0);
    return sel == LB_ERR ? -1 : static_cast<int>(sel);
}

bool PresetDialog::KeepPins() const {
    const Presets::Preset* p = PresetAt(Selected());
    return p && p->hasPins && check_ && SendMessageW(check_, BM_GETCHECK, 0, 0) == BST_CHECKED;
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
    std::wstring title = mode_ == PresetMode::Welcome   ? std::wstring(L"Welcome to WinDock")
                         : mode_ == PresetMode::NewDock ? std::wstring(L"New dock from a preset")
                                                        : L"Apply a preset to " + dockId_;
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
                      L"preset from the \x22EF menu there.",
                      SS_LEFT | SS_NOPREFIX, kPresetIntro);
    }
    // The tab order is the creation order of the tab stops: the list, the check box, the buttons.
    list_ = make(WC_LISTBOXW, L"Presets",
                 WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT, kPresetList);
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
    for (HWND c : {heading_, intro_, list_, name_, description_, lookCaption_, check_, checkCaption_, note_, apply_, cancel_}) {
        if (c) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(c == name_ ? fontName_ : c == heading_ ? fontHeading_ : font_), FALSE);
    }
    SendMessageW(check_, BM_SETCHECK, BST_CHECKED, 0);
    for (const Presets::Preset& p : Presets::All()) SendMessageW(list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(p.name.c_str()));
    SendMessageW(list_, LB_SETITEMHEIGHT, 0, Px(kRowDip));
    SendMessageW(list_, LB_SETCURSEL, 0, 0);  // the first (basic)
    ApplyTheme();
    Layout();
    UpdateSelection();
    // Modal to a settings window: it is disabled while the chooser is open (the app window is hidden:
    // nothing to disable).
    if (visibleOwner && IsWindowEnabled(owner_)) {
        EnableWindow(owner_, FALSE);
        ownerDisabled_ = true;
    }
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
    SetFocus(list_);
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
    for (HWND c : {list_, check_, apply_, cancel_}) {
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
    HDWP dwp = BeginDeferWindowPos(14);
    auto place = [&](HWND h, int x, int y, int w, int hh) {
        if (h && dwp) dwp = DeferWindowPos(dwp, h, nullptr, Px(x), Px(y), Px(w), Px(hh), SWP_NOZORDER | SWP_NOACTIVATE);
    };
    // The left column: (welcome: the heading and its line, then) the list.
    int listTop = kMargin;
    if (mode_ == PresetMode::Welcome) {
        place(heading_, kMargin, 14, kListW, 32);
        place(intro_, kMargin, 50, kListW, 64);
        listTop = 118;
    }
    place(list_, kMargin, listTop, kListW, kHeightDip - 14 - listTop);
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
    const int applyW = mode_ == PresetMode::Welcome ? 124 : 100, cancelW = mode_ == PresetMode::Welcome ? 140 : 100;
    place(cancel_, kWidthDip - kMargin - cancelW, buttonsTop, cancelW, 30);
    place(apply_, kWidthDip - kMargin - cancelW - 8 - applyW, buttonsTop, applyW, 30);
    if (dwp) EndDeferWindowPos(dwp);
}

void PresetDialog::UpdateSelection() {
    const Presets::Preset* p = PresetAt(Selected());
    if (!p) return;
    SetWindowTextW(name_, p->name.c_str());
    SetWindowTextW(description_, p->description.c_str());
    // The pins: the check box, or (no marker) a line saying they are not kept.
    ShowWindow(check_, p->hasPins ? SW_SHOWNA : SW_HIDE);
    ShowWindow(checkCaption_, p->hasPins ? SW_SHOWNA : SW_HIDE);
    ShowWindow(note_, p->hasPins ? SW_HIDE : SW_SHOWNA);
    const wchar_t* note = mode_ == PresetMode::NewDock ? L"This setup has no pinned apps."
                                                        : L"This setup has no pinned apps: this dock's apps and folders are removed.";
    SetWindowTextW(note_, note);
    // The preview strip: the preset's dock (its theme preset over the user's global inline fields).
    if (!previewConfig_.docks.empty()) {
        previewConfig_.docks.front() = p->dock;
        previewConfig_.ResolveDockThemes();
    }
    std::wstring theme = L"your theme";
    if (!p->dock.themePreset.empty()) {
        if (const Theme::Preset* t = Theme::FindBuiltIn(p->dock.themePreset)) theme = std::wstring(t->name) + L" theme";
    }
    wchar_t mag[64];
    if (p->dock.magnification > 1.001) {
        swprintf_s(mag, L"icons grow to %.1f\x00D7", p->dock.magnification);
    } else {
        wcscpy_s(mag, L"no magnification");
    }
    const std::wstring look = L"Look (sample icons): " + theme + L" \x00B7 " + std::to_wstring(p->dock.iconSize) + L" px icons \x00B7 " + mag;
    SetWindowTextW(lookCaption_, look.c_str());
    // The preview's window text: what it shows (automation, accessibility).
    std::wstring items;
    for (const std::wstring& n : Presets::ItemNames(*p, KeepPins())) items += (items.empty() ? L"" : L" \x00B7 ") + n;
    const std::wstring text = WhereText(p->dock) + L"\nOn the dock: " + items;
    SetWindowTextW(preview_, text.c_str());
    InvalidateRect(preview_, nullptr, FALSE);
    strip_.Invalidate();
}

void PresetDialog::DrawListItem(const DRAWITEMSTRUCT& di) {
    const Presets::Preset* p = PresetAt(static_cast<int>(di.itemID));
    HDC dc = di.hDC;
    RECT r = di.rcItem;
    const bool selected = (di.itemState & ODS_SELECTED) != 0;
    const bool focused = GetFocus() == list_;
    COLORREF back = palette_.control;
    if (selected) back = palette_.highContrast ? GetSysColor(COLOR_HIGHLIGHT) : Mix(palette_.accent, palette_.control, focused ? 38 : 26);
    Fill(dc, r, back);
    if (!p) return;
    SetBkMode(dc, TRANSPARENT);
    const COLORREF text = selected && palette_.highContrast ? GetSysColor(COLOR_HIGHLIGHTTEXT) : palette_.text;
    const COLORREF second = selected && palette_.highContrast ? GetSysColor(COLOR_HIGHLIGHTTEXT) : palette_.textSecondary;
    RECT t{r.left + Px(10), r.top + Px(6), r.right - Px(8), r.top + Px(26)};
    HGDIOBJ old = SelectObject(dc, fontBold_);
    SetTextColor(dc, text);
    DrawTextW(dc, p->name.c_str(), -1, &t, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, font_);
    SetTextColor(dc, second);
    RECT g{t.left, r.top + Px(25), t.right, r.bottom - Px(4)};
    DrawTextW(dc, p->tagline.c_str(), -1, &g, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
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
    const Presets::Preset* p = PresetAt(Selected());
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
        const int iw = in.right - in.left, ih = in.bottom - in.top;
        const bool vertical = IsVertical(d.edge);
        if (d.Floating()) {
            const int bw = vertical ? thick : iw * 40 / 100, bh = vertical ? ih * 40 / 100 : thick;
            // x / y in DIPs from the work area (a nominal 1920 x 1080 one): left / centre / right.
            auto at = [](int v, int room, int size, int nominal) {
                if (v == DockConfig::kCentred) return (room - size) / 2;
                return std::clamp(static_cast<int>(static_cast<long long>(v) * room / nominal), 0, std::max(0, room - size));
            };
            const int fx = at(d.x, iw, bw, 1920), fy = at(d.y, ih, bh, 1080);
            FillRound(dc, RECT{in.left + fx, in.top + fy, in.left + fx + bw, in.top + fy + bh}, bar, Px(3));
        } else {
            const int length = vertical ? ih : iw;
            const int len = d.fullWidth ? length : length * 46 / 100;
            const int start = vertical ? in.top : in.left;
            const int pos = d.fullWidth || d.align == DockAlign::Center ? start + (length - len) / 2
                            : d.align == DockAlign::Start               ? start
                                                                        : start + length - len;
            RECT b{};
            switch (d.edge) {
                case DockEdge::Top: b = RECT{pos, in.top, pos + len, in.top + thick}; break;
                case DockEdge::Left: b = RECT{in.left, pos, in.left + thick, pos + len}; break;
                case DockEdge::Right: b = RECT{in.right - thick, pos, in.right, pos + len}; break;
                case DockEdge::Bottom:
                default: b = RECT{pos, in.bottom - thick, pos + len, in.bottom}; break;
            }
            FillRound(dc, b, bar, Px(4));
        }
        // --- the text: where, then what is on the dock ---
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
    if (sel < 0 || !host_.apply || !host_.apply(static_cast<size_t>(sel), KeepPins())) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    Close();
}

void PresetDialog::OnCancel() {
    // The welcome variant's Skip: the basic dock (with the pins the first run gave it).
    if (mode_ == PresetMode::Welcome && host_.apply) {
        const size_t basic = Presets::IndexOf(L"basic");
        if (basic != SIZE_MAX) host_.apply(basic, true);
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
                if (HWND f = GetFocus(); !f || !IsChild(hwnd_, f)) SetFocus(list_);
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
            for (HWND c : {heading_, intro_, list_, name_, description_, lookCaption_, check_, checkCaption_, note_, apply_, cancel_}) {
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
            const bool muted = c == intro_ || c == lookCaption_ || c == note_;
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
