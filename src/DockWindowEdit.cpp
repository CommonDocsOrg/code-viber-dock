// Round 12 phase 12D: customize mode on the dock (DockWindow.h, "customize mode"). The dock draws
// the mode and takes its input; App owns it (enter / leave, the edit bar, the drawer popup, the
// config edits and their undo labels). Every visual here is drawn once per layout (or per hover /
// anchor change) and placed with static offsets: nothing animates or ticks at rest; only the long
// press arms a one-shot timer.

#include <windows.h>

#include <algorithm>
#include <cmath>

#include "DockWindow.h"
#include "Theme.h"

namespace {

constexpr float kOutlineGapDip = 6.0f;     // the dashed outline: this far outside the bar
constexpr float kOutlineStrokeDip = 2.0f;  // ... this thick
constexpr float kBadgeDip = 16.0f;         // the remove badge's disc
constexpr float kBadgeInsetDip = 4.0f;     // its centre this far inside the item box's top-left corner
constexpr float kAnchorDip = 20.0f;        // an anchor target's disc
constexpr float kAnchorGapDip = 10.0f;     // ... this far outside the bar
constexpr float kAnchorHitDip = 16.0f;     // the pointer within this radius of its centre is on it
constexpr int kStillDip = 4;               // a long press: the pointer moved at most this far

bool Contains(const D2D1_RECT_F& r, POINT pt) {
    const float x = static_cast<float>(pt.x) + 0.5f, y = static_cast<float>(pt.y) + 0.5f;
    return r.right > r.left && x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

}  // namespace

void DockWindow::SetEditing(bool on) {
    if (on == editing_ || !hwnd_) return;
    editing_ = on;
    CancelEditPress();
    editHover_ = editPressBadge_ = editPressPlus_ = -1;
    SetAnchorHot(-1);
    if (on) {
        // The dock is shown and stays so (auto-hide held), its popups go, nothing magnifies.
        DismissPreview(false, false);
        DismissClickPopups(true, false);
        DismissWidgetPassive(false);
        TakePopupHold(kHoldEdit);
        if ((hideState_ == HideState::Hidden || hideState_ == HideState::Hiding) && !RevealSuppressed()) StartShow();
        magnifying_ = false;
        grown_ = false;
        editDirty_ = true;
        editFrameShown_ = RECT{};
        // The outline and the anchor targets are beside the bar: the window keeps the stage.
        if (!expanded_) {
            expanded_ = true;
            UpdateWindowRect();
        }
    } else {
        DestroyEditVisuals();
    }
    Relayout();  // the "+" slots' room comes / goes (and the stage room for the anchor targets)
    if (!on) {
        ReleasePopupHold(kHoldEdit, true);  // auto-hide again once the pointer is gone
        TryCompact();
    }
}

void DockWindow::SetDrawerHold(bool on) {
    if (!hwnd_) return;
    if (on) {
        TakePopupHold(kHoldDrawer);
    } else {
        ReleasePopupHold(kHoldDrawer, true);
    }
}

int DockWindow::PlusCount() const {
    if (!editing_) return 0;
    return (layout_.plusPinSlot >= 0 ? 1 : 0) + (layout_.plusEnd ? 1 : 0);
}

bool DockWindow::ClickPlus(int which) {
    const D2D1_RECT_F box = PlusBox(which);
    if (!editing_ || box.right <= box.left) return false;
    if (editRequests_.plus) editRequests_.plus(which);
    return true;
}

bool DockWindow::EditRemovable(int slot) const {
    if (slot < 0 || slot >= static_cast<int>(items_.size())) return false;
    const DockItem& item = items_[static_cast<size_t>(slot)];
    // A pin (an app, folder, group, separator, the Recycle Bin, a tray icon) or a widget kind is a
    // config item of its own; a running app's slot and an unpinned window slot are the running
    // item's (no badge: removing it would take every running app away).
    return (item.pinned || item.IsWidget()) && item.element < DockCfg().items.size();
}

bool DockWindow::EditRemoveSlot(int slot) {
    if (!editing_ || !EditRemovable(slot)) return false;
    const size_t item = items_[static_cast<size_t>(slot)].element;
    editHover_ = -1;
    if (editRequests_.remove) editRequests_.remove(item);
    return true;
}

RECT DockWindow::EditFrameRect() const {
    if (layout_.polar.on) return RingScreenRect();
    const D2D1_RECT_F o = OutlineCanon();
    return StageToScreen(o.left, o.top, o.right, o.bottom, true);
}

D2D1_RECT_F DockWindow::OutlineCanon() const {
    const Layout& l = layout_;
    const float g = std::round((kOutlineGapDip + kOutlineStrokeDip) * l.scale);
    // Clamped to the stage: at a screen edge (a full-width or a floating bar) the edge stays visible.
    return D2D1::RectF(std::max(0.0f, l.bar.left - g), std::max(0.0f, l.bar.top - g), std::min(l.stageMain, l.bar.right + g),
                       std::min(l.stageCross, l.bar.bottom + g));
}

D2D1_RECT_F DockWindow::EditBadgeBox(int slot) const {
    if (slot < 0 || slot >= static_cast<int>(items_.size())) return D2D1_RECT_F{};
    const Layout& l = layout_;
    const DockItem& item = items_[static_cast<size_t>(slot)];
    const float top = item.IsWidget() ? item.y : l.itemTop;
    const D2D1_RECT_F box = l.BoxRect(item.targetX, top, item.targetX + BoxW(item), top + BoxCross(item));
    const float d = std::round(kBadgeDip * l.scale), inset = std::round(kBadgeInsetDip * l.scale);
    const float x = std::round(box.left + inset - d / 2.0f), y = std::round(box.top + inset - d / 2.0f);
    return D2D1::RectF(std::max(0.0f, x), std::max(0.0f, y), std::max(0.0f, x) + d, std::max(0.0f, y) + d);
}

D2D1_RECT_F DockWindow::PlusBox(int which) const {
    const Layout& l = layout_;
    if (!editing_ || l.polar.on) return D2D1_RECT_F{};
    float main = 0.0f;
    if (which == 0 && l.plusPinSlot >= 0) {
        main = l.plusPinMain;
    } else if (which == 1 && l.plusEnd) {
        main = l.plusEndMain;
    } else {
        return D2D1_RECT_F{};
    }
    return l.PhysRect(main, l.itemTop, main + l.iconPx, l.itemTop + l.iconPx);
}

D2D1_RECT_F DockWindow::AnchorBox(int k) const {
    const Layout& l = layout_;
    if (k < 0 || k > 2 || l.polar.on) return D2D1_RECT_F{};
    const float d = std::round(kAnchorDip * l.scale);
    const float v1 = std::max(d, l.bar.top - std::round(kAnchorGapDip * l.scale)), v0 = v1 - d;
    const float inset = l.padding + d / 2.0f;
    const float centre = k == 0 ? l.bar.left + inset : k == 1 ? std::round((l.bar.left + l.bar.right) / 2.0f) : l.bar.right - inset;
    const float u0 = std::round(centre - d / 2.0f);
    return l.PhysRect(u0, v0, u0 + d, v0 + d);
}

int DockWindow::AnchorAt(POINT stagePt) const {
    if (!editing_ || layout_.polar.on) return -1;
    const float r = kAnchorHitDip * layout_.scale;
    for (int k = 0; k < 3; ++k) {
        const D2D1_RECT_F b = AnchorBox(k);
        const float cx = (b.left + b.right) / 2.0f, cy = (b.top + b.bottom) / 2.0f;
        const float dx = static_cast<float>(stagePt.x) + 0.5f - cx, dy = static_cast<float>(stagePt.y) + 0.5f - cy;
        if (dx * dx + dy * dy <= r * r) return k;
    }
    return -1;
}

void DockWindow::SetAnchorHot(int k) {
    if (k == anchorHot_) return;
    anchorHot_ = k;
    if (!editing_ || !editAnchors_[0].visual) return;
    editDirty_ = true;  // the targets are drawn again (the hot one lit)
    RenderWithRecovery([this] {
        PlaceEditVisuals();
        return S_OK;
    });
}

void DockWindow::DestroyEditVisuals() {
    bool any = false;
    for (Layer* layer : {&editOutline_, &editBadge_, &editPlus_[0], &editPlus_[1], &editAnchors_[0], &editAnchors_[1], &editAnchors_[2]}) {
        if (!layer->visual) continue;
        renderer_.DestroyLayer(*layer);
        any = true;
    }
    editDirty_ = true;
    if (any) RenderWithRecovery([] { return S_OK; });  // commit the removal
}

HRESULT DockWindow::RenderEditVisuals() {
    const Layout& l = layout_;
    HRESULT hr = S_OK;
    for (Layer* layer : {&editOutline_, &editBadge_, &editPlus_[0], &editPlus_[1], &editAnchors_[0], &editAnchors_[1], &editAnchors_[2]}) {
        // In front of everything (created after the other layers; a device loss resets them all).
        if (!layer->visual && SUCCEEDED(hr)) {
            hr = renderer_.CreateLayer(*layer);
            if (SUCCEEDED(hr)) renderer_.SetVisible(*layer, false);
        }
    }
    if (FAILED(hr)) return hr;
    editDirty_ = false;
    const float s = l.scale;
    const D2D1_COLOR_F dock = Theme::ToColor(config_->DockColor(dockIndex_));
    winrt::com_ptr<ID2D1StrokeStyle> dashed;
    const D2D1_STROKE_STYLE_PROPERTIES props =
        D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_DASH);
    if (renderer_.D2DFactory()) renderer_.D2DFactory()->CreateStrokeStyle(props, nullptr, 0, dashed.put());
    // The outline: a dashed rounded rectangle in the dock's colour around the bar. A ring has none
    // (a dashed band would have to follow its arc; the edit bar names the dock instead).
    if (!l.polar.on) {
        const D2D1_RECT_F o = OutlineCanon();
        const float w = o.right - o.left, h = o.bottom - o.top;
        const float stroke = std::max(1.0f, std::round(kOutlineStrokeDip * s));
        const float radius = l.cornerRadius + std::round(kOutlineGapDip * s);
        hr = DrawCanonical(editOutline_, w, h, [&](ID2D1DeviceContext* dc) {
            winrt::com_ptr<ID2D1SolidColorBrush> brush;
            if (FAILED(dc->CreateSolidColorBrush(dock, brush.put()))) return;
            const D2D1_ROUNDED_RECT rr{D2D1::RectF(stroke / 2.0f, stroke / 2.0f, w - stroke / 2.0f, h - stroke / 2.0f), radius, radius};
            dc->DrawRoundedRectangle(rr, brush.get(), stroke, dashed.get());
        });
        if (FAILED(hr)) return hr;
    }
    // The remove badge: a dark disc with a white ×.
    {
        const UINT d = static_cast<UINT>(std::round(kBadgeDip * s));
        hr = renderer_.DrawLayer(editBadge_, d, d, [&](ID2D1DeviceContext* dc) {
            winrt::com_ptr<ID2D1SolidColorBrush> brush;
            if (FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(0.18f, 0.18f, 0.18f, 0.95f), brush.put()))) return;
            const float r = static_cast<float>(d) / 2.0f;
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r, r), r - 0.5f, r - 0.5f), brush.get());
            brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.9f));
            const float line = std::max(1.0f, std::round(1.5f * s));
            dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(r, r), r - line / 2.0f, r - line / 2.0f), brush.get(), std::max(1.0f, std::round(s)));
            const float a = r * 0.42f;
            dc->DrawLine(D2D1::Point2F(r - a, r - a), D2D1::Point2F(r + a, r + a), brush.get(), line);
            dc->DrawLine(D2D1::Point2F(r + a, r - a), D2D1::Point2F(r - a, r + a), brush.get(), line);
        });
        if (FAILED(hr)) return hr;
    }
    // The "+" slots: an icon-sized dashed square with a plus.
    for (Layer& plus : editPlus_) {
        const UINT d = static_cast<UINT>(std::max(1.0f, l.iconPx));
        hr = renderer_.DrawLayer(plus, d, d, [&](ID2D1DeviceContext* dc) {
            winrt::com_ptr<ID2D1SolidColorBrush> brush;
            if (FAILED(dc->CreateSolidColorBrush(dock, brush.put()))) return;
            const float stroke = std::max(1.0f, std::round(kOutlineStrokeDip * s));
            const float f = static_cast<float>(d);
            const float radius = std::round(8.0f * s);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(stroke / 2.0f, stroke / 2.0f, f - stroke / 2.0f, f - stroke / 2.0f), radius, radius),
                                     brush.get(), stroke, dashed.get());
            const float c = f / 2.0f, a = f * 0.22f;
            dc->DrawLine(D2D1::Point2F(c - a, c), D2D1::Point2F(c + a, c), brush.get(), stroke * 1.5f);
            dc->DrawLine(D2D1::Point2F(c, c - a), D2D1::Point2F(c, c + a), brush.get(), stroke * 1.5f);
        });
        if (FAILED(hr)) return hr;
    }
    // The anchor targets: start, centre, end discs in the dock's colour (the one under a dragged
    // item lit), each with a mark of where it puts the item.
    for (int k = 0; k < 3; ++k) {
        const UINT d = static_cast<UINT>(std::round(kAnchorDip * s));
        const bool hot = k == anchorHot_;
        hr = renderer_.DrawLayer(editAnchors_[k], d, d, [&](ID2D1DeviceContext* dc) {
            winrt::com_ptr<ID2D1SolidColorBrush> brush;
            if (FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(dock.r, dock.g, dock.b, hot ? 1.0f : 0.45f), brush.put()))) return;
            const float r = static_cast<float>(d) / 2.0f;
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r, r), r - 0.5f, r - 0.5f), brush.get());
            brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, hot ? 1.0f : 0.85f));
            const float line = std::max(1.0f, std::round(1.5f * s));
            dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(r, r), r - line / 2.0f, r - line / 2.0f), brush.get(), line);
            // A short bar at the side it anchors to (the middle for the centre).
            const float bw = r * 0.5f, x = k == 0 ? r * 0.55f : k == 1 ? r - bw / 2.0f : r * 1.45f - bw;
            dc->DrawLine(D2D1::Point2F(x, r), D2D1::Point2F(x + bw, r), brush.get(), line * 1.4f);
        });
        if (FAILED(hr)) return hr;
    }
    return S_OK;
}

