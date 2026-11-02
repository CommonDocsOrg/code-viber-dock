// The Layout page: a scaled drawing of every monitor (its work area shaded) with every dock on its
// edge, labelled with its id (and title). Click a dock to make it the one the other pages edit;
// drag it along its edge (it snaps to the start / centre / end like Ctrl+drag on the dock), onto
// another edge or onto another monitor. A drop applies at once and saves (edge, align, offset,
// monitor). Owner-drawn with GDI in the palette colours; it repaints only when something changed.

#include <windows.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <string>
#include <vector>

#include "SettingsForm.h"
#include "SettingsIds.h"

namespace {

using namespace SettingsIds;

constexpr wchar_t kCanvasClass[] = L"WinDock.LayoutCanvas";
constexpr int kCanvasH = 300;          // DIPs
constexpr float kMarginDip = 8.0f;     // the dock's distance from the monitor edge (DockWindow kBottomMargin)
constexpr float kSnapDip = 24.0f;      // snaps to the centre / an end this close (DockWindow kDockSnapDip)

BOOL CALLBACK CollectMonitor(HMONITOR mon, HDC, LPRECT, LPARAM lp) {
    reinterpret_cast<std::vector<HMONITOR>*>(lp)->push_back(mon);
    return TRUE;
}

// DockManager's order: the primary first, then the others in enumeration order.
std::vector<HMONITOR> Monitors() {
    std::vector<HMONITOR> monitors;
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&monitors));
    HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    std::erase(monitors, primary);
    monitors.insert(monitors.begin(), primary);
    return monitors;
}

const wchar_t* EdgeWord(DockEdge e) {
    switch (e) {
        case DockEdge::Top: return L"top";
        case DockEdge::Left: return L"left";
        case DockEdge::Right: return L"right";
        case DockEdge::Bottom:
        default: return L"bottom";
    }
}
const wchar_t* AlignWord(DockAlign a) { return a == DockAlign::Start ? L"start" : a == DockAlign::End ? L"end" : L"centre"; }

class LayoutPage;

// The canvas control (window class WinDock.LayoutCanvas, created by the page's control table).
class LayoutCanvas {
public:
    explicit LayoutCanvas(LayoutPage& page) : page_(page) {}
    void Attach(HWND hwnd) {
        hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    }
    // Monitors and dock rects again (config loaded, a dock moved, displays changed).
    void Reload();
    void Repaint() {
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);

private:
    struct Mon {
        RECT rc{}, work{};
        UINT dpi = 96;
    };
    struct Dock {
        size_t cfg = 0;
        int monitor = -1;
        RECT bar{};     // screen
        RECT canvas{};  // canvas px (inflated to at least 4 px thick)
    };
    // Where a drop would put the dragged dock.
    struct Target {
        int monitor = 0;
        DockEdge edge = DockEdge::Bottom;
        DockAlign align = DockAlign::Center;
        int offsetDip = 0;
        RECT ghost{};  // screen
    };

    LRESULT OnMessage(UINT msg, WPARAM wp, LPARAM lp);
    void Paint(HDC hdc);
    void Layout();  // the canvas mapping for the client size
    POINT ToCanvas(POINT screen) const;
    POINT ToScreen(POINT canvas) const;
    RECT ToCanvas(const RECT& r) const;
    int HitDock(POINT canvas) const;
    Target TargetFor(POINT canvasPt) const;
    void Drop(const Target& t);
    void Publish();  // window text (automation) and the selection line

    LayoutPage& page_;
    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    std::vector<Mon> mons_;
    std::vector<Dock> docks_;
    RECT unionRc_{};
    float scale_ = 1.0f;
    float ox_ = 0.0f, oy_ = 0.0f;
    // Dragging
    int pressed_ = -1;      // dock index under the press
    POINT pressPt_{};       // canvas
    POINT grab_{};          // screen: pointer - bar centre at the press
    bool dragging_ = false;
    Target target_;
};

class LayoutPage : public FormPage {
public:
    explicit LayoutPage(SettingsWindow& w) : FormPage(w, L"Layout", L'\xE7F4'), canvas_(*this) {}

