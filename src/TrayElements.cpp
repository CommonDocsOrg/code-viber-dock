// The tray grid (`tray` item, round 4 phase 8a): TrayHost's notify icons as a grid in one
// non-magnifying widget slot (like the clock). Input on a cell is forwarded to the icon's owner
// with the icon's version semantics (TrayHost::Forward); the arrow (the overflow button) opens the
// drawer (the dock's group popup in tray mode: every icon) when there are more icons than cells or
// some are hidden from the grid. A cell pressed and dragged is the icon's drag (the dock: onto the
// icon row it becomes a `trayApp` pin, onto the grid it moves there); Ctrl + right click on a cell
// opens the tray menu (8b).
//
// Round 24 phase 24A: the grid's look is the item's (iconSize 16 - 32, spacing, labels beside the
// icons, mono colours) and so is the arrow's (its place, look, and whether it always shows). The
// arrow's cell index stays shown_.size() whatever its place, so hit testing, OnClick and
// TrayOverflow read it as before; only where it is drawn changes (ArrowInGrid / Strip below).
//
// `trayApp` (8b): one notify icon as a full-size, magnifying icon slot (the dock draws its picture
// at the icon size; a placeholder while its app does not run); clicks go to the app like the
// grid's, Ctrl + right click opens the tray menu (Unpin).

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

#include "DockElement.h"

#include <d2d1effects.h>  // after <windows.h> (DockElement.h): it needs DEFINE_GUID
#include "Theme.h"
#include "TrayHost.h"

namespace {

constexpr ULONGLONG kMoveForwardMs = 100;  // WM_MOUSEMOVE to the owner at most this often
// Round 24 phase 24A: labels beside the grid's icons: a fixed width (so the grid does not jump as
// tips change) and the font; shown only where the cell is at least a line tall.
constexpr float kLabelWidthDip = 72.0f;
constexpr float kLabelFontDip = 12.0f;
constexpr float kLabelGapDip = 4.0f;    // between the icon and its label, and after the label
constexpr float kStripFraction = 0.6f;  // the arrow's strip ("before" / "after") along the bar, of a cell
// Icon sizes up to this keep the notify icon's own handle scaled by D2D, exactly as before round
// 24 (the default must look the same); above it the picture is fetched at the drawn size.
constexpr int kNativeIconMaxDip = 24;
// colour "mono": out = text colour x (kShade x luminance + kBase x coverage).
constexpr float kShade = 0.65f, kBase = 0.35f;

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
        if (ctx.scale != scale_) {
            labelFormat_ = nullptr;
            countFormat_ = nullptr;
        }
        scale_ = ctx.scale;
        const DockItemEntry& e = Entry();
        const int rows = std::clamp(e.rows, 1, 20);
        const int columns = std::clamp(e.columns, 1, 20);
        const int iconDip = std::clamp(e.trayIconSize, 16, 32);
        padDip_ = static_cast<float>(std::clamp(e.traySpacing, 0, 8));
        // Round 16 phase 16B: rows / columns as seen on screen, on every edge (rows stacked top to
        // bottom, columns left to right). On a bottom / top dock the rows run across the bar; on a
        // left / right dock the columns do. (Before, `rows` was always across and `columns` along,
        // so a vertical dock's grid looked transposed.)
        size_t along = static_cast<size_t>(vertical_ ? rows : columns);
        size_t across = static_cast<size_t>(vertical_ ? columns : rows);
        // Round 24 phase 24A: the cell is the icon and `spacing` DIPs on each side (3 before 24A).
        const float cellFull = std::round((static_cast<float>(iconDip) + 2.0f * padDip_) * ctx.scale);
        float cell = cellFull;
        // Labels beside the icons: to the right of each icon, so on a horizontal dock the cell
        // grows along the bar, on a vertical one across it (only when a labelled column fits the
        // room at full size: a thin side dock stays icons only).
        bool labels = e.trayLabels == TrayLabels::Beside;
        const float labelFull = labels ? std::round(kLabelWidthDip * ctx.scale) : 0.0f;
        // The room across: the box's cross room at rest inside the bar (a thick bar, titles beside
        // the icons: more than the icon row), else the icon row; unknown (0, 0): no limit. One or
        // two across always fit (shrunk, below); more only when they fit at full size, so a thin
        // bar never turns twenty rows into slivers.
        const float room = ctx.rowRoom > 0.0f ? ctx.rowRoom : ctx.iconPx;
        bool fitted = false;
        if (room > 0.0f && labels && vertical_) {
            const size_t fitLabelled = static_cast<size_t>(std::max(0.0f, std::floor(room / (cellFull + labelFull))));
            if (fitLabelled >= 1) {
                across = std::min(across, fitLabelled);  // full size cells, never shrunk with labels
                fitted = true;
            } else {
                labels = false;
            }
        }
        if (room > 0.0f && !fitted) {
            if (cellFull > 0.0f) {
                const size_t fit = static_cast<size_t>(std::max(0.0f, std::floor(room / cellFull)));
                across = std::min(across, std::max<size_t>(2, fit));
            }
            // The cells fit the room: two rows of large icons shrink to half an icon each.
            cell = std::min(cellFull, std::floor(room / static_cast<float>(across)));
        }
        cell_ = std::max(cell, 8.0f);
        // A shrunk cell keeps the icon 2 px smaller than itself (with no spacing: the icon is the cell).
        const float inset = std::min(2.0f, 2.0f * std::round(padDip_ * ctx.scale));
        iconPx_ = std::max(4.0f, std::min(std::round(static_cast<float>(iconDip) * ctx.scale), cell_ - inset));
        bitmapPx_ = iconDip > kNativeIconMaxDip ? static_cast<int>(iconPx_) : 0;
        // A label needs a cell at least a line tall (two shrunk rows on a thin bar: icons only).
        if (labels && cell_ < std::round(kLabelFontDip * 1.25f * ctx.scale)) labels = false;
        labelPx_ = labels ? labelFull : 0.0f;
        style_ = e.trayExpanderStyle;
        if (IDWriteFactory* dw = ctx.dwrite) EnsureFormats(dw, labels, iconDip);

