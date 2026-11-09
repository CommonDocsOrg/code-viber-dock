#pragma once

// Round 12 phase 12D: the WinDock.SettingsStrip control (Mode Strip / Drawer / Info) and the drawer's
// catalog, moved out of SettingsPagesContents.cpp so that the Contents tab and the dock's drawer popup
// (DrawerPopup, customize mode) share one implementation: the drawer is a grid of tiles now (a
// preview area, the name, a one-line note or "On the dock"), the strip and the inspector line are
// as before. The control asks its host (StripHost: the settings window, or the drawer popup) for
// the DPI, the palette, the dock colour and the widget previews.

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "Config.h"
#include "DarkMode.h"
#include "SettingsForm.h"
#include "SettingsWindow.h"
#include "WidgetPackage.h"

namespace StripUi {

// What the control needs of the window it lives in (the settings window's own methods, or the
// drawer popup's).
class StripHost {
public:
    using Image = SettingsWindow::PreviewImage;
    virtual int Px(int dip) const = 0;
    virtual const DarkMode::Palette& Colors() const = 0;
    virtual COLORREF DockColor() const = 0;
    // As SettingsWindow::Preview / PreviewEntry / PaintPreview (a cache per host).
    virtual const Image* Preview(const std::wstring& id, SIZE fit, bool render, float zoom = 1.0f) = 0;
    virtual const Image* PreviewEntry(const std::wstring& id, SIZE fit, float zoom = 1.0f) = 0;
    virtual void PaintPreview(HDC dc, const RECT& box, const Image& img, COLORREF under) const = 0;

protected:
    ~StripHost() = default;
};
// --- GDI helpers -----------------------------------------------------------------------------

inline COLORREF Mix(COLORREF a, COLORREF b, int pa) {
    auto ch = [&](BYTE x, BYTE y) { return static_cast<BYTE>((x * pa + y * (100 - pa)) / 100); };
    return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)), ch(GetBValue(a), GetBValue(b)));
}

inline int Luma(COLORREF c) { return (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000; }

inline COLORREF Rgb(uint32_t rgb) { return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF); }

inline void Fill(HDC dc, const RECT& r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

// A rounded rectangle filled with `fill`, outlined with `border` (1 px).
inline void Round(HDC dc, const RECT& r, COLORREF fill, COLORREF border, int radius) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(p);
    DeleteObject(b);
}

inline void Frame(HDC dc, const RECT& r, COLORREF border, int radius, int style = PS_SOLID) {
    HPEN p = CreatePen(style, 1, border);
    HGDIOBJ op = SelectObject(dc, p);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(p);
}

inline void Text(HDC dc, const std::wstring& s, RECT r, HFONT font, COLORREF color, UINT format) {
    HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, color);
    DrawTextW(dc, s.c_str(), static_cast<int>(s.size()), &r, format | DT_NOPREFIX | DT_SINGLELINE);
    SelectObject(dc, old);
}

inline int TextWidth(HDC dc, const std::wstring& s, HFONT font) {
    HGDIOBJ old = SelectObject(dc, font);
    SIZE sz{};
    GetTextExtentPoint32W(dc, s.c_str(), static_cast<int>(s.size()), &sz);
    SelectObject(dc, old);
    return sz.cx;
}

inline bool ContainsNoCase(const std::wstring& hay, const std::wstring& needle) {
    if (needle.empty()) return true;
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](wchar_t a, wchar_t b) { return std::towlower(a) == std::towlower(b); });
    return it != hay.end();
}

// The letter tile colours (the design's tints); a script widget's by its id.
inline COLORREF TintOf(ItemKind k, const std::wstring& id) {
    switch (k) {
        case ItemKind::Clock: return Rgb(0xCFE3FF);
        case ItemKind::Weather: return Rgb(0xB3E0FF);
        case ItemKind::Tray:
        case ItemKind::TrayApp:
        case ItemKind::Notifications: return Rgb(0xFFD49A);
        case ItemKind::Group: return Rgb(0xF3C86B);
        case ItemKind::Widget: {
            static const uint32_t kTints[] = {0xD8C8FF, 0xFFB3B3, 0xA8F0D8, 0xF3C86B, 0xC8F0A0, 0x9AD0FF, 0xF0A0C0};
            uint32_t h = 2166136261u;
            for (wchar_t ch : id) h = (h ^ static_cast<uint32_t>(std::towlower(ch))) * 16777619u;
            return Rgb(kTints[h % std::size(kTints)]);
        }
        case ItemKind::Running: return Rgb(0x9A9A9A);
        default: return Rgb(0xC0C0C0);
    }
}

inline wchar_t LetterOf(ItemKind k, const std::wstring& name) {
    switch (k) {
        case ItemKind::Clock: return L'C';
        case ItemKind::Weather: return L'W';
        case ItemKind::Tray:
        case ItemKind::TrayApp: return L'T';
        case ItemKind::Notifications: return L'B';  // the bell
        default: return name.empty() ? L'?' : static_cast<wchar_t>(std::towupper(name[0]));
    }
}

// --- the control -----------------------------------------------------------------------------

struct StripTile {
    enum class Shape { Icon, Letter, Separator, Running };
    Shape shape = Shape::Letter;
    std::wstring name;     // strip: the label under the tile; drawer: the row's name; info: the item's name
    std::wstring note;     // strip: the kind (accessible name); drawer: the one-line note; info: "kind · details"
    std::wstring version;  // drawer: a script widget's version
    HICON icon = nullptr;  // not owned (the page's caches)
    COLORREF tint = RGB(0xC0, 0xC0, 0xC0);
    wchar_t letter = L'?';
    bool disabled = false;  // drawer: a single-instance kind already on the dock
    bool header = false;    // drawer: a group header ("Built in")
    bool desktop = false;   // drawer: a script widget's row also offers "Desktop" (round 6 phase 6B: Add to the desktop)
    bool noDrag = false;    // drawer: not a drag source, Add only (round 8 phase 8B: App or shortcut…, Folder…: a dialog)
    // Round 6 phase 6E: a script widget's id: the drawer row / the inspector shows its live preview
    // (SettingsWindow::Preview) instead of the letter tile ("" none, or a widget that failed).
    std::wstring previewId;
    int ref = -1;           // strip: the item index; drawer: the entry index
};

class SettingsStrip {
public:
    enum class Mode { Strip, Drawer, Info };
    static constexpr wchar_t kClass[] = L"WinDock.SettingsStrip";

