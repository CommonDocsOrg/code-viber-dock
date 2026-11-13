// DockWindow and the tray (round 4 phase 8): the tray elements draw TrayHost's icons and forward
// input to them (TrayElements.cpp); the dock adds the hover label per cell, the pictures on its
// device, the drawer (the group popup in tray mode listing every icon, opened from the grid's
// overflow button) and the icons' screen rects for Shell_NotifyIconGetRect.

#include <ole2.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "DockWindow.h"

namespace {
constexpr UINT kCellLabelDelayMs = 250;  // = DockWindow.cpp's kLabelDelayMs
constexpr wchar_t kDrawerTitle[] = L"Notification area";
// Round 24 phase 24B: a hover-opened drawer closes this long after the pointer left both the arrow
// and the drawer (a little longer than the preview's 300: the way from the arrow to the drawer
// crosses the gap between them).
constexpr UINT kTrayLeaveMs = 400;

// A tray icon dragged (DockWindow::DragTrayIcon): a private clipboard format, so only the docks of
// this process take it.
struct TrayDragData {
    DWORD pid = 0;
    UINT64 dock = 0;  // the dock it was dragged from
    wchar_t key[128]{};
    UINT32 fromDrawer = 0;  // round 19 phase 19A: dragged out of a drawer (a drop on it reorders)
};

CLIPFORMAT TrayDragFormat() {
    static const CLIPFORMAT format = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"WinDock.TrayIcon"));
    return format;
}

// The left button up drops, Escape cancels.
class TrayDropSource final : public IDropSource {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropSource) {
            *ppv = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG n = InterlockedDecrement(&refs_);
        if (n == 0) delete this;
        return static_cast<ULONG>(n);
    }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override {
        if (escape) return DRAGDROP_S_CANCEL;
        return (keys & MK_LBUTTON) ? S_OK : DRAGDROP_S_DROP;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

private:
    LONG refs_ = 1;
};
}  // namespace