    Config& Config_() { return Cfg(); }
    SettingsWindow& Win() { return Window(); }
    SettingsHost& HostRef() { return Host(); }
    void Apply() { Changed(SettingsScope::Appearance); }
    void SetSelectedText(const std::wstring& text) { SetText(kLayoutSelected, text); }

protected:
    void Build() override {
        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW wc{sizeof(wc)};
            wc.lpfnWndProc = &LayoutCanvas::Proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = kCanvasClass;
            wc.style = CS_HREDRAW | CS_VREDRAW;
            registered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        }
        const int right = Width() - kMargin;
        Title(L"Layout");
        Add(kLayoutCanvas, CtlKind::Custom, kCanvasClass, L"", WS_TABSTOP, RECT{kMargin, y_, right, y_ + kCanvasH});
        NextRow(kCanvasH + 8);
        Text(kLayoutSelected, CtlKind::Value, L"", kMargin, right - kMargin, 22);
        NextRow(26);
        Text(kLayoutNote, CtlKind::Note,
             L"Every display with its docks (the shaded part is its work area). Click a dock to edit it on the other "
             L"pages; drag it along its edge (it snaps to the centre and the ends), onto another edge or onto "
             L"another display. The dock moves when you let go.",
             kMargin, right - kMargin, 54);
        NextRow(54);
    }
    void Created() override {
        if (HWND c = Item(kLayoutCanvas)) canvas_.Attach(c);
    }
    void Load() override { canvas_.Reload(); }
    void OnDisplayChange() override { canvas_.Reload(); }
    void OnStyleChanged() override { canvas_.Reload(); }

private:
    LayoutCanvas canvas_;
};