void DockWindow::PlaceEditVisuals() {
    if (!editing_) {
        if (editOutline_.visual) DestroyEditVisuals();
        return;
    }
    if (editDirty_ || !editOutline_.visual) {
        if (FAILED(RenderEditVisuals())) return;
    }
    const Layout& l = layout_;
    const bool dragging = dragPhase_ == DragPhase::Dragging;
    const bool quiet = !dragging && !externalDrag_ && dropGap_ < 0;  // nothing moves the slots
    auto place = [&](Layer& layer, const D2D1_RECT_F& r, bool shown) {
        shown = shown && r.right > r.left;
        if (shown) renderer_.SetOffset(layer, std::round(r.left), std::round(r.top));
        renderer_.SetVisible(layer, shown);
    };
    if (!l.polar.on) {
        const D2D1_RECT_F o = OutlineCanon();
        place(editOutline_, l.PhysRect(o.left, o.top, o.right, o.bottom), true);
    } else {
        renderer_.SetVisible(editOutline_, false);
    }
    place(editBadge_, EditBadgeBox(editHover_), quiet && editHover_ >= 0 && EditRemovable(editHover_));
    for (int k = 0; k < 2; ++k) place(editPlus_[k], PlusBox(k), quiet);
    for (int k = 0; k < 3; ++k) place(editAnchors_[k], AnchorBox(k), dragging);
}

