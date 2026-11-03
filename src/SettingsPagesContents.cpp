// The Dock window's Contents tab (round 5 phase C; it replaces the Dock contents list): the item
// strip (the dock's items as tiles in dock order: drag to reorder, keyboard moves), the widget
// drawer on the right (built-in kinds and the installed script widgets: drag a row onto the strip
// or press its Add), the inspector of the selected item under the strip and the add buttons.
//
// The strip, the drawer and the inspector's identity line are three instances of one custom
// control (WinDock.SettingsStrip, Mode Strip / Drawer / Info): GDI, double-buffered, palette
// colours (dark / light, high contrast), DPI aware through the window's Px, no timers. A drag from
// the drawer to the strip uses plain mouse capture: the drawer keeps the capture and asks the strip
// under the pointer (WindowFromPoint) to show its insertion marker; no OLE drag and drop.

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <winrt/base.h>

#include <algorithm>
#include <cwctype>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "IconUtil.h"
#include "SettingsForm.h"
#include "SettingsIds.h"
#include "WidgetPackage.h"

namespace {

using namespace SettingsIds;

// --- GDI helpers -----------------------------------------------------------------------------

COLORREF Mix(COLORREF a, COLORREF b, int pa) {
    auto ch = [&](BYTE x, BYTE y) { return static_cast<BYTE>((x * pa + y * (100 - pa)) / 100); };
    return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)), ch(GetBValue(a), GetBValue(b)));
}

int Luma(COLORREF c) { return (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000; }

COLORREF Rgb(uint32_t rgb) { return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF); }

void Fill(HDC dc, const RECT& r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

// A rounded rectangle filled with `fill`, outlined with `border` (1 px).
void Round(HDC dc, const RECT& r, COLORREF fill, COLORREF border, int radius) {
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

void Frame(HDC dc, const RECT& r, COLORREF border, int radius, int style = PS_SOLID) {
    HPEN p = CreatePen(style, 1, border);
    HGDIOBJ op = SelectObject(dc, p);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(p);
}

void Text(HDC dc, const std::wstring& s, RECT r, HFONT font, COLORREF color, UINT format) {
    HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, color);
    DrawTextW(dc, s.c_str(), static_cast<int>(s.size()), &r, format | DT_NOPREFIX | DT_SINGLELINE);
    SelectObject(dc, old);
}

int TextWidth(HDC dc, const std::wstring& s, HFONT font) {
    HGDIOBJ old = SelectObject(dc, font);
    SIZE sz{};
    GetTextExtentPoint32W(dc, s.c_str(), static_cast<int>(s.size()), &sz);
    SelectObject(dc, old);
    return sz.cx;
}

bool ContainsNoCase(const std::wstring& hay, const std::wstring& needle) {
    if (needle.empty()) return true;
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](wchar_t a, wchar_t b) { return std::towlower(a) == std::towlower(b); });
    return it != hay.end();
}

// --- what an item is called ------------------------------------------------------------------

std::wstring StemOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const size_t dot = name.rfind(L'.');
    if (dot != std::wstring::npos && dot > 0) name.resize(dot);
    return name;
}

const wchar_t* KindLabel(ItemKind k) {
    switch (k) {
        case ItemKind::App: return L"App";
        case ItemKind::Folder: return L"Folder";
        case ItemKind::Separator: return L"Separator";
        case ItemKind::RecycleBin: return L"Recycle Bin";
        case ItemKind::Running: return L"Running apps";
        case ItemKind::Clock: return L"Clock";
        case ItemKind::Weather: return L"Weather";
        case ItemKind::Widget: return L"Widget";
        case ItemKind::Group: return L"Group";
        case ItemKind::Tray: return L"Tray";
        case ItemKind::TrayApp: return L"Tray icon";
        case ItemKind::Notifications: return L"Notification centre";
        case ItemKind::Unknown:
        default: return L"Unknown";
    }
}

// Name and details of an item, as the Dock contents list's columns showed them (pin: its resolved
// twin from host.pins, or null).
void Describe(const Config& c, const DockItemEntry& e, const AppIdentity::ResolvedPin* pin, bool firstRunning,
              std::wstring& name, std::wstring& details) {
    switch (e.Kind()) {
        case ItemKind::App:
        case ItemKind::Folder:
            name = pin ? pin->displayName : std::wstring();
            if (name.empty()) name = !e.path.empty() ? StemOf(e.path) : e.aumid;
            if (pin && pin->missing) name += L" (missing)";
            details = pin && !pin->targetPath.empty() ? pin->targetPath : !e.path.empty() ? e.path : e.aumid;
            break;
        case ItemKind::Separator:
            name = L"Separator";
            details = L"A thin line between groups of icons";
            break;
        case ItemKind::RecycleBin:
            name = pin && !pin->displayName.empty() ? pin->displayName : L"Recycle Bin";
            details = L"Drop files on it to recycle them";
            break;
        case ItemKind::Group: {
            name = e.name.empty() ? std::wstring(L"Group") : e.name;
            details = std::to_wstring(e.members.size()) + (e.members.size() == 1 ? L" shortcut" : L" shortcuts");
            for (size_t i = 0; i < e.members.size() && i < 6; ++i) {
                std::wstring m = pin && i < pin->members.size() ? pin->members[i].displayName : std::wstring();
                if (m.empty()) m = !e.members[i].path.empty() ? StemOf(e.members[i].path) : e.members[i].aumid;
                details += (i == 0 ? L": " : L", ") + m;
            }
            if (e.members.size() > 6) details += L", …";
            break;
        }
        case ItemKind::Running:
            name = L"Running apps";
            details = firstRunning ? L"Open apps that are not pinned" : L"Not shown: only the first running apps item is used";
            break;
        case ItemKind::Clock: {
            const ClockConfig clock = ClockOf(c, e);
            name = clock.label.empty() ? std::wstring(L"Clock") : L"Clock · " + clock.label;
            details = ClockDetail(clock);
            break;
        }
        case ItemKind::Weather: {
            const WeatherConfig w = WeatherOf(c, e);
            const std::wstring place = !w.label.empty() ? w.label : w.location;
            name = place.empty() ? std::wstring(L"Weather") : L"Weather · " + place;
            details = WeatherDetail(w);
            break;
        }
        case ItemKind::Widget: {
            name = Widgets::DisplayName(e.id);
            std::wstring error;
            const auto pkg = Widgets::Load(e.id, error);
            details = !pkg ? L"Not installed: " + e.id
                           : e.id + (pkg->manifest.version.empty() ? std::wstring() : L" " + pkg->manifest.version) +
                                 (pkg->source == Widgets::Package::Source::BuiltIn ? L" (built-in)" : L" (" + pkg->folder + L")");
            break;
        }
        case ItemKind::Tray:
            name = L"Tray";
            details = std::to_wstring(e.rows) + (e.rows == 1 ? L" row, " : L" rows, ") + std::to_wstring(e.columns) + L" columns, " +
                      std::to_wstring(e.trayIconSize) + L" px" +
                      (c.tray.enabled ? std::wstring() : std::wstring(L" (the tray is off: Global > Tray icons)"));
            if (!e.hidden.empty()) details += L", " + std::to_wstring(e.hidden.size()) + L" hidden";
            break;
        case ItemKind::TrayApp:
            name = L"Tray icon";
            details = e.id;
            break;
        case ItemKind::Notifications:
            name = L"Notification centre";
            details = L"A bell with the unread count; a click lists the notifications";
            break;
        case ItemKind::Unknown:
        default:
            name = e.kind.empty() ? L"(no kind)" : e.kind;
            details = L"Not supported by this version (kept in the file)";
            break;
    }
}

