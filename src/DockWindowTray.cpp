// DockWindow and the tray (round 4 phase 8): the tray elements draw TrayHost's icons and forward
// input to them (TrayElements.cpp); the dock adds the hover label per cell, the pictures on its
// device, the drawer (the group popup in tray mode listing every icon, opened from the grid's
// overflow button) and the icons' screen rects for Shell_NotifyIconGetRect.

#include <ole2.h>
#include <shlobj.h>

#include <algorithm>
#include <iterator>
#include <string>
#include <vector>

#include "DockWindow.h"

namespace {
constexpr UINT kCellLabelDelayMs = 250;  // = DockWindow.cpp's kLabelDelayMs
constexpr wchar_t kDrawerTitle[] = L"Notification area";

// A tray icon dragged (DockWindow::DragTrayIcon): a private clipboard format, so only the docks of
// this process take it.
struct TrayDragData {
    DWORD pid = 0;
    UINT64 dock = 0;  // the dock it was dragged from
    wchar_t key[128]{};
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

std::wstring DockWindow::ReadTrayDrag(IDataObject* data) {
    if (!data || !TrayDragFormat()) return {};
    FORMATETC fmt{TrayDragFormat(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    if (FAILED(data->GetData(&fmt, &medium))) return {};
    std::wstring key;
    if (medium.hGlobal && GlobalSize(medium.hGlobal) >= sizeof(TrayDragData)) {
        if (const auto* p = static_cast<const TrayDragData*>(GlobalLock(medium.hGlobal))) {
            if (p->pid == GetCurrentProcessId()) key.assign(p->key, wcsnlen(p->key, std::size(p->key)));
            GlobalUnlock(medium.hGlobal);
        }
    }
    ReleaseStgMedium(&medium);
    return key;
}

void DockWindow::DragTrayIcon(const std::wstring& key) {
    if (key.empty() || !TrayDragFormat()) return;
    winrt::com_ptr<IDataObject> data;
    if (FAILED(SHCreateDataObject(nullptr, 0, nullptr, nullptr, IID_PPV_ARGS(data.put())))) return;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(TrayDragData));
    if (!h) return;
    if (auto* p = static_cast<TrayDragData*>(GlobalLock(h))) {
        p->pid = GetCurrentProcessId();
        p->dock = reinterpret_cast<UINT64>(hwnd_);
        wcsncpy_s(p->key, key.c_str(), _TRUNCATE);
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
    DoDragDrop(data.get(), source, DROPEFFECT_MOVE, &effect);  // modal; the docks' drop targets take it
    source->Release();
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
    const UINT cmd = static_cast<UINT>(TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, hwnd_, nullptr));
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
            if (pinRequests_.traySettings) pinRequests_.traySettings();
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
    if (!DockCfg().locked) {
        // Dragged out of the drawer: onto the icon row a trayApp pin, onto the grid a new place.
        cb.drag = [this, at](size_t member, POINT) {
            TrayHost* t = Tray();
            const TrayIcon* icon = t ? t->ById(at(member)) : nullptr;
            if (!icon) return;
            const std::wstring key = icon->key;
            DragTrayIcon(key);  // the drawer stays open (it is inside its own input handling here)
        };
    }
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