    static void Register() {
        static bool registered = false;
        if (registered) return;
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = &SettingsStrip::Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        wc.style = CS_DBLCLKS;
        registered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    SettingsStrip(StripHost& window, Mode mode, std::wstring name) : window_(window), mode_(mode), baseName_(std::move(name)) {}
    ~SettingsStrip() { DestroyFonts(); }
    SettingsStrip(const SettingsStrip&) = delete;
    SettingsStrip& operator=(const SettingsStrip&) = delete;

    void Attach(HWND hwnd) {
        hwnd_ = hwnd;
        if (hwnd) SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    }
    HWND Hwnd() const { return hwnd_; }
    void SetPeer(SettingsStrip* strip) { peer_ = strip; }

    // New tiles (a drag in progress is cancelled); selects tile `select` (clamped; -1 none) without
    // telling onSelect, keeps the scroll position where it can.
    void SetTiles(std::vector<StripTile> tiles, int select) {
        CancelDrag();
        tiles_ = std::move(tiles);
        hot_ = -1;
        hotDesk_ = false;
        Layout();
        sel_ = -1;
        Select(select, false, true);
        UpdateName();
        Repaint();
    }
    const std::vector<StripTile>& Tiles() const { return tiles_; }
    int Selected() const { return sel_; }
    // The selected tile's ref (-1 none).
    int SelectedRef() const { return sel_ >= 0 && sel_ < static_cast<int>(tiles_.size()) ? tiles_[static_cast<size_t>(sel_)].ref : -1; }

    // Selects tile `index` (strip: any; drawer: the nearest entry row), scrolls it into view.
    void Select(int index, bool notify, bool force = false) {
        const int n = static_cast<int>(tiles_.size());
        if (n == 0 || index < 0) {
            sel_ = -1;
            return;
        }
        index = std::clamp(index, 0, n - 1);
        if (mode_ == Mode::Drawer && tiles_[static_cast<size_t>(index)].header) {
            int next = index;
            while (next < n && tiles_[static_cast<size_t>(next)].header) ++next;
            if (next >= n) {
                next = index;
                while (next >= 0 && tiles_[static_cast<size_t>(next)].header) --next;
            }
            if (next < 0) return;
            index = next;
        }
        if (index == sel_ && !force) return;
        sel_ = index;
        EnsureVisible(index);
        UpdateName();
        if (hwnd_ && GetFocus() == hwnd_) NotifyWinEvent(EVENT_OBJECT_FOCUS, hwnd_, OBJID_CLIENT, CHILDID_SELF);
        Repaint();
        if (notify && onSelect) onSelect(tiles_[static_cast<size_t>(index)].ref);
    }

    // The palette or the DPI changed.
    void StyleChanged() {
        if (!hwnd_) return;
        DarkMode::ThemeScrollBars(hwnd_, window_.Colors().dark);
        Layout();
        Repaint();
    }

    // Automation: the centre of the tile / row with this ref (screen px); false when there is
    // none or the control is not shown. The point may lie outside the control while it is scrolled.
    bool CentreOf(int ref, POINT& screen) const {
        if (!hwnd_ || !IsWindowVisible(hwnd_)) return false;
        for (size_t i = 0; i < tiles_.size(); ++i) {
            if (tiles_[i].header || tiles_[i].ref != ref) continue;
            const RECT r = TileRect(i);
            screen = POINT{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
            ClientToScreen(hwnd_, &screen);
            return true;
        }
        return false;
    }
    // The centre of the drawer row's "Desktop" button for entry `ref` (screen px; round 6 phase 6B).
    bool DesktopCentreOf(int ref, POINT& screen) const {
        if (!hwnd_ || !IsWindowVisible(hwnd_) || mode_ != Mode::Drawer) return false;
        for (size_t i = 0; i < tiles_.size(); ++i) {
            if (tiles_[i].header || tiles_[i].ref != ref || !tiles_[i].desktop) continue;
            const RECT r = DesktopRect(TileRect(i));
            screen = POINT{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
            ClientToScreen(hwnd_, &screen);
            return true;
        }
        return false;
    }
    // The insertion slot the marker shows (-1: no drag over the strip).
    int Marker() const { return (dragging_ || external_) ? slot_ : -1; }

    // Round 6 phase 6E: the boxes previews are fitted into (px at the window's DPI): a drawer row's
    // preview area (72 x 44 DIPs less the panel's inset) and the inspector's (200 x 100 DIPs).
    static SIZE DrawerPreviewFit(const StripHost& w) {
        return SIZE{w.Px(kPreviewW) - 2 * std::max(1, w.Px(3)), w.Px(kPreviewH) - 2 * std::max(1, w.Px(3))};
    }
    static SIZE InfoPreviewFit(const StripHost& w) {
        return SIZE{w.Px(kInfoPreviewW) - 2 * std::max(1, w.Px(3)), w.Px(kInfoPreviewH) - 2 * std::max(1, w.Px(3))};
    }
    static constexpr int kInfoPreviewW = 200, kInfoPreviewH = 100;  // DIPs
    static constexpr float kInfoZoom = 2.0f;  // a small widget is drawn up to twice its dock size there

    // A drawer row dragged over the strip (screen point; `ghost` follows the pointer).
    void ExternalOver(POINT screen, const StripTile& ghost) {
        POINT pt = screen;
        ScreenToClient(hwnd_, &pt);
        external_ = true;
        ghost_ = ghost;
        ghost_.header = false;
        grab_ = POINT{TileW(ghost_) / 2, Px(kTileH) / 2};
        AutoScroll(pt.x);
        dragPt_ = pt;
        slot_ = SlotAt(pt.x);
        Repaint();
    }
    void ExternalLeave() {
        if (!external_) return;
        external_ = false;
        slot_ = -1;
        Repaint();
    }
    // The slot a drop lands in (-1: not over the strip); the marker goes away.
    int ExternalDrop() {
        const int s = external_ ? slot_ : -1;
        ExternalLeave();
        return s;
    }

    std::function<void(int ref)> onSelect;            // a tile / row became the selected one (mouse, keyboard)
    std::function<void(int ref)> onActivate;          // Enter, a double click; the drawer: a click on a tile (add it)
    std::function<void(int from, int slot)> onMove;   // strip: move item `from` before slot `slot`
    std::function<void(int ref)> onDelete;            // strip: Delete
    std::function<void(int ref, int slot)> onDropEntry;  // drawer: entry `ref` dropped on the strip at `slot`
    std::function<void(int ref)> onDesktop;            // drawer: a tile's "Desktop" button (Add to the desktop, round 6 phase 6B)
    // Round 12 phase 12D: a drawer tile dragged outside the control and its peer strip (screen px):
    // the host shows where it would land (a dock's marker) and answers 0 nowhere (the no-drop
    // cursor), 1 a dock, 2 the desktop (a script widget becomes a standalone widget there); the
    // drag left that area / was cancelled; it was released there (true: something was added).
    std::function<int(POINT screen, int ref)> onDragOutside;
    std::function<void()> onDragOutsideLeave;
    std::function<bool(POINT screen, int ref)> onDropOutside;

    // Round 12 phase 12D: the grid's content height for a client width (px; the drawer popup sizes
    // itself to it).
    int GridHeight(int width) const { return GridLayout(width, nullptr); }
    // The tiles shown (headers left out): QueryState 312.
    int TileCount() const {
        return static_cast<int>(std::count_if(tiles_.begin(), tiles_.end(), [](const StripTile& t) { return !t.header; }));
    }

private:
    // DIPs
    static constexpr int kTileW = 84, kSepW = 28, kRunningW = 150, kTileH = 84, kIcon = 44, kGap = 2, kPad = 10;
    // Round 12 phase 12D: the drawer's grid: tiles about 112 DIPs wide (stretched to fill the row),
    // a 56-DIP preview area, the name and a note under it; a group header spans the row.
    static constexpr int kGridTileW = 112, kGridTileH = 104, kGridGap = 8, kGridHeaderH = 28, kGridPreviewH = 56;
    static constexpr int kDeskW = 26, kDeskH = 20;  // a script widget tile's "Desktop" button (top right of its preview)
    // The preview area's box (DIPs) the previews are fitted into (kept fixed: the cache key and
    // QueryState 264 do not depend on the drawer's width).
    static constexpr int kPreviewW = 100, kPreviewH = kGridPreviewH;
    // Previews rendered during one paint: past this many ms the rest wait for the next paint (the
    // control invalidates itself after the paint), so a page full of new previews stays responsive.
    static constexpr double kPaintBudgetMs = 40.0;

    int Px(int dip) const { return window_.Px(dip); }
    void Repaint() const {
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }

    int TileW(const StripTile& t) const {
        return Px(t.shape == StripTile::Shape::Separator ? kSepW : t.shape == StripTile::Shape::Running ? kRunningW : kTileW);
    }
    SIZE Client() const {
        RECT rc{};
        if (hwnd_) GetClientRect(hwnd_, &rc);
        return SIZE{rc.right, rc.bottom};
    }
    bool Horizontal() const { return mode_ == Mode::Strip; }

    // Tile i in client px (scrolled): a strip tile, or a drawer grid cell / header row.
    RECT TileRect(size_t i) const {
        const SIZE c = Client();
        if (mode_ == Mode::Drawer) {
            if (i >= grid_.size()) return RECT{};
            RECT r = grid_[i];
            OffsetRect(&r, 0, -scroll_);
            return r;
        }
        if (i >= pos_.size()) return RECT{};
        const StripTile& t = tiles_[i];
        const int top = std::max(0, static_cast<int>(c.cy - Px(kTileH)) / 2);
        const int x = pos_[i] - scroll_;
        return RECT{x, top, x + TileW(t), top + Px(kTileH)};
    }

    // Round 12 phase 12D: the grid for client width `width` (content px, unscrolled): rows of
    // cells as wide as fit (at least kGridTileW, the rest shared out), a header starting a new row.
    // Returns the content height; `out` gets each tile's rect.
    int GridLayout(int width, std::vector<RECT>* out) const {
        if (out) out->assign(tiles_.size(), RECT{});
        const int gap = Px(kGridGap), tw = Px(kGridTileW), th = Px(kGridTileH), hh = Px(kGridHeaderH);
        const int cols = std::max(1, (width + gap) / (tw + gap));
        const int cellW = std::max(Px(48), (width - (cols - 1) * gap) / cols);
        int y = 0, col = 0, bottom = 0;
        for (size_t i = 0; i < tiles_.size(); ++i) {
            if (tiles_[i].header) {
                if (col > 0) y += th + gap;
                col = 0;
                const RECT r{0, y, width, y + hh};
                if (out) (*out)[i] = r;
                y += hh;
                bottom = r.bottom;
                continue;
            }
            const int x = col * (cellW + gap);
            const RECT r{x, y, x + cellW, y + th};
            if (out) (*out)[i] = r;
            bottom = r.bottom;
            if (++col == cols) {
                col = 0;
                y += th + gap;
            }
        }
        return bottom;
    }

    // Positions, the content length and the scroll bar.
    void Layout() {
        if (!hwnd_ || mode_ == Mode::Info || inLayout_) return;
        inLayout_ = true;
        if (mode_ == Mode::Drawer) {
            for (int pass = 0; pass < 2; ++pass) {
                // Twice: showing / hiding the scroll bar changes the width.
                const SIZE c = Client();
                content_ = GridLayout(c.cx, &grid_);
                scroll_ = std::clamp(scroll_, 0, std::max(0, content_ - static_cast<int>(c.cy)));
                SCROLLINFO si{sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS, 0, std::max(0, content_ - 1),
                              static_cast<UINT>(std::max<LONG>(0, c.cy)), scroll_, 0};
                SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
            }
            inLayout_ = false;
            return;
        }
        pos_.assign(tiles_.size(), 0);
        int at = Px(kPad);
        for (size_t i = 0; i < tiles_.size(); ++i) {
            pos_[i] = at;
            at += TileW(tiles_[i]) + Px(kGap);
        }
        content_ = at - Px(kGap) + Px(kPad);
        for (int pass = 0; pass < 2; ++pass) {
            // Twice: showing / hiding the bar changes the view.
            const SIZE c = Client();
            const int view = c.cx;
            scroll_ = std::clamp(scroll_, 0, std::max(0, content_ - view));
            SCROLLINFO si{sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS, 0, std::max(0, content_ - 1), static_cast<UINT>(std::max(0, view)),
                          scroll_, 0};
            SetScrollInfo(hwnd_, SB_HORZ, &si, TRUE);
        }
        inLayout_ = false;
    }

    int ViewLength() const {
        const SIZE c = Client();
        return Horizontal() ? c.cx : c.cy;
    }

    void ScrollTo(int pos) {
        pos = std::clamp(pos, 0, std::max(0, content_ - ViewLength()));
        if (pos == scroll_) return;
        scroll_ = pos;
        SetScrollPos(hwnd_, Horizontal() ? SB_HORZ : SB_VERT, scroll_, TRUE);
        Repaint();
    }

    void EnsureVisible(int index) {
        if (!hwnd_ || index < 0 || index >= static_cast<int>(tiles_.size())) return;
        int top = 0, end = 0;
        if (mode_ == Mode::Drawer) {
            if (static_cast<size_t>(index) >= grid_.size()) return;
            top = grid_[static_cast<size_t>(index)].top;
            end = grid_[static_cast<size_t>(index)].bottom;
            // The first row of a group: its header too.
            for (int k = index - 1; k >= 0; --k) {
                const RECT& r = grid_[static_cast<size_t>(k)];
                if (tiles_[static_cast<size_t>(k)].header) {
                    if (r.bottom == top) top = r.top;
                    break;
                }
                if (r.top != top) break;
            }
        } else {
            if (static_cast<size_t>(index) >= pos_.size()) return;
            top = pos_[static_cast<size_t>(index)] - Px(kPad);
            end = pos_[static_cast<size_t>(index)] + TileW(tiles_[static_cast<size_t>(index)]) + Px(kPad);
        }
        const int view = ViewLength();
        if (top < scroll_) {
            ScrollTo(top);
        } else if (end > scroll_ + view) {
            ScrollTo(end - view);
        }
    }

    int HitTile(POINT pt) const {
        for (size_t i = 0; i < tiles_.size(); ++i) {
            const RECT r = TileRect(i);
            if (PtInRect(&r, pt)) return static_cast<int>(i);
        }
        return -1;
    }

    // The insertion slot nearest to client x: before the first tile whose centre is right of x.
    int SlotAt(int x) const {
        for (size_t i = 0; i < tiles_.size(); ++i) {
            const RECT r = TileRect(i);
            if (x < (r.left + r.right) / 2) return static_cast<int>(i);
        }
        return static_cast<int>(tiles_.size());
    }

    RECT MarkerRect(int slot) const {
        const SIZE c = Client();
        int x = Px(kPad) - scroll_;
        if (!tiles_.empty()) {
            x = slot < static_cast<int>(tiles_.size()) ? TileRect(static_cast<size_t>(slot)).left - Px(kGap) / 2
                                                       : TileRect(tiles_.size() - 1).right + Px(kGap) / 2;
        }
        const int w = std::max(2, Px(3)), h = Px(72);
        const int top = std::max(0, static_cast<int>(c.cy - h) / 2);
        return RECT{x - w / 2, top, x - w / 2 + w, top + h};
    }

    // A grid tile's preview area: across its top.
    RECT PreviewRect(const RECT& tile) const {
        return RECT{tile.left + Px(6), tile.top + Px(6), tile.right - Px(6), tile.top + Px(6) + Px(kGridPreviewH)};
    }
    // A script widget tile's "Desktop" button: the preview area's top-right corner.
    RECT DesktopRect(const RECT& tile) const {
        const RECT p = PreviewRect(tile);
        return RECT{p.right - Px(2) - Px(kDeskW), p.top + Px(2), p.right - Px(2), p.top + Px(2) + Px(kDeskH)};
    }

    // While dragging near an end of the strip, it scrolls (one step per mouse move).
    void AutoScroll(int x) {
        if (!Horizontal()) return;
        const int edge = Px(28), step = Px(16);
        if (x < edge) ScrollTo(scroll_ - step);
        if (x > ViewLength() - edge) ScrollTo(scroll_ + step);
    }

    void CancelDrag() {
        const bool was = dragging_ || pressed_ >= 0 || external_;
        if (mode_ == Mode::Drawer && dragging_ && peer_) peer_->ExternalLeave();
        if (outside_ && onDragOutsideLeave) onDragOutsideLeave();  // round 12 phase 12D: a dock's marker goes
        outside_ = false;
        dragging_ = false;
        pressed_ = -1;
        pressAdd_ = false;
        pressDesk_ = false;
        external_ = false;
        slot_ = -1;
        if (hwnd_ && GetCapture() == hwnd_) {
            releasing_ = true;
            ReleaseCapture();
            releasing_ = false;
        }
        if (was) Repaint();
    }

    void UpdateName() {
        if (!hwnd_) return;
        std::wstring name = baseName_;
        if (mode_ == Mode::Info) {
            if (!tiles_.empty()) name = tiles_[0].name + L", " + tiles_[0].note;
        } else if (sel_ >= 0 && sel_ < static_cast<int>(tiles_.size())) {
            const StripTile& t = tiles_[static_cast<size_t>(sel_)];
            name += L": " + t.name;
            if (!t.note.empty()) name += L", " + t.note;
            if (mode_ == Mode::Strip) name += L", item " + std::to_wstring(sel_ + 1) + L" of " + std::to_wstring(tiles_.size());
        }
        wchar_t cur[512]{};
        GetWindowTextW(hwnd_, cur, static_cast<int>(std::size(cur)));
        if (name == cur) return;
        SetWindowTextW(hwnd_, name.c_str());
        NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, hwnd_, OBJID_CLIENT, CHILDID_SELF);
    }

    // --- fonts ---
    void SetFont(HFONT font) {
        DestroyFonts();
        font_ = font;
        LOGFONTW lf{};
        if (!font || !GetObjectW(font, sizeof(lf), &lf)) return;
        LOGFONTW b = lf;
        b.lfWeight = FW_SEMIBOLD;
        bold_ = CreateFontIndirectW(&b);
        LOGFONTW s = lf;
        s.lfHeight = lf.lfHeight * 11 / 12;
        small_ = CreateFontIndirectW(&s);
        s.lfWeight = FW_SEMIBOLD;
        smallBold_ = CreateFontIndirectW(&s);
        LOGFONTW l = b;
        l.lfWeight = FW_BOLD;
        l.lfHeight = lf.lfHeight * 4 / 3;
        letter_ = CreateFontIndirectW(&l);
    }
    void DestroyFonts() {
        for (HFONT* f : {&bold_, &small_, &smallBold_, &letter_}) {
            if (*f) DeleteObject(*f);
            *f = nullptr;
        }
    }
    HFONT Base() const { return font_ ? font_ : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)); }
    HFONT Bold() const { return bold_ ? bold_ : Base(); }
    HFONT Small() const { return small_ ? small_ : Base(); }
    HFONT SmallBold() const { return smallBold_ ? smallBold_ : Bold(); }
    HFONT LetterFont() const { return letter_ ? letter_ : Bold(); }

