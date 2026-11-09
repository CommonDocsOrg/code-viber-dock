// Round 5 phase B: the frame parts of the settings windows that are not pages: the header band
// (WinDock.SettingsBand; Global: the subtitle line, Dock: the dock's identity: its colour, id,
// chips, Flash / Lock and a "⋯" menu (round 8 phase 8B: one 72 DIP row; the monitor silhouette
// with the dock on it moved to the Position tab: PaintSilhouette, still here); Item: the item's
// icon, name, "on <dock>", "item k of n"), the Item window's footer
// (Changes apply at once, Reset to defaults, Console…), the title bar text and the caption colour.
// Custom painted with GDI in the palette colours (dark / light), DPI aware; buttons and links are
// real child controls so the keyboard reaches them. Round 8 phase 8C: the Item header has three
// lines (92 DIPs): the name, "on <dock> · item k of n[ · <widget id> <version>]" and the item's
// description with an end ellipsis, its whole text a tooltip over that line.

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "ContextMenu.h"
#include "SettingsForm.h"
#include "SettingsIds.h"
#include "SettingsPreview.h"
#include "WidgetPackage.h"

namespace {

using namespace SettingsIds;

COLORREF Mix(COLORREF a, COLORREF b, int pa) {
    auto ch = [&](BYTE x, BYTE y) { return static_cast<BYTE>((x * pa + y * (100 - pa)) / 100); };
    return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)), ch(GetBValue(a), GetBValue(b)));
}

int Luma(COLORREF c) { return (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000; }

std::wstring StemOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const size_t dot = name.rfind(L'.');
    if (dot != std::wstring::npos && dot > 0) {
        const std::wstring ext = name.substr(dot);
        if (_wcsicmp(ext.c_str(), L".lnk") == 0 || _wcsicmp(ext.c_str(), L".exe") == 0) name.resize(dot);
    }
    return name;
}

void FillRound(HDC dc, const RECT& r, COLORREF fill, int radius) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 1, fill);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(p);
    DeleteObject(b);
}

void Fill(HDC dc, const RECT& r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

// Round 8 phase 8B: the compact Dock header's buttons (DIPs).
constexpr int kHeaderButtonH = 28;
constexpr int kHeaderFlashW = 64;
constexpr int kHeaderLockW = 72;
constexpr int kHeaderGlobalW = 112;
constexpr int kHeaderMoreW = 36;
constexpr int kHeaderCustomizeW = 88;  // round 12 phase 12D

}  // namespace

const wchar_t* SettingsEdgeName(DockEdge e) {
    switch (e) {
        case DockEdge::Top: return L"Top";
        case DockEdge::Left: return L"Left";
        case DockEdge::Right: return L"Right";
        case DockEdge::Bottom:
        default: return L"Bottom";
    }
}

std::wstring SettingsDisplayName(int monitor) {
    if (monitor == DockConfig::kMonitorEach) return L"Every display";
    if (monitor == DockConfig::kMonitorPrimary || monitor == 0) return L"Primary";
    return L"Display " + std::to_wstring(monitor + 1);
}

// --- creation and layout ---------------------------------------------------------------------

void SettingsWindow::CreateBands(bool footer) {
    auto band = [&](int id) {
        return CreateWindowExW(WS_EX_CONTROLPARENT, kBandClassName, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 0, 0,
                               hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, this);
    };
    auto child = [&](HWND parent, int id, const wchar_t* cls, const wchar_t* text) {
        const bool link = wcscmp(cls, WC_LINK) == 0;
        HWND h = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | (link ? 0 : BS_PUSHBUTTON), 0,
                                 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        if (h && link) {
            // The link in the WM_CTLCOLORSTATIC colour (the palette's link colour).
            LITEM item{};
            item.mask = LIF_ITEMINDEX | LIF_STATE;
            item.state = item.stateMask = LIS_DEFAULTCOLORS;
            SendMessageW(h, LM_SETITEM, 0, reinterpret_cast<LPARAM>(&item));
        }
        return h;
    };
    // Round 12 phase 12A: the save state ("✓ Saved" / "Saving…" / "Not saved"), a static the band
    // colours in its WM_CTLCOLORSTATIC (StatusChanged sets its text).
    auto status = [&](HWND parent) {
        CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_RIGHT | SS_CENTERIMAGE | SS_NOPREFIX | SS_ENDELLIPSIS, 0, 0, 0, 0,
                        parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBandSaveStatus)), instance_, nullptr);
    };
    if (footer) {
        if (kind_ != SettingsKind::Item) return;
        footer_ = band(kFooter);
        if (footer_) status(footer_);
        const DockItemEntry* e = TargetItem();
        const ItemKind k = e ? e->Kind() : ItemKind::Unknown;
        if (footer_ && (k == ItemKind::Widget || k == ItemKind::Clock || k == ItemKind::Weather || k == ItemKind::Tray)) {
            // Round 10 phase 10D: was "Reset to defaults"; it asks first (ConfirmFactoryReset).
            child(footer_, kFooterReset, WC_BUTTONW, L"Reset to factory defaults\x2026");
        }
        if (footer_ && k == ItemKind::Widget) {
            // Round 10 phase 10D: beside it while the widget has a value of its own over a shared
            // one (a place; the page shows it: ShowFooterResetApp). Hidden at first.
            if (HWND h = child(footer_, kFooterResetApp, WC_BUTTONW, L"Reset to app default")) ShowWindow(h, SW_HIDE);
        }
        if (footer_ && k == ItemKind::Widget) child(footer_, kFooterConsole, WC_BUTTONW, L"Console…");
        return;
    }
    header_ = band(kHeader);
    if (!header_) return;
    // Round 12 phase 12A: the bar over the pages (hidden until there is something to say), a band of
    // its own: its text, Keep, Revert / Cancel, and for a failed save Retry and Open folder.
    bar_ = CreateWindowExW(WS_EX_CONTROLPARENT, kBandClassName, L"", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 0, 0, hwnd_,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(kKeepBar)), instance_, this);
    if (bar_) {
        CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX | SS_ENDELLIPSIS, 0, 0, 0, 0,
                        bar_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kKeepBarText)), instance_, nullptr);
        child(bar_, kKeepBarKeep, WC_BUTTONW, L"Keep");
        child(bar_, kKeepBarRevert, WC_BUTTONW, L"Revert");
        child(bar_, kBarRetry, WC_BUTTONW, L"Retry");
        child(bar_, kBarOpenFolder, WC_BUTTONW, L"Open folder");
    }
    if (kind_ != SettingsKind::Item) status(header_);
    if (kind_ == SettingsKind::Dock) {
        // Keep app-wide settings directly accessible from every dock settings window.
        if (!IsFolderWindow()) {
            child(header_, kHeaderFlash, WC_BUTTONW, L"Flash");
            child(header_, kHeaderLock, WC_BUTTONW, L"Lock");
            // Round 12 phase 12D: customize mode on the dock itself (the 12E panel keeps it).
            if (DockIndex() != SIZE_MAX && Dock().IsDock()) child(header_, kHeaderCustomize, WC_BUTTONW, L"Customize");
        }
        child(header_, kHeaderGlobal, WC_BUTTONW, L"App settings…");
        child(header_, kHeaderMore, WC_BUTTONW, L"\x22EF");
        // Round 12 phase 12C: the name edited in place (hidden until a click on the name, F2 or
        // Rename dock…; not a tab stop: the name is not a control until then).
        if (DockIndex() != SIZE_MAX && Dock().IsDock()) {
            nameEdit_ = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | ES_AUTOHSCROLL | WS_BORDER, 0, 0, 0, 0, header_,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kHeaderNameEdit)), instance_, nullptr);
            if (nameEdit_) {
                SendMessageW(nameEdit_, EM_SETLIMITTEXT, 64, 0);
                SetWindowSubclass(nameEdit_, &SettingsWindow::NameEditSubclass, 1, reinterpret_cast<DWORD_PTR>(this));
            }
        }
    } else if (kind_ == SettingsKind::Item) {
        child(header_, kHeaderDockLink, WC_LINK, L"on <a>dock</a>");
        // Round 8 phase 8C: the third line's whole text as a tooltip over that line (one tool;
        // the line is painted by PaintBand, the rect follows LayoutBands).
        headerTip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                                     CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hwnd_, nullptr, instance_, nullptr);
        if (headerTip_) {
            SendMessageW(headerTip_, TTM_SETMAXTIPWIDTH, 0, Px(360));
            SendMessageW(headerTip_, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);
            TTTOOLINFOW ti{};
            ti.cbSize = sizeof(ti);
            ti.uFlags = TTF_SUBCLASS;
            ti.hwnd = header_;
            ti.uId = 1;
            ti.rect = DescriptionRect();
            ti.lpszText = const_cast<wchar_t*>(L"");
            SendMessageW(headerTip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
        }
    }
}