std::wstring DockWindow::ReadTrayDrag(IDataObject* data, bool* fromDrawer) {
    if (fromDrawer) *fromDrawer = false;
    if (!data || !TrayDragFormat()) return {};
    FORMATETC fmt{TrayDragFormat(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    if (FAILED(data->GetData(&fmt, &medium))) return {};
    std::wstring key;
    if (medium.hGlobal && GlobalSize(medium.hGlobal) >= sizeof(TrayDragData)) {
        if (const auto* p = static_cast<const TrayDragData*>(GlobalLock(medium.hGlobal))) {
            if (p->pid == GetCurrentProcessId()) {
                key.assign(p->key, wcsnlen(p->key, std::size(p->key)));
                if (fromDrawer) *fromDrawer = p->fromDrawer != 0;
            }
            GlobalUnlock(medium.hGlobal);
        }
    }
    ReleaseStgMedium(&medium);
    return key;
}

void DockWindow::DragTrayIcon(const std::wstring& key, bool fromDrawer) {
    if (key.empty() || !TrayDragFormat()) return;
    winrt::com_ptr<IDataObject> data;
    if (FAILED(SHCreateDataObject(nullptr, 0, nullptr, nullptr, IID_PPV_ARGS(data.put())))) return;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(TrayDragData));
    if (!h) return;
    if (auto* p = static_cast<TrayDragData*>(GlobalLock(h))) {
        p->pid = GetCurrentProcessId();
        p->dock = reinterpret_cast<UINT64>(hwnd_);
        wcsncpy_s(p->key, key.c_str(), _TRUNCATE);
        p->fromDrawer = fromDrawer ? 1 : 0;
        GlobalUnlock(h);
    }
    FORMATETC fmt{TrayDragFormat(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    medium.tymed = TYMED_HGLOBAL;
    medium.hGlobal = h;
    if (FAILED(data->SetData(&fmt, &medium, TRUE))) {
        GlobalFree(h);
        return;
    }
    if (GetCapture() == hwnd_) ReleaseCapture();
    auto* source = new TrayDropSource();
    DWORD effect = DROPEFFECT_NONE;
    // Round 19 phase 19A: the open drawer is a target too: it stays open while the drag crosses
    // the dock (SetHold would dismiss it) and after the drop, wherever it landed.
    trayDragKeepsDrawer_ = folder_.Kind() == 3;
    DoDragDrop(data.get(), source, DROPEFFECT_MOVE, &effect);  // modal; the docks' and the drawer's drop targets take it
    trayDragKeepsDrawer_ = false;
    source->Release();
    if (folder_.Kind() == 3) RefillTrayDrawer();  // a drop on the grid / the row changed the drawer's order too
    // Round 24 phase 24B: a hover-opened drawer's leave grace never closes it mid-drag (the timer
    // gives way, see OnTrayLeaveTimer); the drag over, the pointer elsewhere starts it again.
    if (TrayDrawerByHover() && !folder_.ContainsCursor() && !PointerOnDrawerArrow()) StartTrayLeave();
}

void DockWindow::ShowTrayMenu(int slot, const std::wstring& key, bool pinnedSlot) {
    if (slot < 0 || slot >= static_cast<int>(items_.size()) || key.empty()) return;
    // The tray item the menu is about: the slot's own (a grid), else this dock's first tray.
    size_t trayItem = SIZE_MAX;
    if (!pinnedSlot) {
        trayItem = items_[static_cast<size_t>(slot)].element;
    } else {
        for (const DockItem& it : items_) {
            if (it.kind == DockItem::Kind::Tray) {
                trayItem = it.element;
                break;
            }
        }
    }
    const std::vector<DockItemEntry>& entries = DockCfg().items;
    const bool hidden = trayItem < entries.size() &&
                        std::find(entries[trayItem].hidden.begin(), entries[trayItem].hidden.end(), key) != entries[trayItem].hidden.end();
    const bool pinned = std::any_of(entries.begin(), entries.end(),
                                    [&](const DockItemEntry& e) { return e.Kind() == ItemKind::TrayApp && e.id == key; });
    constexpr UINT kHide = 1, kPin = 2, kUnpin = 3, kSettings = 4;
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    const UINT locked = DockCfg().locked ? MF_GRAYED : 0;
    if (trayItem < entries.size() && !pinnedSlot) {
        AppendMenuW(menu, MF_STRING | locked, kHide, hidden ? L"Show in the tray grid" : L"Hide from the tray grid");
    }
    if (pinned) {
        AppendMenuW(menu, MF_STRING | locked, kUnpin, L"Unpin from the dock");
    } else {
        AppendMenuW(menu, MF_STRING | locked, kPin, L"Pin to the dock");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kSettings, L"Tray settings…");
    POINT pt{};
    GetCursorPos(&pt);
    SetHold(true);
    SetForegroundWindow(hwnd_);  // the menu closes when it loses the foreground
    // Round 13 phase 13A: TPM_NONOTIFY suppresses WM_ENTERMENULOOP / WM_EXITMENULOOP, so the flag
    // is set here: the dock must not raise itself above its open menu (see menuOpen_).
    menuOpen_ = true;
    const UINT cmd = static_cast<UINT>(TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, hwnd_, nullptr));
    menuOpen_ = false;
    DestroyMenu(menu);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    SetHold(false);
    switch (cmd) {
        case kHide:
            if (pinRequests_.trayHide) pinRequests_.trayHide(trayItem, key, !hidden);
            break;
        case kPin:
            if (pinRequests_.pinTray) pinRequests_.pinTray(key, SIZE_MAX);
            break;
        case kUnpin:
            if (pinRequests_.unpinTray) pinRequests_.unpinTray(key);
            break;
        case kSettings:
            if (pinRequests_.traySettings) pinRequests_.traySettings(trayItem);
            break;
        default:
            break;
    }
}

ID2D1Bitmap* DockWindow::TrayAppBitmap(const DockItem& item) {
    TrayHost* tray = Tray();
    if (!tray || item.element >= elements_.size() || !elements_[item.element]) return nullptr;
    const TrayIcon* icon = tray->ByKey(elements_[item.element]->Entry().id);
    if (!icon || !icon->icon || !icon->Shown()) return nullptr;
    auto it = trayBigBitmaps_.find(icon->imageSerial);
    if (it != trayBigBitmaps_.end()) return it->second.get();
    ID2D1DeviceContext* dc = renderer_.ResourceContext();
    if (!dc) return nullptr;
    if (trayBigBitmaps_.size() > 64) trayBigBitmaps_.clear();  // pictures of icons that changed
    // The best size there is: the icon's resource at the bitmap size, when the handle still knows
    // where it came from (an icon loaded from a module); else the copy as the app sent it.
    winrt::com_ptr<ID2D1Bitmap> bitmap;
    const int px = static_cast<int>(std::max<UINT>(layout_.bitmapPx, 16));
    if (HICON big = static_cast<HICON>(CopyImage(icon->icon, IMAGE_ICON, px, px, LR_COPYFROMRESOURCE))) {
        bitmap = icons_.FromHicon(big, dc);
        DestroyIcon(big);
    }
    if (!bitmap) bitmap = icons_.FromHicon(icon->icon, dc);
    ID2D1Bitmap* raw = bitmap.get();
    trayBigBitmaps_.emplace(icon->imageSerial, std::move(bitmap));
    return raw;
}

TrayHost* DockWindow::Tray() {
    return shared_ && shared_->TrayService().Running() ? &shared_->TrayService() : nullptr;
}

ID2D1Bitmap* DockWindow::TrayBitmap(const TrayIcon& icon, int px) {
    if (!icon.icon) return nullptr;
    // Round 24 phase 24A: keyed by the picture and the size (the grid at a large icon size and the
    // drawer at the handle's own size hold one each).
    constexpr int kSizeBits = 12;
    px = std::clamp(px, 0, (1 << kSizeBits) - 1);
    const uint64_t key = (icon.imageSerial << kSizeBits) | static_cast<uint64_t>(px);
    auto it = trayBitmaps_.find(key);
    if (it != trayBitmaps_.end()) return it->second.get();
    ID2D1DeviceContext* dc = renderer_.ResourceContext();
    if (!dc) return nullptr;
    // Pictures of icons that changed or went: dropped once the cache holds three times the live
    // ones (two sizes per icon are normal since 24A).
    if (TrayHost* tray = Tray(); tray && trayBitmaps_.size() >= 3 * tray->Icons().size() + 8) {
        std::erase_if(trayBitmaps_, [&](const auto& kv) {
            return std::none_of(tray->Icons().begin(), tray->Icons().end(),
                                [&](const TrayIcon& t) { return t.imageSerial == kv.first >> kSizeBits; });
        });
    }
    winrt::com_ptr<ID2D1Bitmap> bitmap;
    if (px > 0) {
        // As TrayAppBitmap: the icon's resource at that size when the handle still knows where it
        // came from (an icon loaded from a module), so 28 - 48 DIP icons are not a 16 px handle
        // scaled up; else the copy as the app sent it.
        if (HICON sized = static_cast<HICON>(CopyImage(icon.icon, IMAGE_ICON, px, px, LR_COPYFROMRESOURCE))) {
            bitmap = icons_.FromHicon(sized, dc);
            DestroyIcon(sized);
        }
    }
    if (!bitmap) bitmap = icons_.FromHicon(icon.icon, dc);
    ID2D1Bitmap* raw = bitmap.get();
    trayBitmaps_.emplace(key, std::move(bitmap));
    return raw;
}

bool DockWindow::TrayDrawerShown(const AppKey& key) const {
    return !key.id.empty() && folder_.Kind() == 3 && folderKey_ == key;
}

void DockWindow::RedrawTraySlot(const AppKey& key) {
    if (!hwnd_ || key.id.empty()) return;
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].key == key && items_[i].kind == DockItem::Kind::Tray) RedrawTraySlots(static_cast<int>(i), -1);
    }
}