void DockWindow::UpdateEditHover(POINT stagePt) {
    int hit = dragPhase_ == DragPhase::None && InZone(stagePt) ? HitTest(stagePt) : -1;
    // The badge sticks out of its item a little: the pointer on it keeps it.
    if (editHover_ >= 0 && dragPhase_ == DragPhase::None && Contains(EditBadgeBox(editHover_), stagePt)) hit = editHover_;
    if (!EditRemovable(hit)) hit = -1;
    if (hit == editHover_) return;
    editHover_ = hit;
    RenderWithRecovery([this] {
        PlaceEditVisuals();
        return S_OK;
    });
}

bool DockWindow::EditPress(POINT stagePt) {
    editPressBadge_ = editPressPlus_ = -1;
    if (editHover_ >= 0 && EditRemovable(editHover_) && Contains(EditBadgeBox(editHover_), stagePt)) {
        editPressBadge_ = editHover_;
        return true;
    }
    for (int k = 0; k < 2; ++k) {
        if (Contains(PlusBox(k), stagePt)) {
            editPressPlus_ = k;
            return true;
        }
    }
    // Beside the bar (the dock's window covers its stage while customizing: what shows there is the
    // desktop or a window below): as a click on the desktop, customize mode ends.
    const D2D1_RECT_F o = OutlineCanon();
    const D2D1_RECT_F frame = layout_.polar.on ? layout_.ring : layout_.PhysRect(o.left, o.top, o.right, o.bottom);
    if (!InZone(stagePt) && !Contains(frame, stagePt)) {
        if (editRequests_.leave) editRequests_.leave();
        return true;
    }
    return false;
}