LRESULT CALLBACK LayoutCanvas::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (auto* self = reinterpret_cast<LayoutCanvas*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA))) {
        return self->OnMessage(msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void LayoutCanvas::Reload() {
    if (!hwnd_) return;
    mons_.clear();
    for (HMONITOR m : Monitors()) {
        MONITORINFO mi{sizeof(mi)};
        if (!GetMonitorInfoW(m, &mi)) continue;
        Mon mon;
        mon.rc = mi.rcMonitor;
        mon.work = mi.rcWork;
        UINT dx = 96, dy = 96;
        if (SUCCEEDED(GetDpiForMonitor(m, MDT_EFFECTIVE_DPI, &dx, &dy))) mon.dpi = dx;
        mons_.push_back(mon);
    }
    docks_.clear();
    if (page_.HostRef().dockRects) {
        for (const SettingsDockRect& r : page_.HostRef().dockRects()) {
            Dock d;
            d.cfg = r.cfg;
            d.monitor = r.monitor;
            d.bar = r.bar;
            docks_.push_back(d);
        }
    }
    Layout();
    Publish();
    Repaint();
}

void LayoutCanvas::Layout() {
    RECT client{};
    GetClientRect(hwnd_, &client);
    unionRc_ = RECT{};
    for (const Mon& m : mons_) UnionRect(&unionRc_, &unionRc_, &m.rc);
    const float pad = static_cast<float>(page_.Win().Px(10));
    const float uw = static_cast<float>(std::max<LONG>(1, unionRc_.right - unionRc_.left));
    const float uh = static_cast<float>(std::max<LONG>(1, unionRc_.bottom - unionRc_.top));
    const float cw = std::max(1.0f, static_cast<float>(client.right) - 2.0f * pad);
    const float ch = std::max(1.0f, static_cast<float>(client.bottom) - 2.0f * pad);
    scale_ = std::min(cw / uw, ch / uh);
    ox_ = pad + (cw - uw * scale_) / 2.0f;
    oy_ = pad + (ch - uh * scale_) / 2.0f;
    for (Dock& d : docks_) {
        if (d.monitor < 0) continue;
        d.canvas = ToCanvas(d.bar);
        // At least 4 px thick so a dock stays visible and grabbable on a small drawing.
        if (d.canvas.bottom - d.canvas.top < 4) {
            const LONG mid = (d.canvas.top + d.canvas.bottom) / 2;
            d.canvas.top = mid - 2;
            d.canvas.bottom = mid + 2;
        }
        if (d.canvas.right - d.canvas.left < 4) {
            const LONG mid = (d.canvas.left + d.canvas.right) / 2;
            d.canvas.left = mid - 2;
            d.canvas.right = mid + 2;
        }
    }
}

POINT LayoutCanvas::ToCanvas(POINT s) const {
    return POINT{static_cast<LONG>(std::lround(ox_ + (s.x - unionRc_.left) * scale_)),
                 static_cast<LONG>(std::lround(oy_ + (s.y - unionRc_.top) * scale_))};
}

POINT LayoutCanvas::ToScreen(POINT c) const {
    return POINT{static_cast<LONG>(std::lround((c.x - ox_) / scale_)) + unionRc_.left,
                 static_cast<LONG>(std::lround((c.y - oy_) / scale_)) + unionRc_.top};
}

RECT LayoutCanvas::ToCanvas(const RECT& r) const {
    const POINT a = ToCanvas(POINT{r.left, r.top}), b = ToCanvas(POINT{r.right, r.bottom});
    return RECT{a.x, a.y, b.x, b.y};
}

int LayoutCanvas::HitDock(POINT c) const {
    for (size_t i = docks_.size(); i-- > 0;) {
        if (docks_[i].monitor < 0) continue;
        RECT r = docks_[i].canvas;
        InflateRect(&r, 4, 4);
        if (PtInRect(&r, c)) return static_cast<int>(i);
    }
    return -1;
}

LayoutCanvas::Target LayoutCanvas::TargetFor(POINT canvasPt) const {
    Target t;
    const Dock& d = docks_[static_cast<size_t>(pressed_)];
    const DockConfig& dc = page_.Config_().docks[d.cfg];
    const POINT p = ToScreen(canvasPt);
    // The dock's centre follows the pointer (as grabbed).
    const POINT c{p.x - grab_.x, p.y - grab_.y};
    // The monitor under the centre (nearest when it is off every monitor).
    long best = LONG_MAX;
    for (size_t i = 0; i < mons_.size(); ++i) {
        const RECT& r = mons_[i].rc;
        const long dx = c.x < r.left ? r.left - c.x : c.x >= r.right ? c.x - r.right + 1 : 0;
        const long dy = c.y < r.top ? r.top - c.y : c.y >= r.bottom ? c.y - r.bottom + 1 : 0;
        if (dx + dy < best) {
            best = dx + dy;
            t.monitor = static_cast<int>(i);
        }
    }
    if (mons_.empty()) return t;
    const Mon& m = mons_[static_cast<size_t>(t.monitor)];
    const RECT& r = m.rc;
    const LONG cx = std::clamp(c.x, r.left, r.right - 1), cy = std::clamp(c.y, r.top, r.bottom - 1);
    // The edge nearest to the centre.
    const LONG dist[4] = {r.bottom - cy, cy - r.top, cx - r.left, r.right - cx};  // DockEdge order
    int e = 0;
    for (int i = 1; i < 4; ++i) {
        if (dist[i] < dist[e]) e = i;
    }
    t.edge = static_cast<DockEdge>(e);
    const bool wasVertical = IsVertical(dc.edge), vertical = IsVertical(t.edge);
    const LONG length = wasVertical ? d.bar.bottom - d.bar.top : d.bar.right - d.bar.left;  // along the bar
    const LONG thick = wasVertical ? d.bar.right - d.bar.left : d.bar.bottom - d.bar.top;
    const float s = static_cast<float>(m.dpi) / 96.0f;
    const LONG margin = dc.fullWidth ? 0 : static_cast<LONG>(std::lround(kMarginDip * s));
    const LONG mStart = vertical ? r.top : r.left, mEnd = vertical ? r.bottom : r.right;
    const LONG along = vertical ? cy : cx;
    LONG barStart = 0;
    if (dc.fullWidth) {
        // Full width: only the edge and the monitor change.
        t.align = dc.align;
        t.offsetDip = dc.offset;
        barStart = mStart;
    } else {
        // The nearest of centre / start / end, snapped to it within kSnapDip (as Ctrl+drag does).
        const LONG offCenter = along - (mStart + mEnd) / 2;
        const LONG offStart = (along - length / 2) - (mStart + margin);
        const LONG offEnd = (along + length / 2) - (mEnd - margin);
        LONG off = offCenter;
        t.align = DockAlign::Center;
        if (std::labs(offStart) < std::labs(off)) {
            off = offStart;
            t.align = DockAlign::Start;
        }
        if (std::labs(offEnd) < std::labs(off)) {
            off = offEnd;
            t.align = DockAlign::End;
        }
        if (std::labs(off) <= std::lround(kSnapDip * s)) off = 0;
        t.offsetDip = static_cast<int>(std::lround(static_cast<float>(off) / s));
        const LONG offPx = static_cast<LONG>(std::lround(t.offsetDip * s));
        switch (t.align) {
            case DockAlign::Start: barStart = mStart + margin + offPx; break;
            case DockAlign::End: barStart = mEnd - margin - length + offPx; break;
            default: barStart = (mStart + mEnd) / 2 - length / 2 + offPx; break;
        }
        barStart = std::clamp(barStart, mStart, std::max(mStart, mEnd - length));
    }
    const LONG barLen = dc.fullWidth ? mEnd - mStart : length;
    switch (t.edge) {
        case DockEdge::Top: t.ghost = RECT{barStart, r.top + margin, barStart + barLen, r.top + margin + thick}; break;
        case DockEdge::Left: t.ghost = RECT{r.left + margin, barStart, r.left + margin + thick, barStart + barLen}; break;
        case DockEdge::Right: t.ghost = RECT{r.right - margin - thick, barStart, r.right - margin, barStart + barLen}; break;
        case DockEdge::Bottom:
        default: t.ghost = RECT{barStart, r.bottom - margin - thick, barStart + barLen, r.bottom - margin}; break;
    }
    return t;
}

void LayoutCanvas::Drop(const Target& t) {
    Config& c = page_.Config_();
    const Dock& d = docks_[static_cast<size_t>(pressed_)];
    DockConfig& dc = c.docks[d.cfg];
    // "each" stays on every monitor; "primary" stays "primary" on the primary.
    int monitor = dc.monitor;
    if (!dc.Each() && !(dc.monitor == DockConfig::kMonitorPrimary && t.monitor == 0)) monitor = t.monitor;
    if (dc.edge == t.edge && dc.align == t.align && dc.offset == t.offsetDip && dc.monitor == monitor) return;
    dc.edge = t.edge;
    dc.align = t.align;
    dc.offset = t.offsetDip;
    dc.monitor = monitor;
    c.editDock = d.cfg;
    page_.Apply();        // the dock moves (and is saved)
    page_.Win().Refresh();  // the other pages show the new position (and this canvas reloads)
}

void LayoutCanvas::Publish() {
    std::wstring text;
    for (size_t i = 0; i < docks_.size(); ++i) {
        const Dock& d = docks_[i];
        text += std::to_wstring(i) + L":" + std::to_wstring(d.cfg) + L":" + std::to_wstring(d.monitor) + L":";
        if (d.monitor >= 0) {
            text += std::to_wstring(d.canvas.left) + L"," + std::to_wstring(d.canvas.top) + L"," +
                    std::to_wstring(d.canvas.right) + L"," + std::to_wstring(d.canvas.bottom);
        } else {
            text += L"-";
        }
        text += L";";
    }
    SetWindowTextW(hwnd_, text.c_str());
    const Config& c = page_.Config_();
    std::wstring line;
    if (dragging_ && pressed_ >= 0) {
        line = L"Drop: " + std::wstring(EdgeWord(target_.edge)) + L", " + AlignWord(target_.align) +
               (target_.offsetDip ? (target_.offsetDip > 0 ? L" +" : L" ") + std::to_wstring(target_.offsetDip) : L"") +
               L", display " + std::to_wstring(target_.monitor + 1);
    } else if (c.editDock < c.docks.size()) {
        const DockConfig& d = c.docks[c.editDock];
        line = L"Selected: " + d.id + L" (" + EdgeWord(d.edge) + L", " + AlignWord(d.align) +
               (d.offset ? (d.offset > 0 ? L" +" : L" ") + std::to_wstring(d.offset) : L"") + L")";
    }
    page_.SetSelectedText(line);
}

void LayoutCanvas::Paint(HDC hdc) {
    RECT client{};
    GetClientRect(hwnd_, &client);
    const DarkMode::Palette& pal = page_.Win().Colors();
    auto mix = [](COLORREF a, COLORREF b, int pa) {
        auto ch = [&](BYTE x, BYTE y) { return static_cast<BYTE>((x * pa + y * (100 - pa)) / 100); };
        return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)), ch(GetBValue(a), GetBValue(b)));
    };
    HBRUSH back = CreateSolidBrush(pal.layer);
    FillRect(hdc, &client, back);
    DeleteObject(back);
    // The page's message font (WM_SETFONT may have come before the canvas was attached: then the
    // selection line's font, which is the same).
    HFONT font = font_ ? font_ : reinterpret_cast<HFONT>(SendMessageW(GetDlgItem(GetParent(hwnd_), kLayoutSelected), WM_GETFONT, 0, 0));
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, font ? font : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT))));
    SetBkMode(hdc, TRANSPARENT);
    const COLORREF monFill = mix(pal.text, pal.layer, 8), workFill = mix(pal.text, pal.layer, 4);
    HPEN monPen = CreatePen(PS_SOLID, 1, pal.textSecondary);
    HGDIOBJ oldPen = SelectObject(hdc, monPen);
    for (size_t i = 0; i < mons_.size(); ++i) {
        const RECT r = ToCanvas(mons_[i].rc);
        HBRUSH f = CreateSolidBrush(monFill);
        HGDIOBJ ob = SelectObject(hdc, f);
        Rectangle(hdc, r.left, r.top, r.right, r.bottom);
        SelectObject(hdc, ob);
        DeleteObject(f);
        RECT w = ToCanvas(mons_[i].work);
        InflateRect(&w, -1, -1);
        HBRUSH wf = CreateSolidBrush(workFill);
        FillRect(hdc, &w, wf);
        DeleteObject(wf);
        const std::wstring name = (i == 0 ? L"1 (primary)  " : std::to_wstring(i + 1) + L"  ") +
                                  std::to_wstring(mons_[i].rc.right - mons_[i].rc.left) + L" × " +
                                  std::to_wstring(mons_[i].rc.bottom - mons_[i].rc.top);
        SetTextColor(hdc, pal.textSecondary);
        RECT tr{r.left + 6, r.top + 4, r.right - 6, r.bottom - 4};
        DrawTextW(hdc, name.c_str(), -1, &tr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    SelectObject(hdc, oldPen);
    DeleteObject(monPen);
    const Config& c = page_.Config_();
    for (size_t i = 0; i < docks_.size(); ++i) {
        const Dock& d = docks_[i];
        if (d.monitor < 0 || d.cfg >= c.docks.size()) continue;
        const bool selected = d.cfg == c.editDock;
        const bool moving = dragging_ && static_cast<int>(i) == pressed_;
        HBRUSH f = CreateSolidBrush(selected ? pal.accent : pal.textSecondary);
        RECT r = d.canvas;
        if (moving) {
            HBRUSH dim = CreateSolidBrush(mix(selected ? pal.accent : pal.textSecondary, pal.layer, 40));
            FillRect(hdc, &r, dim);
            DeleteObject(dim);
        } else {
            FillRect(hdc, &r, f);
        }
        DeleteObject(f);
        // The label beside the bar, toward the middle of its monitor.
        const DockConfig& dc = c.docks[d.cfg];
        std::wstring label = dc.id + (dc.title.empty() ? L"" : L" · " + dc.title) +
                             (dc.layer == DockLayer::Desktop ? L" (desktop)" : L"");
        SetTextColor(hdc, selected ? pal.text : pal.textSecondary);
        SIZE sz{};
        GetTextExtentPoint32W(hdc, label.c_str(), static_cast<int>(label.size()), &sz);
        const int gap = 4;
        RECT tr{};
        switch (dc.edge) {
            case DockEdge::Top: tr = RECT{(r.left + r.right - sz.cx) / 2, r.bottom + gap, 0, 0}; break;
            case DockEdge::Left: tr = RECT{r.right + gap, (r.top + r.bottom - sz.cy) / 2, 0, 0}; break;
            case DockEdge::Right: tr = RECT{r.left - gap - sz.cx, (r.top + r.bottom - sz.cy) / 2, 0, 0}; break;
            case DockEdge::Bottom:
            default: tr = RECT{(r.left + r.right - sz.cx) / 2, r.top - gap - sz.cy, 0, 0}; break;
        }
        tr.right = tr.left + sz.cx;
        tr.bottom = tr.top + sz.cy;
        DrawTextW(hdc, label.c_str(), -1, &tr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
    }
    if (dragging_ && pressed_ >= 0) {
        // Where the drop puts it: an outline in the accent colour.
        const RECT g = ToCanvas(target_.ghost);
        HPEN pen = CreatePen(PS_SOLID, std::max(1, page_.Win().Px(2)), pal.accent);
        HGDIOBJ op = SelectObject(hdc, pen);
        HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, g.left - 1, g.top - 1, std::max(g.right, g.left + 4) + 1, std::max(g.bottom, g.top + 4) + 1);
        SelectObject(hdc, ob);
        SelectObject(hdc, op);
        DeleteObject(pen);
    }
    if (GetFocus() == hwnd_) {
        RECT fr = client;
        InflateRect(&fr, -1, -1);
        DrawFocusRect(hdc, &fr);
    }
    SelectObject(hdc, oldFont);
}

LRESULT LayoutCanvas::OnMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SETFONT:
            font_ = reinterpret_cast<HFONT>(wp);
            return 0;
        case WM_SIZE:
            Layout();
            Publish();
            Repaint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd_, &ps);
            RECT client{};
            GetClientRect(hwnd_, &client);
            // Double-buffered: one blit per repaint.
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, std::max<LONG>(1, client.right), std::max<LONG>(1, client.bottom));
            HGDIOBJ old = SelectObject(mem, bmp);
            Paint(mem);
            BitBlt(hdc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            Repaint();
            return 0;
        case WM_GETDLGCODE:
            return dragging_ ? DLGC_WANTALLKEYS : 0;
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd_);
            const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            pressed_ = HitDock(pt);
            if (pressed_ < 0) return 0;
            pressPt_ = pt;
            const RECT& b = docks_[static_cast<size_t>(pressed_)].bar;
            const POINT s = ToScreen(pt);
            grab_ = POINT{s.x - (b.left + b.right) / 2, s.y - (b.top + b.bottom) / 2};
            SetCapture(hwnd_);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (pressed_ < 0 || GetCapture() != hwnd_) return 0;
            const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (!dragging_ && std::abs(pt.x - pressPt_.x) <= GetSystemMetrics(SM_CXDRAG) &&
                std::abs(pt.y - pressPt_.y) <= GetSystemMetrics(SM_CYDRAG)) {
                return 0;
            }
            dragging_ = true;
            const Target t = TargetFor(pt);
            if (!EqualRect(&t.ghost, &target_.ghost) || t.edge != target_.edge || t.align != target_.align ||
                t.offsetDip != target_.offsetDip || t.monitor != target_.monitor) {
                target_ = t;  // repaint only when the drop place changed
                Publish();
                Repaint();
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (pressed_ < 0) return 0;
            const bool drop = dragging_;
            const int index = pressed_;
            dragging_ = false;
            if (GetCapture() == hwnd_) ReleaseCapture();
            pressed_ = index;
            if (drop) {
                const Target t = TargetFor(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
                Drop(t);
            } else {
                // A click: this dock is the one the other pages edit.
                Config& c = page_.Config_();
                const size_t cfg = docks_[static_cast<size_t>(index)].cfg;
                if (c.editDock != cfg) {
                    c.editDock = cfg;
                    page_.Win().Refresh();
                    page_.Win().ConfigApplied();
                }
            }
            pressed_ = -1;
            target_ = Target{};
            Reload();
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE && dragging_) {
                dragging_ = false;
                pressed_ = -1;
                if (GetCapture() == hwnd_) ReleaseCapture();
                Publish();
                Repaint();
                return 0;
            }
            break;
        case WM_CAPTURECHANGED:
            if (reinterpret_cast<HWND>(lp) != hwnd_ && (dragging_ || pressed_ >= 0)) {
                // Someone took the mouse: cancel the drag.
                dragging_ = false;
                pressed_ = -1;
                Publish();
                Repaint();
            }
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace

std::unique_ptr<SettingsPage> MakeLayoutPage(SettingsWindow& window) { return std::make_unique<LayoutPage>(window); }