void DockWindow::RedrawTraySlots(int a, int b) {
    // Only the slot's surface, at the size its element last measured: no re-measure, so no relayout
    // can start from here (this runs inside a rebuild's SetHover(-1) and popup dismissals too).
    // A slot that cannot be drawn now (a layout or a device recovery runs, its element is new and
    // not measured yet) is marked dirty: the next layout or RefreshDirtyWidgets draws it.
    bool draw = false;
    for (int s : {a, b}) {
        if (s < 0 || s >= static_cast<int>(items_.size())) continue;
        DockItem& item = items_[static_cast<size_t>(s)];
        if (item.kind != DockItem::Kind::Tray || item.element >= elements_.size() || !elements_[item.element]) continue;
        item.dirty = true;
        DockElement::TrayLookState look;
        if (hwnd_ && !recovering_ && !inLayout_ && item.layer.visual && ElementOf(static_cast<size_t>(s)).TrayLook(look) &&
            look.cellAlong > 0) {
            draw = true;
        }
    }
    if (!draw) return;
    RenderWithRecovery([this, a, b] {
        HRESULT hr = S_OK;
        for (int s : {a, b}) {
            if (s < 0 || s >= static_cast<int>(items_.size())) continue;
            DockItem& item = items_[static_cast<size_t>(s)];
            if (SUCCEEDED(hr) && item.dirty && item.kind == DockItem::Kind::Tray && item.layer.visual) hr = RenderWidget(item);
        }
        return hr;
    });
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
    // Round 24 phase 24A: the tray grid draws a plate under its hot cell: its slot is redrawn
    // when the hot cell changes (never on a move within the cell).
    if (oldSlot >= 0 && oldSlot < static_cast<int>(items_.size()) && elements_[items_[static_cast<size_t>(oldSlot)].element]) {
        ElementOf(static_cast<size_t>(oldSlot)).OnCellHover(oldSlot, oldCell, CellEvent::Leave);
    }
    if (slot < 0) {
        RedrawTraySlots(oldSlot, -1);
        UpdateTrayHover();  // round 24 phase 24B
        return;  // the caller sets the hover (SetHover) itself
    }
    ElementOf(static_cast<size_t>(slot)).OnCellHover(slot, cell, CellEvent::Enter);
    RedrawTraySlots(oldSlot, slot == oldSlot ? -1 : slot);
    UpdateTrayHover();  // round 24 phase 24B: the arrow of a `drawerOpen: hover` tray
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
    const DockElement& element = ElementOf(static_cast<size_t>(slot));
    std::vector<const TrayIcon*> icons = TrayIconsFor(shared_->TrayService(), DockCfg(), element.Entry(), false);
    // Round 24 phase 24B: `drawerShows: overflow` leaves out the icons the grid shows (as it was
    // last measured). What stays: hidden icons, those that did not fit, and icons pinned as trayApp
    // items (the grid never shows those; the drawer listed them before 24B too, and it is where a
    // pinned icon is dragged back from). Drops still map through drawerIds_ (this list), so a drop
    // into / within the drawer lands between the members the user sees.
    if (element.Entry().trayDrawerShows == TrayDrawerShows::Overflow) {
        std::erase_if(icons, [&](const TrayIcon* t) { return element.TrayGridShows(t->id); });
    }
    return icons;
}