        shown_.clear();
        all_ = 0;
        if (TrayHost* tray = host_.Tray()) {
            all_ = TrayIconsFor(*tray, host_.DockCfg(), e, false).size();
            for (const TrayIcon* t : TrayIconsFor(*tray, host_.DockCfg(), e, true)) shown_.push_back(t->id);
        }
        const size_t capacity = along * across;  // (16B: across may be fewer than asked, above)
        // The arrow: more icons than cells, icons the grid leaves out (hidden), or (24A) always.
        place_ = e.trayExpander;
        // A strip beside an empty grid would leave an empty cell: the arrow takes that cell.
        if ((place_ == TrayExpander::Before || place_ == TrayExpander::After) && shown_.empty()) place_ = TrayExpander::End;
        arrow_ = e.trayExpanderShow == TrayExpanderShow::Always || shown_.size() > capacity || all_ > shown_.size();
        if (arrow_ && ArrowInGrid()) {
            if (shown_.size() >= capacity) shown_.resize(capacity - 1);
        } else if (shown_.size() > capacity) {
            shown_.resize(capacity);  // the strip uses no cell
        }
        const size_t gridCells = shown_.size() + (arrow_ && ArrowInGrid() ? 1 : 0);
        const size_t n = std::max<size_t>(gridCells, 1);
        used_ = std::min<size_t>(along, (n + across - 1) / across);
        usedAcross_ = (n + used_ - 1) / used_;
        stripPx_ = arrow_ && !ArrowInGrid() ? std::max(6.0f, std::round(cell_ * kStripFraction)) : 0.0f;
        const float main = stripPx_ + static_cast<float>(used_) * AlongStep();
        const float cross = static_cast<float>(usedAcross_) * AcrossStep();
        size_ = vertical_ ? D2D1::SizeU(static_cast<UINT32>(cross), static_cast<UINT32>(main))
                          : D2D1::SizeU(static_cast<UINT32>(main), static_cast<UINT32>(cross));
        if (hotCell_ >= 0 && !ValidCell(static_cast<size_t>(hotCell_))) hotCell_ = -1;
        return {main, cross};
    }

    HRESULT Render(Renderer& r, Layer& content, const LayoutContext&) override {
        TrayHost* tray = host_.Tray();
        const ThemeConfig& theme = host_.DockTheme();
        const DockEdge edge = host_.DockCfg().edge;
        const bool mono = Entry().trayColour == TrayColour::Mono;
        return r.DrawLayer(content, size_.width, size_.height, [&](ID2D1DeviceContext* dc) {
            // Round 24 phase 24A: a plate under the hovered cell (an icon or the arrow), in the
            // popups' hover colours; redrawn only when the hot cell changes (DockWindow::SetHoverCell).
            if (hotCell_ >= 0 && ValidCell(static_cast<size_t>(hotCell_))) {
                winrt::com_ptr<ID2D1SolidColorBrush> fill, border;
                if (SUCCEEDED(dc->CreateSolidColorBrush(Theme::ToColor(theme.previewHotBackground), fill.put())) &&
                    SUCCEEDED(dc->CreateSolidColorBrush(Theme::ToColor(theme.previewHotBorder), border.put()))) {
                    const D2D1_RECT_F c = CellRect(static_cast<size_t>(hotCell_));
                    const float radius = std::round(4.0f * scale_);
                    const D2D1_ROUNDED_RECT plate{D2D1::RectF(c.left + 0.5f, c.top + 0.5f, c.right - 0.5f, c.bottom - 0.5f), radius, radius};
                    dc->FillRoundedRectangle(plate, fill.get());
                    dc->DrawRoundedRectangle(plate, border.get(), 1.0f);
                }
            }
            // Round 24 phase 24A: "mono": a colour matrix per icon (premultiplied, as
            // IconEffects::DrawGlow: straight alpha turns transparent pixels' 0 / 0 into NaN). One
            // effect each: an effect's input is not safely changed while earlier draws of it may
            // still be batched.
            auto makeTint = [&]() -> winrt::com_ptr<ID2D1Effect> {
                winrt::com_ptr<ID2D1Effect> tint;
                if (!mono || FAILED(dc->CreateEffect(CLSID_D2D1ColorMatrix, tint.put()))) return nullptr;
                const uint32_t argb = theme.clockText;
                const float ta = static_cast<float>((argb >> 24) & 0xFF) / 255.0f;
                const float tr = static_cast<float>((argb >> 16) & 0xFF) / 255.0f * ta;
                const float tg = static_cast<float>((argb >> 8) & 0xFF) / 255.0f * ta;
                const float tb = static_cast<float>(argb & 0xFF) / 255.0f * ta;
                // out = text colour x (0.65 x luminance + 0.35 x coverage): the shape in the
                // text colour with the icon's light and dark kept as shading.
                auto row = [&](float w) { return std::array<float, 3>{kShade * w * tr, kShade * w * tg, kShade * w * tb}; };
                const auto rr = row(0.299f), gg = row(0.587f), bb = row(0.114f);
                tint->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX,
                               D2D1::Matrix5x4F(rr[0], rr[1], rr[2], 0.0f,                          // R in
                                                gg[0], gg[1], gg[2], 0.0f,                          // G in
                                                bb[0], bb[1], bb[2], 0.0f,                          // B in
                                                kBase * tr, kBase * tg, kBase * tb, ta,             // A in
                                                0.0f, 0.0f, 0.0f, 0.0f));                           // bias
                tint->SetValue(D2D1_COLORMATRIX_PROP_ALPHA_MODE, D2D1_COLORMATRIX_ALPHA_MODE_PREMULTIPLIED);
                tint->SetValue(D2D1_COLORMATRIX_PROP_CLAMP_OUTPUT, TRUE);
                return tint;
            };
            winrt::com_ptr<ID2D1SolidColorBrush> textBrush;
            if (labelPx_ > 0.0f && labelFormat_) dc->CreateSolidColorBrush(Theme::ToColor(theme.clockText), textBrush.put());
            for (size_t c = 0; c < shown_.size(); ++c) {
                const TrayIcon* icon = tray ? tray->ById(shown_[c]) : nullptr;
                ID2D1Bitmap* bmp = icon ? host_.TrayBitmap(*icon, bitmapPx_) : nullptr;
                if (!bmp) continue;
                const D2D1_RECT_F cell = CellRect(c);
                const float x = std::round(cell.left + (cell_ - iconPx_) / 2.0f);
                const float y = std::round(cell.top + (cell_ - iconPx_) / 2.0f);
                const D2D1_SIZE_F bs = bmp->GetSize();
                winrt::com_ptr<ID2D1Effect> tint = bs.width > 0.0f && bs.height > 0.0f ? makeTint() : nullptr;
                if (tint) {
                    tint->SetInput(0, bmp);
                    D2D1::Matrix3x2F base;
                    dc->GetTransform(&base);
                    dc->SetTransform(D2D1::Matrix3x2F::Scale(iconPx_ / bs.width, iconPx_ / bs.height) *
                                     D2D1::Matrix3x2F::Translation(x, y) * base);
                    dc->DrawImage(tint.get(), D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
                    dc->SetTransform(base);
                } else {
                    const D2D1_RECT_F dest = D2D1::RectF(x, y, x + iconPx_, y + iconPx_);
                    dc->DrawBitmap(bmp, &dest, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
                }
                if (textBrush) {
                    const std::wstring title = icon->Title();
                    const float gap = std::round(kLabelGapDip * scale_);
                    const D2D1_RECT_F text = D2D1::RectF(x + iconPx_ + gap, cell.top, cell.right - gap, cell.bottom);
                    if (!title.empty() && text.right > text.left) {
                        dc->DrawText(title.c_str(), static_cast<UINT32>(title.size()), labelFormat_.get(), text, textBrush.get(),
                                     D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    }
                }
            }
            winrt::com_ptr<ID2D1SolidColorBrush> brush;
            if (FAILED(dc->CreateSolidColorBrush(Theme::ToColor(theme.clockText, 0.85f), brush.put()))) return;
            if (arrow_) {
                DrawArrow(dc, brush.get(), edge);
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
        size_t c = SIZE_MAX;
        float mid = 0.0f;
        if (Strip() && along >= StripStart() && along < StripStart() + stripPx_) {
            // The arrow's strip ("before" / "after"): the grid's whole extent across.
            if (across >= static_cast<float>(usedAcross_) * AcrossStep()) return -1;
            c = ArrowIndex();
            mid = StripStart() + stripPx_ / 2.0f;
        } else {
            const float inGrid = along - GridStart();
            if (inGrid < 0.0f) return -1;
            const size_t m = static_cast<size_t>(inGrid / AlongStep()), k = static_cast<size_t>(across / AcrossStep());
            if (m >= used_) return -1;
            c = CellOfPos(k * used_ + m);
            if (c == SIZE_MAX) return -1;
            mid = GridStart() + (static_cast<float>(m) + 0.5f) * AlongStep();
        }
        if (centre) *centre = mid;
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
        // Round 24 phase 24A: the hot cell for the plate (the arrow's too); the dock redraws the
        // slot when it changes.
        if (e == CellEvent::Enter) hotCell_ = cell;
        if (e == CellEvent::Leave && hotCell_ == cell) hotCell_ = -1;
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

    // A press on the arrow while the drawer is open closes it (the click toggles).
    bool OnPress(int slot) override {
        pressCell_ = CellAt(slot, MessagePoint(), nullptr, nullptr);
        return host_.PopupShownFor(slot) && arrow_ && pressCell_ == static_cast<int>(ArrowIndex());
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
    // menu (hide it, pin it); elsewhere (the arrow, between cells): the dock's menu.
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
    // The grid's drop (DockWindow::OnDrop, DropMode::TrayOrder) inserts before the key under the
    // pointer: CellAt's mapping, so with the arrow at the start or in a strip the drop still lands
    // on the icon drawn there (the arrow and the strip: no key, the end of the order).
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
            if (!arrow_) return false;
            c = ArrowIndex();  // in the drawer only: the arrow (Explorer answers its chevron)
        }
        const D2D1_RECT_F box = host_.SlotScreenBox(slot);
        const D2D1_RECT_F r = CellRect(c);
        screen = RECT{static_cast<LONG>(box.left + r.left), static_cast<LONG>(box.top + r.top),
                      static_cast<LONG>(box.left + r.right), static_cast<LONG>(box.top + r.bottom)};
        return true;
    }
    int TrayCells() const override { return static_cast<int>(shown_.size()); }
    bool TrayOverflow() const override { return arrow_; }
    bool TrayGridShows(uint64_t id) const override { return std::find(shown_.begin(), shown_.end(), id) != shown_.end(); }
    bool TrayLook(TrayLookState& out) const override {
        out.place = static_cast<int>(place_);
        out.style = static_cast<int>(style_);
        out.arrow = arrow_;
        out.cellAlong = static_cast<int>(AlongStep());
        out.cellAcross = static_cast<int>(AcrossStep());
        out.iconPx = static_cast<int>(iconPx_);
        out.hotCell = hotCell_;
        out.labels = labelPx_ > 0.0f;
        out.arrowRect = arrow_ ? CellRect(ArrowIndex()) : D2D1_RECT_F{};
        return true;
    }

    ClickPopup Popup() const override { return ClickPopup::Tray; }
    // The drawer opens over the arrow.
    float PopupAnchor(const DockItem& slot, const LayoutContext&) const override {
        if (!arrow_ || !used_) return slot.x + slot.slotW / 2.0f;
        const D2D1_RECT_F r = CellRect(ArrowIndex());
        return slot.x + (vertical_ ? (r.top + r.bottom) : (r.left + r.right)) / 2.0f;
    }

private:
    // --- the cell mapping (round 24 phase 24A) ---
    // Cell indices: 0 .. shown_.size() - 1 the icons, shown_.size() the arrow. Grid positions run
    // along the bar first (used_ along, then the next row / column across). "end": the arrow is
    // the position after the icons (as before 24A); "start": position 0, the icons one later;
    // "before" / "after": no position, a strip of stripPx_ before / after the grid along the bar.
    size_t ArrowIndex() const { return shown_.size(); }
    bool ArrowInGrid() const { return place_ == TrayExpander::End || place_ == TrayExpander::Start; }
    bool Strip() const { return stripPx_ > 0.0f; }
    bool ValidCell(size_t c) const { return c < shown_.size() || (arrow_ && c == ArrowIndex()); }
    // Px per cell along / across the bar (labels widen the cell on the physical x axis).
    float CellW() const { return cell_ + labelPx_; }
    float AlongStep() const { return vertical_ ? cell_ : CellW(); }
    float AcrossStep() const { return vertical_ ? CellW() : cell_; }
    float GridStart() const { return Strip() && place_ == TrayExpander::Before ? stripPx_ : 0.0f; }
    float StripStart() const { return place_ == TrayExpander::Before ? 0.0f : static_cast<float>(used_) * AlongStep(); }
    size_t PosOf(size_t c) const {
        if (c >= shown_.size()) return place_ == TrayExpander::Start ? 0 : shown_.size();
        return arrow_ && place_ == TrayExpander::Start ? c + 1 : c;
    }
    // The cell at grid position p (SIZE_MAX: none).
    size_t CellOfPos(size_t p) const {
        if (arrow_ && ArrowInGrid() && p == PosOf(ArrowIndex())) return ArrowIndex();
        const size_t shift = arrow_ && place_ == TrayExpander::Start ? 1 : 0;
        if (p < shift || p - shift >= shown_.size()) return SIZE_MAX;
        return p - shift;
    }
    // Along / across rect to the slot surface (physical: main along x on a horizontal dock, y on a
    // vertical one).
    D2D1_RECT_F Rect(float along, float across, float alongLen, float acrossLen) const {
        return vertical_ ? D2D1::RectF(across, along, across + acrossLen, along + alongLen)
                         : D2D1::RectF(along, across, along + alongLen, across + acrossLen);
    }
    // Cell c in the slot surface.
    D2D1_RECT_F CellRect(size_t c) const {
        if (c >= shown_.size() && Strip()) {
            return Rect(StripStart(), 0.0f, stripPx_, static_cast<float>(usedAcross_) * AcrossStep());
        }
        const size_t used = std::max<size_t>(used_, 1);
        const size_t p = PosOf(c);
        const float m = static_cast<float>(p % used) * AlongStep();
        const float k = static_cast<float>(p / used) * AcrossStep();
        return Rect(GridStart() + m, k, AlongStep(), AcrossStep());
    }

    void EnsureFormats(IDWriteFactory* dw, bool labels, int iconDip) {
        if (labels && !labelFormat_) {
            if (SUCCEEDED(dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                               DWRITE_FONT_STRETCH_NORMAL, kLabelFontDip * scale_, L"", labelFormat_.put()))) {
                labelFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                labelFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                winrt::com_ptr<IDWriteInlineObject> sign;
                if (SUCCEEDED(dw->CreateEllipsisTrimmingSign(labelFormat_.get(), sign.put()))) {
                    const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                    labelFormat_->SetTrimming(&trimming, sign.get());
                }
            } else {
                labelFormat_ = nullptr;
            }
        }
        // "+N": about half the icon's height, semi-bold, centred in the arrow's cell or strip.
        const float countPx = std::round(std::max(9.0f, static_cast<float>(iconDip) * 0.55f) * scale_);
        if (style_ == TrayExpanderStyle::Count && (!countFormat_ || countPx != countPx_)) {
            countFormat_ = nullptr;
            if (SUCCEEDED(dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                               DWRITE_FONT_STRETCH_NORMAL, countPx, L"", countFormat_.put()))) {
                countFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                countFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                countFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                countPx_ = countPx;
            } else {
                countFormat_ = nullptr;
            }
        }
    }

    // The arrow in its cell or strip: a chevron pointing away from the screen edge (where the
    // drawer opens; toward the edge while this grid's drawer is open), three dots along the
    // rect's long side, or "+N" (N: the icons the grid does not show; with none, as expanderShow
    // "always" can have it, the chevron).
    void DrawArrow(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* brush, DockEdge edge) const {
        const D2D1_RECT_F cell = CellRect(ArrowIndex());
        const float cx = (cell.left + cell.right) / 2.0f, cy = (cell.top + cell.bottom) / 2.0f;
        const size_t hiddenCount = all_ > shown_.size() ? all_ - shown_.size() : 0;
        if (style_ == TrayExpanderStyle::Dots) {
            const float rad = std::max(1.0f, std::round(1.25f * scale_));
            const float step = std::max(rad * 2.0f + 1.0f, std::round(cell_ * 0.16f));
            const bool wide = cell.right - cell.left >= cell.bottom - cell.top;
            for (int i = -1; i <= 1; ++i) {
                const float d = static_cast<float>(i) * step;
                dc->FillEllipse(D2D1::Ellipse(wide ? D2D1::Point2F(cx + d, cy) : D2D1::Point2F(cx, cy + d), rad, rad), brush);
            }
            return;
        }
        if (style_ == TrayExpanderStyle::Count && hiddenCount > 0 && countFormat_) {
            const std::wstring text = L"+" + std::to_wstring(hiddenCount);
            dc->DrawText(text.c_str(), static_cast<UINT32>(text.size()), countFormat_.get(), cell, brush);
            return;
        }
        const float a = std::round(cell_ * 0.18f), b = std::round(cell_ * 0.09f);
        const float w = std::max(1.0f, std::round(1.5f * scale_));
        // Round 24 phase 24A: flipped while the drawer is open (it points back where it closes).
        DockEdge toward = edge;
        if (host_.TrayDrawerShown(key_)) {
            toward = edge == DockEdge::Top ? DockEdge::Bottom : edge == DockEdge::Left ? DockEdge::Right
                   : edge == DockEdge::Right ? DockEdge::Left : DockEdge::Top;
        }
        D2D1_POINT_2F p0, p1, p2;
        switch (toward) {
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
        dc->DrawLine(p0, p1, brush, w);
        dc->DrawLine(p1, p2, brush, w);
    }

    static POINT MessagePoint() {
        const DWORD pos = GetMessagePos();
        return POINT{static_cast<short>(LOWORD(pos)), static_cast<short>(HIWORD(pos))};
    }

    AppKey key_;
    bool vertical_ = false;
    float scale_ = 0.0f;   // 0 until the first Measure (the text formats are made for a scale)
    float padDip_ = 3.0f;  // `spacing`
    float cell_ = 0.0f;    // px: the icon's square (the icon + spacing on each side)
    float labelPx_ = 0.0f; // px added to the cell's width by a label beside the icon (0: none)
    float iconPx_ = 0.0f;
    int bitmapPx_ = 0;     // the pictures' pixel size asked of the dock (0: the handle as it is)
    std::vector<uint64_t> shown_;  // icon ids in the grid, in cell order
    size_t all_ = 0;               // icons in the drawer
    bool arrow_ = false;           // the arrow is shown (cell ArrowIndex())
    TrayExpander place_ = TrayExpander::End;  // where it is drawn (a strip next to an empty grid: End)
    TrayExpanderStyle style_ = TrayExpanderStyle::Chevron;
    float stripPx_ = 0.0f;         // the arrow's strip along the bar (0: the arrow is a grid cell, or none)
    size_t used_ = 1;              // cells along the bar
    size_t usedAcross_ = 1;        // rows (columns on a vertical dock) used across it
    D2D1_SIZE_U size_{};
    int pressCell_ = -1;
    int hotCell_ = -1;             // the cell under the pointer (the plate), -1 none
    winrt::com_ptr<IDWriteTextFormat> labelFormat_;
    winrt::com_ptr<IDWriteTextFormat> countFormat_;
    float countPx_ = 0.0f;
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
