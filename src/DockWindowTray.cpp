// DockWindow and the tray (round 4 phase 8): the tray elements draw TrayHost's icons and forward
// input to them (TrayElements.cpp); the dock adds the hover label per cell, the pictures on its
// device, the drawer (the group popup in tray mode listing every icon, opened from the grid's
// overflow button) and the icons' screen rects for Shell_NotifyIconGetRect.

#include <algorithm>
#include <string>
#include <vector>

#include "DockWindow.h"

namespace {
constexpr UINT kCellLabelDelayMs = 250;  // = DockWindow.cpp's kLabelDelayMs
constexpr wchar_t kDrawerTitle[] = L"Notification area";
}  // namespace

TrayHost* DockWindow::Tray() {
    return shared_ && shared_->TrayService().Running() ? &shared_->TrayService() : nullptr;
}

ID2D1Bitmap* DockWindow::TrayBitmap(const TrayIcon& icon) {
    if (!icon.icon) return nullptr;
    auto it = trayBitmaps_.find(icon.imageSerial);
    if (it != trayBitmaps_.end()) return it->second.get();
    ID2D1DeviceContext* dc = renderer_.ResourceContext();
    if (!dc) return nullptr;
    // Pictures of icons that changed or went: dropped once the cache holds twice the live ones.
    if (TrayHost* tray = Tray(); tray && trayBitmaps_.size() >= 2 * tray->Icons().size() + 8) {
        std::erase_if(trayBitmaps_, [&](const auto& kv) {
            return std::none_of(tray->Icons().begin(), tray->Icons().end(),
                                [&](const TrayIcon& t) { return t.imageSerial == kv.first; });
        });
    }
    winrt::com_ptr<ID2D1Bitmap> bitmap = icons_.FromHicon(icon.icon, dc);
    ID2D1Bitmap* raw = bitmap.get();
    trayBitmaps_.emplace(icon.imageSerial, std::move(bitmap));
    return raw;
}

void DockWindow::SetHoverCell(int slot, int cell, std::wstring label, float centre) {
    if (slot == hoverCellSlot_ && cell == hoverCell_) {
        if (slot >= 0 && elements_[items_[static_cast<size_t>(slot)].element]) {
            ElementOf(static_cast<size_t>(slot)).OnCellHover(slot, cell, CellEvent::Move);
        }
        if (label != hoverCellLabel_) {  // the app changed its tip while hovered
            hoverCellLabel_ = std::move(label);
            labelDirty_ = true;
        }
        return;
    }
    const int oldSlot = hoverCellSlot_, oldCell = hoverCell_;
    hoverCellSlot_ = slot;
    hoverCell_ = cell;
    hoverCellLabel_ = std::move(label);
    hoverCellCentre_ = centre;
    if (oldSlot >= 0 && oldSlot < static_cast<int>(items_.size()) && elements_[items_[static_cast<size_t>(oldSlot)].element]) {
        ElementOf(static_cast<size_t>(oldSlot)).OnCellHover(oldSlot, oldCell, CellEvent::Leave);
    }
    if (slot < 0) return;  // the caller sets the hover (SetHover) itself
    ElementOf(static_cast<size_t>(slot)).OnCellHover(slot, cell, CellEvent::Enter);
    if (hovered_ == slot) {
        // Another cell of the same slot: the label follows; without a label it hides and the
        // dwell starts again.
        labelDirty_ = true;
        if (labelShown_ && hoverCellLabel_.empty()) HideLabel();
        if (!labelShown_ && !labelPending_) {
            SetTimer(hwnd_, kLabelTimerId, kCellLabelDelayMs, nullptr);
            labelPending_ = true;
        }
        return;
    }
    SetHover(slot);
}

std::vector<const TrayIcon*> DockWindow::TrayDrawerIcons(int slot) const {
    if (slot < 0 || slot >= static_cast<int>(items_.size()) || items_[static_cast<size_t>(slot)].kind != DockItem::Kind::Tray ||
        !shared_ || !shared_->TrayService().Running() || !elements_[items_[static_cast<size_t>(slot)].element]) {
        return {};
    }
    return TrayIconsFor(shared_->TrayService(), DockCfg(), ElementOf(static_cast<size_t>(slot)).Entry(), false);
}

namespace {
// The drawer's members for `icons`, their ids, and a signature (ids, pictures, labels) that tells
// whether an open drawer must be refilled.
std::vector<FolderPopup::GroupEntry> DrawerEntries(const std::vector<const TrayIcon*>& icons,
                                                   const std::function<ID2D1Bitmap*(const TrayIcon&)>& bitmap,
                                                   std::vector<uint64_t>& ids, std::wstring& signature) {
    std::vector<FolderPopup::GroupEntry> entries;
    ids.clear();
    signature.clear();
    for (const TrayIcon* t : icons) {
        FolderPopup::GroupEntry entry;
        entry.label = t->Title();
        if (ID2D1Bitmap* bmp = bitmap(*t)) entry.icon.copy_from(bmp);
        signature += std::to_wstring(t->id) + L":" + std::to_wstring(t->imageSerial) + L":" + entry.label + L"|";
        entries.push_back(std::move(entry));
        ids.push_back(t->id);
    }
    return entries;
}
}  // namespace