FolderPopup::GroupLayout DockWindow::TrayDrawerLayout(int slot) const {
    FolderPopup::GroupLayout l;
    if (slot < 0 || slot >= static_cast<int>(items_.size()) || !elements_[items_[static_cast<size_t>(slot)].element]) return l;
    const DockItemEntry& e = ElementOf(static_cast<size_t>(slot)).Entry();
    l.iconDip = static_cast<float>(std::clamp(e.trayDrawerIconSize, 16, 48));
    l.fontDip = static_cast<float>(std::clamp(e.trayDrawerLabelSize, 10, 16));
    l.labels = e.trayDrawerLabels == TrayDrawerLabels::Beside ? FolderPopup::GroupLayout::Labels::Beside
               : e.trayDrawerLabels == TrayDrawerLabels::None ? FolderPopup::GroupLayout::Labels::None
                                                              : FolderPopup::GroupLayout::Labels::Below;
    l.columns = static_cast<size_t>(std::clamp(e.trayDrawerColumns, 0, 8));
    l.title = e.trayDrawerTitle;
    return l;
}

int DockWindow::TrayDrawerBitmapPx(int slot) const {
    // Round 24 phase 24B: at the default 40 DIPs the drawer keeps the icon handle as the app sent
    // it, scaled by D2D, exactly as before (the default must look the same; 24A's grid does the
    // same up to 24 DIPs). Any other size asks for the picture at the drawn size; the drawer then
    // takes whichever of the two is larger (DrawerBitmap), so a handle that is already big enough
    // is not swapped for a resampled copy.
    const FolderPopup::GroupLayout l = TrayDrawerLayout(slot);
    if (l.iconDip == 40.0f) return 0;
    return static_cast<int>(std::lround(l.iconDip * layout_.scale));
}

