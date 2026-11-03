// Round 5 phase B: the frame parts of the settings windows that are not pages: the header band
// (WinDock.SettingsBand; Global: the subtitle line, Dock: the dock's identity: its colour, id,
// chips, a monitor silhouette with the dock on its edge, Flash this dock / Lock dock / Global
// settings; Item: the item's icon, name, "on <dock>", "item k of n"), the Item window's footer
// (Changes apply at once, Reset to defaults, Console…), the title bar text and the caption colour.
// Custom painted with GDI in the palette colours (dark / light), DPI aware; buttons and links are
// real child controls so the keyboard reaches them.

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <string>
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
    if (footer) {
        if (kind_ != SettingsKind::Item) return;
        footer_ = band(kFooter);
        const DockItemEntry* e = TargetItem();
        const ItemKind k = e ? e->Kind() : ItemKind::Unknown;
        if (footer_ && (k == ItemKind::Widget || k == ItemKind::Clock || k == ItemKind::Weather)) {
            child(footer_, kFooterReset, WC_BUTTONW, L"Reset to defaults");
        }
        if (footer_ && k == ItemKind::Widget) child(footer_, kFooterConsole, WC_BUTTONW, L"Console…");
        return;
    }
    header_ = band(kHeader);
    if (!header_) return;
    if (kind_ == SettingsKind::Dock) {
        if (!IsFolderWindow()) {
            child(header_, kHeaderFlash, WC_BUTTONW, L"Flash this dock");
            child(header_, kHeaderLock, WC_BUTTONW, L"Lock dock");
        }
        child(header_, kHeaderGlobal, WC_LINK, L"<a>Global settings</a>");
    } else if (kind_ == SettingsKind::Item) {
        child(header_, kHeaderDockLink, WC_LINK, L"on <a>dock</a>");
    }
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
            // Buttons in a column left of the monitor silhouette.
            const int sx = rc.right - Px(24) - Px(120);
            const int bx = sx - Px(20) - Px(150);
            const bool folder = IsFolderWindow();
            place(header_, kHeaderFlash, bx, Px(16), Px(150), Px(30));
            place(header_, kHeaderLock, bx, Px(52), Px(150), Px(30));
            place(header_, kHeaderGlobal, bx + Px(4), folder ? Px(20) : Px(92), Px(146), Px(22));
        } else if (kind_ == SettingsKind::Item) {
            place(header_, kHeaderDockLink, Px(70), Px(44), rc.right - Px(70) - Px(16), Px(20));
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
        place(footer_, kFooterReset, right - Px(150), top, Px(150), Px(30));
        InvalidateRect(footer_, nullptr, TRUE);
    }
}

// --- identity ----------------------------------------------------------------------------------

std::wstring SettingsWindow::ItemName() {
    const DockItemEntry* e = TargetItem();
    if (!e) return L"Item";
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
        default: {
            // A folder (or a shortcut to one): its resolved name, else the path's.
            if (host_.pins) {
                const std::vector<size_t> idx = Dock().PinIndexes();
                const auto& pins = host_.pins();
                for (size_t k = 0; k < idx.size() && k < pins.size(); ++k) {
                    if (idx[k] == item_ && !pins[k].displayName.empty()) return pins[k].displayName;
                }
            }
            return e->path.empty() ? std::wstring(L"Folder") : StemOf(e->path);
        }
    }
}