// The letter tile colours (the design's tints); a script widget's by its id.
COLORREF TintOf(ItemKind k, const std::wstring& id) {
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

wchar_t LetterOf(ItemKind k, const std::wstring& name) {
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

    SettingsStrip(SettingsWindow& window, Mode mode, std::wstring name) : window_(window), mode_(mode), baseName_(std::move(name)) {}
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
        hotAdd_ = false;
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
    // The insertion slot the marker shows (-1: no drag over the strip).
    int Marker() const { return (dragging_ || external_) ? slot_ : -1; }

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
    std::function<void(int ref)> onActivate;          // Enter, a double click; the drawer: Add
    std::function<void(int from, int slot)> onMove;   // strip: move item `from` before slot `slot`
    std::function<void(int ref)> onDelete;            // strip: Delete
    std::function<void(int ref, int slot)> onDropEntry;  // drawer: entry `ref` dropped on the strip at `slot`

private:
    // DIPs
    static constexpr int kTileW = 84, kSepW = 28, kRunningW = 150, kTileH = 84, kIcon = 44, kGap = 2, kPad = 10;
    static constexpr int kRowH = 48, kHeaderH = 26, kRowGap = 6, kAddW = 48;

    int Px(int dip) const { return window_.Px(dip); }
    void Repaint() const {
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }

    int TileW(const StripTile& t) const {
        return Px(t.shape == StripTile::Shape::Separator ? kSepW : t.shape == StripTile::Shape::Running ? kRunningW : kTileW);
    }
    int RowH(const StripTile& t) const { return Px(t.header ? kHeaderH : kRowH); }
    SIZE Client() const {
        RECT rc{};
        if (hwnd_) GetClientRect(hwnd_, &rc);
        return SIZE{rc.right, rc.bottom};
    }
    bool Horizontal() const { return mode_ == Mode::Strip; }

    // Tile / row i in client px (scrolled).
    RECT TileRect(size_t i) const {
        const SIZE c = Client();
        if (i >= pos_.size()) return RECT{};
        const StripTile& t = tiles_[i];
        if (Horizontal()) {
            const int top = std::max(0, static_cast<int>(c.cy - Px(kTileH)) / 2);
            const int x = pos_[i] - scroll_;
            return RECT{x, top, x + TileW(t), top + Px(kTileH)};
        }
        const int y = pos_[i] - scroll_;
        return RECT{0, y, c.cx, y + RowH(t)};
    }

    // Positions, the content length and the scroll bar.
    void Layout() {
        if (!hwnd_ || mode_ == Mode::Info || inLayout_) return;
        inLayout_ = true;
        pos_.assign(tiles_.size(), 0);
        int at = Horizontal() ? Px(kPad) : 0;
        for (size_t i = 0; i < tiles_.size(); ++i) {
            pos_[i] = at;
            at += Horizontal() ? TileW(tiles_[i]) + Px(kGap) : RowH(tiles_[i]) + (tiles_[i].header ? 0 : Px(kRowGap));
        }
        content_ = Horizontal() ? at - Px(kGap) + Px(kPad) : at;
        for (int pass = 0; pass < 2; ++pass) {
            // Twice: showing / hiding the bar changes the view.
            const SIZE c = Client();
            const int view = Horizontal() ? c.cx : c.cy;
            scroll_ = std::clamp(scroll_, 0, std::max(0, content_ - view));
            SCROLLINFO si{sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS, 0, std::max(0, content_ - 1), static_cast<UINT>(std::max(0, view)),
                          scroll_, 0};
            SetScrollInfo(hwnd_, Horizontal() ? SB_HORZ : SB_VERT, &si, TRUE);
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
        if (!hwnd_ || index < 0 || index >= static_cast<int>(pos_.size())) return;
        const StripTile& t = tiles_[static_cast<size_t>(index)];
        const int start = pos_[static_cast<size_t>(index)] - (Horizontal() ? Px(kPad) : 0);
        const int end = pos_[static_cast<size_t>(index)] + (Horizontal() ? TileW(t) + Px(kPad) : RowH(t));
        // A drawer row: its group header too when it is the first one.
        int top = start;
        if (!Horizontal() && index > 0 && tiles_[static_cast<size_t>(index) - 1].header) top = pos_[static_cast<size_t>(index) - 1];
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

    RECT AddRect(const RECT& row) const {
        const int cy = (row.top + row.bottom) / 2;
        return RECT{row.right - Px(10) - Px(kAddW), cy - Px(13), row.right - Px(10), cy + Px(13)};
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
        dragging_ = false;
        pressed_ = -1;
        pressAdd_ = false;
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

    // A drawer row (or group header) in `r`.
    void PaintRow(HDC dc, const StripTile& t, const RECT& r, bool focused, bool hot, bool addHot, const Colors& c) const {
        SetBkMode(dc, TRANSPARENT);
        if (t.header) {
            Text(dc, t.name, RECT{r.left + Px(2), r.top + Px(6), r.right, r.bottom}, SmallBold(), c.secondary,
                 DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
            return;
        }
        const bool live = !t.disabled;
        const COLORREF fill = Mix(c.text, c.layer, live && hot ? 10 : 5);
        Round(dc, r, fill, Mix(c.text, c.layer, live && hot ? 24 : 13), Px(6) * 2);
        const int cy = (r.top + r.bottom) / 2;
        const RECT box{r.left + Px(10), cy - Px(15), r.left + Px(10) + Px(30), cy - Px(15) + Px(30)};
        PaintGlyph(dc, t, box, c, fill);
        const RECT add = AddRect(r);
        Round(dc, add, Mix(c.text, c.layer, live && addHot ? 20 : 11), Mix(c.text, c.layer, live ? 30 : 16), Px(4) * 2);
        Text(dc, L"Add", add, Small(), live ? c.text : c.disabled, DT_CENTER | DT_VCENTER);
        const int textL = r.left + Px(50);
        int nameR = add.left - Px(8);
        if (!t.version.empty()) {
            const int vw = TextWidth(dc, t.version, Small());
            Text(dc, t.version, RECT{nameR - vw, r.top + Px(6), nameR, r.top + Px(25)}, Small(), live ? c.secondary : c.disabled,
                 DT_RIGHT | DT_VCENTER);
            nameR -= vw + Px(6);
        }
        Text(dc, t.name, RECT{textL, r.top + Px(6), nameR, r.top + Px(25)}, Bold(), live ? c.text : c.disabled,
             DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        Text(dc, t.note, RECT{textL, r.top + Px(25), add.left - Px(8), r.bottom - Px(5)}, Small(), live ? c.secondary : c.disabled,
             DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
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

    void Paint(HDC dc) const {
        const SIZE client = Client();
        const RECT all{0, 0, client.cx, client.cy};
        const Colors c = Palette();
        const bool focus = GetFocus() == hwnd_;
        if (mode_ == Mode::Info) {
            Fill(dc, all, c.layer);
            SetBkMode(dc, TRANSPARENT);
            if (tiles_.empty()) {
                Text(dc, L"Select an item on the strip", all, Base(), c.secondary, DT_LEFT | DT_VCENTER);
                return;
            }
            const StripTile& t = tiles_[0];
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
            Text(dc, L"No widget matches the search.", RECT{Px(2), 0, client.cx, Px(kHeaderH)}, Small(), c.secondary, DT_LEFT | DT_VCENTER);
        }
        const bool cues = !(SendMessageW(hwnd_, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
        for (size_t i = 0; i < tiles_.size(); ++i) {
            const RECT r = TileRect(i);
            if (r.bottom < 0 || r.top > client.cy) continue;
            const bool isHot = static_cast<int>(i) == hot_;
            PaintRow(dc, tiles_[i], r, focus && cues && static_cast<int>(i) == sel_, isHot, isHot && hotAdd_, c);
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
        const int line = Horizontal() ? Px(kTileW) : Px(kRowH);
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
        switch (vk) {
            case VK_UP: Select(sel_ < 0 ? step(-1, 1) : step(sel_, -1), true); break;
            case VK_DOWN: Select(sel_ < 0 ? step(-1, 1) : step(sel_, 1), true); break;
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
                const int unit = Horizontal() ? Px(kTileW) : Px(kRowH + kRowGap);
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
                    const RECT add = AddRect(r);
                    if (PtInRect(&add, pt)) {
                        if (tiles_[static_cast<size_t>(hit)].disabled) {
                            MessageBeep(MB_ICONWARNING);
                            return 0;
                        }
                        pressAdd_ = true;
                        hotAdd_ = true;
                        pressed_ = hit;
                        SetCapture(hwnd_);
                        Repaint();
                        return 0;
                    }
                    if (msg == WM_LBUTTONDBLCLK) {
                        if (!tiles_[static_cast<size_t>(hit)].disabled && onActivate) onActivate(tiles_[static_cast<size_t>(hit)].ref);
                        return 0;
                    }
                    if (tiles_[static_cast<size_t>(hit)].disabled) return 0;  // not a drag source
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
                    if (pressAdd_) {
                        const RECT add = AddRect(TileRect(static_cast<size_t>(pressed_)));
                        const bool in = PtInRect(&add, pt) != FALSE;
                        if (in != hotAdd_) {
                            hotAdd_ = in;
                            Repaint();
                        }
                        return 0;
                    }
                    if (!dragging_ && std::abs(pt.x - pressPt_.x) <= GetSystemMetrics(SM_CXDRAG) &&
                        std::abs(pt.y - pressPt_.y) <= GetSystemMetrics(SM_CYDRAG)) {
                        return 0;
                    }
                    dragging_ = true;
                    if (mode_ == Mode::Strip) {
                        AutoScroll(pt.x);
                        dragPt_ = pt;
                        slot_ = SlotAt(pt.x);
                        Repaint();
                        return 0;
                    }
                    // The drawer: over the strip, the strip shows where the drop would go.
                    POINT screen = pt;
                    ClientToScreen(hwnd_, &screen);
                    const HWND under = WindowFromPoint(screen);
                    if (peer_ && peer_->Hwnd() && under == peer_->Hwnd()) {
                        peer_->ExternalOver(screen, tiles_[static_cast<size_t>(pressed_)]);
                        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                    } else {
                        if (peer_) peer_->ExternalLeave();
                        SetCursor(LoadCursorW(nullptr, IDC_NO));
                    }
                    return 0;
                }
                // Hover
                TrackLeave();
                const int hit = HitTile(pt);
                bool add = false;
                if (mode_ == Mode::Drawer && hit >= 0) {
                    const RECT a = AddRect(TileRect(static_cast<size_t>(hit)));
                    add = PtInRect(&a, pt) != FALSE;
                }
                if (hit != hot_ || add != hotAdd_) {
                    hot_ = hit;
                    hotAdd_ = add;
                    Repaint();
                }
                return 0;
            }
            case WM_MOUSELEAVE:
                tracking_ = false;
                if (hot_ >= 0 || hotAdd_) {
                    hot_ = -1;
                    hotAdd_ = false;
                    Repaint();
                }
                return 0;
            case WM_LBUTTONUP: {
                if (pressed_ < 0) return 0;
                const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                const int from = pressed_;
                const bool drag = dragging_, addPress = pressAdd_;
                const int ref = from < static_cast<int>(tiles_.size()) ? tiles_[static_cast<size_t>(from)].ref : -1;
                int slot = -1;
                if (drag && mode_ == Mode::Strip) slot = SlotAt(pt.x);
                if (drag && mode_ == Mode::Drawer && peer_) slot = peer_->ExternalDrop();
                bool addHit = false;
                if (addPress) {
                    const RECT add = AddRect(TileRect(static_cast<size_t>(from)));
                    addHit = PtInRect(&add, pt) != FALSE;
                }
                dragging_ = false;
                pressed_ = -1;
                pressAdd_ = false;
                slot_ = -1;
                hotAdd_ = addHit;
                if (GetCapture() == hwnd_) {
                    releasing_ = true;
                    ReleaseCapture();
                    releasing_ = false;
                }
                Repaint();
                // Callbacks last: they rebuild the tiles.
                if (addPress && addHit && onActivate) {
                    onActivate(ref);
                } else if (drag && mode_ == Mode::Strip && onMove) {
                    onMove(from, slot);
                } else if (drag && mode_ == Mode::Drawer && slot >= 0 && onDropEntry) {
                    onDropEntry(ref, slot);
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

    SettingsWindow& window_;
    Mode mode_;
    std::wstring baseName_;
    HWND hwnd_ = nullptr;
    SettingsStrip* peer_ = nullptr;  // the drawer: the strip it drops onto
    std::vector<StripTile> tiles_;
    std::vector<int> pos_;  // content px: each tile's x (strip) / row's y (drawer)
    int content_ = 0;
    int scroll_ = 0;
    bool inLayout_ = false;
    int sel_ = -1;
    int hot_ = -1;
    bool hotAdd_ = false;
    bool tracking_ = false;
    // Pressing / dragging
    int pressed_ = -1;
    bool pressAdd_ = false;
    POINT pressPt_{};
    POINT grab_{};    // the pointer within the tile at the press
    POINT dragPt_{};  // the pointer now (client)
    bool dragging_ = false;
    bool releasing_ = false;
    int slot_ = -1;
    bool external_ = false;  // a drawer row is dragged over the strip
    StripTile ghost_;
    // Fonts: the window's message font (not owned) and those made from it.
    HFONT font_ = nullptr;
    HFONT bold_ = nullptr;
    HFONT small_ = nullptr;
    HFONT smallBold_ = nullptr;
    HFONT letter_ = nullptr;
};

// --- the page --------------------------------------------------------------------------------

// The drawer's state is the session's (not saved): shown at first.
bool g_drawerOpen = true;

constexpr int kDrawerW = 300;  // DIPs
constexpr int kInspectorTop = 278;

// A drawer entry: a built-in kind (0 - 6) or an installed script widget (7 ...).
struct DrawerEntry {
    ItemKind kind = ItemKind::Widget;
    std::wstring id;    // a script widget's id; a built-in's kind name (searched too)
    std::wstring name;
    std::wstring note;
    std::wstring version;
    bool once = false;  // single instance on a dock
};

class ContentsPage : public FormPage {
public:
    explicit ContentsPage(SettingsWindow& w)
        : FormPage(w, L"Contents", L'\xE718'),
          strip_(w, SettingsStrip::Mode::Strip, L"Items on the dock"),
          drawer_(w, SettingsStrip::Mode::Drawer, L"Widget drawer"),
          info_(w, SettingsStrip::Mode::Info, L"Selected item") {}
    ~ContentsPage() override {
        for (auto& entry : icons_) {
            if (entry.second.icon) DestroyIcon(entry.second.icon);
        }
        for (HICON i : {infoIcon_, binIcon_}) {
            if (i) DestroyIcon(i);
        }
    }

protected:
    void Build() override {
        SettingsStrip::Register();
        const int W = Width(), L = kMargin, R = Right(), dx = W - kDrawerW;
        Add(kPageTitle, CtlKind::Section, WC_STATICW, L"Items on the dock", SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE, RECT{L, 12, R, 36});
        Add(kContentsNote, CtlKind::Note, WC_STATICW,
            L"Drag to reorder. Drop a widget from the drawer anywhere on the strip. Each copy keeps its own settings.",
            SS_LEFT | SS_NOPREFIX, RECT{L, 38, R, 72});
        Add(kContentsStrip, CtlKind::Custom, SettingsStrip::kClass, L"Items on the dock", WS_TABSTOP, RECT{L, 76, R, 194});
        y_ = 200;
        Button(kPinAdd, L"Add app…", L, 96);
        Button(kPinAddFolder, L"Add folder…", L + 104, 104);
        Button(kPinAddSeparator, L"Add separator", L + 216, 112);
        Button(kPinAddRecycleBin, L"Add Recycle Bin", L + 336, 124);
        y_ = 236;
        Check(kLockDock, kLockDockCaption, L"Lock the dock (no dragging, no pinning by dropping files)", L);
        Button(kContentsDrawerToggle, g_drawerOpen ? L"Hide widget drawer" : L"Show widget drawer", R - 150, 150);
        // The inspector (its frame is painted by PaintBackground; FlowInspector places the buttons).
        Add(kContentsInfo, CtlKind::Custom, SettingsStrip::kClass, L"Selected item", 0,
            RECT{L + 14, kInspectorTop + 10, R - 14 - 212, kInspectorTop + 46});
        y_ = kInspectorTop + 12;
        Button(kPinUp, L"◀ Move left", R - 14 - 206, 100);
        Button(kPinDown, L"Move right ▶", R - 14 - 100, 100);
        y_ = kInspectorTop + 50;
        Button(kPinSettings, L"Open settings in its own window", L + 14, 196);
        Button(kPinChooseIcon, L"Choose icon…", L + 216, 92);
        Button(kPinChooseImage, L"Choose image…", L + 314, 102);
        Button(kPinResetIcon, L"Reset icon", L + 422, 80);
        Button(kPinRemove, L"Remove from dock", R - 14 - 128, 128);
        // The widget drawer.
        Add(kDrawerTitle, CtlKind::Section, WC_STATICW, L"Widget drawer", SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE,
            RECT{dx + 16, 12, dx + 132, 36});
        Add(kDrawerHint, CtlKind::Note, WC_STATICW, L"drag onto the strip", SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE,
            RECT{dx + 132, 12, W - 16, 36});
        y_ = 38;
        EditBox(kDrawerSearch, dx + 16, kDrawerW - 32, 80);
        Add(kDrawerList, CtlKind::Custom, SettingsStrip::kClass, L"Widget drawer", WS_TABSTOP, RECT{dx + 16, 74, W - 16, 378});
        // Two lines: the text does not fit the drawer's width on one.
        Add(kDrawerLibraryLink, CtlKind::Link, WC_LINK, L"<a>Manage installed widgets in Global > Widget library</a>", WS_TABSTOP,
            RECT{dx + 16, 386, W - 16, 422});
    }

    void Created() override {
        strip_.Attach(Item(kContentsStrip));
        drawer_.Attach(Item(kDrawerList));
        info_.Attach(Item(kContentsInfo));
        drawer_.SetPeer(&strip_);
        strip_.onSelect = [this](int item) {
            sel_ = item;
            UpdateInspector();
        };
        strip_.onActivate = [this](int item) { OpenItemSettings(item); };
        strip_.onMove = [this](int from, int slot) { MoveItem(from, slot); };
        strip_.onDelete = [this](int item) { RemoveItem(item); };
        drawer_.onActivate = [this](int entry) { AddEntry(entry); };
        drawer_.onDropEntry = [this](int entry, int slot) { InsertEntry(entry, slot); };
        if (HWND search = Item(kDrawerSearch)) SendMessageW(search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search widgets"));
        ApplyDrawer();
    }

    void Load() override {
        SetCheck(kLockDock, Dock().locked);
        BuildEntries();
        FillStrip(sel_);
        FillDrawer();
    }

    void OnStyleChanged() override {
        if (Window().Px(44) != iconPx_) {
            // Every icon again at the new size.
            iconPx_ = Window().Px(44);
            for (auto& entry : icons_) {
                if (entry.second.icon) DestroyIcon(entry.second.icon);
            }
            icons_.clear();
            for (HICON* i : {&infoIcon_, &binIcon_}) {
                if (*i) DestroyIcon(*i);
                *i = nullptr;
            }
            infoKey_.clear();
            FillStrip(sel_);
            FillDrawer();
        }
        strip_.StyleChanged();
        drawer_.StyleChanged();
        info_.StyleChanged();
        if (Hwnd()) RedrawWindow(Hwnd(), nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE);
    }

    void PaintBackground(HDC dc) override {
        const DarkMode::Palette& p = Window().Colors();
        const int s = ScrollPx();
        const COLORREF hair = Mix(p.text, p.layer, 16);
        // The inspector's frame.
        const RECT f{Window().Px(kMargin), Window().Px(kInspectorTop) - s, Window().Px(Right()), Window().Px(inspectorBottom_) - s};
        Frame(dc, f, hair, Window().Px(8) * 2);
        // The drawer's edge.
        if (g_drawerOpen) {
            RECT rc{};
            GetClientRect(Hwnd(), &rc);
            const int x = Window().Px(Width() - kDrawerW);
            Fill(dc, RECT{x, 0, x + std::max(1, Window().Px(1)), rc.bottom}, Mix(p.text, p.layer, 12));
        }
    }

    bool Query(int code, LPARAM arg, LRESULT& out) override {
        POINT pt{};
        switch (code) {
            case 229:  // a strip tile's centre (screen px) for item `arg`
                out = strip_.CentreOf(static_cast<int>(arg), pt) ? static_cast<LRESULT>(MAKELONG(static_cast<WORD>(pt.x), static_cast<WORD>(pt.y))) : -1;
                return true;
            case 230:  // a drawer row's centre for entry `arg`
                out = g_drawerOpen && drawer_.CentreOf(static_cast<int>(arg), pt)
                          ? static_cast<LRESULT>(MAKELONG(static_cast<WORD>(pt.x), static_cast<WORD>(pt.y)))
                          : -1;
                return true;
            case 231:  // the strip's selected item (-1 none)
                out = strip_.Selected();
                return true;
            case 232:  // the drawer is shown
                out = g_drawerOpen ? 1 : 0;
                return true;
            case 233:  // the strip's insertion marker (the slot a drop lands in; -1 none)
                out = strip_.Marker();
                return true;
            default:
                return false;
        }
    }

    LRESULT OnNotify(const NMHDR& nm) override {
        if (static_cast<int>(nm.idFrom) == kDrawerLibraryLink && (nm.code == NM_CLICK || nm.code == NM_RETURN)) {
            if (Host().openGlobal) Host().openGlobal(kPageWidgets);
        }
        return 0;
    }

    void OnCommand(int id, int code, HWND) override {
        if (id == kDrawerSearch) {
            if (code == EN_CHANGE) {
                filter_ = GetText(kDrawerSearch);
                FillDrawer();
            }
            return;
        }
        if (code != BN_CLICKED) return;
        DockConfig& dock = Dock();
        const int n = static_cast<int>(dock.items.size());
        const bool valid = sel_ >= 0 && sel_ < n;
        switch (id) {
            case kPinAdd: AddFiles(); break;
            case kPinAddFolder: AddFolder(); break;
            case kPinAddSeparator: {
                // After the selected item; nothing selected: at the end.
                const size_t at = valid ? static_cast<size_t>(sel_) + 1 : dock.items.size();
                DockItemEntry e;
                e.kind = ToString(ItemKind::Separator);
                dock.items.insert(dock.items.begin() + static_cast<ptrdiff_t>(at), std::move(e));
                Changed(SettingsScope::Pins);
                FillStrip(static_cast<int>(at));
                break;
            }
            case kPinAddRecycleBin: AddEntry(kEntryRecycleBin); break;
            case kPinUp:
                if (valid && sel_ > 0) MoveItem(sel_, sel_ - 1);
                break;
            case kPinDown:
                if (valid && sel_ < n - 1) MoveItem(sel_, sel_ + 2);
                break;
            case kPinRemove:
                if (valid) RemoveItem(sel_);
                break;
            case kPinSettings:
                if (valid) OpenItemSettings(sel_);
                break;
            case kPinChooseIcon:
            case kPinChooseImage:
                if (valid && dock.items[static_cast<size_t>(sel_)].IsPin()) ChooseIcon(sel_, id == kPinChooseImage);
                break;
            case kPinResetIcon:
                if (!valid || !dock.items[static_cast<size_t>(sel_)].IsPin() || dock.items[static_cast<size_t>(sel_)].icon.empty()) return;
                dock.items[static_cast<size_t>(sel_)].icon.clear();
                Changed(SettingsScope::Pins);
                FillStrip(sel_);
                break;
            case kLockDock:
                dock.locked = GetCheck(id);  // read by the dock when a drag / drop / menu starts
                Changed(SettingsScope::SaveOnly);
                break;
            case kContentsDrawerToggle:
                g_drawerOpen = !g_drawerOpen;
                ApplyDrawer();
                if (HWND b = Item(kContentsDrawerToggle)) SetFocus(b);
                break;
            default:
                break;
        }
    }

private:
    static constexpr int kEntryRunning = 0, kEntryClock = 1, kEntryWeather = 2, kEntryTray = 3, kEntryBell = 4,
                         kEntryRecycleBin = 5, kEntrySeparator = 6, kBuiltInEntries = 7;

    // The left column's right edge (DIPs): beside the drawer, or the page's width.
    int Right() const { return g_drawerOpen ? Width() - kDrawerW - 20 : Width() - kMargin; }

    // --- the drawer ---

    void BuildEntries() {
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
            // kEntry* order
            builtIn(ItemKind::Running, L"Running apps", L"Open apps that are not pinned", true);
            builtIn(ItemKind::Clock, L"Clock", L"Any number, each with its own format and zone", false);
            builtIn(ItemKind::Weather, L"Weather", L"Any number, each with its own place", false);
            builtIn(ItemKind::Tray, L"System tray", L"The notification area icons as a grid", true);
            builtIn(ItemKind::Notifications, L"Notifications", L"A bell with the unread count", true);
            builtIn(ItemKind::RecycleBin, L"Recycle Bin", L"Drop files on it to recycle them", true);
            builtIn(ItemKind::Separator, L"Separator", L"A thin line between groups of icons", false);
        }
        entries_.resize(kBuiltInEntries);
        for (const Widgets::CatalogEntry& w : Widgets::Catalog()) {
            auto it = packages_.find(w.id);
            if (it == packages_.end()) {
                // Its note (the description's first line) and version, read once per session.
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

    bool Matches(const DrawerEntry& e) const {
        return filter_.empty() || ContainsNoCase(e.name, filter_) || ContainsNoCase(e.note, filter_) || ContainsNoCase(e.id, filter_);
    }

    StripTile EntryTile(size_t i) {
        const DrawerEntry& e = entries_[i];
        StripTile t;
        t.ref = static_cast<int>(i);
        t.name = e.name;
        t.version = e.version;
        t.disabled = e.once && Dock().Has(e.kind);
        t.note = t.disabled ? L"Already on the dock" : e.note;
        t.tint = TintOf(e.kind, e.id);
        t.letter = LetterOf(e.kind, e.name);
        switch (e.kind) {
            case ItemKind::Running: t.shape = StripTile::Shape::Running; break;
            case ItemKind::Separator: t.shape = StripTile::Shape::Separator; break;
            case ItemKind::RecycleBin:
                if (!binIcon_) {
                    DockItemEntry bin;
                    bin.kind = ToString(ItemKind::RecycleBin);
                    binIcon_ = SettingsItemIcon(nullptr, bin, Window().Px(44));
                }
                t.icon = binIcon_;
                t.shape = binIcon_ ? StripTile::Shape::Icon : StripTile::Shape::Letter;
                break;
            default: t.shape = StripTile::Shape::Letter; break;
        }
        return t;
    }

    void FillDrawer() {
        const int keep = drawer_.SelectedRef();
        std::vector<StripTile> tiles;
        int select = -1;
        auto group = [&](const wchar_t* title, size_t from, size_t to) {
            bool any = false;
            for (size_t i = from; i < to; ++i) {
                if (!Matches(entries_[i])) continue;
                if (!any) {
                    StripTile h;
                    h.header = true;
                    h.name = title;
                    tiles.push_back(std::move(h));
                    any = true;
                }
                if (static_cast<int>(i) == keep) select = static_cast<int>(tiles.size());
                tiles.push_back(EntryTile(i));
            }
        };
        group(L"BUILT IN", 0, std::min<size_t>(kBuiltInEntries, entries_.size()));
        group(L"SCRIPT WIDGETS", kBuiltInEntries, entries_.size());
        drawer_.SetTiles(std::move(tiles), select);
    }

    void ApplyDrawer() {
        const bool open = g_drawerOpen;
        for (int id : {kDrawerTitle, kDrawerHint, kDrawerSearch, kDrawerList, kDrawerLibraryLink}) Show(id, open);
        const int L = kMargin, R = Right();
        MoveCtl(kPageTitle, RECT{L, 12, R, 36});
        MoveCtl(kContentsNote, RECT{L, 38, R, 72});
        MoveCtl(kContentsStrip, RECT{L, 76, R, 194});
        MoveCtl(kContentsDrawerToggle, RECT{R - 150, 236 + 2, R, 236 + kRowH - 2});
        SetText(kContentsDrawerToggle, open ? L"Hide widget drawer" : L"Show widget drawer");
        flowKey_.clear();
        FlowInspector(true);
    }

    // The inspector's buttons, left to right in its second row (the shown ones), "Remove from
    // dock" at its right end (a third row when they do not fit beside it).
    void FlowInspector(bool force = false) {
        const int L = kMargin, R = Right();
        const int left = L + 14, right = R - 14;
        auto shown = [&](int id) {
            HWND h = Item(id);
            return h && (GetWindowLongW(h, GWL_STYLE) & WS_VISIBLE) != 0;
        };
        std::wstring key = std::to_wstring(R);
        for (int id : {kPinSettings, kPinChooseIcon, kPinChooseImage, kPinResetIcon}) key += shown(id) ? L"1" : L"0";
        if (!force && key == flowKey_) return;
        flowKey_ = key;
        MoveCtl(kContentsInfo, RECT{left, kInspectorTop + 10, right - 212, kInspectorTop + 46});
        MoveCtl(kPinUp, RECT{right - 206, kInspectorTop + 14, right - 106, kInspectorTop + 42});
        MoveCtl(kPinDown, RECT{right - 100, kInspectorTop + 14, right, kInspectorTop + 42});
        int x = left, y = kInspectorTop + 52;
        const struct {
            int id;
            int w;
        } flow[] = {{kPinSettings, 196}, {kPinChooseIcon, 92}, {kPinChooseImage, 102}, {kPinResetIcon, 80}};
        for (const auto& b : flow) {
            if (!shown(b.id)) continue;
            MoveCtl(b.id, RECT{x, y, x + b.w, y + 28});
            x += b.w + 6;
        }
        constexpr int kRemoveW = 128;
        if (x > left && x + kRemoveW > right) y += 34;  // no room beside them
        MoveCtl(kPinRemove, RECT{right - kRemoveW, y, right, y + 28});
        inspectorBottom_ = y + 28 + 10;
        Relayout();
    }

    // --- the strip ---

    const AppIdentity::ResolvedPin* PinFor(size_t item) {
        if (!Host().pins) return nullptr;
        const auto& pins = Host().pins();
        const std::vector<size_t> idx = Dock().PinIndexes();
        for (size_t k = 0; k < idx.size() && k < pins.size(); ++k) {
            if (idx[k] == item) return &pins[k];
        }
        return nullptr;
    }

    static std::wstring IconKey(const DockItemEntry& e, const AppIdentity::ResolvedPin* pin, int px) {
        std::wstring key = e.kind + L"|" + e.path + L"|" + e.aumid + L"|" + e.icon + L"|" + std::to_wstring(px);
        if (pin) key += L"|" + pin->iconFile + L"|" + std::to_wstring(pin->iconIndex) + (pin->missing ? L"|m" : L"");
        return key;
    }

    // The strip's icon of a pin (cached across refreshes; the unused ones are freed by FillStrip).
    HICON StripIcon(const DockItemEntry& e, const AppIdentity::ResolvedPin* pin) {
        const int px = Window().Px(44);
        const std::wstring key = IconKey(e, pin, px);
        auto it = icons_.find(key);
        if (it == icons_.end()) it = icons_.emplace(key, CachedIcon{SettingsItemIcon(pin, e, px), false}).first;
        it->second.used = true;
        return it->second.icon;
    }

    StripTile ItemTile(size_t i, const AppIdentity::ResolvedPin* pin, bool firstRunning, bool strip, std::wstring* details) {
        const DockItemEntry& e = Dock().items[i];
        const ItemKind k = e.Kind();
        std::wstring name, detail;
        Describe(Cfg(), e, pin, firstRunning, name, detail);
        std::wstring kind = KindLabel(k);
        if (k == ItemKind::App && pin && pin->kind == AppIdentity::PinKind::Folder) kind = L"Folder";  // a shortcut to a folder
        StripTile t;
        t.ref = static_cast<int>(i);
        t.name = name;
        t.note = kind;
        t.tint = TintOf(k, e.id);
        t.letter = LetterOf(k, name);
        if (k == ItemKind::Separator) {
            t.shape = StripTile::Shape::Separator;
        } else if (k == ItemKind::Running) {
            t.shape = StripTile::Shape::Running;
        } else if (e.IsPin() && strip) {
            t.icon = StripIcon(e, pin);
            t.shape = t.icon ? StripTile::Shape::Icon : StripTile::Shape::Letter;
        }
        if (details) *details = detail;
        return t;
    }

    // docks[].items (with host.pins, the pins' resolved twins) -> the strip; selects `select`.
    void FillStrip(int select) {
        for (auto& entry : icons_) entry.second.used = false;
        const std::vector<DockItemEntry>& items = Dock().items;
        std::vector<StripTile> tiles;
        bool runningSeen = false;
        for (size_t i = 0; i < items.size(); ++i) {
            const ItemKind k = items[i].Kind();
            const bool firstRunning = k == ItemKind::Running && !runningSeen;
            if (k == ItemKind::Running) runningSeen = true;
            tiles.push_back(ItemTile(i, items[i].IsPin() ? PinFor(i) : nullptr, firstRunning, true, nullptr));
        }
        // Keep the icons still in use (the tiles point at them).
        for (auto it = icons_.begin(); it != icons_.end();) {
            if (!it->second.used) {
                if (it->second.icon) DestroyIcon(it->second.icon);
                it = icons_.erase(it);
            } else {
                ++it;
            }
        }
        const int n = static_cast<int>(tiles.size());
        strip_.SetTiles(std::move(tiles), n == 0 ? -1 : std::clamp(select, 0, n - 1));
        sel_ = strip_.Selected();
        UpdateInspector();
    }

    bool HasSettings(size_t item) {
        const DockConfig& d = Dock();
        if (item >= d.items.size()) return false;
        switch (d.items[item].Kind()) {
            case ItemKind::Widget:
            case ItemKind::Clock:
            case ItemKind::Weather:
            case ItemKind::Folder:
            case ItemKind::Group:
            case ItemKind::Tray: return true;
            case ItemKind::App: {
                const AppIdentity::ResolvedPin* pin = PinFor(item);
                return pin && pin->kind == AppIdentity::PinKind::Folder;  // a shortcut to a folder
            }
            default: return false;
        }
    }

    void UpdateInspector() {
        const DockConfig& dock = Dock();
        const int n = static_cast<int>(dock.items.size());
        const bool valid = sel_ >= 0 && sel_ < n;
        std::vector<StripTile> info;
        if (valid) {
            const size_t i = static_cast<size_t>(sel_);
            const DockItemEntry& e = dock.items[i];
            const AppIdentity::ResolvedPin* pin = e.IsPin() ? PinFor(i) : nullptr;
            bool firstRunning = true;
            for (size_t k = 0; k < i; ++k) {
                if (dock.items[k].Kind() == ItemKind::Running) firstRunning = false;
            }
            std::wstring details;
            StripTile t = ItemTile(i, pin, firstRunning, false, &details);
            t.note = t.note + (details.empty() ? std::wstring() : L" · " + details);
            if (e.IsPin() && e.Kind() != ItemKind::Separator) {
                const std::wstring key = IconKey(e, pin, Window().Px(32));
                if (key != infoKey_) {
                    if (infoIcon_) DestroyIcon(infoIcon_);
                    infoIcon_ = SettingsItemIcon(pin, e, Window().Px(32));
                    infoKey_ = key;
                }
                t.icon = infoIcon_;
                t.shape = infoIcon_ ? StripTile::Shape::Icon : StripTile::Shape::Letter;
            }
            info.push_back(std::move(t));
        }
        info_.SetTiles(std::move(info), 0);
        const DockItemEntry* e = valid ? &dock.items[static_cast<size_t>(sel_)] : nullptr;
        const ItemKind k = e ? e->Kind() : ItemKind::Unknown;
        Enable(kPinUp, valid && sel_ > 0);
        Enable(kPinDown, valid && sel_ < n - 1);
        Enable(kPinRemove, valid);
        Enable(kPinAddRecycleBin, !dock.Has(ItemKind::RecycleBin));
        Show(kPinSettings, valid && HasSettings(static_cast<size_t>(sel_)));
        const bool iconable = e && e->IsPin() && k != ItemKind::Separator;
        Show(kPinChooseIcon, iconable);
        Show(kPinChooseImage, iconable);
        Show(kPinResetIcon, iconable);
        Enable(kPinResetIcon, iconable && !e->icon.empty());
        FlowInspector();
        // A button that just got disabled must not keep the keyboard focus.
        if (HWND f = GetFocus(); f && IsChild(Hwnd(), f) && !IsWindowEnabled(f)) SetFocus(strip_.Hwnd());
    }

    // --- edits ---

    void MoveItem(int from, int slot) {
        DockConfig& d = Dock();
        if (from < 0 || slot < 0 || !d.MoveItem(static_cast<size_t>(from), static_cast<size_t>(slot))) {
            FillStrip(from);  // nothing moved: the selection stays
            return;
        }
        Changed(SettingsScope::Pins);
        FillStrip(slot > from ? slot - 1 : slot);
    }

    void RemoveItem(int item) {
        DockConfig& d = Dock();
        const int n = static_cast<int>(d.items.size());
        if (item < 0 || item >= n) return;
        d.items.erase(d.items.begin() + item);
        Changed(SettingsScope::Pins);
        FillStrip(std::min(item, n - 2));
        FillDrawer();  // a single-instance kind may be addable again
    }

    // A new item of drawer entry `entry` (nothing for a single-instance kind the dock has).
    std::optional<DockItemEntry> NewEntry(int entry) {
        if (entry < 0 || entry >= static_cast<int>(entries_.size())) return std::nullopt;
        const DrawerEntry& de = entries_[static_cast<size_t>(entry)];
        if (de.once && Dock().Has(de.kind)) {
            MessageBeep(MB_ICONWARNING);
            return std::nullopt;
        }
        DockItemEntry e;
        e.kind = ToString(de.kind);
        if (de.kind == ItemKind::Widget) e.id = de.id;
        if (de.kind == ItemKind::Tray) Cfg().tray.enabled = true;  // as the dock menu's Add widget > Tray
        return e;
    }

    // Add (or Enter on a row): where the dock menu's Add widget / Add separator put it.
    void AddEntry(int entry) {
        std::optional<DockItemEntry> e = NewEntry(entry);
        if (!e) return;
        DockConfig& d = Dock();
        size_t at = SIZE_MAX;
        const ItemKind k = e->Kind();
        if (k == ItemKind::Widget) {
            at = d.AddScriptWidget(e->id);
        } else if (k == ItemKind::Separator || k == ItemKind::RecycleBin) {
            d.InsertPin(SIZE_MAX, std::move(*e));  // after the last pin
            const std::vector<size_t> idx = d.PinIndexes();
            at = idx.empty() ? SIZE_MAX : idx.back();
        } else {
            at = d.AddWidget(k);  // a clock / weather item gets the defaults (EnsureItemSettings)
        }
        if (at == SIZE_MAX) {
            MessageBeep(MB_ICONWARNING);
            return;
        }
        Changed(SettingsScope::Pins);  // also starts the weather service for a weather item
        FillStrip(static_cast<int>(at));
        FillDrawer();
    }

    // A drawer row dropped on the strip at `slot`.
    void InsertEntry(int entry, int slot) {
        std::optional<DockItemEntry> e = NewEntry(entry);
        if (!e) return;
        DockConfig& d = Dock();
        const size_t at = std::min(static_cast<size_t>(std::max(0, slot)), d.items.size());
        d.items.insert(d.items.begin() + static_cast<ptrdiff_t>(at), std::move(*e));
        Changed(SettingsScope::Pins);
        FillStrip(static_cast<int>(at));
        FillDrawer();
        SetFocus(strip_.Hwnd());
    }

    // Enter / a double click on a tile, "Open settings in its own window": an item with settings
    // -> its own Item window (round 5 phase B); the tray -> Global > Tray icons; the running apps
    // -> the Running apps tab; the notification centre -> Global > Notifications.
    void OpenItemSettings(int item) {
        const DockConfig& d = Dock();
        if (item < 0 || item >= static_cast<int>(d.items.size())) return;
        switch (d.items[static_cast<size_t>(item)].Kind()) {
            case ItemKind::Running: Window().ShowPage(kTabRunning); return;
            case ItemKind::Tray:
            case ItemKind::TrayApp:
                if (Host().openGlobal) Host().openGlobal(kPageTray);
                return;
            case ItemKind::Notifications:
                if (Host().openGlobal) Host().openGlobal(kPageNotifications);
                return;
            default: break;
        }
        if (HasSettings(static_cast<size_t>(item)) && Host().openItem && DockIndex() != SIZE_MAX) {
            Host().openItem(DockIndex(), static_cast<size_t>(item));
        }
    }

    void AddFiles() {
        winrt::com_ptr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())))) {
            MessageBeep(MB_ICONERROR);
            return;
        }
        const COMDLG_FILTERSPEC filters[] = {{L"Apps and shortcuts (*.lnk;*.exe)", L"*.lnk;*.exe"}, {L"All files", L"*.*"}};
        dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
        dialog->SetTitle(L"Pin apps to the dock");
        FILEOPENDIALOGOPTIONS options = 0;
        dialog->GetOptions(&options);
        // NODEREFERENCELINKS: a chosen shortcut is pinned as the shortcut (keeps its arguments).
        dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM | FOS_NODEREFERENCELINKS |
                           FOS_NOCHANGEDIR);
        winrt::com_ptr<IShellItem> programs;
        if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_Programs, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(programs.put())))) {
            dialog->SetDefaultFolder(programs.get());
        }
        if (FAILED(dialog->Show(Owner()))) return;  // cancelled
        winrt::com_ptr<IShellItemArray> items;
        DWORD count = 0;
        if (FAILED(dialog->GetResults(items.put())) || FAILED(items->GetCount(&count))) return;
        std::vector<std::wstring> paths;
        for (DWORD i = 0; i < count; ++i) {
            winrt::com_ptr<IShellItem> item;
            PWSTR raw = nullptr;
            if (SUCCEEDED(items->GetItemAt(i, item.put())) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) && raw) {
                paths.emplace_back(raw);
                CoTaskMemFree(raw);
            }
        }
        PinPaths(paths);
    }

    void AddFolder() {
        winrt::com_ptr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())))) {
            MessageBeep(MB_ICONERROR);
            return;
        }
        dialog->SetTitle(L"Pin a folder to the dock");
        FILEOPENDIALOGOPTIONS options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
        if (FAILED(dialog->Show(Owner()))) return;  // cancelled
        winrt::com_ptr<IShellItem> item;
        PWSTR raw = nullptr;
        if (FAILED(dialog->GetResult(item.put())) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || !raw) return;
        std::vector<std::wstring> paths{std::wstring(raw)};
        CoTaskMemFree(raw);
        PinPaths(paths);
    }

    // Pins the paths after the last pin (skipping apps already pinned) and selects the last new one.
    void PinPaths(const std::vector<std::wstring>& paths) {
        if (paths.empty() || !Host().pinFiles) return;
        const size_t before = Dock().PinCount();
        Host().pinFiles(paths);  // reloads the dock and saves
        Window().ConfigApplied();
        const size_t after = Dock().PinCount();
        const std::vector<size_t> idx = Dock().PinIndexes();
        FillStrip(after > before && !idx.empty() ? static_cast<int>(idx.back()) : sel_);
        Window().UpdateIdentity();
    }

    // Choose icon… / Choose image… for the pin at item `item` (not separators).
    void ChooseIcon(int item, bool image) {
        const std::vector<size_t> idx = Dock().PinIndexes();
        size_t k = SIZE_MAX;
        for (size_t i = 0; i < idx.size(); ++i) {
            if (static_cast<int>(idx[i]) == item) k = i;
        }
        const std::vector<DockItemEntry> pinned = Dock().Pins();
        if (k >= pinned.size()) return;
        const DockItemEntry before = pinned[k];
        if (before.Kind() == ItemKind::Separator) return;
        std::wstring file, startImage;
        int index = 0;
        if (Host().pins) {
            const auto& pins = Host().pins();
            if (k < pins.size()) {
                IconUtil::StartIconFor(pins[k], file, index);
                startImage = IconUtil::StartImageFor(pins[k]);
            }
        }
        std::wstring spec;
        const bool ok = image ? IconUtil::ChooseIconFile(Owner(), startImage, spec) : IconUtil::ChooseIconResource(Owner(), file, index, spec);
        if (!ok) return;
        // The items may have changed while the dialog was open (a drag on the dock): only the
        // same entry at the same place gets the icon.
        DockConfig& now = Dock();
        if (k >= now.PinCount()) return;
        DockItemEntry& e = now.PinAt(k);
        if (e.path != before.path || e.aumid != before.aumid || e.kind != before.kind) {
            MessageBeep(MB_ICONWARNING);
            return;
        }
        e.icon = spec;  // the absolute path the dialog returned
        Changed(SettingsScope::Pins);
        FillStrip(static_cast<int>(now.PinIndexes()[k]));
    }

    struct CachedIcon {
        HICON icon = nullptr;
        bool used = false;
    };

    SettingsStrip strip_;
    SettingsStrip drawer_;
    SettingsStrip info_;
    std::vector<DrawerEntry> entries_;
    std::map<std::wstring, std::pair<std::wstring, std::wstring>> packages_;  // widget id -> note, version
    std::map<std::wstring, CachedIcon> icons_;  // the strip's icons by IconKey
    HICON infoIcon_ = nullptr;                  // the inspector's
    std::wstring infoKey_;
    HICON binIcon_ = nullptr;                   // the drawer's Recycle Bin row
    int iconPx_ = 0;
    int sel_ = 0;  // the selected item
    std::wstring filter_;
    std::wstring flowKey_;
    int inspectorBottom_ = kInspectorTop + 90;
};

}  // namespace

std::unique_ptr<SettingsPage> MakeContentsPage(SettingsWindow& window) { return std::make_unique<ContentsPage>(window); }
