// The tray grid (`tray` item, round 4 phase 8a): TrayHost's notify icons as a grid in one
// non-magnifying widget slot (like the clock). Input on a cell is forwarded to the icon's owner
// with the icon's version semantics (TrayHost::Forward); the last cell becomes an overflow button
// that opens the drawer (the dock's group popup in tray mode: every icon) when there are more icons
// than cells or some are hidden from the grid. A cell pressed and dragged is the icon's drag (the
// dock: onto the icon row it becomes a `trayApp` pin, onto the grid it moves there); Ctrl + right
// click on a cell opens the tray menu (8b).
//
// `trayApp` (8b): one notify icon as a full-size, magnifying icon slot (the dock draws its picture
// at the icon size; a placeholder while its app does not run); clicks go to the app like the
// grid's, Ctrl + right click opens the tray menu (Unpin).

#include <algorithm>
#include <cmath>
#include <string>

#include "DockElement.h"
#include "Theme.h"
#include "TrayHost.h"

namespace {

constexpr float kCellPadDip = 3.0f;  // around each icon (the cell is the icon + twice this)
constexpr ULONGLONG kMoveForwardMs = 100;  // WM_MOUSEMOVE to the owner at most this often

// Double clicks across element rebuilds: the last click forwarded (process-wide, UI thread).
uint64_t g_lastClickId = 0;
ULONGLONG g_lastClickAt = 0;
POINT g_lastClickPt{};
ULONGLONG g_lastMoveAt = 0;

bool Contains(const std::vector<std::wstring>& keys, const std::wstring& key) {
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

// The label: the tooltip's lines without the empty ones.
std::wstring LabelOf(const std::wstring& tip) {
    std::wstring out, line;
    auto flush = [&] {
        while (!line.empty() && iswspace(line.back())) line.pop_back();
        if (!line.empty()) out += (out.empty() ? L"" : L"\n") + line;
        line.clear();
    };
    for (wchar_t c : tip) {
        if (c == L'\r' || c == L'\n') {
            flush();
        } else {
            line += c;
        }
    }
    flush();
    return out;
}

class TrayElement final : public DockElement {
public:
    TrayElement(ElementHost& host, const DockItemEntry& entry, size_t ordinal) : DockElement(host, entry) {
        key_.id = L"kind:tray" + (ordinal ? L"#" + std::to_wstring(ordinal) : std::wstring());
    }

    void AppendSlots(SlotBuilder& out) override {
        DockItem& item = out.Add(key_);
        if (item.kind != DockItem::Kind::Tray) {
            item.kind = DockItem::Kind::Tray;
            item.dirty = true;
        }
        item.pinned = false;
        item.displayName = L"Tray";
    }

    float HitSlack(const LayoutContext& ctx) const override { return std::round(5.0f * ctx.scale); }
    bool OnEvent(DockEvent e) override { return e == DockEvent::TrayChanged; }

    ElementSize Measure(const LayoutContext& ctx) override {
        vertical_ = ctx.axis == DockAxis::Vertical;
        scale_ = ctx.scale;
        const DockItemEntry& e = Entry();
        const int rows = std::clamp(e.rows, 1, 20);
        const int columns = std::clamp(e.columns, 1, 20);
        // Round 16 phase 16B: rows / columns as seen on screen, on every edge (rows stacked top to
        // bottom, columns left to right). On a bottom / top dock the rows run across the bar; on a
        // left / right dock the columns do. (Before, `rows` was always across and `columns` along,
        // so a vertical dock's grid looked transposed.)
        size_t along = static_cast<size_t>(vertical_ ? rows : columns);
        size_t across = static_cast<size_t>(vertical_ ? columns : rows);
        const float cellFull = std::round((static_cast<float>(e.trayIconSize) + 2.0f * kCellPadDip) * ctx.scale);
        float cell = cellFull;
        // The room across: the box's cross room at rest inside the bar (a thick bar, titles beside
        // the icons: more than the icon row), else the icon row; unknown (0, 0): no limit. One or
        // two across always fit (shrunk, below); more only when they fit at full size, so a thin
        // bar never turns twenty rows into slivers.
        const float room = ctx.rowRoom > 0.0f ? ctx.rowRoom : ctx.iconPx;
        if (room > 0.0f) {
            if (cellFull > 0.0f) {
                const size_t fit = static_cast<size_t>(std::max(0.0f, std::floor(room / cellFull)));
                across = std::min(across, std::max<size_t>(2, fit));
            }
            // The cells fit the room: two rows of large icons shrink to half an icon each.
            cell = std::min(cellFull, std::floor(room / static_cast<float>(across)));
        }
        cell_ = std::max(cell, 8.0f);
        iconPx_ = std::max(4.0f, std::min(std::round(static_cast<float>(e.trayIconSize) * ctx.scale), cell_ - 2.0f));

        shown_.clear();
        all_ = 0;
        if (TrayHost* tray = host_.Tray()) {
            all_ = TrayIconsFor(*tray, host_.DockCfg(), e, false).size();
            for (const TrayIcon* t : TrayIconsFor(*tray, host_.DockCfg(), e, true)) shown_.push_back(t->id);
        }
        const size_t capacity = along * across;  // (16B: across may be fewer than asked, above)
        // The overflow button: more icons than cells, or icons the grid leaves out (hidden).
        overflow_ = shown_.size() > capacity || all_ > shown_.size();
        if (overflow_ && shown_.size() >= capacity) shown_.resize(capacity - 1);
        cells_ = shown_.size() + (overflow_ ? 1 : 0);
        const size_t n = std::max<size_t>(cells_, 1);
        used_ = std::min<size_t>(along, (n + across - 1) / across);
        const size_t usedAcross = (n + used_ - 1) / used_;
        const float main = static_cast<float>(used_) * cell_;
        const float cross = static_cast<float>(usedAcross) * cell_;
        size_ = vertical_ ? D2D1::SizeU(static_cast<UINT32>(cross), static_cast<UINT32>(main))
                          : D2D1::SizeU(static_cast<UINT32>(main), static_cast<UINT32>(cross));
        return {main, cross};
    }

    HRESULT Render(Renderer& r, Layer& content, const LayoutContext&) override {
        TrayHost* tray = host_.Tray();
        const ThemeConfig& theme = host_.DockTheme();
        const DockEdge edge = host_.DockCfg().edge;
        return r.DrawLayer(content, size_.width, size_.height, [&](ID2D1DeviceContext* dc) {
            for (size_t c = 0; c < shown_.size(); ++c) {
                const TrayIcon* icon = tray ? tray->ById(shown_[c]) : nullptr;
                ID2D1Bitmap* bmp = icon ? host_.TrayBitmap(*icon) : nullptr;
                if (!bmp) continue;
                const D2D1_RECT_F cell = CellRect(c);
                const float x = std::round(cell.left + (cell_ - iconPx_) / 2.0f);
                const float y = std::round(cell.top + (cell_ - iconPx_) / 2.0f);
                const D2D1_RECT_F dest = D2D1::RectF(x, y, x + iconPx_, y + iconPx_);
                dc->DrawBitmap(bmp, &dest, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
            }
            winrt::com_ptr<ID2D1SolidColorBrush> brush;
            if (FAILED(dc->CreateSolidColorBrush(Theme::ToColor(theme.clockText, 0.85f), brush.put()))) return;
            if (overflow_) {
                // A chevron pointing away from the screen edge (where the drawer opens).
                const D2D1_RECT_F cell = CellRect(cells_ - 1);
                const float cx = (cell.left + cell.right) / 2.0f, cy = (cell.top + cell.bottom) / 2.0f;
                const float a = std::round(cell_ * 0.18f), b = std::round(cell_ * 0.09f);
                const float w = std::max(1.0f, std::round(1.5f * scale_));
                D2D1_POINT_2F p0, p1, p2;
                switch (edge) {
                    case DockEdge::Top:  // down
                        p0 = {cx - a, cy - b}, p1 = {cx, cy + b}, p2 = {cx + a, cy - b};
                        break;
                    case DockEdge::Left:  // right
                        p0 = {cx - b, cy - a}, p1 = {cx + b, cy}, p2 = {cx - b, cy + a};
                        break;
                    case DockEdge::Right:  // left
                        p0 = {cx + b, cy - a}, p1 = {cx - b, cy}, p2 = {cx + b, cy + a};
                        break;
                    case DockEdge::Bottom:
                    default:  // up
                        p0 = {cx - a, cy + b}, p1 = {cx, cy - b}, p2 = {cx + a, cy + b};
                        break;
                }
                dc->DrawLine(p0, p1, brush.get(), w);
                dc->DrawLine(p1, p2, brush.get(), w);
            } else if (shown_.empty()) {
                // No notify icon yet (or the tray is off): a dim dot keeps the slot findable.
                brush->SetOpacity(0.35f);
                const float rad = std::max(1.5f, std::round(2.0f * scale_));
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cell_ / 2.0f, cell_ / 2.0f), rad, rad), brush.get());
            }
        });
    }