ID2D1Bitmap* DockWindow::DrawerBitmap(const TrayIcon& icon, int px) {
    ID2D1Bitmap* native = TrayBitmap(icon, 0);
    if (px <= 0 || (native && static_cast<int>(native->GetPixelSize().width) >= px)) return native;
    ID2D1Bitmap* sized = TrayBitmap(icon, px);
    return sized ? sized : native;
}

std::wstring DockWindow::DrawerLayoutSignature(const FolderPopup::GroupLayout& l, int px) {
    return L"#" + std::to_wstring(static_cast<int>(l.iconDip)) + L"," + std::to_wstring(static_cast<int>(l.labels)) + L"," +
           std::to_wstring(static_cast<int>(l.fontDip)) + L"," + std::to_wstring(l.columns) + L"," + (l.title ? L"1" : L"0") +
           L"," + std::to_wstring(px);
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
    // Pinned tray icons: their picture, label and placeholder follow the icon.
    {
        TrayHost* tray = Tray();
        bool redraw = false;
        for (DockItem& item : items_) {
            if (item.kind != DockItem::Kind::TrayApp || !elements_[item.element]) continue;
            const TrayIcon* icon = tray ? tray->ByKey(elements_[item.element]->Entry().id) : nullptr;
            if (icon && !icon->Shown()) icon = nullptr;
            const bool missing = icon == nullptr;
            std::wstring label = icon ? icon->Title() : elements_[item.element]->Entry().id + L" (not running)";
            const uint64_t serial = icon ? icon->imageSerial : 0;
            uint64_t& drawn = trayAppSerials_[item.key.id];
            if (missing != item.missing || serial != drawn || label != item.displayName) {
                item.missing = missing;
                item.displayName = std::move(label);
                drawn = serial;
                item.dirty = true;
                redraw = true;
                if (&item - items_.data() == hovered_) labelDirty_ = true;
            }
        }
        if (redraw) RenderWithRecovery([this] { return DrawFrame(false); });
    }
    // A hovered cell may show another icon now: its label is re-read on the next mouse move.
    // An open drawer follows the icons (new ones, gone ones, new pictures / tips).
    RefillTrayDrawer();
}

void DockWindow::RefillTrayDrawer() {
    if (folder_.Kind() != 3) return;
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].key != folderKey_ || items_[i].kind != DockItem::Kind::Tray) continue;
        if (!Tray()) break;
        std::vector<uint64_t> ids;
        std::wstring signature;
        // Round 24 phase 24B: the tray item's drawer* keys (changed while open: laid out again).
        const FolderPopup::GroupLayout layout = TrayDrawerLayout(static_cast<int>(i));
        const int px = TrayDrawerBitmapPx(static_cast<int>(i));
        auto entries = DrawerEntries(TrayDrawerIcons(static_cast<int>(i)),
                                     [this, px](const TrayIcon& t) { return DrawerBitmap(t, px); }, ids, signature);
        signature += DrawerLayoutSignature(layout, px);
        if (signature != drawerSignature_) {
            drawerSignature_ = std::move(signature);
            drawerIds_ = std::move(ids);
            folder_.UpdateGroup(kDrawerTitle, std::move(entries), &layout);
            RenderWithRecovery([] { return S_OK; });
        }
        return;
    }
    DismissFolderPopup(true);
}