void DockWindow::OnTrayChanged() {
    if (!hwnd_) return;
    DispatchEvent(DockEvent::TrayChanged);
    // A hovered cell may show another icon now: its label is re-read on the next mouse move.
    if (folder_.Kind() != 3) return;
    // An open drawer follows the icons (new ones, gone ones, new pictures / tips).
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].key != folderKey_ || items_[i].kind != DockItem::Kind::Tray) continue;
        if (!Tray()) break;
        std::vector<uint64_t> ids;
        std::wstring signature;
        auto entries = DrawerEntries(TrayDrawerIcons(static_cast<int>(i)), [this](const TrayIcon& t) { return TrayBitmap(t); },
                                     ids, signature);
        if (signature != drawerSignature_) {
            drawerSignature_ = std::move(signature);
            drawerIds_ = std::move(ids);
            folder_.UpdateGroup(kDrawerTitle, std::move(entries));
            RenderWithRecovery([] { return S_OK; });
        }
        return;
    }
    DismissFolderPopup(true);
}

void DockWindow::ShowTrayDrawer(int index, float anchor) {
    if (!Tray() || index < 0 || index >= static_cast<int>(items_.size())) return;
    DismissWidgetPopup(false, false);
    DismissPreview(false, false);
    DismissWeatherPopup(false, false);
    DismissCalendarPopup(false, false);
    auto entries = DrawerEntries(TrayDrawerIcons(index), [this](const TrayIcon& t) { return TrayBitmap(t); }, drawerIds_,
                                 drawerSignature_);
    auto at = [this](size_t member) -> uint64_t { return member < drawerIds_.size() ? drawerIds_[member] : 0; };
    auto cursor = [] {
        POINT pt{};
        GetCursorPos(&pt);
        return pt;
    };
    FolderPopup::GroupCallbacks cb;
    // A click goes to the app as on the grid: it usually opens a window or a menu that takes the
    // foreground, and the drawer then closes as any click popup does.
    cb.open = [this, at, cursor](size_t member) {
        if (TrayHost* t = Tray()) t->Forward(at(member), TrayHost::Click::Left, cursor());
    };
    cb.menu = [this, at](size_t member, POINT screen) {
        if (TrayHost* t = Tray()) t->Forward(at(member), TrayHost::Click::Right, screen);
    };
    cb.middle = [this, at, cursor](size_t member) {
        if (TrayHost* t = Tray()) t->Forward(at(member), TrayHost::Click::Middle, cursor());
    };
    if (!folder_.ShowGroup(kDrawerTitle, std::move(entries), ItemView::Grid, PopupStyle(), PopupAnchorAt(anchor), std::move(cb))) {
        DismissFolderPopup(false);
        MessageBeep(MB_ICONWARNING);
        return;
    }
    folderKey_ = items_[static_cast<size_t>(index)].key;
    TakePopupHold(kHoldFolder);
    HideLabel();
    RenderWithRecovery([] { return S_OK; });  // commit the label hide
}

bool DockWindow::TrayIconRect(uint64_t id, RECT& screen) const {
    // The drawer's cell while it is open and shows the icon.
    if (folder_.Kind() == 3) {
        for (size_t m = 0; m < drawerIds_.size(); ++m) {
            if (drawerIds_[m] == id && folder_.MemberScreenRect(m, screen)) return true;
        }
    }
    for (size_t i = 0; i < items_.size(); ++i) {
        if (!elements_[items_[i].element]) continue;
        if (ElementOf(i).TrayIconRect(static_cast<int>(i), id, screen)) return true;
    }
    return false;
}

bool DockWindow::RightClickAt(POINT clientPt, bool ctrl) {
    const int i = HitTest(ToStage(clientPt));
    if (i < 0 || !elements_[items_[static_cast<size_t>(i)].element]) return false;
    if (!ElementOf(static_cast<size_t>(i)).OnRightClickAt(i, ctrl)) return false;
    DismissClickPopups(true, false);
    HideLabel();
    RenderWithRecovery([] { return S_OK; });
    return true;
}

bool DockWindow::ClickTrayIcon(int slot, size_t icon, bool right) {
    TrayHost* tray = Tray();
    const auto icons = TrayDrawerIcons(slot);
    if (!tray || icon >= icons.size()) return false;
    const uint64_t id = icons[icon]->id;
    // The point the app gets: the icon's place on this dock (its cell or the overflow button).
    RECT r{};
    POINT pt{};
    if (TrayIconRect(id, r)) {
        pt = POINT{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
    } else {
        GetCursorPos(&pt);
    }
    return tray->Forward(id, right ? TrayHost::Click::Right : TrayHost::Click::Left, pt);
}