    // --- cells ---
    int CellAt(int slot, POINT screen, std::wstring* label, float* centre) const override {
        const D2D1_RECT_F box = host_.SlotScreenBox(slot);
        const float lx = static_cast<float>(screen.x) - box.left, ly = static_cast<float>(screen.y) - box.top;
        const float along = vertical_ ? ly : lx, across = vertical_ ? lx : ly;
        if (along < 0.0f || across < 0.0f || cell_ <= 0.0f) return -1;
        const size_t m = static_cast<size_t>(along / cell_), k = static_cast<size_t>(across / cell_);
        if (m >= used_) return -1;
        const size_t c = k * used_ + m;
        if (c >= cells_) return -1;
        if (centre) *centre = (static_cast<float>(m) + 0.5f) * cell_;
        if (label) {
            label->clear();
            if (c < shown_.size()) {
                const TrayHost* tray = host_.Tray();
                const TrayIcon* icon = tray ? tray->ById(shown_[c]) : nullptr;
                if (icon && icon->StandardTip()) *label = LabelOf(icon->tip);
            } else {
                *label = L"Show all tray icons";
            }
        }
        return static_cast<int>(c);
    }

    void OnCellHover(int, int cell, CellEvent e) override {
        TrayHost* tray = host_.Tray();
        if (!tray || cell < 0 || static_cast<size_t>(cell) >= shown_.size()) return;
        const uint64_t id = shown_[static_cast<size_t>(cell)];
        const POINT pt = MessagePoint();
        switch (e) {
            case CellEvent::Enter:
                g_lastMoveAt = GetTickCount64();
                tray->MouseMove(id, pt);
                break;
            case CellEvent::Move:
                if (GetTickCount64() - g_lastMoveAt >= kMoveForwardMs) {
                    g_lastMoveAt = GetTickCount64();
                    tray->MouseMove(id, pt);
                }
                break;
            case CellEvent::Dwell:
                tray->PopupOpen(id, pt);  // version 4 without NIF_SHOWTIP only
                break;
            case CellEvent::Leave:
                tray->PopupClose(id, pt);
                break;
        }
    }