bool DockWindow::EditRelease(POINT stagePt) {
    if (editPressBadge_ >= 0) {
        const int slot = editPressBadge_;
        editPressBadge_ = -1;
        if (Contains(EditBadgeBox(slot), stagePt)) EditRemoveSlot(slot);  // the click: removed (undoable, no question)
        return true;
    }
    if (editPressPlus_ >= 0) {
        const int which = editPressPlus_;
        editPressPlus_ = -1;
        if (Contains(PlusBox(which), stagePt)) ClickPlus(which);
        return true;
    }
    return false;
}

void DockWindow::ArmEditPress(POINT stagePt) {
    if (!hwnd_ || !editRequests_.enter) return;
    editPressPt_ = stagePt;
    editPressPending_ = SetTimer(hwnd_, kEditPressTimerId, kEditPressMs, nullptr) != 0;
}

void DockWindow::CancelEditPress() {
    if (!editPressPending_) return;
    if (hwnd_) KillTimer(hwnd_, kEditPressTimerId);
    editPressPending_ = false;
}

void DockWindow::OnEditPressTimer() {
    KillTimer(hwnd_, kEditPressTimerId);
    editPressPending_ = false;
    if (editing_ || dragPhase_ != DragPhase::None || dockDrag_ || scrollDrag_ || hideState_ != HideState::Shown) return;
    // Still pressed (the primary button) and still where it started.
    const int button = GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;
    if (!(GetAsyncKeyState(button) & 0x8000)) return;
    POINT cursor{};
    if (!GetCursorPos(&cursor)) return;
    const POINT stage{cursor.x - layout_.window.left, cursor.y - layout_.window.top};
    const int still = MulDiv(kStillDip, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI);
    if (std::abs(stage.x - editPressPt_.x) > still || std::abs(stage.y - editPressPt_.y) > still) return;
    // The press is the mode's now: no dock move, no run drag, no click on release.
    dockPress_ = false;
    scrollPress_ = false;
    pressed_ = -1;
    if (GetCapture() == hwnd_) ReleaseCapture();
    if (editRequests_.enter) editRequests_.enter();
}