void DockWindow::ShowTrayDrawer(int index, float anchor) {
    if (!Tray() || index < 0 || index >= static_cast<int>(items_.size())) return;
    DismissWidgetPopup(false, false);
    DismissWidgetPassive(false);
    DismissPreview(false, false);
    DismissWeatherPopup(false, false);
    DismissCalendarPopup(false, false);
    DismissCentre(false, false);
    // Round 24 phase 24B: a click-opened drawer (the hover path sets drawerByHover_ after this);
    // no hover timer may outlive the opening.
    drawerByHover_ = false;
    CancelTrayLeave();
    if (trayHoverPending_) {
        KillTimer(hwnd_, kTrayHoverTimerId);
        trayHoverPending_ = false;
    }
    const FolderPopup::GroupLayout layout = TrayDrawerLayout(index);
    const int px = TrayDrawerBitmapPx(index);
    auto entries = DrawerEntries(TrayDrawerIcons(index), [this, px](const TrayIcon& t) { return DrawerBitmap(t, px); },
                                 drawerIds_, drawerSignature_);
    drawerSignature_ += DrawerLayoutSignature(layout, px);
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
    // Round 19 phase 19A: a right click shows the app's menu with the drawer still open behind it
    // (HoldForMenu, held before the click goes out: the app takes the foreground at once); the
    // menu dismissed (Escape, a click back on the drawer), another icon can be right-clicked.
    cb.menu = [this, at](size_t member, POINT screen) {
        TrayHost* t = Tray();
        if (!t) return;
        // Round 24 phase 24B: a right click makes a hover-opened drawer an ordinary (click-opened)
        // one: no leave grace closes it behind the app's menu; HoldForMenu and the foreground
        // decide, as for a click-opened drawer.
        drawerByHover_ = false;
        CancelTrayLeave();
        if (const TrayIcon* icon = t->ById(at(member))) folder_.HoldForMenu(icon->pid);
        t->Forward(at(member), TrayHost::Click::Right, screen);
    };
    cb.middle = [this, at, cursor](size_t member) {
        if (TrayHost* t = Tray()) t->Forward(at(member), TrayHost::Click::Middle, cursor());
    };
    if (!DockCfg().locked) {
        // Dragged out of the drawer: onto the icon row a trayApp pin, onto the grid a new place.
        cb.drag = [this, at](size_t member, POINT) {
            TrayHost* t = Tray();
            const TrayIcon* icon = t ? t->ById(at(member)) : nullptr;
            if (!icon) return;
            const std::wstring key = icon->key;
            DragTrayIcon(key, true);  // the drawer stays open (it is inside its own input handling here)
        };
        // Round 19 phase 19A: dropped on the drawer (from itself, the grid or a trayApp slot): a
        // new place in the order, out of the grid / off the row.
        cb.trayAccepts = [this](IDataObject* data) { return !DockCfg().locked && !ReadTrayDrag(data).empty(); };
        cb.trayDrop = [this](IDataObject* data, size_t before) {
            bool fromDrawer = false;
            const std::wstring key = ReadTrayDrag(data, &fromDrawer);
            if (!key.empty()) DropOnTrayDrawer(key, fromDrawer, before);
        };
    }
    // Round 24 phase 24B: the gear at the right of the title row opens the tray item's settings
    // window (as the tray menu's "Tray settings…", DockCommand 142 / ItemCommand 854 do). Looked up
    // when pressed: the elements may have been rebuilt since the drawer opened.
    cb.settings = [this] {
        size_t trayItem = SIZE_MAX;
        for (const DockItem& it : items_) {
            if (it.key == folderKey_ && it.kind == DockItem::Kind::Tray) {
                trayItem = it.element;
                break;
            }
        }
        DismissFolderPopup(true);
        if (trayItem != SIZE_MAX && pinRequests_.traySettings) pinRequests_.traySettings(trayItem);
    };
    if (!folder_.ShowGroup(kDrawerTitle, std::move(entries), ItemView::Grid, PopupStyle(), PopupAnchorAt(anchor), std::move(cb),
                           layout)) {
        DismissFolderPopup(false);
        MessageBeep(MB_ICONWARNING);
        return;
    }
    folderKey_ = items_[static_cast<size_t>(index)].key;
    TakePopupHold(kHoldFolder);
    RedrawTraySlot(folderKey_);  // round 24 phase 24A: the chevron points back while it is open
    HideLabel();
    RenderWithRecovery([] { return S_OK; });  // commit the label hide
}