    // A press on the overflow button while the drawer is open closes it (the click toggles).
    bool OnPress(int slot) override {
        pressCell_ = CellAt(slot, MessagePoint(), nullptr, nullptr);
        return host_.PopupShownFor(slot) && overflow_ && pressCell_ == static_cast<int>(cells_) - 1;
    }

    void OnClick(int slot, bool pressClosedPopup) override {
        const POINT pt = MessagePoint();
        const int cell = CellAt(slot, pt, nullptr, nullptr);
        if (cell < 0 || cell != pressCell_) return;
        if (static_cast<size_t>(cell) >= shown_.size()) {
            if (!pressClosedPopup) host_.ShowClickPopup(slot);  // the drawer
            return;
        }
        TrayHost* tray = host_.Tray();
        if (!tray) return;
        const uint64_t id = shown_[static_cast<size_t>(cell)];
        const ULONGLONG now = GetTickCount64();
        const bool dbl = id == g_lastClickId && now - g_lastClickAt <= GetDoubleClickTime() &&
                         std::abs(pt.x - g_lastClickPt.x) <= GetSystemMetrics(SM_CXDOUBLECLK) &&
                         std::abs(pt.y - g_lastClickPt.y) <= GetSystemMetrics(SM_CYDOUBLECLK);
        tray->Forward(id, dbl ? TrayHost::Click::Double : TrayHost::Click::Left, pt);
        g_lastClickId = dbl ? 0 : id;
        g_lastClickAt = now;
        g_lastClickPt = pt;
    }

    void OnMiddleClick(int slot) override {
        const POINT pt = MessagePoint();
        const int cell = CellAt(slot, pt, nullptr, nullptr);
        TrayHost* tray = host_.Tray();
        if (tray && cell >= 0 && static_cast<size_t>(cell) < shown_.size()) {
            tray->Forward(shown_[static_cast<size_t>(cell)], TrayHost::Click::Middle, pt);
        }
    }