RECT SettingsWindow::DescriptionRect() const {
    RECT rc{};
    if (header_) GetClientRect(header_, &rc);
    return RECT{Px(70), Px(64), std::max(Px(70), static_cast<int>(rc.right) - Px(16)), Px(84)};
}

void SettingsWindow::UpdateHeaderTip() {
    if (!headerTip_ || !header_) return;
    TTTOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.hwnd = header_;
    ti.uId = 1;
    ti.rect = DescriptionRect();
    SendMessageW(headerTip_, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&ti));
    ti.lpszText = itemDescription_.data();
    SendMessageW(headerTip_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
}

void SettingsWindow::LayoutBands() {
    auto place = [](HWND parent, int id, int x, int y, int w, int h) {
        if (HWND c = parent ? GetDlgItem(parent, id) : nullptr) {
            SetWindowPos(c, nullptr, x, y, std::max(0, w), std::max(0, h), SWP_NOZORDER | SWP_NOACTIVATE);
        }
    };
    if (header_) {
        RECT rc{};
        GetClientRect(header_, &rc);
        if (kind_ == SettingsKind::Dock) {
            // One row at the right, vertically centred: Flash, Lock, App settings, ⋯.
            const int h = Px(kHeaderButtonH), y = (rc.bottom - h) / 2;
            int x = rc.right - Px(20);
            for (const auto& b : {std::pair{kHeaderMore, kHeaderMoreW}, std::pair{kHeaderGlobal, kHeaderGlobalW}, std::pair{kHeaderLock, kHeaderLockW}, std::pair{kHeaderFlash, kHeaderFlashW}, std::pair{kHeaderCustomize, kHeaderCustomizeW}}) {
                if (!GetDlgItem(header_, b.first)) continue;
                x -= Px(b.second);
                place(header_, b.first, x, y, Px(b.second), h);
                x -= Px(8);
            }
            // Round 12 phase 12A: the save state left of the buttons (the chips end before it).
            place(header_, kBandSaveStatus, x - Px(kStatusDip), y, Px(kStatusDip), h);
            // Round 12 phase 12C: the name being edited follows the header's width.
            if (renaming_ && nameEdit_) {
                const RECT r = NameRect();
                SetWindowPos(nameEdit_, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
            }
        } else if (kind_ == SettingsKind::Global) {
            // Round 12 phase 12A: the save state at the subtitle's right end.
            place(header_, kBandSaveStatus, rc.right - Px(24) - Px(kStatusDip), 0, Px(kStatusDip), rc.bottom - std::max(1, Px(1)));
        } else if (kind_ == SettingsKind::Item) {
            // Round 8 phase 8C: the name (8 - 40), this link (42 - 62), the description (64 - 84).
            place(header_, kHeaderDockLink, Px(70), Px(42), rc.right - Px(70) - Px(16), Px(20));
            UpdateHeaderTip();
        }
        InvalidateRect(header_, nullptr, TRUE);
    }
    if (footer_) {
        RECT rc{};
        GetClientRect(footer_, &rc);
        int right = rc.right - Px(16);
        const int top = (rc.bottom - Px(30)) / 2 + Px(1);
        if (GetDlgItem(footer_, kFooterConsole)) {
            place(footer_, kFooterConsole, right - Px(110), top, Px(110), Px(30));
            right -= Px(110) + Px(8);
        }
        place(footer_, kFooterReset, right - Px(180), top, Px(180), Px(30));  // round 10 phase 10D: 150 -> 180
        right -= Px(180) + Px(8);
        place(footer_, kFooterResetApp, right - Px(150), top, Px(150), Px(30));
        // Round 12 phase 12A: the save state left of the leftmost shown button.
        int left = rc.right - Px(16) + Px(8);
        for (HWND c = GetWindow(footer_, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
            if (GetDlgCtrlID(c) == kBandSaveStatus || !(GetWindowLongW(c, GWL_STYLE) & WS_VISIBLE)) continue;
            RECT cr{};
            GetWindowRect(c, &cr);
            MapWindowPoints(nullptr, footer_, reinterpret_cast<POINT*>(&cr), 2);
            left = std::min(left, static_cast<int>(cr.left));
        }
        place(footer_, kBandSaveStatus, left - Px(12) - Px(kStatusDip), top, Px(kStatusDip), Px(30));
        InvalidateRect(footer_, nullptr, TRUE);
    }
    LayoutBar();
}

// Round 12 phase 12A: the bar: its text from the left, the buttons at the right (only the shown ones).
void SettingsWindow::LayoutBar() {
    if (!bar_) return;
    RECT rc{};
    GetClientRect(bar_, &rc);
    const int h = Px(28), y = (rc.bottom - h) / 2;
    int x = rc.right - Px(16);
    for (const auto& b : {std::pair{kBarOpenFolder, 104}, std::pair{kBarRetry, 72}, std::pair{kKeepBarRevert, 112}, std::pair{kKeepBarKeep, 72}}) {
        HWND c = GetDlgItem(bar_, b.first);
        if (!c || !(GetWindowLongW(c, GWL_STYLE) & WS_VISIBLE)) continue;
        x -= Px(b.second);
        SetWindowPos(c, nullptr, x, y, Px(b.second), h, SWP_NOZORDER | SWP_NOACTIVATE);
        x -= Px(8);
    }
    if (HWND t = GetDlgItem(bar_, kKeepBarText)) {
        SetWindowPos(t, nullptr, Px(24), 0, std::max(0, x - Px(8) - Px(24)), rc.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    InvalidateRect(bar_, nullptr, TRUE);
}

void SettingsWindow::StatusChanged() {
    if (!hwnd_ || !host_.status) return;
    const SettingsStatus s = host_.status();
    // The band's status line.
    if (HWND st = GetDlgItem(kind_ == SettingsKind::Item ? footer_ : header_, kBandSaveStatus)) {
        // A preview holds the save (Keep writes it): "Not saved yet", not "Saving…".
        const wchar_t* text = s.save == 2   ? L"\x26A0 Not saved"
                              : s.bar == 2  ? L"Not saved yet"
                              : s.save == 1 ? L"Saving\x2026"
                                            : L"\x2713 Saved";
        wchar_t cur[64]{};
        GetWindowTextW(st, cur, 64);
        if (wcscmp(cur, text) != 0) {
            SetWindowTextW(st, text);
            InvalidateRect(st, nullptr, TRUE);
        }
    }
    if (!bar_) return;
    // The bar: shown while there is something to say; its buttons by kind.
    const bool show = s.bar != 0;
    std::wstring whole;
    if (show) {
        whole = s.barText;
        if (s.bar == 3) {
            whole += L" Retry \x00B7 Open folder";
        } else {
            whole += L" Keep \x00B7 " + s.cancelText;
        }
    }
    if (show && whole != barTextShown_) {
        SetWindowTextW(bar_, whole.c_str());  // accessibility / automation: what it says, with its buttons
        if (HWND t = GetDlgItem(bar_, kKeepBarText)) SetWindowTextW(t, s.barText.c_str());
        if (HWND b = GetDlgItem(bar_, kKeepBarRevert)) SetWindowTextW(b, s.cancelText.c_str());
        for (const auto& [id, on] : {std::pair{kKeepBarKeep, s.bar == 1 || s.bar == 2}, std::pair{kKeepBarRevert, s.bar == 1 || s.bar == 2},
                                     std::pair{kBarRetry, s.bar == 3}, std::pair{kBarOpenFolder, s.bar == 3}}) {
            HWND c = GetDlgItem(bar_, id);
            if (!c) continue;
            if (!on && GetFocus() == c) SetFocus(hwnd_);
            ShowWindow(c, on ? SW_SHOWNA : SW_HIDE);
        }
        barTextShown_ = whole;
        LayoutBar();
    }
    if (show != barShown_) {
        if (!show && GetFocus() && IsChild(bar_, GetFocus())) SetFocus(DefaultFocus());
        barShown_ = show;
        if (!show) {
            barTextShown_.clear();
            SetWindowTextW(bar_, L"");
        }
        ShowWindow(bar_, show ? SW_SHOWNA : SW_HIDE);
        Layout();  // the pages move down under it / back up
    }
}

void SettingsWindow::RevertWindow() {
    if (!hwnd_ || !host_.status || !host_.timeline) return;
    const int n = host_.status().sinceOpen;
    if (n <= 0) {
        MessageBeep(MB_OK);
        return;
    }
    const std::wstring text = L"Undo the " + (n == 1 ? std::wstring(L"change") : std::to_wstring(n) + L" changes") +
                              L" made since this window opened?\n\nThe settings go back to how they were when you opened it. "
                              L"Undo (Ctrl+Z) brings the changes back.";
    if (host_.modal) host_.modal(true);
    const int answer = MessageBoxW(hwnd_, text.c_str(), L"Revert this window", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
    if (host_.modal) host_.modal(false);
    if (answer == IDYES && hwnd_ && IsWindow(hwnd_)) host_.timeline(SettingsAction::RevertWindow);
}

int SettingsWindow::HeaderTextRight() const {
    RECT rc{};
    if (header_) GetClientRect(header_, &rc);
    int x = rc.right - Px(20);
    for (const auto& b : {std::pair{kHeaderMore, kHeaderMoreW}, std::pair{kHeaderGlobal, kHeaderGlobalW}, std::pair{kHeaderLock, kHeaderLockW}, std::pair{kHeaderFlash, kHeaderFlashW}, std::pair{kHeaderCustomize, kHeaderCustomizeW}}) {
        if (header_ && GetDlgItem(header_, b.first)) x -= Px(b.second) + Px(8);
    }
    if (header_ && GetDlgItem(header_, kBandSaveStatus)) x -= Px(kStatusDip);  // round 12 phase 12A
    return x - Px(8);
}

void SettingsWindow::ShowHeaderMenu() {
    HWND button = header_ ? GetDlgItem(header_, kHeaderMore) : nullptr;
    if (!button) return;
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    // Round 12 phase 12A: the app's timeline first (greyed with nothing to undo / redo / revert).
    const SettingsStatus s = host_.status ? host_.status() : SettingsStatus{};
    auto quoted = [](const wchar_t* verb, const std::wstring& label, const wchar_t* key) {
        std::wstring t = verb;
        if (!label.empty()) {
            std::wstring l;
            for (wchar_t ch : label.size() > 60 ? label.substr(0, 59) + L"\x2026" : label) {
                if (ch == L'&') l += L'&';
                l += ch;
            }
            t += L" \x201C" + l + L"\x201D";
        }
        return t + L"\t" + key;
    };
    AppendMenuW(menu, MF_STRING | (s.undoLabel.empty() ? MF_GRAYED : 0), kHeaderUndo, quoted(L"Undo", s.undoLabel, L"Ctrl+Z").c_str());
    AppendMenuW(menu, MF_STRING | (s.redoLabel.empty() ? MF_GRAYED : 0), kHeaderRedo, quoted(L"Redo", s.redoLabel, L"Ctrl+Y").c_str());
    AppendMenuW(menu, MF_STRING | (s.sinceOpen <= 0 ? MF_GRAYED : 0), kHeaderRevertWindow, L"Revert this window\x2026");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kHeaderFind, L"Find a setting…\tCtrl+F");  // round 9 phase 9B
    // Round 9 phase 9C: a predefined setup for this dock (not for folder windows / standalone widgets).
    // Round 12 phase 12C: layouts and looks; the dock's name, a copy, a saved layout.
    if (DockIndex() != SIZE_MAX && Dock().IsDock()) {
        AppendMenuW(menu, MF_STRING, kHeaderPreset, L"Apply a layout or look\x2026");
        AppendMenuW(menu, MF_STRING, kHeaderSaveLayout, L"Save dock as layout\x2026");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kHeaderRename, L"Rename dock\tF2");
        AppendMenuW(menu, MF_STRING, kHeaderDuplicate, L"Duplicate dock");
    }
    RECT r{};
    GetWindowRect(button, &r);
    const int cmd = static_cast<int>(
        TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTALIGN | TPM_TOPALIGN, r.right, r.bottom, hwnd_, nullptr));
    DestroyMenu(menu);
    if (cmd > 0) OnBandCommand(cmd);
}

// --- identity ----------------------------------------------------------------------------------

// Round 10 phase 10A: the item's name, with an ordinal when the dock holds several items of its
// kind with the same name ("Weather 2", "Weather 2 · Oslo"); a clock's label or a weather item's
// place already tells them apart, so it is part of the name compared. The first keeps no number.
std::wstring SettingsWindow::ItemName() {
    const DockItemEntry* e = TargetItem();
    if (!e) return L"Item";
    std::wstring name = ItemNameOf(item_);
    const std::vector<DockItemEntry>& items = Dock().items;
    size_t same = 0, before = 0;
    for (size_t k = 0; k < items.size(); ++k) {
        if (items[k].Kind() != e->Kind() || ItemNameOf(k) != name) continue;
        ++same;
        if (k < item_) ++before;
    }
    if (same < 2 || before == 0) return name;
    const std::wstring n = L" " + std::to_wstring(before + 1);
    const size_t dot = name.find(L" · ");
    return dot == std::wstring::npos ? name + n : name.insert(dot, n);
}

std::wstring SettingsWindow::ItemNameOf(size_t index) {
    const std::vector<DockItemEntry>& items = Dock().items;
    if (index >= items.size()) return L"Item";
    const DockItemEntry* e = &items[index];
    const Config& c = *host_.config;
    switch (e->Kind()) {
        case ItemKind::Clock: {
            const ClockConfig clock = ClockOf(c, *e);
            return clock.label.empty() ? std::wstring(L"Clock") : L"Clock · " + clock.label;
        }
        case ItemKind::Weather: {
            const WeatherConfig w = WeatherOf(c, *e);
            const std::wstring place = !w.label.empty() ? w.label : w.location;
            return place.empty() ? std::wstring(L"Weather") : L"Weather · " + place;
        }
        case ItemKind::Widget: return Widgets::DisplayName(e->id);
        case ItemKind::Group: return e->name.empty() ? std::wstring(L"Group") : e->name;
        case ItemKind::Tray: return L"System tray";
        default: {
            // A folder (or a shortcut to one): its resolved name, else the path's.
            if (host_.pins) {
                const std::vector<size_t> idx = Dock().PinIndexes();
                const auto& pins = host_.pins();
                for (size_t k = 0; k < idx.size() && k < pins.size(); ++k) {
                    if (idx[k] == index && !pins[k].displayName.empty()) return pins[k].displayName;
                }
            }
            return e->path.empty() ? std::wstring(L"Folder") : StemOf(e->path);
        }
    }
}

std::wstring SettingsWindow::ItemDescription() {
    const DockItemEntry* e = TargetItem();
    if (!e) return L"";
    switch (e->Kind()) {
        case ItemKind::Widget: return widgetDescription_;
        case ItemKind::Clock:
        case ItemKind::Weather: return BuiltInItemDescription(e->Kind());
        case ItemKind::Tray: return L"The notification area's icons in a grid on the dock; its own grid and order.";
        case ItemKind::Group:
            return L"A group of " + std::to_wstring(e->members.size()) + (e->members.size() == 1 ? L" shortcut" : L" shortcuts") +
                   L" in one popup on the dock.";
        default: return e->path;  // a folder (or a shortcut to one)
    }
}

std::vector<std::wstring> SettingsWindow::HeaderChips() {
    std::vector<std::wstring> chips;
    const size_t index = DockIndex();
    if (index == SIZE_MAX) return chips;
    const DockConfig& d = Dock();
    if (d.IsWidgetWindow()) {
        // Round 6 phase 6B: display, layer, size, Locked.
        chips.push_back(SettingsDisplayName(d.monitor));
        chips.push_back(d.layer == DockLayer::Normal    ? L"Above windows"
                        : d.layer == DockLayer::Desktop ? L"Desktop only"
                                                        : L"Behind windows");
        chips.push_back(std::to_wstring(d.widgetWindow.w) + L" × " + std::to_wstring(d.widgetWindow.h));
        const int windows = host_.dockWindows ? host_.dockWindows(index) : 1;
        if (windows == 0) chips.push_back(L"Parked");
        if (d.locked) chips.push_back(L"Locked");
        return chips;
    }
    if (d.IsFolderWindow()) {
        chips.push_back(L"Folder window");
    } else if (d.Floating()) {
        chips.push_back(L"Floating, faces " + std::wstring(SettingsEdgeName(d.edge)));  // round 6 phase 6A
    } else {
        chips.push_back(std::wstring(SettingsEdgeName(d.edge)) + L" edge");
    }
    if (!d.IsFolderWindow() && d.layer != DockLayer::Normal) {
        chips.push_back(d.layer == DockLayer::Desktop ? L"Desktop only" : L"Behind windows");
    }
    chips.push_back(SettingsDisplayName(d.monitor));
    if (!d.IsFolderWindow()) chips.push_back(std::to_wstring(d.items.size()) + (d.items.size() == 1 ? L" item" : L" items"));
    const int windows = host_.dockWindows ? host_.dockWindows(index) : 1;
    chips.push_back(windows == 0 ? std::wstring(L"Parked")
                    : d.Each()   ? L"Shown on " + std::to_wstring(windows) + L" displays"
                                 : std::wstring(L"Shown"));
    if (d.locked && !d.IsFolderWindow()) chips.push_back(L"Locked");
    return chips;
}

std::wstring SettingsWindow::WindowTitle() {
    // Round 10 phase 10A: titles name the target first ("App settings" was "WinDock settings";
    // an Item window "<item> · <dock>", e.g. "Weather 2 · main", was "… · Item settings").
    if (kind_ == SettingsKind::Global) return L"App settings";
    const DockConfig& d = Dock();
    if (kind_ == SettingsKind::Item) return ItemName() + L" · " + DockName();
    if (d.IsFolderWindow()) return DockName() + L" · Folder window settings · " + SettingsDisplayName(d.monitor);
    if (d.IsWidgetWindow()) {
        const DockItemEntry* w = d.WidgetItem();
        return DockName() + L" · " + (w ? Widgets::DisplayName(w->id) : std::wstring(L"Widget")) + L" · Standalone widget settings · " +
               SettingsDisplayName(d.monitor);
    }
    return DockName() + L" · Dock settings · " + SettingsEdgeName(d.edge) + L" · " + SettingsDisplayName(d.monitor);
}

void SettingsWindow::UpdateItemIcon() {
    if (kind_ != SettingsKind::Item) return;
    const DockItemEntry* e = TargetItem();
    if (!e) return;
    if (iconValid_ && iconFor_ == *e && iconDpi_ == dpi_) return;
    if (itemIcon_) DestroyIcon(itemIcon_);
    itemIcon_ = nullptr;
    iconFor_ = *e;
    iconValid_ = true;
    iconDpi_ = dpi_;
    if (!e->IsPin()) return;  // a widget, clock or weather item: a letter tile
    const AppIdentity::ResolvedPin* pin = nullptr;
    if (host_.pins) {
        const std::vector<size_t> idx = Dock().PinIndexes();
        const auto& pins = host_.pins();
        for (size_t k = 0; k < idx.size() && k < pins.size(); ++k) {
            if (idx[k] == item_) pin = &pins[k];
        }
    }
    itemIcon_ = SettingsItemIcon(pin, *e, Px(32));
}

void SettingsWindow::ApplyCaptionColor() {
    if (!hwnd_ || kind_ == SettingsKind::Global) return;
    // DWMWA_BORDER_COLOR / DWMWA_CAPTION_COLOR / DWMWA_TEXT_COLOR (Windows 11 build 22000);
    // older systems refuse them (ignored). High contrast: the system's colours.
    constexpr DWORD kBorderColor = 34, kCaptionColor = 35, kTextColor = 36;
    constexpr COLORREF kDefault = 0xFFFFFFFF;  // DWMWA_COLOR_DEFAULT
    const COLORREF dock = DockColor();
    const COLORREF caption = palette_.highContrast ? kDefault : dock;
    const COLORREF text = palette_.highContrast ? kDefault : Luma(dock) > 150 ? RGB(0, 0, 0) : RGB(0xFF, 0xFF, 0xFF);
    DwmSetWindowAttribute(hwnd_, kBorderColor, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd_, kCaptionColor, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd_, kTextColor, &text, sizeof(text));
}

void SettingsWindow::UpdateIdentity() {
    if (!hwnd_) return;
    if (kind_ == SettingsKind::Item && TargetItem() == nullptr) return;  // App closes it
    const std::wstring title = WindowTitle();
    wchar_t now[512]{};
    GetWindowTextW(hwnd_, now, static_cast<int>(std::size(now)));
    if (title != now) SetWindowTextW(hwnd_, title.c_str());
    // The dock's colour may have changed (its `color`, or its index: the palette is by index).
    // Only the band's brush is made again (a trackbar caches the page's brush).
    if (const COLORREF want = BandColorFor(); want != bandColor_ || !bandBrush_) {
        if (bandBrush_) DeleteObject(bandBrush_);
        bandColor_ = want;
        bandBrush_ = CreateSolidBrush(want);
        if (tabs_) InvalidateRect(tabs_, nullptr, FALSE);
    }
    ApplyCaptionColor();
    std::wstring summary;
    if (kind_ == SettingsKind::Global) {
        summary = L"App settings · apply to every dock";
    } else if (kind_ == SettingsKind::Dock) {
        summary = DockName();
        for (const std::wstring& c : HeaderChips()) summary += L" · " + c;
        if (HWND lock = header_ ? GetDlgItem(header_, kHeaderLock) : nullptr) {
            const wchar_t* text = Dock().locked ? L"Unlock" : L"Lock";  // round 8 phase 8B: short
            wchar_t cur[64]{};
            GetWindowTextW(lock, cur, 64);
            if (wcscmp(cur, text) != 0) SetWindowTextW(lock, text);
        }
        // Round 8 phase 8B: the Position tab's silhouette follows the dock (its edge, place, shape).
        for (auto& page : pages_) {
            if (HWND s = page->Item(kPositionSilhouette)) {
                const std::wstring name = L"Where it sits: " + SilhouetteCaption();
                wchar_t cur[256]{};
                GetWindowTextW(s, cur, 256);
                if (name != cur) SetWindowTextW(s, name.c_str());
                InvalidateRect(s, nullptr, FALSE);
            }
        }
    } else {
        const size_t n = Dock().items.size();
        const std::wstring place = L"item " + std::to_wstring(item_ + 1) + L" of " + std::to_wstring(n);
        summary = ItemName() + L" · on " + DockName() + L" · " + place;
        const DockItemEntry* e = TargetItem();
        if (e && e->Kind() == ItemKind::Widget && widgetLineFor_ != e->id) {
            widgetLineFor_ = e->id;
            std::wstring error;
            const auto pkg = Widgets::Load(e->id, error);
            // Round 8 phase 8C: "system-monitor 1.0.0" on the link's line; the description is the
            // third line.
            widgetLine_ = e->id + (pkg && !pkg->manifest.version.empty() ? L" " + pkg->manifest.version
                                   : pkg                                   ? std::wstring()
                                                                           : std::wstring(L" · not installed"));
            widgetDescription_ = pkg ? pkg->manifest.description : std::wstring();
        }
        if (HWND link = header_ ? GetDlgItem(header_, kHeaderDockLink) : nullptr) {
            std::wstring markup = L"on <a>" + DockName() + L"</a> · " + place;
            if (e && e->Kind() == ItemKind::Widget && !widgetLine_.empty()) markup += L" · " + widgetLine_;
            wchar_t cur[256]{};
            GetWindowTextW(link, cur, 256);
            // SysLink's window text is its markup; new text resets its links' state (colours).
            if (markup != cur) {
                SetWindowTextW(link, markup.c_str());
                LITEM li{};
                li.mask = LIF_ITEMINDEX | LIF_STATE;
                li.state = li.stateMask = LIS_DEFAULTCOLORS;
                SendMessageW(link, LM_SETITEM, 0, reinterpret_cast<LPARAM>(&li));
            }
        }
        if (const std::wstring d = ItemDescription(); d != itemDescription_) {
            itemDescription_ = d;
            UpdateHeaderTip();
        }
        UpdateItemIcon();
    }
    if (header_) {
        wchar_t cur[512]{};
        GetWindowTextW(header_, cur, static_cast<int>(std::size(cur)));
        if (summary != cur) SetWindowTextW(header_, summary.c_str());  // accessibility / automation
        InvalidateRect(header_, nullptr, TRUE);
        RedrawWindow(header_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }
    if (footer_) InvalidateRect(footer_, nullptr, TRUE);
}

// --- painting --------------------------------------------------------------------------------

void SettingsWindow::PaintBand(HWND hwnd, HDC hdc) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    HDC dc = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom));
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    FillRect(dc, &rc, hwnd == bar_ && barBrush_ ? barBrush_ : bandBrush_);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, font_);
    const COLORREF hair = Mix(palette_.text, bandColor_, 14);
    const COLORREF dock = palette_.highContrast ? palette_.accent : DockColor();
    // The dock's colour as text: darker on a light background so it stays readable.
    const COLORREF idText = palette_.highContrast ? palette_.text : palette_.dark ? dock : Mix(dock, RGB(0, 0, 0), 62);
    const bool isFooter = hwnd == footer_;
    auto text = [&](const std::wstring& s, RECT r, HFONT font, COLORREF color, UINT flags) {
        SelectObject(dc, font);
        SetTextColor(dc, color);
        DrawTextW(dc, s.c_str(), static_cast<int>(s.size()), &r, flags | DT_NOPREFIX | DT_SINGLELINE);
    };
    if (hwnd == bar_) {
        // Round 12 phase 12A: the bar: an accent edge at the left, a hairline under it (its text and
        // buttons are children).
        Fill(dc, RECT{0, 0, Px(4), rc.bottom}, palette_.highContrast ? palette_.text : palette_.accent);
        Fill(dc, RECT{0, rc.bottom - std::max(1, Px(1)), rc.right, rc.bottom}, hair);
    } else if (isFooter) {
        Fill(dc, RECT{0, 0, rc.right, std::max(1, Px(1))}, hair);
        // Round 10 phase 10D: up to the leftmost shown button (with Reset to app default beside
        // the two others there is less than half the footer left; "…" when it does not fit).
        int end = rc.right / 2;
        for (HWND c = GetWindow(footer_, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
            if (!(GetWindowLongW(c, GWL_STYLE) & WS_VISIBLE)) continue;
            RECT cr{};
            GetWindowRect(c, &cr);
            MapWindowPoints(nullptr, footer_, reinterpret_cast<POINT*>(&cr), 2);
            end = std::min(end, static_cast<int>(cr.left) - Px(8));
        }
        text(L"Changes apply at once", RECT{Px(20), 0, std::max(static_cast<int>(Px(20)), end), rc.bottom}, font_, palette_.textSecondary,
             DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
    } else if (kind_ == SettingsKind::Global) {
        // Round 12 phase 12A: up to the save state at the right end.
        text(L"App settings · apply to every dock", RECT{Px(24), 0, rc.right - Px(32) - Px(kStatusDip), rc.bottom}, font_,
             palette_.textSecondary, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        Fill(dc, RECT{Px(24), rc.bottom - std::max(1, Px(1)), rc.right - Px(24), rc.bottom}, hair);
    } else if (kind_ == SettingsKind::Dock) {
        // Round 8 phase 8B: the compact header (72 DIPs): the strip, the id in the title font and
        // the chips on one line right of it (dropped from the right when they do not fit); the
        // buttons (children) at the right. The subtitle and the silhouette are gone (the
        // silhouette is on the Position tab: PaintSilhouette).
        Fill(dc, RECT{0, 0, Px(6), rc.bottom}, dock);
        const int textRight = HeaderTextRight();
        // Round 12 phase 12C: the dock's name (else its id); while it is renamed the edit covers it.
        const std::wstring shownName = DockName();
        SelectObject(dc, fontTitle_);
        SIZE idExt{};
        GetTextExtentPoint32W(dc, shownName.c_str(), static_cast<int>(shownName.size()), &idExt);
        const int idRight = std::max(std::min(textRight, Px(24) + static_cast<int>(idExt.cx)),
                                     renaming_ ? static_cast<int>(NameRect().right) - Px(16) : 0);
        if (!renaming_) text(shownName, RECT{Px(24), 0, idRight, rc.bottom}, fontTitle_, idText, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        int x = idRight + Px(16);
        const int ch = Px(26), cy = (rc.bottom - ch) / 2;
        SelectObject(dc, font_);
        for (const std::wstring& c : HeaderChips()) {
            SIZE ext{};
            GetTextExtentPoint32W(dc, c.c_str(), static_cast<int>(c.size()), &ext);
            const int w = ext.cx + Px(20);
            if (x + w > textRight) break;
            FillRound(dc, RECT{x, cy, x + w, cy + ch}, Mix(palette_.text, bandColor_, 10), Px(12));
            text(c, RECT{x, cy, x + w, cy + ch}, font_, palette_.text, DT_CENTER | DT_VCENTER);
            x += w + Px(6);
        }
        Fill(dc, RECT{0, rc.bottom - std::max(1, Px(1)), rc.right, rc.bottom}, hair);
    } else {
        // Item: the strip, the icon (or a letter tile), the name, the link (a child), the widget line.
        Fill(dc, RECT{0, 0, Px(6), rc.bottom}, dock);
        const RECT icon{Px(24), Px(16), Px(24) + Px(32), Px(16) + Px(32)};
        const std::wstring name = ItemName();
        if (itemIcon_) {
            DrawIconEx(dc, icon.left, icon.top, itemIcon_, Px(32), Px(32), 0, nullptr, DI_NORMAL);
        } else {
            FillRound(dc, icon, dock, Px(8));
            const std::wstring letter = name.empty() ? std::wstring(L"?") : name.substr(0, 1);
            text(letter, icon, fontSemibold_, Luma(dock) > 150 ? RGB(0, 0, 0) : RGB(0xFF, 0xFF, 0xFF), DT_CENTER | DT_VCENTER);
        }
        // Round 10 phase 10A: the header names the target as the title bar does ("Weather · main").
        text(name + L" · " + DockName(), RECT{Px(70), Px(8), rc.right - Px(16), Px(40)}, fontTitle_, palette_.text,
             DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        // Round 8 phase 8C: the third line, the description (the widget's id and version moved to
        // the link's line); the whole text is the line's tooltip (headerTip_).
        if (!itemDescription_.empty()) {
            text(itemDescription_, DescriptionRect(), font_, palette_.textSecondary, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        }
        Fill(dc, RECT{0, rc.bottom - std::max(1, Px(1)), rc.right, rc.bottom}, hair);
    }
    SelectObject(dc, oldFont);
    BitBlt(hdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

// --- the dock silhouette (round 8 phase 8B: the Position tab's; it was the header's) -------------

std::wstring SettingsWindow::SilhouetteCaption() {
    if (DockIndex() == SIZE_MAX) return L"";
    const DockConfig& d = Dock();
    const RECT mon = host_.dockMonitor ? host_.dockMonitor() : RECT{};
    std::wstring s = SettingsDisplayName(d.monitor);
    if (mon.right > mon.left && mon.bottom > mon.top) {
        s += L" · " + std::to_wstring(mon.right - mon.left) + L" × " + std::to_wstring(mon.bottom - mon.top);
    } else if (host_.dockWindows && host_.dockWindows(DockIndex()) == 0) {
        s += L" · not connected";
    }
    return s;
}

void SettingsWindow::PaintSilhouette(HDC dc, const RECT& rc, COLORREF back) {
    if (DockIndex() == SIZE_MAX) return;
    const DockConfig& d = Dock();
    const COLORREF dock = palette_.highContrast ? palette_.accent : DockColor();
    const int sx = (rc.left + rc.right) / 2 - Px(60);
    const RECT screen{sx, rc.top + Px(4), sx + Px(120), rc.top + Px(4) + Px(74)};
    FillRound(dc, RECT{screen.left - Px(2), screen.top - Px(2), screen.right + Px(2), screen.bottom + Px(2)},
              Mix(palette_.text, back, 34), Px(8));
    FillRound(dc, screen, Mix(palette_.text, back, 8), Px(6));
    const int mid = (screen.left + screen.right) / 2;
    Fill(dc, RECT{mid - Px(12), screen.bottom + Px(2), mid + Px(12), screen.bottom + Px(12)}, Mix(palette_.text, back, 34));
    FillRound(dc, RECT{mid - Px(26), screen.bottom + Px(11), mid + Px(26), screen.bottom + Px(16)}, Mix(palette_.text, back, 34),
              Px(3));
    const int inset = Px(5), thick = Px(7);
    RECT in{screen.left + inset, screen.top + inset, screen.right - inset, screen.bottom - inset};
    if (d.IsFolderWindow()) {
        const RECT w{in.left + Px(6), in.top + Px(4), in.left + Px(46), in.top + Px(34)};
        FillRound(dc, w, Mix(dock, back, 45), Px(4));
        Fill(dc, RECT{w.left, w.top, w.right, w.top + Px(6)}, dock);
    } else if (d.IsWidgetWindow()) {
        // Round 6 phase 6B: a small rounded box where the widget is on its display (its window
        // rect over its monitor), else centred.
        const int iw = in.right - in.left, ih = in.bottom - in.top;
        const RECT mon = host_.dockMonitor ? host_.dockMonitor() : RECT{};
        const RECT box = host_.dockBar ? host_.dockBar() : RECT{};
        const LONG mw = mon.right - mon.left, mh = mon.bottom - mon.top;
        int bw = Px(18), bh = Px(18);
        int fx = (iw - bw) / 2, fy = (ih - bh) / 2;
        if (mw > 0 && mh > 0 && box.right > box.left) {
            bw = std::clamp(static_cast<int>(static_cast<long long>(box.right - box.left) * iw / mw), Px(8), iw);
            bh = std::clamp(static_cast<int>(static_cast<long long>(box.bottom - box.top) * ih / mh), Px(8), ih);
            fx = std::clamp(static_cast<int>(static_cast<long long>(box.left - mon.left) * iw / mw), 0, std::max(0, iw - bw));
            fy = std::clamp(static_cast<int>(static_cast<long long>(box.top - mon.top) * ih / mh), 0, std::max(0, ih - bh));
        }
        FillRound(dc, RECT{in.left + fx, in.top + fy, in.left + fx + bw, in.top + fy + bh}, dock, Px(5));
    } else if (d.Floating()) {
        // A small bar where the dock is on its display (its rest rect over its monitor), else
        // centred.
        const bool vertical = IsVertical(d.edge);
        const int iw = in.right - in.left, ih = in.bottom - in.top;
        const int bw = vertical ? thick : iw * 34 / 100, bh = vertical ? ih * 40 / 100 : thick;
        int fx = (iw - bw) / 2, fy = (ih - bh) / 2;
        const RECT mon = host_.dockMonitor ? host_.dockMonitor() : RECT{};
        const RECT bar = host_.dockBar ? host_.dockBar() : RECT{};
        const LONG mw = mon.right - mon.left, mh = mon.bottom - mon.top;
        if (mw > 0 && mh > 0 && bar.right > bar.left) {
            fx = static_cast<int>(static_cast<long long>(bar.left - mon.left) * (iw - bw) / std::max<LONG>(1, mw - (bar.right - bar.left)));
            fy = static_cast<int>(static_cast<long long>(bar.top - mon.top) * (ih - bh) / std::max<LONG>(1, mh - (bar.bottom - bar.top)));
            fx = std::clamp(fx, 0, std::max(0, iw - bw));
            fy = std::clamp(fy, 0, std::max(0, ih - bh));
        }
        const RECT fb{in.left + fx, in.top + fy, in.left + fx + bw, in.top + fy + bh};
        FillRound(dc, fb, dock, Px(3));
    } else if (d.shape == DockShape::Circle) {
        const int r = Px(14);
        const int cx = (in.left + in.right) / 2, cyy = (in.top + in.bottom) / 2;
        HBRUSH b = CreateSolidBrush(dock);
        HPEN p = CreatePen(PS_SOLID, std::max(1, Px(2)), dock);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        HGDIOBJ op = SelectObject(dc, p);
        Ellipse(dc, cx - r, cyy - r, cx + r, cyy + r);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(p);
        DeleteObject(b);
    } else {
        const bool vertical = IsVertical(d.edge);
        const int length = vertical ? in.bottom - in.top : in.right - in.left;
        const int len = d.fullWidth ? length : length * 46 / 100;
        const int start = vertical ? in.top : in.left;
        const int pos = d.fullWidth || d.align == DockAlign::Center ? start + (length - len) / 2
                        : d.align == DockAlign::Start               ? start
                                                                    : start + length - len;
        RECT bar{};
        switch (d.edge) {
            case DockEdge::Top: bar = RECT{pos, in.top, pos + len, in.top + thick}; break;
            case DockEdge::Left: bar = RECT{in.left, pos, in.left + thick, pos + len}; break;
            case DockEdge::Right: bar = RECT{in.right - thick, pos, in.right, pos + len}; break;
            case DockEdge::Bottom:
            default: bar = RECT{pos, in.bottom - thick, pos + len, in.bottom}; break;
        }
        if (d.shape == DockShape::Arc) {
            // A half ring on the edge: a bump in the middle of the bar's place.
            const int r = Px(13);
            const int cx = vertical ? (d.edge == DockEdge::Left ? in.left : in.right) : (bar.left + bar.right) / 2;
            const int cyy = vertical ? (bar.top + bar.bottom) / 2 : (d.edge == DockEdge::Top ? in.top : in.bottom);
            HRGN clip = CreateRectRgn(in.left, in.top, in.right, in.bottom);
            SelectClipRgn(dc, clip);
            HBRUSH b = CreateSolidBrush(dock);
            HPEN p = CreatePen(PS_SOLID, 1, dock);
            HGDIOBJ ob = SelectObject(dc, b);
            HGDIOBJ op = SelectObject(dc, p);
            Ellipse(dc, cx - r, cyy - r, cx + r, cyy + r);
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(p);
            DeleteObject(b);
            SelectClipRgn(dc, nullptr);
            DeleteObject(clip);
        } else {
            FillRound(dc, bar, dock, Px(4));
        }
    }
    // The caption under the stand.
    const std::wstring caption = SilhouetteCaption();
    HGDIOBJ oldFont = SelectObject(dc, font_);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, palette_.textSecondary);
    RECT cr{rc.left, screen.bottom + Px(20), rc.right, rc.bottom};
    DrawTextW(dc, caption.c_str(), static_cast<int>(caption.size()), &cr, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, oldFont);
}

// --- messages --------------------------------------------------------------------------------

LRESULT CALLBACK SettingsWindow::BandProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SettingsWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<SettingsWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == WM_NCDESTROY) SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    return self->BandProc(hwnd, msg, wp, lp);
}

LRESULT SettingsWindow::BandProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            PaintBand(hwnd, dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:
            PaintBand(hwnd, reinterpret_cast<HDC>(wp));
            return 0;
        case WM_SIZE:
            LayoutBands();
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HDC dc = reinterpret_cast<HDC>(wp);
            const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
            // Round 12 phase 12A: the save state in the muted text colour; the bar's children on its fill.
            SetTextColor(dc, id == kHeaderDockLink ? palette_.link : id == kBandSaveStatus ? palette_.textSecondary : palette_.text);
            const bool inBar = hwnd == bar_ && barBrush_;
            SetBkColor(dc, inBar ? barColor_ : bandColor_);
            return reinterpret_cast<LRESULT>(inBar ? barBrush_ : bandBrush_);
        }
        // --- round 12 phase 12C: the header's name: a click edits it (an I-beam over it), the edit in
        // the control colours ---
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, palette_.text);
            SetBkColor(dc, palette_.control);
            return reinterpret_cast<LRESULT>(controlBrush_);
        }
        case WM_LBUTTONDOWN: {
            const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            const RECT r = NameRect();
            if (hwnd == header_ && nameEdit_ && !renaming_ && PtInRect(&r, pt)) BeginRename();
            return 0;
        }
        case WM_SETCURSOR: {
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            const RECT r = NameRect();
            if (hwnd == header_ && nameEdit_ && !renaming_ && LOWORD(lp) == HTCLIENT && PtInRect(&r, pt)) {
                SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
                return TRUE;
            }
            break;
        }
        // --- end round 12 phase 12C ---
        case WM_COMMAND:
            // Round 12 phase 12C: the name edit losing the focus (a click elsewhere, Tab) commits it.
            if (LOWORD(wp) == kHeaderNameEdit && HIWORD(wp) == EN_KILLFOCUS) {
                if (renaming_) EndRename(true);
                return 0;
            }
            if (HIWORD(wp) == BN_CLICKED) OnBandCommand(LOWORD(wp));
            return 0;
        case WM_NOTIFY: {
            const auto* nm = reinterpret_cast<const NMHDR*>(lp);
            if (nm->code == NM_CLICK || nm->code == NM_RETURN) OnBandCommand(static_cast<int>(nm->idFrom));
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void SettingsWindow::OnBandCommand(int id) {
    if (loading_) return;
    switch (id) {
        case kHeaderFlash:
            if (host_.flashDock) host_.flashDock();
            break;
        case kHeaderLock: {
            if (DockIndex() == SIZE_MAX) return;
            DockConfig& d = Dock();
            d.locked = !d.locked;  // read by the dock when a drag / drop / menu starts
            if (host_.label) host_.label((d.locked ? L"Locked " : L"Unlocked ") + DockName());  // round 12 phase 12A
            if (host_.changed) host_.changed(SettingsScope::SaveOnly, DockIndex());
            Refresh();  // the Contents tab's check box, the chip, the button
            break;
        }
        case kHeaderGlobal:
            if (host_.openGlobal) host_.openGlobal(-1);
            break;
        case kHeaderCustomize:  // round 12 phase 12D: customize mode on this window's dock (DockCommand 170)
            if (DockIndex() != SIZE_MAX && host_.command) host_.command(ContextMenu::kCustomize);
            break;
        case kHeaderMore:
            ShowHeaderMenu();
            break;
        case kHeaderFind:  // round 9 phase 9B: the "⋯" menu's Find a setting… (Ctrl+F)
            OpenSearch(L"");
            break;
        case kHeaderPreset:  // round 9 phase 9C: the "⋯" menu's Apply a preset… (the chooser, modal to this window)
            if (DockIndex() != SIZE_MAX && Dock().IsDock() && host_.openPresets) host_.openPresets(0, DockIndex());
            break;
        case kHeaderDockLink:
            if (host_.openDock && DockIndex() != SIZE_MAX) host_.openDock(DockIndex(), -1);
            break;
        // --- round 12 phase 12C: the ⋯ menu's Rename dock / Duplicate dock / Save dock as layout… ---
        case kHeaderRename:
            if (!BeginRename()) MessageBeep(MB_ICONWARNING);
            break;
        case kHeaderDuplicate:
        case kHeaderSaveLayout:
            if (DockIndex() == SIZE_MAX || !Dock().IsDock() || !host_.dockAction ||
                !host_.dockAction(id == kHeaderDuplicate ? ContextMenu::kDuplicateDock : ContextMenu::kSaveDockLayout, DockIndex())) {
                if (id == kHeaderDuplicate) MessageBeep(MB_ICONWARNING);
            }
            break;
        // --- end round 12 phase 12C ---
        case kFooterReset:
            // Round 10 phase 10D: "Reset to factory defaults…" asks first.
            if (TargetItem() && ConfirmFactoryReset(ItemName(), TargetItem()->Kind())) {
                // Round 12 phase 12A: one timeline entry, named (before the page's change: the first label wins).
                if (host_.label && TargetItem()) host_.label(L"Reset " + ItemName() + L" to factory defaults");
                if (SettingsPage* page = ActivePage()) page->OnCommand(kWidgetReset, BN_CLICKED, nullptr);
            }
            break;
        // Round 12 phase 12A: the ⋯ menu's Undo / Redo / Revert this window…, the bar's buttons.
        case kHeaderUndo:
        case kHeaderRedo:
            if (!host_.timeline || !host_.timeline(id == kHeaderUndo ? SettingsAction::Undo : SettingsAction::Redo)) MessageBeep(MB_OK);
            break;
        case kHeaderRevertWindow:
            RevertWindow();
            break;
        case kKeepBarKeep:
            if (host_.timeline) host_.timeline(SettingsAction::Keep);
            break;
        case kKeepBarRevert:
            if (host_.timeline) host_.timeline(SettingsAction::Cancel);
            break;
        case kBarRetry:
            if (host_.timeline) host_.timeline(SettingsAction::Retry);
            break;
        case kBarOpenFolder:
            if (host_.timeline) host_.timeline(SettingsAction::OpenFolder);
            break;
        case kFooterResetApp:  // round 10 phase 10D: the page that shows the place drops it
            for (const auto& page : pages_) {
                if (page->Item(kPlaceUseShared)) {
                    page->OnCommand(kFooterResetApp, BN_CLICKED, nullptr);
                    break;
                }
            }
            break;
        case kFooterConsole:
            if (host_.command) host_.command(ContextMenu::kWidgetConsole);
            break;
        default:
            break;
    }
}

bool SettingsWindow::ConfirmFactoryReset(const std::wstring& name, ItemKind kind) {
    if (!hwnd_) return false;
    const std::wstring dock = DockName().empty() ? std::wstring() : L" on " + DockName();
    std::wstring text;
    switch (kind) {
        case ItemKind::Clock:
        case ItemKind::Weather:
            // The values a new clock / weather item gets (Config::EnsureItemSettings): WinDock's,
            // unless config.json's top-level "clock" / "weather" objects were edited by hand.
            text = L"Resets all of " + name + L"'s settings" + dock + L" to the values WinDock ships with (the ones a new " +
                   (kind == ItemKind::Clock ? std::wstring(L"clock") : std::wstring(L"weather item")) +
                   L" starts with). App defaults are not changed.";
            break;
        case ItemKind::Tray:
            text = L"Resets " + name + dock + L" (rows, columns, icon size, hidden icons and their order) to the values "
                   L"WinDock ships with. The tray takeover and other app defaults are not changed.";
            break;
        default:
            text = L"Resets all of " + name + L"'s settings" + dock + L" to the values WinDock ships with (the widget's "
                   L"own defaults). App defaults are not changed.";
            break;
    }
    text += L"\n\nReset now?";
    // Owned by this window (it is disabled meanwhile); App holds what a dock's dialog holds, so
    // the item cannot be removed or the config reloaded under it (host.modal).
    if (host_.modal) host_.modal(true);
    const int answer = MessageBoxW(hwnd_, text.c_str(), L"Reset to factory defaults", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (host_.modal) host_.modal(false);
    return answer == IDYES && hwnd_ && IsWindow(hwnd_);
}

void SettingsWindow::ShowFooterResetApp(bool show) {
    HWND h = footer_ ? GetDlgItem(footer_, kFooterResetApp) : nullptr;
    if (!h || ((GetWindowLongW(h, GWL_STYLE) & WS_VISIBLE) != 0) == show) return;
    if (!show && GetFocus() == h) SetFocus(hwnd_);
    ShowWindow(h, show ? SW_SHOWNA : SW_HIDE);
    InvalidateRect(footer_, nullptr, TRUE);  // "Changes apply at once" ends before the buttons
}

// --- round 12 phase 12C: the dock's name, edited in place in the Dock header ---------------------

std::wstring SettingsWindow::DockName() const {
    const size_t index = DockIndex();
    const std::wstring name = host_.config && index != SIZE_MAX ? host_.config->DockDisplayName(index) : std::wstring();
    return name.empty() ? dockId_ : name;
}

RECT SettingsWindow::NameRect() const {
    RECT rc{};
    if (header_) GetClientRect(header_, &rc);
    // The name as painted (PaintBand: from 24 DIPs, the title font), at least 200 DIPs to type in,
    // never into the buttons.
    int width = 0;
    if (HDC dc = header_ ? GetDC(header_) : nullptr) {
        HGDIOBJ old = SelectObject(dc, fontTitle_);
        const std::wstring name = DockName();
        SIZE ext{};
        GetTextExtentPoint32W(dc, name.c_str(), static_cast<int>(name.size()), &ext);
        width = ext.cx;
        SelectObject(dc, old);
        ReleaseDC(header_, dc);
    }
    const int h = Px(34);
    const int top = (rc.bottom - h) / 2;
    const int right = std::min(HeaderTextRight(), Px(24) + std::max(width + Px(24), Px(200)));
    return RECT{Px(18), top, std::max(static_cast<int>(Px(18)) + Px(60), right), top + h};
}

bool SettingsWindow::BeginRename() {
    if (!hwnd_ || !nameEdit_ || kind_ != SettingsKind::Dock || DockIndex() == SIZE_MAX || !Dock().IsDock()) return false;
    Activate();
    if (!renaming_) {
        renaming_ = true;
        const RECT r = NameRect();
        SendMessageW(nameEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(fontTitle_), FALSE);
        SetWindowTextW(nameEdit_, DockName().c_str());
        SetWindowPos(nameEdit_, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_SHOWWINDOW);
        InvalidateRect(header_, nullptr, TRUE);
    }
    SetFocus(nameEdit_);
    SendMessageW(nameEdit_, EM_SETSEL, 0, -1);
    return true;
}

void SettingsWindow::EndRename(bool commit) {
    if (!renaming_ || !nameEdit_) return;
    renaming_ = false;  // first: the focus change below sends the edit WM_KILLFOCUS again
    wchar_t buf[128]{};
    GetWindowTextW(nameEdit_, buf, static_cast<int>(std::size(buf)));
    const bool hadFocus = GetFocus() == nameEdit_;
    ShowWindow(nameEdit_, SW_HIDE);
    if (hadFocus) SetFocus(DefaultFocus());
    if (commit && DockIndex() != SIZE_MAX) {
        // Trimmed, one line; the id itself (or nothing) means "no name of its own".
        std::wstring n;
        for (const wchar_t* p = buf; *p; ++p) {
            if (*p >= 0x20 && *p != 0x7F) n += *p;
        }
        const size_t a = n.find_first_not_of(L' ');
        n = a == std::wstring::npos ? std::wstring() : n.substr(a, n.find_last_not_of(L' ') - a + 1);
        DockConfig& d = Dock();
        if (n == d.id) n.clear();
        if (n != d.name) {
            const std::wstring before = d.DisplayName();
            d.name = n;
            if (host_.label) host_.label(L"Renamed " + before + L" to " + d.DisplayName());
            // Appearance: a title that follows the name is laid out again (titleUseName).
            if (host_.changed) host_.changed(SettingsScope::Appearance, DockIndex());
        }
    }
    UpdateIdentity();
    InvalidateRect(header_, nullptr, TRUE);
}

LRESULT CALLBACK SettingsWindow::NameEditSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    auto* self = reinterpret_cast<SettingsWindow*>(ref);
    switch (msg) {
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS | DefSubclassProc(hwnd, msg, wp, lp);  // Enter / Esc are the edit's
        case WM_KEYDOWN:
            if (wp == VK_RETURN || wp == VK_ESCAPE) {
                self->EndRename(wp == VK_RETURN);
                return 0;
            }
            break;
        case WM_CHAR:
            if (wp == L'\r' || wp == 0x1B) return 0;  // no beep
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, &SettingsWindow::NameEditSubclass, 1);
            break;
        default:
            break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}