void DockWindow::DropOnTrayDrawer(const std::wstring& key, bool fromDrawer, size_t before) {
    TrayHost* tray = Tray();
    if (!tray || key.empty() || folder_.Kind() != 3 || DockCfg().locked || !pinRequests_.trayDrawerOrder) return;
    size_t trayItem = SIZE_MAX;
    for (const DockItem& it : items_) {
        if (it.key == folderKey_ && it.kind == DockItem::Kind::Tray) {
            trayItem = it.element;
            break;
        }
    }
    const std::vector<DockItemEntry>& entries = DockCfg().items;
    if (trayItem >= entries.size()) return;
    // The icon it goes before: the first member at or after the drop that is not the icon itself
    // (dropped right before or right after itself: the same place).
    std::wstring beforeKey;
    for (size_t m = before; m < drawerIds_.size(); ++m) {
        const TrayIcon* icon = tray->ById(drawerIds_[m]);
        if (!icon || icon->key == key) continue;
        beforeKey = icon->key;
        break;
    }
    const bool pinned = std::any_of(entries.begin(), entries.end(),
                                    [&](const DockItemEntry& e) { return e.Kind() == ItemKind::TrayApp && e.id == key; });
    const std::vector<std::wstring>& hidden = entries[trayItem].hidden;
    const bool isHidden = std::find(hidden.begin(), hidden.end(), key) != hidden.end();
    // From the drawer: only its place (a pinned member stays pinned, a grid one in the grid). From
    // a trayApp slot (a pinned icon dragged from elsewhere): off the row, into its place. From the
    // grid: out of the grid (hidden), so it sits in the overflow where it was dropped.
    const bool unpin = !fromDrawer && pinned;
    const bool hide = !fromDrawer && !pinned && !isHidden;
    pinRequests_.trayDrawerOrder(trayItem, key, beforeKey, hide, unpin);  // rebuilds (RefreshFolderPopup refills)
    RefillTrayDrawer();
}

bool DockWindow::TrayIconRect(uint64_t id, RECT& screen) const {
    // The drawer's cell while it is open and shows the icon.
    if (folder_.Kind() == 3) {
        for (size_t m = 0; m < drawerIds_.size(); ++m) {
            if (drawerIds_[m] == id && folder_.MemberScreenRect(m, screen)) return true;
        }
    }
    // A pinned icon is where its slot is (the grid only shows the chevron for it).
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < items_.size(); ++i) {
            if (!elements_[items_[i].element] || (items_[i].kind == DockItem::Kind::TrayApp) != (pass == 0)) continue;
            if (ElementOf(i).TrayIconRect(static_cast<int>(i), id, screen)) return true;
        }
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

// --- Round 24 phase 24B: the drawer opened by hovering the arrow (`drawerOpen: hover`) ---------
// Resting the pointer on the arrow for previewDelayMs (the dock's popup hover delay) opens the
// drawer as a click would (ShowClickPopup), marked drawerByHover_. Such a drawer closes
// kTrayLeaveMs after the pointer is over neither the arrow nor the drawer (the dock's hover cells
// and the drawer's own hover callback start / cancel that grace). A press on the arrow keeps it
// open and makes it an ordinary click-opened drawer (the click that would have opened it: closing
// it there would surprise), as does a right click on a member (the app's menu). Both timers are
// one-shot and only armed while the pointer is on such an arrow / such a drawer is on screen.

bool DockWindow::TrayHoverEligible() const {
    return (hideState_ == HideState::Shown || hideState_ == HideState::Showing) && !hold_ && !menuOpen_ && !editing_ &&
           dragPhase_ == DragPhase::None && !dropActive_ && !trayDragKeepsDrawer_ && !ClickPopupOnScreen();
}

void DockWindow::UpdateTrayHover() {
    if (!hwnd_) return;
    const int slot = hoverCellSlot_;
    bool onArrow = false, opens = false;
    if (slot >= 0 && slot < static_cast<int>(items_.size()) && items_[static_cast<size_t>(slot)].kind == DockItem::Kind::Tray &&
        elements_[items_[static_cast<size_t>(slot)].element]) {
        const DockElement& e = ElementOf(static_cast<size_t>(slot));
        onArrow = e.TrayOverflow() && hoverCell_ == e.TrayCells();  // the arrow's cell index is the icon count
        opens = onArrow && e.Entry().trayDrawerOpen == TrayDrawerOpen::Hover;
    }
    if (TrayDrawerByHover()) {
        if (onArrow && items_[static_cast<size_t>(slot)].key == folderKey_) {
            CancelTrayLeave();
        } else if (!folder_.ContainsCursor()) {
            StartTrayLeave();
        }
    }
    if (opens && TrayHoverEligible()) {
        if (!trayHoverPending_ || trayHoverSlot_ != slot) {
            SetTimer(hwnd_, kTrayHoverTimerId, std::max<UINT>(USER_TIMER_MINIMUM, static_cast<UINT>(DockCfg().previewDelayMs)),
                     nullptr);
            trayHoverPending_ = true;
            trayHoverSlot_ = slot;
        }
    } else if (trayHoverPending_) {
        KillTimer(hwnd_, kTrayHoverTimerId);
        trayHoverPending_ = false;
        trayHoverSlot_ = -1;
    }
}