    // A right click on an icon is the app's (its menu); Ctrl + right click on an icon the tray
    // menu (hide it, pin it); elsewhere (overflow button, between cells): the dock's menu.
    bool OnRightClickAt(int slot, bool ctrl) override {
        const POINT pt = MessagePoint();
        const int cell = CellAt(slot, pt, nullptr, nullptr);
        TrayHost* tray = host_.Tray();
        if (!tray || cell < 0 || static_cast<size_t>(cell) >= shown_.size()) return false;
        if (ctrl) {
            if (const TrayIcon* icon = tray->ById(shown_[static_cast<size_t>(cell)])) host_.ShowTrayMenu(slot, icon->key, false);
            return true;
        }
        tray->Forward(shown_[static_cast<size_t>(cell)], TrayHost::Click::Right, pt);
        return true;
    }
    std::wstring TrayKeyAt(int slot, POINT screen) const override {
        const int cell = CellAt(slot, screen, nullptr, nullptr);
        const TrayHost* tray = host_.Tray();
        if (!tray || cell < 0 || static_cast<size_t>(cell) >= shown_.size()) return {};
        const TrayIcon* icon = tray->ById(shown_[static_cast<size_t>(cell)]);
        return icon ? icon->key : std::wstring();
    }
    RightClickAction OnRightClick(int) const override { return RightClickAction::DockMenu; }

    bool TrayIconRect(int slot, uint64_t id, RECT& screen) const override {
        size_t c = SIZE_MAX;
        for (size_t i = 0; i < shown_.size(); ++i) {
            if (shown_[i] == id) c = i;
        }
        if (c == SIZE_MAX) {
            if (!overflow_) return false;
            c = cells_ - 1;  // in the drawer only: the overflow button (Explorer answers its chevron)
        }
        const D2D1_RECT_F box = host_.SlotScreenBox(slot);
        const D2D1_RECT_F r = CellRect(c);
        screen = RECT{static_cast<LONG>(box.left + r.left), static_cast<LONG>(box.top + r.top),
                      static_cast<LONG>(box.left + r.right), static_cast<LONG>(box.top + r.bottom)};
        return true;
    }
    int TrayCells() const override { return static_cast<int>(shown_.size()); }
    bool TrayOverflow() const override { return overflow_; }

    ClickPopup Popup() const override { return ClickPopup::Tray; }
    // The drawer opens over the overflow button.
    float PopupAnchor(const DockItem& slot, const LayoutContext&) const override {
        if (!overflow_ || !used_) return slot.x + slot.slotW / 2.0f;
        const size_t m = (cells_ - 1) % used_;
        return slot.x + (static_cast<float>(m) + 0.5f) * cell_;
    }

private:
    // Cell c in the slot surface (physical orientation: main along x on a horizontal dock, y on a
    // vertical one).
    D2D1_RECT_F CellRect(size_t c) const {
        const float m = static_cast<float>(c % std::max<size_t>(used_, 1)) * cell_;
        const float k = static_cast<float>(c / std::max<size_t>(used_, 1)) * cell_;
        return vertical_ ? D2D1::RectF(k, m, k + cell_, m + cell_) : D2D1::RectF(m, k, m + cell_, k + cell_);
    }
    static POINT MessagePoint() {
        const DWORD pos = GetMessagePos();
        return POINT{static_cast<short>(LOWORD(pos)), static_cast<short>(HIWORD(pos))};
    }

    AppKey key_;
    bool vertical_ = false;
    float scale_ = 1.0f;
    float cell_ = 0.0f;    // px
    float iconPx_ = 0.0f;
    std::vector<uint64_t> shown_;  // icon ids in the grid, in cell order
    size_t all_ = 0;               // icons in the drawer
    bool overflow_ = false;
    size_t cells_ = 0;             // shown_ + the overflow button
    size_t used_ = 1;              // cells along the bar
    D2D1_SIZE_U size_{};
    int pressCell_ = -1;
};