    // --- painting ---
    struct Colors {
        COLORREF layer, back, text, secondary, disabled, dock, hair;
        bool hc;
    };
    Colors Palette() const {
        const DarkMode::Palette& p = window_.Colors();
        Colors c{};
        c.layer = p.layer;
        c.back = mode_ == Mode::Strip ? (p.highContrast ? p.control : p.dark ? Mix(RGB(0, 0, 0), p.layer, 30) : p.control) : p.layer;
        c.text = p.text;
        c.secondary = p.textSecondary;
        c.disabled = p.textDisabled;
        c.dock = p.highContrast ? p.accent : window_.DockColor();
        c.hair = Mix(p.text, p.layer, 18);
        c.hc = p.highContrast;
        return c;
    }

    // The icon / letter / bar / placeholder boxes of a tile in `box`.
    void PaintGlyph(HDC dc, const StripTile& t, const RECT& box, const Colors& c, COLORREF back) const {
        const int w = box.right - box.left, h = box.bottom - box.top;
        switch (t.shape) {
            case StripTile::Shape::Separator: {
                const int bw = std::max(1, Px(2));
                const int cx = (box.left + box.right) / 2;
                Fill(dc, RECT{cx - bw / 2, box.top, cx - bw / 2 + bw, box.bottom}, Mix(c.text, back, 35));
                return;
            }
            case StripTile::Shape::Running: {
                const int n = w >= Px(90) ? 3 : 2;
                const int gap = std::max(1, Px(4) * h / Px(kIcon));
                const int bw = (w - (n - 1) * gap) / n;
                SetBkMode(dc, TRANSPARENT);
                for (int i = 0; i < n; ++i) {
                    const RECT b{box.left + i * (bw + gap), box.top, box.left + i * (bw + gap) + bw, box.bottom};
                    Round(dc, b, Mix(c.text, back, 12), Mix(c.text, back, 12), Px(8) * h / Px(kIcon));
                    Frame(dc, b, Mix(c.text, back, 45), Px(8) * h / Px(kIcon), PS_DOT);
                }
                return;
            }
            case StripTile::Shape::Icon:
                if (t.icon) {
                    DrawIconEx(dc, box.left, box.top, t.icon, w, h, 0, nullptr, DI_NORMAL);
                    return;
                }
                [[fallthrough]];
            case StripTile::Shape::Letter:
            default: {
                const COLORREF tint = c.hc ? c.secondary : t.disabled ? Mix(t.tint, back, 45) : t.tint;
                Round(dc, box, tint, tint, std::max(4, w * 10 / 44 * 2));
                const COLORREF ink = c.hc ? c.layer : Luma(tint) > 150 ? RGB(0x10, 0x10, 0x10) : RGB(0xFF, 0xFF, 0xFF);
                SetBkMode(dc, TRANSPARENT);
                Text(dc, std::wstring(1, t.letter), box, w >= Px(40) ? LetterFont() : SmallBold(), ink, DT_CENTER | DT_VCENTER);
                return;
            }
        }
    }