void DockWindow::OnTrayHoverTimer() {
    KillTimer(hwnd_, kTrayHoverTimerId);
    trayHoverPending_ = false;
    const int slot = std::exchange(trayHoverSlot_, -1);
    if (slot < 0 || slot != hoverCellSlot_ || slot >= static_cast<int>(items_.size()) ||
        items_[static_cast<size_t>(slot)].kind != DockItem::Kind::Tray || !elements_[items_[static_cast<size_t>(slot)].element] ||
        !TrayHoverEligible() || !CursorInZone()) {
        return;
    }
    const DockElement& e = ElementOf(static_cast<size_t>(slot));
    if (!e.TrayOverflow() || hoverCell_ != e.TrayCells() || e.Entry().trayDrawerOpen != TrayDrawerOpen::Hover) return;
    if (hideState_ == HideState::Showing) {  // a very short delay: wait for the slide to end
        SetTimer(hwnd_, kTrayHoverTimerId, static_cast<UINT>(std::ceil(motion_.slide * 1000.0)) + 20, nullptr);
        trayHoverPending_ = true;
        trayHoverSlot_ = slot;
        return;
    }
    ShowClickPopup(slot);
    if (folder_.Kind() == 3 && items_[static_cast<size_t>(slot)].key == folderKey_) drawerByHover_ = true;
}

void DockWindow::StartTrayLeave() {
    if (trayLeavePending_ || !hwnd_ || !TrayDrawerByHover()) return;
    trayLeavePending_ = SetTimer(hwnd_, kTrayLeaveTimerId, kTrayLeaveMs, nullptr) != 0;
}

void DockWindow::CancelTrayLeave() {
    if (!trayLeavePending_) return;
    if (hwnd_) KillTimer(hwnd_, kTrayLeaveTimerId);
    trayLeavePending_ = false;
}

void DockWindow::OnTrayLeaveTimer() {
    KillTimer(hwnd_, kTrayLeaveTimerId);
    trayLeavePending_ = false;
    if (!TrayDrawerByHover()) return;
    if (folder_.ContainsCursor() || PointerOnDrawerArrow()) return;  // came back
    // A drag (into, out of or across the drawer) or a press that may become one, or an app's menu
    // over it: it stays. Not re-armed: DragTrayIcon starts the grace again once the drag is over
    // (a right click made it a click-opened drawer already).
    if (trayDragKeepsDrawer_ || dragPhase_ != DragPhase::None || pressKeptDrawer_ || folder_.MenuHeld()) return;
    DismissFolderPopup(true);
}

bool DockWindow::PointerOnDrawerArrow() const {
    if (folder_.Kind() != 3 || folderKey_.id.empty()) return false;
    POINT pt{};
    if (!GetCursorPos(&pt)) return false;
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].key != folderKey_ || items_[i].kind != DockItem::Kind::Tray || !elements_[items_[i].element]) continue;
        const DockElement& e = ElementOf(i);
        return e.TrayOverflow() && e.CellAt(static_cast<int>(i), pt, nullptr, nullptr) == e.TrayCells();
    }
    return false;
}

bool DockWindow::OpenTrayDrawerByHover(int slot) {
    if (slot < 0 || slot >= static_cast<int>(items_.size()) || items_[static_cast<size_t>(slot)].kind != DockItem::Kind::Tray ||
        !elements_[items_[static_cast<size_t>(slot)].element] || !Tray()) {
        return false;
    }
    if (ClickPopupOnScreen()) DismissClickPopups(false, false);
    ShowClickPopup(slot);
    if (folder_.Kind() != 3 || items_[static_cast<size_t>(slot)].key != folderKey_) return false;
    drawerByHover_ = true;
    if (!folder_.ContainsCursor() && !PointerOnDrawerArrow()) StartTrayLeave();
    return true;
}

bool DockWindow::PressTrayDrawerGear() { return folder_.Kind() == 3 && folder_.PressGear(); }