// trayApp: one notify icon pinned as a dock icon. The slot's key is "kind:trayapp:<icon key>";
// the dock draws the picture (DockWindow::RenderItem) and keeps `missing` / the label in step with
// TrayHost (DockWindow::OnTrayChanged).
class TrayAppElement final : public DockElement {
public:
    TrayAppElement(ElementHost& host, const DockItemEntry& entry) : DockElement(host, entry) {
        key_.id = L"kind:trayapp:" + entry.id;
    }

    void AppendSlots(SlotBuilder& out) override {
        bool fresh = false;
        DockItem& item = out.Add(key_, &fresh);
        if (item.kind != DockItem::Kind::TrayApp) {
            item.kind = DockItem::Kind::TrayApp;
            item.dirty = true;
        }
        item.pinned = true;
        const TrayIcon* icon = Icon();
        const bool missing = icon == nullptr;
        if (missing != item.missing) item.dirty = true;
        item.missing = missing;
        item.displayName = icon ? icon->Title() : Entry().id + L" (not running)";
    }
    ElementSize Measure(const LayoutContext& ctx) override { return {ctx.iconPx, ctx.iconPx}; }

    void OnClick(int slot, bool) override { Forward(slot, TrayHost::Click::Left); }
    void OnMiddleClick(int slot) override { Forward(slot, TrayHost::Click::Middle); }
    bool OnRightClickAt(int slot, bool ctrl) override {
        if (ctrl || !Icon()) {
            host_.ShowTrayMenu(slot, Entry().id, true);  // Unpin (also while its app is not running)
            return true;
        }
        Forward(slot, TrayHost::Click::Right);
        return true;
    }
    bool TrayIconRect(int slot, uint64_t id, RECT& screen) const override {
        const TrayIcon* icon = Icon();
        if (!icon || icon->id != id) return false;
        const D2D1_RECT_F box = host_.SlotScreenBox(slot);
        screen = RECT{static_cast<LONG>(box.left), static_cast<LONG>(box.top), static_cast<LONG>(box.right),
                      static_cast<LONG>(box.bottom)};
        return true;
    }
    std::wstring TrayKeyAt(int, POINT) const override { return Entry().id; }

private:
    const TrayIcon* Icon() const {
        const TrayHost* tray = host_.Tray();
        const TrayIcon* icon = tray ? tray->ByKey(Entry().id) : nullptr;
        return icon && icon->Shown() ? icon : nullptr;
    }
    void Forward(int slot, TrayHost::Click click) {
        TrayHost* tray = host_.Tray();
        const TrayIcon* icon = Icon();
        if (!tray || !icon) {
            MessageBeep(MB_ICONINFORMATION);  // its app does not run
            return;
        }
        (void)slot;
        tray->Forward(icon->id, click, MessagePoint());
    }
    static POINT MessagePoint() {
        const DWORD pos = GetMessagePos();
        return POINT{static_cast<short>(LOWORD(pos)), static_cast<short>(HIWORD(pos))};
    }

    AppKey key_;
};

}  // namespace

std::vector<const TrayIcon*> TrayIconsFor(const TrayHost& host, const DockConfig& dock, const DockItemEntry& tray,
                                          bool inGrid) {
    std::vector<const TrayIcon*> out;
    std::vector<std::wstring> pinned;
    if (inGrid) {
        for (const DockItemEntry& e : dock.items) {
            if (e.Kind() == ItemKind::TrayApp) pinned.push_back(e.id);
        }
    }
    auto wanted = [&](const TrayIcon& t) {
        return t.Shown() && (!inGrid || (!Contains(tray.hidden, t.key) && !Contains(pinned, t.key)));
    };
    // The user's order first (8b), then everything else in registration order.
    for (const std::wstring& key : tray.order) {
        if (const TrayIcon* t = host.ByKey(key); t && wanted(*t) && std::find(out.begin(), out.end(), t) == out.end()) {
            out.push_back(t);
        }
    }
    for (const TrayIcon& t : host.Icons()) {
        if (wanted(t) && std::find(out.begin(), out.end(), &t) == out.end()) out.push_back(&t);
    }
    return out;
}

std::unique_ptr<DockElement> MakeTrayElement(ElementHost& host, const DockItemEntry& entry, size_t ordinal) {
    return std::make_unique<TrayElement>(host, entry, ordinal);
}

std::unique_ptr<DockElement> MakeTrayAppElement(ElementHost& host, const DockItemEntry& entry) {
    return std::make_unique<TrayAppElement>(host, entry);
}