    // A strip tile in `r` (selected: the dock colour; hot: a light fill).
    void PaintTile(HDC dc, const StripTile& t, const RECT& r, bool selected, bool hot, const Colors& c) const {
        const int radius = Px(8) * 2;
        if (selected) {
            Round(dc, r, Mix(c.dock, c.back, 24), c.dock, radius);
        } else if (hot) {
            const COLORREF f = Mix(c.text, c.back, 9);
            Round(dc, r, f, f, radius);
        }
        const COLORREF back = selected ? Mix(c.dock, c.back, 24) : hot ? Mix(c.text, c.back, 9) : c.back;
        const int cx = (r.left + r.right) / 2;
        const int top = r.top + Px(8);
        RECT box{cx - Px(kIcon) / 2, top, cx - Px(kIcon) / 2 + Px(kIcon), top + Px(kIcon)};
        if (t.shape == StripTile::Shape::Running) box = RECT{cx - Px(49), top, cx - Px(49) + Px(98), top + Px(kIcon)};
        if (t.shape == StripTile::Shape::Separator) box = RECT{cx - Px(2), top, cx + Px(2), top + Px(kIcon)};
        PaintGlyph(dc, t, box, c, back);
        if (t.shape == StripTile::Shape::Separator) return;
        SetBkMode(dc, TRANSPARENT);
        const RECT label{r.left + Px(4), top + Px(kIcon) + Px(5), r.right - Px(4), r.bottom - Px(4)};
        Text(dc, t.name, label, Small(), Mix(c.text, back, 88), DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);
    }