int DockWindow::ElementGapAt(float stageX) const {
    stageX = RestMain(stageX);
    const int n = static_cast<int>(items_.size());
    auto boundary = [&](int p) {
        if (p < n && !items_[static_cast<size_t>(p)].IsWidget()) return items_[static_cast<size_t>(p)].slotX;
        if (p > 0) return items_[static_cast<size_t>(p - 1)].slotX + items_[static_cast<size_t>(p - 1)].slotW;
        return layout_.itemsLeft;
    };
    int best = -1;
    float bestD = 0.0f;
    for (int p = 0; p <= n; ++p) {
        // Between two elements only (the running apps are one block; a pin's window slots stay with it).
        if (p > 0 && p < n && items_[static_cast<size_t>(p - 1)].element == items_[static_cast<size_t>(p)].element) continue;
        const float d = std::fabs(stageX - boundary(p));
        if (best < 0 || d <= bestD) {
            best = p;
            bestD = d;
        }
    }
    return best;
}

size_t DockWindow::ConfigIndexOfGap(int gap) const {
    const size_t count = DockCfg().items.size();
    if (gap >= 0 && static_cast<size_t>(gap) < items_.size()) return std::min(items_[static_cast<size_t>(gap)].element, count);
    return items_.empty() ? count : std::min(items_.back().element + 1, count);
}

