#include "DockWindow.h"

#include <dwmapi.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cwctype>
#include <vector>

#include "Activation.h"
#include "AppIdentity.h"
#include "ContextMenu.h"
#include "DesktopLayer.h"
#include "IconEffects.h"
#include "Launcher.h"
#include "Theme.h"

namespace {

constexpr DWORD kExStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP;
constexpr DWORD kStyle = WS_POPUP;

// Layout constants in DIPs (scaled by DPI).
// Round 11 phase 11A: the bar's cross padding, the gap between icons and with them the icon row's
// place are theme fields now (barPadding 8: bar height = iconSize + 16, icon top 5, the dots' centre
// 5 from the bar's edge; iconGap 8), see Layout::iconTopPx.
constexpr float kIconTopShare = 0.625f;     // with the dots: bar top -> icon top = 5/8 of barPadding (5 of 8)
constexpr float kWindowExtraHeight = 56.0f; // window height = iconSize * magnification + this (label room)
// Vertical docks: the label sits beside the magnified icon, so the stage's cross size has room for
// the widest label (kLabelMaxWidth + its padding) instead of one label height.
constexpr float kVerticalWindowExtra = 56.0f - 37.0f + 6.0f + 320.0f + 20.0f + 8.0f;
constexpr float kBottomMargin = 8.0f;       // gap between the bar and the screen edge
constexpr float kPadding = 12.0f;           // inner horizontal padding of the bar
constexpr float kDotRadius = 2.0f;          // 4 px dot at 100 %
constexpr float kSeparatorWidth = 17.0f;    // spacer between an icon slot and a widget (hairline in it)
constexpr float kWidgetGap = 10.0f;         // spacer between two widgets (the weather and the clock)
constexpr float kMinSideSlack = 24.0f;      // window space beside the bar (labels of edge icons)
constexpr float kLabelGap = 6.0f;           // magnified icon top -> label bottom
constexpr float kLabelFontSize = 13.0f;
constexpr float kLabelMaxWidth = 320.0f;
constexpr float kBounceHeight = 0.35f;      // launch bounce height, fraction of the icon size
constexpr float kPulseOpacity = 0.45f;      // launchStyle "pulse": lowest opacity of a pulse
constexpr ULONGLONG kBounceTimeoutMs = 5000; // stop bouncing if no window appeared by then

// Interaction timing (the animation durations are DockWindow::k*DurS, scaled by motion_).
constexpr double kMagnifyIntervalS = 0.016;  // at most ~60 magnification updates per second
// 9A follow-up 2: a hovered widget that grows this soon after the pointer came onto it grows on
// hover (it zooms: a "steady" bar grows); later growth is its content changing.
constexpr double kHoverGrowWindowS = 1.0;
constexpr UINT kLabelDelayMs = 250;
// Dragging icons.
constexpr float kDragScale = 1.1f;           // the dragged icon, relative to rest size
constexpr float kDragOpacity = 0.85f;
constexpr float kRemoveOpacity = 0.4f;       // dragged far above the bar: "release to unpin"
constexpr float kRemoveZoneBars = 1.5f;      // that zone starts this many bar heights above the bar
constexpr UINT kDragPollMs = 50;             // Escape polling while dragging (no keyboard focus)
constexpr float kMarkerWidth = 2.0f;         // file-drop insertion marker, DIPs
constexpr UINT kPreviewLeaveMs = 300;        // grace period after leaving the item and the popup
constexpr float kDropScale = 1.15f;          // an app / the bin under a file drag (open with / recycle)
constexpr float kGroupScale = 1.2f;          // a pin / group a dragged icon would group with
constexpr UINT kGroupHoldMs = 600;           // hold a dragged icon over a pin this long: a group
constexpr float kDropEdge = 0.15f;           // slot fraction per side that still means "between items"
constexpr UINT kRecycleBinCoalesceMs = 200;  // bursts of bin change notifications -> one redraw
constexpr float kDockSnapDip = 24.0f;        // a dragged dock snaps to the centre / an end this close
constexpr float kFloatSnapDip = 8.0f;        // a dragged floating dock snaps to the work area / other floating docks
constexpr float kEditSnapDip = 14.0f;        // round 12 phase 12E: customize mode's drag: a centred dock's offset snaps to 0 this close
// Titles under the icons (docks[].titles): the label's font at this size, this far under the icon
// (horizontal docks: in the icon's slot width; vertical docks: beside the icon, this wide).
constexpr float kCaptionFontSize = 10.0f;
constexpr float kCaptionGap = 2.0f;
constexpr float kCaptionBesideGap = 4.0f;
constexpr float kCaptionBesideWidth = 76.0f;
constexpr float kCaptionPad = 2.0f;          // text inset at the ends of a caption box
constexpr float kPi = 3.14159265358979f;
// Rings (docks[].shape, round 4 phase 11b): the smallest hole inside a ring, and the hub a hidden
// circle shrinks to (DIPs).
constexpr float kMinPolarHoleDip = 24.0f;
constexpr float kHubDip = 44.0f;
constexpr float kHubOpacity = 0.9f;

void Log(const wchar_t* what, HRESULT hr) {
    wchar_t buf[160];
    swprintf_s(buf, L"WinDock: %s failed (0x%08X)\n", what, static_cast<unsigned>(hr));
    OutputDebugStringW(buf);
}

UINT TimerMs(double seconds) {
    return std::max<UINT>(USER_TIMER_MINIMUM, static_cast<UINT>(std::ceil(seconds * 1000.0)));
}

// What a slot is for the spacers between slots: an icon (apps, folders, separators, the bin), a
// widget, or nothing (a widget with nothing to show takes no room).
enum class Run { None, Icon, Widget };

// The spacer (main axis, px) before a slot of `run` that follows one of `prev`: none between
// icons, `iconToWidget` between an icon and a widget, `widgetGap` between two widgets.
float SpacerBetween(Run prev, Run run, float iconToWidget, float widgetGap) {
    if (prev == Run::None || run == Run::None || (prev == Run::Icon && run == Run::Icon)) return 0.0f;
    return prev == Run::Widget && run == Run::Widget ? widgetGap : iconToWidget;
}

// Round 7 phase 7B: one slot of an anchored layout (in dock order).
struct AnchorSlot {
    Run run = Run::None;
    float w = 0.0f;       // the slot's length along the bar
    int group = 1;        // 0 start, 1 centre, 2 end
    float offset = 0.0f;  // px: shifts it and the slots after it in its group
    bool flush = false;   // round 11 phase 11C: DockItem::flush (no end padding beside it)
};

// The bar's padding beside its first / last slot: half a gap less beside an icon (its slot holds
// half a gap on each side), none beside a flush widget (round 11 phase 11C).
float EndPadding(const AnchorSlot& s, float padding, float half) {
    return s.flush ? 0.0f : s.run == Run::Icon ? padding - half : padding;
}

// The leads (the spacer before each slot, px) of anchored groups in the bar [barLeft, barRight]:
// the start group packed from the bar's start padding, the end group packed back from its end
// padding, the centre group centred on the bar as a block; within a group the usual spacers. Each
// slot's offset shifts it and the rest of its group; groups never overlap and nothing leaves the
// padding (an item pushed into a neighbour pushes it along, as far as the bar's ends). The first
// lead is measured from the start padding (the run's itemsLeft). False when the slots are not in
// group order or the groups do not fit (the caller lays out one run instead).
bool AnchoredLeads(const std::vector<AnchorSlot>& slots, float barLeft, float barRight, float padding, float half,
                   float iconToWidget, float widgetGap, std::vector<float>& leads) {
    leads.assign(slots.size(), 0.0f);
    int firstReal = -1, lastReal = -1, lastGroup = 0;
    for (size_t k = 0; k < slots.size(); ++k) {
        if (slots[k].run == Run::None) continue;
        if (slots[k].group < lastGroup) return false;  // not in group order
        lastGroup = slots[k].group;
        if (firstReal < 0) firstReal = static_cast<int>(k);
        lastReal = static_cast<int>(k);
    }
    if (firstReal < 0) return false;
    const float startInner = barLeft + EndPadding(slots[static_cast<size_t>(firstReal)], padding, half);
    const float endInner = barRight - EndPadding(slots[static_cast<size_t>(lastReal)], padding, half);
    // Each group packed (its own spacers), and the spacers the groups need between them.
    float width[3] = {0.0f, 0.0f, 0.0f};
    Run firstRun[3] = {Run::None, Run::None, Run::None}, lastRun[3] = {Run::None, Run::None, Run::None};
    std::vector<float> packed(slots.size(), 0.0f);  // start relative to the group's origin
    for (size_t k = 0; k < slots.size(); ++k) {
        const AnchorSlot& s = slots[k];
        if (s.run == Run::None) continue;
        const int g = std::clamp(s.group, 0, 2);
        width[g] += SpacerBetween(lastRun[g], s.run, iconToWidget, widgetGap);
        packed[k] = width[g];
        width[g] += s.w;
        if (firstRun[g] == Run::None) firstRun[g] = s.run;
        lastRun[g] = s.run;
    }
    float total = 0.0f;
    Run prev = Run::None;
    for (int g = 0; g < 3; ++g) {
        if (firstRun[g] == Run::None) continue;
        total += SpacerBetween(prev, firstRun[g], iconToWidget, widgetGap) + width[g];
        prev = lastRun[g];
    }
    if (total > endInner - startInner + 0.5f) return false;
    const float origin[3] = {startInner, std::round((barLeft + barRight) / 2.0f - width[1] / 2.0f), endInner - width[2]};
    // Where each slot wants to start (its group's place, the offsets of it and the slots before it
    // in its group), then pushed apart (forward) and kept inside the padding (backward).
    std::vector<float> x(slots.size(), 0.0f);
    float shift[3] = {0.0f, 0.0f, 0.0f};
    for (size_t k = 0; k < slots.size(); ++k) {
        const AnchorSlot& s = slots[k];
        if (s.run == Run::None) continue;
        const int g = std::clamp(s.group, 0, 2);
        shift[g] += s.offset;
        x[k] = origin[g] + packed[k] + shift[g];
    }
    float end = startInner;
    prev = Run::None;
    for (size_t k = 0; k < slots.size(); ++k) {
        if (slots[k].run == Run::None) continue;
        x[k] = std::max(x[k], end + SpacerBetween(prev, slots[k].run, iconToWidget, widgetGap));
        end = x[k] + slots[k].w;
        prev = slots[k].run;
    }
    float limit = endInner;
    Run next = Run::None;
    for (size_t k = slots.size(); k-- > 0;) {
        if (slots[k].run == Run::None) continue;
        x[k] = std::round(std::min(x[k], limit - SpacerBetween(slots[k].run, next, iconToWidget, widgetGap) - slots[k].w));
        limit = x[k];
        next = slots[k].run;
    }
    end = startInner;
    for (size_t k = 0; k < slots.size(); ++k) {
        if (slots[k].run == Run::None) continue;
        leads[k] = std::max(0.0f, x[k] - end);
        end += leads[k] + slots[k].w;
    }
    return true;
}

// The anchored slots of `order` (indexes into `items`): each slot's run and length as the layout
// measured it, its group (the config item behind it: DockItem::element; slot `dragged`, while
// dragged: `draggedGroup`) and its offset in px (once per config item: the first of the running
// apps' slots carries the running item's).
std::vector<AnchorSlot> MakeAnchorSlots(const std::vector<DockItem>& items, const std::vector<size_t>& order, const DockConfig& d,
                                        float scale, int dragged = -1, int draggedGroup = -1) {
    std::vector<AnchorSlot> slots;
    slots.reserve(order.size());
    size_t lastElement = SIZE_MAX;
    for (const size_t i : order) {
        const DockItem& item = items[i];
        AnchorSlot s;
        s.run = !item.IsWidget() ? Run::Icon : item.slotW > 0.0f ? Run::Widget : Run::None;
        s.w = s.run == Run::None ? 0.0f : item.slotW;
        s.flush = s.run == Run::Widget && item.flush;
        s.group =static_cast<int>(i) == dragged && draggedGroup >= 0 ? draggedGroup : d.GroupOf(item.element);
        if (item.element != lastElement && item.element < d.items.size()) {
            s.offset = std::round(static_cast<float>(d.items[item.element].offset) * scale);
        }
        lastElement = item.element;
        slots.push_back(s);
    }
    return slots;
}

}  // namespace

namespace {

// A group member dragged out of the group popup (DockWindow::DragGroupMember): a private
// clipboard format, so only a dock of this process (the one that owns the group) takes it.
struct MemberDragData {
    DWORD pid = 0;
    UINT64 dock = 0;  // the HWND of the dock whose group it is
    UINT32 member = 0;
    wchar_t group[96]{};
};

CLIPFORMAT MemberFormat() {
    static const CLIPFORMAT format = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"WinDock.GroupMember"));
    return format;
}

bool ReadMemberDrag(IDataObject* data, MemberDragData& out) {
    if (!data || !MemberFormat()) return false;
    FORMATETC fmt{MemberFormat(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    if (FAILED(data->GetData(&fmt, &medium))) return false;
    bool ok = false;
    if (medium.hGlobal && GlobalSize(medium.hGlobal) >= sizeof(MemberDragData)) {
        if (const void* p = GlobalLock(medium.hGlobal)) {
            memcpy(&out, p, sizeof(out));
            out.group[std::size(out.group) - 1] = 0;
            GlobalUnlock(medium.hGlobal);
            ok = out.pid == GetCurrentProcessId();
        }
    }
    ReleaseStgMedium(&medium);
    return ok;
}

// The drag source of a member drag: the left button up drops, Escape cancels.
class MemberDropSource final : public IDropSource {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDropSource)) {
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
        if (!(keys & MK_LBUTTON)) return DRAGDROP_S_DROP;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

private:
    ~MemberDropSource() = default;
    LONG refs_ = 1;
};

}  // namespace

// OLE drop target for the dock HWND: forwards to the DockWindow (on the UI thread; OLE calls
// drop targets on the thread that registered them).
class DockWindow::DropTarget final : public IDropTarget {
public:
    explicit DropTarget(DockWindow* dock) : dock_(dock) {}
    void Detach() { dock_ = nullptr; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDropTarget)) {
            *ppv = static_cast<IDropTarget*>(this);
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
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override {
        if (!effect) return E_INVALIDARG;
        *effect = dock_ ? dock_->OnDragEnter(data, POINT{pt.x, pt.y}, *effect) : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL pt, DWORD* effect) override {
        if (!effect) return E_INVALIDARG;
        *effect = dock_ ? dock_->OnDragOver(POINT{pt.x, pt.y}, *effect) : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        if (dock_) dock_->OnDragLeave();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override {
        if (!effect) return E_INVALIDARG;
        *effect = dock_ ? dock_->OnDrop(data, POINT{pt.x, pt.y}, *effect) : DROPEFFECT_NONE;
        return S_OK;
    }

private:
    ~DropTarget() = default;
    DockWindow* dock_;
    LONG refs_ = 1;
};

DockWindow::~DockWindow() { Destroy(); }

HRESULT DockWindow::Create(HINSTANCE instance, const Config* config, size_t dockIndex, HMONITOR monitor,
                           DockShared* shared, MessageHook hook) {
    config_ = config;
    dockIndex_ = dockIndex;
    monitor_ = monitor;
    shared_ = shared;
    instance_ = instance;
    hook_ = std::move(hook);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &DockWindow::WndProcThunk;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    // Start on the target monitor so GetDpiForWindow reports that monitor's DPI.
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(PickMonitor(), &mi);
    hwnd_ = CreateWindowExW(kExStyle, kClassName, L"WinDock", kStyle, mi.rcMonitor.left, mi.rcMonitor.bottom - 1, 1, 1,
                            nullptr, nullptr, instance, this);
    if (!hwnd_) return HRESULT_FROM_WIN32(GetLastError());
    dpi_ = GetDpiForWindow(hwnd_);

    // Live window thumbnails need a multithreaded D3D device (Windows.Graphics.Capture).
    HRESULT hr = renderer_.Initialize(hwnd_, config_->renderer == L"software", DockCfg().thumbnails == ThumbnailMode::Live);
    if (FAILED(hr)) {
        Log(L"Renderer::Initialize", hr);
        return hr;
    }
    hr = CreateLayers();
    if (FAILED(hr)) {
        Log(L"CreateLayers", hr);
        return hr;
    }
    thumbs_.Init(hwnd_, &renderer_, [this](const std::vector<HWND>& windows) { OnThumbnailFrames(windows); });
    preview_.Init(instance, &renderer_,
                  PreviewPopup::Callbacks{
                      [this](HWND w) {
                          Activation::BringToFront(w);
                          DismissPreview(true);
                      },
                      [](HWND w) {
                          if (IsWindow(w)) PostMessageW(w, WM_CLOSE, 0, 0);
                      },
                      [this](bool inside) { OnPreviewHover(inside); },
                  });
    folder_.Init(instance, &renderer_, &icons_,
                 FolderPopup::Callbacks{
                     [this] { DismissFolderPopup(true); },
                     [this] { OnFolderPopupClosed(); },
                 });
    // The forecast of the weather slot that opened it (round 5: its item's place).
    weatherPopup_.Init(instance, &renderer_,
                       [this]() -> const WeatherService* {
                           WeatherConfig place;
                           return PlaceOfSlot(SlotOfKey(weatherKey_), place) ? WeatherFor(place) : nullptr;
                       },
                       WeatherPopup::Callbacks{
                           [this] {
                               WeatherConfig place;
                               if (PlaceOfSlot(SlotOfKey(weatherKey_), place)) shared_->RefreshWeather(&place);
                           },
                           [this] {
                               const int slot = SlotOfKey(weatherKey_);
                               DismissWeatherPopup(true);
                               OpenItemSettings(slot);
                           },
                           [this] { ReleasePopupHold(kHoldWeather, true); },
                       });
    centre_.Init(instance, &renderer_,
                 NotificationPopup::Callbacks{
                     [this] {  // closed by itself: hidden already
                         centreOpener_.clear();
                         ReleasePopupHold(kHoldCentre, true);
                         if (shared_) shared_->SetCentreOpen(false);
                     },
                     [this](uint32_t id) {
                         // A row: its app to the front, the notification dismissed (as Windows does).
                         DismissCentre(true);
                         if (!shared_) return;
                         shared_->ActivateNotification(id);
                         shared_->Notifications().Dismiss(id);
                     },
                     [this](uint32_t id) {
                         if (shared_) shared_->Notifications().Dismiss(id);
                     },
                     [this] {
                         if (shared_) shared_->Notifications().DismissAll();
                     },
                     [this] {
                         DismissCentre(true);
                         OpenTarget(L"ms-settings:privacy-notifications");
                     },
                     [this](const NotificationItem& n, UINT px) -> ID2D1Bitmap* {
                         if (n.key.id.empty()) return nullptr;
                         return icons_.Get(n.key, nullptr, px, renderer_.ResourceContext(), L"", 0);
                     },
                 });
    calendar_.Init(instance, &renderer_,
                   CalendarPopup::Callbacks{
                       [this] { ReleasePopupHold(kHoldCalendar, true); },
                       [this] {
                           const std::wstring target = calendarTarget_;  // copied: the popup closes first
                           DismissCalendarPopup(true);
                           OpenTarget(target);
                       },
                       [this](const std::wstring& url) {
                           const std::wstring target = url;  // copied: the popup (and its rows) go first
                           DismissCalendarPopup(true);
                           OpenTarget(target);
                       },
                   });

    reminder_.Init(instance, &renderer_,
                   ReminderBanner::Callbacks{
                       [this] { ReminderAction(1); },
                       [this] { ReminderAction(2); },
                       [this] { ReminderAction(3); },
                   });

    widgetPopup_.Init(instance, &renderer_,
                      ScriptPopup::Callbacks{
                          [this](ID2D1DeviceContext* dc, float w, float h, float scale) {
                              if (WidgetInstance* widget = WidgetByKey(popupWidget_)) widget->RenderPopup(dc, w, h, scale);
                          },
                          [this](float x, float y, int button) {
                              if (WidgetInstance* widget = WidgetByKey(popupWidget_)) widget->PopupClick(x, y, button);
                          },
                          [this] {
                              // Closed by itself (Escape, another window came forward): hidden already.
                              const std::wstring key = std::move(popupWidget_);
                              popupWidget_.clear();
                              ReleasePopupHold(kHoldWidget, true);
                              if (WidgetInstance* widget = WidgetByKey(key)) widget->PopupClosed();
                          },
                      });
    // The passive one: drawn by the same drawPopup; it never sees the mouse and never closes
    // itself (a hover popup), so its click / closed callbacks stay empty.
    widgetPassive_.SetMouseTransparent(true);
    widgetPassive_.Init(instance, &renderer_,
                        ScriptPopup::Callbacks{
                            [this](ID2D1DeviceContext* dc, float w, float h, float scale) {
                                if (WidgetInstance* widget = WidgetByKey(passiveWidget_)) widget->RenderPopup(dc, w, h, scale);
                            },
                            {},
                            {},
                        });

    // File drops (OLE: needs OleInitialize on this thread, see main.cpp).
    dropTarget_ = new DropTarget(this);
    if (const HRESULT dr = RegisterDragDrop(hwnd_, dropTarget_); FAILED(dr)) {
        Log(L"RegisterDragDrop", dr);
        dropTarget_->Detach();
        dropTarget_->Release();
        dropTarget_ = nullptr;
    }

    msgQueryItem_ = RegisterWindowMessageW(kQueryItemMessage);
    msgQueryState_ = RegisterWindowMessageW(kQueryStateMessage);
    // Resolves the pins and builds the elements (SyncElements) before the first layout.
    ApplyConfig();
    // A desktop-layer dock starts hidden unless the desktop is the foreground (UpdateDesktopShown).
    desktopHidden_ = DesktopLayered() && !DesktopLayer::DesktopForeground();
    if (!desktopHidden_) ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    if (DesktopLayered()) DesktopLayer::PlaceAboveDesktop(hwnd_);
    if (BehindLayered()) DesktopLayer::Place(hwnd_);  // registered by ApplyConfig (SyncLayer)
    desktopLayerWas_ = DockCfg().layer;
    // The shared tracker's current window list then builds and lays out the items with their windows.
    tracking_ = true;
    RebuildFrom(shared_->Tracker().Current());
    ApplyAutoHide();  // hides after hideDelayMs unless the mouse is on the dock
    return S_OK;
}

void DockWindow::Destroy() {
    if (hwnd_) DestroyWindow(hwnd_);
}

void DockWindow::SetMonitor(HMONITOR monitor) {
    if (monitor == monitor_) return;
    monitor_ = monitor;
    if (!hwnd_) return;
    // A new monitor: its size / work area (and maybe DPI: WM_DPICHANGED follows the move), and a
    // monitor filter shows other windows.
    if (DockCfg().runningFilter == RunningFilter::Monitor) RebuildFrom(shared_->Tracker().Current());
    ApplyConfig();
}

void DockWindow::OnSnapshot(const WindowTracker::Snapshot& snap) {
    if (tracking_ && hwnd_) RebuildFrom(snap);
}

void DockWindow::OnWindowTitles(const std::vector<HWND>& windows) {
    if (!tracking_ || !hwnd_ || windows.empty()) return;
    bool changed = false;
    for (size_t i = 0; i < items_.size(); ++i) {
        DockItem& item = items_[i];
        if (!item.window || std::find(windows.begin(), windows.end(), item.window) == windows.end()) continue;
        std::wstring title = WindowTracker::WindowTitle(item.window);
        if (title == item.windowTitle) continue;
        item.windowTitle = std::move(title);
        changed = true;
        if (layout_.captionLines > 0 && CaptionFor(item) != item.captionText) item.captionDirty = true;
        if (static_cast<int>(i) == hovered_) labelDirty_ = true;  // the label shows the new title
    }
    // The preview popup's cards show the window titles of every slot kind.
    if (preview_.UpdateTitles(windows)) changed = true;
    if (!changed) return;
    ++titleUpdates_;
    // Redraws only the dirty captions and the label (re-placed if its width changed); commits the
    // popup's repaint too (one device).
    RenderWithRecovery([this] { return DrawFrame(false); });
}

bool DockWindow::ClickSlot(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return false;
    ElementOf(static_cast<size_t>(index)).OnClick(index, false);
    return true;
}

void DockWindow::RebuildFrom(const WindowTracker::Snapshot& snap) {
    if (DockCfg().runningFilter != RunningFilter::Monitor) {
        Rebuild(snap);
        return;
    }
    // Only the windows whose centre is on this dock's monitor.
    const HMONITOR mine = PickMonitor();
    WindowTracker::Snapshot filtered;
    filtered.foreground = snap.foreground;
    for (const WindowTracker::Group& g : snap.groups) {
        WindowTracker::Group f;
        f.key = g.key;
        f.wantsAttention = g.wantsAttention;  // per app (the tracker does not say which window flashes)
        for (size_t i = 0; i < g.windows.size(); ++i) {
            if (i < g.monitors.size() && g.monitors[i] != mine) continue;
            f.windows.push_back(g.windows[i]);
            f.monitors.push_back(i < g.monitors.size() ? g.monitors[i] : mine);
        }
        if (!f.windows.empty()) filtered.groups.push_back(std::move(f));
    }
    if (filtered.foreground && std::none_of(filtered.groups.begin(), filtered.groups.end(), [&](const auto& g) {
            return std::find(g.windows.begin(), g.windows.end(), filtered.foreground) != g.windows.end();
        })) {
        filtered.foreground = nullptr;
    }
    Rebuild(filtered);
}

void DockWindow::ApplyConfig() {
    RefreshMotion();
    DismissPreview(false, false);  // previews / theme / sizes may have changed
    DismissClickPopups(false, false);
    DismissWidgetPassive(false);
    FinishLabelFade();             // its text / position may no longer match
    SyncElements();                // the clock may have been turned on / off; config re-read, timers
    // docks[].windows changed: the slots change (one per app <-> one per window).
    // docks[].thumbnails turned on / off: slots become tiles or icons again.
    if (tracking_ && (builtUnstacked_ != (DockCfg().windows == WindowsMode::Unstacked) ||
                      builtThumbnails_ != (DockCfg().thumbnails != ThumbnailMode::Off))) {
        RebuildFrom(shared_->Tracker().Current());
    }
    // The drop marker is only redrawn when its size changes: force it (the accent may differ).
    if (marker_.visual) renderer_.DrawLayer(marker_, 0, 0, {});
    for (DockItem& item : items_) {
        item.dirty = true;
        item.dotDirty = true;
        item.captionDirty = true;
    }
    labelDirty_ = true;
    labelFormat_ = nullptr;  // theme / size may have changed
    labelEllipsis_ = nullptr;
    weatherQuiet_ = true;  // the relayout below measures and draws the widget
    shared_->SyncWeather();
    weatherQuiet_ = false;
    UpdateAcrylic();
    SyncLayer();  // the layer (or the backdrop beneath a "behind" dock) may have changed
    Relayout();
    SyncThumbnails();  // mode, rate, interval, the tile size
    ApplyAutoHide();
    UpdateDesktopShown();  // the layer may have changed
    if (desktopLayerWas_ != DockCfg().layer) {
        // Another layer: placed by its rules now (normal: topmost again; desktop / behind: above
        // the desktop window). ApplyWindowPos above only re-asserted the new layer's z-order.
        desktopLayerWas_ = DockCfg().layer;
        AssertTopmost();
        if (backdrop_.Exists()) backdrop_.Restack(hwnd_);
    }
}

HMONITOR DockWindow::PickMonitor() const {
    // The manager picks the monitor (docks[].monitor: an index, "primary", one per monitor for
    // "each"; a dock whose monitor is not connected is parked, so it has no window). A handle that
    // went stale (displays changing; the manager re-assigns it shortly) reads as the primary.
    MONITORINFO mi{sizeof(mi)};
    if (monitor_ && GetMonitorInfoW(monitor_, &mi)) return monitor_;
    return MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
}

void DockWindow::MonitorRects(RECT& monitor, RECT& work) const {
    // The one place the dock's monitor comes from.
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(PickMonitor(), &mi);
    monitor = mi.rcMonitor;  // the taskbar is hidden, so the dock uses the full monitor
    work = mi.rcWork;
}

void DockWindow::ComputeLayout() {
    const DockConfig& d = DockCfg();
    ComputeLayoutAt(d.iconSize);
    if (d.overflow != OverflowMode::Shrink || d.Polar()) return;  // a ring grows its radius, then scrolls
    // "shrink": smaller icons until the run fits the room along the edge (the gaps, padding,
    // spacers and widgets keep their size). The widgets measure against the icon size, so a pass
    // may still be a little long: a few passes, never below kMinShrinkDip (then it is clipped).
    constexpr int kMinShrinkDip = 16;
    for (int pass = 0; pass < 4; ++pass) {
        const Layout& l = layout_;
        float room = static_cast<float>(DockGeometry::MainEnd(l.edge, l.monitor) - DockGeometry::MainStart(l.edge, l.monitor)) -
                     (l.fullWidth ? 0.0f : 2.0f * std::round(kBottomMargin * l.scale));
        room = std::min(room, l.roomPx);  // docks[].maxLength (round 7 phase 7A)
        const float barW = l.fullWidth ? (l.itemsW + 2.0f * l.padding) : (l.bar.right - l.bar.left);
        if (barW <= room + 0.5f || l.iconDip <= kMinShrinkDip) return;
        float iconsW = 0.0f;  // the part that scales with the icon size
        for (const DockItem& item : items_) {
            if (item.Magnifies()) iconsW += BoxW(item);
        }
        if (iconsW <= 0.0f) return;
        const float factor = std::clamp((room - (barW - iconsW)) / iconsW, 0.0f, 1.0f);
        int next = static_cast<int>(std::floor(static_cast<float>(l.iconDip) * factor));
        next = std::clamp(next, kMinShrinkDip, l.iconDip - 1);
        ComputeLayoutAt(next);
    }
}

void DockWindow::ComputeLayoutAt(int iconDip) {
    const DockConfig& d = DockCfg();
    Layout l;
    l.scale = static_cast<float>(dpi_) / USER_DEFAULT_SCREEN_DPI;
    const float s = l.scale;
    l.edge = d.edge;
    // A ring (docks[].shape "arc" / "circle") is laid out like a horizontal bar and wrapped onto
    // the ring at the end of this function: no vertical layout, no full width.
    const bool polar = d.Polar();
    l.shape = d.shape;
    l.rotateIcons = polar && d.rotateIcons;
    l.vertical = !polar && IsVertical(d.edge);
    // Round 6 phase 6A: a floating bar is placed at x / y; it is an edge dock whose "screen edge" is
    // its own outer side (no margin, no full width).
    l.floating = d.Floating();
    // Full width: the bar spans the monitor's edge, flush with the screen edge, square corners.
    l.fullWidth = d.fullWidth && !polar && !l.floating;
    l.iconDip = iconDip;

    l.iconPx = std::round(static_cast<float>(iconDip) * s);
    l.padding = std::round(kPadding * s);
    // With acrylic, DWM rounds the blur with its own radius; the tint matches it.
    l.cornerRadius = l.fullWidth ? 0.0f : std::round((acrylic_ ? acrylicRadiusDip_ : DockTheme().cornerRadius) * s);
    l.magnification = static_cast<float>(d.magnification);
    l.bitmapPx = static_cast<UINT>(std::lround(static_cast<double>(iconDip) * d.magnification * s));

    // Icon effects. The reflection surface is drawn at the bitmap size and shown at rest scale
    // under each icon: the bar grows by its height. The glow reaches glowPad beyond the icon box.
    // Vertical docks have no reflections.
    const ThemeConfig& t = DockTheme();
    // Round 11 phase 11A: the theme's spacing (iconGap, barPadding) and slot buttons (not on a ring:
    // a frame cannot follow a sector; a ring keeps its dots). With the defaults (8 / 8 / none) every
    // number below is what the constants gave before.
    l.gap = std::round(std::clamp(t.iconGap, 0.0f, 48.0f) * s);
    const float barPadDip = std::clamp(t.barPadding, 0.0f, 24.0f);
    l.buttons = SlotStyleOf(t) == SlotStyle::Buttons && !polar;
    l.iconTopPx = std::round((l.buttons ? barPadDip : barPadDip * kIconTopShare) * s);
    const float dotFromEdge = std::round(barPadDip * kIconTopShare * s);  // bar edge -> the dots' centre (5 DIPs at 8)
    const bool reflect = t.reflection && !l.vertical && !polar;  // a ring has no reflections
    const UINT reflSurface = reflect ? IconEffects::ReflectionHeight(l.bitmapPx, t.reflectionHeight) : 0;
    l.reflH = reflSurface && l.bitmapPx ? std::ceil(static_cast<float>(reflSurface) * l.iconPx /
                                                    static_cast<float>(l.bitmapPx))
                                        : 0.0f;
    // Clipped reflections (theme.reflectionClip) do not grow the bar: the icons are clipped at the
    // bar's edge instead (ApplyReflectionClip), showing the part of the reflection that fits.
    l.reflClip = reflSurface > 0 && t.reflectionClip;
    if (l.reflClip) l.reflH = 0.0f;
    const GlowMode glow = GlowOf(t);
    l.glowPad = glow != GlowMode::Off ? std::ceil(t.glowRadius * s) + 1.0f : 0.0f;

    // Titles under the icons: a band between the icon (and its reflection) and the dots, the
    // label's font at kCaptionFontSize; on vertical docks a column beside the icon (toward the
    // screen edge) or none (titlesVertical "off"). The bar's cross size grows by the band.
    int lines = d.titles == TitlesMode::One ? 1 : d.titles == TitlesMode::Two ? 2 : 0;
    if (l.vertical && d.titlesVertical == TitlesVertical::Off) lines = 0;
    if (polar) lines = 0;  // no titles under the icons of a ring
    if (lines && !EnsureCaptionFormat(s)) lines = 0;
    l.captionLines = lines;
    float captionGap = 0.0f;
    if (lines) {
        const float textH = std::ceil(captionLineH_ * static_cast<float>(lines));
        if (l.vertical) {
            captionGap = std::round(kCaptionBesideGap * s);
            l.captionCross = std::round(kCaptionBesideWidth * s);
            l.captionMain = l.iconPx;
            l.captionShift = 0.0f;
        } else {
            captionGap = std::round(kCaptionGap * s);
            l.captionCross = textH;
            l.captionMain = l.iconPx + l.gap;  // the icon's slot
            l.captionShift = -std::floor(l.gap / 2.0f);
        }
        l.captionBand = captionGap + l.captionCross;
    }

    // The title (docks[].title): upright, in the label's font, kLabelGap beyond the bar's far side.
    // Round 12 phase 12C: laid out before the bar's size (an "inside" title adds its band to the
    // bar); its text is TitleText() (the dock's name with titleUseName; none while titleShow is off).
    const std::wstring titleText = d.TitleText();
    float titleMain = 0.0f, titleCross = 0.0f;
    const float titleGap = std::round(kLabelGap * s);
    l.titleInside = false;
    l.titleBand = 0.0f;
    if (!titleText.empty() && EnsureLabelFormat(s)) {
        if (!titleLayout_ || titleText_ != titleText || titleScale_ != s) {
            titleLayout_ = nullptr;
            if (FAILED(renderer_.DWrite()->CreateTextLayout(titleText.c_str(), static_cast<UINT32>(titleText.size()),
                                                            labelFormat_.get(), std::round(kLabelMaxWidth * s), 1000.0f,
                                                            titleLayout_.put()))) {
                titleLayout_ = nullptr;
            }
            titleText_ = titleText;
            titleScale_ = s;
            titleDirty_ = true;
        }
        DWRITE_TEXT_METRICS tm{};
        if (titleLayout_ && SUCCEEDED(titleLayout_->GetMetrics(&tm))) {
            const float textW = std::ceil(std::min(tm.widthIncludingTrailingWhitespace, std::round(kLabelMaxWidth * s)));
            const float w = textW + 2.0f * std::round(10.0f * s);
            const float h = std::ceil(tm.height) + 2.0f * std::round(4.0f * s);
            titleMain = l.vertical ? h : w;
            titleCross = l.vertical ? w : h;
            // Round 12 phase 12C: "inside" (a top / bottom bar only; a vertical bar or a ring draws it
            // outside): a band of the label font's height + 4 DIPs inside the bar, the text without
            // its pill.
            if (d.titlePlace == TitlePlace::Inside && !l.vertical && !polar) {
                l.titleInside = true;
                l.titleBand = std::ceil(tm.height) + std::round(4.0f * s);
                titleMain = textW + 2.0f * std::round(4.0f * s);
                titleCross = l.titleBand;
            }
        }
    } else if (titleLayout_) {
        titleLayout_ = nullptr;
        titleText_.clear();
        titleDirty_ = true;
    }
    // The band sits on the bar's far side: the icon row (and everything measured from the bar's top)
    // moves down by it, the bar grows away from the screen edge. Magnified icons grow from the icon
    // row, which stays as far from the window's edge side as before.
    l.iconTopPx += l.titleBand;
    if (l.titleInside != layout_.titleInside) titleDirty_ = true;  // drawn with / without its pill

    // The bar without reflections (the icon row and the titles).
    const float iconBarH = std::round(l.iconPx + 2.0f * barPadDip * s) + l.captionBand + l.titleBand;
    const float barH = iconBarH + l.reflH;
    const float margin = l.fullWidth || l.floating ? 0.0f : std::round(kBottomMargin * s);
    // --- round 12 phase 12E --- docks[].edgeGap: the resting bar this much further from its screen
    // edge (a bar on an edge that is not full width). The window still stands on the screen edge and
    // grows by the gap (the gap is part of the dock's zone, as the margin is): the stage's far side,
    // the slide, the reveal strip and the peek strip are measured from the screen edge as before, the
    // acrylic follower keeps the window's rect, and the app bar band (bar top -> screen edge) includes
    // the gap. `margin` stays the along-the-edge margin; `crossMargin` is the bar -> screen edge.
    const float gapPx =
        l.fullWidth || l.floating || polar ? 0.0f : std::round(static_cast<float>(std::clamp(d.edgeGap, 0, DockConfig::kMaxEdgeGap)) * s);
    const float crossMargin = margin + gapPx;
    float winH = std::round(l.iconPx * l.magnification + (l.vertical ? kVerticalWindowExtra : kWindowExtraHeight) * s) +
                 l.reflH + l.captionBand + gapPx;
    winH = std::max(winH, barH + crossMargin);
    if (titleCross > 0.0f && !l.titleInside) winH = std::max(winH, barH + crossMargin + titleGap + titleCross);
    // Round 12 phase 12D: customize mode's anchor targets (10 DIPs beyond the bar, 20 DIPs across)
    // and its outline need this much stage beyond the bar's far side.
    if (editing_) winH = std::max(winH, barH + crossMargin + std::round(36.0f * s));
    if (l.glowPad > 0.0f) {
        // Room above a fully magnified icon for its halo.
        const float magTopRel =
            winH - crossMargin - barH + l.iconTopPx + l.iconPx - l.iconPx * l.magnification;
        winH += std::max(0.0f, std::ceil(l.glowPad * l.magnification) - magTopRel);
    }

    // Slots, in dock order, as their elements measure them: an icon slot is iconPx + gap (the icon
    // centred), a separator a narrow slot, a widget its content box. Spacers (`lead`, never
    // scaled) separate icons from widgets (with a hairline when the widgets are at an end of the
    // dock) and widgets from each other; the bar adds padding at both ends (half a gap less beside
    // an icon slot, whose slot already holds half a gap on each side).
    const float half = std::floor(l.gap / 2.0f);  // (l.gap: theme.iconGap, set above)
    const float stride = l.iconPx + l.gap;
    const float sepW = std::round(kSeparatorWidth * s);
    const float widgetGap = std::round(kWidgetGap * s);
    // Round 11 phase 11C: a full-width bar tells flush widgets its thickness (l.barCross).
    l.barCross = l.fullWidth ? iconBarH - l.titleBand : 0.0f;  // round 12 phase 12C: the icon row's, not the title band's
    const LayoutContext ctx{l.vertical ? DockAxis::Vertical : DockAxis::Horizontal, s, l.iconPx, renderer_.DWrite(), l.barCross};
    Run first = Run::None, prev = Run::None;
    bool firstFlush = false, lastFlush = false;  // the first / last real slot is a flush widget
    const float tilePx = std::round(
        static_cast<float>(d.thumbnailSize > 0 ? d.thumbnailSize : 2 * iconDip) * s);
    float maxBox = l.iconPx;  // the longest magnifying box (a thumbnail tile is longer than an icon)
    size_t icons = 0;  // icon and separator slots (they make up the "items" of the bar)
    float itemsW = 0.0f;
    l.firstMag = l.lastMag = -1;
    for (size_t i = 0; i < items_.size(); ++i) {
        DockItem& item = items_[i];
        DockElement& element = ElementOf(i);
        Run run = Run::Icon;
        if (item.IsWidget()) {
            if (element.Update()) item.dirty = true;  // the clock's text changed
            const ElementSize box = element.Measure(ctx);
            if (box.main != item.slotW || box.cross != item.boxCross) item.dirty = true;
            item.slotW = box.main;
            item.boxCross = box.cross;
            item.hitSlack = element.HitSlack(ctx);
            item.flush = box.flush && l.fullWidth;
            run = box.main > 0.0f ? Run::Widget : Run::None;  // nothing to show: takes no room
        } else {
            item.flush = false;
            const ElementSize box = element.Measure(ctx);
            // A thumbnail tile (docks[].thumbnails) is longer along the bar than an icon.
            const float main = item.thumbWindow && item.Magnifies() ? tilePx : box.main;
            item.boxMain = main;
            if (item.Magnifies()) maxBox = std::max(maxBox, main);
            item.slotW = item.Magnifies() ? main + l.gap : main;
            item.boxCross = l.iconPx;
            item.hitSlack = 0.0f;
            ++icons;
            if (item.Magnifies()) {
                if (l.firstMag < 0) l.firstMag = static_cast<int>(i);
                l.lastMag = static_cast<int>(i);
            }
        }
        item.lead = 0.0f;
        if (run == Run::None) continue;
        item.lead = SpacerBetween(prev, run, sepW, widgetGap);
        if (first == Run::None) {
            first = run;
            firstFlush = item.flush;
        }
        prev = run;
        lastFlush = item.flush;
        itemsW += item.lead + item.slotW;
    }
    l.itemsW = itemsW;
    // A flush widget at an end of a full-width bar has no padding on that side (round 11 phase 11C).
    const float lead = firstFlush ? 0.0f : first == Run::Icon ? l.padding - half : l.padding;
    float trail = lastFlush ? 0.0f : prev == Run::Icon ? l.padding - half : l.padding;
    const size_t n = icons;
    float barW = lead + itemsW + trail;
    barW = std::round(std::max(barW, l.iconPx + 2.0f * l.padding));

    // Magnified icons widen the run by sum((scale - 1) * iconPx). Both falloffs (cosine, linear) sum
    // to at most magnifyRange over all items (and each term is at most 1), which bounds the growth.
    l.growMax = 0.0f;
    if (n && l.magnification > 1.001f) {
        const float terms = static_cast<float>(std::min<size_t>(n, static_cast<size_t>(d.magnifyRange)));
        l.growMax = std::ceil((l.magnification - 1.0f) * maxBox * terms) + 2.0f;
    }
    const float magGrow = l.growMax;  // the magnification's part (a steady bar grows by this much)
    // A file dragged over the dock opens a one-slot gap: the bar grows by half a slot per side.
    if (n) l.growMax = std::max(l.growMax, std::ceil(stride / 2.0f) + 2.0f);
    // The bar may grow by up to growMax on either side (it grows away from the cursor).
    float slack = std::max(l.growMax, std::round(kMinSideSlack * s));
    float winW = barW + 2.0f * slack;

    // Placement along the edge: the bar centred (the stage centred on the monitor, as always), or
    // `margin` from the start / end, plus `offset` DIPs; the bar is kept on the monitor and the
    // stage is cut at the monitor's ends (the bar then grows away from that end).
    MonitorRects(l.monitor, l.work);
    const RECT& m = l.monitor;
    const LONG mStart = DockGeometry::MainStart(l.edge, m), mEnd = DockGeometry::MainEnd(l.edge, m);
    const LONG offsetPx = static_cast<LONG>(std::lround(static_cast<double>(d.offset) * s));
    // Overflow (docks[].overflow): the run does not fit the room along the edge (the monitor's
    // extent minus the margins). "scroll": the bar takes that room and the run scrolls inside it
    // (layout_.view); the bar never grows then (a magnified run is clipped at the bar's ends).
    // "clip" (and "shrink" once the icons cannot get smaller): the bar starts at the monitor's start
    // and the stage cuts it at its end, as always.
    float roomPx = static_cast<float>(mEnd - mStart) - 2.0f * margin;
    // docks[].maxLength (round 7 phase 7A): a cap on that room, so the bar hugs a shorter run and
    // the overflow mode takes over at the cap as it does at the monitor's ends (not for a ring or a
    // full-width bar).
    if (d.maxLength > 0 && !polar && !l.fullWidth) {
        roomPx = std::min(roomPx, std::round(static_cast<float>(d.maxLength) * s));
    }
    l.roomPx = roomPx;
    // docks[].magnifyBar "always" (9A follow-up): the resting bar already has the room a magnified
    // run grows into, half before the first magnifying slot and half after the last one (between
    // the icons and the widgets at the ends, so those never move); the bar never grows. Not for a
    // ring, a full-width or an anchored bar, nor when the room would make the run overflow.
    l.always = false;
    l.padStartSlot = l.padEndSlot = -1;
    float alwaysStart = 0.0f, alwaysEnd = 0.0f;
    if (d.magnifyBar == MagnifyBar::Always && !polar && !l.fullWidth && !d.Anchored() && magGrow > 0.0f && l.firstMag >= 0 &&
        lead + itemsW + trail + magGrow <= roomPx + 0.5f) {
        l.always = true;
        alwaysStart = std::floor(magGrow / 2.0f);
        alwaysEnd = magGrow - alwaysStart;
        l.padStartSlot = l.firstMag;
        items_[static_cast<size_t>(l.firstMag)].lead += alwaysStart;
        for (size_t i = static_cast<size_t>(l.lastMag) + 1; i < items_.size() && l.padEndSlot < 0; ++i) {
            if (!items_[i].IsWidget() || items_[i].slotW > 0.0f) l.padEndSlot = static_cast<int>(i);
        }
        itemsW += alwaysStart;
        if (l.padEndSlot >= 0) {
            items_[static_cast<size_t>(l.padEndSlot)].lead += alwaysEnd;
            itemsW += alwaysEnd;
        } else {
            trail += alwaysEnd;  // nothing after the last icon: the bar's own end
        }
        l.itemsW = itemsW;
        barW = std::round(std::max(lead + itemsW + trail, l.iconPx + 2.0f * l.padding));
        winW = barW + 2.0f * slack;
    }
    // --- round 12 phase 12D --- customize mode: a slot's room for each "+" (right after the pinned
    // run, before the next element's slot, and at the run's end), on a bar whose run still fits the
    // room with them. Not on a ring, an anchored bar (its gaps are the groups') or a run that would
    // scroll: there the edit bar's Add items opens the drawer. Like "always"'s room it is on the
    // icon side of a spacer, so a hairline stays beside the widgets.
    l.plusPinSlot = -1;
    l.plusEnd = false;
    l.plusRoom = 0.0f;
    if (editing_ && !polar && !d.Anchored()) {
        int lastPin = -1;
        for (size_t i = 0; i < items_.size(); ++i) {
            if (items_[i].pinned) lastPin = static_cast<int>(i);
        }
        int pinSlot = -1;
        for (size_t i = lastPin >= 0 ? static_cast<size_t>(lastPin) + 1 : items_.size(); i < items_.size() && pinSlot < 0; ++i) {
            if (items_[i].element == items_[static_cast<size_t>(lastPin)].element) continue;  // the pin's own window slots
            if (items_[i].IsWidget() && items_[i].slotW <= 0.0f) continue;                    // takes no room
            pinSlot = static_cast<int>(i);
        }
        const float extra = stride * (pinSlot >= 0 ? 2.0f : 1.0f);
        if (lead + itemsW + trail + extra <= roomPx + 0.5f) {
            l.plusRoom = stride;
            l.plusEnd = true;
            trail += stride;
            if (pinSlot >= 0) {
                l.plusPinSlot = pinSlot;
                items_[static_cast<size_t>(pinSlot)].lead += stride;
                itemsW += stride;
            }
            l.itemsW = itemsW;
            barW = std::round(std::max(lead + itemsW + trail, l.iconPx + 2.0f * l.padding));
            winW = barW + 2.0f * slack;
        }
    }
    // --- end round 12 phase 12D ---
    // Item anchors (round 7 phase 7B): with an anchor or an offset on some item, a bar whose run fits
    // the room is as long as the room (full width: the edge) and its items are laid out as groups
    // (AnchoredLeads: the gaps between the groups become leads, so everything else still sees one
    // run). A run that does not fit overflows as usual (the anchors wait until it fits again). A
    // floating bar has no room of its own: anchors need its maxLength.
    std::vector<float> anchorLeads, baseLeads;
    if (d.Anchored() && !polar && itemsW > 0.0f && (!l.floating || d.maxLength > 0)) {
        const float len = l.fullWidth ? static_cast<float>(mEnd - mStart) : std::floor(roomPx);
        std::vector<size_t> order(items_.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        if (lead + itemsW + trail <= len + 0.5f &&
            AnchoredLeads(MakeAnchorSlots(items_, order, d, s), 0.0f, len, l.padding, half, sepW, widgetGap, anchorLeads)) {
            l.anchored = true;
            if (!l.fullWidth) {
                barW = len;
                winW = barW + 2.0f * slack;
            }
        }
    }
    l.view.on = !l.anchored && d.overflow == OverflowMode::Scroll && itemsW > 0.0f && lead + itemsW + trail > roomPx + 0.5f;
    if (l.view.on) {
        if (!l.fullWidth) barW = std::floor(roomPx);
        l.growMax = 0.0f;
        slack = std::round(kMinSideSlack * s);
        winW = barW + 2.0f * slack;
    }
    if (l.fullWidth) {
        // The whole edge; the items are placed in it below.
        barW = static_cast<float>(mEnd - mStart);
        slack = 0.0f;
        winW = barW;
    }
    const LONG barPx = static_cast<LONG>(barW), slackPx = static_cast<LONG>(slack), marginPx = static_cast<LONG>(margin);
    LONG barStart = 0;
    switch (l.fullWidth ? DockAlign::Start : d.align) {
        case DockAlign::Start: barStart = mStart + (l.fullWidth ? 0 : marginPx + offsetPx); break;
        case DockAlign::End: barStart = mEnd - marginPx - barPx + offsetPx; break;
        case DockAlign::Center:
        default: barStart = mStart + ((mEnd - mStart) - static_cast<LONG>(winW)) / 2 + slackPx + offsetPx; break;
    }
    // The "screen edge" the stage stands on: the monitor's edge, or a floating bar's outer side.
    LONG edgeLine = l.edge == DockEdge::Top ? m.top : l.edge == DockEdge::Left ? m.left : l.edge == DockEdge::Right ? m.right : m.bottom;
    if (l.floating) {
        // x / y: the resting bar's top-left in DIPs from the work area's top-left (kCentred: centred
        // on the work area), the bar kept on the monitor. The stage reaches from the bar's outer side
        // (the side away from the way it faces) toward the way it faces.
        const RECT& w = l.work;
        const LONG thickPx = static_cast<LONG>(std::lround(barH));
        const LONG bw = l.vertical ? thickPx : barPx, bh = l.vertical ? barPx : thickPx;
        LONG left = d.x == DockConfig::kCentred ? w.left + ((w.right - w.left) - bw) / 2
                                                : w.left + static_cast<LONG>(std::lround(static_cast<double>(d.x) * s));
        LONG top = d.y == DockConfig::kCentred ? w.top + ((w.bottom - w.top) - bh) / 2
                                               : w.top + static_cast<LONG>(std::lround(static_cast<double>(d.y) * s));
        left = std::clamp(left, m.left, std::max(m.left, m.right - bw));
        top = std::clamp(top, m.top, std::max(m.top, m.bottom - bh));
        barStart = l.vertical ? top : left;
        switch (l.edge) {
            case DockEdge::Top: edgeLine = top; break;
            case DockEdge::Left: edgeLine = left; break;
            case DockEdge::Right: edgeLine = left + bw; break;
            case DockEdge::Bottom:
            default: edgeLine = top + bh; break;
        }
    }
    barStart = std::clamp(barStart, mStart, std::max(mStart, mEnd - barPx));
    const LONG stageStart = std::max(barStart - slackPx, mStart);
    const LONG stageEnd = std::max(stageStart + 1, std::min(barStart + barPx + slackPx, mEnd));
    const float barLeft = static_cast<float>(barStart - stageStart);
    l.slack = slack;
    l.stageMain = static_cast<float>(stageEnd - stageStart);
    l.stageCross = winH;
    const LONG crossPx = static_cast<LONG>(winH);
    switch (l.edge) {
        case DockEdge::Top: l.window = RECT{stageStart, edgeLine, stageEnd, edgeLine + crossPx}; break;
        case DockEdge::Left: l.window = RECT{edgeLine, stageStart, edgeLine + crossPx, stageEnd}; break;
        case DockEdge::Right: l.window = RECT{edgeLine - crossPx, stageStart, edgeLine, stageEnd}; break;
        case DockEdge::Bottom:
        default: l.window = RECT{stageStart, edgeLine - crossPx, stageEnd, edgeLine}; break;
    }

    l.bar = D2D1::RectF(barLeft, winH - crossMargin - barH, barLeft + barW, winH - crossMargin);  // 12E: off its edge by the gap
    l.itemTop = l.bar.top + l.iconTopPx;
    l.captionTop = l.itemTop + l.iconPx + l.reflH + captionGap;  // under the icon and its reflection
    l.growRoomStart = l.bar.left;
    l.growRoomEnd = l.stageMain - l.bar.right;
    float polarRun = 0.0f;  // a ring: the run's length along it (0: not a ring)
    if (polar) {
        // The ring (DockGeometry::Polar): the canonical bar becomes the arc [0, arcLen] at the
        // radius of the icons' centre line; v keeps the bar's frame (the band [bar.top, bar.bottom],
        // `room` beyond it for magnified icons and the label). Placed as a bar 2 x outer radius
        // long would be (an arc), or at docks[].x / y on the work area (a circle).
        const bool circle = d.shape == DockShape::Circle;
        constexpr float kTwoPi = 6.28318531f;
        const float span = circle ? kTwoPi : kTwoPi / 2.0f;
        const float run = lead + itemsW + trail;
        polarRun = run;
        const float room = std::max(0.0f, winH - margin - barH);  // beyond the ring
        const float band = barH;
        const float iconMid = l.iconTopPx + l.iconPx / 2.0f;  // bar top -> the icons' centre line
        const float outFromMid = iconMid;                                 // the centre line -> the outer edge
        const float inFromMid = band - iconMid;                           // ... -> the inner edge
        const float minR = std::max(inFromMid + std::round(kMinPolarHoleDip * s), 1.5f * l.iconPx);
        const float mainLen = static_cast<float>(mEnd - mStart);
        const float crossLen = static_cast<float>(IsVertical(l.edge) ? m.right - m.left : m.bottom - m.top);
        const float workW = static_cast<float>(l.work.right - l.work.left), workH = static_cast<float>(l.work.bottom - l.work.top);
        float maxR = circle ? std::min(workW, workH) / 2.0f - outFromMid - room
                            : std::min(mainLen / 2.0f - outFromMid - room, crossLen - outFromMid - room);
        maxR = std::max(maxR, minR);
        // Auto: the run just fits (a circle: with a gap of a slot and a quarter where its ends meet at
        // the top, which a magnified run's push uses up instead of overlapping there).
        const float gapRun = circle ? std::ceil(1.25f * (l.iconPx + l.gap)) : 0.0f;
        float R = d.radius > 0 ? std::round(static_cast<float>(d.radius) * s) : std::ceil((run + gapRun) / span + 0.5f);
        R = std::clamp(R, std::ceil(minR), std::max(std::ceil(minR), std::floor(maxR)));
        const float arcLen = span * R;
        const float rOut = R + outFromMid, rIn = R - inFromMid;
        // The stage along the edge (an arc): its diameter plus `room` at each end, placed by align /
        // offset in the bar's own arithmetic (EndDockDrag repeats it).
        const LONG diaPx = static_cast<LONG>(std::ceil(2.0f * rOut));
        const LONG ringRoomPx = static_cast<LONG>(std::ceil(room));
        const LONG extent = static_cast<LONG>(std::ceil(rOut + room));  // centre -> the stage's far side
        float cxs = 0.0f, cys = 0.0f;  // the centre, screen px
        if (circle) {
            const RECT& w = l.work;
            float cxw = d.x == DockConfig::kCentred ? (w.left + w.right) / 2.0f : w.left + static_cast<float>(d.x) * s;
            float cyw = d.y == DockConfig::kCentred ? (w.top + w.bottom) / 2.0f : w.top + static_cast<float>(d.y) * s;
            cxs = std::round(std::clamp(cxw, m.left + rOut, std::max(m.left + rOut, m.right - rOut)));
            cys = std::round(std::clamp(cyw, m.top + rOut, std::max(m.top + rOut, m.bottom - rOut)));
            l.window = RECT{std::max(m.left, static_cast<LONG>(cxs) - extent), std::max(m.top, static_cast<LONG>(cys) - extent),
                            std::min(m.right, static_cast<LONG>(cxs) + extent), std::min(m.bottom, static_cast<LONG>(cys) + extent)};
        } else {
            LONG start = 0;
            switch (d.align) {
                case DockAlign::Start: start = mStart + marginPx + offsetPx; break;
                case DockAlign::End: start = mEnd - marginPx - diaPx + offsetPx; break;
                case DockAlign::Center:
                default: start = mStart + ((mEnd - mStart) - (diaPx + 2 * ringRoomPx)) / 2 + ringRoomPx + offsetPx; break;
            }
            start = std::clamp(start, mStart, std::max(mStart, mEnd - diaPx));
            const LONG s0 = std::max(start - ringRoomPx, mStart), s1 = std::max(s0 + 1, std::min(start + diaPx + ringRoomPx, mEnd));
            const float along = static_cast<float>(start) + static_cast<float>(diaPx) / 2.0f;
            switch (l.edge) {
                case DockEdge::Top:
                    cxs = along, cys = static_cast<float>(m.top);
                    l.window = RECT{s0, m.top, s1, m.top + extent};
                    break;
                case DockEdge::Left:
                    cxs = static_cast<float>(m.left), cys = along;
                    l.window = RECT{m.left, s0, m.left + extent, s1};
                    break;
                case DockEdge::Right:
                    cxs = static_cast<float>(m.right), cys = along;
                    l.window = RECT{m.right - extent, s0, m.right, s1};
                    break;
                case DockEdge::Bottom:
                default:
                    cxs = along, cys = static_cast<float>(m.bottom);
                    l.window = RECT{s0, m.bottom - extent, s1, m.bottom};
                    break;
            }
        }
        l.slack = static_cast<float>(ringRoomPx);
        // The canonical frame: the arc [0, arcLen] as the bar, the band as before.
        l.bar = D2D1::RectF(0.0f, winH - margin - barH, arcLen, winH - margin);
        l.itemTop = l.bar.top + l.iconTopPx;
        l.captionTop = l.itemTop + l.iconPx;
        l.stageMain = arcLen;
        l.stageCross = winH;
        l.growMax = 0.0f;
        l.growRoomStart = l.growRoomEnd = 1e6f;  // a magnified run moves along the ring (it never grows)
        DockGeometry::Polar& p = l.polar;
        p.on = true;
        DockGeometry::PolarDirection(circle, l.edge, p);
        p.R = R;
        p.vRing = l.bar.top + iconMid;
        p.cx = cxs - static_cast<float>(l.window.left);
        p.cy = cys - static_cast<float>(l.window.top);
        l.ringIn = rIn;
        l.ringOut = rOut;
        l.ring = p.Bounds(0.0f, l.bar.top, arcLen, l.bar.bottom);
        // (Kept on the stage: the ends of an arc lie on the screen edge, a rounding error past it.)
        const float stageW = static_cast<float>(l.window.right - l.window.left), stageH = static_cast<float>(l.window.bottom - l.window.top);
        l.ring = D2D1::RectF(std::max(0.0f, l.ring.left), std::max(0.0f, l.ring.top), std::min(stageW, l.ring.right),
                             std::min(stageH, l.ring.bottom));
        if (circle) {
            const float hubD = std::round(std::max(kHubDip * s, l.iconPx));
            l.hub = D2D1::RectF(p.cx - hubD / 2.0f, p.cy - hubD / 2.0f, p.cx + hubD / 2.0f, p.cy + hubD / 2.0f);
            l.hubScale = hubD / (2.0f * rOut);
        }
        // More than the ring holds: "scroll" (and "shrink": a ring does not shrink its icons)
        // scrolls along it; "clip" lets the run go on past the ring's end.
        l.view.on = d.overflow != OverflowMode::Clip && itemsW > 0.0f && run > arcLen + 0.5f;
    }

    // Hairlines in the icon -> widget spacers at the ends: before the trailing widgets, after the
    // leading ones (bar-local; both exist only when the dock also has icons).
    int firstIcon = -1, lastIcon = -1;
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].IsWidget()) continue;
        if (firstIcon < 0) firstIcon = static_cast<int>(i);
        lastIcon = static_cast<int>(i);
    }
    l.separatorX = l.leftSeparatorX = -1.0f;
    l.itemsLeft = l.bar.left + lead;
    if (polar && !l.view.on) l.itemsLeft = std::round((l.bar.right - polarRun) / 2.0f) + lead;  // centred on the ring
    if (l.anchored) {
        // The groups' leads (bar-relative, so they hold wherever the bar was placed).
        baseLeads.resize(items_.size());
        itemsW = 0.0f;
        for (size_t i = 0; i < items_.size(); ++i) {
            DockItem& item = items_[i];
            baseLeads[i] = item.lead;
            item.lead = anchorLeads[i];
            if (!item.IsWidget() || item.slotW > 0.0f) itemsW += item.lead + item.slotW;
        }
        l.itemsW = itemsW;
        // A full-width bar: a magnified run grows on both sides of the pointer (the window cuts it at
        // the monitor's ends).
        if (l.fullWidth) l.growRoomStart = l.growRoomEnd = 1e6f;
    } else if (l.fullWidth) {
        // The run of items inside the full bar: at its start, centred or at its end, plus the
        // offset, kept inside the bar's padding. Magnification grows the run inside the bar (the
        // bar's own ends move past the monitor's, where the window cuts them off).
        const float lo = l.bar.left + lead, hi = std::max(lo, l.bar.right - trail - itemsW);
        float at = lo;
        if (d.align == DockAlign::End) at = hi;
        if (d.align == DockAlign::Center) at = l.bar.left + std::round((barW - (lead + itemsW + trail)) / 2.0f) + lead;
        l.itemsLeft = std::clamp(std::round(at + static_cast<float>(offsetPx)), lo, hi);
        l.growRoomStart = l.itemsLeft - lo;
        l.growRoomEnd = hi - l.itemsLeft;
    }
    if (l.view.on) {
        // The viewport: the run's room inside the bar, the run shown from scroll_ on (kept across
        // relayouts, clamped to the new length). Every slot position below is a view position.
        DockGeometry::Viewport& v = l.view;
        v.start = l.bar.left + lead;
        v.end = l.bar.right - trail;
        v.clipStart = l.bar.left;
        v.clipEnd = l.bar.right;
        if (polar) {
            // A ring is not clipped (a rectangle cannot cut a sector, ApplyReflectionClip): the fades
            // take the slots out, so "outside" means beyond the fades' ends.
            v.clipStart = v.start;
            v.clipEnd = v.end;
        }
        v.content = itemsW;
        v.max = std::max(0.0f, std::ceil(itemsW - v.Length()));
        scroll_ = std::clamp(std::round(scroll_), 0.0f, v.max);
        if (!scrollShown_.valid || scrollShown_.to != scroll_) scrollShown_ = Tween{scroll_, scroll_, Renderer::Now(), 0.0, true};
        v.scroll = scroll_;
        v.fade = std::round(std::min(stride * 1.25f, v.Length() / 4.0f));
        v.fadeStart = scroll_ > 0.5f;
        v.fadeEnd = scroll_ < v.max - 0.5f;
        l.itemsLeft = v.start - scroll_;
        // The magnified run grows past the viewport's ends on both sides (the slot under the
        // pointer stays under it); the clip cuts what goes beyond the bar.
        l.growRoomStart = l.growRoomEnd = 1e6f;
    }
    // Round 9 phase 9A (docks[].magnifyBar "steady"): the magnified bar's length is fixed for the
    // whole hover: rest + the magnification's growth, half at each end, except where the stage's
    // room cuts an end (a monitor end, a start / end aligned bar): that part goes to the other end.
    // Not for a ring, a scrolling run, a full-width or an anchored bar (their growth is as before).
    // 9A follow-up: the bar keeps that length from the first time something zooms under the pointer
    // (follow-up 2) until it leaves (grown_), not only while magnifying; "always" has the same room
    // at rest (above).
    l.steady = d.magnifyBar == MagnifyBar::Steady && !polar && !l.view.on && !l.fullWidth && !l.anchored && magGrow > 0.0f &&
               l.firstMag >= 0;
    l.steadyStart = l.steadyEnd = 0.0f;
    if (l.always) {
        l.steady = true;
        l.steadyStart = alwaysStart;
        l.steadyEnd = alwaysEnd;
    } else if (l.steady) {
        float start = std::floor(magGrow / 2.0f);
        float end = magGrow - start;
        const float roomStart = std::max(0.0f, l.growRoomStart), roomEnd = std::max(0.0f, l.growRoomEnd);
        if (start > roomStart) {
            end += start - roomStart;
            start = roomStart;
        }
        if (end > roomEnd) {
            start = std::min(roomStart, start + (end - roomEnd));
            end = roomEnd;
        }
        l.steadyStart = start;
        l.steadyEnd = end;
    }
    float slotX = l.itemsLeft;
    bool trailingSpacerSeen = false;
    for (size_t i = 0; i < items_.size(); ++i) {
        DockItem& item = items_[i];
        const float spacerLeft = slotX - l.bar.left;  // bar-local
        if (static_cast<int>(i) == l.plusPinSlot) l.plusPinMain = slotX + half;  // round 12 phase 12D: the "+" box in its room
        slotX += item.lead;
        item.slotX = slotX;
        slotX += item.slotW;
        item.x = item.slotX + BoxInset(item);
        // Icons stand on the icon row; widgets are centred on it (above any reflections). A widget
        // box thicker than the bar (round 7 phase 7C: the analog clock grown on hover) stands on the
        // bar's near edge and sticks out on the far side only, as a magnified icon does.
        // Round 12 phase 12C: centred on the icon row, under an "inside" title's band.
        const float rowTop = l.bar.top + l.titleBand, rowH = iconBarH - l.titleBand;
        item.y = item.IsWidget() ? std::round(std::min(rowTop + (rowH - item.boxCross) / 2.0f, rowTop + rowH - item.boxCross))
                                 : l.itemTop;
        const int k = static_cast<int>(i);
        // "always": the room before the first magnifying slot is the icon side of its spacer, the
        // room after the last one the icon side of the next spacer; a hairline stays in the spacer
        // proper, beside the widgets.
        const float padBefore = (k == l.padEndSlot ? alwaysEnd : 0.0f) + (k == l.plusPinSlot ? l.plusRoom : 0.0f);  // 12D: "+" room too
        const float spacer = item.lead - padBefore - (k == l.padStartSlot ? alwaysStart : 0.0f);
        if (spacer <= 0.0f || firstIcon < 0) continue;
        // Anchored groups: no hairline in a gap between groups (only in a spacer of the usual size).
        const bool gap = l.anchored && item.lead > baseLeads[i] + 0.5f;
        if (k > lastIcon && !trailingSpacerSeen) {
            trailingSpacerSeen = true;
            if (!gap) l.separatorX = std::round(spacerLeft + padBefore + spacer / 2.0f);
        }
        if (k == firstIcon && !gap) l.leftSeparatorX = std::round(spacerLeft + spacer / 2.0f);
    }
    if (l.plusEnd) l.plusEndMain = slotX + half;  // round 12 phase 12D: the end "+" right after the run
    // Scrolling: the spacers move with the run, so the background (fixed) draws no hairlines (nor
    // does a ring: its edges are its hairlines).
    if (l.view.on || polar) l.separatorX = l.leftSeparatorX = -1.0f;
    l.itemHeight = l.bar.bottom - l.itemTop;
    l.magTop = l.itemTop + l.iconPx - l.iconPx * l.magnification;
    // Popups open beyond a fully magnified icon (round 7 phase 7A: on every edge; before, above a
    // horizontal dock they opened beyond the whole stage), docks[].popupGap further (PopupAnchorAt).
    l.popupY = std::max(0.0f, std::floor(l.magTop));
    l.dotRadius = std::max(1.5f, kDotRadius * s);
    l.dotHeight = static_cast<UINT>(std::ceil(2.0f * l.dotRadius + 2.0f));
    // With reflections the dot sits at the icon's foot, on the reflection.
    const float dotCenter = l.reflH > 0.0f ? l.itemTop + l.iconPx + dotFromEdge : l.bar.bottom - dotFromEdge;
    l.dotTop = std::round(dotCenter - static_cast<float>(l.dotHeight) / 2.0f);
    l.dotCenterY = dotCenter - l.dotTop;
    // A glow that shows at rest (always / the active app) reaches above the bar: the resting HWND
    // grows by that much so it is not cut off (only that band; the bar stays the active zone).
    l.restTopExtra = (glow == GlowMode::Always || glow == GlowMode::Active)
                         ? std::max(0.0f, l.glowPad - l.iconTopPx)
                         : 0.0f;
    // Round 11 phase 11A: the slot buttons' band across the bar.
    if (l.buttons) {
        l.buttonInsetMain = std::max(1.0f, std::round(s));
        const float crossInset = std::round(std::max(1.0f, barPadDip / 2.0f) * s);
        l.buttonTop = l.bar.top + l.titleBand + crossInset;  // round 12 phase 12C: under an "inside" title
        l.buttonH = std::max(0.0f, (l.bar.bottom - l.bar.top - l.titleBand) - 2.0f * crossInset);
    }
    // The title is part of the resting window too (it takes no part in hit testing), centred on the
    // run of items along the bar.
    if (titleCross > 0.0f && l.titleInside) {
        // Round 12 phase 12C: in its band at the bar's far side, centred on the run like outside; the
        // bar (its hit testing and zone) already holds it, so the resting window needs nothing more.
        const float runMiddle = l.view.on ? (l.view.start + l.view.end) / 2.0f : l.itemsLeft + itemsW / 2.0f;
        const float u0 = std::clamp(std::round(runMiddle - titleMain / 2.0f), 0.0f, std::max(0.0f, l.stageMain - titleMain));
        l.title = l.PhysRect(u0, l.bar.top, u0 + titleMain, l.bar.top + titleCross);
    } else if (titleCross > 0.0f) {
        const float runMiddle = l.view.on ? (l.view.start + l.view.end) / 2.0f : l.itemsLeft + itemsW / 2.0f;
        const float u0 = std::round(runMiddle - titleMain / 2.0f);
        const float v0 = l.bar.top - titleGap - titleCross;
        l.title = l.PhysRect(std::clamp(u0, 0.0f, std::max(0.0f, l.stageMain - titleMain)), v0,
                             std::clamp(u0, 0.0f, std::max(0.0f, l.stageMain - titleMain)) + titleMain, v0 + titleCross);
        l.restTopExtra = std::max(l.restTopExtra, titleGap + titleCross);
        if (!polar) l.popupY = std::min(l.popupY, std::max(0.0f, std::floor(v0)));  // popups beyond the title too
        if (polar) {
            // In the middle of a circle; beyond the middle of an arc (outside it, like a label).
            const DockGeometry::Polar& p = l.polar;
            D2D1_POINT_2F c{p.cx, p.cy};
            if (l.shape == DockShape::Arc) c = p.Outside(l.bar.right / 2.0f, l.bar.top - titleGap, titleMain, titleCross);
            l.title = D2D1::RectF(std::round(c.x - titleMain / 2.0f), std::round(c.y - titleCross / 2.0f),
                                  std::round(c.x - titleMain / 2.0f) + titleMain, std::round(c.y - titleCross / 2.0f) + titleCross);
            l.restTopExtra = 0.0f;
        }
    }
    if (polar) l.restTopExtra = 0.0f;  // the resting window is the ring's box

    // Background halves: the seam must sit where both halves show plain fill (between the
    // rounded left end / left hairline and the hairline / rounded right end) for every amount of
    // growth.
    l.bgSurfaceW = barW + l.growMax;
    float radius = std::ceil(std::min(l.cornerRadius, barH / 2.0f));
    if (DockTheme().shelf) radius = std::max(radius, std::ceil(DockTheme().shelfSlant * s));  // the angled ends
    // A spring settle overshoots: returning from growth g, the bar briefly gets narrower than at
    // rest by up to kSpringOvershoot * g per side. Keep the seam that much further in.
    const float undershoot =
        motion_.easing == Easing::Spring ? std::ceil(static_cast<float>(Tween::kSpringOvershoot) * l.growMax) : 0.0f;
    const float stroke = std::max(1.0f, std::round(s));  // the hairlines' width (RenderBackground)
    const float seamMin = std::max(radius, l.leftSeparatorX >= 0.0f ? l.leftSeparatorX + stroke : 0.0f) + 2.0f + undershoot;
    const float seamMax = (l.separatorX >= 0.0f ? l.separatorX - 2.0f : barW - radius - 2.0f) - undershoot;
    l.seamX = l.bar.left + (seamMin <= seamMax ? seamMin : std::floor(barW / 2.0f));

    // Moves the bar (and a glow shown above it at rest) just past the window's screen edge.
    l.slide = winH - (l.bar.top - l.restTopExtra) + 2.0f;
    // The peek strip (docks[].peekPx with auto-hide): hidden, the bar's far side stays this far on
    // screen (at peekOpacity) instead of the whole dock sliding off.
    // (Only used while hidden, so it does not depend on the auto-hide mode.)
    l.peek = d.peekPx > 0 && !polar ? std::min(std::round(static_cast<float>(d.peekPx) * s), barH) : 0.0f;
    l.hiddenSlide = l.peek > 0.0f ? l.stageCross - l.peek - l.bar.top : l.slide;
    // An arc slides into its edge: its outer radius and a little; a circle shrinks to its hub.
    if (polar) l.slide = l.hiddenSlide = std::ceil(l.ringOut) + 2.0f;

    const DockGeometry::Viewport& ov = layout_.view;
    if (ov.on != l.view.on || ov.start != l.view.start || ov.end != l.view.end || ov.fade != l.view.fade ||
        ov.fadeStart != l.view.fadeStart || ov.fadeEnd != l.view.fadeEnd) {
        ++fadeSerial_;
    }
    layout_ = l;
}

RECT DockWindow::StageToScreen(float u0, float v0, float u1, float v1, bool outer) const {
    const Layout& l = layout_;
    const D2D1_RECT_F r = l.PhysRect(u0, v0, u1, v1);
    auto lo = [outer](float v) { return static_cast<LONG>(outer ? std::floor(v) : v); };
    auto hi = [outer](float v) { return static_cast<LONG>(outer ? std::ceil(v) : v); };
    return RECT{l.window.left + lo(r.left), l.window.top + lo(r.top), l.window.left + hi(r.right),
                l.window.top + hi(r.bottom)};
}

RECT DockWindow::HiddenRect() const {
    // A hidden circle is its hub (the ring shrunk about its centre); hovering the hub reveals it.
    if (layout_.shape == DockShape::Circle && layout_.polar.on) {
        const D2D1_RECT_F& h = layout_.hub;
        return RECT{layout_.window.left + static_cast<LONG>(std::floor(h.left)), layout_.window.top + static_cast<LONG>(std::floor(h.top)),
                    layout_.window.left + static_cast<LONG>(std::ceil(h.right)), layout_.window.top + static_cast<LONG>(std::ceil(h.bottom))};
    }
    // The peek strip: the part of the bar left on screen (drawn at peekOpacity); hovering it reveals.
    if (layout_.peek > 0.0f) {
        const Layout& l = layout_;
        return StageToScreen(l.bar.left, l.stageCross - l.peek, l.bar.right, l.stageCross, true);
    }
    // A floating bar never auto-hides (Config::Sanitize); were it hidden, its strip is its outer side.
    if (layout_.floating) {
        const Layout& l = layout_;
        return StageToScreen(l.bar.left, l.stageCross - 1.0f, l.bar.right, l.stageCross, true);
    }
    // The reveal trigger: a 1 px strip along the monitor's edge. Nothing is drawn in it (the root
    // visual is transparent while hidden and there is no redirection bitmap).
    const RECT& m = layout_.monitor;
    switch (layout_.edge) {
        case DockEdge::Top: return RECT{m.left, m.top, m.right, m.top + 1};
        case DockEdge::Left: return RECT{m.left, m.top, m.left + 1, m.bottom};
        case DockEdge::Right: return RECT{m.right - 1, m.top, m.right, m.bottom};
        case DockEdge::Bottom:
        default: return RECT{m.left, m.bottom - 1, m.right, m.bottom};
    }
}

RECT DockWindow::DesiredRect() const {
    const Layout& l = layout_;
    if (hideState_ == HideState::Hidden) return HiddenRect();
    if (expanded_) return l.window;
    if (l.polar.on) {
        // A ring at rest (and an arc sliding into its edge) is its band's box; the window region
        // (UpdateWindowRegion) leaves the hole inside it to the windows below.
        const D2D1_RECT_F& r = l.ring;
        return RECT{l.window.left + static_cast<LONG>(std::floor(r.left)), l.window.top + static_cast<LONG>(std::floor(r.top)),
                    l.window.left + static_cast<LONG>(std::ceil(r.right)), l.window.top + static_cast<LONG>(std::ceil(r.bottom))};
    }
    // Only the bar is interactive at rest: the transparent room above and beside it must not
    // swallow clicks meant for the windows below. While sliding, the bar travels through the edge
    // margin, so the rect extends to the screen edge (the stage's edge side).
    float top = l.bar.top - l.restTopExtra;
    if (hideState_ != HideState::Shown && motion_.easing == Easing::Spring &&
        (motion_.hide == HideStyle::Slide || motion_.hide == HideStyle::SlideOnly)) {
        // A spring slide-in overshoots above the resting bar: room for it.
        top -= std::ceil(static_cast<float>(Tween::kSpringOvershoot) * l.slide) + 1.0f;
        top = std::max(top, 0.0f);
    }
    const float bottom = hideState_ == HideState::Shown ? l.bar.bottom : l.stageCross;
    return StageToScreen(l.bar.left, top, l.bar.right, bottom);
}

HRESULT DockWindow::DrawCanonical(Layer& layer, float main, float cross,
                                  const std::function<void(ID2D1DeviceContext*)>& draw) {
    const D2D1_SIZE_F size = layout_.PhysSize(main, cross);
    const DockEdge edge = layout_.DrawEdge();  // a ring's surfaces are upright (as for a bottom dock)
    return renderer_.DrawLayer(layer, static_cast<UINT>(size.width), static_cast<UINT>(size.height),
                               [&](ID2D1DeviceContext* dc) {
                                   D2D1_MATRIX_3X2_F old;
                                   dc->GetTransform(&old);
                                   dc->SetTransform(DockGeometry::Matrix(edge, cross) * old);
                                   draw(dc);
                                   dc->SetTransform(old);
                               });
}

PopupAnchor DockWindow::PopupAnchorAt(float anchor, int labelSlot) const {
    // Centred on the slot along the edge, docks[].popupGap beyond l.popupY (a fully magnified
    // icon's far side; round 7 phase 7A), away from the edge. With the hover label shown for slot
    // `labelSlot` (ShowPreview keeps it), beyond the label: they never overlap.
    const Layout& l = layout_;
    const int gap = PopupGapPx();
    const bool label = labelSlot >= 0 && labelSlot == hovered_ && labelShown_ && label_.width > 0 && label_.height > 0 &&
                       !(hoverCell_ >= 0 && hoverCellSlot_ == labelSlot);
    const float labelGap = std::round(kLabelGap * l.scale);
    if (l.polar.on) {
        // Outward from the ring at the slot's angle, beyond a magnified icon: the side the outward
        // direction mostly points to (up: the popup opens above, as over a bottom dock; right:
        // beside it, as for a left dock ...), centred on the slot there; PopupWindow keeps it on the
        // work area.
        const D2D1_POINT_2F o = l.polar.Outward(anchor);
        const D2D1_POINT_2F p = l.polar.Point(anchor, l.magTop);
        PopupAnchor a;
        const bool across = std::fabs(o.x) > std::fabs(o.y);
        a.edge = across ? (o.x > 0.0f ? DockEdge::Left : DockEdge::Right) : (o.y < 0.0f ? DockEdge::Bottom : DockEdge::Top);
        const int px = l.window.left + static_cast<int>(std::lround(p.x)), py = l.window.top + static_cast<int>(std::lround(p.y));
        a.along = across ? py : px;
        int nearSide = across ? px : py;
        if (label) {
            // The label's box (placed as ComputeTargets does, at full magnification: the farthest
            // out it gets): the popup's near side beyond it too.
            const float lw = static_cast<float>(label_.width), lh = static_cast<float>(label_.height);
            const D2D1_POINT_2F c = l.polar.Outside(anchor, l.magTop - labelGap, lw, lh);
            const int lLeft = l.window.left + static_cast<int>(std::floor(c.x - lw / 2.0f));
            const int lRight = l.window.left + static_cast<int>(std::ceil(c.x + lw / 2.0f));
            const int lTop = l.window.top + static_cast<int>(std::floor(c.y - lh / 2.0f));
            const int lBottom = l.window.top + static_cast<int>(std::ceil(c.y + lh / 2.0f));
            switch (a.edge) {
                case DockEdge::Top: nearSide = std::max(nearSide, lBottom); break;    // the popup below
                case DockEdge::Left: nearSide = std::max(nearSide, lRight); break;    // ... to the right
                case DockEdge::Right: nearSide = std::min(nearSide, lLeft); break;    // ... to the left
                case DockEdge::Bottom:
                default: nearSide = std::min(nearSide, lTop); break;                  // ... above
            }
        }
        a.side = nearSide + static_cast<int>(DockGeometry::OutwardSign(a.edge)) * -gap;
        // Diagonal: the popup's near corner at the slot, the popup wholly on the outward side.
        const float main = across ? o.y : o.x, cross = std::max(0.05f, std::fabs(across ? o.x : o.y));
        a.alongBias = std::clamp(main / cross, -1.0f, 1.0f);
        return a;
    }
    float v = l.popupY;
    if (label) {
        // ComputeTargets' label place at full magnification (upright: on a vertical dock its cross
        // length is its width).
        const float lh = static_cast<float>(l.vertical ? label_.width : label_.height);
        v = std::min(v, std::round(std::max(0.0f, l.magTop - labelGap - lh)));
    }
    const D2D1_POINT_2F p = l.Phys(anchor, v);
    PopupAnchor a;
    a.edge = l.edge;
    a.along = (l.vertical ? l.window.top : l.window.left) + static_cast<int>(std::lround(l.vertical ? p.y : p.x));
    const int side = (l.vertical ? l.window.left : l.window.top) + static_cast<int>(std::lround(l.vertical ? p.x : p.y));
    a.side = side + static_cast<int>(DockGeometry::OutwardSign(l.edge)) * -gap;
    return a;
}

int DockWindow::PopupGapPx() const {
    return static_cast<int>(std::lround(static_cast<float>(DockCfg().popupGap) * layout_.scale));
}

int DockWindow::MaxLengthPx() const {
    const DockConfig& d = DockCfg();
    if (d.maxLength <= 0 || d.Polar() || layout_.fullWidth) return 0;  // ignored for rings / full width
    return static_cast<int>(std::lround(static_cast<float>(d.maxLength) * layout_.scale));
}

void DockWindow::ApplyStageTransform() {
    // Stage point p is client point p - (winRect_ origin - stage origin). A static transform on
    // the root; the auto-hide slide (root OffsetY) adds on top of it.
    IDCompositionVisual2* root = renderer_.Root();
    if (!root) return;
    const float dx = static_cast<float>(layout_.window.left - winRect_.left);
    const float dy = static_cast<float>(layout_.window.top - winRect_.top);
    IDCompositionDesktopDevice* dcomp = renderer_.DComp();
    if (layout_.shape == DockShape::Circle && layout_.polar.on && dcomp) {
        // A circle: scale about its centre (its hub when hidden, ApplyRootPose), then the stage
        // translation.
        if (!rootScale_ || !rootTranslate_) {
            winrt::com_ptr<IDCompositionScaleTransform> scale;
            winrt::com_ptr<IDCompositionTranslateTransform> translate;
            winrt::com_ptr<IDCompositionTransform> group;
            if (SUCCEEDED(dcomp->CreateScaleTransform(scale.put())) && SUCCEEDED(dcomp->CreateTranslateTransform(translate.put()))) {
                IDCompositionTransform* parts[] = {scale.get(), translate.get()};
                if (SUCCEEDED(dcomp->CreateTransformGroup(parts, 2, group.put())) && SUCCEEDED(root->SetTransform(group.get()))) {
                    rootScale_ = std::move(scale);
                    rootTranslate_ = std::move(translate);
                    rootScaleT_ = Tween{1.0f, 1.0f, Renderer::Now(), 0.0, true};
                    rootY_.valid = false;  // the pose is applied again (to the new transform)
                }
            }
        }
        if (rootScale_ && rootTranslate_) {
            rootScale_->SetCenterX(layout_.polar.cx);
            rootScale_->SetCenterY(layout_.polar.cy);
            rootTranslate_->SetOffsetX(dx);
            rootTranslate_->SetOffsetY(dy);
            return;
        }
    }
    if (rootScale_) {
        rootScale_ = nullptr;
        rootTranslate_ = nullptr;
        rootScaleT_.valid = false;
    }
    root->SetTransform(D2D1::Matrix3x2F::Translation(dx, dy));
}

void DockWindow::ApplyWindowPos() {
    const RECT r = DesiredRect();
    winRect_ = r;
    ApplyStageTransform();
    // While a modal dialog owned by the dock is open (an icon picker; it disables the dock), the
    // z-order stays: HWND_TOPMOST with SWP_NOOWNERZORDER would put the dock above its own dialog.
    // The same while a widget's text editor (owned by the dock, round 6 phase 6C) is open. While a
    // fullscreen app runs (an app bar's ABN_FULLSCREENAPP) the dock stays where it was put, below it;
    // while the full-screen guard hides it (round 9 phase 9A) it is only moved / sized.
    const UINT keepOrder =
        IsWindowEnabled(hwnd_) && !fullscreenApp_ && !fullscreenHidden_ && editorWidget_.empty() ? 0 : SWP_NOZORDER;
    if (DesktopLayered() || BehindLayered()) {
        // The desktop layer: directly above the desktop window, never topmost (a "behind" dock is
        // registered with the layer, which keeps it there; a "desktop" dock is only shown while the
        // desktop is the foreground).
        SetWindowPos(hwnd_, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER);
        if (!keepOrder) {
            if (layerRegistered_) {
                DesktopLayer::Place(hwnd_);
            } else {
                DesktopLayer::PlaceAboveDesktop(hwnd_);
            }
        }
        UpdateWindowRegion();
        return;
    }
    SetWindowPos(hwnd_, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | keepOrder);
    UpdateWindowRegion();
}

void DockWindow::UpdateWindowRect() {
    if (!hwnd_) return;
    const RECT r = DesiredRect();
    if (EqualRect(&r, &winRect_)) return;
    winRect_ = r;
    // Commit the new root translation, then move the window right away: both reach DWM before
    // its next frame, so the content stays put on screen while the HWND grows or shrinks.
    // There is no redirection bitmap, so nothing needs repainting.
    RenderWithRecovery([this] {
        ApplyStageTransform();
        return S_OK;
    });
    SetWindowPos(hwnd_, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOREDRAW);
    UpdateWindowRegion();
    // A resize under the cursor may end the leave tracking without a WM_MOUSELEAVE: re-arm it.
    if (trackingMouse_) {
        TRACKMOUSEEVENT cancel{sizeof(cancel), TME_LEAVE | TME_CANCEL, hwnd_, 0};
        TrackMouseEvent(&cancel);
        trackingMouse_ = false;
    }
    POINT pt{};
    if (GetCursorPos(&pt) && PtInRect(&r, pt)) ArmMouseTracking();
}

bool DockWindow::NeedsStage() const {
    if (magnifying_ || grown_ || hovered_ >= 0 || labelShown_ || labelPending_ || labelFading_ || restPending_ || hold_) {
        return true;
    }
    if (dragPhase_ != DragPhase::None || dropActive_) return true;
    // Round 12 phase 12D: customize mode draws beside the bar (the outline, the anchor targets).
    if (editing_) return true;
    // A bounce / hop leaves the bar; a pulse stays inside it.
    return std::any_of(items_.begin(), items_.end(), [](const DockItem& item) {
        return item.bounce != DockItem::Bounce::None && item.bounceStyle != LaunchStyle::Pulse;
    });
}

void DockWindow::TryCompact() {
    if (!expanded_ || NeedsStage()) return;
    expanded_ = false;
    UpdateWindowRect();
}

void DockWindow::Relayout() {
    if (!hwnd_ || inLayout_) return;
    LayoutPass();
    RenderAll();
    UpdateAppBar();  // the reserved band follows the bar (no-op unless it changed)
}

void DockWindow::RelayoutItems() {
    if (!hwnd_ || inLayout_) return;
    LayoutPass();
    RenderWithRecovery([this] { return DrawFrame(false); });
    UpdateAppBar();
}

void DockWindow::LayoutPass() {
    inLayout_ = true;
    const Layout old = layout_;

    for (int pass = 0; pass < 2; ++pass) {
        const UINT before = dpi_;
        ComputeLayout();  // measures the elements (widget content) at the current scale
        ApplyWindowPos();
        // Moving to another monitor may change our DPI (WM_DPICHANGED arrives re-entrantly
        // during SetWindowPos); lay out once more if it did.
        const UINT now = GetDpiForWindow(hwnd_);
        dpi_ = now;
        if (now == before) break;
        labelDirty_ = true;
        for (DockItem& item : items_) {
            item.dirty = true;
            item.dotDirty = true;
        }
    }
    // Stage coordinates changed: place everything without animation. With the same geometry
    // (e.g. pins reordered) the running placement animations stay valid and are retargeted.
    const Layout& l = layout_;
    if ((old.edge != l.edge || old.shape != l.shape) && renderer_.Root()) {
        // Another edge: the slide and the launch bounces move along the other axis now. Start
        // from a clean pose (DrawFrame re-applies the root's; bounces stop where they are).
        renderer_.Root()->SetOffsetX(0.0f);
        renderer_.Root()->SetOffsetY(0.0f);
        rootY_.valid = false;
        for (DockItem& item : items_) {
            if (item.bounce == DockItem::Bounce::None || item.bounceStyle == LaunchStyle::Pulse) continue;
            renderer_.StopOffsetAnimation(item.layer, 0.0f, IsVertical(old.edge));
            item.bounce = DockItem::Bounce::None;
        }
    }
    if (old.edge != l.edge || old.shape != l.shape || old.polar.R != l.polar.R || old.polar.cx != l.polar.cx ||
        old.polar.cy != l.polar.cy || !EqualRect(&old.window, &l.window) || old.scale != l.scale || old.bar.left != l.bar.left ||
        old.bar.top != l.bar.top || old.bar.right != l.bar.right || old.bar.bottom != l.bar.bottom ||
        old.itemsLeft != l.itemsLeft || old.iconPx != l.iconPx || old.gap != l.gap || old.growMax != l.growMax ||
        old.itemsW != l.itemsW) {
        snapPending_ = true;
        if (watching_) shared_->Watch().Poke();  // the bar's column moved: re-test the overlap (no-op unless intelligent)
    }

    inLayout_ = false;
    // Round 12 phase 12D: customize mode: the outline is drawn for the new bar, the edit bar follows.
    if (editing_) {
        editDirty_ = true;
        const RECT frame = EditFrameRect();
        if (!EqualRect(&frame, &editFrameShown_)) {
            editFrameShown_ = frame;
            if (editRequests_.moved) editRequests_.moved();
        }
    }
    // Round 12 phase 12E: the settings panel beside the dock follows its resting bar.
    if (restMoved_) {
        const RECT rest = RestBarRect();
        if (!EqualRect(&rest, &restShown_)) {
            restShown_ = rest;
            restMoved_();
        }
    }
}

void DockWindow::ResetLayers() {
    bgLeftClip_.Reset();
    bgLeft_.Reset();
    bgRightClip_.Reset();
    bgRight_.Reset();
    widgetsGroup_.Reset();
    perspectiveApplied_ = false;
    itemsGroup_.Reset();
    for (Layer& d : dotSources_) d.Reset();
    marker_.Reset();
    markerX_.valid = false;
    label3_ = nullptr;
    labelOpacity_.valid = false;
    label_.Reset();
    title3_ = nullptr;  // round 12 phase 12C
    titleOpacity_.valid = false;
    title_.Reset();
    scrollThumb_.Reset();
    thumbMain_.valid = thumbOpacity_.valid = false;
    thumbLen_ = 0.0f;
    hintShown_ = false;
    // Round 12 phase 12D: customize mode's visuals (made again by the next PlaceEditVisuals).
    editOutline_.Reset();
    editBadge_.Reset();
    for (Layer& p : editPlus_) p.Reset();
    for (Layer& a : editAnchors_) a.Reset();
    editDirty_ = true;
    for (DockItem& item : items_) {
        item.layer.Reset();
        item.glow.Reset();
        item.reflection.Reset();
        item.icon.Reset();
        item.iconBig.Reset();
        item.dot.Reset();
        item.button.Reset();  // round 11 phase 11A (CreateItemVisuals makes it again)
        item.buttonDrawn = -1;
        item.buttonPulse = false;
        item.caption.Reset();
        item.captionDirty = true;
        item.dotShift = D2D1_POINT_2F{};
        item.badge.Reset();
        item.badgeBig.Reset();
        item.badgeDirty = item.badgeCount > 0;
    }
}

HRESULT DockWindow::CreateLayers() {
    // Z-order (back to front): background halves, widgets, items, label. The widget slots and the
    // icon slots sit in two containers at the stage origin (both carry the perspective tilt).
    ResetLayers();
    HRESULT hr = renderer_.CreateLayer(bgLeftClip_);
    if (SUCCEEDED(hr)) hr = renderer_.CreateLayer(bgLeft_, &bgLeftClip_);
    if (SUCCEEDED(hr)) hr = renderer_.CreateLayer(bgRightClip_);
    if (SUCCEEDED(hr)) hr = renderer_.CreateLayer(bgRight_, &bgRightClip_);
    if (SUCCEEDED(hr)) hr = renderer_.CreateLayer(title_);
    // Round 12 phase 12C: titleShow "hover" fades the title (no fade without IDCompositionVisual3:
    // it is shown / hidden instead).
    if (SUCCEEDED(hr) && FAILED(title_.visual->QueryInterface(IID_PPV_ARGS(title3_.put())))) title3_ = nullptr;
    if (SUCCEEDED(hr)) hr = renderer_.CreateLayer(widgetsGroup_);
    if (SUCCEEDED(hr)) hr = renderer_.CreateLayer(itemsGroup_);
    for (Layer& d : dotSources_) {
        if (SUCCEEDED(hr)) hr = renderer_.CreateLayer(d, &itemsGroup_);
        if (SUCCEEDED(hr)) renderer_.SetVisible(d, false);
    }
    if (SUCCEEDED(hr)) hr = renderer_.CreateLayer(marker_);
    if (SUCCEEDED(hr)) renderer_.SetVisible(marker_, false);
    if (SUCCEEDED(hr)) hr = renderer_.CreateLayer(label_);
    if (SUCCEEDED(hr)) {
        renderer_.SetVisible(label_, false);
        if (FAILED(label_.visual->QueryInterface(IID_PPV_ARGS(label3_.put())))) label3_ = nullptr;  // no fade
    }
    for (DockItem& item : items_) {  // item visuals are created by DrawFrame
        item.dirty = true;
        item.dotDirty = true;
    }
    labelShown_ = false;
    labelDirty_ = true;
    titleDirty_ = true;
    snapPending_ = true;
    rootY_.valid = false;
    rootOpacity_.valid = false;
    rootScale_ = nullptr;  // the circle's root transform belonged to the old root
    rootTranslate_ = nullptr;
    rootScaleT_.valid = false;
    ringShown_ = false;
    bgRing_ = false;
    for (DockItem& item : items_) item.turn = 0.0f;
    return hr;
}

HRESULT DockWindow::CreateItemVisuals(DockItem& item) {
    item.layer.Reset();
    item.glow.Reset();
    item.reflection.Reset();
    item.icon.Reset();
    item.iconBig.Reset();
    item.dot.Reset();
    item.caption.Reset();  // created by RenderCaption while titles are on
    item.captionDirty = true;
    item.badge.Reset();    // created by RenderBadge when a count shows
    item.badgeBig.Reset();
    item.badgeDirty = item.badgeCount > 0;
    item.dotShift = D2D1_POINT_2F{};  // a fresh dot visual has no transform
    item.edgeOpacity = 1.0f;          // fresh visuals are opaque and not yet placed
    item.placementStale = false;
    // A widget is one plain visual in widgetsGroup_ (behind the icons): its content is drawn into
    // `layer` itself and it is placed with the visual's offsets (it never scales or bounces).
    if (item.IsWidget()) {
        const HRESULT hr = renderer_.CreateLayer(item.layer, &widgetsGroup_);
        item.dirty = true;
        item.dotDirty = false;
        item.appliedY = -1.0f;
        item.tx.valid = item.ts.valid = item.fade.valid = item.glowOpacity.valid = false;
        return hr;
    }
    // Round 11 phase 11A: the slot button first, behind the icon container (no surface until
    // RenderButtons gives it one; separators never do).
    renderer_.DestroyLayer(item.button);
    item.buttonDrawn = -1;
    item.buttonStacked = 0;
    item.buttonHot = D2D1::Point2F(-1.0f, -1.0f);
    item.buttonShift = 0.0f;
    item.buttonPulse = false;
    HRESULT hr = renderer_.CreateLayer(item.button, &itemsGroup_);
    // The dot goes in front of its icon container: with reflections it sits on the reflection.
    if (SUCCEEDED(hr)) hr = renderer_.CreateLayer(item.layer, &itemsGroup_);
    if (SUCCEEDED(hr)) hr = renderer_.EnsurePlacement(item.layer, FootOf(item).x, FootOf(item).y);
    if (SUCCEEDED(hr)) hr = renderer_.CreateLayer(item.glow, &item.layer);
    if (SUCCEEDED(hr)) hr = renderer_.CreateLayer(item.reflection, &item.layer);
    if (SUCCEEDED(hr)) hr = renderer_.CreateLayer(item.icon, &item.layer);
    if (SUCCEEDED(hr)) hr = renderer_.CreateLayer(item.iconBig, &item.layer);
    if (SUCCEEDED(hr)) hr = renderer_.CreateLayer(item.dot, &itemsGroup_);
    item.dirty = true;
    item.dotDirty = true;
    item.appliedY = -1.0f;
    item.tx.valid = item.ts.valid = item.fade.valid = item.glowOpacity.valid = false;
    return hr;
}

HRESULT DockWindow::RenderBackground() {
    const Layout& l = layout_;
    if (l.polar.on) {
        // A ring / half ring in the theme's fill, its edges the hairlines (no glass shelf, no seam:
        // one static surface the ring's box large).
        const ThemeConfig& theme = DockTheme();
        const float x0 = std::floor(l.ring.left), y0 = std::floor(l.ring.top);
        const UINT w = static_cast<UINT>(std::ceil(l.ring.right) - x0), h = static_cast<UINT>(std::ceil(l.ring.bottom) - y0);
        const float stroke = std::max(1.0f, std::round(l.scale));
        const D2D1_COLOR_F fill = Theme::ToColor(acrylic_ ? theme.acrylicBackground : theme.background);
        DockGeometry::Polar p = l.polar;
        p.cx -= x0;
        p.cy -= y0;
        const HRESULT hr = renderer_.DrawLayer(bgLeft_, w, h, [&](ID2D1DeviceContext* dc) {
            winrt::com_ptr<ID2D1PathGeometry> geo;
            winrt::com_ptr<ID2D1GeometrySink> sink;
            if (FAILED(renderer_.D2DFactory()->CreatePathGeometry(geo.put())) || FAILED(geo->Open(sink.put()))) return;
            sink->SetFillMode(D2D1_FILL_MODE_ALTERNATE);
            const float a0 = p.Angle(0.0f), a1 = p.Angle(l.bar.right);
            const int steps = std::clamp(static_cast<int>(std::ceil(std::fabs(a1 - a0) / (kPi / 180.0f))), 8, 1440);
            const bool full = l.shape == DockShape::Circle;
            for (int ring = 0; ring < 2; ++ring) {
                // The outer edge, then (a half ring) the inner one back, or (a circle) a loop of its own.
                const float r = ring == 0 ? l.ringOut - stroke / 2.0f : l.ringIn + stroke / 2.0f;
                for (int i = 0; i <= steps; ++i) {
                    const int k = ring == 0 || full ? i : steps - i;
                    const float a = a0 + (a1 - a0) * static_cast<float>(k) / static_cast<float>(steps);
                    const D2D1_POINT_2F pt = D2D1::Point2F(p.cx + r * std::cos(a), p.cy - r * std::sin(a));
                    if (i == 0 && (ring == 0 || full)) {
                        sink->BeginFigure(pt, D2D1_FIGURE_BEGIN_FILLED);
                    } else {
                        sink->AddLine(pt);
                    }
                }
                if (full || ring == 1) sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            }
            if (FAILED(sink->Close())) return;
            winrt::com_ptr<ID2D1SolidColorBrush> brush;
            if (FAILED(dc->CreateSolidColorBrush(fill, brush.put()))) return;
            dc->FillGeometry(geo.get(), brush.get());
            // Flash this dock: the edges in the dock's colour, twice as thick, in its lit steps.
            brush->SetColor(Theme::ToColor(FlashLit() ? flashColor_ : theme.barBorder));
            dc->DrawGeometry(geo.get(), brush.get(), FlashLit() ? stroke * 2.0f : stroke);
        });
        if (FAILED(hr)) return hr;
        bgSeparators_[0] = l.separatorX;
        bgSeparators_[1] = l.leftSeparatorX;
        bgLeftX_.valid = false;  // placed again
        return S_OK;
    }
    const UINT w = static_cast<UINT>(l.bgSurfaceW);
    const UINT h = static_cast<UINT>(l.bar.bottom - l.bar.top);
    const ThemeConfig& theme = DockTheme();
    const D2D1_COLOR_F fill = Theme::ToColor(acrylic_ ? theme.acrylicBackground : theme.background);
    const float radius = std::min(l.cornerRadius, static_cast<float>(std::min(w, h)) / 2.0f);
    const float stroke = std::max(1.0f, std::round(l.scale));
    // The hairline before trailing widgets keeps its distance from the right end (it moves with
    // the right half); the one after leading widgets from the left end (left half).
    const float separatorX = l.separatorX >= 0.0f ? l.separatorX + l.growMax : -1.0f;
    const float leftSeparatorX = l.leftSeparatorX;
    const float rowH = static_cast<float>(h) - l.reflH;  // the icon row, above any reflections

    // Drawn as for a bottom dock (DrawCanonical maps it to the dock's edge).
    HRESULT hr = DrawCanonical(bgLeft_, static_cast<float>(w), static_cast<float>(h), [&](ID2D1DeviceContext* dc) {
        if (theme.shelf) {
            Theme::DrawShelf(dc, D2D1::RectF(0, 0, static_cast<float>(w), static_cast<float>(h)), theme, l.scale,
                             stroke, separatorX, l.reflH, leftSeparatorX);
            if (FlashLit()) {
                winrt::com_ptr<ID2D1SolidColorBrush> lit;
                if (SUCCEEDED(dc->CreateSolidColorBrush(Theme::ToColor(flashColor_), lit.put()))) {
                    dc->DrawRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(stroke, stroke, static_cast<float>(w) - stroke, static_cast<float>(h) - stroke), radius, radius}, lit.get(), stroke * 2.0f);
                }
            }
            return;
        }
        winrt::com_ptr<ID2D1SolidColorBrush> brush;
        if (FAILED(dc->CreateSolidColorBrush(fill, brush.put()))) return;
        const D2D1_ROUNDED_RECT rr{D2D1::RectF(0, 0, static_cast<float>(w), static_cast<float>(h)), radius, radius};
        dc->FillRoundedRectangle(rr, brush.get());
        // Round 11 phase 11A: the glossy sheen over the fill (and the acrylic tint), under the border.
        Theme::DrawSheen(dc, rr.rect, radius, theme, stroke);
        // Hairline highlight so the bar reads against dark wallpapers.
        const float inset = stroke / 2.0f;
        const D2D1_ROUNDED_RECT edge{
            D2D1::RectF(inset, inset, static_cast<float>(w) - inset, static_cast<float>(h) - inset),
            std::max(0.0f, radius - inset), std::max(0.0f, radius - inset)};
        if (FlashLit()) {
            // Flash this dock: the border in the dock's colour, twice as thick.
            const D2D1_ROUNDED_RECT lit{D2D1::RectF(stroke, stroke, static_cast<float>(w) - stroke, static_cast<float>(h) - stroke),
                                        std::max(0.0f, radius - stroke), std::max(0.0f, radius - stroke)};
            brush->SetColor(Theme::ToColor(flashColor_));
            dc->DrawRoundedRectangle(lit, brush.get(), stroke * 2.0f);
        } else {
            brush->SetColor(Theme::ToColor(theme.barBorder));
            dc->DrawRoundedRectangle(edge, brush.get(), stroke);
        }
        // Hairlines between the app icons and the widgets.
        const float vInset = std::round(rowH * 0.22f);
        brush->SetColor(Theme::ToColor(theme.separator));
        for (const float sx : {separatorX, leftSeparatorX}) {
            if (sx < 0.0f) continue;
            const float x = sx + stroke / 2.0f;
            dc->DrawLine(D2D1::Point2F(x, vInset), D2D1::Point2F(x, rowH - vInset), brush.get(), stroke);
        }
    });
    if (FAILED(hr)) return hr;
    bgSeparators_[0] = l.separatorX;
    bgSeparators_[1] = l.leftSeparatorX;
    bgLeftX_.valid = false;  // re-apply clips / offsets (the seam may have moved)
    return renderer_.ShareContent(bgRight_, bgLeft_);
}

void DockWindow::OnWeatherChanged() {
    if (!hwnd_) return;
    // The weather elements redraw (relayout if their size changed); ApplyConfig: its relayout
    // measures and draws them (DispatchEvent only marks them dirty while weatherQuiet_).
    DispatchEvent(DockEvent::WeatherData);
    if (!inLayout_ && !weatherQuiet_) weatherPopup_.Update();
}

void DockWindow::ApplyWeather() {
    // A weather item may have been added / removed / given another place.
    SyncElements();
    if (SlotOfKey(weatherKey_) < 0) DismissWeatherPopup(false, true);
    weatherQuiet_ = true;
    shared_->SyncWeather();
    weatherQuiet_ = false;
    OnWeatherChanged();  // redraw (relayout if the widget's size changed); once, whatever changed
}

int DockWindow::SlotOfKey(const AppKey& key) const {
    if (key.id.empty()) return -1;
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].key == key) return static_cast<int>(i);
    }
    return -1;
}

bool DockWindow::PlaceOfSlot(int slot, WeatherConfig& place) const {
    if (slot < 0 || slot >= static_cast<int>(items_.size())) return false;
    const size_t e = items_[static_cast<size_t>(slot)].element;
    return e < elements_.size() && elements_[e] && elements_[e]->WeatherPlace(place);
}

const WeatherService* DockWindow::WeatherOfItem(LPARAM item) const {
    // Config item `item` of this dock when it is a weather item; item 0 otherwise means the first
    // weather item (QueryState 27 - 30 before round 5 had no item: lParam 0).
    const auto& entries = DockCfg().items;
    size_t pick = SIZE_MAX;
    if (item >= 0 && static_cast<size_t>(item) < entries.size() && entries[static_cast<size_t>(item)].Kind() == ItemKind::Weather) {
        pick = static_cast<size_t>(item);
    } else if (item == 0) {
        for (size_t i = 0; i < entries.size() && pick == SIZE_MAX; ++i) {
            if (entries[i].Kind() == ItemKind::Weather) pick = i;
        }
    }
    if (pick == SIZE_MAX) return nullptr;
    const WeatherConfig place = WeatherOf(*config_, entries[pick]);
    return place.Configured() ? WeatherFor(place) : nullptr;
}

void DockWindow::OpenItemSettings(int slot) {
    if (slot < 0 || slot >= static_cast<int>(items_.size())) return;
    const size_t item = items_[static_cast<size_t>(slot)].element;  // = its index in the dock's items
    if (item < DockCfg().items.size() && weatherCallbacks_.openSettings) weatherCallbacks_.openSettings(item);
}

HRESULT DockWindow::RenderSeparator(DockItem& item) {
    // A vertical hairline in theme.separator, centred in the slot, as long as the one before the
    // clock. Never magnified, so there is no magnified-size surface.
    const Layout& l = layout_;
    const UINT w = static_cast<UINT>(std::max(1.0f, item.slotW));
    const UINT h = static_cast<UINT>(l.iconPx);
    const float stroke = std::max(1.0f, std::round(l.scale));
    const float barH = l.bar.bottom - l.bar.top - l.reflH;  // the icon row
    const float vInset = std::round(barH * 0.22f);
    const float boxTop = l.itemTop - l.bar.top;  // the icon box inside the bar
    const float top = std::clamp(vInset - boxTop, 0.0f, static_cast<float>(h));
    const float bottom = std::clamp(barH - vInset - boxTop, top, static_cast<float>(h));
    const D2D1_COLOR_F color = Theme::ToColor(DockTheme().separator);
    HRESULT hr = DrawCanonical(item.icon, static_cast<float>(w), static_cast<float>(h), [&](ID2D1DeviceContext* dc) {
        winrt::com_ptr<ID2D1SolidColorBrush> brush;
        if (FAILED(dc->CreateSolidColorBrush(color, brush.put()))) return;
        const float x = std::floor(static_cast<float>(w) / 2.0f) + stroke / 2.0f;
        dc->DrawLine(D2D1::Point2F(x, top), D2D1::Point2F(x, bottom), brush.get(), stroke);
    });
    if (SUCCEEDED(hr)) hr = renderer_.DrawLayer(item.iconBig, 0, 0, {});
    // No glow, no reflection.
    if (SUCCEEDED(hr)) hr = renderer_.DrawLayer(item.glow, 0, 0, {});
    if (SUCCEEDED(hr)) hr = renderer_.DrawLayer(item.reflection, 0, 0, {});
    if (FAILED(hr)) return hr;
    item.glowOpacity.valid = false;
    item.dirty = false;
    renderer_.SetVisible(item.icon, true);
    renderer_.SetVisible(item.iconBig, false);
    return S_OK;
}

HRESULT DockWindow::RenderWidget(DockItem& item) {
    // The element draws its content at its measured size into the slot's one visual.
    const HRESULT hr = elements_[item.element]->Render(renderer_, item.layer, Context());
    if (FAILED(hr)) return hr;
    item.dirty = false;
    return S_OK;
}

HRESULT DockWindow::RenderItem(DockItem& item) {
    if (item.IsWidget()) return RenderWidget(item);
    if (item.kind == DockItem::Kind::Separator) return RenderSeparator(item);
    const Layout& l = layout_;
    const float icon = l.iconPx;
    const UINT smallPx = static_cast<UINT>(icon);
    const UINT bigPx = l.bitmapPx > smallPx ? l.bitmapPx : 0;
    HWND sample = item.windows.empty() ? nullptr : item.windows.front();
    // A group without a custom icon: a tile with up to four of its members' icons.
    const bool composed = item.kind == DockItem::Kind::Group && !item.customIcon;
    // May be slow (shell icon extraction) the first time per app; cached afterwards.
    // A pinned file that no longer exists shows the placeholder (unless the app is running).
    ID2D1Bitmap* bmp = composed || (item.missing && item.windows.empty()) ? nullptr
                       : item.kind == DockItem::Kind::TrayApp
                           ? TrayAppBitmap(item)
                           : icons_.Get(item.key, sample, l.bitmapPx, renderer_.ResourceContext(), item.iconFile,
                                        item.iconIndex);
    if (item.thumbWindow) return RenderTile(item, bmp);
    std::vector<ID2D1Bitmap*> minis;
    if (composed) {
        for (size_t k = 0; k < item.members.size() && minis.size() < 4; ++k) {
            const DockItem::Member& m = item.members[k];
            minis.push_back(m.missing ? nullptr
                                      : icons_.Get(m.key, nullptr, l.bitmapPx, renderer_.ResourceContext(), m.iconFile,
                                                   m.iconIndex));
        }
    }

    auto drawIcon = [&](ID2D1DeviceContext* dc, float size) {
        if (composed) {
            // A translucent rounded tile in the label text colour, a hairline, a 2 x 2 grid.
            const ThemeConfig& t = DockTheme();
            winrt::com_ptr<ID2D1SolidColorBrush> brush;
            if (FAILED(dc->CreateSolidColorBrush(Theme::ToColor(t.labelText, 0.22f), brush.put()))) return;
            const float inset = std::round(size * 0.06f);
            const float r = size * 0.22f;
            const D2D1_RECT_F tile = D2D1::RectF(inset, inset, size - inset, size - inset);
            dc->FillRoundedRectangle(D2D1::RoundedRect(tile, r, r), brush.get());
            const float stroke = std::max(1.0f, std::round(size / 48.0f));
            brush->SetColor(Theme::ToColor(t.barBorder));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tile.left + stroke / 2.0f, tile.top + stroke / 2.0f,
                                                                   tile.right - stroke / 2.0f, tile.bottom - stroke / 2.0f),
                                                       r - stroke / 2.0f, r - stroke / 2.0f),
                                     brush.get(), stroke);
            const float pad = size * 0.19f, gap = size * 0.06f;
            const float mini = (size - 2.0f * pad - gap) / 2.0f;
            for (size_t k = 0; k < minis.size(); ++k) {
                const float x = pad + static_cast<float>(k % 2) * (mini + gap);
                const float y = pad + static_cast<float>(k / 2) * (mini + gap);
                if (ID2D1Bitmap* b = minis[k]) {
                    const D2D1_SIZE_F bs = b->GetSize();
                    const float fit = std::min(mini / std::max(bs.width, 1.0f), mini / std::max(bs.height, 1.0f));
                    const float dw = bs.width * fit, dh = bs.height * fit;
                    const D2D1_RECT_F dst =
                        D2D1::RectF(x + (mini - dw) / 2.0f, y + (mini - dh) / 2.0f, x + (mini + dw) / 2.0f, y + (mini + dh) / 2.0f);
                    dc->DrawBitmap(b, &dst, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
                } else {
                    brush->SetColor(Theme::ToColor(t.placeholderFill));
                    const float mr = mini * 0.22f;
                    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + mini, y + mini), mr, mr), brush.get());
                }
            }
            return;
        }
        if (bmp) {
            const D2D1_SIZE_F bs = bmp->GetSize();
            const float fit = std::min(size / std::max(bs.width, 1.0f), size / std::max(bs.height, 1.0f));
            const float dw = bs.width * fit;
            const float dh = bs.height * fit;
            const D2D1_RECT_F dst =
                D2D1::RectF((size - dw) / 2.0f, (size - dh) / 2.0f, (size + dw) / 2.0f, (size + dh) / 2.0f);
            dc->DrawBitmap(bmp, &dst, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
            return;
        }
        // Placeholder: rounded square with the app's first letter.
        winrt::com_ptr<ID2D1SolidColorBrush> brush;
        if (FAILED(dc->CreateSolidColorBrush(Theme::ToColor(DockTheme().placeholderFill), brush.put()))) return;
        const float inset = std::round(size * 0.06f);
        const float r = size * 0.22f;
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(inset, inset, size - inset, size - inset), r, r),
                                 brush.get());
        winrt::com_ptr<IDWriteTextFormat> letterFormat;
        if (renderer_.DWrite() &&
            SUCCEEDED(renderer_.DWrite()->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                                           DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                           size * 0.5f, L"", letterFormat.put()))) {
            letterFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            letterFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            const std::wstring& name = item.displayName.empty() ? item.key.Source() : item.displayName;
            const wchar_t letter[2] = {name.empty() ? L'?' : static_cast<wchar_t>(std::towupper(name[0])), 0};
            brush->SetColor(Theme::ToColor(DockTheme().placeholderText));
            dc->DrawText(letter, 1, letterFormat.get(), D2D1::RectF(0, 0, size, size), brush.get());
        }
    };

    HRESULT hr = renderer_.DrawLayer(item.icon, smallPx, smallPx, [&](ID2D1DeviceContext* dc) { drawIcon(dc, icon); });
    if (SUCCEEDED(hr)) {
        if (bigPx) {
            hr = renderer_.DrawLayer(item.iconBig, bigPx, bigPx,
                                     [&](ID2D1DeviceContext* dc) { drawIcon(dc, static_cast<float>(bigPx)); });
        } else {
            hr = renderer_.DrawLayer(item.iconBig, 0, 0, {});
        }
    }
    if (SUCCEEDED(hr)) hr = RenderEffects(item, drawIcon);
    if (FAILED(hr)) return hr;
    item.dirty = false;
    // The magnified-size surface is drawn at bitmapPx and shown at iconPx * scale.
    if (bigPx) renderer_.SetStaticTransform(item.iconBig, D2D1::Matrix3x2F::Scale(icon / bigPx, icon / bigPx));
    const bool big = bigIcons_ && bigPx;
    renderer_.SetVisible(item.icon, !big);
    renderer_.SetVisible(item.iconBig, big);
    return S_OK;
}

float DockWindow::TilePx() const {
    const DockConfig& d = DockCfg();
    const int icon = layout_.iconDip > 0 ? layout_.iconDip : d.iconSize;  // "shrink" lays out smaller icons
    return std::round(static_cast<float>(d.thumbnailSize > 0 ? d.thumbnailSize : 2 * icon) * layout_.scale);
}

HRESULT DockWindow::RenderTile(DockItem& item, ID2D1Bitmap* icon) {
    // Drawn upright like an icon, at rest size and at the magnified size (crisp both ways); the
    // frame keeps the window's aspect inside the tile, the app icon sits in its bottom-right corner.
    const Layout& l = layout_;
    const D2D1_SIZE_F rest = l.PhysSize(BoxW(item), l.iconPx);
    const UINT smallW = static_cast<UINT>(rest.width), smallH = static_cast<UINT>(rest.height);
    const float k = l.bitmapPx > static_cast<UINT>(l.iconPx) ? static_cast<float>(l.bitmapPx) / l.iconPx : 0.0f;
    const UINT bigW = k > 0.0f ? static_cast<UINT>(std::lround(rest.width * k)) : 0;
    const UINT bigH = k > 0.0f ? static_cast<UINT>(std::lround(rest.height * k)) : 0;
    ID2D1Bitmap* frame = thumbs_.FrameOf(item.thumbWindow);
    // A minimized window: its last frame, dimmed.
    const bool dim = item.thumbDim;
    const ThemeConfig& t = DockTheme();
    auto draw = [&](ID2D1DeviceContext* dc, float w, float h) {
        const float cross = std::min(w, h);  // the tile across the dock (an icon's size)
        const float u = cross / 48.0f;       // tile units
        const float radius = std::max(2.0f, std::round(5.0f * u));
        winrt::com_ptr<ID2D1SolidColorBrush> brush;
        if (FAILED(dc->CreateSolidColorBrush(Theme::ToColor(t.previewHotBackground), brush.put()))) return;
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, w, h), radius, radius), brush.get());
        if (frame) {
            const D2D1_SIZE_F fs = frame->GetSize();
            const float inset = std::max(1.0f, std::round(2.0f * u));
            const float fit = std::min((w - 2.0f * inset) / std::max(fs.width, 1.0f), (h - 2.0f * inset) / std::max(fs.height, 1.0f));
            const float fw = std::round(fs.width * fit), fh = std::round(fs.height * fit);
            const float x = std::round((w - fw) / 2.0f), y = std::round((h - fh) / 2.0f);
            dc->DrawBitmap(frame, D2D1::RectF(x, y, x + fw, y + fh), dim ? 0.45f : 1.0f,
                           D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
            brush->SetColor(Theme::ToColor(t.barBorder));
            const float stroke = std::max(1.0f, std::round(u));
            dc->DrawRectangle(D2D1::RectF(x + stroke / 2.0f, y + stroke / 2.0f, x + fw - stroke / 2.0f, y + fh - stroke / 2.0f),
                              brush.get(), stroke);
        }
        if (!icon) return;
        // The app icon: in the corner over the frame, or centred while there is no frame yet.
        const float size = frame ? std::round(cross * 0.46f) : cross;
        const float x = frame ? w - size : std::round((w - size) / 2.0f);
        const float y = frame ? h - size : std::round((h - size) / 2.0f);
        const D2D1_SIZE_F bs = icon->GetSize();
        const float fit = std::min(size / std::max(bs.width, 1.0f), size / std::max(bs.height, 1.0f));
        const float dw = bs.width * fit, dh = bs.height * fit;
        const D2D1_RECT_F dst = D2D1::RectF(x + (size - dw) / 2.0f, y + (size - dh) / 2.0f, x + (size + dw) / 2.0f,
                                            y + (size + dh) / 2.0f);
        dc->DrawBitmap(icon, &dst, dim && frame ? 0.7f : 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
    };
    HRESULT hr = renderer_.DrawLayer(item.icon, smallW, smallH, [&](ID2D1DeviceContext* dc) {
        draw(dc, static_cast<float>(smallW), static_cast<float>(smallH));
    });
    if (SUCCEEDED(hr)) {
        hr = bigW ? renderer_.DrawLayer(item.iconBig, bigW, bigH,
                                        [&](ID2D1DeviceContext* dc) { draw(dc, static_cast<float>(bigW), static_cast<float>(bigH)); })
                  : renderer_.DrawLayer(item.iconBig, 0, 0, {});
    }
    // Tiles have no glow and no reflection.
    if (SUCCEEDED(hr)) hr = renderer_.DrawLayer(item.glow, 0, 0, {});
    if (SUCCEEDED(hr)) hr = renderer_.DrawLayer(item.reflection, 0, 0, {});
    if (FAILED(hr)) return hr;
    item.glowOpacity.valid = false;
    item.dirty = false;
    if (bigW) renderer_.SetStaticTransform(item.iconBig, D2D1::Matrix3x2F::Scale(1.0f / k, 1.0f / k));
    const bool big = bigIcons_ && bigW;
    renderer_.SetVisible(item.icon, !big);
    renderer_.SetVisible(item.iconBig, big);
    return S_OK;
}

HRESULT DockWindow::RenderEffects(DockItem& item, const std::function<void(ID2D1DeviceContext*, float)>& drawIcon) {
    // Drawn here, once per icon draw; nothing in them changes with magnification, bounces or
    // drags (the static scale maps the bitmap size to the rest size, the layer transform does the rest).
    const Layout& l = layout_;
    const ThemeConfig& t = DockTheme();
    const UINT bitmapPx = l.bitmapPx;
    const float k = bitmapPx ? l.iconPx / static_cast<float>(bitmapPx) : 1.0f;
    const bool wantGlow = GlowOf(t) != GlowMode::Off && t.glowStrength > 0.0f && bitmapPx;
    // Reflections toward the screen edge: under the icons of a bottom dock, above those of a top
    // dock; none on vertical docks.
    const UINT reflH =
        t.reflection && bitmapPx && !l.vertical ? IconEffects::ReflectionHeight(bitmapPx, t.reflectionHeight) : 0;
    const bool top = l.edge == DockEdge::Top;
    winrt::com_ptr<ID2D1CommandList> icon, mirrored;
    if ((wantGlow || reflH) && renderer_.ResourceContext()) {
        icon = IconEffects::Record(renderer_.ResourceContext(), static_cast<float>(bitmapPx), drawIcon);
    }
    if (reflH && top && renderer_.ResourceContext()) {
        // A top dock reflects the icon's top: the reflection of the icon drawn upside down is the
        // icon's top rows in reading order, shown flipped above the icon (below).
        mirrored = IconEffects::Record(renderer_.ResourceContext(), static_cast<float>(bitmapPx),
                                       [&](ID2D1DeviceContext* dc, float size) {
                                           D2D1_MATRIX_3X2_F old;
                                           dc->GetTransform(&old);
                                           dc->SetTransform(D2D1::Matrix3x2F::Scale(1.0f, -1.0f) *
                                                            D2D1::Matrix3x2F::Translation(0.0f, size) * old);
                                           drawIcon(dc, size);
                                           dc->SetTransform(old);
                                       });
    }

    HRESULT hr = S_OK;
    if (wantGlow && icon) {
        const float radiusPx = t.glowRadius * l.scale / k;  // DIPs -> bitmap pixels
        const UINT pad = IconEffects::GlowPad(radiusPx);
        const uint32_t color = IconEffects::GlowColor(t, icons_.DominantColor(item.key), t.accent);
        const float fpad = static_cast<float>(pad);
        bool drawn = false;
        hr = renderer_.DrawLayer(item.glow, bitmapPx + 2 * pad, bitmapPx + 2 * pad, [&](ID2D1DeviceContext* dc) {
            drawn = IconEffects::DrawGlow(dc, icon.get(), radiusPx, color, t.glowStrength, D2D1::Point2F(fpad, fpad));
        });
        if (SUCCEEDED(hr) && !drawn) {
            Log(L"glow effect", E_FAIL);
            hr = renderer_.DrawLayer(item.glow, 0, 0, {});
        }
        if (SUCCEEDED(hr) && drawn) {
            renderer_.SetStaticTransform(item.glow,
                                         D2D1::Matrix3x2F::Translation(-fpad, -fpad) * D2D1::Matrix3x2F::Scale(k, k));
        }
    } else {
        hr = renderer_.DrawLayer(item.glow, 0, 0, {});
    }
    if (FAILED(hr)) return hr;
    item.glowOpacity.valid = false;  // a new surface: set its opacity statically
    DriveGlow(item, 0.0);

    ID2D1CommandList* source = top ? mirrored.get() : icon.get();
    if (reflH && source) {
        const float size = static_cast<float>(bitmapPx);
        hr = renderer_.DrawLayer(item.reflection, bitmapPx, reflH, [&](ID2D1DeviceContext* dc) {
            IconEffects::DrawReflection(dc, source, size, static_cast<float>(reflH), t.reflectionOpacity);
        });
        // One pixel past the icon's foot, inside the container: it scales with the icon about the
        // foot, so it grows toward the screen edge while magnified (downward; a top dock: upward,
        // the surface flipped so its most opaque row stays at the icon).
        if (SUCCEEDED(hr)) {
            renderer_.SetStaticTransform(item.reflection,
                                         top ? D2D1::Matrix3x2F::Scale(k, -k) * D2D1::Matrix3x2F::Translation(0.0f, -1.0f)
                                             : D2D1::Matrix3x2F::Scale(k, k) *
                                                   D2D1::Matrix3x2F::Translation(0.0f, l.iconPx + 1.0f));
        }
    } else {
        hr = renderer_.DrawLayer(item.reflection, 0, 0, {});
    }
    return hr;
}

float DockWindow::GlowTarget(const DockItem& item) const {
    if (!item.glow.width) return 0.0f;
    switch (GlowOf(DockTheme())) {
        case GlowMode::Always: return 1.0f;
        case GlowMode::Active: return item.active ? 1.0f : 0.0f;
        case GlowMode::Hover:
            return hovered_ >= 0 && hovered_ < static_cast<int>(items_.size()) &&
                           &items_[static_cast<size_t>(hovered_)] == &item
                       ? 1.0f
                       : 0.0f;
        case GlowMode::Off:
        default: return 0.0f;
    }
}

void DockWindow::DriveGlow(DockItem& item, double duration) {
    if (!item.glow.visual) return;
    winrt::com_ptr<IDCompositionVisual3> v3 = item.glow.visual.try_as<IDCompositionVisual3>();
    if (!v3) return;  // no opacity: the halo shows whenever it has a surface
    renderer_.Drive(
        item.glowOpacity, GlowTarget(item), duration, Renderer::Now(), [&](auto v) { return v3->SetOpacity(v); },
        0.001f, renderer_.FadeEasing());
}

void DockWindow::UpdateGlows(double duration) {
    if (GlowOf(DockTheme()) == GlowMode::Off) return;
    for (DockItem& item : items_) DriveGlow(item, duration);
}

void DockWindow::ApplyPerspective() {
    // One static 3D effect shared by the background halves' clips, the clock / weather container
    // and the items group (all at the stage origin): a rotation about the X axis through the bar's
    // bottom edge, then a perspective projection centred on the same point. The children's 2D
    // placement animations keep running inside it; nothing is redrawn.
    Layer* targets[] = {&bgLeftClip_, &bgRightClip_, &widgetsGroup_, &itemsGroup_};
    const ThemeConfig& t = DockTheme();
    IDCompositionDesktopDevice* dcomp = renderer_.DComp();
    const bool want = t.perspective && t.perspectiveAngle > 0.0f && dcomp && !layout_.polar.on;  // not on a ring
    if (!want) {
        if (perspectiveApplied_) {
            for (Layer* layer : targets) {
                if (layer->visual) layer->visual->SetEffect(nullptr);
            }
        }
        perspectiveApplied_ = false;
        return;
    }
    const Layout& l = layout_;
    // About the bar's edge-side line (the bottom edge of a bottom dock), its middle the centre.
    const float stageW = l.stageMain;
    const D2D1_POINT_2F pivot = l.Phys(stageW / 2.0f, l.bar.bottom);
    const float cx = pivot.x;
    const float cy = pivot.y;
    // Viewer distance: one stage width (along the bar). (Four stage widths, as first planned, made
    // the convergence ~0.5 % at the default 18 degrees: only the vertical squash was visible.)
    const float d = std::max(stageW, 1.0f);
    winrt::com_ptr<IDCompositionRotateTransform3D> rotate;
    winrt::com_ptr<IDCompositionMatrixTransform3D> project;
    winrt::com_ptr<IDCompositionTransform3D> group;
    HRESULT hr = dcomp->CreateRotateTransform3D(rotate.put());
    if (SUCCEEDED(hr)) {
        // Horizontal docks tilt about the X axis, vertical ones about Y; the side away from the
        // screen edge recedes.
        rotate->SetAxisX(l.vertical ? 0.0f : 1.0f);
        rotate->SetAxisY(l.vertical ? 1.0f : 0.0f);
        rotate->SetAxisZ(0.0f);
        // Positive about X: the top edge recedes (the bar leans back, its top narrower), as measured.
        const bool positive = l.edge == DockEdge::Bottom || l.edge == DockEdge::Left;
        rotate->SetAngle(positive ? t.perspectiveAngle : -t.perspectiveAngle);
        rotate->SetCenterX(cx);
        rotate->SetCenterY(cy);
        rotate->SetCenterZ(0.0f);
        hr = dcomp->CreateMatrixTransform3D(project.put());
    }
    if (SUCCEEDED(hr)) {
        // Translate(-c) x Perspective(_34 = -1/d) x Translate(c), row vectors: the vanishing point
        // is straight above the centre of the bar's bottom edge.
        D3DMATRIX m{};
        m._11 = m._22 = m._33 = m._44 = 1.0f;
        m._31 = -cx / d;
        m._32 = -cy / d;
        m._34 = -1.0f / d;
        hr = project->SetMatrix(m);
    }
    if (SUCCEEDED(hr)) {
        IDCompositionTransform3D* parts[] = {rotate.get(), project.get()};  // applied in order
        hr = dcomp->CreateTransform3DGroup(parts, 2, group.put());
    }
    for (Layer* layer : targets) {
        if (SUCCEEDED(hr) && layer->visual) hr = layer->visual->SetEffect(group.get());
    }
    if (FAILED(hr)) {
        Log(L"perspective effect", hr);
        for (Layer* layer : targets) {
            if (layer->visual) layer->visual->SetEffect(nullptr);
        }
        perspectiveApplied_ = false;
        return;
    }
    perspectiveApplied_ = true;
}

HRESULT DockWindow::RenderDotSources() {
    const Layout& l = layout_;
    const float icon = l.iconPx;
    const D2D1_COLOR_F colors[3] = {
        Theme::ToColor(DockTheme().dotRunning),
        Theme::ToColor(DockTheme().dotActive),     // owns the foreground window
        Theme::ToColor(DockTheme().dotAttention),  // wants attention
    };
    for (int i = 0; i < 3; ++i) {
        HRESULT hr = DrawCanonical(dotSources_[i], icon, static_cast<float>(l.dotHeight),
                                         [&](ID2D1DeviceContext* dc) {
                                             winrt::com_ptr<ID2D1SolidColorBrush> brush;
                                             if (FAILED(dc->CreateSolidColorBrush(colors[i], brush.put()))) return;
                                             dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(icon / 2.0f, l.dotCenterY),
                                                                           l.dotRadius, l.dotRadius),
                                                             brush.get());
                                         });
        if (FAILED(hr)) return hr;
    }
    for (DockItem& item : items_) item.dotDirty = true;  // the shared surfaces were replaced
    return S_OK;
}

HRESULT DockWindow::RenderDot(DockItem& item) {
    const Layer none{};  // no surface: no dot
    // A window slot's minimized window has no dot (it stays on the dock: minimize to dock).
    // A group: while one of its members runs.
    const bool running = item.kind == DockItem::Kind::Group ? item.groupRunning : !item.windows.empty();
    // Round 11 phase 11A: with slot buttons the frame is the indicator (the dot layers stay: cheap,
    // and the style can change at runtime).
    const Layer& source = layout_.buttons || !running || (item.window && item.minimized) ? none
                          : item.wantsAttention ? dotSources_[2]
                          : item.active         ? dotSources_[1]
                                                : dotSources_[0];
    const HRESULT hr = renderer_.ShareContent(item.dot, source);
    if (SUCCEEDED(hr)) item.dotDirty = false;
    return hr;
}

// --- slot buttons (theme.slotStyle "buttons", round 11 phase 11A) ------------------------------

int DockWindow::ButtonStateOf(size_t i, int* stacked) const {
    if (stacked) *stacked = 0;
    if (!layout_.buttons || i >= items_.size()) return 0;
    const DockItem& item = items_[i];
    if (!item.Magnifies()) return 0;  // separators and widgets get no button
    // Running as the dot counts it, except that a minimized-only window slot still shows its frame.
    const bool running = item.kind == DockItem::Kind::Group ? item.groupRunning : !item.windows.empty();
    int state = !running ? 0 : item.wantsAttention ? 3 : item.active ? 2 : 1;
    const int k = static_cast<int>(i);
    if (hovered_ == k) state |= 16;
    if (pressed_ == k && dragPhase_ == DragPhase::None) state |= 32;  // until the release or a drag start
    // Stacked windows: an app slot with two or more (a window slot never stacks; a group has none).
    if (running && !item.window && item.kind != DockItem::Kind::Group && item.windows.size() >= 2) {
        state |= 64;
        if (stacked) *stacked = static_cast<int>(std::min<size_t>(2, item.windows.size() - 1));
    }
    return state;
}

HRESULT DockWindow::RenderButtons() {
    const Layout& l = layout_;
    const ThemeConfig& t = DockTheme();
    constexpr float kNoHot = -100000.0f;
    HRESULT hr = S_OK;
    for (size_t i = 0; i < items_.size() && SUCCEEDED(hr); ++i) {
        DockItem& item = items_[i];
        if (item.IsWidget() || !item.button.visual) continue;
        int stacked = 0;
        const int state = ButtonStateOf(i, &stacked);
        const float w = std::max(0.0f, std::round(item.slotW - 2.0f * l.buttonInsetMain));
        const float h = std::round(l.buttonH);
        const bool shown = state != 0 && w >= 2.0f && h >= 2.0f;
        // Hot tracking: the pointer in the surface's canonical px (whole px: a move within a pixel
        // draws nothing), only while hovered. The frame is where the box's target puts it.
        const bool hot = shown && (state & 16) && t.slotHotTrack;
        const D2D1_POINT_2F hotAt = hot ? D2D1::Point2F(std::round(cursorX_ - (item.targetX + item.buttonShift)),
                                                        std::round(cursorV_ - l.buttonTop))
                                        : D2D1::Point2F(kNoHot, kNoHot);
        const D2D1_SIZE_F phys = l.PhysSize(w, h);
        const bool sized = !shown || (item.button.width == static_cast<UINT>(phys.width) &&
                                      item.button.height == static_cast<UINT>(phys.height));
        if (state == item.buttonDrawn && stacked == item.buttonStacked && hotAt.x == item.buttonHot.x &&
            hotAt.y == item.buttonHot.y && sized) {
            continue;  // nothing changed: nothing is drawn
        }
        if (!shown) {
            hr = renderer_.DrawLayer(item.button, 0, 0, {});  // no surface
        } else {
            Theme::SlotButton b;
            b.state = state & 15;
            b.hovered = (state & 16) != 0;
            b.pressed = (state & 32) != 0;
            b.stacked = stacked;
            b.hot = hot;
            b.hotAt = hotAt;
            if (hot) {
                // The icon's dominant colour (IconEffects, as the glow uses it); a group tile or a
                // placeholder: the accent.
                const bool ownIcon = !(item.kind == DockItem::Kind::Group && !item.customIcon) && !(item.missing && item.windows.empty());
                const uint32_t dominant = ownIcon ? icons_.DominantColor(item.key) : 0;
                b.hotColor = dominant ? dominant : t.accent;
            }
            hr = DrawCanonical(item.button, w, h, [&](ID2D1DeviceContext* dc) {
                Theme::DrawSlotButton(dc, D2D1::RectF(0.0f, 0.0f, w, h), t, l.scale, b);
            });
        }
        if (FAILED(hr)) break;
        // Attention pulses: opacity 100 % <-> 55 % every 0.8 s, run by DWM (no timer); none with
        // reduced motion.
        const bool pulse = shown && (state & 15) == 3 && !motion_.reduce;
        if (pulse != item.buttonPulse) {
            winrt::com_ptr<IDCompositionAnimation> anim = pulse ? renderer_.CreateAnimation() : nullptr;
            if (anim) {
                constexpr float kHalf = 0.4f, kLow = 0.55f, a = 1.0f - kLow;
                anim->AddCubic(0.0, 1.0f, 0.0f, -3.0f * a / (kHalf * kHalf), 2.0f * a / (kHalf * kHalf * kHalf));
                anim->AddCubic(kHalf, kLow, 0.0f, 3.0f * a / (kHalf * kHalf), -2.0f * a / (kHalf * kHalf * kHalf));
                anim->AddRepeat(2.0 * kHalf, 2.0 * kHalf);
                renderer_.AnimateOpacity(item.button, anim.get());
                item.buttonPulse = true;
            } else {
                renderer_.StopOpacityAnimation(item.button, 1.0f);
                item.buttonPulse = false;
                item.edgeOpacity = -1.0f;  // a scrolling dock's edge fade is set on it again
            }
        }
        item.buttonDrawn = state;
        item.buttonStacked = stacked;
        item.buttonHot = hotAt;
    }
    return hr;
}

void DockWindow::RefreshButtons() {
    if (!hwnd_ || !layout_.buttons) return;
    RenderWithRecovery([this] { return RenderButtons(); });
}

// --- notification badges (docks[].badges, round 4 phase 9) -----------------------------------

bool DockWindow::UpdateBadges() {
    const bool on = DockCfg().badges && shared_ && shared_->Notifications().Active();
    const NotificationService* svc = shared_ ? &shared_->Notifications() : nullptr;
    const std::vector<std::wstring>& excluded = config_->notifications.excluded;
    auto countFor = [&](const AppKey& k) -> int {
        if (k.id.empty() || std::find(excluded.begin(), excluded.end(), k.id) != excluded.end()) return 0;
        return static_cast<int>(svc->UnreadFor(k));
    };
    bool changed = false;
    for (size_t i = 0; i < items_.size(); ++i) {
        DockItem& item = items_[i];
        int n = 0;
        if (on && item.kind == DockItem::Kind::App) {
            if (item.pinned || !item.window) {
                n = countFor(item.key);
            } else {
                // Unstacked: the badge sits on the app's pin slot; an app without one shows it on
                // its first window slot.
                bool shown = false;
                for (size_t j = 0; j < items_.size() && !shown; ++j) {
                    if (j == i || items_[j].key != item.key || items_[j].kind != DockItem::Kind::App) continue;
                    shown = items_[j].pinned || !items_[j].window || j < i;
                }
                if (!shown) n = countFor(item.key);
            }
        } else if (on && item.kind == DockItem::Kind::Group) {
            for (const DockItem::Member& m : item.members) n += countFor(m.key);
        }
        if (n != item.badgeCount) {
            item.badgeCount = n;
            item.badgeDirty = true;
            changed = true;
        }
    }
    return changed;
}

HRESULT DockWindow::RenderBadge(DockItem& item) {
    item.badgeDirty = false;
    if (item.badgeCount <= 0) {
        HRESULT hr = S_OK;
        if (item.badge.visual) hr = renderer_.DrawLayer(item.badge, 0, 0, {});
        if (SUCCEEDED(hr) && item.badgeBig.visual) hr = renderer_.DrawLayer(item.badgeBig, 0, 0, {});
        return hr;
    }
    if (!item.layer.visual || !renderer_.DWrite()) return S_OK;
    HRESULT hr = S_OK;
    if (!item.badge.visual) hr = renderer_.CreateLayer(item.badge, &item.layer);  // in front of the icon
    if (SUCCEEDED(hr) && !item.badgeBig.visual) hr = renderer_.CreateLayer(item.badgeBig, &item.layer);
    if (FAILED(hr)) return hr;
    const Layout& l = layout_;
    const ThemeConfig& t = DockTheme();
    const std::wstring text = item.badgeCount > 99 ? L"99+" : std::to_wstring(item.badgeCount);
    // The bubble at rest size: badgeSize of the icon tall (at least 12 DIPs), a pill as wide as the text needs.
    const float h = std::max(std::round(l.iconPx * t.badgeSize), std::round(12.0f * l.scale));
    winrt::com_ptr<IDWriteTextFormat> format;
    winrt::com_ptr<IDWriteTextLayout> layout;
    float textW = 0.0f;
    if (SUCCEEDED(renderer_.DWrite()->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                                       DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, h * 0.66f,
                                                       L"", format.put())) &&
        SUCCEEDED(renderer_.DWrite()->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format.get(),
                                                       1000.0f, h, layout.put()))) {
        DWRITE_TEXT_METRICS m{};
        if (SUCCEEDED(layout->GetMetrics(&m))) textW = m.widthIncludingTrailingWhitespace;
    }
    const float w = std::max(h, std::ceil(textW + h * 0.62f));
    auto draw = [&](ID2D1DeviceContext* dc, float k) {
        const float bw = w * k, bh = h * k;
        winrt::com_ptr<ID2D1SolidColorBrush> brush;
        if (FAILED(dc->CreateSolidColorBrush(Theme::ToColor(t.badgeFill), brush.put()))) return;
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, bw, bh), bh / 2.0f, bh / 2.0f), brush.get());
        winrt::com_ptr<IDWriteTextFormat> f;
        if (FAILED(renderer_.DWrite()->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                        h * 0.66f * k, L"", f.put()))) {
            return;
        }
        f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        brush->SetColor(Theme::ToColor(t.badgeText));
        dc->DrawText(text.c_str(), static_cast<UINT32>(text.size()), f.get(), D2D1::RectF(0, -bh * 0.04f, bw, bh),
                     brush.get());
    };
    const UINT restW = static_cast<UINT>(w), restH = static_cast<UINT>(h);
    hr = renderer_.DrawLayer(item.badge, restW, restH, [&](ID2D1DeviceContext* dc) { draw(dc, 1.0f); });
    // Crisp while magnified: the same bubble at the magnified bitmap size, scaled down.
    const float k = l.bitmapPx > static_cast<UINT>(l.iconPx) ? static_cast<float>(l.bitmapPx) / l.iconPx : 0.0f;
    if (SUCCEEDED(hr)) {
        hr = k > 0.0f ? renderer_.DrawLayer(item.badgeBig, static_cast<UINT>(std::ceil(w * k)), static_cast<UINT>(std::ceil(h * k)),
                                            [&](ID2D1DeviceContext* dc) { draw(dc, k); })
                      : renderer_.DrawLayer(item.badgeBig, 0, 0, {});
    }
    if (FAILED(hr)) return hr;
    // At the icon's outer corner (away from the screen edge), overhanging it a little: top right on
    // bottom and left docks, bottom right on top docks, top left on right docks.
    const D2D1_SIZE_F box = l.PhysSize(BoxW(item), l.iconPx);
    const float o = std::round(h * 0.18f);
    float x = box.width - w + o, y = -o;
    if (l.edge == DockEdge::Top) y = box.height - h + o;
    if (l.edge == DockEdge::Right) x = -o;
    x = std::round(x);
    y = std::round(y);
    renderer_.SetStaticTransform(item.badge, D2D1::Matrix3x2F::Translation(x, y));
    if (k > 0.0f) renderer_.SetStaticTransform(item.badgeBig, D2D1::Matrix3x2F::Scale(1.0f / k, 1.0f / k) * D2D1::Matrix3x2F::Translation(x, y));
    const bool big = bigIcons_ && item.badgeBig.width > 0;
    renderer_.SetVisible(item.badge, !big);
    renderer_.SetVisible(item.badgeBig, big);
    return S_OK;
}

void DockWindow::OnNotificationsChanged() {
    if (!hwnd_ || !tracking_) return;
    if (UpdateBadges()) RenderWithRecovery([this] { return DrawFrame(false); });
    DispatchEvent(DockEvent::Notifications);  // the centre's bell
    if (centre_.Visible() && shared_) {
        centre_.Update(shared_->Notifications());
        RenderWithRecovery([] { return S_OK; });
    }
}

bool DockWindow::ActivateKey(const AppKey& key) {
    int slot = -1;
    for (size_t i = 0; i < items_.size(); ++i) {
        const DockItem& item = items_[i];
        if (item.kind != DockItem::Kind::App || item.key != key) continue;
        if (item.pinned || !item.window) {
            slot = static_cast<int>(i);
            break;
        }
        if (slot < 0) slot = static_cast<int>(i);
    }
    if (slot < 0) return false;
    ActivateOrLaunch(slot);
    return true;
}

bool DockWindow::WidgetActivateNotification(uint32_t id) { return shared_ && shared_->ActivateNotification(id); }

bool DockWindow::WidgetOpenCentre(WidgetInstance& widget) {
    if (SlotOfWidget(widget) < 0) return false;
    // From inside a script call: run it once the call has returned.
    centreRequest_ = widget.Key();
    PostMessageW(hwnd_, kCentreRequestMessage, 0, 0);
    return true;
}

HRESULT DockWindow::RenderLabel() {
    if (hovered_ < 0 || hovered_ >= static_cast<int>(items_.size()) || !renderer_.DWrite()) {
        labelShown_ = false;
        renderer_.SetVisible(label_, false);
        return S_OK;
    }
    if (!labelDirty_ && labelItem_ == hovered_ && label_.width) return S_OK;

    const DockItem& item = items_[static_cast<size_t>(hovered_)];
    const std::wstring text = SlotTitle(item);
    labelChars_ = text.size();
    const float s = layout_.scale;
    IDWriteFactory* dw = renderer_.DWrite();
    if (!EnsureLabelFormat(s)) return S_OK;

    const float maxW = std::round(kLabelMaxWidth * s);
    winrt::com_ptr<IDWriteTextLayout> textLayout;
    if (FAILED(dw->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), labelFormat_.get(), maxW, 1000.0f,
                                    textLayout.put()))) {
        return S_OK;
    }
    DWRITE_TEXT_METRICS m{};
    textLayout->GetMetrics(&m);
    const float textW = std::ceil(std::min(m.widthIncludingTrailingWhitespace, maxW));
    const float textH = std::ceil(m.height);
    const float padX = std::round(10.0f * s);
    const float padY = std::round(4.0f * s);
    const UINT w = static_cast<UINT>(textW + 2.0f * padX);
    const UINT h = static_cast<UINT>(textH + 2.0f * padY);
    const float radius = std::min(std::round(DockTheme().labelCornerRadius * s), static_cast<float>(h) / 2.0f);
    const float stroke = std::max(1.0f, std::round(s));
    const D2D1_COLOR_F back = Theme::ToColor(DockTheme().labelBackground);
    const D2D1_COLOR_F fore = Theme::ToColor(DockTheme().labelText);
    const D2D1_COLOR_F border = Theme::ToColor(DockTheme().labelBorder);

    HRESULT hr = renderer_.DrawLayer(label_, w, h, [&](ID2D1DeviceContext* dc) {
        winrt::com_ptr<ID2D1SolidColorBrush> brush;
        if (FAILED(dc->CreateSolidColorBrush(back, brush.put()))) return;
        const float fw = static_cast<float>(w), fh = static_cast<float>(h);
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, fw, fh), radius, radius), brush.get());
        const float inset = stroke / 2.0f;
        brush->SetColor(border);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(inset, inset, fw - inset, fh - inset),
                                                   std::max(0.0f, radius - inset), std::max(0.0f, radius - inset)),
                                 brush.get(), stroke);
        brush->SetColor(fore);
        dc->DrawTextLayout(D2D1::Point2F(padX, padY), textLayout.get(), brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    });
    if (FAILED(hr)) return hr;
    labelItem_ = hovered_;
    labelDirty_ = false;
    return S_OK;
}

bool DockWindow::EnsureLabelFormat(float s) {
    IDWriteFactory* dw = renderer_.DWrite();
    if (!dw) return false;
    if (labelFormat_ && labelScale_ == s) return true;
    labelFormat_ = nullptr;
    labelEllipsis_ = nullptr;
    if (FAILED(dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                    DWRITE_FONT_STRETCH_NORMAL, kLabelFontSize * s, L"", labelFormat_.put()))) {
        labelFormat_ = nullptr;
        return false;
    }
    labelFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (SUCCEEDED(dw->CreateEllipsisTrimmingSign(labelFormat_.get(), labelEllipsis_.put()))) {
        const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        labelFormat_->SetTrimming(&trimming, labelEllipsis_.get());
    }
    labelScale_ = s;
    return true;
}

std::wstring DockWindow::SlotTitle(const DockItem& item) const {
    if (hoverCell_ >= 0 && hoverCellSlot_ >= 0 && hoverCellSlot_ < static_cast<int>(items_.size()) &&
        &items_[static_cast<size_t>(hoverCellSlot_)] == &item) {
        return hoverCellLabel_;
    }
    if (item.kind == DockItem::Kind::RecycleBin) return RecycleBinLabel();
    if (item.window && !item.windowTitle.empty()) return item.windowTitle;
    return item.displayName.empty() ? item.key.Source() : item.displayName;
}

std::wstring DockWindow::CaptionFor(const DockItem& item) const {
    if (item.IsWidget() || item.kind == DockItem::Kind::Separator) return {};
    if (item.window && !item.windowTitle.empty()) return item.windowTitle;
    if (!item.displayName.empty()) return item.displayName;
    // No display name: the file name of the path behind the key (never a whole path).
    const std::wstring& src = item.key.Source();
    const size_t slash = src.find_last_of(L"\\/");
    return slash == std::wstring::npos ? src : src.substr(slash + 1);
}

int DockWindow::CaptionLines() const { return layout_.captionLines; }

bool DockWindow::EnsureCaptionFormat(float s) {
    IDWriteFactory* dw = renderer_.DWrite();
    if (!dw) return false;
    if (captionFormat_ && captionScale_ == s) return true;
    captionFormat_ = nullptr;
    captionEllipsis_ = nullptr;
    captionLineH_ = 0.0f;
    // The label's font (Segoe UI), smaller.
    if (FAILED(dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                    DWRITE_FONT_STRETCH_NORMAL, kCaptionFontSize * s, L"", captionFormat_.put()))) {
        captionFormat_ = nullptr;
        return false;
    }
    if (SUCCEEDED(dw->CreateEllipsisTrimmingSign(captionFormat_.get(), captionEllipsis_.put()))) {
        const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        captionFormat_->SetTrimming(&trimming, captionEllipsis_.get());
    }
    winrt::com_ptr<IDWriteTextLayout> probe;
    DWRITE_TEXT_METRICS m{};
    if (SUCCEEDED(dw->CreateTextLayout(L"Ag", 2, captionFormat_.get(), 1000.0f, 1000.0f, probe.put())) &&
        SUCCEEDED(probe->GetMetrics(&m))) {
        captionLineH_ = std::ceil(m.height);
    }
    if (captionLineH_ <= 0.0f) captionLineH_ = std::ceil(kCaptionFontSize * 1.33f * s);
    captionScale_ = s;
    return true;
}

HRESULT DockWindow::RenderCaption(DockItem& item) {
    const Layout& l = layout_;
    std::wstring text = l.captionLines > 0 ? CaptionFor(item) : std::wstring();
    IDWriteFactory* dw = renderer_.DWrite();
    if (text.empty() || !dw || !EnsureCaptionFormat(l.scale)) {
        // Titles off (or nothing to show): no visual at all.
        if (item.caption.visual) renderer_.DestroyLayer(item.caption);
        item.caption.Reset();
        item.captionText.clear();
        item.captionDirty = false;
        return S_OK;
    }
    if (!item.caption.visual) {
        const HRESULT hr = renderer_.CreateLayer(item.caption, &itemsGroup_);
        if (FAILED(hr)) return hr;
        item.tx.valid = false;  // ApplyTargets places it (cross offset, then the main tween with the icon)
        item.placementStale = false;
        if (item.edgeOpacity != 1.0f) item.edgeOpacity = -1.0f;  // the new visual is opaque: fade it again
    }
    // Physical box: under the icon on horizontal docks (the slot's width), beside it on vertical
    // ones (the text stays upright).
    const float s = l.scale;
    const float pad = std::round(kCaptionPad * s);
    // The slot's width (horizontal; a thumbnail tile is longer than an icon) or the box's length.
    const float captionMain = l.vertical ? BoxW(item) : BoxW(item) + (l.captionMain - l.iconPx);
    const D2D1_SIZE_F box = l.PhysSize(captionMain, l.captionCross);
    const float textW = std::max(1.0f, box.width - 2.0f * pad);
    const float textH = l.vertical ? box.height : std::ceil(captionLineH_ * static_cast<float>(l.captionLines));
    winrt::com_ptr<IDWriteTextLayout> layout;
    auto make = [&](const std::wstring& t) {
        layout = nullptr;
        if (FAILED(dw->CreateTextLayout(t.c_str(), static_cast<UINT32>(t.size()), captionFormat_.get(), textW,
                                        l.captionLines == 1 ? textH : 10000.0f, layout.put()))) {
            layout = nullptr;
            return;
        }
        layout->SetWordWrapping(l.captionLines == 1 ? DWRITE_WORD_WRAPPING_NO_WRAP : DWRITE_WORD_WRAPPING_EMERGENCY_BREAK);
        // Horizontal docks: centred under the icon. Vertical: toward the icon (right-aligned on a
        // left dock, whose titles sit between the icon and the screen edge).
        layout->SetTextAlignment(!l.vertical ? DWRITE_TEXT_ALIGNMENT_CENTER
                                 : l.edge == DockEdge::Left ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                            : DWRITE_TEXT_ALIGNMENT_LEADING);
    };
    make(text);
    if (layout && l.captionLines > 1) {
        // Two lines at most: cut what the wrap puts beyond them and end the second line in an
        // ellipsis (DirectWrite trims by width only).
        UINT32 count = 0;
        layout->GetLineMetrics(nullptr, 0, &count);
        if (count > static_cast<UINT32>(l.captionLines)) {
            std::vector<DWRITE_LINE_METRICS> lm(count);
            layout->GetLineMetrics(lm.data(), count, &count);
            size_t keep = 0;
            for (int k = 0; k < l.captionLines; ++k) keep += lm[static_cast<size_t>(k)].length;
            keep = std::min(keep, text.size());
            for (int tries = 0; tries < 64 && keep > 0; ++tries) {
                std::wstring cut = text.substr(0, keep);
                while (!cut.empty() && std::iswspace(cut.back())) cut.pop_back();
                cut += L"\x2026";
                make(cut);
                if (!layout) break;
                UINT32 n = 0;
                layout->GetLineMetrics(nullptr, 0, &n);
                if (n <= static_cast<UINT32>(l.captionLines)) break;
                --keep;
            }
        }
    }
    if (!layout) return S_OK;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    const float y = l.vertical ? std::round((box.height - std::min(m.height, box.height)) / 2.0f) : 0.0f;
    const D2D1_COLOR_F color = Theme::ToColor(DockTheme().clockText);  // drawn on the bar, like the clock
    HRESULT hr = renderer_.DrawLayer(item.caption, static_cast<UINT>(box.width), static_cast<UINT>(box.height),
                                     [&](ID2D1DeviceContext* dc) {
                                         winrt::com_ptr<ID2D1SolidColorBrush> brush;
                                         if (FAILED(dc->CreateSolidColorBrush(color, brush.put()))) return;
                                         dc->DrawTextLayout(D2D1::Point2F(pad, y), layout.get(), brush.get(),
                                                            D2D1_DRAW_TEXT_OPTIONS_CLIP);
                                     });
    if (FAILED(hr)) return hr;
    // The box sits captionShift along the bar from the icon box (the slot's left edge on horizontal
    // docks); the icon's main tween drives the visual's own offset.
    renderer_.SetStaticTransform(item.caption, l.vertical ? D2D1::Matrix3x2F::Translation(0.0f, l.captionShift)
                                                          : D2D1::Matrix3x2F::Translation(l.captionShift, 0.0f));
    item.captionText = std::move(text);
    item.captionDirty = false;
    return S_OK;
}

HRESULT DockWindow::RenderTitle() {
    // Like the name label: the label's pill, font and colours.
    const Layout& l = layout_;
    const UINT w = static_cast<UINT>(l.title.right - l.title.left);
    const UINT h = static_cast<UINT>(l.title.bottom - l.title.top);
    titleDirty_ = false;
    if (!titleLayout_ || !w || !h) return renderer_.DrawLayer(title_, 0, 0, {});
    const float s = l.scale;
    if (l.titleInside) {
        // Round 12 phase 12C: in the bar's band: the text alone, centred in the label's text colour
        // (the bar's fill is its background).
        const ThemeConfig& t = DockTheme();
        DWRITE_TEXT_METRICS tm{};
        titleLayout_->GetMetrics(&tm);
        const float padX = std::round(4.0f * s);
        const float padY = std::max(0.0f, std::round((static_cast<float>(h) - std::ceil(tm.height)) / 2.0f));
        HRESULT hr = renderer_.DrawLayer(title_, w, h, [&](ID2D1DeviceContext* dc) {
            winrt::com_ptr<ID2D1SolidColorBrush> brush;
            if (FAILED(dc->CreateSolidColorBrush(Theme::ToColor(t.labelText), brush.put()))) return;
            dc->DrawTextLayout(D2D1::Point2F(padX, padY), titleLayout_.get(), brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        });
        ApplyTitleOpacity(0.0);
        return hr;
    }
    const float padX = std::round(10.0f * s), padY = std::round(4.0f * s);
    const float radius = std::min(std::round(DockTheme().labelCornerRadius * s), static_cast<float>(h) / 2.0f);
    const float stroke = std::max(1.0f, std::round(s));
    const ThemeConfig& t = DockTheme();
    const HRESULT hr = renderer_.DrawLayer(title_, w, h, [&](ID2D1DeviceContext* dc) {
        winrt::com_ptr<ID2D1SolidColorBrush> brush;
        if (FAILED(dc->CreateSolidColorBrush(Theme::ToColor(t.labelBackground), brush.put()))) return;
        const float fw = static_cast<float>(w), fh = static_cast<float>(h);
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, fw, fh), radius, radius), brush.get());
        const float inset = stroke / 2.0f;
        brush->SetColor(Theme::ToColor(t.labelBorder));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(inset, inset, fw - inset, fh - inset),
                                                   std::max(0.0f, radius - inset), std::max(0.0f, radius - inset)),
                                 brush.get(), stroke);
        brush->SetColor(Theme::ToColor(t.labelText));
        dc->DrawTextLayout(D2D1::Point2F(padX, padY), titleLayout_.get(), brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    });
    ApplyTitleOpacity(0.0);  // round 12 phase 12C: a hover title starts as the pointer finds it
    return hr;
}

// --- round 12 phase 12C: docks[].titleShow "hover" ---------------------------------------------------

void DockWindow::ApplyTitleOpacity(double duration) {
    // Shown: always, or on hover while the pointer is over the dock. One DComp animation per change
    // (DWM runs it; nothing ticks here), a static set with reduced motion or duration 0.
    const bool hover = DockCfg().titleShow == TitleShow::Hover;
    const float target = !hover || titleHover_ ? 1.0f : 0.0f;
    if (!title3_) {
        renderer_.SetVisible(title_, target > 0.5f);
        return;
    }
    IDCompositionVisual3* v3 = title3_.get();
    renderer_.Drive(
        titleOpacity_, target, motion_.reduce ? 0.0 : duration, Renderer::Now(), [&](auto v) { return v3->SetOpacity(v); }, 0.001f,
        renderer_.FadeEasing());
}

void DockWindow::SetTitleHover(bool hover) {
    if (hover == titleHover_) return;
    titleHover_ = hover;
    // Only a hover title changes (and only when there is one): no commit otherwise.
    if (DockCfg().titleShow != TitleShow::Hover || !titleLayout_ || !title_.visual) return;
    RenderWithRecovery([this] {
        ApplyTitleOpacity(0.15);
        return S_OK;
    });
}

void DockWindow::ApplyReflectionClip() {
    // A clip on the icons' container at the bar's screen-edge side (in front of it, nothing of the
    // icons is cut: bounces and the label go the other way).
    // A scrolling run: both slot groups are cut at the viewport's clip (the bar's ends) on the main
    // axis.
    if (!itemsGroup_.visual) return;
    const Layout& l = layout_;
    const bool clipMain = l.view.on && !l.polar.on;  // a ring's sectors cannot be cut by a rectangle
    const float u0 = clipMain ? l.view.clipStart : -1e5f;
    const float u1 = clipMain ? l.view.clipEnd : 1e5f;
    if (layout_.reflClip || clipMain) {
        renderer_.SetClip(itemsGroup_, l.PhysRect(u0, -1e5f, u1, l.reflClip ? l.bar.bottom : 1e5f));
    } else if (itemsGroup_.clip) {
        itemsGroup_.visual->SetClip(nullptr);
        itemsGroup_.clip = nullptr;
    }
    if (!widgetsGroup_.visual) return;
    if (clipMain) {
        renderer_.SetClip(widgetsGroup_, l.PhysRect(u0, -1e5f, u1, 1e5f));
    } else if (widgetsGroup_.clip) {
        widgetsGroup_.visual->SetClip(nullptr);
        widgetsGroup_.clip = nullptr;
    }
}

// --- Scrolling (docks[].overflow "scroll", DockGeometry::Viewport) ----------------------------
// The layout keeps every rest position in view coordinates. A scroll shifts them all (ShiftScroll)
// and the slots glide to their new targets with one ease-out curve each (ApplyTargets), so the run
// moves as one; slots whose whole path is outside the viewport are only tracked. The edge fades
// are per-slot opacity curves built from the same placement curves (DriveEdgeFade). Nothing runs
// once the glide has ended (the thumb's fade-out is one one-shot timer).

namespace {
constexpr double kWheelScrollS = 0.2;      // the glide of a wheel step
constexpr double kFlingS = 0.55;           // inertia after a drag on the bar (an ease-out this long)
constexpr float kFlingMinSpeed = 250.0f;   // px / s: a slower release just stops
constexpr float kWheelSlots = 2.0f;        // icon slots per wheel notch
constexpr UINT kScrollHintMs = 900;        // the thumb fades out this long after the last scroll
constexpr double kHintFadeInS = 0.1;
constexpr double kHintFadeOutS = 0.3;
constexpr float kAutoScrollSlots = 14.0f;  // icon slots per second at a viewport's very end
constexpr UINT kAutoScrollMs = 30;
constexpr float kThumbDip = 3.0f;          // the thumb's thickness
constexpr float kThumbMinDip = 24.0f;      // and its shortest length
constexpr float kThumbInsetDip = 2.0f;     // from the bar's far side
}  // namespace

bool DockWindow::ShiftScroll(float to) {
    Layout& l = layout_;
    if (!l.view.on) return false;
    to = std::round(std::clamp(to, 0.0f, l.view.max));
    const float shift = scroll_ - to;
    if (shift == 0.0f) return false;
    scroll_ = to;
    l.view.scroll = to;
    l.itemsLeft += shift;
    for (DockItem& item : items_) {
        item.slotX += shift;
        item.x += shift;
    }
    // A preview popup would stay where its slot was.
    if (previewItem_ >= 0 || previewPending_) DismissPreview(true, false);
    // The slot under a still pointer is another one now.
    if (magnifying_ && dragPhase_ == DragPhase::None && !scrollDrag_) {
        ComputeTargets();  // the hit test reads the targets (the run moved)
        POINT screen{};
        if (GetCursorPos(&screen)) {
            const int hit = HitTest(POINT{screen.x - l.window.left, screen.y - l.window.top});
            SetHover(hit >= 0 && items_[static_cast<size_t>(hit)].IsWidget() ? -1 : hit);
        }
    }
    return true;
}

void DockWindow::ScrollTo(float to, double duration) {
    if (!ShiftScroll(to)) return;
    const double now = Renderer::Now();
    renderer_.Track(scrollShown_, scroll_, duration, now, 0.01f, Easing::EaseOut);
    if (dragPhase_ == DragPhase::Dragging) UpdateDragTarget(cursorX_);
    RenderWithRecovery([&] {
        ComputeTargets();
        ApplyTargets(duration, true);  // tracking: always an ease-out (the curve a fling is computed for)
        ShowScrollHint();
        PlaceScrollThumb(duration);
        return S_OK;
    });
}

void DockWindow::ScrollByWheel(int delta) {
    if (!layout_.view.on || delta == 0) return;
    // Proportional (precision touchpads send fractions of a notch); whole px per step, the rest kept.
    const float px = static_cast<float>(delta) / WHEEL_DELTA * kWheelSlots * (layout_.iconPx + layout_.gap) +
                     static_cast<float>(wheelRemainder_) / 100.0f;
    const float whole = std::trunc(px);
    wheelRemainder_ = static_cast<int>(std::lround((px - whole) * 100.0f));
    if (whole != 0.0f) ScrollTo(scroll_ + whole, kWheelScrollS * motion_.scale);
}

bool DockWindow::ScrollCommand(UINT cmd, int arg) {
    if (cmd == ContextMenu::kMagnifyAt) {
        // As if the pointer rested at stage main coordinate `arg` (0xFFFF: as if it left).
        if (arg == 0xFFFF) {
            LeaveZone();
            return true;
        }
        // The targets only: the window is not expanded (with no real pointer on it, its leave
        // tracking would end the magnification at once).
        if (hideState_ != HideState::Shown || dragPhase_ != DragPhase::None) return false;
        magnifying_ = true;
        cursorX_ = static_cast<float>(arg);
        cursorV_ = layout_.itemTop + layout_.iconPx / 2.0f;  // round 11 phase 11A: on the icon row (hot tracking)
        ComputeTargets();  // the hit test below reads them
        int slot = HitAlong(cursorX_);
        // As a real pointer: a "steady" bar grows over something that zooms (9A follow-up 2).
        if (GrowForZoom(slot)) slot = HitAlong(cursorX_);
        SetHover(slot >= 0 && !items_[static_cast<size_t>(slot)].IsWidget() ? slot : -1);  // the label follows
        // Round 11 phase 11B: a script widget there gets onHover as under a real pointer (the Start
        // orb's hover glow can be tested in a locked session); 0xFFFF's LeaveZone ends it.
        UpdateWidgetHover(slot >= 0 && items_[static_cast<size_t>(slot)].IsWidget() ? slot : -1);
        ApplyMagnify();
        return true;
    }
    if (!layout_.view.on) return false;
    if (cmd == ContextMenu::kScrollIntoView) return ScrollSlotIntoView(arg);
    ScrollTo(static_cast<float>(arg), cmd == ContextMenu::kScrollTo ? 0.0 : kWheelScrollS * motion_.scale);
    return true;
}

bool DockWindow::ScrollSlotIntoView(int index) {
    if (!layout_.view.on || index < 0 || index >= static_cast<int>(items_.size())) return false;
    const DockItem& it = items_[static_cast<size_t>(index)];
    float to = scroll_;
    if (it.slotX < layout_.view.start) to -= layout_.view.start - it.slotX;
    else if (it.slotX + it.slotW > layout_.view.end) to += it.slotX + it.slotW - layout_.view.end;
    ScrollTo(to, kWheelScrollS * motion_.scale);
    return true;
}

void DockWindow::BeginScrollDrag() {
    scrollDrag_ = true;
    pressed_ = -1;
    pressClosedPopup_ = false;
    scrollSampleCount_ = 0;
    SetHold(true);  // no hide meanwhile; the window stays at stage size
    magnifying_ = false;
    if (magnifyPending_) {
        KillTimer(hwnd_, kMagnifyTimerId);
        magnifyPending_ = false;
    }
    SetHover(-1);
    if (!expanded_) {
        expanded_ = true;
        UpdateWindowRect();
    }
}

void DockWindow::UpdateScrollDrag(POINT pt) {
    const float main = layout_.Canon(pt).x;
    const double now = Renderer::Now();
    if (scrollSampleCount_ == std::size(scrollSamples_)) {
        std::move(std::begin(scrollSamples_) + 1, std::end(scrollSamples_), std::begin(scrollSamples_));
        --scrollSampleCount_;
    }
    scrollSamples_[scrollSampleCount_++] = ScrollSample{now, main};
    if (!ShiftScroll(scrollPressOffset_ + (scrollPressMain_ - main))) return;
    renderer_.Track(scrollShown_, scroll_, motion_.magnify, now, 0.01f, Easing::EaseOut);
    ScheduleMagnify();  // the throttled retarget (with the thumb)
    RenderWithRecovery([this] {
        ShowScrollHint();
        return S_OK;
    });
}

void DockWindow::EndScrollDrag(bool fling) {
    const bool dragged = scrollDrag_;
    scrollPress_ = false;
    scrollDrag_ = false;
    if (GetCapture() == hwnd_) ReleaseCapture();  // WM_CAPTURECHANGED finds both flags cleared
    if (!dragged) return;
    // The pointer's speed over the last ~100 ms.
    float speed = 0.0f;
    if (fling && scrollSampleCount_ >= 2) {
        const ScrollSample& last = scrollSamples_[scrollSampleCount_ - 1];
        size_t first = scrollSampleCount_ - 1;
        while (first > 0 && last.t - scrollSamples_[first - 1].t <= 0.1) --first;
        const double dt = last.t - scrollSamples_[first].t;
        if (dt > 0.008 && Renderer::Now() - last.t < 0.08) {
            speed = static_cast<float>((last.main - scrollSamples_[first].main) / dt);
        }
    }
    SetHold(false);
    const double duration = kFlingS * motion_.scale;
    if (duration > 0.0 && std::fabs(speed) > kFlingMinSpeed) {
        // An ease-out starts at 3 d / D: matching the pointer's speed, the run glides d further.
        ScrollTo(scroll_ - speed * static_cast<float>(duration) / 3.0f, duration);
    } else {
        RenderWithRecovery([this] {
            ComputeTargets();
            ApplyTargets(motion_.rest);  // land on whole pixels
            PlaceScrollThumb(motion_.rest);
            return S_OK;
        });
    }
    if (!CursorInZone()) LeaveZone();
}

void DockWindow::UpdateAutoScroll(float main) {
    const DockGeometry::Viewport& v = layout_.view;
    float speed = 0.0f;
    if (v.on) {
        const float zone = layout_.iconPx + layout_.gap;
        const float top = kAutoScrollSlots * zone;
        if (main < v.start + zone && scroll_ > 0.0f) {
            speed = -top * std::clamp((v.start + zone - main) / zone, 0.25f, 1.0f);
        } else if (main > v.end - zone && scroll_ < v.max) {
            speed = top * std::clamp((main - (v.end - zone)) / zone, 0.25f, 1.0f);
        }
    }
    autoScrollSpeed_ = speed;
    if (speed == 0.0f) {
        StopAutoScroll();
        return;
    }
    if (!autoScrollPending_) {
        autoScrollLast_ = Renderer::Now();
        autoScrollPending_ = SetTimer(hwnd_, kAutoScrollTimerId, kAutoScrollMs, nullptr) != 0;
    }
}

void DockWindow::StopAutoScroll() {
    if (autoScrollPending_) KillTimer(hwnd_, kAutoScrollTimerId);
    autoScrollPending_ = false;
    autoScrollSpeed_ = 0.0f;
}

void DockWindow::OnAutoScrollTimer() {
    const double now = Renderer::Now();
    const double dt = std::min(now - autoScrollLast_, 0.1);
    autoScrollLast_ = now;
    const bool dragging = dragPhase_ == DragPhase::Dragging;
    if ((!dragging && !dropActive_) || autoScrollSpeed_ == 0.0f ||
        !ShiftScroll(scroll_ + autoScrollSpeed_ * static_cast<float>(dt))) {
        StopAutoScroll();  // the drag ended, or the run reached its end
        return;
    }
    renderer_.Track(scrollShown_, scroll_, motion_.magnify, now, 0.01f, Easing::EaseOut);
    if (dragging) {
        // The drag target under the (still) pointer moved: as a mouse move does.
        POINT screen{};
        GetCursorPos(&screen);
        UpdateDrag(POINT{screen.x - layout_.window.left, screen.y - layout_.window.top});
        RenderWithRecovery([this] {
            ShowScrollHint();
            return S_OK;
        });
        return;
    }
    int item = -1, gap = -1;
    const DropMode mode = DropModeAt(dropMain_, &item, &gap);
    SetDropTarget(mode, item, gap);
    RenderWithRecovery([this] {
        ComputeTargets();
        ApplyTargets(motion_.magnify, true);
        ShowScrollHint();
        PlaceScrollThumb(motion_.magnify);
        return S_OK;
    });
}

HRESULT DockWindow::RenderScrollThumb() {
    const Layout& l = layout_;
    const DockGeometry::Viewport& v = l.view;
    if (!v.on) return S_OK;
    if (!scrollThumb_.visual) {
        // In front of everything of the dock (it lies inside the bar, under no label).
        if (const HRESULT hr = renderer_.CreateLayer(scrollThumb_); FAILED(hr)) return hr;
        renderer_.StopOpacityAnimation(scrollThumb_, 0.0f);
        thumbOpacity_ = Tween{0.0f, 0.0f, Renderer::Now(), 0.0, true};
        thumbLen_ = 0.0f;
    }
    const float s = l.scale;
    const float thick = std::max(2.0f, std::round(kThumbDip * s));
    const float len = std::round(std::clamp(v.Length() * v.Length() / std::max(1.0f, v.content), kThumbMinDip * s, v.Length()));
    if (len == thumbLen_ && scrollThumb_.width) return S_OK;
    thumbLen_ = len;
    thumbMain_.valid = false;  // placed again (its cross offset too)
    const uint32_t c = DockTheme().labelText;
    const D2D1_COLOR_F color = Theme::ToColor((c & 0x00FFFFFFu) | 0x8C000000u);
    return DrawCanonical(scrollThumb_, len, thick, [&](ID2D1DeviceContext* dc) {
        winrt::com_ptr<ID2D1SolidColorBrush> brush;
        if (FAILED(dc->CreateSolidColorBrush(color, brush.put()))) return;
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.0f, 0.0f, len, thick), thick / 2.0f, thick / 2.0f),
                                 brush.get());
    });
}

void DockWindow::PlaceScrollThumb(double duration) {
    const Layout& l = layout_;
    if (!scrollThumb_.visual || !l.view.on || !hintShown_) return;
    IDCompositionVisual2* visual = scrollThumb_.visual.get();
    const DockGeometry::Viewport& v = l.view;
    if (!thumbMain_.valid) {
        const float thick = std::max(2.0f, std::round(kThumbDip * l.scale));
        SetCrossOffset(visual, l.CrossOrigin(l.bar.top + std::round(kThumbInsetDip * l.scale), thick));
    }
    const float track = std::max(0.0f, v.Length() - thumbLen_);
    const float u = std::round(v.start + (v.max > 0.0f ? track * scroll_ / v.max : 0.0f));
    renderer_.Drive(
        thumbMain_, u, duration, Renderer::Now(), [&](auto x) { return SetMainOffset(visual, x); }, 0.01f,
        Easing::EaseOut);
}

void DockWindow::ShowScrollHint() {
    // Called inside a RenderWithRecovery step (the caller commits). A ring has no thumb.
    if (!layout_.view.on || layout_.polar.on || FAILED(RenderScrollThumb()) || !scrollThumb_.visual) return;
    if (!hintShown_) {
        hintShown_ = true;
        thumbMain_.valid = false;
        PlaceScrollThumb(0.0);  // appears where the run is now heading
        winrt::com_ptr<IDCompositionVisual3> v3;
        if (SUCCEEDED(scrollThumb_.visual->QueryInterface(IID_PPV_ARGS(v3.put())))) {
            renderer_.Drive(
                thumbOpacity_, 1.0f, kHintFadeInS * motion_.scale, Renderer::Now(),
                [&](auto o) { return v3->SetOpacity(o); }, 0.001f, renderer_.FadeEasing());
        }
    }
    hintPending_ = SetTimer(hwnd_, kScrollHintTimerId, kScrollHintMs, nullptr) != 0;  // restarts it
}

void DockWindow::OnScrollHintTimer() {
    KillTimer(hwnd_, kScrollHintTimerId);
    hintPending_ = false;
    if (scrollDrag_ || autoScrollPending_) {  // still scrolling: keep it
        hintPending_ = SetTimer(hwnd_, kScrollHintTimerId, kScrollHintMs, nullptr) != 0;
        return;
    }
    if (!hintShown_ || !scrollThumb_.visual) return;
    hintShown_ = false;
    RenderWithRecovery([this] {
        winrt::com_ptr<IDCompositionVisual3> v3;
        if (SUCCEEDED(scrollThumb_.visual->QueryInterface(IID_PPV_ARGS(v3.put())))) {
            renderer_.Drive(
                thumbOpacity_, 0.0f, kHintFadeOutS * motion_.scale, Renderer::Now(),
                [&](auto o) { return v3->SetOpacity(o); }, 0.001f, renderer_.FadeEasing());
        }
        return S_OK;
    });
}

// --- Rings (docks[].shape "arc" / "circle", DockGeometry::Polar) -------------------------------

namespace {
// The band of a ring sector [0, arcLen] between radii rIn and rOut as a polygon (2 degree steps;
// a full circle: the outer loop, then the inner one backwards, filled even-odd), offset by (ox, oy).
std::vector<POINT> RingPolygon(const DockGeometry::Polar& p, float arcLen, float rIn, float rOut, float ox, float oy) {
    const float a0 = p.Angle(0.0f), a1 = p.Angle(arcLen);
    const int steps = std::clamp(static_cast<int>(std::ceil(std::fabs(a1 - a0) / (kPi / 90.0f))), 8, 720);
    std::vector<POINT> pts;
    pts.reserve(2 * static_cast<size_t>(steps) + 2);
    for (int i = 0; i <= steps; ++i) {
        const float a = a0 + (a1 - a0) * static_cast<float>(i) / static_cast<float>(steps);
        pts.push_back(POINT{std::lround(p.cx + ox + rOut * std::cos(a)), std::lround(p.cy + oy - rOut * std::sin(a))});
    }
    for (int i = steps; i >= 0; --i) {
        const float a = a0 + (a1 - a0) * static_cast<float>(i) / static_cast<float>(steps);
        pts.push_back(POINT{std::lround(p.cx + ox + rIn * std::cos(a)), std::lround(p.cy + oy - rIn * std::sin(a))});
    }
    return pts;
}

HRGN RingRegion(const DockGeometry::Polar& p, float arcLen, float rIn, float rOut, float ox, float oy) {
    const std::vector<POINT> pts = RingPolygon(p, arcLen, rIn, rOut, ox, oy);
    return CreatePolygonRgn(pts.data(), static_cast<int>(pts.size()), ALTERNATE);
}
}  // namespace

RECT DockWindow::RingScreenRect() const {
    const Layout& l = layout_;
    return RECT{l.window.left + static_cast<LONG>(std::floor(l.ring.left)), l.window.top + static_cast<LONG>(std::floor(l.ring.top)),
                l.window.left + static_cast<LONG>(std::ceil(l.ring.right)), l.window.top + static_cast<LONG>(std::ceil(l.ring.bottom))};
}

void DockWindow::UpdateWindowRegion() {
    if (!hwnd_) return;
    const Layout& l = layout_;
    const bool want = l.polar.on && !expanded_ && hideState_ == HideState::Shown;
    if (!want) {
        if (regionSet_) SetWindowRgn(hwnd_, nullptr, FALSE);
        regionSet_ = false;
        return;
    }
    HRGN rgn = RingRegion(l.polar, l.bar.right, l.ringIn - 1.0f, l.ringOut + 1.0f,
                          static_cast<float>(l.window.left - winRect_.left), static_cast<float>(l.window.top - winRect_.top));
    if (rgn && SetWindowRgn(hwnd_, rgn, FALSE)) {
        regionSet_ = true;  // the system owns the region now
    } else if (rgn) {
        DeleteObject(rgn);
    }
}

void DockWindow::ApplyIconTurn(DockItem& item) {
    // About the icon box's centre; the magnified-size surface keeps its scale to the rest size.
    const float w = BoxW(item), h = layout_.iconPx;
    const D2D1_POINT_2F c = D2D1::Point2F(w / 2.0f, h / 2.0f);
    const D2D1_MATRIX_3X2_F turn = D2D1::Matrix3x2F::Rotation(item.turn, c);
    renderer_.SetStaticTransform(item.icon, turn);
    if (item.iconBig.width > 0 && w > 0.0f) {
        const float k = static_cast<float>(item.iconBig.width) / w;
        renderer_.SetStaticTransform(item.iconBig, D2D1::Matrix3x2F::Scale(1.0f / k, 1.0f / k) * turn);
    }
}

void DockWindow::PlaceOnRing(DockItem& item, double now, bool fresh) {
    const Layout& l = layout_;
    const DockGeometry::Polar& p = l.polar;
    const float w = BoxW(item), h = BoxCross(item);
    const float vc = item.y + h / 2.0f;              // the box's centre line (the icons': the ring's radius)
    const float vDot = l.dotTop + l.dotCenterY;      // the dots: inside the ring, toward its centre
    const Tween& t = item.tx;
    IDCompositionVisual2* widget = item.IsWidget() ? item.layer.visual.get() : nullptr;
    IDCompositionTranslateTransform* translate = widget ? nullptr : item.layer.translate.get();
    IDCompositionVisual2* dot = widget ? nullptr : item.dot.visual.get();
    if (fresh && !widget) {
        // Scale about the side facing the centre, so a magnified icon grows outward; turned with
        // rotateIcons (its top facing outward). Both for the angle it is placed at.
        const float uc = t.to + w / 2.0f;
        const D2D1_POINT_2F o = p.Outward(uc);
        renderer_.EnsurePlacement(item.layer, w / 2.0f - o.x * h / 2.0f, h / 2.0f - o.y * h / 2.0f);
        if (item.dotShift.x != 0.0f || item.dotShift.y != 0.0f) {
            renderer_.SetStaticTransform(item.dot, D2D1::Matrix3x2F::Identity());
            item.dotShift = D2D1_POINT_2F{};
        }
        const float turn = l.rotateIcons ? 90.0f - p.Angle(uc) * 180.0f / kPi : 0.0f;
        if (turn != item.turn) {
            item.turn = turn;
            ApplyIconTurn(item);
        }
    }
    // Along the arc: each coordinate is a sampled curve of the slot's u curve (short moves: one
    // piece; a long glide around the ring: up to 8).
    const bool running = t.Running(now);
    const bool linear = t.easing == Easing::Linear;
    const int pieces = std::clamp(static_cast<int>(std::ceil(std::fabs(t.to - t.from) / p.R / (kPi / 16.0f))), 1, 8);
    auto drive = [&](auto&& f, auto&& setter) {
        if (!running) {
            setter(f(t.to));
            return;
        }
        DockGeometry::CubicSegment segs[8];
        const int n = DockGeometry::PathSegments(t.from, t.to, t.duration, linear, now - t.start, pieces, f, segs, 8);
        winrt::com_ptr<IDCompositionAnimation> anim = renderer_.CreateAnimation();
        if (!anim) {
            setter(f(t.to));
            return;
        }
        for (int i = 0; i < n; ++i) anim->AddCubic(segs[i].begin, segs[i].c0, segs[i].c1, segs[i].c2, segs[i].c3);
        anim->End(std::max(0.0, t.duration - (now - t.start)), f(t.to));
        setter(anim.get());
    };
    auto boxX = [&](float u) { return p.Point(u + w / 2.0f, vc).x - w / 2.0f; };
    auto boxY = [&](float u) { return p.Point(u + w / 2.0f, vc).y - h / 2.0f; };
    if (widget) {
        drive(boxX, [&](auto v) { return widget->SetOffsetX(v); });
        drive(boxY, [&](auto v) { return widget->SetOffsetY(v); });
        return;
    }
    if (translate) {
        drive(boxX, [&](auto v) { return translate->SetOffsetX(v); });
        drive(boxY, [&](auto v) { return translate->SetOffsetY(v); });
    }
    if (dot) {
        const float dw = l.iconPx, dh = static_cast<float>(l.dotHeight);
        drive([&](float u) { return p.Point(u + w / 2.0f, vDot).x - dw / 2.0f; }, [&](auto v) { return dot->SetOffsetX(v); });
        drive([&](float u) { return p.Point(u + w / 2.0f, vDot).y - dh / 2.0f; }, [&](auto v) { return dot->SetOffsetY(v); });
    }
}

void DockWindow::SetEdgeOpacity(DockItem& item, float value, IDCompositionAnimation* anim) {
    // Round 11 phase 11A: the slot button too, unless its attention pulse owns its opacity.
    for (Layer* layer : {&item.layer, &item.dot, &item.caption, &item.button}) {
        if (!layer->visual || (layer == &item.button && item.buttonPulse)) continue;
        if (anim) {
            renderer_.AnimateOpacity(*layer, anim);
        } else {
            renderer_.StopOpacityAnimation(*layer, value);
        }
    }
}

void DockWindow::DriveEdgeFade(DockItem& item, size_t index, double now) {
    const DockGeometry::Viewport& v = layout_.view;
    // Another opacity owns the icon container now (a launch pulse, the dragged icon, the fade of
    // an icon dragged off): leave it; the edge fade is set again afterwards.
    const bool pulsing = item.bounce != DockItem::Bounce::None && item.bounceStyle == LaunchStyle::Pulse;
    const bool dragged = dragPhase_ != DragPhase::None && static_cast<int>(index) == dragSource_;
    const bool fading = item.fade.valid && (item.fade.to != 1.0f || item.fade.Running(now));
    if (pulsing || dragged || fading) {
        if (v.on || item.edgeOpacity != 1.0f) item.edgeOpacity = -1.0f;
        return;
    }
    if (!v.on) {
        // Not scrolling: nothing is touched unless an earlier (scrolling) layout faded the slot.
        if (item.edgeOpacity != 1.0f) {
            SetEdgeOpacity(item, 1.0f, nullptr);
            item.edgeOpacity = 1.0f;
        }
        return;
    }
    const Tween& t = item.tx;
    const float half = BoxW(item) / 2.0f;
    if (!t.valid || !t.Running(now)) {
        const float o = v.Opacity(t.to + half);
        if (o != item.edgeOpacity) {
            SetEdgeOpacity(item, o, nullptr);
            item.edgeOpacity = o;
        }
        return;
    }
    const Tween& f = item.edgeFadeFor;
    if (item.edgeOpacity < 0.0f && item.edgeFadeSerial == fadeSerial_ && f.valid && f.start == t.start && f.to == t.to &&
        f.from == t.from && f.duration == t.duration) {
        return;  // already built for this curve and these ends
    }
    DockGeometry::CubicSegment segs[5];
    const int n = DockGeometry::FadeSegments(v, t.from, t.to, t.duration, t.easing == Easing::Linear, half,
                                             now - t.start, segs);
    item.edgeFadeFor = t;
    item.edgeFadeSerial = fadeSerial_;
    if (n == 1 && segs[0].c1 == 0.0f && segs[0].c2 == 0.0f && segs[0].c3 == 0.0f) {
        const float o = segs[0].c0;  // constant along the whole way
        if (o != item.edgeOpacity) {
            SetEdgeOpacity(item, o, nullptr);
            item.edgeOpacity = o;
        }
        return;
    }
    winrt::com_ptr<IDCompositionAnimation> anim = renderer_.CreateAnimation();
    const float end = v.Opacity(t.to + half);
    if (!anim) {
        SetEdgeOpacity(item, end, nullptr);
        item.edgeOpacity = end;
        return;
    }
    for (int i = 0; i < n; ++i) anim->AddCubic(segs[i].begin, segs[i].c0, segs[i].c1, segs[i].c2, segs[i].c3);
    anim->End(std::max(0.0, t.duration - (now - t.start)), end);
    SetEdgeOpacity(item, end, anim.get());
    item.edgeOpacity = -1.0f;
}

HRESULT DockWindow::DrawFrame(bool all) {
    HRESULT hr = S_OK;
    const bool snap = all || snapPending_;
    // Surface sizes are physical (main / cross swap on vertical docks).
    const D2D1_SIZE_F bgSize = layout_.PhysSize(static_cast<float>(static_cast<UINT>(layout_.bgSurfaceW)),
                                                static_cast<float>(static_cast<UINT>(layout_.bar.bottom - layout_.bar.top)));
    const D2D1_SIZE_F dotSize = layout_.PhysSize(static_cast<float>(static_cast<UINT>(layout_.iconPx)),
                                                 static_cast<float>(layout_.dotHeight));
    const D2D1_SIZE_F ringSize = D2D1::SizeF(std::ceil(layout_.ring.right) - std::floor(layout_.ring.left),
                                             std::ceil(layout_.ring.bottom) - std::floor(layout_.ring.top));
    const D2D1_SIZE_F bgWant = layout_.polar.on ? ringSize : bgSize;
    if (all || bgLeft_.width != static_cast<UINT>(bgWant.width) || bgLeft_.height != static_cast<UINT>(bgWant.height) ||
        bgSeparators_[0] != layout_.separatorX || bgSeparators_[1] != layout_.leftSeparatorX || bgRing_ != layout_.polar.on) {
        bgRing_ = layout_.polar.on;
        hr = RenderBackground();
    }
    if (SUCCEEDED(hr) && (all || dotSources_[0].width != static_cast<UINT>(dotSize.width) ||
                          dotSources_[0].height != static_cast<UINT>(dotSize.height))) {
        hr = RenderDotSources();
    }
    for (DockItem& item : items_) {
        if (FAILED(hr)) break;
        if (!item.layer.visual) hr = CreateItemVisuals(item);
        if (SUCCEEDED(hr) && (all || item.dirty)) {
            hr = RenderItem(item);
            if (SUCCEEDED(hr) && item.turn != 0.0f && !item.IsWidget()) ApplyIconTurn(item);  // rotateIcons
        }
        if (SUCCEEDED(hr) && (all || item.dotDirty) && !item.IsWidget()) hr = RenderDot(item);  // widgets: no dot
        // Notification badges (only slots that show or showed one).
        if (SUCCEEDED(hr) && item.Magnifies() && (item.badgeDirty || (all && (item.badgeCount > 0 || item.badge.visual)))) {
            hr = RenderBadge(item);
        }
        // Titles under the icons (none while off: the caption visual only exists while shown).
        if (SUCCEEDED(hr) && !item.IsWidget() && (all || item.captionDirty)) hr = RenderCaption(item);
        // Round 11 phase 11A: a new layout or theme draws every slot button again (RenderButtons,
        // from ApplyTargets below).
        if (all || snap) item.buttonDrawn = -1;
    }
    if (SUCCEEDED(hr) && labelShown_ && (all || labelDirty_)) hr = RenderLabel();
    const UINT titleW = static_cast<UINT>(layout_.title.right - layout_.title.left);
    if (SUCCEEDED(hr) && (all || titleDirty_ || title_.width != titleW)) hr = RenderTitle();
    if (FAILED(hr)) return hr;

    if (snap) {
        snapPending_ = false;
        ResetPlacementTweens();
        ApplyPerspective();  // its centre follows the stage size and the bar
        ApplyReflectionClip();
        if (title_.visual) {
            title_.visual->SetOffsetX(layout_.title.left);
            title_.visual->SetOffsetY(layout_.title.top);
        }
        if (scrollThumb_.visual) {
            // The viewport may have changed: the thumb is re-drawn / re-placed (or goes).
            thumbMain_.valid = false;
            if (!layout_.view.on) {
                renderer_.StopOpacityAnimation(scrollThumb_, 0.0f);
                thumbOpacity_ = Tween{0.0f, 0.0f, Renderer::Now(), 0.0, true};
                hintShown_ = false;
            } else if (hintShown_) {
                hr = RenderScrollThumb();
                PlaceScrollThumb(0.0);
                if (FAILED(hr)) return hr;
            }
        }
    }
    if (!rootY_.valid) ApplyRootPose(0.0);
    ComputeTargets();
    ApplyTargets(snap ? 0.0 : motion_.magnify, true);
    return S_OK;
}

void DockWindow::RenderAll() {
    RenderWithRecovery([this] { return DrawFrame(true); });
}

void DockWindow::RenderWithRecovery(const std::function<HRESULT()>& step) {
    HRESULT hr = renderer_.CheckDevice();
    if (SUCCEEDED(hr)) hr = step();
    if (SUCCEEDED(hr)) hr = renderer_.Commit();
    if (FAILED(hr)) {
        if (!Renderer::IsDeviceLost(hr)) {
            Log(L"render", hr);
        } else if (!RecoverDevice()) {
            Log(L"device recovery", hr);
        }
    }
    SyncBackdrop();  // after the commit: the blur follows what was just sent to DWM
}

bool DockWindow::RecoverDevice() {
    // Device removed/reset: rebuild the whole chain and every surface. Retry a few times
    // because a driver update can keep the adapter unavailable for a moment.
    DismissPreview(false, false);
    DismissClickPopups(false, false);
    DismissWidgetPassive(false);
    preview_.Destroy();  // its composition target belongs to the lost device; recreated on demand
    folder_.Destroy();
    weatherPopup_.Destroy();
    calendar_.Destroy();
    reminder_.Destroy();
    widgetPopup_.Destroy();
    widgetPassive_.Destroy();
    centre_.Destroy();
    thumbs_.Reset();  // the frames and the capture device belong to the lost device
    for (int attempt = 0; attempt < 3; ++attempt) {
        ResetLayers();
        icons_.Clear();  // the bitmaps belong to the lost device
        trayBitmaps_.clear();
        trayBigBitmaps_.clear();
        labelFormat_ = nullptr;
        labelEllipsis_ = nullptr;
        HRESULT hr = renderer_.RecreateDevice();
        if (SUCCEEDED(hr)) hr = CreateLayers();
        if (SUCCEEDED(hr)) hr = DrawFrame(true);
        if (SUCCEEDED(hr)) hr = renderer_.Commit();
        if (SUCCEEDED(hr)) {
            SyncThumbnails();  // capture again on the new device
            return true;
        }
        Log(L"RecreateDevice", hr);
        Sleep(100);
    }
    return false;
}

void DockWindow::DispatchEvent(DockEvent e) {
    for (size_t i = 0; i < elements_.size(); ++i) {
        if (!elements_[i] || !elements_[i]->OnEvent(e)) continue;
        for (DockItem& item : items_) {
            if (item.element == i) item.dirty = true;
        }
    }
    RefreshDirtyWidgets();
}

void DockWindow::RefreshDirtyWidgets() {
    // ApplyConfig / ApplyWeather: their relayout measures and draws the dirty slots.
    if (!hwnd_ || inLayout_ || weatherQuiet_) return;
    // Re-measure the changed widgets: a new size needs a relayout (the bar width depends on it),
    // otherwise only their slots are redrawn.
    const LayoutContext ctx = Context();
    bool resized = false, any = false, hoverGrew = false;
    for (DockItem& item : items_) {
        if (!item.dirty || !item.IsWidget()) continue;
        any = true;
        const ElementSize box = elements_[item.element]->Measure(ctx);
        if (box.main != item.slotW || box.cross != item.boxCross || (box.flush && layout_.fullWidth) != item.flush) resized = true;
        // 9A follow-up 2: the widget under the pointer grows within a second of the pointer coming
        // onto it (the analog clock's hoverScale, any script widget's measure() growing in its
        // onHover): it zooms, so a "steady" bar grows as for a magnifying icon.
        if (!hoverWidget_.id.empty() && item.key == hoverWidget_ && Renderer::Now() - hoverWidgetSince_ < kHoverGrowWindowS &&
            (box.main > item.slotW + 0.5f || box.cross > item.boxCross + 0.5f)) {
            hoverGrew = true;
        }
    }
    if (!any) return;
    if (hoverGrew && layout_.steady && !layout_.always && !grown_ && magnifying_ && hideState_ == HideState::Shown) {
        grown_ = true;  // the relayout below (it resized) lays the grown bar out
    }
    if (resized) {
        Relayout();
        return;
    }
    RenderWithRecovery([this] {
        HRESULT hr = S_OK;
        for (DockItem& item : items_) {
            if (SUCCEEDED(hr) && item.dirty && item.IsWidget() && item.layer.visual) hr = RenderWidget(item);
        }
        return hr;
    });
}

// --- Placement and magnification -------------------------------------------------------------
// Scales come from the cursor's distance to each item's REST slot centre (no feedback from the
// magnified layout): scale = 1 + (mag - 1) * f(d), f(d) = (1 + cos(pi d / R)) / 2 (or, with
// magnifyFalloff "linear", 1 - d / R) for d < R, R = magnifyRange slots. Magnified slots are laid
// out left to right, and the run is anchored so the point under the cursor stays under the
// cursor: the rest-layout offset u of the cursor in the bar maps to M(u) in the magnified bar
// (lead padding and spacers unscaled, icon slots stretched, separators and widgets moved by the
// growth before them), and the magnified bar starts at cursorX - M(u). So the bar grows away from
// the cursor on both sides (macOS behaviour) and the hovered icon stays under the pointer. Widgets
// before the first / after the last icon move with the bar's left / right end. Hit testing reads
// these targets (HitAlong; 9A follow-up), so it matches what is drawn also where the run cannot
// stay anchored (a grown bar's room used up, a monitor end).

void DockWindow::ComputeTargets() {
    const Layout& l = layout_;
    const size_t n = items_.size();
    const float stride = l.iconPx + l.gap;  // an icon's slot
    const float half = std::floor(l.gap / 2.0f);
    // 9A follow-up 2: a "steady" bar not grown yet (the pointer only over widgets, separators or
    // the bar's padding so far) magnifies nothing: the icons zoom together with the bar's growth.
    // Round 12 phase 12D: customize mode pauses it (the badges and "+" slots sit on the rest layout).
    const bool magnify = magnifying_ && !editing_ && l.magnification > 1.001f && n > 0 && (!l.steady || GrownBar());
    const float range = static_cast<float>(DockCfg().magnifyRange) * stride;

    // Dragging an icon: no magnification; the others make room at the insertion slot and the
    // dragged icon follows the cursor. A file dragged over the dock opens a gap (the bar grows by
    // one slot, half on each side). Slots keep their own widths (separators are narrow, widgets
    // their content), so the targets come from walking the new order.
    const int src = dragSource_;
    const bool dragLayout = (dragPhase_ != DragPhase::None || dragLanding_) && src >= 0 &&
                            src < static_cast<int>(n) && items_[static_cast<size_t>(src)].key == dragKey_ &&
                            dragInsert_ >= 0 && dragOrder_.size() == n;
    const bool gapLayout = !dragLayout && dropGap_ >= 0 && n > 0;
    // A scrolling run fades at an end with content beyond it: scrolled there, or pushed past it
    // (magnified, a drop gap opened). The bar itself never grows.
    auto viewEnds = [&](bool pushedStart, bool pushedEnd) {
        if (!l.view.on) return;
        const bool fs = scroll_ > 0.5f || pushedStart;
        const bool fe = scroll_ < l.view.max - 0.5f || pushedEnd;
        if (fs == layout_.view.fadeStart && fe == layout_.view.fadeEnd) return;
        layout_.view.fadeStart = fs;
        layout_.view.fadeEnd = fe;
        ++fadeSerial_;
    };
    if (dragLayout || gapLayout) {
        const float shiftLeft = gapLayout ? std::floor(stride / 2.0f) : 0.0f;
        const float shiftRight = gapLayout ? stride - shiftLeft : 0.0f;
        float slotLeft = l.itemsLeft - shiftLeft;
        auto place = [&](size_t i, float lead) {
            DockItem& item = items_[i];
            slotLeft += lead;
            const float left = slotLeft;
            slotLeft += item.slotW;
            if (dragLayout && static_cast<int>(i) == src) {
                if (dragPhase_ == DragPhase::Removing) return;  // fades out where it was dropped
                if (dragPhase_ == DragPhase::Dragging) {
                    const float winW = l.stageMain;
                    item.targetScale = kDragScale;
                    item.targetX = std::clamp(cursorX_ - dragGrabDx_, 0.0f, std::max(0.0f, winW - BoxW(item)));
                    return;
                }
                // landing: into its slot like the others
            }
            item.targetScale = 1.0f;
            item.targetX = left + BoxInset(item);
        };
        if (dragLayout) {
            // The slots in the drag order, with the spacers that order gets at rest (a widget
            // moved between icons, a running app pinned across a widget: the spacers move with
            // them), the run centred where the rest run is. A reorder that keeps every slot's
            // kind of neighbour (pins among pins) keeps every rest spacer and position.
            const float sepW = std::round(kSeparatorWidth * l.scale);
            const float widgetGap = std::round(kWidgetGap * l.scale);
            std::vector<float> leads(n, 0.0f);
            // Round 7 phase 7B: an anchored layout shows the drag order as groups (the dragged item
            // in the group it would drop into), from the bar's start padding.
            std::vector<AnchorSlot> slots;
            if (l.anchored) slots = MakeAnchorSlots(items_, dragOrder_, DockCfg(), l.scale, src, dragGroup_);
            if (l.anchored && AnchoredLeads(slots, l.bar.left, l.bar.right, l.padding, half, sepW, widgetGap, leads)) {
                const auto first = std::find_if(slots.begin(), slots.end(), [](const AnchorSlot& a) { return a.run != Run::None; });
                slotLeft = l.bar.left + (first != slots.end() ? EndPadding(*first, l.padding, half) : l.padding);
            } else {
                leads.assign(n, 0.0f);
                float width = 0.0f;
                Run prev = Run::None;
                // "always" (9A follow-up): the room for a magnified run stays before the first /
                // after the last magnifying slot of the order shown.
                int firstMagK = -1, lastMagK = -1;
                if (l.always) {
                    for (size_t k = 0; k < n; ++k) {
                        if (!items_[dragOrder_[k]].Magnifies()) continue;
                        if (firstMagK < 0) firstMagK = static_cast<int>(k);
                        lastMagK = static_cast<int>(k);
                    }
                }
                bool endPadded = lastMagK < 0;
                for (size_t k = 0; k < n; ++k) {
                    const DockItem& it = items_[dragOrder_[k]];
                    const Run run = !it.IsWidget() ? Run::Icon : it.slotW > 0.0f ? Run::Widget : Run::None;
                    if (run == Run::None) continue;
                    leads[k] = SpacerBetween(prev, run, sepW, widgetGap);
                    if (static_cast<int>(k) == firstMagK) leads[k] += l.steadyStart;
                    if (!endPadded && static_cast<int>(k) > lastMagK) {
                        leads[k] += l.steadyEnd;
                        endPadded = true;
                    }
                    prev = run;
                    width += leads[k] + it.slotW;
                }
                // (The rest run holds the end room in a lead, or in the bar's trail: centre alike.)
                if (lastMagK >= 0) {
                    if (endPadded) width -= l.steadyEnd;
                    if (l.padEndSlot >= 0) width += l.steadyEnd;
                }
                slotLeft += std::round((l.itemsW - width) / 2.0f);
            }
            for (size_t k = 0; k < n; ++k) place(dragOrder_[k], leads[k]);
        } else {
            for (size_t i = 0; i <= n; ++i) {
                float lead = i < n ? items_[i].lead : 0.0f;
                if (static_cast<int>(i) == dropGap_) {
                    // The gap is an icon slot: before an icon it takes that icon's spacer (none
                    // between icons); a widget after it keeps its own.
                    const bool icon = i < n && !items_[i].IsWidget();
                    slotLeft += (icon ? lead : 0.0f) + stride;
                    if (icon) lead = 0.0f;
                }
                if (i < n) place(i, lead);
            }
        }
        float barStart = l.view.on ? 0.0f : shiftLeft;
        float barEnd = l.view.on ? 0.0f : shiftRight;
        if (GrownBar()) {
            // 9A follow-up: a drag (or a drop gap) on the grown bar ("steady" while grown_, or
            // "always"): the slots before the first / after the last magnifying slot of the order
            // shown stay with the grown bar's ends, a gap's push goes into the room beside the run
            // first (the bar grows further only by what does not fit).
            const float outS = GrownOutStart(), outE = GrownOutEnd();
            const float padS = l.steadyStart, padE = l.steadyEnd;
            const float moveS = std::min(shiftLeft, padS) - outS;   // added to the walked targets before the run
            const float moveE = outE - std::min(shiftRight, padE);  // ... and after it
            int firstK = -1, lastK = -1;
            for (size_t k = 0; k < n; ++k) {
                const size_t i = dragLayout ? dragOrder_[k] : k;
                if (!items_[i].Magnifies()) continue;
                if (firstK < 0) firstK = static_cast<int>(k);
                lastK = static_cast<int>(k);
            }
            for (size_t k = 0; k < n && firstK >= 0; ++k) {
                const size_t i = dragLayout ? dragOrder_[k] : k;
                if (dragLayout && static_cast<int>(i) == src && dragPhase_ != DragPhase::None) continue;  // follows the cursor / fades
                if (static_cast<int>(k) < firstK) items_[i].targetX += moveS;
                if (static_cast<int>(k) > lastK) items_[i].targetX += moveE;
            }
            barStart = outS + std::max(0.0f, shiftLeft - padS);
            barEnd = outE + std::max(0.0f, shiftRight - padE);
        }
        bgLeftTarget_ = l.bar.left - barStart;
        bgRightTarget_ = l.bar.right + barEnd - l.bgSurfaceW;
        viewEnds(gapLayout, gapLayout);
        return;
    }

    float delta = 0.0f;  // total growth of the run
    for (DockItem& item : items_) {
        float scale = 1.0f;
        if (magnify && item.Magnifies()) {  // separators and widgets never magnify
            const float centre = item.slotX + item.slotW / 2.0f;
            const float d = std::fabs(cursorX_ - centre);
            if (d < range) {
                const float f = motion_.falloff == Falloff::Linear ? 1.0f - d / range
                                                                   : 0.5f * (1.0f + std::cos(kPi * d / range));
                scale = 1.0f + (l.magnification - 1.0f) * f;
            }
        }
        item.targetScale = scale;
        delta += BoxW(item) * (scale - 1.0f);  // separators and widgets: scale 1
    }
    // Width of an item's slot in the magnified layout (an icon or a thumbnail tile scales).
    auto magnifiedW = [&](const DockItem& item) {
        return item.Magnifies() ? BoxW(item) * item.targetScale + l.gap : item.slotW;
    };

    float growLeft = 0.0f;
    if (magnify) {
        const float u = cursorX_ - l.bar.left;
        const float lead = l.itemsLeft - l.bar.left;
        float mapped = u + delta;  // right of every icon: the whole growth is left of the cursor
        if (u <= lead) {
            mapped = u;
        } else if (u < lead + l.itemsW) {
            float at = lead;  // bar-local left edge of slot j in the magnified layout
            for (size_t j = 0; j < n; ++j) {
                const DockItem& it = items_[j];
                if (static_cast<int>(j) > l.lastMag && it.IsWidget()) break;  // nothing scales from here on
                at += it.lead;
                if (cursorX_ < it.slotX) {  // in the (unscaled) spacer before it
                    mapped = at - (it.slotX - cursorX_);
                    break;
                }
                if (cursorX_ < it.slotX + it.slotW) {
                    const float frac = std::clamp((cursorX_ - it.slotX) / std::max(1.0f, it.slotW), 0.0f, 1.0f);
                    mapped = at + frac * magnifiedW(it);
                    break;
                }
                at += magnifiedW(it);
            }
        }
        growLeft = std::clamp(mapped - u, 0.0f, delta);
        // At a monitor end the stage is cut: the bar grows away from it (the icon under the
        // cursor then drifts off the pointer by what did not fit). (A grown bar's room already
        // respects the stage: below.)
        if (!GrownBar()) {
            if (delta - growLeft > l.growRoomEnd) growLeft = delta - l.growRoomEnd;
            growLeft = std::clamp(growLeft, 0.0f, std::max(0.0f, l.growRoomStart));
        }
    }
    // How far the bar's ends move out: with the run (follow), or (round 9 phase 9A, steady) by the
    // layout's fixed amounts. 9A follow-up: from the first time something zooms under the pointer
    // (follow-up 2: GrowForZoom) until it leaves (grown_; a menu, a popup or a drag holding the dock
    // keeps it), whether or not anything magnifies meanwhile, so the widgets and separators at the
    // ends move once, never while the pointer walks the dock; "always": the resting bar has that room already and its ends never move. The run
    // slides inside the room (padS / padE beside it), anchored at the pointer while it fits; where
    // it does not, the hovered icon drifts off the pointer (the hit test follows the targets) and
    // only where the run's growth exceeds both rooms (both cut by the stage) the bar grows further.
    float barStart = growLeft;
    float barEnd = delta - growLeft;
    if (GrownBar()) {
        const float padS = l.steadyStart, padE = l.steadyEnd;
        growLeft = std::max(std::min(growLeft, padS), delta - padE);
        growLeft = std::clamp(growLeft, 0.0f, delta);
        barStart = GrownOutStart() + std::max(0.0f, growLeft - padS);
        barEnd = GrownOutEnd() + std::max(0.0f, delta - growLeft - padE);
    }
    const float growRight = delta - growLeft;

    float slotLeft = l.itemsLeft - growLeft;
    for (size_t i = 0; i < n; ++i) {
        DockItem& item = items_[i];
        const int k = static_cast<int>(i);
        slotLeft += item.lead;
        if (item.Magnifies()) {
            // The icon box keeps its rest size and is scaled about its bottom centre, so its left
            // edge sits half an icon left of the magnified icon's centre.
            const float w = BoxW(item) * item.targetScale;
            item.targetX = slotLeft + half + w / 2.0f - BoxW(item) / 2.0f;
        } else if (k > l.lastMag) {
            // After the last icon (widgets, a separator before them): moves with the bar's right end
            // (follow: the run's end, the same place; steady: the slack stays before them).
            item.targetX = (item.IsWidget() ? item.x : item.slotX) + barEnd;
        } else if (k < l.firstMag) {
            item.targetX = (item.IsWidget() ? item.x : item.slotX) - barStart;  // with the bar's left end
        } else {
            item.targetX = slotLeft;  // separators, widgets between icons
        }
        slotLeft += magnifiedW(item);
    }
    // A file dragged onto an app (open with) or the bin (recycle): that icon grows in place.
    if (dropItem_ >= 0 && dropItem_ < static_cast<int>(n) &&
        (dropMode_ == DropMode::OpenWith || dropMode_ == DropMode::Recycle || dropMode_ == DropMode::Group)) {
        items_[static_cast<size_t>(dropItem_)].targetScale = kDropScale;
    }
    // A dragged icon held over a pin / a group: the target grows (a release groups them).
    if (dragPhase_ == DragPhase::Dragging && groupTarget_ >= 0 && groupTarget_ < static_cast<int>(n)) {
        items_[static_cast<size_t>(groupTarget_)].targetScale = kGroupScale;
    }

    bgLeftTarget_ = l.bar.left - (l.view.on ? 0.0f : barStart);
    bgRightTarget_ = l.bar.right + (l.view.on ? 0.0f : barEnd) - l.bgSurfaceW;
    viewEnds(growLeft > 0.5f, growRight > 0.5f);

    if (labelShown_ && hovered_ >= 0 && hovered_ < static_cast<int>(n) && label_.width) {
        // Centred on the icon along the bar, kLabelGap beyond the magnified icon (above a bottom
        // dock, below a top dock, beside a vertical one, toward the middle of the screen). The
        // label stays upright: its main / cross lengths are its height / width on vertical docks.
        const DockItem& h = items_[static_cast<size_t>(hovered_)];
        const float winW = l.stageMain;
        const float lw = static_cast<float>(l.vertical ? label_.height : label_.width);
        const float lh = static_cast<float>(l.vertical ? label_.width : label_.height);
        const bool cell = hoverCell_ >= 0 && hoverCellSlot_ == hovered_;
        const float centre = h.targetX + (cell ? hoverCellCentre_ : BoxW(h) / 2.0f);
        const float iconTop = l.itemTop + l.iconPx * (1.0f - h.targetScale);
        if (l.polar.on) {
            // Outside the ring at the icon's angle, kLabelGap beyond the magnified icon.
            const D2D1_POINT_2F c = l.polar.Outside(centre, iconTop - std::round(kLabelGap * l.scale), lw, lh);
            labelXTarget_ = std::round(c.x - lw / 2.0f);
            labelYTarget_ = std::round(c.y - lh / 2.0f);
            return;
        }
        const float u = std::round(std::clamp(centre - lw / 2.0f, 0.0f, std::max(0.0f, winW - lw)));
        const float v = std::round(std::max(0.0f, iconTop - std::round(kLabelGap * l.scale) - lh));
        const D2D1_RECT_F r = l.PhysRect(u, v, u + lw, v + lh);
        labelXTarget_ = r.left;
        labelYTarget_ = r.top;
    }
}

void DockWindow::ApplyTargets(double duration, bool trackingMove) {
    const Layout& l = layout_;
    const double now = Renderer::Now();
    // While the mouse moves, skip retargets below a third of a pixel (fewer animations per
    // commit); settling at rest uses exact targets so icons land pixel-aligned. Tracking always
    // eases out (a spring restarted at every mouse move would jitter); settling uses the
    // configured easing.
    const bool tracking = trackingMove && duration > 0.0;
    const float posEpsilon = tracking ? 0.35f : 0.01f;
    const float scaleEpsilon = tracking ? 0.003f : 0.0005f;
    const std::optional<Easing> easing = tracking ? std::optional<Easing>(Easing::EaseOut) : std::nullopt;

    // Main-axis positions are driven (x, or y on vertical docks); cross-axis ones are static,
    // set after a snap (layout change) through CrossOrigin. A ring's background is one static
    // surface (it never grows).
    if (l.polar.on) {
        if (!bgLeftX_.valid && bgLeft_.visual) {
            if (bgLeftClip_.clip) {
                bgLeftClip_.visual->SetClip(nullptr);
                bgLeftClip_.clip = nullptr;
            }
            bgLeft_.visual->SetOffsetX(std::floor(l.ring.left));
            bgLeft_.visual->SetOffsetY(std::floor(l.ring.top));
            if (bgRight_.visual) renderer_.SetVisible(bgRight_, false);
            ringShown_ = true;
            bgLeftX_ = bgRightX_ = Tween{0.0f, 0.0f, now, 0.0, true};
        }
    } else if (!bgLeftX_.valid) {
        if (ringShown_ && bgRight_.visual) renderer_.SetVisible(bgRight_, true);  // after a ring
        ringShown_ = false;
        // Static parts, re-applied after a snap (layout change) or a background redraw. The seam
        // clips the halves on the main axis.
        renderer_.SetClip(bgLeftClip_, l.vertical ? D2D1::RectF(-1e5f, -1e5f, 1e5f, l.seamX)
                                                  : D2D1::RectF(-1e5f, -1e5f, l.seamX, 1e5f));
        renderer_.SetClip(bgRightClip_, l.vertical ? D2D1::RectF(-1e5f, l.seamX, 1e5f, 1e5f)
                                                   : D2D1::RectF(l.seamX, -1e5f, 1e5f, 1e5f));
        const float barCross = l.CrossOrigin(l.bar.top, l.bar.bottom - l.bar.top);
        if (bgLeft_.visual) SetCrossOffset(bgLeft_.visual.get(), barCross);
        if (bgRight_.visual) SetCrossOffset(bgRight_.visual.get(), barCross);
    }
    if (bgLeft_.visual && !l.polar.on) {
        renderer_.Drive(
            bgLeftX_, bgLeftTarget_, duration, now, [&](auto v) { return SetMainOffset(bgLeft_.visual.get(), v); },
            posEpsilon, easing);
    }
    if (bgRight_.visual && !l.polar.on) {
        renderer_.Drive(
            bgRightX_, bgRightTarget_, duration, now, [&](auto v) { return SetMainOffset(bgRight_.visual.get(), v); },
            posEpsilon, easing);
    }
    // A scrolling run: a slot whose whole path (where it is now -> its target) stays outside the
    // viewport is not touched; its tweens only follow (Renderer::Track) and it is put back on its
    // curve (Resume) once it comes near the viewport again.
    const float outMargin = l.iconPx + l.gap;
    auto scrolledOut = [&](const DockItem& item) {
        // Not for a visual that has not been placed since the last snap (a new slot, a layout
        // change): it is placed once, statically, so it never shows at its default offset.
        if (!l.view.on || !item.tx.valid) return false;
        const float from = item.tx.Value(now);
        return l.view.Outside(std::min(from, item.targetX), std::max(from, item.targetX) + BoxW(item), outMargin);
    };
    placedDriven_ = placedSkipped_ = 0;
    for (size_t index = 0; index < items_.size(); ++index) {
        DockItem& item = items_[index];
        if (l.polar.on) {
            // A ring: the slot's u follows its tween (bookkeeping), its visuals follow the ring.
            if (!item.layer.visual || (!item.IsWidget() && (!item.layer.translate || !item.layer.scale))) continue;
            if (scrolledOut(item)) {
                const bool moved = renderer_.Track(item.tx, item.targetX, duration, now, posEpsilon, easing);
                const bool scaled = renderer_.Track(item.ts, item.targetScale, duration, now, scaleEpsilon, easing);
                if (moved || scaled) item.placementStale = true;
                ++placedSkipped_;
                continue;
            }
            const bool fresh = !item.tx.valid || item.placementStale;
            const bool moved = renderer_.Track(item.tx, item.targetX, duration, now, posEpsilon, easing);
            if (fresh || moved) PlaceOnRing(item, now, fresh);
            if (!item.IsWidget()) {
                IDCompositionScaleTransform* scale = item.layer.scale.get();
                if (item.placementStale) {
                    renderer_.Resume(item.ts, now, [&](auto v) {
                        HRESULT hr = scale->SetScaleX(v);
                        if (SUCCEEDED(hr)) hr = scale->SetScaleY(v);
                        return hr;
                    });
                }
                renderer_.Drive(
                    item.ts, item.targetScale, duration, now,
                    [&](auto v) {
                        HRESULT hr = scale->SetScaleX(v);
                        if (SUCCEEDED(hr)) hr = scale->SetScaleY(v);
                        return hr;
                    },
                    scaleEpsilon, easing);
            }
            item.placementStale = false;
            ++placedDriven_;
            DriveEdgeFade(item, index, now);
            continue;
        }
        if (item.IsWidget() && item.layer.visual) {
            // A widget: the visual's own offsets. Its height can change without a snap (the first
            // weather data arriving): follow it here.
            IDCompositionVisual2* visual = item.layer.visual.get();
            const float cross = l.CrossOrigin(item.y, item.boxCross);
            if (item.appliedY != cross || !item.tx.valid) {
                SetCrossOffset(visual, cross);
                item.appliedY = cross;
            }
            auto setMain = [&](auto v) { return SetMainOffset(visual, v); };
            if (scrolledOut(item)) {  // (a visual never placed since the last snap is placed once below)
                if (renderer_.Track(item.tx, item.targetX, duration, now, posEpsilon, easing)) item.placementStale = true;
                ++placedSkipped_;
                continue;
            }
            if (item.placementStale) {
                renderer_.Resume(item.tx, now, setMain);
                item.placementStale = false;
            }
            renderer_.Drive(item.tx, item.targetX, duration, now, setMain, posEpsilon, easing);
            ++placedDriven_;
            DriveEdgeFade(item, index, now);
            continue;
        }
        if (!item.layer.visual || !item.layer.translate || !item.layer.scale) continue;
        if (!item.tx.valid) {
            const D2D1_POINT_2F foot = FootOf(item);
            renderer_.EnsurePlacement(item.layer, foot.x, foot.y);
            SetCrossOffset(item.layer.translate.get(), l.CrossOrigin(item.y, l.iconPx));
            if (item.dot.visual) {
                SetCrossOffset(item.dot.visual.get(), l.CrossOrigin(l.dotTop, static_cast<float>(l.dotHeight)));
                // The dot surface is an icon wide: centred under a longer thumbnail tile.
                const float shift = std::floor((BoxW(item) - l.iconPx) / 2.0f);
                const D2D1_POINT_2F want = l.vertical ? D2D1::Point2F(0.0f, shift) : D2D1::Point2F(shift, 0.0f);
                if (want.x != item.dotShift.x || want.y != item.dotShift.y) {
                    renderer_.SetStaticTransform(item.dot, D2D1::Matrix3x2F::Translation(want.x, want.y));
                    item.dotShift = want;
                }
            }
            if (item.caption.visual) {
                SetCrossOffset(item.caption.visual.get(), l.CrossOrigin(l.captionTop, l.captionCross));
            }
            if (item.button.visual && l.buttons) {
                // Round 11 phase 11A: across the bar inset; along it from the rest slot (inset) on,
                // a static offset from the icon box (the same tween drives both).
                SetCrossOffset(item.button.visual.get(), l.CrossOrigin(l.buttonTop, l.buttonH));
                item.buttonShift = l.buttonInsetMain - BoxInset(item);
                renderer_.SetStaticTransform(item.button, l.vertical ? D2D1::Matrix3x2F::Translation(0.0f, item.buttonShift)
                                                                     : D2D1::Matrix3x2F::Translation(item.buttonShift, 0.0f));
            }
        }
        IDCompositionTranslateTransform* translate = item.layer.translate.get();
        IDCompositionScaleTransform* scale = item.layer.scale.get();
        IDCompositionVisual2* dot = item.dot.visual.get();
        IDCompositionVisual2* caption = item.caption.visual.get();
        IDCompositionVisual2* button = l.buttons ? item.button.visual.get() : nullptr;
        // The dot and the title follow the icon box along the bar: one animation object drives all.
        // (The title does not scale: magnification grows the icon away from it.) So does a slot
        // button (round 11 phase 11A; it never scales either).
        auto setMain = [&](auto v) {
            HRESULT hr = SetMainOffset(translate, v);
            if (SUCCEEDED(hr) && dot) hr = SetMainOffset(dot, v);
            if (SUCCEEDED(hr) && caption) hr = SetMainOffset(caption, v);
            if (SUCCEEDED(hr) && button) hr = SetMainOffset(button, v);
            return hr;
        };
        auto setScale = [&](auto v) {
            HRESULT hr = scale->SetScaleX(v);
            if (SUCCEEDED(hr)) hr = scale->SetScaleY(v);
            return hr;
        };
        if (scrolledOut(item)) {
            const bool moved = renderer_.Track(item.tx, item.targetX, duration, now, posEpsilon, easing);
            const bool scaled = renderer_.Track(item.ts, item.targetScale, duration, now, scaleEpsilon, easing);
            if (moved || scaled) item.placementStale = true;
            ++placedSkipped_;
            continue;
        }
        if (item.placementStale) {
            renderer_.Resume(item.tx, now, setMain);
            renderer_.Resume(item.ts, now, setScale);
            item.placementStale = false;
        }
        renderer_.Drive(item.tx, item.targetX, duration, now, setMain, posEpsilon, easing);
        renderer_.Drive(item.ts, item.targetScale, duration, now, setScale, scaleEpsilon, easing);
        ++placedDriven_;
        DriveEdgeFade(item, index, now);
    }

    if (dropGap_ >= 0 && marker_.visual && marker_.width) {
        // Centred in the gap (the gap layout starts half a slot left of the rest layout; the gap is
        // an icon slot wide and, before an icon, comes after that icon's spacer: ComputeTargets).
        const float stride = l.iconPx + l.gap;
        float gapLeft = l.itemsLeft - std::floor(stride / 2.0f);
        const size_t gap = static_cast<size_t>(dropGap_);
        for (size_t i = 0; i < items_.size() && i < gap; ++i) gapLeft += items_[i].lead + items_[i].slotW;
        if (gap < items_.size() && !items_[gap].IsWidget()) gapLeft += items_[gap].lead;
        const float markerMain = static_cast<float>(l.vertical ? marker_.height : marker_.width);
        const float x = gapLeft + std::floor(l.gap / 2.0f) + std::round((l.iconPx - markerMain) / 2.0f);
        if (l.polar.on) {
            // Upright, centred on the gap's place on the ring.
            const D2D1_POINT_2F c = l.polar.Point(x + markerMain / 2.0f, l.itemTop + l.iconPx / 2.0f);
            marker_.visual->SetOffsetX(std::round(c.x - static_cast<float>(marker_.width) / 2.0f));
            marker_.visual->SetOffsetY(std::round(c.y - static_cast<float>(marker_.height) / 2.0f));
            markerX_ = Tween{x, x, now, 0.0, true};
        } else {
            if (!markerX_.valid) SetCrossOffset(marker_.visual.get(), l.CrossOrigin(l.itemTop, l.iconPx));
            renderer_.Drive(
                markerX_, x, duration, now, [&](auto v) { return SetMainOffset(marker_.visual.get(), v); }, 0.01f, easing);
        }
    }

    if (labelShown_ && label_.visual) {
        renderer_.Drive(
            labelX_, labelXTarget_, duration, now, [&](auto v) { return label_.visual->SetOffsetX(v); }, 0.01f,
            easing);
        renderer_.Drive(
            labelY_, labelYTarget_, duration, now, [&](auto v) { return label_.visual->SetOffsetY(v); }, 0.01f,
            easing);
    }
    // Round 11 phase 11A: the slot buttons follow the hover / press / state (and the pointer with
    // hot tracking) in the same commit.
    if (const HRESULT hr = RenderButtons(); FAILED(hr)) Log(L"slot buttons", hr);
    // Round 12 phase 12D: customize mode's outline, badge, "+" slots and anchor targets.
    if (editing_ || editOutline_.visual) PlaceEditVisuals();
}

void DockWindow::ResetPlacementTweens() {
    bgLeftX_.valid = bgRightX_.valid = false;
    labelX_.valid = labelY_.valid = false;
    markerX_.valid = false;
    for (DockItem& item : items_) item.tx.valid = item.ts.valid = false;
}

void DockWindow::ScheduleMagnify() {
    // Mouse moves can arrive at the pointer's report rate; retarget at most every ~12 ms and
    // let DWM interpolate in between. The throttle timer only exists while the mouse moves.
    const double wait = kMagnifyIntervalS - (Renderer::Now() - lastMagnify_);
    if (wait <= 0.0) {
        ApplyMagnify();
        return;
    }
    if (!magnifyPending_) {
        SetTimer(hwnd_, kMagnifyTimerId, TimerMs(wait), nullptr);
        magnifyPending_ = true;
    }
}

void DockWindow::ApplyMagnify() {
    if (magnifyPending_) {
        KillTimer(hwnd_, kMagnifyTimerId);
        magnifyPending_ = false;
    }
    if (restPending_) {
        KillTimer(hwnd_, kRestTimerId);
        restPending_ = false;
    }
    lastMagnify_ = Renderer::Now();
    RenderWithRecovery([this] {
        if (magnifying_) SetBigIcons(true);
        HRESULT hr = S_OK;
        if (labelShown_ && labelDirty_) hr = RenderLabel();
        ComputeTargets();
        ApplyTargets(motion_.magnify, true);
        PlaceScrollThumb(motion_.magnify);  // a drag on the bar / an auto-scroll moved the run
        return hr;
    });
}

void DockWindow::SetBigIcons(bool big) {
    if (layout_.bitmapPx <= static_cast<UINT>(layout_.iconPx)) big = false;
    if (big == bigIcons_) return;
    bigIcons_ = big;
    for (DockItem& item : items_) {
        const bool useBig = big && item.iconBig.width > 0;
        renderer_.SetVisible(item.icon, !useBig);
        renderer_.SetVisible(item.iconBig, useBig);
        if (item.badge.visual && item.badgeBig.visual) {
            const bool bigBadge = big && item.badgeBig.width > 0;
            renderer_.SetVisible(item.badge, !bigBadge);
            renderer_.SetVisible(item.badgeBig, bigBadge);
        }
    }
}

void DockWindow::LeaveZone() {
    if (!CursorInZone()) previewSuppressed_ = false;  // really left (not a menu taking the mouse)
    if (editing_ && editHover_ >= 0) {  // round 12 phase 12D: the remove badge goes with the pointer
        editHover_ = -1;
        RenderWithRecovery([this] {
            PlaceEditVisuals();
            return S_OK;
        });
    }
    pressed_ = -1;
    pressedMiddle_ = -1;
    if (magnifyPending_) {
        KillTimer(hwnd_, kMagnifyTimerId);
        magnifyPending_ = false;
    }
    // 9A follow-up: a menu or a modal dialog of the dock (hold_) took the mouse: the layout stays as
    // it is (the bar grown, the icons magnified where the pointer was) until it ends; SetHold(false)
    // then runs the leave check again (the pointer gone: back to rest below; still on the dock:
    // nothing moves). An open popup keeps the grown bar (the icons return to rest as before).
    if (hold_ && grown_) {
        SetHover(-1);
        UpdateWidgetHover(-1);
        return;
    }
    SetTitleHover(false);  // round 12 phase 12C: a hover title fades out
    const bool keepGrown = grown_ && popupHolds_ != 0;
    const bool wasActive = magnifying_ || hovered_ >= 0 || (grown_ && !keepGrown);
    magnifying_ = false;
    if (!keepGrown) grown_ = false;
    SetHover(-1);
    UpdateWidgetHover(-1);
    if (wasActive) {
        RenderWithRecovery([this] {
            ComputeTargets();
            ApplyTargets(motion_.rest);
            return S_OK;
        });
        // Once the icons have landed: swap back to the rest-size (crisper) icon surfaces and
        // shrink the window to the bar.
        SetTimer(hwnd_, kRestTimerId, TimerMs(motion_.rest + 0.04), nullptr);
        restPending_ = true;
    }
    TryCompact();
    ScheduleHide();
}

bool DockWindow::InZone(POINT pt) const {
    // The active zone: the bar's (rest) extent along the edge, from the bar top (or, while
    // magnified, the magnified icons' top) to the screen edge (canonical; stage pixel in).
    if (hideState_ == HideState::Hidden) return false;
    const Layout& l = layout_;
    const D2D1_POINT_2F c = l.Canon(pt);  // canonical: x along the bar, y toward the edge
    const float x = c.x;
    const float y = c.y;
    // 9A follow-up: a grown "steady" bar's whole extent (its ends and the widgets there moved out).
    if (x < l.bar.left - GrownOutStart() || x >= l.bar.right + GrownOutEnd()) return false;
    const float top = magnifying_ ? std::min(l.magTop, l.bar.top) : l.bar.top;
    // A ring: its band (and the magnified icons beyond it); its hole is not the dock.
    return y >= top && y < (l.polar.on ? l.bar.bottom : l.stageCross);
}

bool DockWindow::CursorInZone() const {
    POINT pt{};
    if (!hwnd_ || !GetCursorPos(&pt)) return false;
    // Only where the window is: below a compact (bar-sized) window the zone's bottom margin
    // belongs to whatever is underneath, and we would never see the mouse leave it.
    if (!PtInRect(&winRect_, pt)) return false;
    return InZone(POINT{pt.x - layout_.window.left, pt.y - layout_.window.top});
}

// --- Hover label -----------------------------------------------------------------------------

void DockWindow::SetHover(int index) {
    if (index != hoverCellSlot_ && hoverCellSlot_ >= 0) SetHoverCell(-1, -1, {}, 0.0f);
    if (index == hovered_) return;
    const int previous = hovered_;
    hovered_ = index;
    // Hover glow: fades with the label's duration (committed by the caller).
    if (GlowOf(DockTheme()) == GlowMode::Hover) UpdateGlows(motion_.labelFade);
    UpdatePreviewHover();
    // A separator has no name to show.
    if (index < 0 || items_[static_cast<size_t>(index)].kind == DockItem::Kind::Separator) {
        HideLabel();
        return;
    }
    if (labelShown_) {
        labelDirty_ = true;  // redrawn for the new item by the next ApplyMagnify
        return;
    }
    if (previous < 0 || !labelPending_) {
        SetTimer(hwnd_, kLabelTimerId, kLabelDelayMs, nullptr);
        labelPending_ = true;
    }
}

void DockWindow::OnLabelTimer() {
    KillTimer(hwnd_, kLabelTimerId);
    labelPending_ = false;
    if (hovered_ < 0 || hold_ || preview_.Visible() || hideState_ == HideState::Hidden ||
        hideState_ == HideState::Hiding || items_[static_cast<size_t>(hovered_)].kind == DockItem::Kind::Separator) {
        return;
    }
    if (hoverCell_ >= 0 && hoverCellSlot_ == hovered_) {
        // A cell: the pointer rested on it (version 4 tray icons open their own popup), and a cell
        // without a label shows none.
        ElementOf(static_cast<size_t>(hovered_)).OnCellHover(hovered_, hoverCell_, CellEvent::Dwell);
        if (hoverCellLabel_.empty()) return;
    }
    // Shown again while the previous label still fades out: fade in from where it is now.
    const bool fadingOut = labelFading_;
    if (labelFading_) {
        KillTimer(hwnd_, kLabelFadeTimerId);
        labelFading_ = false;
    }
    labelShown_ = true;
    labelDirty_ = true;
    labelX_.valid = labelY_.valid = false;  // appear in place, then follow the icon
    RenderWithRecovery([&] {
        HRESULT hr = RenderLabel();
        if (FAILED(hr)) return hr;
        ComputeTargets();
        ApplyTargets(motion_.magnify, true);
        if (labelShown_) {
            if (!fadingOut) DriveLabelOpacity(0.0f, 0.0);
            DriveLabelOpacity(1.0f, motion_.labelFade);
        }
        renderer_.SetVisible(label_, labelShown_);
        return S_OK;
    });
}

void DockWindow::HideLabel() {
    if (labelPending_) {
        KillTimer(hwnd_, kLabelTimerId);
        labelPending_ = false;
    }
    if (!labelShown_) return;
    labelShown_ = false;  // stops following the icon at once
    labelX_.valid = labelY_.valid = false;
    if (label3_ && motion_.labelFade > 0.0 && hwnd_) {
        DriveLabelOpacity(0.0f, motion_.labelFade);  // committed by the caller
        SetTimer(hwnd_, kLabelFadeTimerId, TimerMs(motion_.labelFade + 0.02), nullptr);
        labelFading_ = true;
        return;
    }
    renderer_.SetVisible(label_, false);  // committed by the caller
}

void DockWindow::OnLabelFadeTimer() {
    KillTimer(hwnd_, kLabelFadeTimerId);
    if (!labelFading_) return;
    labelFading_ = false;
    if (!labelShown_) {
        RenderWithRecovery([this] {
            renderer_.SetVisible(label_, false);
            return S_OK;
        });
    }
    TryCompact();
}

void DockWindow::FinishLabelFade() {
    if (!labelFading_) return;
    if (hwnd_) KillTimer(hwnd_, kLabelFadeTimerId);
    labelFading_ = false;
    if (!labelShown_) renderer_.SetVisible(label_, false);
}

void DockWindow::DriveLabelOpacity(float to, double duration) {
    if (!label3_) return;
    IDCompositionVisual3* v3 = label3_.get();
    renderer_.Drive(
        labelOpacity_, to, duration, Renderer::Now(), [&](auto v) { return v3->SetOpacity(v); }, 0.001f,
        renderer_.FadeEasing());
}

// --- Thumbnail popup -------------------------------------------------------------------------
// Resting on an item with two or more windows (previewsSingleWindow: one or more) for
// previewDelayMs shows PreviewPopup above the dock (centred on the item's magnified position,
// docks[].popupGap beyond the magnified icon, or beyond the name label when it is shown for the
// item: round 7 phase 7A keeps the label with the popup). While it is on screen it holds the dock
// shown (kHoldPreview); a label that was not shown yet does not appear. Moving to another
// multi-window item switches the popup; to a single-window item dismisses it. Leaving both the
// item and the popup starts a kPreviewLeaveMs grace timer. Menus, drags, dock clicks and the dock
// hiding dismiss it. All timers are one-shot; none runs once the popup is hidden.

bool DockWindow::PreviewEligible(int index) const {
    if (!DockCfg().previews || index < 0 || index >= static_cast<int>(items_.size())) return false;
    const DockItem& item = items_[static_cast<size_t>(index)];
    // A window slot shows its one window (it never has more).
    const size_t minWindows = DockCfg().previewSingle || item.window ? 1 : 2;
    if (item.kind != DockItem::Kind::App || item.windows.size() < minWindows) return false;
    // Showing counts: the hover usually starts while the dock slides in (OnPreviewTimer requires
    // Shown).
    return (hideState_ == HideState::Shown || hideState_ == HideState::Showing) && !hold_ &&
           dragPhase_ == DragPhase::None && !dropActive_ && !ClickPopupOnScreen() && !editing_;  // round 12 phase 12D: none while customizing
}

void DockWindow::UpdatePreviewHover() {
    if (previewPending_) {
        KillTimer(hwnd_, kPreviewTimerId);
        previewPending_ = false;
    }
    if (preview_.Visible()) {
        if (hovered_ < 0) {
            if (!preview_.ContainsCursor()) StartPreviewLeave();
        } else if (hovered_ == previewItem_) {
            CancelPreviewLeave();
        } else if (PreviewEligible(hovered_)) {
            CancelPreviewLeave();
            ShowPreview(hovered_);
        } else {
            DismissPreview(true, false);  // a single-window item: its label takes over
        }
        return;
    }
    if (previewSuppressed_ && hovered_ >= 0 && (items_[static_cast<size_t>(hovered_)].key != previewSuppressKey_ ||
                               items_[static_cast<size_t>(hovered_)].window != previewSuppressWindow_)) {
        previewSuppressed_ = false;  // moved on to another item
    }
    if (hovered_ >= 0 && !previewSuppressed_ && PreviewEligible(hovered_) && hwnd_) {
        SetTimer(hwnd_, kPreviewTimerId,
                 std::max<UINT>(USER_TIMER_MINIMUM, static_cast<UINT>(DockCfg().previewDelayMs)), nullptr);
        previewPending_ = true;
    }
}

void DockWindow::OnPreviewTimer() {
    KillTimer(hwnd_, kPreviewTimerId);
    previewPending_ = false;
    if (!PreviewEligible(hovered_) || !CursorInZone()) return;
    if (hideState_ == HideState::Showing) {  // a very short delay: wait for the slide to end
        SetTimer(hwnd_, kPreviewTimerId, TimerMs(motion_.slide), nullptr);
        previewPending_ = true;
        return;
    }
    ShowPreview(hovered_);
}

void DockWindow::StartPreviewLeave() {
    if (previewLeavePending_ || !hwnd_) return;
    SetTimer(hwnd_, kPreviewLeaveTimerId, kPreviewLeaveMs, nullptr);
    previewLeavePending_ = true;
}

void DockWindow::CancelPreviewLeave() {
    if (!previewLeavePending_) return;
    KillTimer(hwnd_, kPreviewLeaveTimerId);
    previewLeavePending_ = false;
}

void DockWindow::OnPreviewLeaveTimer() {
    KillTimer(hwnd_, kPreviewLeaveTimerId);
    previewLeavePending_ = false;
    if (!preview_.Visible()) return;
    if (preview_.ContainsCursor() || (hovered_ >= 0 && hovered_ == previewItem_)) return;  // came back
    DismissPreview(true);
}

void DockWindow::OnPreviewHover(bool inside) {
    if (inside) {
        CancelPreviewLeave();
        CancelHide();
    } else if (hovered_ < 0 || hovered_ != previewItem_) {
        StartPreviewLeave();
    }
}

void DockWindow::ShowPreview(int index, bool keepAnchor) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    const DockItem& item = items_[static_cast<size_t>(index)];
    if (item.windows.empty()) return;
    const Layout& l = layout_;
    PreviewPopup::Content content;
    content.windows = item.windows;
    if (ID2D1Bitmap* bmp = icons_.Get(item.key, item.windows.front(), l.bitmapPx, renderer_.ResourceContext(),
                                      item.iconFile, item.iconIndex)) {
        content.icon.copy_from(bmp);
    }
    PreviewPopup::Style style;
    style.scale = l.scale;
    style.acrylic = acrylic_;
    const ThemeConfig& theme = DockTheme();
    style.tint = acrylic_ ? theme.acrylicBackground : theme.background;
    style.text = theme.labelText;
    style.border = theme.barBorder;
    style.hotBackground = theme.previewHotBackground;
    style.hotBorder = theme.previewHotBorder;
    style.closeHover = theme.closeHover;
    style.cornerRadiusDip = theme.cornerRadius;
    style.monitor = l.work;
    // Round 7 phase 7A: the hover label shown for this slot stays; the popup opens beyond it.
    const bool keepLabel = labelShown_ && hovered_ == index;
    if (!keepAnchor) previewAnchor_ = PopupAnchorAt(item.targetX + BoxW(item) / 2.0f, keepLabel ? index : -1);
    const PopupAnchor anchor = previewAnchor_;
    const AppKey key = item.key;
    const HWND window = item.window;
    if (!preview_.Show(content, style, anchor)) {
        DismissPreview(false);
        return;
    }
    previewItem_ = index;
    previewKey_ = key;
    previewWindow_ = window;
    TakePopupHold(kHoldPreview);
    // Another slot's label (or a pending one) goes: the popup was anchored for this slot only.
    if (!keepLabel) {
        HideLabel();
        RenderWithRecovery([] { return S_OK; });  // commit the label hide
    }
}

void DockWindow::DismissPreview(bool animate, bool leaveCheck) {
    if (previewPending_) {
        KillTimer(hwnd_, kPreviewTimerId);
        previewPending_ = false;
    }
    CancelPreviewLeave();
    preview_.Hide(animate);
    previewItem_ = -1;
    previewKey_ = AppKey{};
    previewWindow_ = nullptr;
    previewOpener_.clear();
    ReleasePopupHold(kHoldPreview, leaveCheck);
}

void DockWindow::TakePopupHold(uint32_t bit) {
    if (popupHolds_ & bit) return;
    popupHolds_ |= bit;
    CancelHide();
}

void DockWindow::ReleasePopupHold(uint32_t bit, bool leaveCheck) {
    if (!(popupHolds_ & bit)) return;
    popupHolds_ &= ~bit;
    // A grown bar (9A follow-up) the popup kept: without a leave check here, one runs once the
    // current message is done (nothing holding any more and the pointer gone: back to rest).
    if (hwnd_ && !leaveCheck && grown_ && !hold_ && !popupHolds_ && !grownCheckPosted_) {
        grownCheckPosted_ = PostMessageW(hwnd_, kGrownCheckMessage, 0, 0) != FALSE;
    }
    // Another popup (or a menu / drag) still holds the dock: it does the leave check when it ends.
    if (!hwnd_ || !leaveCheck || hold_ || popupHolds_ || hideState_ == HideState::Hidden) return;
    if (!CursorInZone()) LeaveZone();  // return to rest, compact, start the hide delay
}

void DockWindow::SuppressPreview() {
    if (hovered_ >= 0 && hovered_ < static_cast<int>(items_.size())) {
        previewSuppressed_ = true;
        previewSuppressKey_ = items_[static_cast<size_t>(hovered_)].key;
        previewSuppressWindow_ = items_[static_cast<size_t>(hovered_)].window;
    }
    DismissPreview(!hold_, false);
}

void DockWindow::RefreshPreview() {
    if (!preview_.Visible()) return;
    int index = -1;
    for (size_t i = 0; i < items_.size() && index < 0; ++i) {
        if (items_[i].key == previewKey_ && items_[i].window == previewWindow_) index = static_cast<int>(i);
    }
    if (index < 0 || items_[static_cast<size_t>(index)].windows.empty()) {
        DismissPreview(true);  // the last window closed
        return;
    }
    ShowPreview(index, true);  // cards follow the windows (a closed one disappears)
}

// --- Folder popup ----------------------------------------------------------------------------
// A left click on a folder item toggles FolderPopup (a click popup: it takes the foreground and
// closes itself when it loses it or on Escape) above the icon, anchored like the thumbnail popup.
// While it is open it holds the dock shown (kHoldFolder). The dock's own button-down, a drag, a
// menu, a hide and any relayout dismiss it. Opening an entry from it dismisses it too.

void DockWindow::ToggleFolderPopup(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    const DockItem& item = items_[static_cast<size_t>(index)];
    if (item.kind != DockItem::Kind::Folder && item.kind != DockItem::Kind::Group) return;
    if (folder_.Visible() && item.key == folderKey_) {
        DismissFolderPopup(true);
        return;
    }
    ShowClickPopup(index);
}

void DockWindow::ShowFolderPopup(int index, float anchor) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    const DockItem& item = items_[static_cast<size_t>(index)];
    if (item.kind != DockItem::Kind::Folder) return;
    const std::wstring path = !item.targetPath.empty() ? item.targetPath : item.launchPath.value_or(item.key.Source());
    ShowFolderPopupAt(path, item.displayName, anchor, item.key, ElementOf(static_cast<size_t>(index)).Entry().view);
}

void DockWindow::ShowFolderPopupAt(const std::wstring& path, const std::wstring& name, float anchor, const AppKey& key,
                                   ItemView view) {
    DismissWidgetPopup(false, false);
    DismissWidgetPassive(false);
    DismissPreview(false, false);
    DismissWeatherPopup(false, false);
    DismissCalendarPopup(false, false);
    DismissCentre(false, false);
    if (!folder_.Show(path, name, view, PopupStyle(), PopupAnchorAt(anchor))) {
        DismissFolderPopup(false);
        MessageBeep(MB_ICONWARNING);
        return;
    }
    folderKey_ = key;
    TakePopupHold(kHoldFolder);
    HideLabel();
    RenderWithRecovery([] { return S_OK; });  // commit the label hide
}

FolderPopup::Style DockWindow::PopupStyle() const {
    const Layout& l = layout_;
    FolderPopup::Style style;
    style.scale = l.scale;
    style.acrylic = acrylic_;
    const ThemeConfig& theme = DockTheme();
    style.tint = acrylic_ ? theme.acrylicBackground : theme.background;
    style.text = theme.labelText;
    style.border = theme.barBorder;
    style.hotBackground = theme.previewHotBackground;
    style.hotBorder = theme.previewHotBorder;
    style.secondary = theme.clockDateText;
    style.accent = theme.accent;
    style.cornerRadiusDip = theme.cornerRadius;
    style.reduceMotion = motion_.reduce;
    style.monitor = l.work;
    return style;
}

const AppIdentity::ResolvedPin* DockWindow::GroupPin(const AppKey& key) const {
    for (const AppIdentity::ResolvedPin& p : pins_) {
        if (p.kind == DockItem::Kind::Group && p.key == key) return &p;
    }
    return nullptr;
}

std::vector<FolderPopup::GroupEntry> DockWindow::GroupEntries(const AppIdentity::ResolvedPin& group) {
    std::vector<FolderPopup::GroupEntry> entries;
    entries.reserve(group.members.size());
    for (const AppIdentity::ResolvedPin& m : group.members) {
        FolderPopup::GroupEntry e;
        e.label = m.displayName;
        if (!m.missing) {
            e.icon.copy_from(icons_.Get(m.key, nullptr, layout_.bitmapPx, renderer_.ResourceContext(), m.iconFile, m.iconIndex));
        }
        entries.push_back(std::move(e));
    }
    return entries;
}

std::wstring DockWindow::GroupSignature(const AppIdentity::ResolvedPin& group) {
    std::wstring s = group.displayName;
    for (const AppIdentity::ResolvedPin& m : group.members) s += L"|" + m.key.id + L"/" + m.displayName + L"/" + m.iconFile;
    return s;
}

// A click on a group toggles its popup: the members (icons, names) in the folder popup's window;
// clicking one opens / activates it, right click Open / Remove from group / Move to dock, a member
// dragged out onto the dock becomes a pin there, files dropped on it become members.
void DockWindow::ShowGroupPopup(int index, float anchor) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    const DockItem& item = items_[static_cast<size_t>(index)];
    const AppIdentity::ResolvedPin* group = item.kind == DockItem::Kind::Group ? GroupPin(item.key) : nullptr;
    if (!group) return;
    DismissWidgetPopup(false, false);
    DismissWidgetPassive(false);
    DismissPreview(false, false);
    DismissWeatherPopup(false, false);
    DismissCalendarPopup(false, false);
    DismissCentre(false, false);
    const AppKey key = item.key;
    const ItemView view = ElementOf(static_cast<size_t>(index)).Entry().view;
    FolderPopup::GroupCallbacks cb;
    cb.open = [this, key](size_t member) { OpenGroupMember(key, member); };
    cb.command = [this, key](size_t member, UINT command) {
        switch (command) {
            case FolderPopup::kMemberOpen: OpenGroupMember(key, member); break;
            case FolderPopup::kMemberRemove:
                if (pinRequests_.removeMember) pinRequests_.removeMember(key, member);
                break;
            case FolderPopup::kMemberToDock:
                if (pinRequests_.memberOut) pinRequests_.memberOut(key, member, SIZE_MAX);
                break;
            default: break;
        }
    };
    if (!DockCfg().locked) {
        cb.drag = [this, key](size_t member, POINT) { DragGroupMember(key, member); };
        cb.accepts = [](const std::vector<std::wstring>& files) { return !PinnableFiles(files).empty(); };
        cb.drop = [this, key](const std::vector<std::wstring>& files) {
            if (pinRequests_.addToGroup) pinRequests_.addToGroup(key, PinnableFiles(files));
        };
    }
    groupSignature_ = GroupSignature(*group);
    if (!folder_.ShowGroup(group->displayName, GroupEntries(*group), view, PopupStyle(), PopupAnchorAt(anchor), std::move(cb))) {
        DismissFolderPopup(false);
        MessageBeep(MB_ICONWARNING);
        return;
    }
    folderKey_ = key;
    TakePopupHold(kHoldFolder);
    HideLabel();
    RenderWithRecovery([] { return S_OK; });  // commit the label hide
}

void DockWindow::OpenGroupMember(const AppKey& groupKey, size_t member) {
    const AppIdentity::ResolvedPin* group = GroupPin(groupKey);
    if (!group || member >= group->members.size()) return;
    const AppIdentity::ResolvedPin m = group->members[member];  // a copy: the popup closes first
    DismissFolderPopup(true);
    // Running: its windows come to the front (as a click on its own slot would), else it starts.
    for (const WindowTracker::Group& g : shared_->Tracker().Current().groups) {
        if (g.key == m.key && !g.windows.empty()) {
            Activation::ActivateItem(g.windows);
            return;
        }
    }
    DockItem copy;
    copy.key = m.key;
    copy.kind = m.kind;
    copy.launchPath = m.launchPath.empty() ? std::nullopt : std::optional<std::wstring>(m.launchPath);
    copy.workingDir = m.workingDir;
    AllowSetForegroundWindow(ASFW_ANY);
    if (!Launcher::Launch(copy)) MessageBeep(MB_ICONWARNING);
}

void DockWindow::DragGroupMember(const AppKey& group, size_t member) {
    winrt::com_ptr<IDataObject> data;
    if (!MemberFormat() || FAILED(SHCreateDataObject(nullptr, 0, nullptr, nullptr, IID_PPV_ARGS(data.put())))) return;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(MemberDragData));
    if (!h) return;
    if (auto* p = static_cast<MemberDragData*>(GlobalLock(h))) {
        p->pid = GetCurrentProcessId();
        p->dock = reinterpret_cast<UINT64>(hwnd_);
        p->member = static_cast<UINT32>(member);
        wcsncpy_s(p->group, group.id.c_str(), _TRUNCATE);
        GlobalUnlock(h);
    }
    FORMATETC fmt{MemberFormat(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    medium.tymed = TYMED_HGLOBAL;
    medium.hGlobal = h;
    if (FAILED(data->SetData(&fmt, &medium, TRUE))) {
        GlobalFree(h);
        return;
    }
    MemberDropSource* source = new MemberDropSource();
    DWORD effect = DROPEFFECT_NONE;
    DoDragDrop(data.get(), source, DROPEFFECT_MOVE, &effect);  // modal; our own drop target handles a drop on the dock
    source->Release();
}

void DockWindow::DismissFolderPopup(bool animate, bool leaveCheck) {
    folder_.Hide(animate);
    folderKey_ = AppKey{};
    ReleasePopupHold(kHoldFolder, leaveCheck);
}

void DockWindow::OnFolderPopupClosed() {
    // Hidden already (fading out): only the dock's side.
    folderKey_ = AppKey{};
    ReleasePopupHold(kHoldFolder, true);
}

void DockWindow::RefreshFolderPopup() {
    if (!folder_.OnScreen() || folderKey_.id.empty()) return;
    if (folder_.Kind() == 3) {
        // The tray drawer: gone with its tray slot; otherwise it follows the icons (OnTrayChanged).
        const bool present = std::any_of(items_.begin(), items_.end(), [&](const DockItem& item) {
            return item.key == folderKey_ && item.kind == DockItem::Kind::Tray;
        });
        if (!present || !Tray()) DismissFolderPopup(true);
        return;
    }
    if (folder_.Kind() == 2) {
        // A group: gone (unpinned, ungrouped) -> dismissed; other members / name -> shown again.
        const AppIdentity::ResolvedPin* group = GroupPin(folderKey_);
        if (!group) {
            DismissFolderPopup(true);
            return;
        }
        const std::wstring signature = GroupSignature(*group);
        if (signature != groupSignature_) {
            groupSignature_ = signature;
            folder_.UpdateGroup(group->displayName, GroupEntries(*group));
        }
        return;
    }
    const bool present = std::any_of(items_.begin(), items_.end(), [&](const DockItem& item) {
        // A folder item, or the script widget that opened it (popup.folder).
        return item.key == folderKey_ && (item.kind == DockItem::Kind::Folder || item.kind == DockItem::Kind::Widget);
    });
    if (!present) DismissFolderPopup(true);  // unpinned while open
}

// --- Click popups ----------------------------------------------------------------------------
// A left click on the weather widget toggles WeatherPopup (unconfigured: opens Settings > Weather
// instead); on the clock, CalendarPopup (clock.calendarPopup); on a folder, FolderPopup. The
// elements decide (DockElement::OnClick, Popup, PopupAnchor); the dock hosts the popups: they are
// anchored over their slot, hold the dock shown (kHoldFolder / kHoldWeather / kHoldCalendar) and
// are dismissed by the dock's own button-down, a drag, a menu, a hide and any relayout. At most
// one click popup is open at a time.

bool DockWindow::PopupShownFor(int slot) const {
    if (slot < 0 || slot >= static_cast<int>(items_.size())) return false;
    switch (ElementOf(static_cast<size_t>(slot)).Popup()) {
        case ClickPopup::Folder:
        case ClickPopup::Group:
        case ClickPopup::Tray: return folder_.Visible() && items_[static_cast<size_t>(slot)].key == folderKey_;
        // Round 5: per item (two weather items show two places; two clocks two click targets).
        case ClickPopup::Weather: return weatherPopup_.Visible() && items_[static_cast<size_t>(slot)].key == weatherKey_;
        case ClickPopup::Calendar:
            return calendar_.Visible() && (calendarClock_.id.empty() || items_[static_cast<size_t>(slot)].key == calendarClock_);
        case ClickPopup::Notifications: return centre_.Visible() && centreOpener_.empty();
        case ClickPopup::None:
        default: return false;
    }
}

void DockWindow::ShowClickPopup(int slot) {
    if (slot < 0 || slot >= static_cast<int>(items_.size())) return;
    // A scrolling run: the slot glides fully into view first (the popup opens over where it lands).
    if (layout_.view.on) ScrollSlotIntoView(slot);
    const DockElement& element = ElementOf(static_cast<size_t>(slot));
    const float anchor = SlotPopupAnchor(slot);
    switch (element.Popup()) {
        case ClickPopup::Folder: ShowFolderPopup(slot, anchor); return;
        case ClickPopup::Group: ShowGroupPopup(slot, anchor); return;
        case ClickPopup::Tray: ShowTrayDrawer(slot, anchor); return;
        case ClickPopup::Weather:
            weatherKey_ = items_[static_cast<size_t>(slot)].key;
            ShowWeatherPopup(anchor);
            return;
        case ClickPopup::Calendar:
            // The footer opens this clock's target.
            calendarTarget_ = ClockOf(*config_, element.Entry()).clickTarget;
            ShowCalendarPopup(anchor);
            calendarClock_ = calendar_.Visible() ? items_[static_cast<size_t>(slot)].key : AppKey{};
            return;
        case ClickPopup::Notifications: ShowCentre(anchor); return;
        case ClickPopup::None:
        default: return;
    }
}

void DockWindow::TogglePopup(ItemKind kind) {
    for (size_t i = 0; i < items_.size(); ++i) {
        if (!elements_[items_[i].element] || ElementOf(i).Kind() != kind) continue;
        if (PopupShownFor(static_cast<int>(i))) {
            DismissClickPopups(true);
        } else {
            ShowClickPopup(static_cast<int>(i));
        }
        return;
    }
}

void DockWindow::ToggleSlotPopup(int slot) {
    if (slot < 0 || slot >= static_cast<int>(items_.size()) || !elements_[items_[static_cast<size_t>(slot)].element]) return;
    if (PopupShownFor(slot)) {
        DismissClickPopups(true);
    } else {
        ShowClickPopup(slot);
    }
}

WeatherPopup::Style DockWindow::WeatherPopupStyle() const {
    const Layout& l = layout_;
    const ThemeConfig& theme = DockTheme();
    WeatherPopup::Style style;
    style.scale = l.scale;
    style.acrylic = acrylic_;
    style.tint = acrylic_ ? theme.acrylicBackground : theme.background;
    style.text = theme.labelText;
    style.secondary = theme.clockDateText;
    style.border = theme.barBorder;
    style.hotBackground = theme.previewHotBackground;
    style.hotBorder = theme.previewHotBorder;
    style.cornerRadiusDip = theme.cornerRadius;
    style.monitor = l.work;
    style.glyphs.ink = theme.labelText;
    style.glyphs.water = theme.accent;
    return style;
}

void DockWindow::ShowWeatherPopup(float anchor) {
    if (hideState_ == HideState::Hidden) return;
    DismissPreview(false, false);
    DismissFolderPopup(false, false);
    DismissCalendarPopup(false, false);
    DismissWidgetPopup(false, false);
    DismissWidgetPassive(false);
    DismissCentre(false, false);
    if (!weatherPopup_.Show(WeatherPopupStyle(), PopupAnchorAt(anchor))) {
        DismissWeatherPopup(false);
        MessageBeep(MB_ICONWARNING);
        return;
    }
    TakePopupHold(kHoldWeather);
    HideLabel();
    RenderWithRecovery([] { return S_OK; });  // commit the label hide
}

void DockWindow::DismissWeatherPopup(bool animate, bool leaveCheck) {
    weatherPopup_.Hide(animate);
    ReleasePopupHold(kHoldWeather, leaveCheck);
}

void DockWindow::ShowCalendarPopup(float anchor, int64_t day) {
    if (hideState_ == HideState::Hidden) return;
    calendarOpener_.clear();  // the clock's (popup.calendar sets it after this)
    calendarAnchor_ = anchor;
    DismissPreview(false, false);
    DismissFolderPopup(false, false);
    DismissWeatherPopup(false, false);
    DismissWidgetPopup(false, false);
    DismissWidgetPassive(false);
    DismissCentre(false, false);
    const Layout& l = layout_;
    const ThemeConfig& theme = DockTheme();
    CalendarPopup::Style style;
    style.scale = l.scale;
    style.acrylic = acrylic_;
    style.tint = acrylic_ ? theme.acrylicBackground : theme.background;
    style.text = theme.labelText;
    style.secondary = theme.clockDateText;
    style.border = theme.barBorder;
    style.hotBackground = theme.previewHotBackground;
    style.hotBorder = theme.previewHotBorder;
    style.accent = theme.accent;
    style.cornerRadiusDip = theme.cornerRadius;
    style.monitor = l.work;
    // The clock's clickTarget set: a footer button opens it (also when a click on the clock does).
    if (const std::wstring name = Launcher::TargetName(calendarTarget_); !name.empty()) {
        style.footer = L"Open " + name;
    }
    style.dots = config_->calendars.showDots;
    style.reduceMotion = motion_.reduce;
    CalendarService* service = shared_ && shared_->Calendar().Active() ? &shared_->Calendar() : nullptr;
    if (!calendar_.Show(style, PopupAnchorAt(anchor), service, day)) {
        DismissCalendarPopup(false);
        MessageBeep(MB_ICONWARNING);
        return;
    }
    TakePopupHold(kHoldCalendar);
    HideLabel();
    RenderWithRecovery([] { return S_OK; });  // commit the label hide
}

void DockWindow::DismissCalendarPopup(bool animate, bool leaveCheck) {
    calendar_.Hide(animate);
    calendarOpener_.clear();
    calendarClock_ = AppKey{};
    ReleasePopupHold(kHoldCalendar, leaveCheck);
}

void DockWindow::DismissClickPopups(bool animate, bool leaveCheck) {
    DismissFolderPopup(animate, leaveCheck);
    DismissWeatherPopup(animate, leaveCheck);
    DismissCalendarPopup(animate, leaveCheck);
    DismissWidgetPopup(animate, leaveCheck);
    DismissCentre(animate, leaveCheck);
}

// --- the notification centre (round 4 phase 9b) -------------------------------------------------

NotificationPopup::Style DockWindow::CentreStyle() const {
    const Layout& l = layout_;
    const ThemeConfig& theme = DockTheme();
    NotificationPopup::Style style;
    style.scale = l.scale;
    style.acrylic = acrylic_;
    style.tint = acrylic_ ? theme.acrylicBackground : theme.background;
    style.text = theme.labelText;
    style.secondary = theme.clockDateText;
    style.border = theme.barBorder;
    style.hotBackground = theme.previewHotBackground;
    style.hotBorder = theme.previewHotBorder;
    style.accent = theme.accent;
    style.cornerRadiusDip = theme.cornerRadius;
    style.monitor = l.work;
    style.reduceMotion = motion_.reduce;
    return style;
}

void DockWindow::ShowCentre(float anchor) {
    if (hideState_ == HideState::Hidden || !shared_) return;
    DismissPreview(false, false);
    DismissFolderPopup(false, false);
    DismissWeatherPopup(false, false);
    DismissCalendarPopup(false, false);
    DismissWidgetPopup(false, false);
    DismissWidgetPassive(false);
    shared_->SetCentreOpen(true);  // the service runs (and polls every second) while it is open
    if (!centre_.Show(CentreStyle(), PopupAnchorAt(anchor), shared_->Notifications())) {
        DismissCentre(false);
        MessageBeep(MB_ICONWARNING);
        return;
    }
    TakePopupHold(kHoldCentre);
    HideLabel();
    RenderWithRecovery([] { return S_OK; });  // commit the label hide
}

void DockWindow::DismissCentre(bool animate, bool leaveCheck) {
    const bool was = centre_.OnScreen();
    centre_.Hide(animate);
    centreOpener_.clear();
    ReleasePopupHold(kHoldCentre, leaveCheck);
    if (was && shared_) shared_->SetCentreOpen(false);
}

bool DockWindow::CentreCommand(UINT cmd, size_t row) {
    if (!centre_.Visible()) return false;
    switch (cmd) {
        case ContextMenu::kCentreDismiss: return centre_.DismissRow(row);
        case ContextMenu::kCentreActivate: return centre_.ActivateRow(row);
        case ContextMenu::kCentreClearAll: return centre_.ClearAll();
        default: return false;
    }
}

size_t DockWindow::UnreadNotifications() const { return shared_ ? shared_->Notifications().Unread() : 0; }

size_t DockWindow::CalendarEventsToday() const {
    if (!shared_ || !shared_->Calendar().Active()) return 0;
    return shared_->Calendar().CountOn(CalendarService::Today());
}

void DockWindow::OnCalendarChanged() {
    if (!hwnd_ || !tracking_) return;
    DispatchEvent(DockEvent::Calendar);  // the clock's dot
    if (!calendar_.Visible()) return;
    // The service started / stopped while the popup is open: show it again with / without events.
    const bool active = shared_ && shared_->Calendar().Active();
    if (active != (calendar_.SelectedDay() != INT64_MIN)) {
        const int64_t day = calendar_.SelectedDay();
        const std::wstring opener = calendarOpener_;
        const AppKey clock = calendarClock_;
        ShowCalendarPopup(calendarAnchor_, day);
        if (calendar_.Visible()) {
            calendarOpener_ = opener;
            calendarClock_ = clock;
        }
        return;
    }
    calendar_.Update();
}

void DockWindow::ShowReminder(const CalendarService::ReminderInfo& reminder) {
    if (!hwnd_) return;
    for (const CalendarService::ReminderInfo& r : reminders_) {
        if (r.id == reminder.id) return;  // already waiting
    }
    reminders_.push_back(reminder);
    if (reminders_.size() > 32) reminders_.erase(reminders_.begin() + 1);  // the oldest waiting ones go
    if (fullscreenHidden_) return;  // round 9 phase 9A: shown when the full-screen app is gone
    ShowFrontReminder();
}

void DockWindow::ShowFrontReminder() {
    if (reminders_.empty()) {
        reminder_.Hide(true);
        return;
    }
    const Layout& l = layout_;
    const ThemeConfig& theme = DockTheme();
    ReminderBanner::Style style;
    style.scale = l.scale;
    style.acrylic = acrylic_;
    style.tint = acrylic_ ? theme.acrylicBackground : theme.background;
    style.text = theme.labelText;
    style.secondary = theme.clockDateText;
    style.border = theme.barBorder;
    style.hotBackground = theme.previewHotBackground;
    style.hotBorder = theme.previewHotBorder;
    style.cornerRadiusDip = theme.cornerRadius;
    style.monitor = l.work;
    // Over the clock when the dock has one, else over the middle of the bar.
    float anchor = (l.bar.left + l.bar.right) / 2.0f;
    for (const DockItem& item : items_) {
        if (item.kind == DockItem::Kind::Clock) {
            anchor = item.targetX + item.slotW / 2.0f;
            break;
        }
    }
    if (!reminder_.Show(style, PopupAnchorAt(anchor), reminders_.front(), reminders_.size() - 1)) MessageBeep(MB_ICONINFORMATION);
}

void DockWindow::ReminderAction(int action) {
    if (reminders_.empty()) return;
    const CalendarService::ReminderInfo r = reminders_.front();
    reminders_.erase(reminders_.begin());
    if (action == 1 && shared_) shared_->Calendar().Snooze(r.id, 5 * 60);
    ShowFrontReminder();
    if (action == 3) {
        for (size_t i = 0; i < items_.size(); ++i) {
            if (items_[i].kind != DockItem::Kind::Clock) continue;
            calendarTarget_ = ClockOf(*config_, ElementOf(i).Entry()).clickTarget;
            ShowCalendarPopup(items_[i].targetX + items_[i].slotW / 2.0f, r.day);
            if (calendar_.Visible()) calendarClock_ = items_[i].key;
            break;
        }
    }
}

bool DockWindow::ReminderCommand(UINT cmd) {
    if (!reminder_.Visible() || reminders_.empty()) return false;
    ReminderAction(cmd == ContextMenu::kReminderSnooze ? 1 : 2);
    return true;
}

bool DockWindow::CalendarSelect(int offset) {
    if (!calendar_.Visible()) return false;
    return calendar_.SelectDay(CalendarService::Today() + offset);
}

bool DockWindow::NotificationsDenied() const {
    return shared_ && shared_->Notifications().GetState() == NotificationService::State::Denied;
}

// --- Script widget popups ------------------------------------------------------------------------
// popup.open / close / redraw / folder / previews of a widget arrive here posted (they are asked
// for from inside script calls). A script popup is ScriptPopup (drawn by the widget's drawPopup),
// anchored on the widget's slot, holding the dock shown like the other click popups (kHoldWidget).

int DockWindow::SlotOfWidget(const WidgetInstance& w) const {
    for (size_t i = 0; i < items_.size(); ++i) {
        if (WidgetAt(static_cast<int>(i)) == &w) return static_cast<int>(i);
    }
    return -1;
}

WidgetInstance* DockWindow::WidgetByKey(const std::wstring& key) const {
    auto it = widgets_.find(key);
    return it == widgets_.end() ? nullptr : it->second.get();
}

void DockWindow::WidgetPopupRequest(WidgetInstance& w) {
    if (std::find(popupRequests_.begin(), popupRequests_.end(), w.Key()) == popupRequests_.end()) {
        popupRequests_.push_back(w.Key());
    }
    if (!popupRequestPosted_ && hwnd_) popupRequestPosted_ = PostMessageW(hwnd_, kWidgetPopupMessage, 0, 0) != FALSE;
}

bool DockWindow::WidgetPopupShown(const WidgetInstance& w) const {
    if (widgetPopup_.Visible() && popupWidget_ == w.Key()) return true;
    if (previewOpener_ == w.Key() && preview_.Visible()) return true;
    if (calendarOpener_ == w.Key() && calendar_.Visible()) return true;
    const int slot = SlotOfWidget(w);
    return slot >= 0 && folder_.Visible() && folderKey_ == items_[static_cast<size_t>(slot)].key;
}

bool DockWindow::WidgetPassiveShown(const WidgetInstance& w) const {
    return widgetPassive_.OnScreen() && passiveWidget_ == w.Key();
}

ScriptPopup::Style DockWindow::WidgetPopupStyle() const {
    const Layout& l = layout_;
    const ThemeConfig& theme = DockTheme();
    ScriptPopup::Style style;
    style.scale = l.scale;
    style.acrylic = acrylic_;
    style.tint = acrylic_ ? theme.acrylicBackground : theme.background;
    style.border = theme.barBorder;
    style.cornerRadiusDip = theme.cornerRadius;
    style.monitor = l.work;
    return style;
}

void DockWindow::OnWidgetPopupRequests() {
    popupRequestPosted_ = false;
    const std::vector<std::wstring> keys = std::move(popupRequests_);
    popupRequests_.clear();
    for (const std::wstring& key : keys) {
        WidgetInstance* w = WidgetByKey(key);
        if (!w) continue;
        const WidgetInstance::PopupRequest r = w->TakePopupRequest();
        const int slot = SlotOfWidget(*w);
        using Kind = WidgetInstance::PopupRequest::Kind;
        switch (r.kind) {
            case Kind::Close:
                if (passiveWidget_ == key) DismissWidgetPassive(true);
                if (popupWidget_ == key) DismissWidgetPopup(true);
                if (previewOpener_ == key) DismissPreview(true);
                if (slot >= 0 && folderKey_ == items_[static_cast<size_t>(slot)].key) DismissFolderPopup(true);
                if (calendarOpener_ == key) DismissCalendarPopup(true);
                break;
            case Kind::Redraw:
                if (popupWidget_ == key && widgetPopup_.OnScreen()) widgetPopup_.Repaint();
                if (passiveWidget_ == key && widgetPassive_.OnScreen()) widgetPassive_.Repaint();
                break;
            case Kind::Open: {
                if (r.passive) {
                    // The passive popup: no focus, no hold, no label change. Opened again while
                    // shown, it is resized and redrawn in place (a wheel handler opens it per notch).
                    if (slot < 0 || hideState_ != HideState::Shown || fullscreenHidden_) break;
                    if (popupWidget_ == key && widgetPopup_.OnScreen()) {
                        widgetPopup_.Repaint();  // its ordinary popup is open: that one shows the change
                        break;
                    }
                    if (!passiveWidget_.empty() && passiveWidget_ != key) DismissWidgetPassive(false);
                    const DockItem& item = items_[static_cast<size_t>(slot)];
                    passiveWidget_ = key;
                    if (!widgetPassive_.Show(r.width, r.height, WidgetPopupStyle(), PopupAnchorAt(item.targetX + item.slotW / 2.0f))) {
                        passiveWidget_.clear();
                        widgetPassive_.Hide(false);
                    }
                    break;
                }
                if (slot < 0 || hideState_ != HideState::Shown || fullscreenHidden_) break;
                if (passiveWidget_ == key) DismissWidgetPassive(false);  // the ordinary popup replaces it
                DismissPreview(false, false);
                if (popupWidget_ != key) DismissClickPopups(false, false);
                const DockItem& item = items_[static_cast<size_t>(slot)];
                popupWidget_ = key;
                if (!widgetPopup_.Show(r.width, r.height, WidgetPopupStyle(), PopupAnchorAt(item.targetX + item.slotW / 2.0f))) {
                    popupWidget_.clear();
                    MessageBeep(MB_ICONWARNING);
                    break;
                }
                TakePopupHold(kHoldWidget);
                HideLabel();
                RenderWithRecovery([] { return S_OK; });
                break;
            }
            case Kind::Folder: {
                if (slot < 0 || hideState_ != HideState::Shown) break;
                const DockItem& item = items_[static_cast<size_t>(slot)];
                std::wstring name = r.arg;
                while (name.size() > 3 && (name.back() == L'\\' || name.back() == L'/')) name.pop_back();
                const size_t cut = name.find_last_of(L"\\/");
                if (cut != std::wstring::npos && cut + 1 < name.size()) name = name.substr(cut + 1);
                ItemView view = ItemView::Auto;
                FromString(r.view, view);
                ShowFolderPopupAt(r.arg, name, item.targetX + item.slotW / 2.0f, item.key, view);
                break;
            }
            case Kind::Calendar: {
                if (slot < 0 || hideState_ != HideState::Shown) break;
                const DockItem& item = items_[static_cast<size_t>(slot)];
                calendarTarget_ = config_->clock.clickTarget;  // a widget's: the defaults' target
                ShowCalendarPopup(item.targetX + item.slotW / 2.0f, r.day);
                if (calendar_.Visible()) calendarOpener_ = key;
                break;
            }
            case Kind::WeatherSettings:  // weather.openSettings(): this widget item's settings
                if (popupWidget_ == key) DismissWidgetPopup(true);
                OpenItemSettings(slot);
                break;
            case Kind::Previews: {
                if (slot < 0 || hideState_ != HideState::Shown) break;
                // The app's slot on this dock (exe path or AppUserModelID, any case).
                std::wstring want = r.arg;
                for (wchar_t& c : want) c = static_cast<wchar_t>(towlower(c));
                int app = -1;
                for (size_t i = 0; i < items_.size() && app < 0; ++i) {
                    if (items_[i].kind == DockItem::Kind::App && !items_[i].windows.empty() && items_[i].key.id == want) {
                        app = static_cast<int>(i);
                    }
                }
                if (app < 0) {
                    w->Note(L"popup.previews: " + r.arg + L" has no windows on this dock");
                    break;
                }
                DismissClickPopups(false, false);
                const DockItem& item = items_[static_cast<size_t>(slot)];
                previewAnchor_ = PopupAnchorAt(item.targetX + item.slotW / 2.0f);
                ShowPreview(app, true);
                if (preview_.Visible()) previewOpener_ = key;
                break;
            }
            case Kind::None:
            default:
                break;
        }
        // popup.editText (round 6 phase 6C): asked for in the same script call as any popup.
        if (WidgetInstance* still = WidgetByKey(key)) still->OpenPendingEditor();
    }
}

// --- popup.editText (round 6 phase 6C) -------------------------------------------------------------
// The editor (TextEditorWindow) is owned by the dock window and placed on the dock's popup side of
// the widget's slot; topmost when the dock is. While it is open the dock holds itself shown
// (kHoldEditor) and leaves its z-order alone (ApplyWindowPos / AssertTopmost).

WidgetInstance::Client::EditorPlace DockWindow::WidgetEditorPlace(const WidgetInstance& w) const {
    EditorPlace p;
    const int slot = SlotOfWidget(w);
    if (!hwnd_ || slot < 0) return p;
    const D2D1_RECT_F b = SlotScreenBox(slot);
    p.owner = hwnd_;
    p.box = RECT{static_cast<LONG>(std::lround(b.left)), static_cast<LONG>(std::lround(b.top)),
                 static_cast<LONG>(std::lround(b.right)), static_cast<LONG>(std::lround(b.bottom))};
    p.topmost = !(DesktopLayered() || BehindLayered());
    switch (DockCfg().edge) {
        case DockEdge::Top: p.side = 1; break;     // below the slot
        case DockEdge::Left: p.side = 2; break;    // right of it
        case DockEdge::Right: p.side = 3; break;   // left of it
        case DockEdge::Bottom:
        default: p.side = 0; break;                // above it
    }
    return p;
}

void DockWindow::WidgetEditorChanged(WidgetInstance& w, bool open) {
    if (!hwnd_) return;
    if (open) {
        editorWidget_ = w.Key();
        TakePopupHold(kHoldEditor);
        if ((hideState_ == HideState::Hidden || hideState_ == HideState::Hiding) && !RevealSuppressed()) StartShow();
        return;
    }
    if (editorWidget_ != w.Key()) return;
    editorWidget_.clear();
    ReleasePopupHold(kHoldEditor, IsWindowVisible(hwnd_) != FALSE);
}

bool DockWindow::EditorCommand(int index, bool ok) {
    WidgetInstance* w = index == 0xFFFF ? WidgetByKey(editorWidget_) : WidgetAt(index);
    return w && w->CloseEditor(ok);
}

void DockWindow::DismissWidgetPopup(bool animate, bool leaveCheck) {
    if (popupWidget_.empty() && !widgetPopup_.OnScreen()) return;
    widgetPopup_.Hide(animate);
    const std::wstring key = std::move(popupWidget_);
    popupWidget_.clear();
    ReleasePopupHold(kHoldWidget, leaveCheck);
    if (WidgetInstance* w = WidgetByKey(key)) w->PopupClosed();
}

void DockWindow::DismissWidgetPassive(bool animate) {
    if (passiveWidget_.empty() && !widgetPassive_.OnScreen()) return;
    widgetPassive_.Hide(animate);
    const std::wstring key = std::move(passiveWidget_);
    passiveWidget_.clear();
    if (WidgetInstance* w = WidgetByKey(key)) w->PopupClosed(true);
}

// --- Recycle Bin item --------------------------------------------------------------------------
// While a Recycle Bin item is on the dock, the shell notifies the dock window of changes to the
// bin (SHChangeNotifyRegister, shell-level events, new delivery). A burst of them (a large delete
// or an empty) is coalesced by a 200 ms one-shot timer into one icon reload; the icon cache entry
// is dropped so the next draw asks the shell for the empty / full icon again. Nothing polls.

void DockWindow::UpdateRecycleBinWatch() {
    if (!hwnd_) return;
    const bool want = std::any_of(items_.begin(), items_.end(),
                                  [](const DockItem& item) { return item.kind == DockItem::Kind::RecycleBin; });
    if (want == (binNotify_ != 0)) return;
    if (!want) {
        SHChangeNotifyDeregister(binNotify_);
        binNotify_ = 0;
        if (binPending_) {
            KillTimer(hwnd_, kRecycleBinTimerId);
            binPending_ = false;
        }
        return;
    }
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHGetKnownFolderIDList(FOLDERID_RecycleBinFolder, KF_FLAG_DEFAULT, nullptr, &pidl)) || !pidl) return;
    // Recursive: recycling and restoring show up as creates / deletes of the files the bin keeps
    // in each drive's $Recycle.Bin, which the shell reports under the bin's PIDL; emptying also
    // updates the bin itself.
    SHChangeNotifyEntry entry{pidl, TRUE};
    binNotify_ = SHChangeNotifyRegister(hwnd_, SHCNRF_ShellLevel | SHCNRF_NewDelivery,
                                        SHCNE_CREATE | SHCNE_DELETE | SHCNE_MKDIR | SHCNE_RMDIR | SHCNE_RENAMEITEM |
                                            SHCNE_RENAMEFOLDER | SHCNE_UPDATEDIR | SHCNE_UPDATEITEM,
                                        kRecycleBinChangedMessage, 1, &entry);
    CoTaskMemFree(pidl);
    if (!binNotify_) OutputDebugStringW(L"WinDock: SHChangeNotifyRegister (Recycle Bin) failed\n");
}

void DockWindow::OnRecycleBinChanged() {
    for (DockItem& item : items_) {
        if (item.kind != DockItem::Kind::RecycleBin) continue;
        icons_.Evict(item.key);
        item.dirty = true;
    }
    if (hovered_ >= 0 && hovered_ < static_cast<int>(items_.size()) &&
        items_[static_cast<size_t>(hovered_)].kind == DockItem::Kind::RecycleBin) {
        labelDirty_ = true;  // the item count / size in its name
    }
    RenderWithRecovery([this] { return DrawFrame(false); });
}

std::wstring DockWindow::RecycleBinLabel() {
    SHQUERYRBINFO info{sizeof(info)};
    if (FAILED(SHQueryRecycleBinW(nullptr, &info))) return L"Recycle Bin";
    if (info.i64NumItems <= 0) return L"Recycle Bin (empty)";
    wchar_t size[32]{};
    if (FAILED(StrFormatByteSizeEx(static_cast<ULONGLONG>(std::max<__int64>(info.i64Size, 0)),
                                   SFBS_FLAGS_ROUND_TO_NEAREST_DISPLAYED_DIGIT, size, static_cast<UINT>(std::size(size))))) {
        size[0] = 0;
    }
    std::wstring text = L"Recycle Bin (" + std::to_wstring(info.i64NumItems) +
                        (info.i64NumItems == 1 ? L" item" : L" items");
    if (size[0]) text += std::wstring(L", ") + size;
    return text + L")";
}

// --- Acrylic backdrop ------------------------------------------------------------------------
// The blur is a separate window (Backdrop) beneath the dock window. DirectComposition animations
// cannot move a window, so while the bar's edges or the slide offset animate, a one-shot 16 ms
// timer chain samples the same Tweens and moves the window; it stops as soon as they settle, so
// nothing runs at rest. The auto-hide slide uses the same chain (the blur slides with the bar and
// is clipped at the monitor's bottom edge) instead of hiding the blur, which would make the bar
// visibly lose its blur at the start of every hide.

void DockWindow::UpdateAcrylic() {
    // The blur window is a flat rectangle: not with the glass shelf or the perspective tilt.
    // Nor on the desktop layer (the blur window would have to follow the dock's z-order and
    // visibility there; the bar uses its plain fill). Nor on a ring: DWM draws a system backdrop
    // over the blur window's whole rectangle and ignores its window region (a grey box around the
    // ring on screen, seen in the 2026-11-03 verification), so a ring uses its plain fill too.
    // (A "behind" dock keeps its blur: the desktop layer keeps the backdrop window beneath it.)
    const bool want = DockTheme().acrylic && !DockTheme().shelf && !DockTheme().perspective &&
                      DockCfg().layer != DockLayer::Desktop && !DockCfg().Polar() && Backdrop::SystemAllowsTransparency();
    if (want && !backdrop_.Exists()) backdrop_.Create(instance_);
    acrylic_ = want && backdrop_.Exists();
    if (acrylic_) {
        acrylicRadiusDip_ = backdrop_.SetCornerRadius(DockCfg().fullWidth ? 0.0f : DockTheme().cornerRadius);
        return;
    }
    if (backdropPending_) {
        KillTimer(hwnd_, kBackdropTimerId);
        backdropPending_ = false;
    }
    backdrop_.Destroy();
}

void DockWindow::SyncBackdrop() {
    if (!acrylic_ || !hwnd_) return;
    const Layout& l = layout_;
    const double now = Renderer::Now();
    bool animating = false;
    auto value = [&](const Tween& t, float fallback) {
        if (!t.valid) return fallback;
        if (t.Running(now)) animating = true;
        return t.Value(now);
    };
    RECT r{};  // empty: hidden
    // hideStyle "fade": the blur cannot fade, so it goes at the start of a hide and comes back at
    // the end of a show.
    const bool fadeHidden = motion_.hide == HideStyle::Fade &&
                            (hideState_ == HideState::Hiding || hideState_ == HideState::Showing);
    if (hideState_ != HideState::Hidden && !fadeHidden && IsWindowVisible(hwnd_)) {
        const bool shown = hideState_ == HideState::Shown || hideState_ == HideState::Showing;
        const float left = value(bgLeftX_, bgLeftTarget_);
        const float right = value(bgRightX_, bgRightTarget_) + l.bgSurfaceW;
        // The slide is a physical offset on the cross axis (signed: toward the edge).
        const float dy = value(rootY_, shown ? 0.0f : DockGeometry::OutwardSign(l.edge) * l.hiddenSlide);
        D2D1_RECT_F p = l.PhysRect(left, l.bar.top, right, l.bar.bottom);
        if (IsVertical(l.edge)) {
            p.left += dy;
            p.right += dy;
        } else {
            p.top += dy;
            p.bottom += dy;
        }
        r.left = l.window.left + std::lround(p.left);
        r.right = l.window.left + std::lround(p.right);
        r.top = l.window.top + std::lround(p.top);
        r.bottom = l.window.top + std::lround(p.bottom);
        // Sliding out: clip at the monitor's edge (a monitor beyond it must not show the blur).
        switch (l.edge) {
            case DockEdge::Top: r.top = std::max(r.top, l.monitor.top); break;
            case DockEdge::Left: r.left = std::max(r.left, l.monitor.left); break;
            case DockEdge::Right: r.right = std::min(r.right, l.monitor.right); break;
            case DockEdge::Bottom:
            default: r.bottom = std::min(r.bottom, l.monitor.bottom); break;
        }
        // A full-width bar's ends move past the monitor's while it grows: the blur stays on it.
        r.left = std::max(r.left, l.monitor.left);
        r.right = std::min(r.right, l.monitor.right);
        r.top = std::max(r.top, l.monitor.top);
        r.bottom = std::min(r.bottom, l.monitor.bottom);
        if (r.right <= r.left || r.bottom <= r.top) r = RECT{};
    }
    backdrop_.Place(hwnd_, r);
    if (animating && !backdropPending_) {
        SetTimer(hwnd_, kBackdropTimerId, 16, nullptr);
        backdropPending_ = true;
    }
}

// --- Animation settings ----------------------------------------------------------------------
// Every animation duration comes from motion_: the speed-1 constant divided by animation.speed.
// speed 0 or reduce-motion make them 0 (instant: hide/show, return to rest, landing, label, popup
// fade; no launch feedback). The magnification follow (input smoothing while the mouse moves)
// keeps its 80 ms with reduce-motion and is only 0 with speed 0. End-of-animation timers use the
// same scaled values (TimerMs floors them at USER_TIMER_MINIMUM).

bool DockWindow::SystemReducesMotion() {
    BOOL animations = TRUE;
    if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0)) return false;
    return animations == FALSE;
}

void DockWindow::RefreshMotion() {
    const AnimationConfig& a = config_->animation;
    Motion m;
    m.reduce = a.reduceMotion == ReduceMotion::On || (a.reduceMotion == ReduceMotion::System && SystemReducesMotion());
    m.scale = m.reduce || a.speed <= 0.0 ? 0.0 : 1.0 / a.speed;
    m.hide = a.hideStyle;
    m.launch = m.scale > 0.0 ? a.launchStyle : LaunchStyle::None;
    m.falloff = a.magnifyFalloff;
    m.easing = a.easing;
    m.magnify = a.speed <= 0.0 ? 0.0 : kMagnifyDurS;
    m.rest = kRestDurS * m.scale;
    m.slide = a.hideStyle == HideStyle::None ? 0.0 : kSlideDurS * m.scale;
    m.bounceHalf = kBounceHalfS * m.scale;
    m.removeFade = kRemoveFadeS * m.scale;
    m.labelFade = static_cast<double>(std::max(a.labelFadeMs, 0)) / 1000.0 * m.scale;
    m.previewFade = kPreviewFadeS * m.scale;
    motion_ = m;
    renderer_.SetDefaultEasing(m.easing);
    preview_.SetFadeSeconds(m.previewFade);
    folder_.SetFadeSeconds(m.previewFade);
    weatherPopup_.SetFadeSeconds(m.previewFade);
    calendar_.SetFadeSeconds(m.previewFade);
}

// --- Auto-hide -------------------------------------------------------------------------------
// Shown --(mouse leaves, hideDelayMs)--> Hiding --(slide/fade done)--> Hidden (1 px strip)
// Hidden --(mouse on the strip for revealDelayMs, no fullscreen app)--> Showing --> Shown.
// autoHide "intelligent" adds: the hide delay only starts (and only ends in a hide) while the
// foreground window overlaps the dock (overlapped_, kept current by watch_), and the dock shows
// by itself, without the strip, as soon as that window stops overlapping it.
// Hiding <-> Showing reverse smoothly: the root's slide offset and opacity are Tweens, so a new
// animation starts from the interpolated current value, with a duration proportional to the
// remaining distance. All waits are one-shot SetTimers.

void DockWindow::ScheduleHide() {
    if (!hwnd_ || !DockCfg().AutoHideOn() || Held() || hidePending_ || fullscreenHidden_) return;
    // Intelligent: re-test now (cheap; this runs on a mouse leave, not while idle) rather than
    // trusting overlapped_, which may be stale after a suppressed show (see OnOverlapCheck).
    if (DockCfg().autoHide == AutoHideMode::Intelligent) {
        overlapped_ = ForegroundOverlaps();
        if (!overlapped_) return;
    }
    if (hideState_ == HideState::Hidden || hideState_ == HideState::Hiding) return;
    SetTimer(hwnd_, kHideTimerId, std::max<UINT>(USER_TIMER_MINIMUM, static_cast<UINT>(DockCfg().hideDelayMs)),
             nullptr);
    hidePending_ = true;
}

void DockWindow::CancelHide() {
    if (!hidePending_) return;
    KillTimer(hwnd_, kHideTimerId);
    hidePending_ = false;
}

void DockWindow::OnHideTimer() {
    KillTimer(hwnd_, kHideTimerId);
    hidePending_ = false;
    if (!DockCfg().AutoHideOn() || Held()) return;
    if (DockCfg().autoHide == AutoHideMode::Intelligent) {
        overlapped_ = ForegroundOverlaps();
        if (!overlapped_) return;
    }
    if (hideState_ == HideState::Hidden || hideState_ == HideState::Hiding) return;
    if (CursorInZone()) return;  // came back; its WM_MOUSEMOVE cancels the next attempt anyway
    StartHide();
}

void DockWindow::StartHide() {
    CancelHide();
    DismissPreview(false, false);
    DismissClickPopups(false, false);
    DismissWidgetPassive(false);
    if (magnifyPending_) {
        KillTimer(hwnd_, kMagnifyTimerId);
        magnifyPending_ = false;
    }
    magnifying_ = false;
    grown_ = false;  // slides off at rest
    SetHover(-1);
    hideState_ = HideState::Hiding;
    SyncWidgetVisibility();
    UpdateWindowRect();  // bar -> bar column down to the screen edge (room for the slide)
    double duration = 0.0;
    RenderWithRecovery([&] {
        ComputeTargets();
        ApplyTargets(motion_.rest);  // normally already at rest
        duration = ApplyRootPose(motion_.slide);
        return S_OK;
    });
    SetTimer(hwnd_, kSlideTimerId, TimerMs(duration + 0.02), nullptr);
    UpdateWindowRegion();  // a ring: no band region while it moves
}

void DockWindow::StartShow() {
    CancelHide();
    if (revealPending_) {
        KillTimer(hwnd_, kRevealTimerId);
        revealPending_ = false;
    }
    const bool wasHidden = hideState_ == HideState::Hidden;
    hideState_ = HideState::Showing;
    if (wasHidden) expanded_ = false;
    SyncWidgetVisibility();
    UpdateWindowRect();  // strip -> bar column (the content is still transparent)
    double duration = 0.0;
    RenderWithRecovery([&] {
        duration = ApplyRootPose(motion_.slide);
        return S_OK;
    });
    SetTimer(hwnd_, kSlideTimerId, TimerMs(duration + 0.02), nullptr);
    UpdateWindowRegion();  // a ring: no band region while it moves
}

void DockWindow::OnSlideTimer() {
    KillTimer(hwnd_, kSlideTimerId);
    if (hideState_ == HideState::Hiding) {
        hideState_ = HideState::Hidden;
        expanded_ = false;
        // The hidden pose (slideOnly: the bar below the edge also becomes transparent, so nothing
        // that rises out of it, like a launch bounce, shows in the strip).
        RenderWithRecovery([this] {
            ApplyRootPose(0.0);
            return S_OK;
        });
        UpdateWindowRect();  // shrink to the reveal strip
        SyncBackdrop();      // hide the blur
        AssertTopmost();     // a monitor-sized window in front may have been put above the dock
        // If the cursor already sits on the edge (e.g. it was below the dock), require it to
        // leave the strip first; otherwise the dock would reveal itself right away.
        revealArmed_ = !CursorOnStrip();
    } else if (hideState_ == HideState::Showing) {
        hideState_ = HideState::Shown;
        // Revealed from the bottom edge, the cursor usually sits in the margin below the bar:
        // that counts as being on the dock (expand), not as having left it (shrink and hide).
        const bool onDock = CursorInZone();  // tested against the bar column, before resizing
        if (onDock) expanded_ = true;
        UpdateWindowRect();  // bar column -> bar, or -> stage
        SyncBackdrop();      // hideStyle "fade": the blur comes back now
        if (!onDock) LeaveZone();  // revealed but the mouse is not on the dock
    }
    UpdateWindowRegion();  // a ring at rest again: its band
}

void DockWindow::OnRevealTimer() {
    KillTimer(hwnd_, kRevealTimerId);
    revealPending_ = false;
    if (hideState_ != HideState::Hidden || !DockCfg().AutoHideOn() || !CursorOnStrip()) return;
    if (RevealSuppressed()) return;  // a fullscreen app / presentation: stay hidden
    StartShow();
}

bool DockWindow::RevealSuppressed() const {
    QUERY_USER_NOTIFICATION_STATE quns{};
    if (SUCCEEDED(SHQueryUserNotificationState(&quns)) &&
        (quns == QUNS_RUNNING_D3D_FULL_SCREEN || quns == QUNS_PRESENTATION_MODE || quns == QUNS_BUSY)) {
        wchar_t buf[96];
        swprintf_s(buf, L"WinDock: reveal suppressed (notification state %d)\n", static_cast<int>(quns));
        OutputDebugStringW(buf);
        return true;
    }
    return false;
}

bool DockWindow::ForegroundOverlaps() const {
    const HWND fg = GetForegroundWindow();
    if (!fg || !hwnd_) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    // The dock's own windows (the settings window, an activated click popup) never overlap it.
    if (pid == GetCurrentProcessId()) return false;
    if (IsIconic(fg) || !IsWindowVisible(fg)) return false;
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(fg, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return false;
    if (WindowTracker::IsExcludedClass(fg)) return false;  // desktop, taskbars, Start / Search
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(fg, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))) && !GetWindowRect(fg, &r)) {
        return false;
    }
    // The bar's column from the resting bar's top to the screen edge (the edge margin included:
    // a window there sits between the bar and the reveal strip).
    const Layout& l = layout_;
    const RECT dock = l.polar.on ? RingScreenRect() : StageToScreen(l.bar.left, l.bar.top, l.bar.right, l.stageCross, true);
    RECT hit{};
    return IntersectRect(&hit, &r, &dock) != FALSE;
}

void DockWindow::OnOverlapCheck(bool force) {
    // (Not while the full-screen guard hides the dock: its release re-tests with `force`.)
    if (!hwnd_ || DockCfg().autoHide != AutoHideMode::Intelligent || fullscreenHidden_) return;
    const bool overlapped = ForegroundOverlaps();
    if (overlapped == overlapped_ && !force) return;
    const bool hidden = hideState_ == HideState::Hidden || hideState_ == HideState::Hiding;
    if (!overlapped && hidden && RevealSuppressed()) {
        // A fullscreen app / presentation in front: stay hidden and keep the old state, so the
        // next event (that app going away) counts as a change and re-tests.
        return;
    }
    overlapped_ = overlapped;
    if (overlapped) {
        // Like the mouse leaving: hide after hideDelayMs, unless the mouse is on the dock or a
        // menu / drag holds it (leaving / releasing schedules the hide then).
        if (hideState_ == HideState::Hidden) {
            AssertTopmost();  // keep the reveal strip above the window that just covered the dock
        } else if (hideState_ != HideState::Hiding && !Held() && !CursorInZone()) {
            ScheduleHide();
        }
        return;
    }
    CancelHide();
    if (hidden) StartShow();
}

void DockWindow::AssertTopmost() {
    // A newly activated window that covers its whole monitor (maximized with the taskbar hidden,
    // borderless fullscreen) can end up above the topmost dock, burying the 1 px reveal strip.
    // Re-asserting the z-order is cheap and only happens on hide transitions. Not while a modal
    // dialog owned by the dock is open, nor while a widget's text editor is (see ApplyWindowPos),
    // nor (round 9 phase 9A) while the full-screen guard hides the dock: no z-order call at all.
    if (!hwnd_ || !IsWindowEnabled(hwnd_) || fullscreenApp_ || fullscreenHidden_ || !editorWidget_.empty()) return;
    if (BehindLayered() && layerRegistered_) {
        DesktopLayer::Place(hwnd_);  // no-op while it is in place
        return;
    }
    if (DesktopLayered() || BehindLayered()) {
        DesktopLayer::PlaceAboveDesktop(hwnd_);
        return;
    }
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void DockWindow::SyncLayer() {
    if (!hwnd_) return;
    if (BehindLayered()) {
        if (!layerRegistered_) {
            // Out of the topmost band first: the layer places it after another app's window, but
            // with no such window it would go to the top and stay topmost.
            SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            layerRegistered_ = true;
        }
        // (Again: the backdrop may have come or gone; Register places the window.)
        DesktopLayer::Register(hwnd_, acrylic_ ? backdrop_.Hwnd() : nullptr);
        return;
    }
    if (layerRegistered_) {
        DesktopLayer::Unregister(hwnd_);
        layerRegistered_ = false;
    }
}

void DockWindow::OnDashboardChanged() {
    if (!hwnd_ || DockCfg().layer == DockLayer::Normal) return;
    const bool on = InDashboard();
    // A "behind" dock leaves the layer while lifted (its z-order lock would drop the raise).
    SyncLayer();
    if (on) {
        // Shown and revealed (Held() keeps it from hiding while the mode lasts).
        CancelHide();
        if ((hideState_ == HideState::Hidden || hideState_ == HideState::Hiding) && !RevealSuppressed()) StartShow();
    }
    UpdateDesktopShown();  // a "desktop" dock: shown while lifted, its foreground rule again after
    AssertTopmost();       // topmost while lifted; back above the desktop window after
    if (backdrop_.Exists()) backdrop_.Restack(hwnd_);
    if (!on) ApplyAutoHide();
    SyncWidgetVisibility();
}

void DockWindow::ReassertLayer() {
    if (!hwnd_) return;
    if (DesktopLayered() && IsWindowVisible(hwnd_)) DesktopLayer::PlaceAboveDesktop(hwnd_);
    // A "behind" dock is registered: DesktopLayer::Reassert covers it.
}

bool DockWindow::FloatingShown() const { return hwnd_ && DockCfg().Floating() && IsWindowVisible(hwnd_); }

void DockWindow::OnWatchEvent() {
    if (DesktopLayered()) UpdateDesktopShown();
    if (DockCfg().autoHide == AutoHideMode::Intelligent) OnOverlapCheck();
}

void DockWindow::UpdateDesktopShown() {
    if (!hwnd_) return;
    const bool hide = DesktopLayered() && !DesktopLayer::DesktopForeground();
    if (hide == desktopHidden_) return;
    desktopHidden_ = hide;
    if (hide) {
        // Another window came to the foreground: the dock leaves with its popups and hover state.
        DismissPreview(false, false);
        DismissClickPopups(false, false);
        DismissWidgetPassive(false);
        if (dragPhase_ == DragPhase::Dragging) EndDrag(false);
        if (dockDrag_) EndDockDrag(false);
        ShowWindow(hwnd_, SW_HIDE);
        LeaveZone();
        SyncWidgetVisibility();
        return;
    }
    // The desktop is the foreground again (or the dock left the desktop layer): back, above the
    // desktop window (or topmost). Not while the full-screen guard hides it (it shows it again).
    if (fullscreenHidden_) return;
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    AssertTopmost();
    SyncWidgetVisibility();
}

bool DockWindow::TopmostBand() const { return hwnd_ && (DockCfg().layer == DockLayer::Normal || InDashboard()); }

void DockWindow::SetFullscreenHidden(bool hidden) {
    if (!hwnd_ || hidden == fullscreenHidden_) return;
    fullscreenHidden_ = hidden;
    if (hidden) {
        // A full-screen app is in front on this dock's monitor (DockManager's guard): out of its way
        // without touching the z-order (a topmost window placed over an exclusive full-screen game
        // ends its mode). Its popups, label and previews close, its hover / hide / reveal timers stop;
        // the hide state stays as it was.
        DismissPreview(false, false);
        DismissClickPopups(false, false);
        DismissWidgetPassive(false);
        reminder_.Hide(false);  // the waiting reminders come back with the dock
        if (dragPhase_ == DragPhase::Dragging) EndDrag(false);
        if (dockDrag_) EndDockDrag(false);
        CancelHide();
        if (revealPending_) {
            KillTimer(hwnd_, kRevealTimerId);
            revealPending_ = false;
        }
        ShowWindow(hwnd_, SW_HIDE);
        LeaveZone();      // the magnify timer, the hover and the label (ScheduleHide is a no-op now)
        SyncBackdrop();   // the blur goes with the window
        SyncWidgetVisibility();
        return;
    }
    // The full-screen app is gone: back as it was (a "desktop" dock only while the desktop is in
    // front), on top once, then the auto-hide rules again.
    if (!desktopHidden_) ShowWindow(hwnd_, SW_SHOWNA);
    AssertTopmost();
    if (backdrop_.Exists()) backdrop_.Restack(hwnd_);
    SyncBackdrop();
    SyncWidgetVisibility();
    if (!reminders_.empty() && !desktopHidden_) ShowFrontReminder();
    if (DockCfg().autoHide == AutoHideMode::Intelligent) {
        OnOverlapCheck(true);
    } else if (DockCfg().AutoHideOn() && hideState_ == HideState::Shown && !CursorInZone()) {
        ScheduleHide();
    }
}

void DockWindow::SyncWidgetVisibility() {
    const bool visible = !desktopHidden_ && (hideState_ == HideState::Shown || hideState_ == HideState::Showing);
    for (auto& [key, w] : widgets_) w->SetVisible(visible);
    // Window thumbnails capture only while the dock can be seen (hidden: sessions closed, no timer).
    thumbs_.Activate(visible);
}

void DockWindow::SyncThumbnails() {
    if (!hwnd_) return;
    const DockConfig& d = DockCfg();
    if (d.thumbnails == ThumbnailMode::Live && !renderer_.Multithreaded()) {
        // Live capture was turned on: the D3D device must be multithreaded. Rebuild it (and every
        // surface) once, the device-loss path; it syncs the thumbnails again at its end.
        renderer_.SetMultithreaded(true);
        thumbs_.Reset();
        RecoverDevice();
        return;
    }
    thumbs_.Configure(d.thumbnails, d.thumbnailFps, d.thumbnailIntervalS);
    if (d.thumbnails == ThumbnailMode::Off) return;
    std::vector<HWND> windows;
    for (const DockItem& item : items_) {
        if (item.thumbWindow && std::find(windows.begin(), windows.end(), item.thumbWindow) == windows.end()) {
            windows.push_back(item.thumbWindow);
        }
    }
    // Frames are kept at the magnified tile's pixel size (upright: on a vertical dock the tile is an
    // icon wide and thumbnailSize tall), so a magnified tile stays sharp.
    const float k = std::max(1.0f, layout_.magnification);
    const D2D1_SIZE_F tile = layout_.PhysSize(TilePx() * k, std::max(1.0f, layout_.iconPx) * k);
    thumbs_.SetWindows(windows, static_cast<UINT>(std::lround(tile.width)), static_cast<UINT>(std::lround(tile.height)));
    thumbs_.Activate(!desktopHidden_ && (hideState_ == HideState::Shown || hideState_ == HideState::Showing));
}

void DockWindow::OnThumbnailFrames(const std::vector<HWND>& windows) {
    bool any = false;
    for (DockItem& item : items_) {
        if (!item.thumbWindow || std::find(windows.begin(), windows.end(), item.thumbWindow) == windows.end()) continue;
        item.dirty = true;
        any = true;
    }
    // One redraw for the frames of every window that arrive together.
    if (any && !thumbRedrawPosted_ && hwnd_) thumbRedrawPosted_ = PostMessageW(hwnd_, kThumbRedrawMessage, 0, 0) != FALSE;
}

double DockWindow::ApplyRootPose(double duration) {
    IDCompositionVisual2* root = renderer_.Root();
    if (!root) return 0.0;
    if (!rootY_.valid) ApplyStageTransform();  // a fresh root (device recovery)
    const HideStyle style = motion_.hide;
    if (style == HideStyle::None) duration = 0.0;
    if (layout_.shape == DockShape::Circle && layout_.polar.on && rootScale_) {
        // A circle does not slide: it shrinks about its centre to the hub (kept a little
        // transparent) and grows back; the hidden window is the hub (HiddenRect).
        const bool shown = hideState_ == HideState::Shown || hideState_ == HideState::Showing;
        const double now = Renderer::Now();
        rootY_ = Tween{0.0f, 0.0f, now, 0.0, true};
        renderer_.Drive(rootScaleT_, shown ? 1.0f : layout_.hubScale, duration, now, [&](auto v) {
            HRESULT hr = rootScale_->SetScaleX(v);
            if (SUCCEEDED(hr)) hr = rootScale_->SetScaleY(v);
            return hr;
        }, 0.0005f, renderer_.FadeEasing());
        if (IDCompositionVisual3* root3 = renderer_.Root3()) {
            renderer_.Drive(
                rootOpacity_, shown ? 1.0f : kHubOpacity, duration, now, [&](auto v) { return root3->SetOpacity(v); },
                0.001f, renderer_.FadeEasing());
        }
        return duration;
    }
    // With a peek strip the dock always moves to it (also with "fade").
    const bool peek = layout_.peek > 0.0f;
    const bool moves = style != HideStyle::Fade || peek;                     // slide, slideOnly, none
    const bool fades = style == HideStyle::Slide || style == HideStyle::Fade;  // animated opacity
    const bool shown = hideState_ == HideState::Shown || hideState_ == HideState::Showing;
    // The slide: a physical offset on the cross axis toward the screen edge (root OffsetY, or
    // OffsetX on vertical docks; negative for top / left docks).
    const float travel = layout_.hiddenSlide;
    const float targetY = shown || !moves ? 0.0f : DockGeometry::OutwardSign(layout_.edge) * travel;
    // slideOnly keeps full opacity while it slides and is transparent (or at peekOpacity) only once
    // Hidden.
    const float hiddenOpacity = peek ? static_cast<float>(DockCfg().peekOpacity) : 0.0f;
    const float targetOpacity =
        shown ? 1.0f : (fades || style == HideStyle::None || hideState_ == HideState::Hidden ? hiddenOpacity : 1.0f);
    const double now = Renderer::Now();
    if (duration > 0.0) {
        // A reversal mid-slide only travels the remaining distance, at the same speed. The
        // fraction comes from the property that animates (the opacity when it does).
        float remaining = 1.0f;
        if (fades && rootOpacity_.valid) {
            remaining = std::fabs(targetOpacity - rootOpacity_.Value(now));
        } else if (!fades && rootY_.valid && travel > 0.0f) {
            remaining = std::min(1.0f, std::fabs(targetY - rootY_.Value(now)) / travel);
        }
        if (fades ? rootOpacity_.valid : rootY_.valid) {
            duration = std::max(std::min(0.04, duration), duration * static_cast<double>(remaining));
        }
    }
    const double durationY = moves ? duration : 0.0;
    const double durationOpacity = fades ? duration : 0.0;
    // The slide is on the edge's own axis (a ring is laid out horizontally whatever its edge).
    const bool slideX = IsVertical(layout_.edge);
    renderer_.Drive(rootY_, targetY, durationY, now, [&](auto v) { return slideX ? root->SetOffsetX(v) : root->SetOffsetY(v); });
    if (IDCompositionVisual3* root3 = renderer_.Root3()) {
        renderer_.Drive(
            rootOpacity_, targetOpacity, durationOpacity, now, [&](auto v) { return root3->SetOpacity(v); }, 0.001f,
            renderer_.FadeEasing());
    } else {
        const float current = rootOpacity_.Value(now);  // bookkeeping only
        rootOpacity_ = Tween{current, targetOpacity, now, durationOpacity, true, renderer_.FadeEasing()};
    }
    return duration;
}

bool DockWindow::CursorOnStrip() const {
    POINT pt{};
    if (!GetCursorPos(&pt)) return false;
    const RECT strip = HiddenRect();
    return PtInRect(&strip, pt) != FALSE;
}

void DockWindow::ArmMouseTracking() {
    // WS_EX_NOACTIVATE windows still get WM_MOUSELEAVE, but tracking ends with every leave and
    // must be re-armed on the next entry.
    if (trackingMouse_) return;
    TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
    trackingMouse_ = TrackMouseEvent(&tme) != FALSE;
}

void DockWindow::ApplyAutoHide() {
    if (!hwnd_) return;
    UpdateAppBar();  // only a dock that does not auto-hide reserves space
    // A desktop-layer dock follows the foreground too (shown only while it is the desktop).
    if (DesktopLayered()) SetWatchingForeground(true);
    if (DockCfg().autoHide != AutoHideMode::Intelligent) {
        // Off / always: no hooks (unless the desktop layer or another dock needs them).
        if (!DesktopLayered()) SetWatchingForeground(false);
        overlapped_ = false;
    }
    if (!DockCfg().AutoHideOn()) {
        CancelHide();
        if (revealPending_) {
            KillTimer(hwnd_, kRevealTimerId);
            revealPending_ = false;
        }
        if (hideState_ == HideState::Hidden || hideState_ == HideState::Hiding) StartShow();
        return;
    }
    if (DockCfg().autoHide == AutoHideMode::Intelligent) {
        SetWatchingForeground(true);
        OnOverlapCheck(true);  // hide if overlapped now, show if hidden and not
        return;
    }
    if ((hideState_ == HideState::Shown || hideState_ == HideState::Showing) && !Held() && !CursorInZone()) {
        ScheduleHide();
    }
}

void DockWindow::Flash(uint32_t argb) {
    if (!hwnd_) return;
    flashColor_ = argb | 0xFF000000u;
    flashSteps_ = kFlashSteps;  // again from the start if it was flashing
    // Revealed as SetKeepShown does (Held() holds it while the steps run).
    CancelHide();
    if ((hideState_ == HideState::Hidden || hideState_ == HideState::Hiding) && !RevealSuppressed()) StartShow();
    RenderWithRecovery([this] { return RenderBackground(); });
    SetTimer(hwnd_, kFlashTimerId, kFlashStepMs, nullptr);  // replaces a running step
}

void DockWindow::OnFlashTimer() {
    KillTimer(hwnd_, kFlashTimerId);  // one-shot, re-armed per step
    if (flashSteps_ <= 0) return;
    --flashSteps_;
    RenderWithRecovery([this] { return RenderBackground(); });
    if (flashSteps_ > 0) {
        SetTimer(hwnd_, kFlashTimerId, kFlashStepMs, nullptr);
        return;
    }
    ApplyAutoHide();  // over: hides after the delay if auto-hide wants it now
}

void DockWindow::SetKeepShown(bool keep) {
    if (keep == keepShown_ || !hwnd_) return;
    keepShown_ = keep;
    if (keep) {
        CancelHide();
        if ((hideState_ == HideState::Hidden || hideState_ == HideState::Hiding) && !RevealSuppressed()) StartShow();
        return;
    }
    ApplyAutoHide();  // hides after the delay if auto-hide wants it now
}

RECT DockWindow::RestBarRect() const {
    const Layout& l = layout_;
    return StageToScreen(l.bar.left, l.bar.top, l.bar.right, l.bar.bottom, true);
}

RECT DockWindow::RestFootprintRect() const {
    // Round 12 phase 12H: the bar grown across its run by a title drawn outside it (measured: the
    // Dock settings panel of a left dock titled "Tools" covered the title, 44 px of it). Along the
    // run it stays the bar's, so what is centred on the bar stays centred on it.
    RECT r = RestBarRect();
    const Layout& l = layout_;
    const D2D1_RECT_F& t = l.title;
    if (t.right <= t.left || t.bottom <= t.top) return r;
    const RECT tr{l.window.left + static_cast<LONG>(std::floor(t.left)), l.window.top + static_cast<LONG>(std::floor(t.top)),
                  l.window.left + static_cast<LONG>(std::ceil(t.right)), l.window.top + static_cast<LONG>(std::ceil(t.bottom))};
    if (l.vertical) {
        r.left = std::min(r.left, tr.left);
        r.right = std::max(r.right, tr.right);
    } else {
        r.top = std::min(r.top, tr.top);
        r.bottom = std::max(r.bottom, tr.bottom);
    }
    return r;
}

void DockWindow::SetHold(bool hold) {
    if (hold == hold_ || !hwnd_) return;
    hold_ = hold;
    if (hold) {
        CancelHide();
        SuppressPreview();  // a menu opens or a drag starts
        DismissClickPopups(true, false);
        HideLabel();
        RenderWithRecovery([] { return S_OK; });  // commit the label hide
        return;
    }
    // A menu's modal loop may have swallowed WM_MOUSELEAVE: re-arm tracking on the next move.
    trackingMouse_ = false;
    if (hideState_ == HideState::Hidden) return;
    if (!CursorInZone()) LeaveZone();
}

// --- Items -----------------------------------------------------------------------------------

void DockWindow::ResolvePins() {
    std::vector<AppIdentity::ResolvedPin> pins;
    std::unordered_map<std::wstring, AppIdentity::ResolvedPin> cache;
    const std::vector<DockItemEntry>& entries = DockCfg().items;
    pins.reserve(entries.size());
    size_t separators = 0, groups = 0;
    // An app / folder entry (a pin, or a group's member), from the cache when it is unchanged.
    auto resolve = [&](const DockItemEntry& e) {
        // The icon and kind are part of the id: editing either re-resolves the entry.
        std::wstring id = e.kind + L"|" + e.icon + L"|" + (e.path.empty() ? L"a:" + e.aumid : L"p:" + e.path);
        auto it = pinCache_.find(id);
        AppIdentity::ResolvedPin pin = it != pinCache_.end() ? it->second : AppIdentity::ResolvePin(e);
        cache[std::move(id)] = pin;
        return pin;
    };
    // The pin kinds (app / folder / separator / recycleBin), in item order: pins_ is parallel to
    // DockConfig::PinIndexes(), and each pin element shows pins_[its pin index].
    for (const DockItemEntry& e : entries) {
        const ItemKind kind = e.Kind();
        if (!IsPinKind(kind)) continue;
        // Separators and the Recycle Bin resolve without any file access: not cached (a
        // separator's key depends on its position among the separators).
        if (kind == ItemKind::Separator || kind == ItemKind::RecycleBin) {
            pins.push_back(AppIdentity::ResolvePin(e, kind == ItemKind::Separator ? separators++ : 0));
            continue;
        }
        if (kind == ItemKind::Group) {
            AppIdentity::ResolvedPin group = AppIdentity::ResolvePin(e, groups++);
            group.members.reserve(e.members.size());
            for (const DockItemEntry& m : e.members) group.members.push_back(resolve(m));
            pins.push_back(std::move(group));
            continue;
        }
        pins.push_back(resolve(e));
    }
    pins_ = std::move(pins);
    pinCache_ = std::move(cache);  // drops entries that are no longer pinned
}

void DockWindow::BuildElements() {
    // One element per item, in order (an unknown kind gets none and is not shown). The elements
    // hold no visuals: the slots (items_) keep them across a rebuild, matched by key.
    std::vector<std::unique_ptr<DockElement>> elements;
    const std::vector<DockItemEntry>& entries = DockCfg().items;
    elements.reserve(entries.size());
    size_t pinIndex = 0;
    size_t clocks = 0, weathers = 0, trays = 0, bells = 0;
    std::unordered_map<std::wstring, size_t> widgetIds;  // script widgets: ordinal per id
    widgetsClaimed_.clear();
    for (const DockItemEntry& e : entries) {
        const ItemKind kind = e.Kind();
        const size_t ordinal = kind == ItemKind::Clock     ? clocks++
                               : kind == ItemKind::Weather ? weathers++
                               : kind == ItemKind::Widget  ? widgetIds[e.id]++
                               : kind == ItemKind::Tray    ? trays++
                               : kind == ItemKind::Notifications ? bells++
                                                           : 0;
        elements.push_back(MakeElement(*this, e, pinIndex, ordinal));
        if (IsPinKind(kind)) ++pinIndex;
    }
    elements_ = std::move(elements);
    // Script widgets no item shows any more go (their runtimes with them).
    std::erase_if(widgets_, [&](const auto& kv) {
        return std::find(widgetsClaimed_.begin(), widgetsClaimed_.end(), kv.first) == widgetsClaimed_.end();
    });
    widgetsClaimed_.clear();
    if (!popupWidget_.empty() && !WidgetByKey(popupWidget_)) DismissWidgetPopup(false);  // its widget went
    if (!passiveWidget_.empty() && !WidgetByKey(passiveWidget_)) DismissWidgetPassive(false);
    if (!editorWidget_.empty() && !WidgetByKey(editorWidget_)) {  // its widget went (with its editor)
        editorWidget_.clear();
        ReleasePopupHold(kHoldEditor, true);
    }
}

WidgetInstance* DockWindow::AcquireWidget(const DockItemEntry& entry, size_t ordinal) {
    if (!renderer_.D2DFactory() || !renderer_.DWrite() || !shared_) return nullptr;
    const std::wstring key = entry.id + L"#" + std::to_wstring(ordinal);
    widgetsClaimed_.push_back(key);
    // Round 5: the place its `weather` provider answers for (its own, else this dock's first
    // weather item's, else the defaults).
    const WeatherConfig place = ScriptWeatherOf(*config_, DockCfg(), entry);
    if (auto it = widgets_.find(key); it != widgets_.end()) {
        it->second->SetWeatherConfig(place);
        it->second->SetSettings(entry.settings);
        return it->second.get();
    }
    ScriptDockInfo info = WidgetDockInfo(*this);
    info.visible = !desktopHidden_ && (hideState_ == HideState::Shown || hideState_ == HideState::Showing);
    auto widget = std::make_unique<WidgetInstance>(shared_->Widgets(), static_cast<WidgetInstance::Client&>(*this),
                                                   renderer_.D2DFactory(), renderer_.DWrite(), entry.id, key,
                                                   entry.settings, info, place);
    WidgetInstance* raw = widget.get();
    widgets_.emplace(key, std::move(widget));
    return raw;
}

WidgetInstance* DockWindow::WidgetAt(int index) const {
    if (index < 0 || index >= static_cast<int>(items_.size())) return nullptr;
    const DockItem& item = items_[static_cast<size_t>(index)];
    if (item.kind != DockItem::Kind::Widget || item.element >= elements_.size() || !elements_[item.element]) {
        return nullptr;
    }
    return elements_[item.element]->Widget();
}

bool DockWindow::ReloadWidget(int index) {
    WidgetInstance* w = WidgetAt(index);
    if (!w) return false;
    w->Reload();
    return true;
}

D2D1_RECT_F DockWindow::SlotScreenBox(int slot) const {
    if (slot < 0 || slot >= static_cast<int>(items_.size())) return {};
    const DockItem& item = items_[static_cast<size_t>(slot)];
    // Widgets do not scale: their box is where the layout targets it now (it moves with the bar's
    // ends while the dock magnifies).
    const D2D1_RECT_F r = layout_.BoxRect(item.targetX, item.y, item.targetX + BoxW(item), item.y + BoxCross(item));
    const float x0 = static_cast<float>(layout_.window.left), y0 = static_cast<float>(layout_.window.top);
    return D2D1::RectF(x0 + r.left, y0 + r.top, x0 + r.right, y0 + r.bottom);
}

void DockWindow::WidgetChanged(WidgetInstance&) {
    if (widgetRedrawPosted_ || !hwnd_) return;
    widgetRedrawPosted_ = PostMessageW(hwnd_, kWidgetRedrawMessage, 0, 0) != FALSE;
}

void DockWindow::UpdateWidgetHover(int index) {
    const AppKey key =
        index >= 0 && index < static_cast<int>(items_.size()) ? items_[static_cast<size_t>(index)].key : AppKey{};
    if (key == hoverWidget_) return;
    // Leave the old one (if it is still on the dock), then enter the new one.
    if (!hoverWidget_.id.empty()) {
        for (size_t i = 0; i < items_.size(); ++i) {
            if (items_[i].key == hoverWidget_ && elements_[items_[i].element]) {
                elements_[items_[i].element]->OnHover(static_cast<int>(i), false);
                break;
            }
        }
    }
    hoverWidget_ = key;
    hoverWidgetSince_ = Renderer::Now();  // a growth soon after this is a hover growth (RefreshDirtyWidgets)
    if (index >= 0 && !key.id.empty() && elements_[items_[static_cast<size_t>(index)].element]) {
        ElementOf(static_cast<size_t>(index)).OnHover(index, true);
    }
}

void DockWindow::SyncElements(bool force) {
    const std::vector<DockItemEntry>& entries = DockCfg().items;
    bool same = !force && entries.size() == elements_.size();
    for (size_t i = 0; same && i < entries.size(); ++i) {
        const DockItemEntry& a = entries[i];
        const DockItemEntry* b = elements_[i] ? &elements_[i]->Entry() : nullptr;
        same = b ? a == *b : a.Kind() == ItemKind::Unknown;
    }
    if (!same) {
        ResolvePins();
        BuildElements();  // configured from the config as they are made
    } else {
        for (auto& e : elements_) {
            if (e) e->Configure();
        }
    }
    // Every element's timers restart (the clock's tick: none once the clock is gone).
    if (hwnd_) KillTimer(hwnd_, kClockTimerId);
    for (auto& e : elements_) {
        if (e) e->Start();
    }
    // Create: the tracker's first enumeration builds the items (with their windows) right after.
    if (!same && tracking_) {
        RebuildFrom(shared_->Tracker().Current());
        // New elements keep their slots (same keys): a rebuild that is not structural lays
        // nothing out, so the new widget elements are measured here (their content, the tray's
        // cells) and drawn; a new size relays the dock out.
        for (DockItem& item : items_) {
            if (item.IsWidget()) item.dirty = true;
        }
        RefreshDirtyWidgets();
    }
}

void DockWindow::ReloadPins() {
    SyncElements(true);
    // A clock / weather item added or removed (the dock menu's Add widget, Settings > Dock
    // contents): their popups and the weather service follow.
    const DockConfig& d = DockCfg();
    if (!d.Has(ItemKind::Clock) || (!calendarClock_.id.empty() && SlotOfKey(calendarClock_) < 0)) {
        DismissCalendarPopup(false, true);
    }
    if (SlotOfKey(weatherKey_) < 0) DismissWeatherPopup(false, true);
    // The services follow the weather items (one per place; added, removed, a new place).
    weatherQuiet_ = true;
    shared_->SyncWeather();
    weatherQuiet_ = false;
    OnWeatherChanged();
}

void DockWindow::SlotWindows(int slot, WindowsCommand cmd) {
    if (slot < 0 || slot >= static_cast<int>(items_.size())) return;
    const std::vector<HWND> windows = items_[static_cast<size_t>(slot)].windows;  // copy
    switch (cmd) {
        case WindowsCommand::Minimize:
            for (HWND w : windows) Activation::Minimize(w);
            break;
        case WindowsCommand::Close:
            for (HWND w : windows) {
                if (IsWindow(w)) PostMessageW(w, WM_CLOSE, 0, 0);
            }
            break;
        case WindowsCommand::Next:
        case WindowsCommand::Previous:
            Activation::CycleItem(windows, cmd == WindowsCommand::Next);
            break;
    }
}

void DockWindow::OpenTarget(const std::wstring& target) {
    if (!Launcher::OpenTarget(target)) MessageBeep(MB_ICONWARNING);
}

LayoutContext DockWindow::Context() const {
    return LayoutContext{layout_.vertical ? DockAxis::Vertical : DockAxis::Horizontal, layout_.scale, layout_.iconPx,
                         renderer_.DWrite(), layout_.barCross};
}

void DockWindow::Rebuild(const WindowTracker::Snapshot& snap) {
    std::vector<AppKey> oldKeys;
    std::vector<HWND> oldWindows;  // window slots: key + window identify a slot
    oldKeys.reserve(items_.size());
    oldWindows.reserve(items_.size());
    for (const DockItem& it : items_) {
        oldKeys.push_back(it.key);
        oldWindows.push_back(it.window);
    }

    // 1. Every element's slots, in config order: a pin's slot (entries that resolve to the same
    //    app show once), the unpinned running apps for the `running` element (first-seen order, or
    //    the order the user dragged them into), one slot per widget. Previous slots are reused by
    //    key (visuals, animation state, windows).
    std::vector<DockItem> next;
    next.reserve(pins_.size() + snap.groups.size() + elements_.size());
    SlotBuilder builder(snap, pins_, runningOrder_, items_, next);
    builder.unstacked = DockCfg().windows == WindowsMode::Unstacked;
    builtUnstacked_ = builder.unstacked;
    for (size_t i = 0; i < elements_.size(); ++i) {
        if (!elements_[i]) continue;
        builder.SetElement(i);
        elements_[i]->AppendSlots(builder);
    }

    // 2. Windows: a running app's group goes to the app slot with its key (a pinned app or its
    //    running slot; folders, separators, the Recycle Bin and widgets never take windows).
    bool landed = false;  // a launch feedback ended at once (a hop that was already over)
    const bool thumbsOn = DockCfg().thumbnails != ThumbnailMode::Off;
    builtThumbnails_ = thumbsOn;
    bool tilesChanged = false;  // a slot became a thumbnail tile or an icon again (another width)
    for (DockItem& item : next) {
        if (item.kind == DockItem::Kind::Group) {
            // The dot follows the members: running, active (one owns the foreground), attention.
            bool running = false, attention = false, active = false;
            for (const DockItem::Member& m : item.members) {
                for (const WindowTracker::Group& group : snap.groups) {
                    if (group.key != m.key) continue;
                    running = true;
                    attention = attention || group.wantsAttention;
                    if (snap.foreground && std::find(group.windows.begin(), group.windows.end(), snap.foreground) !=
                                               group.windows.end()) {
                        active = true;
                    }
                }
            }
            if (running != item.groupRunning || attention != item.wantsAttention || active != item.active) item.dotDirty = true;
            item.groupRunning = running;
            item.wantsAttention = attention;
            item.active = active;
            item.windows.clear();
            item.thumbWindow = nullptr;
            if (layout_.captionLines > 0 && CaptionFor(item) != item.captionText) item.captionDirty = true;
            continue;
        }
        const WindowTracker::Group* g = nullptr;
        if (item.kind == DockItem::Kind::App) {
            for (const WindowTracker::Group& group : snap.groups) {
                if (group.key == item.key) {
                    g = &group;
                    break;
                }
            }
        }
        std::vector<HWND> windows;
        bool attention = false, minimized = false;
        if (g && item.window) {
            // A window slot: its own window, its own attention and minimized state.
            const auto it = std::find(g->windows.begin(), g->windows.end(), item.window);
            if (it != g->windows.end()) {
                const size_t k = static_cast<size_t>(it - g->windows.begin());
                windows.push_back(item.window);
                attention = k < g->attention.size() && g->attention[k];
                minimized = k < g->minimized.size() && g->minimized[k];
            }
        } else if (g) {
            windows = g->windows;
            attention = g->wantsAttention;
        }
        if (item.windows.empty() != windows.empty()) {
            item.dotDirty = true;           // indicator dot
            if (item.missing) item.dirty = true;  // placeholder <-> live icon
        }
        item.windows = std::move(windows);
        if (item.wantsAttention != attention) {
            item.wantsAttention = attention;
            item.dotDirty = true;
        }
        if (item.minimized != minimized) {
            item.minimized = minimized;
            item.dotDirty = true;  // a minimized window slot has no dot
        }
        if (item.window) item.windowTitle = WindowTracker::WindowTitle(item.window);
        if (layout_.captionLines > 0 && CaptionFor(item) != item.captionText) item.captionDirty = true;
        const bool active = snap.foreground &&
                            std::find(item.windows.begin(), item.windows.end(), snap.foreground) != item.windows.end();
        if (item.active != active) {
            item.active = active;
            item.dotDirty = true;
        }
        // Thumbnail tiles: a window slot shows its window; on a stacked dock an app slot with windows
        // shows its active one (the foreground window, else the one it showed, else the first).
        HWND thumb = nullptr;
        if (thumbsOn && item.kind == DockItem::Kind::App && !item.windows.empty()) {
            if (item.window) {
                thumb = item.window;
            } else if (!builder.unstacked) {
                thumb = active ? snap.foreground
                        : std::find(item.windows.begin(), item.windows.end(), item.thumbWindow) != item.windows.end()
                            ? item.thumbWindow
                            : item.windows.front();
            }
        }
        if ((thumb != nullptr) != (item.thumbWindow != nullptr)) tilesChanged = true;  // the slot's size changes
        if (thumb != item.thumbWindow) {
            item.thumbWindow = thumb;
            item.dirty = true;
        }
        // A tile whose window is minimized shows its last frame dimmed.
        bool dim = false;
        if (thumb && g) {
            const auto it = std::find(g->windows.begin(), g->windows.end(), thumb);
            const size_t k = static_cast<size_t>(it - g->windows.begin());
            dim = it != g->windows.end() && k < g->minimized.size() && g->minimized[k];
        }
        if (dim != item.thumbDim) {
            item.thumbDim = dim;
            if (thumb) item.dirty = true;
        }
        // The launched app's window appeared (any of its windows: an unstacked app has one per
        // slot): let the icon land.
        const size_t appWindows = g ? g->windows.size() : 0;
        if (item.bounce == DockItem::Bounce::Bouncing && appWindows > item.bounceWindowCount) {
            FinishBounce(item);
            if (item.bounce == DockItem::Bounce::None) landed = true;
        }
    }

    // Items that went away: remove their visuals (the container takes its icon children along).
    for (size_t i = 0; i < items_.size(); ++i) {
        if (builder.Taken(i)) continue;
        renderer_.DestroyLayer(items_[i].dot);
        renderer_.DestroyLayer(items_[i].button);  // round 11 phase 11A
        renderer_.DestroyLayer(items_[i].caption);
        renderer_.DestroyLayer(items_[i].layer);
        items_[i].glow.Reset();
        items_[i].reflection.Reset();
        items_[i].icon.Reset();
        items_[i].iconBig.Reset();
        items_[i].badge.Reset();
        items_[i].badgeBig.Reset();
    }

    bool structural = next.size() != oldKeys.size() || next.size() != oldWindows.size() || tilesChanged;
    for (size_t i = 0; !structural && i < next.size(); ++i) {
        structural = next[i].key != oldKeys[i] || next[i].window != oldWindows[i];
    }

    items_ = std::move(next);
    UpdateBadges();  // a slot came, went or changed its windows (drawn by the redraw below)
    // Active-app glow follows the foreground (committed by the redraw below).
    if (GlowOf(DockTheme()) == GlowMode::Active) UpdateGlows(motion_.labelFade);
    // Indices changed under a drag: drop it (no animation; the icons settle in the new layout).
    const bool abortDrag = structural && dragPhase_ == DragPhase::Dragging;
    if (abortDrag) AbortDrag();
    if (structural && dragPhase_ == DragPhase::Removing) dragSource_ = -1;  // keeps fading in its slot
    if (structural) {
        // The window is resized and re-centred: drop hover state. Windows sends a synthetic
        // WM_MOUSEMOVE when the window moves under the cursor, which re-magnifies. A grown bar
        // stays grown while the pointer is on it or something holds the dock (9A follow-up).
        magnifying_ = false;
        if (grown_ && !hold_ && !popupHolds_ && !CursorInZone()) grown_ = false;
        SetHover(-1);
        pressed_ = -1;
        pressedMiddle_ = -1;
        RelayoutItems();
        if (!CursorInZone()) TryCompact();
    } else {
        // Also commits a FinishBounce animation set above.
        RenderWithRecovery([this] { return DrawFrame(false); });
    }
    if (abortDrag) {
        KeepStage(motion_.rest + 0.04);
        SetHold(false);
    }
    RefreshPreview();
    RefreshFolderPopup();
    UpdateRecycleBinWatch();
    UpdateIconUsage();
    SyncThumbnails();  // closed windows release their capture, new tiles start theirs
    ScheduleBounceTimer();
    if (landed && !CursorInZone()) TryCompact();  // no bounce timer left to do it
    if (watching_) shared_->Watch().Poke();  // windows came or went: the foreground one may have changed
}

int DockWindow::IndexOfWindow(HWND hwnd) const {
    // Unstacked: the window's own slot (a pinned app's pin slot lists the window too).
    for (size_t i = 0; i < items_.size(); ++i) {
        if (hwnd && items_[i].window == hwnd) return static_cast<int>(i);
    }
    for (size_t i = 0; i < items_.size(); ++i) {
        if (std::find(items_[i].windows.begin(), items_[i].windows.end(), hwnd) != items_[i].windows.end()) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool DockWindow::LaunchItem(int index, bool newInstance) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return false;
    const DockItem& target = items_[static_cast<size_t>(index)];
    if (!newInstance && target.bounce == DockItem::Bounce::Bouncing) {
        return true;  // already launching: do not start a second copy
    }
    if (target.kind == DockItem::Kind::Separator || target.kind == DockItem::Kind::Group || target.IsWidget()) {
        return true;  // nothing to launch
    }
    // ShellExecuteEx may pump messages (and so rebuild items_): launch from a copy of the
    // launch data, then find the item again by key.
    DockItem copy;
    copy.key = target.key;
    copy.kind = target.kind;
    copy.launchPath = target.launchPath;
    copy.workingDir = target.workingDir;
    // The app's windows (a window slot holds one of them; an unstacked app has a slot per window).
    auto appWindows = [this](const AppKey& key) {
        size_t app = 0, slots = 0;
        for (const DockItem& i : items_) {
            if (i.key != key) continue;
            if (i.window) {
                ++slots;
            } else {
                app = std::max(app, i.windows.size());
            }
        }
        return std::max(app, slots);
    };
    const size_t windowsBefore = appWindows(target.key);
    if (!Launcher::Launch(copy)) {
        MessageBeep(MB_ICONWARNING);
        return false;
    }
    // Folders and the Recycle Bin open in Explorer, whose windows belong to explorer.exe: no
    // window of this item will ever appear, so there is no launch feedback.
    if (copy.kind != DockItem::Kind::App) return true;
    const size_t windowsNow = appWindows(copy.key);
    for (DockItem& item : items_) {
        if (item.key != copy.key) continue;
        if (windowsNow <= windowsBefore) StartBounce(item, windowsBefore);
        break;
    }
    return true;
}

void DockWindow::ActivateOrLaunch(int slot) {
    if (slot < 0 || slot >= static_cast<int>(items_.size())) return;
    const std::vector<HWND> windows = items_[static_cast<size_t>(slot)].windows;  // copy
    const AppKey key = items_[static_cast<size_t>(slot)].key;
    const bool app = items_[static_cast<size_t>(slot)].kind == DockItem::Kind::App;
    if (windows.empty()) {
        LaunchItem(slot, false);
    } else {
        Activation::ActivateItem(windows);
    }
    // notifications.clearOnActivate: its notifications are read (the badge goes; nothing is
    // removed from Windows' notification centre).
    if (app && shared_ && config_->notifications.clearOnActivate) shared_->Notifications().MarkRead(key);
}

// --- Launch bounce ---------------------------------------------------------------------------
// launchStyle "bounce": one bounce = rise for bounceHalf (ease out), fall for bounceHalf (ease in),
// repeated by DWM until a new window of the app appears or kBounceTimeoutMs elapses; then the
// current bounce is finished (no jump) and OffsetY returns to its static 0. "hop": the same single
// bounce without the repeat (the item still counts as launching until a window appears or the
// timeout). "pulse": the icon's opacity goes 1 -> kPulseOpacity -> 1 every two bounceHalf,
// repeated by DWM, and fades back to opaque at the end. Only one-shot timers at the transitions;
// nothing runs in this process while the icon is in the air. The bounce drives the item
// container's OffsetY, which is added after its placement (magnification) transform, and the
// indicator dot (a separate layer) stays put.

void DockWindow::StartBounce(DockItem& item, size_t windowsBeforeLaunch) {
    const LaunchStyle style = motion_.launch;
    if (style == LaunchStyle::None || motion_.bounceHalf <= 0.0) return;
    if (!item.layer.visual) return;
    if (item.bounce != DockItem::Bounce::None) {
        // Launched again (new instance) while the previous feedback runs: start over cleanly.
        if (item.bounceStyle == LaunchStyle::Pulse) {
            renderer_.StopOpacityAnimation(item.layer, 1.0f);
            item.fade = Tween{1.0f, 1.0f, Renderer::Now(), 0.0, true};
        } else {
            renderer_.StopOffsetAnimation(item.layer, 0.0f, layout_.vertical);
        }
        item.bounce = DockItem::Bounce::None;
    }
    winrt::com_ptr<IDCompositionAnimation> anim = renderer_.CreateAnimation();
    if (!anim) return;
    const float t = static_cast<float>(motion_.bounceHalf);
    HRESULT hr = S_OK;
    if (style == LaunchStyle::Pulse) {
        // Smoothstep down, smoothstep up: o = 1 - a (3u^2 - 2u^3), then kPulseOpacity + a (3u^2 - 2u^3).
        const float a = 1.0f - kPulseOpacity;
        anim->AddCubic(0.0, 1.0f, 0.0f, -3.0f * a / (t * t), 2.0f * a / (t * t * t));
        anim->AddCubic(t, kPulseOpacity, 0.0f, 3.0f * a / (t * t), -2.0f * a / (t * t * t));
        anim->AddRepeat(2.0 * t, 2.0 * t);
        hr = renderer_.AnimateOpacity(item.layer, anim.get());
    } else {
        // Away from the screen edge: up on a bottom dock (OffsetY < 0), down on a top dock,
        // sideways (OffsetX) on vertical docks.
        const float h = std::round(layout_.iconPx * kBounceHeight) * DockGeometry::OutwardSign(layout_.edge);
        // Rise: y = -h * (2u - u^2), u = s / t.   Fall: y = -h * (1 - u^2).
        anim->AddCubic(0.0, 0.0f, -2.0f * h / t, h / (t * t), 0.0f);
        anim->AddCubic(t, -h, 0.0f, h / (t * t), 0.0f);
        if (style == LaunchStyle::Hop) {
            anim->End(2.0 * t, 0.0f);
        } else {
            anim->AddRepeat(2.0 * t, 2.0 * t);
        }
        hr = renderer_.AnimateOffset(item.layer, anim.get(), layout_.vertical);
    }
    if (FAILED(hr)) return;
    const ULONGLONG now = GetTickCount64();
    item.bounce = DockItem::Bounce::Bouncing;
    item.bounceStyle = style;
    item.bounceHalf = t;
    item.bounceStart = now;
    item.bounceDeadline = now + kBounceTimeoutMs;
    item.bounceWindowCount = windowsBeforeLaunch;
    RenderWithRecovery([] { return S_OK; });  // commit
    ScheduleBounceTimer();
}

void DockWindow::FinishBounce(DockItem& item) {
    if (item.bounce != DockItem::Bounce::Bouncing) return;
    const ULONGLONG now = GetTickCount64();
    const float t = item.bounceHalf;
    const float elapsed = static_cast<float>(now - item.bounceStart) / 1000.0f;

    if (item.bounceStyle == LaunchStyle::Pulse) {
        // Fade back to opaque from wherever the pulse is now.
        const float a = 1.0f - kPulseOpacity;
        const float phase = std::fmod(elapsed, 2.0f * t);
        const float u = phase < t ? phase / t : (phase - t) / t;
        const float step = a * u * u * (3.0f - 2.0f * u);
        const float current = phase < t ? 1.0f - step : kPulseOpacity + step;
        item.fade = Tween{current, current, Renderer::Now(), 0.0, true};  // the fade starts here
        const double duration = motion_.rest;
        SetItemOpacity(item, 1.0f, duration);
        if (duration < 0.02 || !item.fade.Running(Renderer::Now())) {
            renderer_.StopOpacityAnimation(item.layer, 1.0f);
            item.fade = Tween{1.0f, 1.0f, Renderer::Now(), 0.0, true};
            item.bounce = DockItem::Bounce::None;
            return;
        }
        item.bounce = DockItem::Bounce::Finishing;
        item.bounceDeadline = now + static_cast<ULONGLONG>(duration * 1000.0) + 30;
        return;
    }

    const float h = std::round(layout_.iconPx * kBounceHeight) * DockGeometry::OutwardSign(layout_.edge);
    const float c = h / (t * t);
    // A hop is over after one bounce; a bounce repeats.
    const float phase = item.bounceStyle == LaunchStyle::Hop ? elapsed : std::fmod(elapsed, 2.0f * t);

    // The rest of the current bounce: the same curves shifted by the elapsed part d
    // (p(s + d) expanded into polynomial coefficients), ending at 0.
    winrt::com_ptr<IDCompositionAnimation> anim = phase < 2.0f * t ? renderer_.CreateAnimation() : nullptr;
    float remaining = 0.0f;
    if (anim) {
        if (phase < t) {
            const float d = phase;
            const float b = -2.0f * h / t;
            anim->AddCubic(0.0, b * d + c * d * d, b + 2.0f * c * d, c, 0.0f);
            anim->AddCubic(t - d, -h, 0.0f, c, 0.0f);
            remaining = 2.0f * t - d;
        } else {
            const float d = phase - t;
            anim->AddCubic(0.0, -h + c * d * d, 2.0f * c * d, c, 0.0f);
            remaining = t - d;
        }
        anim->End(remaining, 0.0f);
    }
    if (!anim || remaining < 0.02f || FAILED(renderer_.AnimateOffset(item.layer, anim.get(), layout_.vertical))) {
        renderer_.StopOffsetAnimation(item.layer, 0.0f, layout_.vertical);
        item.bounce = DockItem::Bounce::None;
        return;
    }
    item.bounce = DockItem::Bounce::Finishing;
    item.bounceDeadline = now + static_cast<ULONGLONG>(remaining * 1000.0f) + 30;
}

void DockWindow::OnBounceTimer() {
    const ULONGLONG now = GetTickCount64();
    for (DockItem& item : items_) {
        if (item.bounce == DockItem::Bounce::Bouncing && now >= item.bounceDeadline) {
            FinishBounce(item);  // timed out: the app never showed a new window
        } else if (item.bounce == DockItem::Bounce::Finishing && now >= item.bounceDeadline) {
            if (item.bounceStyle == LaunchStyle::Pulse) {
                SetItemOpacity(item, 1.0f, 0.0);  // normally already there
            } else {
                renderer_.StopOffsetAnimation(item.layer, 0.0f, layout_.vertical);
            }
            item.bounce = DockItem::Bounce::None;
        }
    }
    RenderWithRecovery([] { return S_OK; });  // commit
    ScheduleBounceTimer();
    if (!CursorInZone()) TryCompact();  // a bounce that outlived the hover
}

void DockWindow::ScheduleBounceTimer() {
    if (!hwnd_) return;
    ULONGLONG next = ULLONG_MAX;
    for (const DockItem& item : items_) {
        if (item.bounce != DockItem::Bounce::None) next = std::min(next, item.bounceDeadline);
    }
    if (next == ULLONG_MAX) {
        KillTimer(hwnd_, kBounceTimerId);
        return;
    }
    const ULONGLONG now = GetTickCount64();
    const UINT delay = next > now ? static_cast<UINT>(std::min<ULONGLONG>(next - now, 60'000)) : 0;
    SetTimer(hwnd_, kBounceTimerId, std::max<UINT>(delay, USER_TIMER_MINIMUM), nullptr);
}

void DockWindow::UpdateIconUsage() {
    std::vector<AppKey> keys;
    keys.reserve(items_.size());
    for (const DockItem& item : items_) {
        keys.push_back(item.key);
        for (const DockItem::Member& m : item.members) keys.push_back(m.key);  // a group's composed icon
    }
    icons_.SetInUse(keys);
    if (icons_.HasUnused() && !evictPending_ && hwnd_) {
        SetTimer(hwnd_, kIconEvictTimerId, static_cast<UINT>(IconCache::kEvictAfterMs), nullptr);
        evictPending_ = true;
    }
}

// --- Drag to reorder ---------------------------------------------------------------------------
// A left press on an item that moves past the system drag threshold starts a drag: the dock
// takes the mouse capture and holds itself shown (no auto-hide, no label), magnification stops,
// the dragged icon follows the cursor (slightly enlarged and translucent) and the other icons
// make room at the insertion slot (80 ms retargets through the usual placement tweens; nothing is
// redrawn). The insertion slot is the rest slot under the cursor (over a widget or a spacer: the
// nearest pin / running app on the cursor's side; widgets are never drop places for icons);
// pinned icons stay among the pins, running ones move anywhere (a pinned slot pins them).
// The pins keep their places in the item list (pin k always shows at the place of the k-th pin
// item), so the drag layout shows the pins and the running apps refilled into their places.
// A widget (clock, weather) is dragged the same way but moves its own config item: it goes in
// front of the first other element whose rest centre is right of the cursor, the running apps
// counting as one block (UpdateElementDragTarget). The drag layout gives every order the spacers
// it would have at rest. A locked dock (docks[].locked) starts no drag.
// Release: the icon lands in its slot (120 ms) and the new pinned order goes to the config (or,
// among the running apps, to runningOrder_ for this session). The
// layout keeps its geometry, so the landing animation keeps running through the item rebuild.
// Released more than 1.5 bar heights above the bar, a pinned app without windows is unpinned (the
// icon fades out first). Escape (polled: the dock never has keyboard focus) or losing the capture
// cancels: everything slides back.

size_t DockWindow::ConfigIndexOfItem(size_t itemIndex) const {
    const size_t end = DockCfg().PinCount();
    if (itemIndex >= items_.size() || !items_[itemIndex].pinned) return end;
    const AppKey& key = items_[itemIndex].key;
    for (size_t i = 0; i < pins_.size() && i < end; ++i) {
        if (pins_[i].key == key) return i;
    }
    return end;
}

void DockWindow::ResetDragOrder() {
    dragOrder_.clear();
    for (size_t i = 0; i < items_.size(); ++i) dragOrder_.push_back(i);
    dragGroup_ = -1;
}

int DockWindow::DropGroupAt(float stageX) const {
    const DockConfig& d = DockCfg();
    float lo[3] = {0.0f, 0.0f, 0.0f}, hi[3] = {0.0f, 0.0f, 0.0f};
    bool any[3] = {false, false, false};
    for (size_t i = 0; i < items_.size(); ++i) {
        const DockItem& item = items_[i];
        if (static_cast<int>(i) == dragSource_ || (item.IsWidget() && item.slotW <= 0.0f)) continue;
        const int g = std::clamp(d.GroupOf(item.element), 0, 2);
        const float a = item.slotX, b = item.slotX + item.slotW;
        lo[g] = any[g] ? std::min(lo[g], a) : a;
        hi[g] = any[g] ? std::max(hi[g], b) : b;
        any[g] = true;
    }
    const Layout& l = layout_;
    if (l.anchored) {
        // An empty group: a point at its place in the bar.
        const float at[3] = {l.bar.left + l.padding, (l.bar.left + l.bar.right) / 2.0f, l.bar.right - l.padding};
        for (int g = 0; g < 3; ++g) {
            if (!any[g]) lo[g] = hi[g] = at[g];
        }
    }
    int best = -1;
    float bestD = 0.0f;
    for (int g = 0; g < 3; ++g) {
        if (!any[g] && !l.anchored) continue;
        const float dist = stageX < lo[g] ? lo[g] - stageX : stageX > hi[g] ? stageX - hi[g] : 0.0f;
        // Nearer wins; on a tie a group with items beats an empty one.
        if (best < 0 || dist < bestD - 0.01f || (dist <= bestD + 0.01f && any[g] && !any[best])) {
            best = g;
            bestD = dist;
        }
    }
    return best >= 0 ? best : DragOwnGroup();
}

int DockWindow::DragOwnGroup() const {
    if (dragSource_ < 0 || dragSource_ >= static_cast<int>(items_.size())) return -1;
    return DockCfg().GroupOf(items_[static_cast<size_t>(dragSource_)].element);
}

std::optional<ItemAnchor> DockWindow::DragAnchor(bool newPin) const {
    if (dragGroup_ < 0 || (!newPin && dragGroup_ == DragOwnGroup())) return std::nullopt;
    return DockCfg().AnchorFor(dragGroup_);
}

void DockWindow::ApplyDragGroup(float stageX, bool canChangeGroup) {
    dragGroup_ = -1;
    const DockConfig& d = DockCfg();
    const int src = dragSource_;
    if (!d.Anchored() || src < 0 || dragOrder_.size() != items_.size()) return;
    dragGroup_ = canChangeGroup ? DropGroupAt(stageX) : DragOwnGroup();
    auto groupOf = [&](size_t i) { return static_cast<int>(i) == src ? dragGroup_ : d.GroupOf(items_[i].element); };
    std::stable_sort(dragOrder_.begin(), dragOrder_.end(), [&](size_t a, size_t b) { return groupOf(a) < groupOf(b); });
    for (size_t k = 0; k < dragOrder_.size(); ++k) {
        if (static_cast<int>(dragOrder_[k]) == src) dragInsert_ = static_cast<int>(k);
    }
}

void DockWindow::UpdateDragTarget(float stageX) {
    stageX = RestMain(stageX);  // the targeting works in rest coordinates (a grown bar: 9A follow-up)
    const size_t n = items_.size();
    const int s = dragSource_;
    dragInsert_ = -1;
    dragGroup_ = -1;
    if (s < 0 || static_cast<size_t>(s) >= n) return;
    const size_t src = static_cast<size_t>(s);
    if (dragWidget_) {
        UpdateElementDragTarget(stageX);
        return;
    }

    // The droppable slot under the cursor (rest layout). Beyond the ends: the first / last one;
    // over a widget (or the spacer before a slot) the nearest one on the cursor's side of it.
    auto search = [&](int from, int step) {
        for (int i = from; i >= 0 && i < static_cast<int>(n); i += step) {
            if (Droppable(static_cast<size_t>(i))) return i;
        }
        return -1;
    };
    int raw = -1;
    if (stageX < layout_.itemsLeft) {
        raw = search(0, 1);
    } else {
        int cell = -1;  // the slot whose spacer + box holds x
        for (size_t i = 0; i < n && cell < 0; ++i) {
            if (stageX < items_[i].slotX + items_[i].slotW) cell = static_cast<int>(i);
        }
        if (cell < 0) {
            raw = search(static_cast<int>(n) - 1, -1);
        } else {
            const DockItem& c = items_[static_cast<size_t>(cell)];
            const bool inBox = stageX >= c.slotX;
            if (inBox && Droppable(static_cast<size_t>(cell))) {
                raw = cell;
            } else {
                const float mid = inBox ? c.slotX + c.slotW / 2.0f : c.slotX - c.lead / 2.0f;
                if (stageX < mid) {
                    raw = search(cell - 1, -1);
                    if (raw < 0) raw = search(cell, 1);
                } else {
                    raw = search(cell, 1);
                    if (raw < 0) raw = search(cell - 1, -1);
                }
            }
        }
    }
    if (raw < 0) return;

    // Pin places and running places, and the rank of the cursor's slot among its kind (the
    // dragged item counted where it is now).
    size_t pinPlaces = 0, rank = 0;
    const bool rawPinned = items_[static_cast<size_t>(raw)].pinned;
    for (size_t i = 0; i < n; ++i) {
        if (items_[i].pinned) ++pinPlaces;
        if (static_cast<int>(i) < raw && !items_[i].IsWidget() && items_[i].pinned == rawPinned) ++rank;
    }
    const DockItem& item = items_[src];
    bool toPins = rawPinned;
    if (item.pinned && !toPins) {
        rank = pinPlaces - 1;  // a pin stays among the pins: beyond them, it becomes the last one
        toPins = true;
    } else if (!item.pinned && toPins && item.key.IsClassFallback()) {
        rank = 0;  // no pinnable identity: it stays among the running apps (the first of them)
        toPins = false;
    }
    // A running (unpinned) item dropped on a pin's place is pinned there; among the running apps
    // it just moves (for this session).

    dragToPins_ = toPins;
    dragPins_.clear();
    dragRunning_.clear();
    for (size_t i = 0; i < n; ++i) {
        if (i == src || items_[i].IsWidget()) continue;
        (items_[i].pinned ? dragPins_ : dragRunning_).push_back(i);
    }
    std::vector<size_t>& list = toPins ? dragPins_ : dragRunning_;
    list.insert(list.begin() + static_cast<ptrdiff_t>(std::min(rank, list.size())), src);

    // The order the drop would show: pins refilled into the pin places (a newly pinned one right
    // after the last pin place), the running apps into theirs, the widgets where they are.
    dragOrder_.clear();
    size_t pi = 0, ri = 0, pinSeen = 0;
    auto flushPins = [&] {
        while (pi < dragPins_.size()) dragOrder_.push_back(dragPins_[pi++]);
    };
    for (size_t i = 0; i < n; ++i) {
        if (items_[i].pinned) {
            if (pi < dragPins_.size()) dragOrder_.push_back(dragPins_[pi++]);
            if (++pinSeen == pinPlaces) flushPins();
        } else if (!items_[i].IsWidget()) {
            if (ri < dragRunning_.size()) dragOrder_.push_back(dragRunning_[ri++]);
        } else {
            dragOrder_.push_back(i);
        }
    }
    flushPins();
    while (ri < dragRunning_.size()) dragOrder_.push_back(dragRunning_[ri++]);
    for (size_t k = 0; k < dragOrder_.size(); ++k) {
        if (dragOrder_[k] == src) dragInsert_ = static_cast<int>(k);
    }
    // Anchors (round 7 phase 7B): a pin (or a running app becoming one) takes the group it is
    // dropped into; a running app that stays among the running apps keeps the running item's.
    ApplyDragGroup(stageX, toPins);
}

void DockWindow::UpdateElementDragTarget(float stageX) {
    // The other slots as units in their order: the running apps (the slots of one element) are
    // one block, every other slot is one unit. The widget goes in front of the first unit whose
    // rest centre is right of the cursor, so over the running apps it lands before or after the
    // block, whichever side is nearer.
    const size_t n = items_.size();
    const size_t src = static_cast<size_t>(dragSource_);
    struct Unit {
        size_t first, last;
    };
    std::vector<Unit> units;
    for (size_t i = 0; i < n; ++i) {
        if (i == src) continue;
        if (!units.empty() && items_[units.back().last].element == items_[i].element) {
            units.back().last = i;
        } else {
            units.push_back(Unit{i, i});
        }
    }
    size_t at = 0;
    for (const Unit& u : units) {
        const float centre = (items_[u.first].slotX + items_[u.last].slotX + items_[u.last].slotW) / 2.0f;
        if (centre < stageX) ++at;
    }
    dragOrder_.clear();
    for (size_t k = 0; k <= units.size(); ++k) {
        if (k == at) {
            dragInsert_ = static_cast<int>(dragOrder_.size());
            dragOrder_.push_back(src);
        }
        if (k == units.size()) break;
        for (size_t i = units[k].first; i <= units[k].last; ++i) dragOrder_.push_back(i);
    }
    // The config item it goes before (items with no slot, such as a kind this version does not
    // know or a `running` item without running apps, keep their places relative to the rest).
    dragMoveTo_ = at < units.size() ? items_[units[at].first].element : SIZE_MAX;
    ApplyDragGroup(stageX, true);  // round 7 phase 7B
}

int DockWindow::DropGapIndex(float stageX) const {
    // A pinnable file is inserted as a pin: before a pin, after the last pin, or (no pins) before
    // the first running app. Of those places, the one whose (rest) boundary is nearest to the
    // cursor; between two icons that is the slot-midpoint rule (ties: the later place).
    const int n = static_cast<int>(items_.size());
    auto boundary = [&](int p) {
        if (p < n && !items_[static_cast<size_t>(p)].IsWidget()) return items_[static_cast<size_t>(p)].slotX;
        if (p > 0) return items_[static_cast<size_t>(p - 1)].slotX + items_[static_cast<size_t>(p - 1)].slotW;
        return layout_.itemsLeft;
    };
    int best = -1;
    float bestD = 0.0f;
    auto consider = [&](int p) {
        const float d = std::fabs(stageX - boundary(p));
        if (best < 0 || d <= bestD) {
            best = p;
            bestD = d;
        }
    };
    int lastPin = -1, firstRunning = -1;
    for (int i = 0; i < n; ++i) {
        const DockItem& item = items_[static_cast<size_t>(i)];
        if (item.pinned) {
            consider(i);
            lastPin = i;
        } else if (!item.IsWidget() && firstRunning < 0) {
            firstRunning = i;
        }
    }
    if (lastPin >= 0) {
        consider(lastPin + 1);
    } else if (firstRunning >= 0) {
        consider(firstRunning);
    }
    return best;
}

bool DockWindow::DragInRemoveZone(POINT pt) const {
    const float barH = layout_.bar.bottom - layout_.bar.top;
    return layout_.Canon(pt).y < layout_.bar.top - kRemoveZoneBars * barH;
}

bool DockWindow::DragRemovable() const {
    if (dragSource_ < 0 || dragSource_ >= static_cast<int>(items_.size())) return false;
    const DockItem& item = items_[static_cast<size_t>(dragSource_)];
    return item.pinned && item.windows.empty();
}

void DockWindow::SetItemOpacity(DockItem& item, float to, double duration) {
    if (!item.layer.visual) return;
    winrt::com_ptr<IDCompositionVisual3> v3;
    if (FAILED(item.layer.visual->QueryInterface(IID_PPV_ARGS(v3.put())))) return;
    renderer_.Drive(
        item.fade, to, duration, Renderer::Now(), [&](auto v) { return v3->SetOpacity(v); }, 0.001f,
        renderer_.FadeEasing());
}

void DockWindow::KeepStage(double seconds) {
    if (!hwnd_) return;
    SetTimer(hwnd_, kRestTimerId, TimerMs(seconds), nullptr);  // its handler compacts the window
    restPending_ = true;
}

void DockWindow::BeginDrag(POINT pt) {
    const int s = pressed_;
    pressed_ = -1;
    if (s < 0 || s >= static_cast<int>(items_.size())) return;
    DockItem& item = items_[static_cast<size_t>(s)];
    if (!item.layer.visual || !Draggable(static_cast<size_t>(s))) return;
    // A launch pulse animates the opacity the drag uses: end it first (it fades back to opaque,
    // which the drag's dimming below replaces).
    if (item.bounce == DockItem::Bounce::Bouncing && item.bounceStyle == LaunchStyle::Pulse) FinishBounce(item);

    dragPhase_ = DragPhase::Dragging;
    dragSource_ = s;
    dragKey_ = item.key;
    dragRemove_ = false;
    dragWidget_ = item.IsWidget();
    dragMoveTo_ = SIZE_MAX;
    // Keep the grab point: the icon box's left edge stays this far left of the cursor.
    const float boxW = BoxW(item);
    const float centre = item.targetX + boxW / 2.0f;
    dragGrabDx_ = boxW / 2.0f + std::clamp(layout_.Canon(pressPt_).x - centre, -boxW / 2.0f, boxW / 2.0f);
    GetAsyncKeyState(VK_ESCAPE);  // clears the "pressed since the last call" bit
    SetCapture(hwnd_);

    // No magnification, label or auto-hide while dragging; the window stays at stage size.
    SetHold(true);
    magnifying_ = false;
    if (magnifyPending_) {
        KillTimer(hwnd_, kMagnifyTimerId);
        magnifyPending_ = false;
    }
    if (restPending_) {
        KillTimer(hwnd_, kRestTimerId);
        restPending_ = false;
    }
    SetHover(-1);
    if (!expanded_) {
        expanded_ = true;
        UpdateWindowRect();
    }

    // In front of the other icons while it passes over them (a widget: moved into the icons'
    // container for the drag).
    if (dragWidget_) {
        SetWidgetFront(item, true);
    } else if (itemsGroup_.visual) {
        itemsGroup_.visual->RemoveVisual(item.layer.visual.get());
        itemsGroup_.visual->AddVisual(item.layer.visual.get(), FALSE, nullptr);
    }
    SetItemOpacity(item, kDragOpacity, 0.0);
    SetTimer(hwnd_, kDragTimerId, kDragPollMs, nullptr);
    dragPending_ = true;
    UpdateDrag(pt);
}

void DockWindow::SetWidgetFront(DockItem& item, bool front) {
    Layer& group = front ? itemsGroup_ : widgetsGroup_;
    if (!item.IsWidget() || !item.layer.visual || !group.visual || item.layer.parent == group.visual) return;
    if (item.layer.parent) item.layer.parent->RemoveVisual(item.layer.visual.get());
    item.layer.parent = SUCCEEDED(group.visual->AddVisual(item.layer.visual.get(), FALSE, nullptr)) ? group.visual : nullptr;
}

void DockWindow::UpdateDrag(POINT pt) {
    if (dragPhase_ != DragPhase::Dragging) return;
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8001) {
        EndDrag(false);
        return;
    }
    cursorX_ = layout_.Canon(pt).x;
    // Over the middle of a pin: held there it makes a group (a group takes it at once). Meanwhile
    // the others stay where they are (no gap), so the target stays under the pointer.
    // Round 12 phase 12D: customize mode has no hold-to-group (plain dragging reorders); over one of
    // its anchor targets the others stay put too (a drop there sets the item's anchor).
    const int candidate = editing_ ? -1 : GroupCandidateAt(pt);
    if (editing_) SetAnchorHot(AnchorAt(pt));
    if (candidate != groupCandidate_) {
        ResetGroupTarget();
        groupCandidate_ = candidate;
        if (candidate >= 0 && items_[static_cast<size_t>(candidate)].kind == DockItem::Kind::Group) {
            groupTarget_ = candidate;
            SetBigIcons(true);
        } else if (candidate >= 0) {
            groupHoldPending_ = SetTimer(hwnd_, kGroupHoldTimerId, kGroupHoldMs, nullptr) != 0;
        }
    }
    if (groupCandidate_ >= 0 || anchorHot_ >= 0) {
        dragInsert_ = dragSource_;
        ResetDragOrder();
    } else {
        UpdateDragTarget(cursorX_);
    }
    // Near a viewport end (not up in the remove zone): the run scrolls that way.
    if (DragInRemoveZone(pt)) {
        StopAutoScroll();
    } else {
        UpdateAutoScroll(cursorX_);
    }
    const bool remove = DragRemovable() && DragInRemoveZone(pt);
    if (remove != dragRemove_) {
        dragRemove_ = remove;
        SetItemOpacity(items_[static_cast<size_t>(dragSource_)], remove ? kRemoveOpacity : kDragOpacity,
                       motion_.magnify);
    }
    ScheduleMagnify();  // throttled ComputeTargets + ApplyTargets (80 ms)
}

int DockWindow::GroupCandidateAt(POINT pt) const {
    const int n = static_cast<int>(items_.size());
    if (dragPhase_ != DragPhase::Dragging || dragWidget_ || dragSource_ < 0 || dragSource_ >= n) return -1;
    const DockItem& src = items_[static_cast<size_t>(dragSource_)];
    if ((src.window && !src.pinned) || (src.kind != DockItem::Kind::App && src.kind != DockItem::Kind::Folder)) return -1;
    if (!src.pinned && src.key.IsClassFallback()) return -1;  // nothing a group could keep
    const D2D1_POINT_2F c = layout_.Canon(pt);
    if (c.y < layout_.itemTop - layout_.iconPx * 0.5f) return -1;  // well above the icons (the unpin zone)
    const float x = RestMain(c.x);
    const int slot = SlotAt(x);
    if (slot < 0 || slot == dragSource_) return -1;
    const DockItem& t = items_[static_cast<size_t>(slot)];
    const bool pin = t.pinned && (t.kind == DockItem::Kind::App || t.kind == DockItem::Kind::Folder);
    if (!pin && t.kind != DockItem::Kind::Group) return -1;
    const float quarter = t.slotW * 0.25f;
    if (x < t.slotX + quarter || x >= t.slotX + t.slotW - quarter) return -1;
    return slot;
}

void DockWindow::OnGroupHoldTimer() {
    KillTimer(hwnd_, kGroupHoldTimerId);
    groupHoldPending_ = false;
    if (dragPhase_ != DragPhase::Dragging || groupCandidate_ < 0) return;
    groupTarget_ = groupCandidate_;
    RenderWithRecovery([this] {
        SetBigIcons(true);  // the target grows: crisp from the magnified-size surface
        ComputeTargets();
        ApplyTargets(motion_.magnify, true);
        return S_OK;
    });
}

void DockWindow::ResetGroupTarget() {
    if (groupHoldPending_ && hwnd_) KillTimer(hwnd_, kGroupHoldTimerId);
    groupHoldPending_ = false;
    groupCandidate_ = -1;
    groupTarget_ = -1;
}

void DockWindow::EndDrag(bool commit) {
    if (dragPhase_ != DragPhase::Dragging) return;
    StopAutoScroll();
    if (dragPending_) {
        KillTimer(hwnd_, kDragTimerId);
        dragPending_ = false;
    }
    if (magnifyPending_) {
        KillTimer(hwnd_, kMagnifyTimerId);
        magnifyPending_ = false;
    }
    if (commit && (GetAsyncKeyState(VK_ESCAPE) & 0x8001)) commit = false;
    const int s = dragSource_;
    const bool valid = s >= 0 && s < static_cast<int>(items_.size()) && items_[static_cast<size_t>(s)].key == dragKey_;
    // Round 12 phase 12D: dropped on one of customize mode's anchor targets: the item's anchor
    // changes (start / centre / end), its order does not.
    const int anchorDrop = editing_ && valid && commit ? anchorHot_ : -1;
    const size_t anchorItem = anchorDrop >= 0 ? items_[static_cast<size_t>(s)].element : SIZE_MAX;
    SetAnchorHot(-1);
    const bool remove = valid && commit && anchorDrop < 0 && dragRemove_ && DragRemovable();
    // Released over a pin it was held on (or a group): the two make a group.
    AppKey groupWith;
    if (valid && commit && !remove && groupTarget_ >= 0 && groupTarget_ < static_cast<int>(items_.size())) {
        groupWith = items_[static_cast<size_t>(groupTarget_)].key;
    }
    ResetGroupTarget();

    // Leave the drag state before releasing the capture (WM_CAPTURECHANGED arrives synchronously).
    dragPhase_ = remove ? DragPhase::Removing : DragPhase::None;
    if (GetCapture() == hwnd_) ReleaseCapture();

    if (remove) {
        // Fade out where it was dropped, then unpin (OnRemoveTimer).
        removeKey_ = dragKey_;
        RenderWithRecovery([&] {
            SetItemOpacity(items_[static_cast<size_t>(s)], 0.0f, motion_.removeFade);
            return S_OK;
        });
        SetTimer(hwnd_, kRemoveTimerId, TimerMs(motion_.removeFade + 0.02), nullptr);
        removePending_ = true;
        return;
    }

    // The new pinned order, or the new order of the running apps, if either changed; a widget:
    // the config item it moves to.
    std::vector<AppKey> order;
    std::vector<AppKey> running;
    size_t moveFrom = SIZE_MAX;
    bool moved = false;
    for (size_t k = 0; k < dragOrder_.size(); ++k) moved = moved || dragOrder_[k] != k;
    // Round 7 phase 7B: dropped into another group (the order may stay): that group's anchor.
    if (valid && dragGroup_ >= 0 && dragGroup_ != DragOwnGroup()) moved = true;
    std::optional<ItemAnchor> anchor;
    if (valid && commit && anchorDrop < 0 && dragInsert_ >= 0 && moved && dragOrder_.size() == items_.size()) {
        if (dragWidget_) {
            moveFrom = items_[static_cast<size_t>(s)].element;
            anchor = DragAnchor(false);
        } else {
            for (const size_t i : dragToPins_ ? dragPins_ : dragRunning_) {
                (dragToPins_ ? order : running).push_back(items_[i].key);
            }
            if (dragToPins_) anchor = DragAnchor(!items_[static_cast<size_t>(s)].pinned);
        }
    }
    const size_t moveTo = dragMoveTo_;
    if (order.empty() && running.empty() && moveFrom == SIZE_MAX) {  // cancelled / unchanged: back to its own slot
        dragInsert_ = s;
        ResetDragOrder();
    }

    // Land: the dragged icon slides into its slot (120 ms) and becomes opaque. With a new order
    // these are exactly its targets in the rebuilt list, so the rebuild leaves the animation alone.
    dragLanding_ = valid;
    RenderWithRecovery([&] {
        ComputeTargets();
        ApplyTargets(motion_.rest);
        if (valid) SetItemOpacity(items_[static_cast<size_t>(s)], 1.0f, motion_.rest);
        return S_OK;
    });
    dragLanding_ = false;
    dragSource_ = -1;
    dragInsert_ = -1;
    dragGroup_ = -1;
    dragRemove_ = false;
    if (dragWidget_) {
        for (DockItem& item : items_) {
            if (item.key == dragKey_) SetWidgetFront(item, false);  // lands behind the icons again
        }
    }
    dragWidget_ = false;

    KeepStage(motion_.rest + 0.04);  // the window stays at stage size until the icon has landed
    if (!order.empty() && pinRequests_.reorder) pinRequests_.reorder(order, dragKey_, anchor);
    if (!groupWith.id.empty() && pinRequests_.group) pinRequests_.group(dragKey_, groupWith);
    // A widget's new place usually changes the dock's width (the spacers move with it): the
    // rebuilt layout then snaps instead of finishing the landing.
    if (moveFrom != SIZE_MAX && pinRequests_.moveItem) pinRequests_.moveItem(moveFrom, moveTo, anchor);
    if (anchorItem != SIZE_MAX && editRequests_.anchor) {
        editRequests_.anchor(anchorItem, anchorDrop == 0 ? ItemAnchor::Start : anchorDrop == 1 ? ItemAnchor::Center : ItemAnchor::End);
    }
    if (!running.empty()) {
        runningOrder_ = std::move(running);
        RebuildFrom(shared_->Tracker().Current());
    }
    SetHold(false);  // starts the hide delay if the mouse is no longer on the dock
    POINT cursor{};
    if (hwnd_ && GetCursorPos(&cursor) && PtInRect(&winRect_, cursor)) ArmMouseTracking();
}

void DockWindow::AbortDrag() {
    if (dragPhase_ != DragPhase::Dragging) return;
    StopAutoScroll();
    ResetGroupTarget();
    if (dragPending_) {
        KillTimer(hwnd_, kDragTimerId);
        dragPending_ = false;
    }
    dragPhase_ = DragPhase::None;
    dragSource_ = -1;
    dragInsert_ = -1;
    dragGroup_ = -1;
    dragRemove_ = false;
    dragWidget_ = false;
    if (GetCapture() == hwnd_) ReleaseCapture();
    for (DockItem& item : items_) {
        if (item.key != dragKey_) continue;
        SetItemOpacity(item, 1.0f, 0.0);
        SetWidgetFront(item, false);
    }
}

void DockWindow::OnRemoveTimer() {
    KillTimer(hwnd_, kRemoveTimerId);
    removePending_ = false;
    if (dragPhase_ != DragPhase::Removing) return;
    dragPhase_ = DragPhase::None;
    dragSource_ = -1;
    dragInsert_ = -1;
    dragRemove_ = false;
    const AppKey key = removeKey_;
    if (pinRequests_.unpin) pinRequests_.unpin(key);  // rebuilds the items (the icon goes away)
    // Still on the dock (it started meanwhile, or nothing was unpinned): show it again.
    for (DockItem& item : items_) {
        if (item.key == key) SetItemOpacity(item, 1.0f, motion_.rest);
    }
    RenderWithRecovery([this] {
        ComputeTargets();
        ApplyTargets(motion_.rest);
        return S_OK;
    });
    KeepStage(motion_.rest + 0.04);
    SetHold(false);
}

// --- External file drops -------------------------------------------------------------------------
// The dock HWND is an OLE drop target for file drags (CF_HDROP). Where the pointer is decides
// what a drop does (dropMode_, QueryState 26):
//  - over the central 70 % of an app's slot: open the files with that app (the icon grows to
//    kDropScale, no marker);
//  - over the central 70 % of the Recycle Bin's slot: recycle them (the bin grows the same way);
//  - anywhere else (between icons, over folders / separators): pin the .lnk / .exe / folder files
//    among them, with a gap at the insertion slot (same slot-midpoint rule, limited to the pinned
//    section) and a thin accent marker in it; without pinnable files nothing happens there.
// Dragged onto the hidden dock's 1 px strip, the dock reveals itself right away. Open-with and
// recycle run from a posted message once Drop has returned (the drag source is not kept waiting).

std::vector<std::wstring> DockWindow::DroppedFiles(IDataObject* data) {
    std::vector<std::wstring> files;
    if (!data) return files;
    FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    if (FAILED(data->GetData(&fe, &medium))) return files;
    if (HDROP drop = static_cast<HDROP>(GlobalLock(medium.hGlobal))) {
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            const UINT len = DragQueryFileW(drop, i, nullptr, 0);
            if (len == 0) continue;
            std::wstring path(len + 1, L'\0');
            DragQueryFileW(drop, i, path.data(), len + 1);
            path.resize(len);
            files.push_back(std::move(path));
        }
        GlobalUnlock(medium.hGlobal);
    }
    ReleaseStgMedium(&medium);
    return files;
}

std::vector<std::wstring> DockWindow::PinnableFiles(const std::vector<std::wstring>& files) {
    std::vector<std::wstring> pinnable;
    for (const std::wstring& path : files) {
        const size_t dot = path.rfind(L'.');
        const std::wstring ext = dot == std::wstring::npos ? L"" : AppIdentity::Normalize(path.substr(dot));
        const DWORD attrs = GetFileAttributesW(path.c_str());
        const bool folder = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
        if (folder || ext == L".lnk" || ext == L".exe") pinnable.push_back(path);  // only these can be pinned
    }
    return pinnable;
}

namespace {
DWORD DropEffect(DWORD allowed) {
    if (allowed & DROPEFFECT_LINK) return DROPEFFECT_LINK;
    if (allowed & DROPEFFECT_COPY) return DROPEFFECT_COPY;
    return DROPEFFECT_NONE;
}
// Open with: the files are handed to the app (a copy from the source's point of view).
DWORD OpenEffect(DWORD allowed) {
    if (allowed & DROPEFFECT_COPY) return DROPEFFECT_COPY;
    if (allowed & DROPEFFECT_LINK) return DROPEFFECT_LINK;
    return DROPEFFECT_NONE;
}
// Recycle: shown as a move while dragging (the drop itself reports none, see OnDrop).
DWORD RecycleEffect(DWORD allowed) {
    if (allowed & DROPEFFECT_MOVE) return DROPEFFECT_MOVE;
    return OpenEffect(allowed);
}
}  // namespace

HRESULT DockWindow::RenderMarker() {
    const float s = layout_.scale;
    const UINT w = static_cast<UINT>(std::max(2.0f, std::round(kMarkerWidth * s)));
    const UINT h = static_cast<UINT>(layout_.iconPx);
    const D2D1_SIZE_F size = layout_.PhysSize(static_cast<float>(w), static_cast<float>(h));
    if (marker_.width == static_cast<UINT>(size.width) && marker_.height == static_cast<UINT>(size.height)) return S_OK;
    const D2D1_COLOR_F color = Theme::ToColor(DockTheme().accent);
    return DrawCanonical(marker_, static_cast<float>(w), static_cast<float>(h), [&](ID2D1DeviceContext* dc) {
        winrt::com_ptr<ID2D1SolidColorBrush> brush;
        if (FAILED(dc->CreateSolidColorBrush(color, brush.put()))) return;
        const float r = static_cast<float>(w) / 2.0f;
        dc->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(0, 0, static_cast<float>(w), static_cast<float>(h)), r, r), brush.get());
    });
}

DockWindow::DropMode DockWindow::DropModeAt(float stageX, int* item, int* gap) const {
    stageX = RestMain(stageX);  // rest coordinates (a grown bar: 9A follow-up)
    *item = -1;
    *gap = -1;
    if (!trayDragKey_.empty()) {
        // A tray icon: onto a tray grid it takes the place under the pointer; elsewhere on the
        // icon row it becomes a trayApp pin at the insertion marker.
        if (items_.empty() || DockCfg().locked) return DropMode::None;
        const int slot = SlotAt(stageX);
        if (slot >= 0 && items_[static_cast<size_t>(slot)].kind == DockItem::Kind::Tray) {
            *item = slot;
            return DropMode::TrayOrder;
        }
        *gap = DropGapIndex(stageX);
        return *gap >= 0 ? DropMode::Insert : DropMode::None;
    }
    if (!memberDragGroup_.id.empty()) {
        // A group member pulled out of its popup: it becomes a pin at the insertion marker.
        if (items_.empty() || DockCfg().locked) return DropMode::None;
        *gap = DropGapIndex(stageX);
        return *gap >= 0 ? DropMode::Insert : DropMode::None;
    }
    const int slot = SlotAt(stageX);
    if (slot >= 0) {
        const DockItem& it = items_[static_cast<size_t>(slot)];
        const float edge = it.slotW * kDropEdge;
        const bool central = stageX >= it.slotX + edge && stageX < it.slotX + it.slotW - edge;
        // An app needs something to start (not a bare window-class fallback).
        const bool openable =
            it.kind == DockItem::Kind::App && (!it.key.IsClassFallback() || it.launchPath.has_value());
        if (central && openable) {
            *item = slot;
            return DropMode::OpenWith;
        }
        if (central && it.kind == DockItem::Kind::RecycleBin) {
            *item = slot;
            return DropMode::Recycle;
        }
        // A group takes pinnable files as members (other files: the no-drop cursor below, as on
        // the dock between icons when nothing can be pinned).
        if (central && it.kind == DockItem::Kind::Group && dropPinnable_ && !DockCfg().locked) {
            *item = slot;
            return DropMode::Group;
        }
        // A script widget that takes files: the whole slot.
        if (it.kind == DockItem::Kind::Widget) {
            if (const WidgetInstance* w = WidgetAt(slot); w && w->AcceptsDrops()) {
                *item = slot;
                return DropMode::Widget;
            }
        }
    }
    // A locked dock pins nothing (open with / recycle still work): the no-drop cursor.
    if (!dropPinnable_ || items_.empty() || DockCfg().locked) return DropMode::None;
    *gap = DropGapIndex(stageX);
    return *gap >= 0 ? DropMode::Insert : DropMode::None;  // no pin and no running app: nowhere to insert
}

void DockWindow::SetDropTarget(DropMode mode, int item, int gap) {
    if (items_.empty()) gap = -1;  // nothing to make room between
    if (mode == dropMode_ && item == dropItem_ && gap == dropGap_) return;
    const bool markerWasShown = dropGap_ >= 0;
    dropMode_ = mode;
    dropItem_ = item;
    dropGap_ = gap;
    RenderWithRecovery([&] {
        HRESULT hr = S_OK;
        if (gap >= 0) {
            hr = RenderMarker();
        } else {
            renderer_.SetVisible(marker_, false);
            markerX_.valid = false;
        }
        if (item >= 0) SetBigIcons(true);  // the target icon is shown above its rest size
        ComputeTargets();
        ApplyTargets(motion_.magnify, true);
        if (gap >= 0 && !markerWasShown) renderer_.SetVisible(marker_, true);
        return hr;
    });
}

DWORD DockWindow::OnDragEnter(IDataObject* data, POINT screenPt, DWORD allowed) {
    memberDragGroup_ = AppKey{};
    memberDragIndex_ = SIZE_MAX;
    trayDragKey_ = ReadTrayDrag(data);
    MemberDragData member;
    const bool memberDrag = trayDragKey_.empty() && ReadMemberDrag(data, member);
    const std::vector<std::wstring> files =
        memberDrag || !trayDragKey_.empty() ? std::vector<std::wstring>{} : DroppedFiles(data);
    if (!trayDragKey_.empty()) {
        // Any dock of this process takes a tray icon (each has its own grid / pins).
        dropAccept_ = dragPhase_ == DragPhase::None && !DockCfg().locked;
        dropPinnable_ = dropAccept_;
        if (!dropAccept_) trayDragKey_.clear();
    } else if (memberDrag) {
        // Only the dock that owns the group takes its member (groups are per dock).
        dropAccept_ = member.dock == reinterpret_cast<UINT64>(hwnd_) && dragPhase_ == DragPhase::None;
        dropPinnable_ = dropAccept_;
        if (dropAccept_) {
            memberDragGroup_.id = member.group;
            memberDragIndex_ = member.member;
        }
    } else {
        dropAccept_ = !files.empty() && dragPhase_ == DragPhase::None;
        dropPinnable_ = dropAccept_ && !PinnableFiles(files).empty();
    }
    if (!dropAccept_ || allowed == DROPEFFECT_NONE) return DROPEFFECT_NONE;
    dropActive_ = true;
    dropMode_ = DropMode::None;
    dropItem_ = -1;
    SetHold(true);  // no auto-hide or label while the file is over the dock
    if (hideState_ == HideState::Hidden || hideState_ == HideState::Hiding) {
        // A file dragged to the screen edge: reveal right away (no reveal delay).
        StartShow();
    }
    magnifying_ = false;
    if (magnifyPending_) {
        KillTimer(hwnd_, kMagnifyTimerId);
        magnifyPending_ = false;
    }
    SetHover(-1);
    if (!expanded_) {
        expanded_ = true;
        UpdateWindowRect();
    }
    return OnDragOver(screenPt, allowed);
}

DWORD DockWindow::OnDragOver(POINT screenPt, DWORD allowed) {
    if (!dropActive_) return DROPEFFECT_NONE;
    const bool shown = hideState_ == HideState::Shown || hideState_ == HideState::Showing;
    int item = -1, gap = -1;
    dropMain_ = DropMainAt(screenPt);
    // Near a viewport end of a scrolling run: it scrolls that way (the drop target follows).
    UpdateAutoScroll(shown ? dropMain_ : 0.0f);
    if (!shown) StopAutoScroll();
    const DropMode mode = shown ? DropModeAt(dropMain_, &item, &gap) : DropMode::None;
    SetDropTarget(mode, item, gap);
    switch (mode) {
        case DropMode::Insert:
            if (!memberDragGroup_.id.empty() || !trayDragKey_.empty()) return allowed & DROPEFFECT_MOVE;
            return DropEffect(allowed);
        case DropMode::TrayOrder:
            return allowed & DROPEFFECT_MOVE;
        case DropMode::Group:
            return DropEffect(allowed);
        case DropMode::OpenWith:
        case DropMode::Widget:
            return OpenEffect(allowed);
        case DropMode::Recycle:
            return RecycleEffect(allowed);
        default:
            return DROPEFFECT_NONE;
    }
}

void DockWindow::OnDragLeave() {
    memberDragGroup_ = AppKey{};
    memberDragIndex_ = SIZE_MAX;
    trayDragKey_.clear();
    StopAutoScroll();
    if (!dropActive_) return;
    SetDropTarget(DropMode::None, -1, -1);
    dropActive_ = false;
    KeepStage(motion_.magnify + 0.04);
    SetHold(false);
}

DWORD DockWindow::OnDrop(IDataObject* data, POINT screenPt, DWORD allowed) {
    StopAutoScroll();
    if (!dropActive_) return DROPEFFECT_NONE;
    const std::vector<std::wstring> files = DroppedFiles(data);
    int item = -1, gap = -1;
    const bool shown = hideState_ == HideState::Shown || hideState_ == HideState::Showing;
    const DropMode mode = shown ? DropModeAt(DropMainAt(screenPt), &item, &gap) : DropMode::None;
    DWORD effect = DROPEFFECT_NONE;
    const AppKey memberGroup = memberDragGroup_;
    const size_t memberIndex = memberDragIndex_;
    const std::wstring trayKey = std::move(trayDragKey_);
    memberDragGroup_ = AppKey{};
    memberDragIndex_ = SIZE_MAX;
    trayDragKey_.clear();
    if (!trayKey.empty() && (mode == DropMode::Insert || mode == DropMode::TrayOrder)) {
        // A tray icon: a new place in the grid under the pointer, or a trayApp pin at the marker.
        const size_t at = ConfigIndexOfItem(static_cast<size_t>(std::max(gap, 0)));
        const size_t trayItem = item >= 0 ? items_[static_cast<size_t>(item)].element : SIZE_MAX;
        const std::wstring before =
            mode == DropMode::TrayOrder && item >= 0 ? ElementOf(static_cast<size_t>(item)).TrayKeyAt(item, screenPt) : std::wstring();
        dropActive_ = false;
        dropMode_ = DropMode::None;
        dropGap_ = -1;
        renderer_.SetVisible(marker_, false);
        markerX_.valid = false;
        if (mode == DropMode::TrayOrder) {
            if (before != trayKey && pinRequests_.trayOrder) pinRequests_.trayOrder(trayItem, trayKey, before);
        } else if (pinRequests_.pinTray) {
            pinRequests_.pinTray(trayKey, at);
        }
        effect = allowed & DROPEFFECT_MOVE;
    } else if (mode == DropMode::Insert && !memberGroup.id.empty()) {
        // A member pulled out of its group: a pin at the marker.
        const size_t at = ConfigIndexOfItem(static_cast<size_t>(std::max(gap, 0)));
        dropActive_ = false;
        dropMode_ = DropMode::None;
        dropGap_ = -1;
        renderer_.SetVisible(marker_, false);
        markerX_.valid = false;
        if (pinRequests_.memberOut) pinRequests_.memberOut(memberGroup, memberIndex, at);
        effect = allowed & DROPEFFECT_MOVE;
    } else if (mode == DropMode::Group) {
        const std::vector<std::wstring> pinnable = PinnableFiles(files);
        const AppKey group = item >= 0 ? items_[static_cast<size_t>(item)].key : AppKey{};
        SetDropTarget(DropMode::None, -1, -1);
        dropActive_ = false;
        if (!pinnable.empty() && !group.id.empty() && pinRequests_.addToGroup) pinRequests_.addToGroup(group, pinnable);
        effect = pinnable.empty() ? DROPEFFECT_NONE : DropEffect(allowed);
    } else if (mode == DropMode::Insert) {
        const std::vector<std::wstring> pinnable = PinnableFiles(files);
        const size_t at = ConfigIndexOfItem(static_cast<size_t>(std::max(gap, 0)));
        // The gap closes as the new icon takes its slot: no closing animation, the rebuilt layout
        // (one slot wider, re-centred) puts everything where the gap layout had it.
        dropActive_ = false;
        dropMode_ = DropMode::None;
        dropGap_ = -1;
        renderer_.SetVisible(marker_, false);
        markerX_.valid = false;
        if (!pinnable.empty() && pinRequests_.pinFiles) pinRequests_.pinFiles(pinnable, at);
        effect = pinnable.empty() ? DROPEFFECT_NONE : DropEffect(allowed);
    } else {
        if ((mode == DropMode::OpenWith || mode == DropMode::Recycle || mode == DropMode::Widget) && !files.empty() &&
            item >= 0 && hwnd_) {
            dropAction_ = DropAction{mode, items_[static_cast<size_t>(item)].key, files, screenPt};
            PostMessageW(hwnd_, kDropActionMessage, 0, 0);
            // A recycle is done by us (the files are deleted, not moved by the source): report no
            // effect so the source does not try to delete them itself.
            effect = mode == DropMode::Recycle ? DROPEFFECT_NONE : OpenEffect(allowed);
        }
        SetDropTarget(DropMode::None, -1, -1);  // the icon shrinks back
        dropActive_ = false;
    }
    // Nothing new was pinned (duplicates) or the target shrinks back: settle.
    RenderWithRecovery([this] {
        ComputeTargets();
        ApplyTargets(motion_.rest);
        return S_OK;
    });
    KeepStage(motion_.rest + 0.04);
    SetHold(false);
    return effect;
}

void DockWindow::OnDropAction() {
    DropAction action = std::move(dropAction_);
    dropAction_ = DropAction{};
    if (action.files.empty()) return;
    if (action.mode == DropMode::Recycle) {
        if (!Launcher::Recycle(action.files, hwnd_)) MessageBeep(MB_ICONWARNING);
        return;  // the bin's change notification redraws its icon
    }
    if (action.mode == DropMode::Widget) {
        for (size_t i = 0; i < items_.size(); ++i) {
            if (items_[i].key != action.key) continue;
            WidgetInstance* w = WidgetAt(static_cast<int>(i));
            if (!w) return;
            const D2D1_RECT_F box = SlotScreenBox(static_cast<int>(i));
            const float s = layout_.scale > 0.0f ? layout_.scale : 1.0f;
            w->Drop(action.files, (static_cast<float>(action.screen.x) - box.left) / s,
                    (static_cast<float>(action.screen.y) - box.top) / s);
            return;
        }
        return;
    }
    if (action.mode != DropMode::OpenWith) return;
    int index = -1;
    for (size_t i = 0; i < items_.size() && index < 0; ++i) {
        if (items_[i].key == action.key && items_[i].kind == DockItem::Kind::App) index = static_cast<int>(i);
    }
    if (index < 0) return;
    // OpenWith may pump messages (and so rebuild items_): from a copy, then find the item again.
    const DockItem& target = items_[static_cast<size_t>(index)];
    DockItem copy;
    copy.key = target.key;
    copy.kind = target.kind;
    copy.launchPath = target.launchPath;
    copy.workingDir = target.workingDir;
    copy.targetPath = target.targetPath;
    const size_t windowsBefore = target.windows.size();
    if (!Launcher::OpenWith(copy, action.files)) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    // A running app usually opens the file in a window it already has: bounce only when it starts.
    if (windowsBefore > 0) return;
    for (DockItem& item : items_) {
        if (item.key != copy.key) continue;
        if (item.windows.empty() && item.bounce == DockItem::Bounce::None) StartBounce(item, 0);
        break;
    }
}

// --- App bar (reserve screen space) ----------------------------------------------------------------
// A full-width dock with reserveSpace and auto-hide "off" registers its window as an app bar
// (SHAppBarMessage ABM_NEW), so the shell takes the bar's band along the edge out of the work
// area and maximized windows stop at the dock. With auto-hide on nothing is reserved (like an
// auto-hide taskbar: the dock would never hide in intelligent mode, and "always" hides over the
// windows anyway). The owner (App) allows the registration only while the taskbar controller is
// not saving or restoring the work area (AllowAppBar), and is told the reservation, which the
// controller then treats as the correct work area instead of forcing the full monitor. The
// registration is removed on every path out: config changes, WM_DESTROY, App::Shutdown before the
// taskbar is restored, and the crash filter (EmergencyRemoveAppBar).

namespace {
// The registered dock windows (crash path; one slot per reserving dock).
constexpr size_t kMaxAppBars = 32;
std::atomic<HWND> g_appBarHwnds[kMaxAppBars]{};

void RememberAppBar(HWND hwnd) {
    for (auto& slot : g_appBarHwnds) {
        HWND expected = nullptr;
        if (slot.compare_exchange_strong(expected, hwnd)) return;
    }
}

void ForgetAppBar(HWND hwnd) {
    for (auto& slot : g_appBarHwnds) {
        HWND expected = hwnd;
        if (slot.compare_exchange_strong(expected, nullptr)) return;
    }
}
}  // namespace

void DockWindow::EmergencyRemoveAppBar() {
    for (auto& slot : g_appBarHwnds) {
        HWND h = slot.exchange(nullptr);
        if (!h) continue;
        APPBARDATA abd{sizeof(abd)};
        abd.hWnd = h;
        SHAppBarMessage(ABM_REMOVE, &abd);
    }
}

void DockWindow::AllowAppBar(bool allow) {
    appBarAllowed_ = allow;
    if (allow) {
        UpdateAppBar();
    } else {
        RemoveAppBar();
    }
}

RECT DockWindow::AppBarRect() const {
    // The resting bar's band from the screen edge (its margin included: full-width bars have
    // none), along the whole edge of the monitor (an app bar reserves whole edges anyway).
    const Layout& l = layout_;
    RECT r = StageToScreen(l.bar.left, l.bar.top, l.bar.right, l.stageCross, true);
    if (l.vertical) {
        r.top = l.monitor.top;
        r.bottom = l.monitor.bottom;
    } else {
        r.left = l.monitor.left;
        r.right = l.monitor.right;
    }
    return r;
}

void DockWindow::UpdateAppBar() {
    const DockConfig& d = DockCfg();
    // Any edge, full width or not; only a dock that does not auto-hide reserves space.
    // (Not on the desktop layer: that dock is only shown when no window is in the way.)
    const bool wanted = hwnd_ && appBarAllowed_ && d.reserveSpace && !d.AutoHideOn() && d.layer == DockLayer::Normal &&
                        !d.Polar() &&      // a ring reserves no band (it does not span its edge)
                        !d.Floating();  // nor does a floating bar (it is not on an edge)
    if (!wanted) {
        RemoveAppBar();
        return;
    }
    if (appBarBusy_) return;  // ABM_SETPOS broadcasts WM_SETTINGCHANGE, which relayouts (re-enters)
    const RECT rect = AppBarRect();
    if (appBar_ && EqualRect(&rect, &appBarRect_)) return;
    appBarBusy_ = true;
    APPBARDATA abd{sizeof(abd)};
    abd.hWnd = hwnd_;
    if (!appBar_) {
        abd.uCallbackMessage = kAppBarMessage;
        if (!SHAppBarMessage(ABM_NEW, &abd)) {
            Log(L"ABM_NEW", E_FAIL);
            appBarBusy_ = false;
            return;
        }
        appBar_ = true;
        RememberAppBar(hwnd_);
    }
    static const UINT kEdges[] = {ABE_BOTTOM, ABE_TOP, ABE_LEFT, ABE_RIGHT};
    const Layout& l = layout_;
    abd.uEdge = kEdges[static_cast<int>(l.edge)];
    abd.rc = rect;
    SHAppBarMessage(ABM_QUERYPOS, &abd);
    // The shell may move the rect off another app bar on that edge: keep our thickness.
    const LONG thick = l.vertical ? rect.right - rect.left : rect.bottom - rect.top;
    switch (l.edge) {
        case DockEdge::Top: abd.rc.bottom = abd.rc.top + thick; break;
        case DockEdge::Left: abd.rc.right = abd.rc.left + thick; break;
        case DockEdge::Right: abd.rc.left = abd.rc.right - thick; break;
        case DockEdge::Bottom:
        default: abd.rc.top = abd.rc.bottom - thick; break;
    }
    // The work area this leaves, told first: ABM_SETPOS broadcasts the work area change while it
    // runs, and the taskbar controller must not "fix" it back to the full monitor.
    RECT work = l.monitor;
    switch (l.edge) {
        case DockEdge::Top: work.top = std::max(work.top, abd.rc.bottom); break;
        case DockEdge::Left: work.left = std::max(work.left, abd.rc.right); break;
        case DockEdge::Right: work.right = std::min(work.right, abd.rc.left); break;
        case DockEdge::Bottom:
        default: work.bottom = std::min(work.bottom, abd.rc.top); break;
    }
    appBarRect_ = rect;
    if (appBarChanged_) appBarChanged_(true, l.monitor, work);
    SHAppBarMessage(ABM_SETPOS, &abd);
    appBarBusy_ = false;
}

void DockWindow::RemoveAppBar() {
    if (!appBar_) return;
    appBar_ = false;
    appBarRect_ = RECT{};
    if (appBarChanged_) appBarChanged_(false, layout_.monitor, layout_.monitor);
    APPBARDATA abd{sizeof(abd)};
    abd.hWnd = hwnd_;
    SHAppBarMessage(ABM_REMOVE, &abd);
    ForgetAppBar(hwnd_);
    if (fullscreenApp_) {
        fullscreenApp_ = false;
        AssertTopmost();
    }
}

void DockWindow::OnAppBarNotify(WPARAM wp, LPARAM lp) {
    if (!appBar_) return;
    switch (wp) {
        case ABN_POSCHANGED:
            // Another app bar or the taskbar changed: ask for our place again.
            appBarRect_ = RECT{};
            UpdateAppBar();
            break;
        case ABN_FULLSCREENAPP:
            // A fullscreen window opened / closed: step out of its way, then come back on top.
            fullscreenApp_ = lp != 0;
            // Round 9 phase 9A: with the full-screen guard on (settings.fullscreen "hide") the shell's
            // notice is one more input of the guard, which hides / shows every topmost window on the
            // monitor without z-order calls; "ignore" keeps the app bar's own answer below.
            if (shared_ && shared_->FullscreenNotice(this, fullscreenApp_)) break;
            if (fullscreenApp_) {
                // Out of the topmost band, then to the bottom of the z-order.
                SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                SetWindowPos(hwnd_, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            } else {
                AssertTopmost();
            }
            break;
        default:
            break;
    }
}

// --- Drag the dock -----------------------------------------------------------------------------
// Ctrl + left press on the bar (anywhere on it, icons included) and a move past the system drag
// threshold moves the whole dock along its edge (the window follows the pointer; the bar is kept
// on the monitor). Released, the dock snaps to the centre or an end of the edge when within
// 24 DIP of it, otherwise it keeps the nearest alignment (start / centre / end) plus the offset
// from it: docks[0].align and offset. Dragged out of the edge's band (a quarter of the monitor)
// toward another edge (the nearest, within 40 % of the monitor), the dock dims and a release
// puts it on that edge there. Escape or losing the capture cancels. A locked dock does not move;
// a full-width dock only changes edges. No timer runs but the 50 ms Escape poll during the drag.

DockEdge DockWindow::DockZoneAt(POINT pt) const {
    const RECT& m = layout_.monitor;
    const float w = std::max(1.0f, static_cast<float>(m.right - m.left));
    const float h = std::max(1.0f, static_cast<float>(m.bottom - m.top));
    // Distance to each edge as a fraction of the monitor's size across it, in DockEdge order.
    const float dist[4] = {static_cast<float>(m.bottom - pt.y) / h, static_cast<float>(pt.y - m.top) / h,
                           static_cast<float>(pt.x - m.left) / w, static_cast<float>(m.right - pt.x) / w};
    const int current = static_cast<int>(layout_.edge);
    if (dist[current] <= 0.25f) return layout_.edge;
    int best = -1;
    for (int e = 0; e < 4; ++e) {
        if (e != current && (best < 0 || dist[e] < dist[best])) best = e;
    }
    return best >= 0 && dist[best] < 0.4f ? static_cast<DockEdge>(best) : layout_.edge;
}

void DockWindow::BeginDockDrag() {
    dockPress_ = false;
    dockDrag_ = true;
    dockShift_ = 0;
    dockShiftY_ = 0;
    dockZone_ = layout_.edge;
    dockStartBar_ = RestBarRect();
    // Round 12 phase 12E: customize mode's drag starts from the dock's own offset and gap.
    gapShift_ = 0;
    moveOffset_ = DockCfg().offset;
    moveGap_ = DockCfg().edgeGap;
    readoutText_.clear();
    pressed_ = -1;
    GetAsyncKeyState(VK_ESCAPE);  // clears the "pressed since the last call" bit
    SetCapture(hwnd_);
    SetHold(true);  // no label, popups or auto-hide while moving
    magnifying_ = false;
    if (magnifyPending_) {
        KillTimer(hwnd_, kMagnifyTimerId);
        magnifyPending_ = false;
    }
    SetHover(-1);
    if (!expanded_) {
        expanded_ = true;
        UpdateWindowRect();
    }
    RenderWithRecovery([this] {
        ComputeTargets();
        ApplyTargets(motion_.rest);  // back to rest: the dock moves as it is
        return S_OK;
    });
    SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
    SetTimer(hwnd_, kDragTimerId, kDragPollMs, nullptr);
    dragPending_ = true;
    UpdateDockDrag();
}

void DockWindow::UpdateDockDrag() {
    if (!dockDrag_) return;
    POINT pt{};
    if (!GetCursorPos(&pt)) return;
    Layout& l = layout_;
    if (l.shape == DockShape::Circle && l.polar.on) {
        // A circle floats: it follows the pointer anywhere on its monitor (its ring kept on it).
        const RECT ring = RingScreenRect();
        const LONG x0 = ring.left - dockShift_, x1 = ring.right - dockShift_, y0 = ring.top - dockShiftY_, y1 = ring.bottom - dockShiftY_;
        const LONG sx = std::clamp(pt.x - dockPressScreen_.x, std::min(0L, l.monitor.left - x0), std::max(0L, l.monitor.right - x1));
        const LONG sy = std::clamp(pt.y - dockPressScreen_.y, std::min(0L, l.monitor.top - y0), std::max(0L, l.monitor.bottom - y1));
        if (sx != dockShift_ || sy != dockShiftY_) {
            OffsetRect(&l.window, sx - dockShift_, sy - dockShiftY_);
            OffsetRect(&winRect_, sx - dockShift_, sy - dockShiftY_);
            dockShift_ = sx;
            dockShiftY_ = sy;
            SetWindowPos(hwnd_, nullptr, winRect_.left, winRect_.top, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            SyncBackdrop();
        }
        return;
    }
    if (l.floating) {
        // A floating bar (round 6 phase 6A) follows the pointer anywhere on its monitor (the grab
        // offset kept), snapping within kFloatSnapDip to the work area's edges and to the other
        // floating docks' bars, and kept on the monitor.
        RECT r = dockStartBar_;
        OffsetRect(&r, pt.x - dockPressScreen_.x, pt.y - dockPressScreen_.y);
        const LONG thr = static_cast<LONG>(std::lround(kFloatSnapDip * l.scale));
        std::vector<LONG> xs{l.work.left, l.work.right}, ys{l.work.top, l.work.bottom};
        if (shared_) {
            for (const RECT& n : shared_->FloatingNeighbours(this)) {
                xs.push_back(n.left);
                xs.push_back(n.right);
                ys.push_back(n.top);
                ys.push_back(n.bottom);
            }
        }
        auto best = [&](LONG a, LONG b, const std::vector<LONG>& cs) {
            LONG shift = 0, dist = thr + 1;
            for (LONG c : cs) {
                if (std::labs(c - a) < dist) {
                    dist = std::labs(c - a);
                    shift = c - a;
                }
                if (std::labs(c - b) < dist) {
                    dist = std::labs(c - b);
                    shift = c - b;
                }
            }
            return dist <= thr ? shift : 0;
        };
        OffsetRect(&r, best(r.left, r.right, xs), best(r.top, r.bottom, ys));
        const LONG w = r.right - r.left, h = r.bottom - r.top;
        const LONG left = std::clamp(r.left, l.monitor.left, std::max(l.monitor.left, l.monitor.right - w));
        const LONG top = std::clamp(r.top, l.monitor.top, std::max(l.monitor.top, l.monitor.bottom - h));
        const LONG sx = left - dockStartBar_.left, sy = top - dockStartBar_.top;
        if (sx != dockShift_ || sy != dockShiftY_) {
            OffsetRect(&l.window, sx - dockShift_, sy - dockShiftY_);
            OffsetRect(&winRect_, sx - dockShift_, sy - dockShiftY_);
            dockShift_ = sx;
            dockShiftY_ = sy;
            SetWindowPos(hwnd_, nullptr, winRect_.left, winRect_.top, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            SyncBackdrop();
        }
        return;
    }
    // Along the edge: follow the pointer, the bar kept on the monitor (a full-width bar stays). An
    // arc: its box along the edge (its layout is horizontal whatever the edge).
    const bool vertical = IsVertical(l.edge);
    const LONG mStart = DockGeometry::MainStart(l.edge, l.monitor), mEnd = DockGeometry::MainEnd(l.edge, l.monitor);
    const LONG origin = (vertical ? l.window.top : l.window.left) - dockShift_;  // the stage before the drag
    const float lo = l.polar.on ? (vertical ? l.ring.top : l.ring.left) : l.bar.left;
    const float hi = l.polar.on ? (vertical ? l.ring.bottom : l.ring.right) : l.bar.right;
    const LONG barStart = origin + static_cast<LONG>(lo), barEnd = origin + static_cast<LONG>(hi);
    const LONG delta = vertical ? pt.y - dockPressScreen_.y : pt.x - dockPressScreen_.x;
    const LONG shiftLo = std::min(0L, mStart - barStart), shiftHi = std::max(0L, mEnd - barEnd);
    LONG shift = std::clamp(delta, shiftLo, shiftHi);
    const DockEdge zone = DockZoneAt(pt);
    LONG gap = 0;
    if (editMove_) {
        // Round 12 phase 12E: customize mode's drag. Along the edge: the offset from the aligned
        // place (the alignment is kept); a centred dock stays at 0 while the pointer is within
        // kEditSnapDip of it (the detent: a short pause in the centre). Toward the middle of the
        // screen: the edge gap (0 - 200 DIPs), only while the pointer is over the dock's own edge.
        const DockConfig& dc = DockCfg();
        const float s = l.scale;
        int offset = dc.offset + static_cast<int>(std::lround(static_cast<double>(shift) / s));
        if (dc.align == DockAlign::Center && std::abs(offset) <= static_cast<int>(kEditSnapDip)) {
            shift = std::clamp(static_cast<LONG>(-std::lround(static_cast<double>(dc.offset) * s)), shiftLo, shiftHi);
            offset = 0;
        }
        moveOffset_ = std::clamp(offset, -16000, 16000);
        const LONG across = l.edge == DockEdge::Top    ? pt.y - dockPressScreen_.y
                            : l.edge == DockEdge::Left  ? pt.x - dockPressScreen_.x
                            : l.edge == DockEdge::Right ? dockPressScreen_.x - pt.x
                                                        : dockPressScreen_.y - pt.y;  // toward the middle
        moveGap_ = zone == l.edge ? std::clamp(dc.edgeGap + static_cast<int>(std::lround(static_cast<double>(across) / s)), 0,
                                               DockConfig::kMaxEdgeGap)
                                  : dc.edgeGap;
        gap = static_cast<LONG>(std::lround(static_cast<double>(moveGap_ - dc.edgeGap) * s));
    }
    if (shift != dockShift_ || gap != gapShift_) {
        const LONG d = shift - dockShift_;
        // (12E) The gap moves the window off its edge: up from a bottom edge, right from a left one ...
        const LONG g = gap - gapShift_;
        const LONG gx = l.edge == DockEdge::Left ? g : l.edge == DockEdge::Right ? -g : 0;
        const LONG gy = l.edge == DockEdge::Top ? g : l.edge == DockEdge::Bottom ? -g : 0;
        OffsetRect(&l.window, (vertical ? 0 : d) + gx, (vertical ? d : 0) + gy);
        OffsetRect(&winRect_, (vertical ? 0 : d) + gx, (vertical ? d : 0) + gy);
        dockShift_ = shift;
        gapShift_ = gap;
        SetWindowPos(hwnd_, nullptr, winRect_.left, winRect_.top, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        SyncBackdrop();
    }
    if (editMove_) ShowReadout(pt);  // round 12 phase 12E
    // Over another edge's zone the dock dims: a release moves it there.
    if (zone != dockZone_) {
        dockZone_ = zone;
        RenderWithRecovery([&] {
            if (IDCompositionVisual3* root3 = renderer_.Root3()) {
                renderer_.Drive(
                    rootOpacity_, zone == l.edge ? 1.0f : 0.5f, motion_.labelFade, Renderer::Now(),
                    [&](auto v) { return root3->SetOpacity(v); }, 0.001f, renderer_.FadeEasing());
            }
            return S_OK;
        });
    }
}

void DockWindow::EndDockDrag(bool commit) {
    if (!dockDrag_) return;
    if (commit && (GetAsyncKeyState(VK_ESCAPE) & 0x8001)) commit = false;
    dockDrag_ = false;  // first: releasing the capture sends WM_CAPTURECHANGED
    if (dragPending_ && dragPhase_ == DragPhase::None) {
        KillTimer(hwnd_, kDragTimerId);
        dragPending_ = false;
    }
    if (GetCapture() == hwnd_) ReleaseCapture();
    RenderWithRecovery([this] {
        if (IDCompositionVisual3* root3 = renderer_.Root3()) {
            renderer_.Drive(
                rootOpacity_, 1.0f, 0.0, Renderer::Now(), [&](auto v) { return root3->SetOpacity(v); }, 0.001f,
                renderer_.FadeEasing());
        }
        return S_OK;
    });
    const DockConfig& d = DockCfg();
    POINT pt{};
    // --- round 12 phase 12E: customize mode's drag released on its own edge: offset and gap (one
    // undo entry); over another edge's zone it falls through to the Ctrl+drag path below (the edge
    // switch, with Keep / Revert). ---
    const bool editMove = editMove_;
    editMove_ = false;
    gapShift_ = 0;
    HideReadout();
    if (editMove && commit && GetCursorPos(&pt) && DockZoneAt(pt) == layout_.edge) {
        SetHold(false);
        if (!pinRequests_.moveInEdge || (moveOffset_ == d.offset && moveGap_ == d.edgeGap)) {
            Relayout();  // back to its own place
            return;
        }
        pinRequests_.moveInEdge(moveOffset_, moveGap_);  // relayouts the dock
        return;
    }
    // --- end round 12 phase 12E ---
    if (layout_.shape == DockShape::Circle && layout_.polar.on) {
        // A circle: its centre, in DIPs from the work area's top-left.
        const Layout& l = layout_;
        if (!commit || !pinRequests_.moveCircle || (dockShift_ == 0 && dockShiftY_ == 0)) {
            Relayout();
            SetHold(false);
            return;
        }
        const float cx = static_cast<float>(l.window.left) + l.polar.cx, cy = static_cast<float>(l.window.top) + l.polar.cy;
        const int x = static_cast<int>(std::lround((cx - static_cast<float>(l.work.left)) / l.scale));
        const int y = static_cast<int>(std::lround((cy - static_cast<float>(l.work.top)) / l.scale));
        SetHold(false);
        pinRequests_.moveCircle(x, y);
        return;
    }
    if (layout_.floating) {
        // A floating bar: its resting top-left, in DIPs from the work area's top-left.
        const Layout& l = layout_;
        if (!commit || !pinRequests_.moveCircle || (dockShift_ == 0 && dockShiftY_ == 0)) {
            Relayout();
            SetHold(false);
            return;
        }
        const RECT bar = RestBarRect();
        const int x = static_cast<int>(std::lround(static_cast<float>(bar.left - l.work.left) / l.scale));
        const int y = static_cast<int>(std::lround(static_cast<float>(bar.top - l.work.top) / l.scale));
        SetHold(false);
        pinRequests_.moveCircle(x, y);
        return;
    }
    if (!commit || !pinRequests_.moveDock || !GetCursorPos(&pt)) {
        Relayout();  // back to where it was
        SetHold(false);
        return;
    }
    const Layout& l = layout_;
    const DockEdge edge = DockZoneAt(pt);
    DockAlign align = d.align;
    int offset = d.offset;
    if (!d.fullWidth) {
        // Where the bar starts along the (new) edge (the same edge: where it was dragged to;
        // another edge: centred on the pointer), then the alignment whose place is nearest, in
        // ComputeLayout's own integer arithmetic so an undragged dock gives offset 0.
        const bool vertical = IsVertical(edge);
        const float s = l.scale;
        const LONG mStart = DockGeometry::MainStart(edge, l.monitor), mEnd = DockGeometry::MainEnd(edge, l.monitor);
        // (An arc: its diameter is the bar, as ComputeLayoutAt places it.)
        const LONG len = l.polar.on ? static_cast<LONG>(std::ceil(2.0f * l.ringOut))
                                    : static_cast<LONG>(l.bar.right - l.bar.left);  // on another edge it differs a little
        const LONG slack = static_cast<LONG>(l.slack);
        const LONG margin = static_cast<LONG>(std::round(kBottomMargin * s));
        LONG start = (vertical ? pt.y : pt.x) - len / 2;
        if (edge == l.edge) {
            const bool was = IsVertical(l.edge);
            start = l.polar.on ? (was ? l.window.top : l.window.left) +
                                     static_cast<LONG>(std::lround((was ? l.polar.cy : l.polar.cx) - static_cast<float>(len) / 2.0f))
                               : (l.vertical ? l.window.top : l.window.left) + static_cast<LONG>(l.bar.left);
        }
        start = std::clamp(start, mStart, std::max(mStart, mEnd - len));
        const LONG places[3] = {mStart + ((mEnd - mStart) - (len + 2 * slack)) / 2 + slack, mStart + margin,
                                mEnd - margin - len};  // DockAlign order
        int best = 0;
        for (int a = 1; a < 3; ++a) {
            if (std::abs(start - places[a]) < std::abs(start - places[best])) best = a;
        }
        align = static_cast<DockAlign>(best);
        const LONG off = start - places[best];
        offset = std::abs(off) <= static_cast<LONG>(std::round(kDockSnapDip * s))
                     ? 0
                     : static_cast<int>(std::lround(static_cast<double>(off) / s));
    }
    SetHold(false);
    pinRequests_.moveDock(edge, align, offset);  // relayouts the dock (also when nothing changed)
}

int DockWindow::HitTest(POINT pt) const {  // stage coordinates
    if (items_.empty() || !InZone(pt)) return -1;
    return HitAlong(layout_.Canon(pt).x);  // along the bar
}

bool DockWindow::LiveHits() const {
    // The magnified (or grown) layout: its slots are where the targets put them. A drag or a file
    // drag's gap layout keeps the rest slots (their targeting works in rest coordinates).
    const bool dragLayout = dragPhase_ != DragPhase::None || dragLanding_ || dropGap_ >= 0;
    return !dragLayout && (magnifying_ || GrownBar());
}

bool DockWindow::GrowForZoom(int slot) {
    // 9A follow-up 2: what zooms under the pointer is a magnifying slot (pins, running apps,
    // folders, the Recycle Bin, groups, thumbnails); widgets, separators and the bar's padding do
    // not (a widget growing on hover counts too: RefreshDirtyWidgets).
    if (!layout_.steady || layout_.always || grown_ || layout_.magnification <= 1.001f) return false;
    if (editing_) return false;  // round 12 phase 12D: customize mode pauses magnification (the icons stay at rest size)
    if (slot < 0 || slot >= static_cast<int>(items_.size()) || !items_[static_cast<size_t>(slot)].Magnifies()) return false;
    grown_ = true;
    ComputeTargets();  // the grown bar, the run magnified about the pointer
    return true;
}

int DockWindow::HitAlong(float x) const {
    if (items_.empty()) return -1;
    if (!LiveHits()) {
        const int slot = SlotAt(x);
        if (slot >= 0) return slot;
        // Beside a widget: its hit slack (half the gap to a neighbouring widget) still counts.
        for (size_t i = 0; i < items_.size(); ++i) {
            const DockItem& item = items_[i];
            if (item.hitSlack > 0.0f && item.slotW > 0.0f && x >= item.slotX - item.hitSlack &&
                x < item.slotX + item.slotW + item.hitSlack) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }
    // 9A follow-up: the slots as targeted now. A magnifying slot is its scaled box plus the gap,
    // centred on the box (scaled about its foot's centre); any other slot its rest width from its
    // target. The magnified layout is anchored at the pointer, so this is the rest slot under it
    // wherever the run fits; where it does not (the room beside a grown bar used up, a monitor end),
    // it is still the one drawn under the pointer.
    const Layout& l = layout_;
    if (l.view.on && (x < l.view.clipStart || x >= l.view.clipEnd)) return -1;
    auto span = [&](const DockItem& item, float& a, float& b) {
        if (item.Magnifies()) {
            const float box = BoxW(item), w = box * item.targetScale;
            a = item.targetX + box / 2.0f - w / 2.0f - BoxInset(item);
            b = a + w + l.gap;
        } else {
            a = item.targetX - BoxInset(item);
            b = a + item.slotW;
        }
    };
    for (size_t i = 0; i < items_.size(); ++i) {
        const DockItem& item = items_[i];
        if (item.IsWidget() && item.slotW <= 0.0f) continue;  // takes no room
        float a = 0.0f, b = 0.0f;
        span(item, a, b);
        if (x < a) break;  // in the spacer before it
        if (x < b) return static_cast<int>(i);
    }
    for (size_t i = 0; i < items_.size(); ++i) {
        const DockItem& item = items_[i];
        if (item.hitSlack <= 0.0f || item.slotW <= 0.0f) continue;
        float a = 0.0f, b = 0.0f;
        span(item, a, b);
        if (x >= a - item.hitSlack && x < b + item.hitSlack) return static_cast<int>(i);
    }
    return -1;
}

float DockWindow::RestMain(float x) const {
    // The grown bar moved what lies before the first / after the last magnifying slot out by its
    // room; a drag shows the run itself in rest places.
    const Layout& l = layout_;
    const float outS = GrownOutStart(), outE = GrownOutEnd();
    if ((outS <= 0.0f && outE <= 0.0f) || l.firstMag < 0 || l.lastMag < 0 || l.lastMag >= static_cast<int>(items_.size())) return x;
    const float a = items_[static_cast<size_t>(l.firstMag)].slotX;
    const float b = items_[static_cast<size_t>(l.lastMag)].slotX + items_[static_cast<size_t>(l.lastMag)].slotW;
    if (x < a) return std::min(a, x + outS);
    if (x >= b) return std::max(b, x - outE);
    return x;
}

float DockWindow::SlotPopupAnchor(int slot) const {
    if (slot < 0 || slot >= static_cast<int>(items_.size())) return 0.0f;
    const DockItem& item = items_[static_cast<size_t>(slot)];
    const float anchor = ElementOf(static_cast<size_t>(slot)).PopupAnchor(item, Context());
    // A widget's element anchors at its rest place: it is drawn where its target is (moved with the
    // grown bar's end, 9A follow-up).
    return item.IsWidget() ? anchor + (item.targetX - item.x) : anchor;
}

int DockWindow::SlotAt(float x) const {
    if (items_.empty() || x < layout_.itemsLeft) return -1;
    // A scrolling run: only what the viewport shows (scrolled-out slots lie beyond its clip).
    if (layout_.view.on && (x < layout_.view.clipStart || x >= layout_.view.clipEnd)) return -1;
    for (size_t i = 0; i < items_.size(); ++i) {
        const DockItem& item = items_[i];
        if (x < item.slotX) return -1;  // in the spacer before it
        if (x < item.slotX + item.slotW) return static_cast<int>(i);
    }
    return -1;
}

const DockItem* DockWindow::ItemAt(POINT clientPt) const {
    const int i = HitTest(ToStage(clientPt));
    return i >= 0 ? &items_[static_cast<size_t>(i)] : nullptr;
}

int DockWindow::MenuIndexAt(POINT clientPt) const {
    const int i = HitTest(ToStage(clientPt));
    if (i < 0) return -1;
    return ElementOf(static_cast<size_t>(i)).OnRightClick(i) == RightClickAction::ItemMenu ? i : -1;
}

LRESULT DockWindow::QueryItemPoint(WPARAM wp, LPARAM lp) const {
    int index = -1;
    if (wp) {
        index = IndexOfWindow(reinterpret_cast<HWND>(wp));
    } else if (lp >= 0 && lp < static_cast<LPARAM>(items_.size())) {
        index = static_cast<int>(lp);
    }
    if (index < 0) return -1;
    const DockItem& item = items_[static_cast<size_t>(index)];
    // Rest layout, relative to the shown window position (also while hidden).
    const D2D1_POINT_2F c = layout_.Phys(item.x + BoxW(item) / 2.0f, item.y + BoxCross(item) / 2.0f);
    // (A ring's points are rounded; a bar's truncated, as always.)
    const LONG cx = layout_.polar.on ? std::lround(c.x) : static_cast<LONG>(c.x);
    const LONG cy = layout_.polar.on ? std::lround(c.y) : static_cast<LONG>(c.y);
    POINT pt{layout_.window.left + cx, layout_.window.top + cy};
    return static_cast<LRESULT>(MAKELONG(static_cast<WORD>(pt.x), static_cast<WORD>(pt.y)));
}

LRESULT DockWindow::QueryState(WPARAM wp, LPARAM lp) const {
    switch (wp) {
        case 0:
            return static_cast<LRESULT>(hideState_);
        case 1: {
            QUERY_USER_NOTIFICATION_STATE quns{};
            return SUCCEEDED(SHQueryUserNotificationState(&quns)) ? static_cast<LRESULT>(quns) : -1;
        }
        case 2:
            return hovered_;
        case 3:
            return labelShown_ ? 1 : 0;
        case 4:
            return magnifying_ ? 1 : 0;
        case 5:
            return (hidePending_ ? 1 : 0) + (revealPending_ ? 1 : 0) + (magnifyPending_ ? 1 : 0) +
                   (labelPending_ ? 1 : 0) + (labelFading_ ? 1 : 0) + (restPending_ ? 1 : 0) +
                   (hideState_ == HideState::Hiding || hideState_ == HideState::Showing ? 1 : 0) +
                   (backdropPending_ ? 1 : 0) + (dragPending_ ? 1 : 0) + (removePending_ ? 1 : 0) +
                   (previewPending_ ? 1 : 0) + (previewLeavePending_ ? 1 : 0) + (preview_.FadePending() ? 1 : 0) +
                   (folder_.FadePending() ? 1 : 0) + (folder_.ViewTimerPending() ? 1 : 0) +
                   (weatherPopup_.FadePending() ? 1 : 0) +
                   (calendar_.FadePending() ? 1 : 0) + (binPending_ ? 1 : 0) + (groupHoldPending_ ? 1 : 0) +
                   (watching_ && shared_->Watch().Pending() ? 1 : 0) + (hintPending_ ? 1 : 0) +
                   (autoScrollPending_ ? 1 : 0) + (flashSteps_ > 0 ? 1 : 0);
        case 6:
            return acrylic_ ? 1 : 0;
        case 7:
            return dragPhase_ == DragPhase::Dragging ? 1 : dragPhase_ == DragPhase::Removing ? 2 : dropActive_ ? 3 : 0;
        case 8:
            return GetCapture() == hwnd_ ? 1 : 0;
        case 9:
            return dragPhase_ == DragPhase::Dragging ? dragInsert_ : dropActive_ ? dropGap_ : -1;
        case 10:
            return dropTarget_ ? 1 : 0;
        case 11:
            return preview_.Visible() ? 1 : 0;
        case 12:
            return static_cast<LRESULT>(preview_.CardCount());
        case 13:
            return reinterpret_cast<LRESULT>(preview_.CardWindow(static_cast<size_t>(lp)));
        case 14:
            return static_cast<LRESULT>(DockCfg().autoHide);  // 0 off, 1 always, 2 intelligent
        case 15:
            return overlapped_ ? 1 : 0;
        case 17:
            return motion_.reduce ? 1 : 0;
        case 19:
            return static_cast<LRESULT>(shared_->Watch().AcceptedEvents());
        case 20:
            return config_->themeResolveFailed ? 1 : 0;
        case 21:
            return static_cast<LRESULT>(shared_->Watch().DeliveredEvents());
        case 22:
            return folder_.Visible() ? 1 : 0;
        case 23:
            return static_cast<LRESULT>(folder_.EntryCount());
        case 97:  // the tray takeover runs (TrayHost owns Shell_TrayWnd)
            return shared_ && shared_->TrayService().Running() ? 1 : 0;
        case 98:  // notify icons registered with it (every app's, hidden ones included)
            return shared_ && shared_->TrayService().Running() ? static_cast<LRESULT>(shared_->TrayService().Icons().size()) : 0;
        case 99:   // icons drawn in tray slot lp's grid (-1: not a tray slot)
        case 101: {  // tray slot lp: 1 with the overflow button (-1: not a tray slot)
            if (lp < 0 || lp >= static_cast<LPARAM>(items_.size()) || !elements_[items_[static_cast<size_t>(lp)].element]) {
                return -1;
            }
            const DockElement& e = ElementOf(static_cast<size_t>(lp));
            if (e.TrayCells() < 0) return -1;
            return wp == 99 ? e.TrayCells() : (e.TrayOverflow() ? 1 : 0);
        }
        case 100:  // the tray drawer is open on this dock
            return folder_.Kind() == 3 ? 1 : 0;
        case 102:  // the cell of a tray slot under the pointer (-1: none)
            return hoverCell_;
        case 103:  // Shell_NotifyIconGetRect queries answered so far
            return shared_ ? static_cast<LRESULT>(shared_->TrayService().RectQueries()) : 0;
        case 104:  // times our Shell_TrayWnd was raised above Explorer's again
            return shared_ ? static_cast<LRESULT>(shared_->TrayService().Raises()) : 0;
        case 110:  // the `order` keys of tray slot lp's item (-1: not a tray slot)
            if (lp < 0 || lp >= static_cast<LPARAM>(items_.size()) || items_[static_cast<size_t>(lp)].kind != DockItem::Kind::Tray ||
                !elements_[items_[static_cast<size_t>(lp)].element]) {
                return -1;
            }
            return static_cast<LRESULT>(ElementOf(static_cast<size_t>(lp)).Entry().order.size());
        case 111:  // hidden keys of tray slot lp's item (-1: not a tray slot)
            if (lp < 0 || lp >= static_cast<LPARAM>(items_.size()) || items_[static_cast<size_t>(lp)].kind != DockItem::Kind::Tray ||
                !elements_[items_[static_cast<size_t>(lp)].element]) {
                return -1;
            }
            return static_cast<LRESULT>(ElementOf(static_cast<size_t>(lp)).Entry().hidden.size());
        case 112:  // trayApp slot lp: 1 its icon is registered, 0 the placeholder (not running), -1 not a trayApp slot
            if (lp < 0 || lp >= static_cast<LPARAM>(items_.size()) || items_[static_cast<size_t>(lp)].kind != DockItem::Kind::TrayApp) {
                return -1;
            }
            return items_[static_cast<size_t>(lp)].missing ? 0 : 1;
        // --- notifications (round 4 phase 9) ---
        case 113:  // the notification service: 0 off, 1 polling, 2 denied (or not available)
            return shared_ ? static_cast<LRESULT>(shared_->Notifications().GetState()) : 0;
        case 114:  // unread notifications (every app)
            return shared_ ? static_cast<LRESULT>(shared_->Notifications().Unread()) : 0;
        case 115:  // the badge count of slot lp (0 none; -1 no such slot)
            if (lp < 0 || lp >= static_cast<LPARAM>(items_.size())) return -1;
            return items_[static_cast<size_t>(lp)].badgeCount;
        case 116:  // ms since the last poll (-1: none / off)
            return shared_ ? static_cast<LRESULT>(shared_->Notifications().LastPollAgeMs()) : -1;
        case 117:  // polls done since the service started
            return shared_ ? static_cast<LRESULT>(shared_->Notifications().Polls()) : 0;
        case 118:  // the interval the worker waits now (ms; 0 off)
            return shared_ ? static_cast<LRESULT>(shared_->Notifications().IntervalMs()) : 0;
        case 119:  // notifications listed (read ones and balloons included)
            return shared_ ? static_cast<LRESULT>(shared_->Notifications().Items().size()) : 0;
        case 120:  // balloons whose NIF_INFO was stripped from the copy mirrored to Explorer
            return shared_ ? static_cast<LRESULT>(shared_->TrayService().BalloonsStripped()) : 0;
        case 122:  // CPU time of the notification worker thread so far (µs; 0 off)
            return shared_ ? static_cast<LRESULT>(shared_->Notifications().WorkerCpuUs()) : 0;
        case 123:  // 1 while the notification centre is open
            return centre_.Visible() ? 1 : 0;
        case 124:  // its rows (-1: not open)
            return centre_.Visible() ? static_cast<LRESULT>(centre_.Rows()) : -1;
        case 125:  // what it shows: 0 rows, 1 "No notifications", 2 access denied (-1: not open)
            return centre_.Visible() ? centre_.Content() : -1;
        case 126:  // its selected row (-1 none / not open)
            return centre_.Visible() ? centre_.Selected() : -1;
        case 127:  // the notification id of its row lp (0: none)
            return centre_.Visible() && lp >= 0 ? static_cast<LRESULT>(centre_.IdAt(static_cast<size_t>(lp))) : 0;
        case 121:  // width in px of slot lp's drawn badge surface (rest size; 0 none, -1 no such slot)
            if (lp < 0 || lp >= static_cast<LPARAM>(items_.size())) return -1;
            return static_cast<LRESULT>(items_[static_cast<size_t>(lp)].badge.width);
        case 109:  // TaskbarCreated broadcasts repeated after Explorer raised its tray above ours
            return shared_ ? static_cast<LRESULT>(shared_->TrayService().Rebroadcasts()) : 0;
        case 105:  // balloons (NIF_INFO) captured so far
            return shared_ ? static_cast<LRESULT>(shared_->TrayService().BalloonCount()) : 0;
        case 106: {  // the owner HWND of the drawer's icon lp of the first tray slot (0: none)
            for (size_t i = 0; i < items_.size(); ++i) {
                if (items_[i].kind != DockItem::Kind::Tray) continue;
                const auto icons = TrayDrawerIcons(static_cast<int>(i));
                return lp >= 0 && lp < static_cast<LPARAM>(icons.size()) ? reinterpret_cast<LRESULT>(icons[static_cast<size_t>(lp)]->owner) : 0;
            }
            return 0;
        }
        case 107: {  // the uID of the drawer's icon lp of the first tray slot (-1: none)
            for (size_t i = 0; i < items_.size(); ++i) {
                if (items_[i].kind != DockItem::Kind::Tray) continue;
                const auto icons = TrayDrawerIcons(static_cast<int>(i));
                return lp >= 0 && lp < static_cast<LPARAM>(icons.size()) ? static_cast<LRESULT>(icons[static_cast<size_t>(lp)]->uid) : -1;
            }
            return -1;
        }
        case 108:  // icons in the drawer of the first tray slot (-1: no tray slot)
            for (size_t i = 0; i < items_.size(); ++i) {
                if (items_[i].kind == DockItem::Kind::Tray) return static_cast<LRESULT>(TrayDrawerIcons(static_cast<int>(i)).size());
            }
            return -1;
        case 87:  // the folder popup's layout: 0 grid, 1 list (-1 hidden)
            return folder_.ViewMode();
        case 88:  // its rows that have a visual (surface) now
            return static_cast<LRESULT>(folder_.LiveRows());
        case 89:  // its scroll offset (px)
            return static_cast<LRESULT>(folder_.ScrollOffset());
        case 90:  // its selected entry (-1 none)
            return folder_.Selected();
        case 91:  // characters in the path of the folder it shows (0 hidden)
            return static_cast<LRESULT>(folder_.PathLength());
        case 92:  // its columns (grid; 1 in a list; 0 hidden)
            return static_cast<LRESULT>(folder_.Columns());
        case 93:  // what the folder popup shows: 0 hidden, 1 a folder, 2 a group
            return folder_.Kind();
        case 94:  // a dragged icon would group with this slot on release (-1 none)
            return groupTarget_;
        case 95:  // members of group slot lp (-1: not a group)
            if (lp < 0 || lp >= static_cast<LPARAM>(items_.size())) return -1;
            return items_[static_cast<size_t>(lp)].kind == DockItem::Kind::Group
                       ? static_cast<LRESULT>(items_[static_cast<size_t>(lp)].members.size())
                       : -1;
        case 96:  // the slot a dragged icon is held over (the group target to be; -1 none)
            return groupCandidate_;
        case 26:
            return dropActive_ ? static_cast<LRESULT>(dropMode_) : 0;
        // 27 - 30: the weather of config item lp (a weather item), else (lp 0) the first one.
        case 27: {
            const WeatherService* weather = WeatherOfItem(lp);
            return weather ? static_cast<LRESULT>(weather->State()) : 0;
        }
        case 28: {
            const WeatherService* weather = WeatherOfItem(lp);
            if (!weather) return INT_MIN;
            const WeatherService::Forecast& data = weather->Data();
            if (!weather->Active() || !data.valid || !std::isfinite(data.temp)) return INT_MIN;
            return static_cast<LRESULT>(std::lround(data.temp * 10.0));
        }
        case 29: {
            if (!weatherPopup_.Visible()) return 0;
            const int slot = SlotOfKey(weatherKey_);
            if (lp == 0 || slot < 0) return 1;
            return items_[static_cast<size_t>(slot)].element == static_cast<size_t>(lp) ? 1 : 0;
        }
        case 30: {
            const WeatherService* weather = WeatherOfItem(lp);
            return weather && weather->Active() ? static_cast<LRESULT>(weather->Age()) : -1;
        }
        case 226:  // Flash: steps left (0: not flashing; round 5 phase B)
            return flashSteps_;
        case 228:  // kept shown by its own settings window being active (SetKeepShown; round 5 phase B)
            return keepShown_ ? 1 : 0;
        case 219: {  // the zone offset of clock item lp (minutes ahead of the local time; INT_MIN: not a clock)
            for (size_t i = 0; i < items_.size(); ++i) {
                const size_t e = items_[i].element;
                if (e != static_cast<size_t>(lp) || e >= elements_.size() || !elements_[e]) continue;
                int minutes = 0;
                return elements_[e]->ClockOffset(minutes) ? static_cast<LRESULT>(minutes) : INT_MIN;
            }
            return INT_MIN;
        }
        case 31:
            return calendar_.Visible() ? 1 : 0;
        case 36:
            return DockCfg().locked ? 1 : 0;
        case 38:
            return static_cast<LRESULT>(layout_.edge);  // the edge laid out (0 bottom, 1 top, 2 left, 3 right)
        case 39:
            return static_cast<LRESULT>(DockCfg().align);  // 0 center, 1 start, 2 end
        case 40:
            return static_cast<LRESULT>(DockCfg().offset);  // DIPs
        case 41:
            return layout_.fullWidth ? 1 : 0;
        case 42:
            return DockCfg().reserveSpace ? 1 : 0;
        case 43:
            return appBar_ ? 1 : 0;
        case 44:
            return static_cast<LRESULT>(layout_.peek);
        case 45:
            return titleLayout_ && title_.width > 0 ? 1 : 0;
        case 46:
            return !dockDrag_ ? 0 : dockZone_ == DockCfg().edge ? 1 : 2;
        case 255: {  // round 6 phase 6C: slot lp's widget has its text editor open (1 / 0; -1 not a script widget); lp 0xFFFF: the slot whose editor is open (-1 none)
            if ((lp & 0xFFFF) == 0xFFFF) {
                const WidgetInstance* open = WidgetByKey(editorWidget_);
                return open ? static_cast<LRESULT>(SlotOfWidget(*open)) : -1;
            }
            const WidgetInstance* w = WidgetAt(static_cast<int>(lp));
            return w ? (w->EditorOpen() ? 1 : 0) : -1;
        }
        case 256: {  // the HWND of slot lp's text editor (0: none; lp 0xFFFF: the open one's)
            const WidgetInstance* w = (lp & 0xFFFF) == 0xFFFF ? WidgetByKey(editorWidget_) : WidgetAt(static_cast<int>(lp));
            return w ? reinterpret_cast<LRESULT>(w->EditorHwnd()) : 0;
        }
        case 54: {  // script widget state of slot lp (0 ok, 1 error, 2 suspended; -1 not a script widget)
            const WidgetInstance* w = WidgetAt(static_cast<int>(lp));
            return w ? static_cast<LRESULT>(w->GetState()) : -1;
        }
        case 55:
            return static_cast<LRESULT>(widgets_.size());  // this dock's script widgets
        case 56:
            return static_cast<LRESULT>(shared_->Widgets().Instances().size());  // every dock's
        case 57: {  // script heap bytes of slot lp's widget
            const WidgetInstance* w = WidgetAt(static_cast<int>(lp));
            return w ? static_cast<LRESULT>(w->HeapBytes()) : -1;
        }
        case 58: {  // draw() calls of slot lp's widget so far
            const WidgetInstance* w = WidgetAt(static_cast<int>(lp));
            return w ? static_cast<LRESULT>(w->DrawCount()) : -1;
        }
        case 59:
            return static_cast<LRESULT>(shared_->Widgets().Wakeups());  // widget timer wake-ups (process)
        case 60: {  // log lines slot lp's widget has written since it was created
            const WidgetInstance* w = WidgetAt(static_cast<int>(lp));
            return w ? static_cast<LRESULT>(w->LogTotal()) : -1;
        }
        case 67: {  // the audio provider: change notifications handled (-1: not running)
            const AudioService& a = shared_->Widgets().Audio();
            return a.Active() ? static_cast<LRESULT>(a.Notifications()) : -1;
        }
        case 68: {  // the media provider: snapshots received (-1: not running)
            const MediaService& m = shared_->Widgets().Media();
            return m.Active() ? static_cast<LRESULT>(m.Updates()) : -1;
        }
        case 69: {  // the media provider: 0 no session, 1 stopped, 2 paused, 3 playing (-1: not running)
            const MediaService& m = shared_->Widgets().Media();
            if (!m.Active()) return -1;
            return m.Get().session ? static_cast<LRESULT>(m.Get().status) : 0;
        }
        case 70:  // docks[].windows: 0 stacked, 1 unstacked (as the slots were last built)
            return builtUnstacked_ ? 1 : 0;
        case 71:  // docks[].titles: 0 off, 1 one line, 2 two lines
            return static_cast<LRESULT>(DockCfg().titles);
        case 72:  // lines of title text actually laid out under / beside the icons (0: none)
            return layout_.captionLines;
        case 73:  // the window of window slot lp (0: an app-level slot; -1 no such slot)
            if (lp < 0 || lp >= static_cast<LPARAM>(items_.size())) return -1;
            return reinterpret_cast<LRESULT>(items_[static_cast<size_t>(lp)].window);
        case 74:  // characters in the text the hover label shows (-1: no label)
            return labelShown_ && label_.width ? static_cast<LRESULT>(labelChars_) : -1;
        case 75:  // characters in popup card lp's title (-1: no such card)
            return preview_.CardTitleLength(static_cast<size_t>(lp));
        case 76:  // live title changes applied (slot titles, label, preview cards)
            return static_cast<LRESULT>(titleUpdates_);
        case 78: {  // slot lp's dot: 0 none, 1 running, 2 active, 3 attention (-1 no such slot)
            if (lp < 0 || lp >= static_cast<LPARAM>(items_.size())) return -1;
            const DockItem& item = items_[static_cast<size_t>(lp)];
            if (item.kind == DockItem::Kind::Group) return !item.groupRunning ? 0 : item.wantsAttention ? 3 : item.active ? 2 : 1;
            if (item.IsWidget() || item.windows.empty() || (item.window && item.minimized)) return 0;
            return item.wantsAttention ? 3 : item.active ? 2 : 1;
        }
        case 79:  // docks[].thumbnails: 0 off, 1 live, 2 snapshots
            return static_cast<LRESULT>(DockCfg().thumbnails);
        case 80: {  // slot lp: 0 not a thumbnail tile, 1 a tile without a frame yet, 2 a tile with a frame (-1 no slot)
            if (lp < 0 || lp >= static_cast<LPARAM>(items_.size())) return -1;
            const DockItem& item = items_[static_cast<size_t>(lp)];
            if (!item.thumbWindow) return 0;
            return thumbs_.FrameOf(item.thumbWindow) ? 2 : 1;
        }
        case 81:  // live capture frames received so far (this dock)
            return static_cast<LRESULT>(thumbs_.LiveFrames());
        case 82:  // PrintWindow calls so far (every dock)
            return static_cast<LRESULT>(Thumbnails::PrintCalls());
        case 83:  // live capture sessions running now
            return static_cast<LRESULT>(thumbs_.Sessions());
        case 84:  // snapshot rounds started so far (this dock)
            return static_cast<LRESULT>(thumbs_.Rounds());
        case 86:  // the HRESULT of the last live capture that could not start (0: none)
            return static_cast<LRESULT>(thumbs_.LastError());
        case 85:  // the window slot lp's tile shows (0: not a tile)
            if (lp < 0 || lp >= static_cast<LPARAM>(items_.size())) return -1;
            return reinterpret_cast<LRESULT>(items_[static_cast<size_t>(lp)].thumbWindow);
        case 77:  // slot lp's title under the icon: characters of its text (0: none; -1 no such slot)
            if (lp < 0 || lp >= static_cast<LPARAM>(items_.size())) return -1;
            return items_[static_cast<size_t>(lp)].caption.visual
                       ? static_cast<LRESULT>(items_[static_cast<size_t>(lp)].captionText.size())
                       : 0;
        case 65:  // a script widget's popup is visible
            return widgetPopup_.Visible() ? 1 : 0;
        case 66:  // the widget popup's size in px, MAKELONG(w, h) (0 hidden)
            return widgetPopup_.Visible() ? static_cast<LRESULT>(MAKELONG(static_cast<WORD>(std::lround(widgetPopup_.WidthDip() * layout_.scale)),
                                                                         static_cast<WORD>(std::lround(widgetPopup_.HeightDip() * layout_.scale))))
                                          : 0;
        case 218:  // a script widget's passive popup (popup.open(w, h, { passive: true })) is visible
            return widgetPassive_.Visible() ? 1 : 0;
        case 61:  // slot lp's box along the dock (px; icons: the icon) ...
        case 62:  // ... and across it
            if (lp < 0 || lp >= static_cast<LPARAM>(items_.size())) return -1;
            return static_cast<LRESULT>(wp == 61 ? BoxW(items_[static_cast<size_t>(lp)])
                                                 : BoxCross(items_[static_cast<size_t>(lp)]));
        case 52:
        case 235:  // round 6 phase 6A
            return static_cast<LRESULT>(DockCfg().layer);  // 0 normal, 1 desktop, 2 behind
        case 236:
            return DockCfg().Floating() ? 1 : 0;
        case 237: {  // the resting bar's left / top (screen px)
            const RECT r = RestBarRect();
            return static_cast<LRESULT>(MAKELONG(static_cast<WORD>(r.left), static_cast<WORD>(r.top)));
        }
        case 238: {  // ... its width / height
            const RECT r = RestBarRect();
            return static_cast<LRESULT>(MAKELONG(static_cast<WORD>(r.right - r.left), static_cast<WORD>(r.bottom - r.top)));
        }
        case 239:  // above the desktop window with no other app's window between (DesktopLayer::AboveDesktop)
            return DesktopLayer::AboveDesktop(hwnd_) ? 1 : 0;
        case 53:
            // A desktop-layer dock: 1 while shown (the desktop is the foreground), 0 hidden; -1 normal.
            return DesktopLayered() ? (IsWindowVisible(hwnd_) ? 1 : 0) : -1;
        case 37:
            if (lp < 0 || lp >= static_cast<LPARAM>(items_.size())) return -1;
            return static_cast<LRESULT>(items_[static_cast<size_t>(lp)].element);
        case 32: {
            LRESULT mask = 0;
            const bool glow = std::any_of(items_.begin(), items_.end(), [](const DockItem& i) { return i.glow.width > 0; });
            const bool reflection =
                std::any_of(items_.begin(), items_.end(), [](const DockItem& i) { return i.reflection.width > 0; });
            if (glow && GlowOf(DockTheme()) != GlowMode::Off) mask |= 1;
            if (reflection && (layout_.reflH > 0.0f || layout_.reflClip)) mask |= 2;
            if (perspectiveApplied_) mask |= 4;
            return mask;
        }
        case 24:
        case 25:
        case 33:
        case 34:
        case 35: {
            if (lp < 0 || lp >= static_cast<LPARAM>(items_.size())) return -1;
            const DockItem& item = items_[static_cast<size_t>(lp)];
            if (wp == 35) return static_cast<LRESULT>(icons_.DominantColor(item.key));
            if (wp == 33) {
                return item.glow.width && item.glowOpacity.valid ? static_cast<LRESULT>(std::lround(item.glowOpacity.to * 100.0f))
                                                                 : 0;
            }
            if (wp == 34) return static_cast<LRESULT>(item.reflection.height);
            return wp == 24 ? static_cast<LRESULT>(item.kind) : (item.customIcon ? 1 : 0);
        }
        // --- round 7 phase 7A ---
        case 270:  // docks[].popupGap in px (the gap between the dock and its popups)
            return PopupGapPx();
        case 271:  // docks[].maxLength in px as applied (0: none, or ignored for a ring / full width)
            return MaxLengthPx();
        // --- round 7 phase 7B: item anchors ---
        case 272:  // slot LOWORD(lp)'s group (0 start, 1 centre, 2 end; its config item's anchor, else the align's); -1 none
        case 274:  // ... its config item's offset in px
            if (lp < 0 || lp >= static_cast<LPARAM>(items_.size())) return -1;
            if (wp == 272) return DockCfg().GroupOf(items_[static_cast<size_t>(lp)].element);
            if (items_[static_cast<size_t>(lp)].element >= DockCfg().items.size()) return 0;
            return static_cast<LRESULT>(std::lround(static_cast<float>(DockCfg().items[items_[static_cast<size_t>(lp)].element].offset) *
                                                    layout_.scale));
        case 273:  // 1 while the dock lays its items out as anchored groups (anchors on a bar whose run fits)
            return layout_.anchored ? 1 : 0;
        case 281: {  // round 9 phase 9A: the bar's main-axis extent as laid out now, MAKELONG(start px, length px), stage coordinates
            const float start = bgLeftTarget_;
            const float length = bgRightTarget_ + layout_.bgSurfaceW - bgLeftTarget_;
            return static_cast<LRESULT>(MAKELONG(static_cast<WORD>(std::clamp(std::lround(start), 0L, 65535L)),
                                                 static_cast<WORD>(std::clamp(std::lround(length), 0L, 65535L))));
        }
        case 288:  // 9A follow-up: the slot at stage main coordinate LOWORD(lp) by the live hit test (clicks, menus, hover; -1 none)
            return HitAlong(static_cast<float>(LOWORD(lp)));
        // --- round 11 phase 11A: slot buttons, theme spacing ---
        case 293:  // the button state drawn for slot LOWORD(lp): 0 none / 1 running / 2 active / 3 attention, +16 hovered, +32 pressed, +64 stacked edge; -1 no such slot
            if (lp < 0 || lp >= static_cast<LPARAM>(items_.size())) return -1;
            return std::max(0, items_[static_cast<size_t>(lp)].buttonDrawn);
        case 294:  // the dock's effective theme iconGap in DIPs (rounded)
            return static_cast<LRESULT>(std::lround(DockTheme().iconGap));
        // --- overflow / scrolling (round 4 phase 11a) ---
        case 128:  // docks[].overflow (0 scroll, 1 shrink, 2 clip)
            return static_cast<LRESULT>(DockCfg().overflow);
        case 129:  // 1 while the run scrolls (it is longer than the viewport)
            return layout_.view.on ? 1 : 0;
        case 130:  // the scroll offset (px; where the run is going)
            return layout_.view.on ? static_cast<LRESULT>(std::lround(scroll_)) : 0;
        case 131:  // the run's length (px; also when it does not scroll)
            return static_cast<LRESULT>(std::lround(layout_.view.on ? layout_.view.content : layout_.itemsW));
        case 132:  // the viewport's length (px; 0 when not scrolling)
            return layout_.view.on ? static_cast<LRESULT>(std::lround(layout_.view.Length())) : 0;
        case 133:  // edge fades in effect now (bit 0 the start, bit 1 the end)
            return (layout_.view.on && layout_.view.fadeStart ? 1 : 0) | (layout_.view.on && layout_.view.fadeEnd ? 2 : 0);
        case 134: {  // slot lp in the viewport: 2 whole, 1 partly, 0 scrolled out (-1: no such slot)
            if (lp < 0 || lp >= static_cast<LPARAM>(items_.size())) return -1;
            const DockItem& it = items_[static_cast<size_t>(lp)];
            if (!layout_.view.on) return 2;
            const float a = it.slotX, b = it.slotX + it.slotW;
            if (a >= layout_.view.start - 0.5f && b <= layout_.view.end + 0.5f) return 2;
            return b > layout_.view.clipStart && a < layout_.view.clipEnd ? 1 : 0;
        }
        case 135:  // 1 while the scroll thumb is shown
            return hintShown_ ? 1 : 0;
        case 136:  // the icon size laid out (DIPs; smaller than docks[].iconSize with "shrink")
            return layout_.iconDip;
        case 137:  // slot placements in the last pass: driven (low word), only tracked as scrolled out (high word)
            return static_cast<LRESULT>(MAKELONG(static_cast<WORD>(placedDriven_), static_cast<WORD>(placedSkipped_)));
        case 138:  // 1 while a drag near a viewport end auto-scrolls the run
            return autoScrollPending_ ? 1 : 0;
        case 139:  // 1 while the bar is dragged to scroll (2: pressed, not moved yet)
            return scrollDrag_ ? 1 : scrollPress_ ? 2 : 0;
        case 140:  // the scroll offset the slots show now (px; follows 130 over the glide)
            return layout_.view.on ? static_cast<LRESULT>(std::lround(scrollShown_.Value(Renderer::Now()))) : 0;
        case 142:    // the main coordinate (px, stage) of slot lp's box as placed now (magnified / dragged targets)
        case 143: {  // ... and its target scale x 1000 (-1: no such slot)
            if (lp < 0 || lp >= static_cast<LPARAM>(items_.size())) return -1;
            const DockItem& it = items_[static_cast<size_t>(lp)];
            return wp == 142 ? std::lround(it.targetX) : std::lround(it.targetScale * 1000.0f);
        }
        // --- rings (round 4 phase 11b) ---
        case 145:  // docks[].shape as laid out (0 bar, 1 arc, 2 circle)
            return layout_.polar.on ? static_cast<LRESULT>(layout_.shape) : 0;
        case 146:  // the ring's radius (px, the icons' centre line; 0: a bar)
            return layout_.polar.on ? std::lround(layout_.polar.R) : 0;
        case 147:    // slot lp's angle at rest (degrees x 10, 0 - 3599, 0 = right, counter-clockwise; -1: none / a bar)
        case 149: {  // ... where it is targeted now (magnified / dragged)
            if (!layout_.polar.on || lp < 0 || lp >= static_cast<LPARAM>(items_.size())) return -1;
            const DockItem& it = items_[static_cast<size_t>(lp)];
            const float u = (wp == 147 ? it.x : it.targetX) + BoxW(it) / 2.0f;
            float deg = layout_.polar.Angle(u) * 180.0f / kPi;
            deg = std::fmod(deg, 360.0f);
            if (deg < 0.0f) deg += 360.0f;
            return std::lround(deg * 10.0f) % 3600;
        }
        case 148:  // the ring's centre on screen, MAKELONG(x, y) (-1: a bar)
            if (!layout_.polar.on) return -1;
            return static_cast<LRESULT>(MAKELONG(static_cast<WORD>(layout_.window.left + std::lround(layout_.polar.cx)),
                                                 static_cast<WORD>(layout_.window.top + std::lround(layout_.polar.cy))));
        case 150:  // the ring's inner / outer radius (px): inner in the low word, outer in the high word
            return layout_.polar.on ? static_cast<LRESULT>(MAKELONG(static_cast<WORD>(std::lround(layout_.ringIn)),
                                                                    static_cast<WORD>(std::lround(layout_.ringOut))))
                                    : 0;
        case 151:  // slot lp's turn (rotateIcons; degrees x 10, clockwise; 0 upright)
            if (lp < 0 || lp >= static_cast<LPARAM>(items_.size())) return -1;
            return std::lround(items_[static_cast<size_t>(lp)].turn * 10.0f);
        case 152:  // 1 while the window region is the ring's band
            return regionSet_ ? 1 : 0;
        case 153:  // the hover label's target place on screen, MAKELONG(x, y) of its top-left (-1: not shown)
        case 154:  // ... its size, MAKELONG(width, height)
            if (!labelShown_ || !label_.width) return -1;
            if (wp == 154) return static_cast<LRESULT>(MAKELONG(static_cast<WORD>(label_.width), static_cast<WORD>(label_.height)));
            return static_cast<LRESULT>(MAKELONG(static_cast<WORD>(layout_.window.left + std::lround(labelXTarget_)),
                                                 static_cast<WORD>(layout_.window.top + std::lround(labelYTarget_))));
        // --- calendars (round 4 phase 13) ---
        case 155:  // the calendar service: 0 off (no enabled feed), 1 running, 2 a feed's last attempt failed
            return shared_ ? shared_->Calendar().State() : 0;
        case 156:  // enabled feeds (lp 1: every configured feed)
            if (!shared_) return 0;
            return static_cast<LRESULT>(lp == 1 ? shared_->Calendar().Feeds().size() : shared_->Calendar().EnabledFeeds());
        case 157:  // occurrences in the expansion window
            return shared_ ? static_cast<LRESULT>(shared_->Calendar().Items().size()) : 0;
        case 158:  // events today (every enabled feed)
            return static_cast<LRESULT>(CalendarEventsToday());
        case 159:  // seconds since the newest good fetch / read of this session (-1 none)
            return shared_ ? static_cast<LRESULT>(shared_->Calendar().LastFetchAge()) : -1;
        case 160:  // the open calendar popup: events listed for its selected day (-1: not open / no events shown)
            return calendar_.Visible() && calendar_.SelectedDay() != INT64_MIN ? static_cast<LRESULT>(calendar_.Rows()) : -1;
        case 161: {  // ... its selected day as yyyymmdd (-1)
            const int64_t day = calendar_.Visible() ? calendar_.SelectedDay() : INT64_MIN;
            if (day == INT64_MIN) return -1;
            int y = 0, m = 0, d = 0;
            Ics::CivilFromDays(day, y, m, d);
            return y * 10000 + m * 100 + d;
        }
        case 162:  // ... dots drawn in its grid at its last paint (-1 not open)
            return calendar_.Visible() ? calendar_.DotsDrawn() : -1;
        case 163:  // web feed responses received; 164: of them 304 Not Modified; 165: file feed reads
            return shared_ ? static_cast<LRESULT>(shared_->Calendar().Fetches()) : 0;
        case 164:
            return shared_ ? static_cast<LRESULT>(shared_->Calendar().NotModified()) : 0;
        case 165:
            return shared_ ? static_cast<LRESULT>(shared_->Calendar().FileReads()) : 0;
        case 166:  // the calendar service's pending timers (the refresh timer, the file debounce)
            return shared_ ? shared_->Calendar().PendingTimers() : 0;
        case 167:  // seconds until its refresh timer fires (-1 none)
            return shared_ ? static_cast<LRESULT>(shared_->Calendar().NextDueIn()) : -1;
        case 168:  // feed lp's status (0 off, 1 loading, 2 ok, 3 error; -1 no such feed)
        case 169:  // ... its consecutive failures
        case 170: {  // ... its occurrences in the window
            if (!shared_ || lp < 0 || static_cast<size_t>(lp) >= shared_->Calendar().Feeds().size()) return -1;
            const CalendarService::Feed& f = shared_->Calendar().Feeds()[static_cast<size_t>(lp)];
            return wp == 168 ? static_cast<LRESULT>(f.status) : wp == 169 ? f.failures : static_cast<LRESULT>(f.occurrences);
        }
        case 171:  // 1 while the file watcher thread runs
            return shared_ && shared_->Calendar().Watching() ? 1 : 0;
        case 172:  // the expansion window's first / (173) last local day as yyyymmdd (-1: off)
        case 173: {
            if (!shared_ || !shared_->Calendar().Active()) return -1;
            int y = 0, m = 0, d = 0;
            Ics::CivilFromDays(wp == 172 ? shared_->Calendar().WindowFrom() : shared_->Calendar().WindowTo(), y, m, d);
            return y * 10000 + m * 100 + d;
        }
        case 174: {  // the calendar popup's window size, MAKELONG(width, height) (-1 not open)
            RECT r{};
            if (!calendar_.Visible() || !GetWindowRect(calendar_.Hwnd(), &r)) return -1;
            return static_cast<LRESULT>(MAKELONG(static_cast<WORD>(r.right - r.left), static_cast<WORD>(r.bottom - r.top)));
        }
        case 175:  // events on the day lp days from today (negative lp: before)
            return shared_ && shared_->Calendar().Active()
                       ? static_cast<LRESULT>(shared_->Calendar().CountOn(CalendarService::Today() + static_cast<int64_t>(lp)))
                       : 0;
        case 176:  // the calendar popup's hovered event row (-1 none / not open)
            return calendar_.Visible() ? calendar_.HotRow() : -1;
        case 177:  // ... the row its detail strip describes (hovered, else selected with Up / Down; -1 none)
            return calendar_.Visible() ? calendar_.DetailShown() : -1;
        // --- reminders (13b) ---
        case 178:  // reminders pending (timed events ahead on feeds with reminders, not fired yet; snoozed ones)
            return shared_ ? static_cast<LRESULT>(shared_->Calendar().PendingReminders()) : 0;
        case 179:  // 1 while this dock's reminder banner is shown
            return reminder_.Visible() ? 1 : 0;
        case 180:  // seconds until the reminder timer fires (-1 none)
            return shared_ ? static_cast<LRESULT>(shared_->Calendar().NextReminderIn()) : -1;
        case 181:  // reminders fired this session
            return shared_ ? static_cast<LRESULT>(shared_->Calendar().RemindersFired()) : 0;
        case 182:  // reminders waiting for this dock's banner (the shown one included)
            return static_cast<LRESULT>(reminders_.size());
        case 144:  // the stage's screen coordinate along the main axis (stage main 0; QueryState 142, ItemCommand 834)
            return layout_.vertical ? layout_.window.top : layout_.window.left;
        case 141: {  // the edge-fade opacity x 100 of slot lp at rest (100: none; -1 no such slot)
            if (lp < 0 || lp >= static_cast<LPARAM>(items_.size())) return -1;
            const DockItem& it = items_[static_cast<size_t>(lp)];
            return std::lround(layout_.view.Opacity(it.targetX + BoxW(it) / 2.0f) * 100.0f);
        }
        default:
            return -1;
    }
}

LRESULT CALLBACK DockWindow::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    DockWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<DockWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<DockWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    LRESULT result = self->WndProc(msg, wp, lp);
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        self->hwnd_ = nullptr;
    }
    return result;
}

LRESULT DockWindow::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    if (hook_) {
        LRESULT result = 0;
        if (hook_(hwnd_, msg, wp, lp, result)) return result;
    }
    if (msgQueryItem_ && msg == msgQueryItem_) return QueryItemPoint(wp, lp);
    if (msgQueryState_ && msg == msgQueryState_) return QueryState(wp, lp);

    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;  // clicks never focus the dock

        case WM_MOUSEMOVE: {
            leaveRearmed_ = false;
            ArmMouseTracking();
            const POINT pt = ToStage(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            if (dockDrag_) {
                UpdateDockDrag();
                return 0;
            }
            if (scrollDrag_) {
                UpdateScrollDrag(pt);
                return 0;
            }
            if (scrollPress_ && (wp & MK_LBUTTON) && dragPhase_ == DragPhase::None &&
                std::fabs(layout_.Canon(pt).x - scrollPressMain_) >
                    static_cast<float>(GetSystemMetricsForDpi(layout_.vertical ? SM_CYDRAG : SM_CXDRAG, dpi_))) {
                // A press on the background of a scrolling bar, moved along it: the run follows.
                BeginScrollDrag();
                UpdateScrollDrag(pt);
                return 0;
            }
            if (dockPress_ && (wp & MK_LBUTTON) && dragPhase_ == DragPhase::None) {
                POINT screen{};
                GetCursorPos(&screen);
                if (std::abs(screen.x - dockPressScreen_.x) > GetSystemMetricsForDpi(SM_CXDRAG, dpi_) ||
                    std::abs(screen.y - dockPressScreen_.y) > GetSystemMetricsForDpi(SM_CYDRAG, dpi_)) {
                    BeginDockDrag();
                    return 0;
                }
            }
            if (dragPhase_ == DragPhase::Dragging) {
                UpdateDrag(pt);
                return 0;
            }
            if (dragPhase_ == DragPhase::Removing) return 0;
            // Round 12 phase 12D: the long press into customize mode wants a still pointer (4 DIPs).
            if (editPressPending_ && (std::abs(pt.x - editPressPt_.x) > MulDiv(4, static_cast<int>(dpi_), 96) ||
                                      std::abs(pt.y - editPressPt_.y) > MulDiv(4, static_cast<int>(dpi_), 96))) {
                CancelEditPress();
            }
            if (!pressTrayKey_.empty() && (wp & MK_LBUTTON) && hideState_ != HideState::Hidden && !DockCfg().locked &&
                (std::abs(pt.x - pressPt_.x) > GetSystemMetricsForDpi(SM_CXDRAG, dpi_) ||
                 std::abs(pt.y - pressPt_.y) > GetSystemMetricsForDpi(SM_CYDRAG, dpi_))) {
                // A tray icon pressed and moved: the icon's own drag (pin it, reorder the grid).
                const std::wstring key = std::move(pressTrayKey_);
                pressTrayKey_.clear();
                pressed_ = -1;
                HideLabel();
                DragTrayIcon(key);
                return 0;
            }
            if (pressed_ >= 0 && !scrollPress_ && Draggable(static_cast<size_t>(pressed_)) && (wp & MK_LBUTTON) &&
                hideState_ != HideState::Hidden &&
                (std::abs(pt.x - pressPt_.x) > GetSystemMetricsForDpi(SM_CXDRAG, dpi_) ||
                 std::abs(pt.y - pressPt_.y) > GetSystemMetricsForDpi(SM_CYDRAG, dpi_))) {
                BeginDrag(pt);
                if (dragPhase_ == DragPhase::Dragging) return 0;
            }
            if (hideState_ == HideState::Hidden) {
                // On the reveal strip.
                if (revealArmed_ && DockCfg().AutoHideOn() && !revealPending_) {
                    SetTimer(hwnd_, kRevealTimerId,
                             std::max<UINT>(USER_TIMER_MINIMUM, static_cast<UINT>(DockCfg().revealDelayMs)), nullptr);
                    revealPending_ = true;
                }
                return 0;
            }
            if (editing_) UpdateEditHover(pt);  // round 12 phase 12D: the remove badge follows the pointer
            if (!InZone(pt)) {
                LeaveZone();
                return 0;
            }
            CancelHide();
            SetTitleHover(true);  // round 12 phase 12C: a hover title fades in (once per entry)
            if (!expanded_) {
                // Entering the bar: give magnified icons and the label their room first. The
                // cursor is on the bar, so the larger transparent area is harmless meanwhile.
                expanded_ = true;
                UpdateWindowRect();
            }
            if (hideState_ == HideState::Hiding) StartShow();  // came back mid-slide: reverse
            magnifying_ = true;
            cursorX_ = layout_.Canon(pt).x;
            cursorV_ = layout_.Canon(pt).y;  // round 11 phase 11A: the hot tracking's cross coordinate
            // The targets for this pointer position first: the hit test reads them (what is drawn
            // under the pointer once they are applied; ScheduleMagnify applies them).
            ComputeTargets();
            {
                int hit = HitTest(pt);  // a widget is not hovered: no label, no preview
                // 9A follow-up 2: a "steady" bar grows the first time the pointer is over something
                // that zooms (a magnifying slot here; a widget growing on hover: RefreshDirtyWidgets)
                // and keeps that length until the pointer leaves (LeaveZone), whatever it is over
                // meanwhile. Grown, the run is anchored at the pointer: test again.
                if (GrowForZoom(hit)) hit = HitTest(pt);
                const bool widget = hit >= 0 && items_[static_cast<size_t>(hit)].IsWidget();
                // ... unless it has cells (the tray grid): the label shows the cell's label.
                std::wstring cellLabel;
                float cellCentre = 0.0f;
                POINT screen{};
                GetCursorPos(&screen);
                const int cell =
                    widget ? ElementOf(static_cast<size_t>(hit)).CellAt(hit, screen, &cellLabel, &cellCentre) : -1;
                if (cell >= 0) {
                    SetHoverCell(hit, cell, std::move(cellLabel), cellCentre);
                } else {
                    SetHoverCell(-1, -1, {}, 0.0f);
                    SetHover(widget ? -1 : hit);
                }
                UpdateWidgetHover(widget ? hit : -1);  // script widgets: onHover
            }
            ScheduleMagnify();
            return 0;
        }

        case WM_MOUSELEAVE:
            trackingMouse_ = false;
            // A drag owns the mouse (an icon, the dock, the run of a scrolling dock).
            if (dragPhase_ != DragPhase::None || dockDrag_ || scrollDrag_ || scrollPress_) return 0;
            if (hideState_ == HideState::Hidden) {
                if (revealPending_) {  // left the strip before the reveal delay: stay hidden
                    KillTimer(hwnd_, kRevealTimerId);
                    revealPending_ = false;
                }
                revealArmed_ = true;
                return 0;
            }
            // 9A follow-up 2: a grown bar with the pointer still on the dock window, in its zone (a
            // leave handled after a menu closed): not a leave; tracking again (a pointer that is
            // gone after all posts the leave at once). Leaving here would keep the bar at rest
            // over a widget, which does not grow it again. Once per mouse move (no leave loop).
            if (grown_ && !hold_ && !leaveRearmed_) {
                POINT cursor{};
                if (GetCursorPos(&cursor) && GetAncestor(WindowFromPoint(cursor), GA_ROOT) == hwnd_ && CursorInZone()) {
                    leaveRearmed_ = true;
                    ArmMouseTracking();
                    return 0;
                }
            }
            LeaveZone();
            return 0;

        case WM_LBUTTONDOWN:
            SuppressPreview();  // the click cycles / starts a drag: no popup over it
            CancelEditPress();
            // Round 12 phase 12D: customize mode's own targets first (a badge, a "+", beside the bar).
            if (editing_ && dragPhase_ == DragPhase::None && !(wp & MK_CONTROL) &&
                EditPress(ToStage(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}))) {
                pressed_ = -1;
                return 0;
            }
            pressPt_ = ToStage(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            pressed_ = dragPhase_ == DragPhase::None ? HitTest(pressPt_) : -1;
            pressTrayKey_.clear();
            if (pressed_ >= 0 && !(wp & MK_CONTROL) && elements_[items_[static_cast<size_t>(pressed_)].element]) {
                POINT screen{};
                GetCursorPos(&screen);
                pressTrayKey_ = ElementOf(static_cast<size_t>(pressed_)).TrayKeyAt(pressed_, screen);
            }
            // Ctrl + press on the bar of an unlocked dock: moving on drags the whole dock. A floating
            // dock (round 6 phase 6A) also moves by a plain press on its bar's background (not on an
            // item or a widget; beside an icon's box across the bar counts as background).
            {
                bool floatingBackground = false;
                if (DockCfg().Floating() && !(wp & MK_CONTROL) && pressTrayKey_.empty()) {
                    const float v = layout_.Canon(pressPt_).y;
                    floatingBackground = pressed_ < 0 || (!items_[static_cast<size_t>(pressed_)].IsWidget() &&
                                                          (v < layout_.itemTop || v >= layout_.itemTop + layout_.iconPx));
                }
                // Round 12 phase 12E: customize mode: a plain press on the background of a bar on an
                // edge (not full width, not a ring) moves the dock too: along the edge (offset), off
                // it (edgeGap), or onto another edge (as Ctrl+drag).
                bool editBackground = false;
                if (editing_ && !(wp & MK_CONTROL) && pressTrayKey_.empty() && !DockCfg().Floating() && !DockCfg().Polar() &&
                    !DockCfg().fullWidth) {
                    const float v = layout_.Canon(pressPt_).y;
                    editBackground = pressed_ < 0 || (!items_[static_cast<size_t>(pressed_)].IsWidget() &&
                                                      (v < layout_.itemTop || v >= layout_.itemTop + layout_.iconPx));
                }
                dockPress_ = ((wp & MK_CONTROL) || floatingBackground || editBackground) && !DockCfg().locked &&
                             dragPhase_ == DragPhase::None && hideState_ == HideState::Shown && InZone(pressPt_) &&
                             GetCursorPos(&dockPressScreen_);
                editMove_ = dockPress_ && editBackground;
                if (editMove_) pressed_ = -1;  // the background's press: no item click / drag on release
            }
            // The capture from the press on: a quick first move may already leave the dock.
            if (dockPress_) SetCapture(hwnd_);
            // A press on the background of a scrolling bar (or on a slot that cannot be dragged: a
            // locked dock) may start a drag of the run.
            // The bar's surface beside an icon box (above / below it on the cross axis) counts as
            // background too: a full bar has hardly any room between its icons.
            {
                const float v = layout_.Canon(pressPt_).y;
                const bool offBox = pressed_ >= 0 && !items_[static_cast<size_t>(pressed_)].IsWidget() &&
                                    (v < layout_.itemTop || v >= layout_.itemTop + layout_.iconPx);
                scrollPress_ = !dockPress_ && layout_.view.on && dragPhase_ == DragPhase::None &&
                               hideState_ == HideState::Shown && InZone(pressPt_) && pressTrayKey_.empty() &&
                               (pressed_ < 0 || offBox || !Draggable(static_cast<size_t>(pressed_)));
            }
            if (scrollPress_) {
                scrollPressMain_ = layout_.Canon(pressPt_).x;
                scrollPressOffset_ = scroll_;
                SetCapture(hwnd_);
            }
            // A press on the dock closes the click popup; on the slot whose popup it is (a folder,
            // the clock, the weather) the release then does not open it again (the click toggles).
            // Round 12 phase 12D: customize mode: an item's press and click do nothing of their own.
            pressClosedPopup_ = !editing_ && pressed_ >= 0 && ElementOf(static_cast<size_t>(pressed_)).OnPress(pressed_);
            if (ClickPopupOnScreen()) DismissClickPopups(true, false);
            RefreshButtons();  // round 11 phase 11A: the pressed frame (no-op without slot buttons)
            // Round 12 phase 12D: a press on the bar's background (or beside an icon's box across the
            // bar) held still for 600 ms enters customize mode (an item's press is hold-to-group's).
            if (!editing_ && !(wp & MK_CONTROL) && dragPhase_ == DragPhase::None && hideState_ == HideState::Shown &&
                InZone(pressPt_) && pressTrayKey_.empty()) {
                const float v = layout_.Canon(pressPt_).y;
                const bool offBox = pressed_ >= 0 && !items_[static_cast<size_t>(pressed_)].IsWidget() &&
                                    (v < layout_.itemTop || v >= layout_.itemTop + layout_.iconPx);
                if (pressed_ < 0 || offBox) ArmEditPress(pressPt_);
            }
            return 0;

        case WM_LBUTTONUP: {
            CancelEditPress();  // round 12 phase 12D: released before the long press: an ordinary click
            if (dockPress_) {
                dockPress_ = false;  // a Ctrl + click without a move: an ordinary click
                if (GetCapture() == hwnd_) ReleaseCapture();
            }
            if (dockDrag_) {
                EndDockDrag(true);
                return 0;
            }
            if (scrollDrag_) {
                EndScrollDrag(true);  // flings on; no click
                return 0;
            }
            if (scrollPress_) EndScrollDrag(false);  // not moved: an ordinary click
            if (dragPhase_ == DragPhase::Dragging) {
                EndDrag(true);
                return 0;
            }
            pressTrayKey_.clear();
            const POINT upPt = ToStage(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            if (editing_ && EditRelease(upPt)) {  // round 12 phase 12D: a badge's / a "+" slot's click
                pressed_ = -1;
                return 0;
            }
            const int index = HitTest(upPt);
            const bool clicked = index >= 0 && index == pressed_ && !editing_;  // 12D: no launch while customizing
            pressed_ = -1;
            const bool pressClosed = pressClosedPopup_;
            pressClosedPopup_ = false;
            if (clicked) ElementOf(static_cast<size_t>(index)).OnClick(index, pressClosed);
            RefreshButtons();  // round 11 phase 11A: the frame is no longer pressed
            return 0;
        }

        case WM_MBUTTONDOWN:
            SuppressPreview();
            if (ClickPopupOnScreen()) DismissClickPopups(true, false);
            pressedMiddle_ = HitTest(ToStage(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}));
            return 0;

        case WM_MBUTTONUP: {
            const int index = HitTest(ToStage(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}));
            const bool clicked = index >= 0 && index == pressedMiddle_ && !editing_;  // round 12 phase 12D
            pressedMiddle_ = -1;
            if (clicked) ElementOf(static_cast<size_t>(index)).OnMiddleClick(index);  // apps: a new instance
            return 0;
        }

        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL: {
            // Routed to the element under the pointer (apps: the dock's wheel action). A scrolling
            // run: Shift + wheel always scrolls it; otherwise a wheel no element takes scrolls it
            // when the dock's wheel action is "none" (a horizontal wheel: always).
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};  // screen coordinates
            ScreenToClient(hwnd_, &pt);
            const POINT stagePt = ToStage(pt);
            const int index = HitTest(stagePt);
            const int delta = GET_WHEEL_DELTA_WPARAM(wp);
            const bool horizontal = msg == WM_MOUSEHWHEEL;
            // + = toward the run's end: wheel down (negative), tilt right (positive).
            const int toEnd = horizontal ? delta : -delta;
            if (layout_.view.on && (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) && InZone(stagePt)) {
                ScrollByWheel(toEnd);
                return 0;
            }
            if (index >= 0 && ElementOf(static_cast<size_t>(index)).OnWheel(index, delta, horizontal)) {
                return 0;
            }
            if (layout_.view.on && (horizontal || DockCfg().wheel == WheelAction::None) && InZone(stagePt)) {
                ScrollByWheel(toEnd);
                return 0;
            }
            break;
        }

        case kRecycleBinChangedMessage: {
            // New delivery: the notification's data must be locked and released.
            PIDLIST_ABSOLUTE* pidls = nullptr;
            LONG event = 0;
            if (HANDLE lock = SHChangeNotification_Lock(reinterpret_cast<HANDLE>(wp), static_cast<DWORD>(lp), &pidls,
                                                        &event)) {
                SHChangeNotification_Unlock(lock);
            }
            if (binNotify_ && !binPending_) {
                SetTimer(hwnd_, kRecycleBinTimerId, kRecycleBinCoalesceMs, nullptr);
                binPending_ = true;
            }
            return 0;
        }

        case kDropActionMessage:
            OnDropAction();
            return 0;

        case kAppBarMessage:
            OnAppBarNotify(wp, lp);
            return 0;

        case kGrownCheckMessage:
            grownCheckPosted_ = false;
            if (grown_ && !hold_ && !popupHolds_ && hideState_ != HideState::Hidden && !CursorInZone()) LeaveZone();
            return 0;

        case kWidgetRedrawMessage:
            widgetRedrawPosted_ = false;
            DispatchEvent(DockEvent::WidgetRedraw);
            return 0;

        case kWidgetPopupMessage:
            OnWidgetPopupRequests();
            return 0;

        case kCentreRequestMessage: {
            // notifications.openCentre() of a widget: toggles the centre over the widget's slot.
            const std::wstring key = std::move(centreRequest_);
            centreRequest_.clear();
            WidgetInstance* w = WidgetByKey(key);
            const int slot = w ? SlotOfWidget(*w) : -1;
            if (slot < 0) return 0;
            if (centre_.OnScreen() && centreOpener_ == key) {
                DismissCentre(true);
                return 0;
            }
            ShowCentre(SlotPopupAnchor(slot));
            if (centre_.OnScreen()) centreOpener_ = key;
            return 0;
        }

        case kThumbRedrawMessage:
            thumbRedrawPosted_ = false;
            RenderWithRecovery([this] { return DrawFrame(false); });  // the dirty tiles
            return 0;

        case Thumbnails::kSnapshotMessage:  // owns the job: frees it
        case Thumbnails::kLiveMessage:      // start / stop the live sessions
            thumbs_.HandleMessage(msg, wp, lp);
            return 0;

        case WM_ACTIVATE:
            if (appBar_) {
                APPBARDATA abd{sizeof(abd)};
                abd.hWnd = hwnd_;
                SHAppBarMessage(ABM_ACTIVATE, &abd);
            }
            break;

        case WM_CAPTURECHANGED:
            // Someone else took the mouse (a menu, Alt+Tab...): cancel the drag.
            if (dragPhase_ == DragPhase::Dragging && reinterpret_cast<HWND>(lp) != hwnd_) EndDrag(false);
            if (dockDrag_ && reinterpret_cast<HWND>(lp) != hwnd_) EndDockDrag(false);
            if ((scrollDrag_ || scrollPress_) && reinterpret_cast<HWND>(lp) != hwnd_) EndScrollDrag(false);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            // Content comes from DirectComposition; there is no redirection bitmap to paint.
            ValidateRect(hwnd_, nullptr);
            return 0;

        case WM_TIMER:
            switch (wp) {
                case kClockTimerId:
                    KillTimer(hwnd_, kClockTimerId);  // one-shot; the clock reschedules it (next boundary)
                    DispatchEvent(DockEvent::ClockTick);
                    return 0;
                case kBounceTimerId:
                    KillTimer(hwnd_, kBounceTimerId);  // one-shot; rescheduled while bounces run
                    OnBounceTimer();
                    return 0;
                case kIconEvictTimerId:
                    KillTimer(hwnd_, kIconEvictTimerId);
                    evictPending_ = false;
                    if (icons_.EvictExpired()) {
                        SetTimer(hwnd_, kIconEvictTimerId, static_cast<UINT>(IconCache::kEvictAfterMs), nullptr);
                        evictPending_ = true;
                    }
                    return 0;
                case kHideTimerId:
                    OnHideTimer();
                    return 0;
                case kRevealTimerId:
                    OnRevealTimer();
                    return 0;
                case kSlideTimerId:
                    OnSlideTimer();
                    return 0;
                case kMagnifyTimerId:
                    KillTimer(hwnd_, kMagnifyTimerId);
                    magnifyPending_ = false;
                    if (magnifying_ || dragPhase_ == DragPhase::Dragging) ApplyMagnify();
                    return 0;
                case kDragTimerId:
                    if (dockDrag_) {
                        if (GetAsyncKeyState(VK_ESCAPE) & 0x8001) EndDockDrag(false);
                    } else if (dragPhase_ != DragPhase::Dragging) {
                        KillTimer(hwnd_, kDragTimerId);
                        dragPending_ = false;
                    } else if (GetAsyncKeyState(VK_ESCAPE) & 0x8001) {
                        EndDrag(false);
                    }
                    return 0;
                case kRemoveTimerId:
                    OnRemoveTimer();
                    return 0;
                case kGroupHoldTimerId:
                    OnGroupHoldTimer();
                    return 0;
                case kLabelTimerId:
                    OnLabelTimer();
                    return 0;
                case kLabelFadeTimerId:
                    OnLabelFadeTimer();
                    return 0;
                case kPreviewTimerId:
                    OnPreviewTimer();
                    return 0;
                case kPreviewLeaveTimerId:
                    OnPreviewLeaveTimer();
                    return 0;
                case kRecycleBinTimerId:
                    KillTimer(hwnd_, kRecycleBinTimerId);  // one-shot; re-armed by the next notification
                    binPending_ = false;
                    OnRecycleBinChanged();
                    return 0;
                case kBackdropTimerId:
                    KillTimer(hwnd_, kBackdropTimerId);  // one-shot; re-armed while the bar animates
                    backdropPending_ = false;
                    SyncBackdrop();
                    return 0;
                case Thumbnails::kTimerId:
                    thumbs_.HandleMessage(msg, wp, lp);  // the snapshot interval
                    return 0;
                case kScrollHintTimerId:
                    OnScrollHintTimer();
                    return 0;
                case kAutoScrollTimerId:
                    OnAutoScrollTimer();
                    return 0;
                case kFlashTimerId:
                    OnFlashTimer();
                    return 0;
                case kEditPressTimerId:  // round 12 phase 12D
                    OnEditPressTimer();
                    return 0;
                case kRestTimerId:
                    KillTimer(hwnd_, kRestTimerId);
                    restPending_ = false;
                    if (!magnifying_) {
                        RenderWithRecovery([this] {
                            SetBigIcons(false);
                            return S_OK;
                        });
                        TryCompact();
                    }
                    return 0;
                default:
                    break;
            }
            break;

        case WM_TIMECHANGE:
            DispatchEvent(DockEvent::TimeChanged);
            return 0;

        case WM_POWERBROADCAST:
            if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) DispatchEvent(DockEvent::TimeChanged);
            return TRUE;

        case WM_DPICHANGED:
            // New scale: Relayout re-renders every surface at the new pixel size (the icon cache
            // re-fetches bitmaps whose pixel size changed). The suggested rect is ignored: we
            // always place ourselves.
            dpi_ = HIWORD(wp);
            labelDirty_ = true;
            DismissPreview(false, false);
            DismissClickPopups(false, false);
            DismissWidgetPassive(false);
            Relayout();
            return 0;

        case WM_DISPLAYCHANGE:
            DismissPreview(false, false);
            DismissClickPopups(false, false);
            DismissWidgetPassive(false);
            Relayout();  // re-picks the monitor (falls back to the primary) and re-centres
            return 0;

        case WM_SETTINGCHANGE:
            if (wp == SPI_SETHIGHCONTRAST ||
                (lp && CompareStringOrdinal(reinterpret_cast<const wchar_t*>(lp), -1, L"ImmersiveColorSet", -1,
                                            TRUE) == CSTR_EQUAL)) {
                // "Transparency effects" or high contrast toggled: acrylic on/off.
                UpdateAcrylic();
                SyncLayer();  // a "behind" dock's backdrop beneath it (or none)
                Relayout();
            } else if (wp == SPI_SETWORKAREA || wp == SPI_SETNONCLIENTMETRICS) {
                Relayout();
            } else if (wp == SPI_SETCLIENTAREAANIMATION) {
                // Animation effects toggled: reduceMotion "system" follows it from the next animation.
                RefreshMotion();
            }
            return 0;

        case WM_WINDOWPOSCHANGING:
            // A "behind" dock (registered with the desktop layer): z-order changes the layer did not
            // make are dropped (no-op for any other dock).
            if (layerRegistered_) DesktopLayer::FilterPosChanging(hwnd_, reinterpret_cast<WINDOWPOS*>(lp));
            break;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:  // (what the active window gets while no window has the focus)
            // Dashboard mode gave this dock the keyboard (DockManager::SetDashboard): Escape ends it.
            if (wp == VK_ESCAPE && shared_ && shared_->DashboardOn()) {
                shared_->EndDashboard();
                return 0;
            }
            // Round 12 phase 12D: customize mode gave the dock the keyboard (App, on entering): Escape
            // leaves it (a drag's Escape is the drag's: the poll timer cancels it).
            if (wp == VK_ESCAPE && editing_ && dragPhase_ == DragPhase::None && editRequests_.leave) {
                editRequests_.leave();
                return 0;
            }
            break;

        case WM_WINDOWPOSCHANGED:
            // Keep the blur directly beneath the dock whenever the dock's z-order changes (layout
            // passes re-assert topmost; another topmost window must not end up between the two).
            if (!(reinterpret_cast<const WINDOWPOS*>(lp)->flags & SWP_NOZORDER)) backdrop_.Restack(hwnd_);
            if (appBar_) {
                APPBARDATA abd{sizeof(abd)};
                abd.hWnd = hwnd_;
                SHAppBarMessage(ABM_WINDOWPOSCHANGED, &abd);
            }
            break;

        case WM_DESTROY:
            // Round 12 phase 12D: customize mode ends with its window (a disabled dock, a display
            // gone): App forgets it and closes the edit bar and the drawer.
            if (editing_) {
                editing_ = false;
                if (editRequests_.ended) editRequests_.ended();
            }
            editPressPending_ = false;
            RemoveAppBar();  // never leave a registration behind
            appBarAllowed_ = false;
            if (layerRegistered_) {
                DesktopLayer::Unregister(hwnd_);
                layerRegistered_ = false;
            }
            SetWatchingForeground(false);
            overlapped_ = false;
            if (dropTarget_) {
                RevokeDragDrop(hwnd_);
                dropTarget_->Detach();
                dropTarget_->Release();
                dropTarget_ = nullptr;
            }
            dragPhase_ = DragPhase::None;
            dropActive_ = false;
            dockDrag_ = dockPress_ = false;
            editMove_ = false;  // round 12 phase 12E
            HideReadout();
            if (GetCapture() == hwnd_) ReleaseCapture();
            for (UINT_PTR id : {kClockTimerId, kIconEvictTimerId, kBounceTimerId, kHideTimerId, kRevealTimerId,
                                kSlideTimerId, kMagnifyTimerId, kLabelTimerId, kRestTimerId, kBackdropTimerId,
                                kDragTimerId, kRemoveTimerId, kPreviewTimerId, kPreviewLeaveTimerId,
                                kLabelFadeTimerId, kRecycleBinTimerId}) {
                KillTimer(hwnd_, id);
            }
            if (binNotify_) {
                SHChangeNotifyDeregister(binNotify_);
                binNotify_ = 0;
            }
            binPending_ = false;
            dragPending_ = removePending_ = false;
            evictPending_ = hidePending_ = revealPending_ = magnifyPending_ = labelPending_ = restPending_ = false;
            labelFading_ = false;
            backdropPending_ = false;
            previewPending_ = previewLeavePending_ = false;
            popupHolds_ = 0;
            preview_.Destroy();
            folder_.Destroy();
            folderKey_ = AppKey{};
            weatherPopup_.Destroy();
            calendar_.Destroy();
            widgetPopup_.Destroy();
            widgetPassive_.Destroy();
            if (centre_.OnScreen() && shared_) shared_->SetCentreOpen(false);
            centre_.Destroy();
            centreOpener_.clear();
            popupWidget_.clear();
            passiveWidget_.clear();
            editorWidget_.clear();
            previewOpener_.clear();
            popupRequests_.clear();
            acrylic_ = false;
            backdrop_.Destroy();
            thumbs_.Reset();  // capture sessions and frames (on this dock's device), the snapshot timer
            labelFormat_ = nullptr;
            labelEllipsis_ = nullptr;
            ResetLayers();
            items_.clear();
            elements_.clear();  // their text layouts (the clock's, the weather's)
            widgets_.clear();   // the script widgets: runtimes, canvases, their D2D resources
            hoverWidget_ = AppKey{};
            icons_.Shutdown();
            renderer_.Shutdown();
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