    // A drawer grid tile (or group header) in `r` (round 12 phase 12D: was a row with Add / Desktop
    // buttons; a click on the tile adds it now, the Desktop button stays for script widgets).
    void PaintGridTile(HDC dc, const StripTile& t, const RECT& r, bool focused, bool hot, bool deskHot, const Colors& c) const {
        SetBkMode(dc, TRANSPARENT);
        if (t.header) {
            Text(dc, t.name, RECT{r.left + Px(2), r.top + Px(6), r.right, r.bottom}, SmallBold(), c.secondary,
                 DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
            return;
        }
        const bool live = !t.disabled;
        const COLORREF fill = Mix(c.text, c.layer, live && hot ? 10 : 5);
        Round(dc, r, fill, Mix(c.text, c.layer, live && hot ? 24 : 13), Px(6) * 2);
        // Round 6 phase 6E: a script widget's live preview fills the preview area; the letter tile
        // while it renders / when it failed; the built-in kinds keep their glyphs (centred in it).
        const RECT area = PreviewRect(r);
        const StripHost::Image* img = t.previewId.empty() ? nullptr : PreviewFor(t.previewId);
        if (img) {
            window_.PaintPreview(dc, area, *img, fill);
        } else {
            const int cx = (area.left + area.right) / 2, cy = (area.top + area.bottom) / 2;
            RECT box{cx - Px(20), cy - Px(20), cx - Px(20) + Px(40), cy - Px(20) + Px(40)};
            if (t.shape == StripTile::Shape::Running) box = RECT{cx - Px(36), cy - Px(16), cx - Px(36) + Px(72), cy - Px(16) + Px(32)};
            PaintGlyph(dc, t, box, c, fill);
        }
        SetBkMode(dc, TRANSPARENT);
        if (t.desktop) {
            // A small monitor: "put it on the desktop" (QueryState 252 gives its centre).
            const RECT desk = DesktopRect(r);
            Round(dc, desk, Mix(c.text, c.layer, live && deskHot ? 26 : 14), Mix(c.text, c.layer, live ? 36 : 18), Px(4) * 2);
            const COLORREF ink = live ? c.text : c.disabled;
            const int mx = (desk.left + desk.right) / 2, my = (desk.top + desk.bottom) / 2;
            const RECT screen{mx - Px(7), my - Px(6), mx + Px(7), my + Px(3)};
            Frame(dc, screen, ink, Px(2));
            Fill(dc, RECT{mx - Px(3), my + Px(5), mx + Px(3), my + Px(5) + std::max(1, Px(1))}, ink);
            Fill(dc, RECT{mx - std::max(1, Px(1)) / 2, my + Px(3), mx - std::max(1, Px(1)) / 2 + std::max(1, Px(1)), my + Px(5)}, ink);
        }
        // The name, then the note ("On the dock" for a kind the dock can have once and has).
        const int y = area.bottom + Px(4);
        Text(dc, t.name, RECT{r.left + Px(6), y, r.right - Px(6), y + Px(18)}, SmallBold(), live ? c.text : c.disabled,
             DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);
        Text(dc, t.note, RECT{r.left + Px(6), y + Px(18), r.right - Px(6), y + Px(34)}, Small(), live ? c.secondary : c.disabled,
             DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);
        if (focused) {
            RECT f = r;
            InflateRect(&f, -Px(2), -Px(2));
            SetTextColor(dc, c.text);
            DrawFocusRect(dc, &f);
        }
    }

    // The dragged tile at 70 % alpha, following the pointer.
    void PaintGhost(HDC dc, const StripTile& t, const Colors& c) const {
        const SIZE client = Client();
        const int w = TileW(t), h = Px(kTileH);
        int x = std::clamp(static_cast<int>(dragPt_.x - grab_.x), static_cast<int>(-w / 2), static_cast<int>(client.cx - w / 2));
        int y = std::clamp(static_cast<int>(dragPt_.y - grab_.y), 0, std::max(0, static_cast<int>(client.cy) - h));
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1, w), std::max(1, h));
        HGDIOBJ old = SelectObject(mem, bmp);
        Fill(mem, RECT{0, 0, w, h}, c.back);
        PaintTile(mem, t, RECT{0, 0, w, h}, true, false, c);
        const BLENDFUNCTION blend{AC_SRC_OVER, 0, 178, 0};
        GdiAlphaBlend(dc, x, y, w, h, mem, 0, 0, w, h, blend);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
    }

    // A drawer row's preview (rendered now while the paint's budget lasts; null: not yet, or failed).
    const StripHost::Image* PreviewFor(const std::wstring& id) const {
        const SIZE fit = DrawerPreviewFit(window_);
        if (!window_.PreviewEntry(id, fit)) {
            if (paintRenders_ > 0 && NowMs() - paintStart_ >= kPaintBudgetMs) {
                previewPending_ = true;  // the next paint renders it
                return nullptr;
            }
            ++paintRenders_;
        }
        return window_.Preview(id, fit, true);
    }
    static double NowMs() {
        LARGE_INTEGER f{}, c{};
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&c);
        return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
    }

    void Paint(HDC dc) const {
        const SIZE client = Client();
        const RECT all{0, 0, client.cx, client.cy};
        const Colors c = Palette();
        const bool focus = GetFocus() == hwnd_;
        paintStart_ = NowMs();
        paintRenders_ = 0;
        previewPending_ = false;
        if (mode_ == Mode::Info) {
            Fill(dc, all, c.layer);
            SetBkMode(dc, TRANSPARENT);
            if (tiles_.empty()) {
                Text(dc, L"Select an item on the strip", all, Base(), c.secondary, DT_LEFT | DT_VCENTER);
                return;
            }
            const StripTile& t = tiles_[0];
            // Round 6 phase 6E: a script widget's larger preview above its name.
            if (!t.previewId.empty()) {
                if (const StripHost::Image* img = window_.Preview(t.previewId, InfoPreviewFit(window_), true, kInfoZoom)) {
                    const RECT area{0, 0, std::min(static_cast<int>(client.cx), Px(kInfoPreviewW)), Px(kInfoPreviewH)};
                    window_.PaintPreview(dc, area, *img, c.layer);
                    const int y = Px(kInfoPreviewH) + Px(2);
                    Text(dc, t.name, RECT{0, y, client.cx, y + Px(17)}, Bold(), c.text, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
                    Text(dc, t.note, RECT{0, y + Px(17), client.cx, y + Px(34)}, Small(), c.secondary, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
                    return;
                }
            }
            const int s = std::min(Px(32), static_cast<int>(client.cy));
            const int top = (client.cy - s) / 2;
            RECT box{0, top, s, top + s};
            if (t.shape == StripTile::Shape::Running) box.right = s * 3 / 2;
            PaintGlyph(dc, t, box, c, c.layer);
            const int x = box.right + Px(12);
            Text(dc, t.name, RECT{x, 0, client.cx, client.cy / 2 + Px(1)}, Bold(), c.text, DT_LEFT | DT_BOTTOM | DT_END_ELLIPSIS);
            Text(dc, t.note, RECT{x, client.cy / 2 + Px(1), client.cx, client.cy}, Small(), c.secondary,
                 DT_LEFT | DT_TOP | DT_END_ELLIPSIS);
            return;
        }
        Fill(dc, all, c.layer);
        if (mode_ == Mode::Strip) {
            Round(dc, all, c.back, c.hair, Px(12) * 2);
            if (tiles_.empty()) {
                SetBkMode(dc, TRANSPARENT);
                Text(dc, L"The dock is empty: add apps, folders or widgets.", all, Base(), c.secondary, DT_CENTER | DT_VCENTER);
            }
            for (size_t i = 0; i < tiles_.size(); ++i) {
                const RECT r = TileRect(i);
                if (r.right < 0 || r.left > client.cx) continue;
                PaintTile(dc, tiles_[i], r, static_cast<int>(i) == sel_, static_cast<int>(i) == hot_ && !dragging_, c);
            }
            if (focus && sel_ >= 0 && sel_ < static_cast<int>(tiles_.size()) && !(SendMessageW(hwnd_, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
                RECT f = TileRect(static_cast<size_t>(sel_));
                InflateRect(&f, -Px(3), -Px(3));
                SetTextColor(dc, c.text);
                DrawFocusRect(dc, &f);
            }
            if ((dragging_ || external_) && slot_ >= 0) {
                const RECT m = MarkerRect(slot_);
                Round(dc, m, c.dock, c.dock, std::max(2, Px(2)));
            }
            if (dragging_ && pressed_ >= 0 && pressed_ < static_cast<int>(tiles_.size())) PaintGhost(dc, tiles_[static_cast<size_t>(pressed_)], c);
            if (external_) PaintGhost(dc, ghost_, c);
            return;
        }
        // Drawer
        if (tiles_.empty()) {
            SetBkMode(dc, TRANSPARENT);
            Text(dc, L"No widget matches the search.", RECT{Px(2), 0, client.cx, Px(kGridHeaderH)}, Small(), c.secondary, DT_LEFT | DT_VCENTER);
        }
        const bool cues = !(SendMessageW(hwnd_, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
        for (size_t i = 0; i < tiles_.size(); ++i) {
            const RECT r = TileRect(i);
            if (r.bottom < 0 || r.top > client.cy) continue;
            const bool isHot = static_cast<int>(i) == hot_;
            PaintGridTile(dc, tiles_[i], r, focus && cues && static_cast<int>(i) == sel_, isHot, isHot && hotDesk_, c);
        }
    }

    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<SettingsStrip*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        if (msg == WM_NCDESTROY) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            self->hwnd_ = nullptr;
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        return self->OnMessage(msg, wp, lp);
    }

    void OnScrollBar(int code) {
        SCROLLINFO si{sizeof(si), SIF_ALL};
        GetScrollInfo(hwnd_, Horizontal() ? SB_HORZ : SB_VERT, &si);
        const int line = Horizontal() ? Px(kTileW) : Px(kGridTileH + kGridGap);
        int pos = scroll_;
        switch (code) {
            case SB_LINEUP: pos -= line; break;
            case SB_LINEDOWN: pos += line; break;
            case SB_PAGEUP: pos -= static_cast<int>(si.nPage); break;
            case SB_PAGEDOWN: pos += static_cast<int>(si.nPage); break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION: pos = si.nTrackPos; break;
            case SB_TOP: pos = 0; break;
            case SB_BOTTOM: pos = content_; break;
            default: return;
        }
        ScrollTo(pos);
    }

    void TrackLeave() {
        if (tracking_) return;
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
        tracking_ = TrackMouseEvent(&tme) != FALSE;
    }

    void OnKey(WPARAM vk) {
        const int n = static_cast<int>(tiles_.size());
        if (mode_ == Mode::Strip) {
            const bool ctrl = GetKeyState(VK_CONTROL) < 0;
            switch (vk) {
                case VK_LEFT:
                case VK_UP:
                    if (ctrl) {
                        if (sel_ > 0 && onMove) onMove(sel_, sel_ - 1);
                    } else {
                        Select(sel_ < 0 ? 0 : sel_ - 1, true);
                    }
                    break;
                case VK_RIGHT:
                case VK_DOWN:
                    if (ctrl) {
                        if (sel_ >= 0 && sel_ < n - 1 && onMove) onMove(sel_, sel_ + 2);
                    } else {
                        Select(sel_ < 0 ? 0 : sel_ + 1, true);
                    }
                    break;
                case VK_HOME: Select(0, true); break;
                case VK_END: Select(n - 1, true); break;
                case VK_DELETE:
                    if (sel_ >= 0 && sel_ < n && onDelete) onDelete(sel_);
                    break;
                case VK_RETURN:
                    if (sel_ >= 0 && sel_ < n && onActivate) onActivate(sel_);
                    break;
                default: break;
            }
            return;
        }
        if (mode_ != Mode::Drawer || n == 0) return;
        auto step = [&](int from, int dir) {
            for (int i = from + dir; i >= 0 && i < n; i += dir) {
                if (!tiles_[static_cast<size_t>(i)].header) return i;
            }
            return from;
        };
        // Round 12 phase 12D, the grid: Up / Down go to the tile of the row above / below whose
        // centre is nearest to the selected one's (across a header).
        auto vertical = [&](int dir) {
            if (sel_ < 0 || static_cast<size_t>(sel_) >= grid_.size()) return step(-1, 1);
            const RECT& at = grid_[static_cast<size_t>(sel_)];
            const int cx = (at.left + at.right) / 2;
            int rowTop = -1, best = sel_, bestD = 0;
            for (int i = sel_ + dir; i >= 0 && i < n; i += dir) {
                const RECT& r = grid_[static_cast<size_t>(i)];
                if (tiles_[static_cast<size_t>(i)].header || r.top == at.top) continue;
                if (rowTop < 0) rowTop = r.top;
                if (r.top != rowTop) break;
                const int d = std::abs((r.left + r.right) / 2 - cx);
                if (best == sel_ || d < bestD) {
                    best = i;
                    bestD = d;
                }
            }
            return best;
        };
        switch (vk) {
            case VK_LEFT: Select(sel_ < 0 ? step(-1, 1) : step(sel_, -1), true); break;
            case VK_RIGHT: Select(sel_ < 0 ? step(-1, 1) : step(sel_, 1), true); break;
            case VK_UP: Select(vertical(-1), true); break;
            case VK_DOWN: Select(vertical(1), true); break;
            case VK_HOME: Select(step(-1, 1), true); break;
            case VK_END: Select(step(n, -1), true); break;
            case VK_PRIOR: Select(std::max(0, sel_ - 5), true); break;
            case VK_NEXT: Select(std::min(n - 1, sel_ + 5), true); break;
            case VK_RETURN:
            case VK_SPACE:
                if (sel_ >= 0 && sel_ < n) {
                    const StripTile& t = tiles_[static_cast<size_t>(sel_)];
                    if (t.disabled) {
                        MessageBeep(MB_ICONWARNING);
                    } else if (onActivate) {
                        onActivate(t.ref);
                    }
                }
                break;
            default: break;
        }
    }

    LRESULT OnMessage(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_SETFONT:
                SetFont(reinterpret_cast<HFONT>(wp));
                Layout();
                if (lp) Repaint();
                return 0;
            case WM_GETFONT:
                return reinterpret_cast<LRESULT>(font_);
            case WM_SIZE:
                Layout();
                Repaint();
                return 0;
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(hwnd_, &ps);
                const SIZE c = Client();
                HDC mem = CreateCompatibleDC(hdc);
                HBITMAP bmp = CreateCompatibleBitmap(hdc, std::max<LONG>(1, c.cx), std::max<LONG>(1, c.cy));
                HGDIOBJ old = SelectObject(mem, bmp);
                Paint(mem);
                BitBlt(hdc, 0, 0, c.cx, c.cy, mem, 0, 0, SRCCOPY);
                SelectObject(mem, old);
                DeleteObject(bmp);
                DeleteDC(mem);
                EndPaint(hwnd_, &ps);
                if (previewPending_) InvalidateRect(hwnd_, nullptr, FALSE);  // the previews this paint had no time for
                return 0;
            }
            case WM_SETFOCUS:
                if (sel_ < 0 && !tiles_.empty() && mode_ != Mode::Info) Select(0, true);
                NotifyWinEvent(EVENT_OBJECT_FOCUS, hwnd_, OBJID_CLIENT, CHILDID_SELF);
                Repaint();
                return 0;
            case WM_KILLFOCUS:
                Repaint();
                return 0;
            case WM_UPDATEUISTATE:
                Repaint();
                break;
            case WM_GETDLGCODE: {
                if (mode_ == Mode::Info) return DLGC_STATIC;
                if (dragging_ || pressed_ >= 0) return DLGC_WANTALLKEYS;
                // Arrows, Home / End, Delete and Enter are the control's; Tab and Esc the window's.
                LRESULT code = DLGC_WANTARROWS | DLGC_WANTCHARS;
                if (const MSG* m = reinterpret_cast<const MSG*>(lp); m && m->message == WM_KEYDOWN && m->wParam == VK_RETURN) {
                    code |= DLGC_WANTMESSAGE;
                }
                return code;
            }
            case WM_KEYDOWN:
                if (wp == VK_ESCAPE && (dragging_ || pressed_ >= 0)) {
                    CancelDrag();
                    return 0;
                }
                if (dragging_ || pressed_ >= 0) return 0;
                OnKey(wp);
                return 0;
            case WM_CHAR:
                return 0;
            case WM_HSCROLL:
            case WM_VSCROLL:
                if ((msg == WM_HSCROLL) == Horizontal()) OnScrollBar(LOWORD(wp));
                return 0;
            case WM_MOUSEWHEEL:
            case WM_MOUSEHWHEEL: {
                if (mode_ == Mode::Info || content_ <= ViewLength()) break;  // the page scrolls
                const int delta = GET_WHEEL_DELTA_WPARAM(wp);
                const int unit = Horizontal() ? Px(kTileW) : Px(kGridTileH + kGridGap);
                const int d = MulDiv(delta, unit, WHEEL_DELTA);
                ScrollTo(msg == WM_MOUSEHWHEEL ? scroll_ + d : scroll_ - d);
                return 0;
            }
            case WM_LBUTTONDOWN:
            case WM_LBUTTONDBLCLK: {
                if (mode_ == Mode::Info) return 0;
                SetFocus(hwnd_);
                const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                const int hit = HitTile(pt);
                if (hit < 0) return 0;
                const StripTile& t = tiles_[static_cast<size_t>(hit)];
                if (t.header) return 0;
                Select(hit, true);
                if (hit >= static_cast<int>(tiles_.size())) return 0;  // the selection rebuilt the tiles
                const RECT r = TileRect(static_cast<size_t>(hit));
                if (mode_ == Mode::Drawer) {
                    // Round 12 phase 12D: a click on a tile adds it (the release on the same tile), a
                    // drag moves it onto the strip, a dock or the desktop; the second press of a double
                    // click adds nothing more.
                    if (msg == WM_LBUTTONDBLCLK) return 0;
                    const RECT desk = DesktopRect(r);
                    if (tiles_[static_cast<size_t>(hit)].desktop && PtInRect(&desk, pt)) {
                        pressDesk_ = true;
                        hotDesk_ = true;
                        pressed_ = hit;
                        SetCapture(hwnd_);
                        Repaint();
                        return 0;
                    }
                    pressAdd_ = true;  // a click candidate until the pointer moves away
                } else if (msg == WM_LBUTTONDBLCLK) {
                    if (onActivate) onActivate(hit);
                    return 0;
                }
                pressed_ = hit;
                pressPt_ = pt;
                grab_ = POINT{pt.x - r.left, pt.y - r.top};
                SetCapture(hwnd_);
                return 0;
            }
            case WM_MOUSEMOVE: {
                const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                if (pressed_ >= 0 && GetCapture() == hwnd_) {
                    if (pressDesk_) {
                        const RECT desk = DesktopRect(TileRect(static_cast<size_t>(pressed_)));
                        const bool in = PtInRect(&desk, pt) != FALSE;
                        if (in != hotDesk_) {
                            hotDesk_ = in;
                            Repaint();
                        }
                        return 0;
                    }
                    if (!dragging_ && std::abs(pt.x - pressPt_.x) <= GetSystemMetrics(SM_CXDRAG) &&
                        std::abs(pt.y - pressPt_.y) <= GetSystemMetrics(SM_CYDRAG)) {
                        return 0;
                    }
                    if (mode_ == Mode::Drawer && !dragging_) {
                        const StripTile& t = tiles_[static_cast<size_t>(pressed_)];
                        if (t.disabled || t.noDrag) return 0;  // not a drag source (a release on it still adds / beeps)
                    }
                    dragging_ = true;
                    pressAdd_ = false;
                    if (mode_ == Mode::Strip) {
                        AutoScroll(pt.x);
                        dragPt_ = pt;
                        slot_ = SlotAt(pt.x);
                        Repaint();
                        return 0;
                    }
                    // The drawer: over the strip, the strip shows where the drop would go; elsewhere
                    // the host (round 12 phase 12D: a dock's marker, the desktop for a script widget).
                    POINT screen = pt;
                    ClientToScreen(hwnd_, &screen);
                    const HWND under = WindowFromPoint(screen);
                    if (peer_ && peer_->Hwnd() && under == peer_->Hwnd()) {
                        if (outside_ && onDragOutsideLeave) onDragOutsideLeave();
                        outside_ = false;
                        peer_->ExternalOver(screen, tiles_[static_cast<size_t>(pressed_)]);
                        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                    } else {
                        if (peer_) peer_->ExternalLeave();
                        const int where = onDragOutside ? onDragOutside(screen, tiles_[static_cast<size_t>(pressed_)].ref) : 0;
                        outside_ = onDragOutside != nullptr;
                        SetCursor(LoadCursorW(nullptr, where ? IDC_ARROW : IDC_NO));
                    }
                    return 0;
                }
                // Hover
                TrackLeave();
                const int hit = HitTile(pt);
                bool desk = false;
                if (mode_ == Mode::Drawer && hit >= 0) {
                    const RECT d = DesktopRect(TileRect(static_cast<size_t>(hit)));
                    desk = tiles_[static_cast<size_t>(hit)].desktop && PtInRect(&d, pt) != FALSE;
                }
                if (hit != hot_ || desk != hotDesk_) {
                    hot_ = hit;
                    hotDesk_ = desk;
                    Repaint();
                }
                return 0;
            }
            case WM_MOUSELEAVE:
                tracking_ = false;
                if (hot_ >= 0 || hotDesk_) {
                    hot_ = -1;
                    hotDesk_ = false;
                    Repaint();
                }
                return 0;
            case WM_LBUTTONUP: {
                if (pressed_ < 0) return 0;
                const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                const int from = pressed_;
                const bool drag = dragging_, clickPress = pressAdd_, deskPress = pressDesk_;
                const bool outside = outside_;
                const int ref = from < static_cast<int>(tiles_.size()) ? tiles_[static_cast<size_t>(from)].ref : -1;
                const bool disabled = from < static_cast<int>(tiles_.size()) && tiles_[static_cast<size_t>(from)].disabled;
                int slot = -1;
                if (drag && mode_ == Mode::Strip) slot = SlotAt(pt.x);
                if (drag && mode_ == Mode::Drawer && peer_) slot = peer_->ExternalDrop();
                bool deskHit = false;
                if (deskPress) {
                    const RECT desk = DesktopRect(TileRect(static_cast<size_t>(from)));
                    deskHit = PtInRect(&desk, pt) != FALSE;
                }
                const bool clickHit = clickPress && HitTile(pt) == from;
                dragging_ = false;
                pressed_ = -1;
                pressAdd_ = false;
                pressDesk_ = false;
                outside_ = false;
                hotDesk_ = deskHit;
                slot_ = -1;
                if (GetCapture() == hwnd_) {
                    releasing_ = true;
                    ReleaseCapture();
                    releasing_ = false;
                }
                Repaint();
                POINT screen = pt;
                ClientToScreen(hwnd_, &screen);
                // Callbacks last: they rebuild the tiles.
                if (deskPress && deskHit && onDesktop) {
                    onDesktop(ref);
                } else if (clickHit && mode_ == Mode::Drawer) {
                    if (disabled) {
                        MessageBeep(MB_ICONWARNING);
                    } else if (onActivate) {
                        onActivate(ref);
                    }
                } else if (drag && mode_ == Mode::Strip && onMove) {
                    onMove(from, slot);
                } else if (drag && mode_ == Mode::Drawer && slot >= 0 && onDropEntry) {
                    onDropEntry(ref, slot);
                } else if (drag && mode_ == Mode::Drawer && outside && slot < 0) {
                    if (!onDropOutside || !onDropOutside(screen, ref)) {
                        if (onDragOutsideLeave) onDragOutsideLeave();
                    }
                }
                return 0;
            }
            case WM_CAPTURECHANGED:
                // Someone else took the mouse (or Esc): the drag is cancelled.
                if (!releasing_ && reinterpret_cast<HWND>(lp) != hwnd_ && (dragging_ || pressed_ >= 0)) CancelDrag();
                return 0;
            case WM_CANCELMODE:
                CancelDrag();
                break;
            default:
                break;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

    StripHost& window_;
    Mode mode_;
    std::wstring baseName_;
    HWND hwnd_ = nullptr;
    SettingsStrip* peer_ = nullptr;  // the drawer: the strip it drops onto
    std::vector<StripTile> tiles_;
    std::vector<int> pos_;     // content px: each strip tile's x
    std::vector<RECT> grid_;   // round 12 phase 12D: content px: each drawer tile's / header's rect
    int content_ = 0;
    int scroll_ = 0;
    bool inLayout_ = false;
    int sel_ = -1;
    int hot_ = -1;
    bool hotDesk_ = false;
    bool tracking_ = false;
    // Pressing / dragging
    int pressed_ = -1;
    bool pressAdd_ = false;    // the drawer: a press on a tile that adds it when released on it (round 12 phase 12D)
    bool pressDesk_ = false;
    POINT pressPt_{};
    POINT grab_{};    // the pointer within the tile at the press
    POINT dragPt_{};  // the pointer now (client)
    bool dragging_ = false;
    bool releasing_ = false;
    int slot_ = -1;
    bool external_ = false;  // a drawer row is dragged over the strip
    bool outside_ = false;   // round 12 phase 12D: the drawer's drag is over the host's area (onDragOutside)
    StripTile ghost_;
    // Fonts: the window's message font (not owned) and those made from it.
    HFONT font_ = nullptr;
    HFONT bold_ = nullptr;
    HFONT small_ = nullptr;
    HFONT smallBold_ = nullptr;
    HFONT letter_ = nullptr;
    // The paint in progress (round 6 phase 6E): when it began, the previews it rendered, and
    // whether one was left for the next paint.
    mutable double paintStart_ = 0.0;
    mutable int paintRenders_ = 0;
    mutable bool previewPending_ = false;
};

// A drawer entry: a built-in kind (0 - 6) or an installed script widget (7 ...).
struct DrawerEntry {
    ItemKind kind = ItemKind::Widget;
    std::wstring id;    // a script widget's id; a built-in's kind name (searched too)
    std::wstring name;
    std::wstring note;
    std::wstring version;
    bool once = false;  // single instance on a dock
};

// Round 12 phase 12D: the drawer's entries, shared by the Contents tab and the drawer popup (it was
// the Contents page's own): the built-in kinds (refs 0 - 6, the retired native weather entry 2
// kept empty so the automation indexes stay), the installed script widgets (7 ...), and the two
// "Apps and folders" entries that open a dialog (refs 1000 / 1001). Refs are what QueryState 230 /
// 252 / 264 / 267 and ItemCommand 863 / 864 take.
namespace DrawerRefs {
inline constexpr int kRunning = 0, kClock = 1, kWeather = 2, kTray = 3, kBell = 4, kRecycleBin = 5, kSeparator = 6, kBuiltIns = 7;
inline constexpr int kApp = 1000, kFolder = 1001;
}  // namespace DrawerRefs

// The filter chips over the grid (All, Built in, Widgets, Apps and folders): the groups shown.
enum class DrawerFilter { All = 0, BuiltIn = 1, Widgets = 2, AppsAndFolders = 3 };

class DrawerCatalog {
public:
    DrawerCatalog() = default;
    ~DrawerCatalog() { DropIcons(); }
    DrawerCatalog(const DrawerCatalog&) = delete;
    DrawerCatalog& operator=(const DrawerCatalog&) = delete;

    // The entries again: the built-ins once, the installed widgets every time (Reload widgets, an
    // installed folder); a widget's note and version are read once per session.
    void Build() {
        using namespace DrawerRefs;
        if (addEntries_.empty()) {
            DrawerEntry app;
            app.kind = ItemKind::App;
            app.id = L"app";
            app.name = L"App or shortcut…";
            app.note = L"Pin apps or shortcuts";
            addEntries_.push_back(app);
            DrawerEntry folder;
            folder.kind = ItemKind::Folder;
            folder.id = L"folder";
            folder.name = L"Folder…";
            folder.note = L"A folder opens as a stack";
            addEntries_.push_back(folder);
        }
        if (entries_.empty()) {
            auto builtIn = [&](ItemKind k, const wchar_t* name, const wchar_t* note, bool once) {
                DrawerEntry e;
                e.kind = k;
                e.id = ToString(k);
                e.name = name;
                e.note = note;
                e.once = once;
                entries_.push_back(std::move(e));
            };
            // DrawerRefs order
            builtIn(ItemKind::Running, L"Running apps", L"Open apps that are not pinned", true);
            builtIn(ItemKind::Clock, L"Clock", L"Any number, each with its own format and zone", false);
            entries_.emplace_back();  // retired native weather entry: preserve automation indexes
            builtIn(ItemKind::Tray, L"System tray", L"The notification area icons as a grid", true);
            builtIn(ItemKind::Notifications, L"Notification centre", L"A bell with the unread count; a click lists the notifications", true);
            builtIn(ItemKind::RecycleBin, L"Recycle Bin", L"Drop files on it to recycle them", true);
            builtIn(ItemKind::Separator, L"Separator", L"A thin line between groups of icons", false);
        }
        entries_.resize(kBuiltIns);
        for (const Widgets::CatalogEntry& w : Widgets::Catalog()) {
            auto it = packages_.find(w.id);
            if (it == packages_.end()) {
                std::pair<std::wstring, std::wstring> info;
                std::wstring error;
                if (const auto pkg = Widgets::Load(w.id, error)) {
                    std::wstring d = pkg->manifest.description;
                    if (const size_t nl = d.find_first_of(L"\r\n"); nl != std::wstring::npos) d.resize(nl);
                    info.first = d.empty() ? w.id : d;
                    info.second = pkg->manifest.version;
                } else {
                    info.first = L"Not readable: " + error;
                }
                it = packages_.emplace(w.id, std::move(info)).first;
            }
            DrawerEntry e;
            e.kind = ItemKind::Widget;
            e.id = w.id;
            e.name = w.name;
            e.note = it->second.first;
            e.version = it->second.second;
            entries_.push_back(std::move(e));
        }
    }

    // The entry behind ref `ref` (null: none, the retired weather entry, or before Build).
    const DrawerEntry* EntryOf(int ref) const {
        using namespace DrawerRefs;
        if (ref == kWeather) return nullptr;
        if (ref == kApp || ref == kFolder) {
            const size_t k = static_cast<size_t>(ref - kApp);
            return k < addEntries_.size() ? &addEntries_[k] : nullptr;
        }
        return ref >= 0 && ref < static_cast<int>(entries_.size()) ? &entries_[static_cast<size_t>(ref)] : nullptr;
    }
    size_t EntryCount() const { return entries_.size(); }

    static bool Matches(const DrawerEntry& e, const std::wstring& filter) {
        return filter.empty() || ContainsNoCase(e.name, filter) || ContainsNoCase(e.note, filter) || ContainsNoCase(e.id, filter) ||
               ContainsNoCase(e.version, filter);
    }

    // The tile of entry `ref` for `dock` ("On the dock": a kind it can have once and has). Icons at
    // `iconPx` (the catalog keeps them; a new size makes them again).
    StripTile Tile(int ref, const DockConfig& dock, int iconPx) {
        if (iconPx != iconPx_) {
            DropIcons();
            iconPx_ = iconPx;
        }
        const DrawerEntry& e = *EntryOf(ref);
        StripTile t;
        t.ref = ref;
        t.name = e.name;
        t.version = e.version;
        t.disabled = e.once && dock.Has(e.kind);
        t.note = t.disabled ? L"On the dock" : e.note;
        t.tint = TintOf(e.kind, e.id);
        t.letter = LetterOf(e.kind, e.name);
        t.desktop = e.kind == ItemKind::Widget;  // round 6 phase 6B: Add to the desktop
        if (e.kind == ItemKind::Widget) t.previewId = e.id;  // round 6 phase 6E: its live preview
        switch (e.kind) {
            case ItemKind::Running: t.shape = StripTile::Shape::Running; break;
            case ItemKind::Separator: t.shape = StripTile::Shape::Separator; break;
            case ItemKind::RecycleBin:
                if (!binIcon_) {
                    DockItemEntry bin;
                    bin.kind = ToString(ItemKind::RecycleBin);
                    binIcon_ = SettingsItemIcon(nullptr, bin, iconPx);
                }
                t.icon = binIcon_;
                t.shape = binIcon_ ? StripTile::Shape::Icon : StripTile::Shape::Letter;
                break;
            case ItemKind::App:
            case ItemKind::Folder: {
                // Round 8 phase 8B: "App or shortcut…" / "Folder…" (the shell's stock icons; a dialog,
                // so a click only, no drag).
                HICON& icon = e.kind == ItemKind::App ? appIcon_ : folderIcon_;
                if (!icon) {
                    SHSTOCKICONINFO sii{sizeof(sii)};
                    if (SUCCEEDED(SHGetStockIconInfo(e.kind == ItemKind::App ? SIID_APPLICATION : SIID_FOLDER,
                                                     SHGSI_ICON | SHGSI_LARGEICON, &sii))) {
                        icon = sii.hIcon;
                    }
                }
                t.icon = icon;
                t.shape = icon ? StripTile::Shape::Icon : StripTile::Shape::Letter;
                t.noDrag = true;
                break;
            }
            default: t.shape = StripTile::Shape::Letter; break;
        }
        return t;
    }

    // The grid's tiles for a search and a chip, in groups with their headers (Apps and folders,
    // Built in, Script widgets); `select` gets the index of `keep`'s tile (-1 none), `groupRows`
    // the tiles per group as filtered (QueryState 278).
    std::vector<StripTile> Tiles(const DockConfig& dock, const std::wstring& filter, DrawerFilter chip, int keep, int iconPx,
                                 int* select, int groupRows[3]) {
        using namespace DrawerRefs;
        std::vector<StripTile> tiles;
        if (select) *select = -1;
        auto group = [&](size_t index, const wchar_t* title, const std::vector<int>& refs, bool shown) {
            int rows = 0;
            for (int ref : refs) {
                const DrawerEntry* e = EntryOf(ref);
                if (!shown || !e || !Matches(*e, filter)) continue;
                if (rows++ == 0) {
                    StripTile h;
                    h.header = true;
                    h.name = title;
                    tiles.push_back(std::move(h));
                }
                if (ref == keep && select) *select = static_cast<int>(tiles.size());
                tiles.push_back(Tile(ref, dock, iconPx));
            }
            if (groupRows) groupRows[index] = rows;
        };
        std::vector<int> builtIn, scripts;
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (i == static_cast<size_t>(kWeather)) continue;  // the weather script is listed with the other scripts
            (i < static_cast<size_t>(kBuiltIns) ? builtIn : scripts).push_back(static_cast<int>(i));
        }
        group(0, L"APPS AND FOLDERS", {kApp, kFolder}, chip == DrawerFilter::All || chip == DrawerFilter::AppsAndFolders);
        group(1, L"BUILT IN", builtIn, chip == DrawerFilter::All || chip == DrawerFilter::BuiltIn);
        group(2, L"SCRIPT WIDGETS", scripts, chip == DrawerFilter::All || chip == DrawerFilter::Widgets);
        return tiles;
    }

    // A new item of entry `ref` for `dock` (nullopt: a dialog entry, none, or a kind the dock can
    // have once and has: a warning beep). `tray`: it is a tray item (the takeover goes on with it,
    // as the dock menu's Add > Tray does).
    std::optional<DockItemEntry> NewEntry(int ref, const DockConfig& dock, bool* tray = nullptr) const {
        if (tray) *tray = false;
        if (ref == DrawerRefs::kWeather || ref < 0 || ref >= static_cast<int>(entries_.size())) return std::nullopt;
        const DrawerEntry& de = entries_[static_cast<size_t>(ref)];
        if (de.once && dock.Has(de.kind)) {
            MessageBeep(MB_ICONWARNING);
            return std::nullopt;
        }
        DockItemEntry e;
        e.kind = ToString(de.kind);
        if (de.kind == ItemKind::Widget) e.id = de.id;
        if (tray) *tray = de.kind == ItemKind::Tray;
        return e;
    }

private:
    void DropIcons() {
        for (HICON* i : {&binIcon_, &appIcon_, &folderIcon_}) {
            if (*i) DestroyIcon(*i);
            *i = nullptr;
        }
    }

    std::vector<DrawerEntry> entries_;
    std::vector<DrawerEntry> addEntries_;  // App or shortcut… (kApp), Folder… (kFolder)
    std::map<std::wstring, std::pair<std::wstring, std::wstring>> packages_;  // widget id -> note, version
    HICON binIcon_ = nullptr;
    HICON appIcon_ = nullptr;
    HICON folderIcon_ = nullptr;
    int iconPx_ = 0;
};

}  // namespace StripUi