bool DockWindow::ExternalItemOver(POINT screen) {
    if (!hwnd_ || hideState_ == HideState::Hidden || DockCfg().locked || dragPhase_ != DragPhase::None) {
        ExternalItemLeave();
        return false;
    }
    const POINT stage{screen.x - layout_.window.left, screen.y - layout_.window.top};
    if (!PtInRect(&winRect_, screen) || !InZone(stage)) {
        ExternalItemLeave();
        return false;
    }
    if (!externalDrag_) {
        // As a file dragged over the dock (OnDragEnter): held, no label, the stage for the gap.
        externalDrag_ = true;
        dropActive_ = true;
        dropMode_ = DropMode::None;
        dropItem_ = -1;
        SetHold(true);
        magnifying_ = false;
        SetHover(-1);
        if (!expanded_) {
            expanded_ = true;
            UpdateWindowRect();
        }
    }
    const int gap = items_.empty() ? -1 : ElementGapAt(layout_.Canon(stage).x);
    SetDropTarget(gap >= 0 ? DropMode::Insert : DropMode::None, -1, gap);
    return true;
}

void DockWindow::ExternalItemLeave() {
    if (!externalDrag_) return;
    externalDrag_ = false;
    SetDropTarget(DropMode::None, -1, -1);
    dropActive_ = false;
    KeepStage(motion_.magnify + 0.04);
    SetHold(false);
}

size_t DockWindow::ExternalItemDrop(POINT screen) {
    if (!ExternalItemOver(screen)) return SIZE_MAX;
    const size_t at = dropGap_ >= 0 ? ConfigIndexOfGap(dropGap_) : DockCfg().items.size();
    // The gap closes as the new item takes its slot (as a file drop: the rebuilt layout puts
    // everything where the gap layout had it).
    externalDrag_ = false;
    dropActive_ = false;
    dropMode_ = DropMode::None;
    dropGap_ = -1;
    renderer_.SetVisible(marker_, false);
    markerX_.valid = false;
    RenderWithRecovery([this] {
        ComputeTargets();
        ApplyTargets(motion_.rest);
        return S_OK;
    });
    KeepStage(motion_.rest + 0.04);
    SetHold(false);
    return at;
}