std::vector<std::wstring> SettingsWindow::HeaderChips() {
    std::vector<std::wstring> chips;
    const size_t index = DockIndex();
    if (index == SIZE_MAX) return chips;
    const DockConfig& d = Dock();
    if (d.IsFolderWindow()) {
        chips.push_back(L"Folder window");
    } else {
        chips.push_back(std::wstring(SettingsEdgeName(d.edge)) + L" edge");
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
    if (kind_ == SettingsKind::Global) return L"WinDock settings";
    const DockConfig& d = Dock();
    if (kind_ == SettingsKind::Item) return ItemName() + L" · " + dockId_ + L" · Item settings";
    if (d.IsFolderWindow()) return dockId_ + L" · Folder window settings · " + SettingsDisplayName(d.monitor);
    return dockId_ + L" · Dock settings · " + SettingsEdgeName(d.edge) + L" · " + SettingsDisplayName(d.monitor);
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
        summary = L"Global · applies to every dock";
    } else if (kind_ == SettingsKind::Dock) {
        summary = dockId_;
        for (const std::wstring& c : HeaderChips()) summary += L" · " + c;
        if (HWND lock = header_ ? GetDlgItem(header_, kHeaderLock) : nullptr) {
            const wchar_t* text = Dock().locked ? L"Unlock dock" : L"Lock dock";
            wchar_t cur[64]{};
            GetWindowTextW(lock, cur, 64);
            if (wcscmp(cur, text) != 0) SetWindowTextW(lock, text);
        }
    } else {
        const size_t n = Dock().items.size();
        const std::wstring place = L"item " + std::to_wstring(item_ + 1) + L" of " + std::to_wstring(n);
        summary = ItemName() + L" · on " + dockId_ + L" · " + place;
        if (HWND link = header_ ? GetDlgItem(header_, kHeaderDockLink) : nullptr) {
            const std::wstring markup = L"on <a>" + dockId_ + L"</a> · " + place;
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
        const DockItemEntry* e = TargetItem();
        if (e && e->Kind() == ItemKind::Widget && widgetLineFor_ != e->id) {
            widgetLineFor_ = e->id;
            std::wstring error;
            const auto pkg = Widgets::Load(e->id, error);
            widgetLine_ = e->id + (pkg && !pkg->manifest.version.empty() ? L" · version " + pkg->manifest.version
                                   : pkg                                   ? std::wstring()
                                                                           : std::wstring(L" · not installed"));
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
    FillRect(dc, &rc, bandBrush_);
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
    if (isFooter) {
        Fill(dc, RECT{0, 0, rc.right, std::max(1, Px(1))}, hair);
        text(L"Changes apply at once", RECT{Px(20), 0, rc.right / 2, rc.bottom}, font_, palette_.textSecondary,
             DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
    } else if (kind_ == SettingsKind::Global) {
        text(L"Global · applies to every dock", RECT{Px(24), 0, rc.right - Px(16), rc.bottom}, font_, palette_.textSecondary,
             DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        Fill(dc, RECT{Px(24), rc.bottom - std::max(1, Px(1)), rc.right - Px(24), rc.bottom}, hair);
    } else if (kind_ == SettingsKind::Dock) {
        Fill(dc, RECT{0, 0, Px(6), rc.bottom}, dock);
        const DockConfig& d = Dock();
        const int sx = rc.right - Px(24) - Px(120);
        const int textRight = sx - Px(20) - Px(150) - Px(16);
        text(dockId_, RECT{Px(24), Px(10), textRight, Px(52)}, fontTitle_, idText, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        text(d.IsFolderWindow() ? L"You are editing this folder window only" : L"You are editing this dock only",
             RECT{Px(24), Px(52), textRight, Px(74)}, font_, palette_.textSecondary, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        // The chips.
        int x = Px(24);
        const int cy = Px(84), ch = Px(26);
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
        // The monitor silhouette with the dock on its edge.
        const RECT screen{sx, Px(14), sx + Px(120), Px(14) + Px(74)};
        FillRound(dc, RECT{screen.left - Px(2), screen.top - Px(2), screen.right + Px(2), screen.bottom + Px(2)},
                  Mix(palette_.text, bandColor_, 34), Px(8));
        FillRound(dc, screen, Mix(palette_.text, bandColor_, 8), Px(6));
        const int mid = (screen.left + screen.right) / 2;
        Fill(dc, RECT{mid - Px(12), screen.bottom + Px(2), mid + Px(12), screen.bottom + Px(12)}, Mix(palette_.text, bandColor_, 34));
        FillRound(dc, RECT{mid - Px(26), screen.bottom + Px(11), mid + Px(26), screen.bottom + Px(16)}, Mix(palette_.text, bandColor_, 34),
                  Px(3));
        const int inset = Px(5), thick = Px(7);
        RECT in{screen.left + inset, screen.top + inset, screen.right - inset, screen.bottom - inset};
        if (d.IsFolderWindow()) {
            const RECT w{in.left + Px(6), in.top + Px(4), in.left + Px(46), in.top + Px(34)};
            FillRound(dc, w, Mix(dock, bandColor_, 45), Px(4));
            Fill(dc, RECT{w.left, w.top, w.right, w.top + Px(6)}, dock);
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
        text(name, RECT{Px(70), Px(8), rc.right - Px(16), Px(42)}, fontTitle_, palette_.text, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        const DockItemEntry* e = TargetItem();
        if (e && e->Kind() == ItemKind::Widget && !widgetLine_.empty()) {
            text(widgetLine_, RECT{Px(70), Px(64), rc.right - Px(16), Px(84)}, font_, palette_.textSecondary,
                 DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        }
        Fill(dc, RECT{0, rc.bottom - std::max(1, Px(1)), rc.right, rc.bottom}, hair);
    }
    SelectObject(dc, oldFont);
    BitBlt(hdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
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
            SetTextColor(dc, id == kHeaderGlobal || id == kHeaderDockLink ? palette_.link : palette_.text);
            SetBkColor(dc, bandColor_);
            return reinterpret_cast<LRESULT>(bandBrush_);
        }
        case WM_COMMAND:
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
            if (host_.changed) host_.changed(SettingsScope::SaveOnly, DockIndex());
            Refresh();  // the Contents tab's check box, the chip, the button
            break;
        }
        case kHeaderGlobal:
            if (host_.openGlobal) host_.openGlobal(-1);
            break;
        case kHeaderDockLink:
            if (host_.openDock && DockIndex() != SIZE_MAX) host_.openDock(DockIndex(), -1);
            break;
        case kFooterReset:
            if (SettingsPage* page = ActivePage()) page->OnCommand(kWidgetReset, BN_CLICKED, nullptr);
            break;
        case kFooterConsole:
            if (host_.command) host_.command(ContextMenu::kWidgetConsole);
            break;
        default:
            break;
    }
}
