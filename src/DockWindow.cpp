#include "DockWindow.h"

#include <dwmapi.h>
#include <ole2.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cwctype>
#include <vector>

#include "Activation.h"
#include "AppIdentity.h"
#include "Launcher.h"
#include "Theme.h"

namespace {

constexpr DWORD kExStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP;
constexpr DWORD kStyle = WS_POPUP;

// Layout constants in DIPs (scaled by DPI).
constexpr float kBarExtraHeight = 16.0f;    // bar height = iconSize + this
constexpr float kWindowExtraHeight = 56.0f; // window height = iconSize * magnification + this (label room)
constexpr float kBottomMargin = 8.0f;       // gap between the bar and the screen edge
constexpr float kPadding = 12.0f;           // inner horizontal padding of the bar
constexpr float kGap = 8.0f;                // between icons
constexpr float kIconTop = 5.0f;            // bar top -> icon top
constexpr float kDotFromBottom = 5.0f;      // bar bottom -> indicator dot centre
constexpr float kDotRadius = 2.0f;          // 4 px dot at 100 %
constexpr float kSeparatorWidth = 17.0f;    // space between the last icon slot and the clock
constexpr float kMinSideSlack = 24.0f;      // window space beside the bar (labels of edge icons)
constexpr float kLabelGap = 6.0f;           // magnified icon top -> label bottom
constexpr float kLabelFontSize = 13.0f;
constexpr float kLabelMaxWidth = 320.0f;
constexpr float kBounceHeight = 0.35f;      // launch bounce height, fraction of the icon size
constexpr float kPulseOpacity = 0.45f;      // launchStyle "pulse": lowest opacity of a pulse
constexpr ULONGLONG kBounceTimeoutMs = 5000; // stop bouncing if no window appeared by then

// Interaction timing (the animation durations are DockWindow::k*DurS, scaled by motion_).
constexpr double kMagnifyIntervalS = 0.016;  // at most ~60 magnification updates per second
constexpr UINT kLabelDelayMs = 250;
// Dragging icons.
constexpr float kDragScale = 1.1f;           // the dragged icon, relative to rest size
constexpr float kDragOpacity = 0.85f;
constexpr float kRemoveOpacity = 0.4f;       // dragged far above the bar: "release to unpin"
constexpr float kRemoveZoneBars = 1.5f;      // that zone starts this many bar heights above the bar
constexpr UINT kDragPollMs = 50;             // Escape polling while dragging (no keyboard focus)
constexpr float kMarkerWidth = 2.0f;         // file-drop insertion marker, DIPs
constexpr float kPreviewGap = 12.0f;         // stage top -> thumbnail popup bottom, DIPs
constexpr UINT kPreviewLeaveMs = 300;        // grace period after leaving the item and the popup
constexpr float kPi = 3.14159265358979f;

BOOL CALLBACK CollectMonitor(HMONITOR mon, HDC, LPRECT, LPARAM lp) {
    reinterpret_cast<std::vector<HMONITOR>*>(lp)->push_back(mon);
    return TRUE;
}

void Log(const wchar_t* what, HRESULT hr) {
    wchar_t buf[160];
    swprintf_s(buf, L"WinDock: %s failed (0x%08X)\n", what, static_cast<unsigned>(hr));
    OutputDebugStringW(buf);
}

UINT TimerMs(double seconds) {
    return std::max<UINT>(USER_TIMER_MINIMUM, static_cast<UINT>(std::ceil(seconds * 1000.0)));
}

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

HRESULT DockWindow::Create(HINSTANCE instance, const Config* config, MessageHook hook) {
    config_ = config;
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

    HRESULT hr = renderer_.Initialize(hwnd_, config_->renderer == L"software");
    if (FAILED(hr)) {
        Log(L"Renderer::Initialize", hr);
        return hr;
    }
    hr = CreateLayers();
    if (FAILED(hr)) {
        Log(L"CreateLayers", hr);
        return hr;
    }
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
    ApplyConfig();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    // Pins first; the initial enumeration (synchronous) then builds and lays out the items.
    ResolvePins();
    tracker_.Start(hwnd_, [this](const WindowTracker::Snapshot& snap) { Rebuild(snap); });
    ApplyAutoHide();  // hides after hideDelayMs unless the mouse is on the dock
    return S_OK;
}

void DockWindow::Destroy() {
    if (hwnd_) DestroyWindow(hwnd_);
}

void DockWindow::ApplyConfig() {
    RefreshMotion();
    DismissPreview(false, false);  // previews / theme / sizes may have changed
    FinishLabelFade();             // its text / position may no longer match
    clock_.Configure(config_->clock.format, config_->clock.showDate);
    clock_.SetColors(config_->theme.clockText, config_->theme.clockDateText);
    // The drop marker is only redrawn when its size changes: force it (the accent may differ).
    if (marker_.visual) renderer_.DrawLayer(marker_, 0, 0, {});
    for (DockItem& item : items_) {
        item.dirty = true;
        item.dotDirty = true;
    }
    labelDirty_ = true;
    labelFormat_ = nullptr;  // theme / size may have changed
    labelEllipsis_ = nullptr;
    UpdateAcrylic();
    Relayout();
    if (config_->clock.show) {
        clock_.ScheduleNextTick(hwnd_, kClockTimerId);
    } else {
        Clock::Stop(hwnd_, kClockTimerId);
    }
    ApplyAutoHide();
}

HMONITOR DockWindow::PickMonitor() const {
    // Index 0 = primary; 1..n = the other monitors in enumeration order. An index that no
    // longer exists (monitor unplugged) falls back to the primary monitor.
    std::vector<HMONITOR> monitors;
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&monitors));
    HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    std::erase(monitors, primary);
    monitors.insert(monitors.begin(), primary);
    const size_t index = config_ ? static_cast<size_t>(std::max(config_->monitor, 0)) : 0;
    return index < monitors.size() ? monitors[index] : primary;
}

void DockWindow::ComputeLayout() {
    const Config& c = *config_;
    Layout l;
    l.scale = static_cast<float>(dpi_) / USER_DEFAULT_SCREEN_DPI;
    const float s = l.scale;

    l.iconPx = std::round(static_cast<float>(c.iconSize) * s);
    l.padding = std::round(kPadding * s);
    // With acrylic, DWM rounds the blur with its own radius; the tint matches it.
    l.cornerRadius = std::round((acrylic_ ? acrylicRadiusDip_ : c.theme.cornerRadius) * s);
    l.magnification = static_cast<float>(c.magnification);

    const float barH = std::round(l.iconPx + kBarExtraHeight * s);
    const float margin = std::round(kBottomMargin * s);
    float winH = std::round(l.iconPx * l.magnification + kWindowExtraHeight * s);
    winH = std::max(winH, barH + margin);

    // Clock text block (already laid out at this scale).
    const float clockW = c.clock.show ? static_cast<float>(clockSize_.width) : 0.0f;
    const float clockH = c.clock.show ? static_cast<float>(clockSize_.height) : 0.0f;

    // Bar: lead + N slots (iconPx + gap each, icon centred) + separator + clock + padding.
    const size_t n = items_.size();
    l.gap = std::round(kGap * s);
    const float half = std::floor(l.gap / 2.0f);
    const float stride = l.iconPx + l.gap;
    const float lead = n ? l.padding - half : l.padding;
    const float itemsW = static_cast<float>(n) * stride;
    const float sepW = (n && c.clock.show) ? std::round(kSeparatorWidth * s) : 0.0f;
    float barW = lead + itemsW + sepW + clockW + l.padding;
    if (n && !c.clock.show) barW = lead + itemsW + lead;
    barW = std::round(std::max(barW, l.iconPx + 2.0f * l.padding));

    // Magnified icons widen the run by sum((scale - 1) * iconPx). Both falloffs (cosine, linear) sum
    // to at most magnifyRange over all items (and each term is at most 1), which bounds the growth.
    l.growMax = 0.0f;
    if (n && l.magnification > 1.001f) {
        const float terms = static_cast<float>(std::min<size_t>(n, static_cast<size_t>(c.magnifyRange)));
        l.growMax = std::ceil((l.magnification - 1.0f) * l.iconPx * terms) + 2.0f;
    }
    // A file dragged over the dock opens a one-slot gap: the bar grows by half a slot per side.
    if (n) l.growMax = std::max(l.growMax, std::ceil(stride / 2.0f) + 2.0f);
    // The bar may grow by up to growMax on either side (it grows away from the cursor).
    const float slack = std::max(l.growMax, std::round(kMinSideSlack * s));
    const float winW = barW + 2.0f * slack;

    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(PickMonitor(), &mi);
    l.monitor = mi.rcMonitor;  // the taskbar is hidden, so use the full monitor
    const RECT& m = l.monitor;
    const LONG left = m.left + ((m.right - m.left) - static_cast<LONG>(winW)) / 2;
    l.window = RECT{left, m.bottom - static_cast<LONG>(winH), left + static_cast<LONG>(winW), m.bottom};

    l.bar = D2D1::RectF(slack, winH - margin - barH, slack + barW, winH - margin);
    const float clockX = l.bar.right - l.padding - clockW;
    const float clockY = std::round(l.bar.top + (barH - clockH) / 2.0f);
    l.clock = D2D1::RectF(clockX, clockY, clockX + clockW, clockY + clockH);

    l.itemsLeft = l.bar.left + lead;
    l.itemTop = l.bar.top + std::round(kIconTop * s);
    l.itemHeight = l.bar.bottom - l.itemTop;
    l.magTop = l.itemTop + l.iconPx - l.iconPx * l.magnification;
    l.dotRadius = std::max(1.5f, kDotRadius * s);
    l.dotHeight = static_cast<UINT>(std::ceil(2.0f * l.dotRadius + 2.0f));
    const float dotCenter = l.bar.bottom - std::round(kDotFromBottom * s);
    l.dotTop = std::round(dotCenter - static_cast<float>(l.dotHeight) / 2.0f);
    l.dotCenterY = dotCenter - l.dotTop;
    l.bitmapPx = static_cast<UINT>(std::lround(static_cast<double>(c.iconSize) * c.magnification * s));
    l.separatorX = sepW > 0.0f ? std::round(lead + itemsW + sepW / 2.0f) : -1.0f;

    // Background halves: the seam must sit where both halves show plain fill (between the
    // rounded left end and the separator / rounded right end) for every amount of growth.
    l.bgSurfaceW = barW + l.growMax;
    const float radius = std::ceil(std::min(l.cornerRadius, barH / 2.0f));
    // A spring settle overshoots: returning from growth g, the bar briefly gets narrower than at
    // rest by up to kSpringOvershoot * g per side. Keep the seam that much further in.
    const float undershoot =
        motion_.easing == Easing::Spring ? std::ceil(static_cast<float>(Tween::kSpringOvershoot) * l.growMax) : 0.0f;
    const float seamMin = radius + 2.0f + undershoot;
    const float seamMax = (l.separatorX >= 0.0f ? l.separatorX - 2.0f : barW - radius - 2.0f) - undershoot;
    l.seamX = l.bar.left + (seamMin <= seamMax ? seamMin : std::floor(barW / 2.0f));

    l.slide = winH - l.bar.top + 2.0f;  // moves the bar just below the window's bottom edge

    layout_ = l;
}

RECT DockWindow::DesiredRect() const {
    const Layout& l = layout_;
    if (hideState_ == HideState::Hidden) {
        // The reveal trigger: a 1 px strip along the monitor's bottom edge. Nothing is drawn in
        // it (the root visual is transparent while hidden and there is no redirection bitmap).
        const RECT& m = l.monitor;
        return RECT{m.left, m.bottom - 1, m.right, m.bottom};
    }
    if (expanded_) return l.window;
    // Only the bar is interactive at rest: the transparent room above and beside it must not
    // swallow clicks meant for the windows below. While sliding, the bar travels down through
    // the bottom margin, so the rect extends to the screen edge (the stage's bottom).
    const LONG left = l.window.left + static_cast<LONG>(l.bar.left);
    const LONG right = l.window.left + static_cast<LONG>(l.bar.right);
    LONG top = l.window.top + static_cast<LONG>(l.bar.top);
    if (hideState_ != HideState::Shown && motion_.easing == Easing::Spring &&
        (motion_.hide == HideStyle::Slide || motion_.hide == HideStyle::SlideOnly)) {
        // A spring slide-in overshoots above the resting bar: room for it.
        top -= static_cast<LONG>(std::ceil(static_cast<float>(Tween::kSpringOvershoot) * l.slide)) + 1;
        top = std::max(top, l.window.top);
    }
    const LONG bottom =
        hideState_ == HideState::Shown ? l.window.top + static_cast<LONG>(l.bar.bottom) : l.window.bottom;
    return RECT{left, top, right, bottom};
}

void DockWindow::ApplyStageTransform() {
    // Stage point p is client point p - (winRect_ origin - stage origin). A static transform on
    // the root; the auto-hide slide (root OffsetY) adds on top of it.
    if (IDCompositionVisual2* root = renderer_.Root()) {
        root->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(layout_.window.left - winRect_.left),
                                                         static_cast<float>(layout_.window.top - winRect_.top)));
    }
}

void DockWindow::ApplyWindowPos() {
    const RECT r = DesiredRect();
    winRect_ = r;
    ApplyStageTransform();
    SetWindowPos(hwnd_, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
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
    if (magnifying_ || hovered_ >= 0 || labelShown_ || labelPending_ || labelFading_ || restPending_ || hold_) {
        return true;
    }
    if (dragPhase_ != DragPhase::None || dropActive_) return true;
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
}

void DockWindow::RelayoutItems() {
    if (!hwnd_ || inLayout_) return;
    LayoutPass();
    RenderWithRecovery([this] { return DrawFrame(false); });
}

void DockWindow::LayoutPass() {
    inLayout_ = true;
    const Layout old = layout_;

    for (int pass = 0; pass < 2; ++pass) {
        const UINT before = dpi_;
        const float scale = static_cast<float>(dpi_) / USER_DEFAULT_SCREEN_DPI;
        if (clock_.UpdateText()) clockDirty_ = true;
        clockSize_ = config_->clock.show ? clock_.BuildLayout(renderer_.DWrite(), scale) : D2D1_SIZE_U{};
        ComputeLayout();
        ApplyWindowPos();
        // Moving to another monitor may change our DPI (WM_DPICHANGED arrives re-entrantly
        // during SetWindowPos); lay out once more if it did.
        const UINT now = GetDpiForWindow(hwnd_);
        dpi_ = now;
        if (now == before) break;
        clockDirty_ = true;
        labelDirty_ = true;
        for (DockItem& item : items_) {
            item.dirty = true;
            item.dotDirty = true;
        }
    }
    // Stage coordinates changed: place everything without animation. With the same geometry
    // (e.g. pins reordered) the running placement animations stay valid and are retargeted.
    const Layout& l = layout_;
    if (!EqualRect(&old.window, &l.window) || old.scale != l.scale || old.bar.left != l.bar.left ||
        old.bar.top != l.bar.top || old.bar.right != l.bar.right || old.bar.bottom != l.bar.bottom ||
        old.itemsLeft != l.itemsLeft || old.iconPx != l.iconPx || old.gap != l.gap || old.growMax != l.growMax) {
        snapPending_ = true;
        watch_.Poke();  // the bar's column moved: re-test the overlap (no-op unless intelligent)
    }

    inLayout_ = false;
}

void DockWindow::ResetLayers() {
    bgLeftClip_.Reset();
    bgLeft_.Reset();
    bgRightClip_.Reset();
    bgRight_.Reset();
    clockLayer_.Reset();
    itemsGroup_.Reset();
    for (Layer& d : dotSources_) d.Reset();
    marker_.Reset();
    markerX_.valid = false;
    label3_ = nullptr;
    labelOpacity_.valid = false;
    label_.Reset();
    for (DockItem& item : items_) {
        item.layer.Reset();
        item.icon.Reset();
        item.iconBig.Reset();
        item.dot.Reset();
    }
}

HRESULT DockWindow::CreateLayers() {
    // Z-order (back to front): background halves, clock, items, label.
    ResetLayers();
    HRESULT hr = renderer_.CreateLayer(bgLeftClip_);
    if (SUCCEEDED(hr)) hr = renderer_.CreateLayer(bgLeft_, &bgLeftClip_);
    if (SUCCEEDED(hr)) hr = renderer_.CreateLayer(bgRightClip_);
    if (SUCCEEDED(hr)) hr = renderer_.CreateLayer(bgRight_, &bgRightClip_);
    if (SUCCEEDED(hr)) hr = renderer_.CreateLayer(clockLayer_);
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
    clockDirty_ = true;
    snapPending_ = true;
    rootY_.valid = false;
    rootOpacity_.valid = false;
    return hr;
}

HRESULT DockWindow::CreateItemVisuals(DockItem& item) {
    item.layer.Reset();
    item.icon.Reset();
    item.iconBig.Reset();
    item.dot.Reset();
    HRESULT hr = renderer_.CreateLayer(item.dot, &itemsGroup_);
    if (SUCCEEDED(hr)) hr = renderer_.CreateLayer(item.layer, &itemsGroup_);
    if (SUCCEEDED(hr)) hr = renderer_.EnsurePlacement(item.layer, layout_.iconPx / 2.0f, layout_.iconPx);
    if (SUCCEEDED(hr)) hr = renderer_.CreateLayer(item.icon, &item.layer);
    if (SUCCEEDED(hr)) hr = renderer_.CreateLayer(item.iconBig, &item.layer);
    item.dirty = true;
    item.dotDirty = true;
    item.tx.valid = item.ts.valid = item.fade.valid = false;
    return hr;
}

HRESULT DockWindow::RenderBackground() {
    const Layout& l = layout_;
    const UINT w = static_cast<UINT>(l.bgSurfaceW);
    const UINT h = static_cast<UINT>(l.bar.bottom - l.bar.top);
    const ThemeConfig& theme = config_->theme;
    const D2D1_COLOR_F fill = Theme::ToColor(acrylic_ ? theme.acrylicBackground : theme.background);
    const float radius = std::min(l.cornerRadius, static_cast<float>(std::min(w, h)) / 2.0f);
    const float stroke = std::max(1.0f, std::round(l.scale));
    // The separator keeps its distance from the right end (it moves with the right half).
    const float separatorX = l.separatorX >= 0.0f ? l.separatorX + l.growMax : -1.0f;

    HRESULT hr = renderer_.DrawLayer(bgLeft_, w, h, [&](ID2D1DeviceContext* dc) {
        winrt::com_ptr<ID2D1SolidColorBrush> brush;
        if (FAILED(dc->CreateSolidColorBrush(fill, brush.put()))) return;
        const D2D1_ROUNDED_RECT rr{D2D1::RectF(0, 0, static_cast<float>(w), static_cast<float>(h)), radius, radius};
        dc->FillRoundedRectangle(rr, brush.get());
        // Hairline highlight so the bar reads against dark wallpapers.
        const float inset = stroke / 2.0f;
        const D2D1_ROUNDED_RECT edge{
            D2D1::RectF(inset, inset, static_cast<float>(w) - inset, static_cast<float>(h) - inset),
            std::max(0.0f, radius - inset), std::max(0.0f, radius - inset)};
        brush->SetColor(Theme::ToColor(theme.barBorder));
        dc->DrawRoundedRectangle(edge, brush.get(), stroke);
        // Separator between the app icons and the clock.
        if (separatorX >= 0.0f) {
            const float x = separatorX + stroke / 2.0f;
            const float vInset = std::round(static_cast<float>(h) * 0.22f);
            brush->SetColor(Theme::ToColor(theme.separator));
            dc->DrawLine(D2D1::Point2F(x, vInset), D2D1::Point2F(x, static_cast<float>(h) - vInset), brush.get(),
                         stroke);
        }
    });
    if (FAILED(hr)) return hr;
    bgLeftX_.valid = false;  // re-apply clips / offsets (the seam may have moved)
    return renderer_.ShareContent(bgRight_, bgLeft_);
}

HRESULT DockWindow::RenderClock() {
    if (!config_->clock.show) {
        clockDirty_ = false;
        return renderer_.DrawLayer(clockLayer_, 0, 0, {});
    }
    HRESULT hr = renderer_.DrawLayer(clockLayer_, clockSize_.width, clockSize_.height,
                                     [&](ID2D1DeviceContext* dc) { clock_.Draw(dc, 0.0f, 0.0f); });
    if (FAILED(hr)) return hr;
    clockDirty_ = false;
    return S_OK;
}

HRESULT DockWindow::RenderItem(DockItem& item) {
    const Layout& l = layout_;
    const float icon = l.iconPx;
    const UINT smallPx = static_cast<UINT>(icon);
    const UINT bigPx = l.bitmapPx > smallPx ? l.bitmapPx : 0;
    HWND sample = item.windows.empty() ? nullptr : item.windows.front();
    // May be slow (shell icon extraction) the first time per app; cached afterwards.
    // A pinned file that no longer exists shows the placeholder (unless the app is running).
    ID2D1Bitmap* bmp = item.missing && item.windows.empty()
                           ? nullptr
                           : icons_.Get(item.key, sample, l.bitmapPx, renderer_.ResourceContext(), item.iconFile,
                                        item.iconIndex);

    auto drawIcon = [&](ID2D1DeviceContext* dc, float size) {
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
        if (FAILED(dc->CreateSolidColorBrush(Theme::ToColor(config_->theme.placeholderFill), brush.put()))) return;
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
            brush->SetColor(Theme::ToColor(config_->theme.placeholderText));
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
    if (FAILED(hr)) return hr;
    item.dirty = false;
    // The magnified-size surface is drawn at bitmapPx and shown at iconPx * scale.
    if (bigPx) renderer_.SetStaticTransform(item.iconBig, D2D1::Matrix3x2F::Scale(icon / bigPx, icon / bigPx));
    const bool big = bigIcons_ && bigPx;
    renderer_.SetVisible(item.icon, !big);
    renderer_.SetVisible(item.iconBig, big);
    return S_OK;
}

HRESULT DockWindow::RenderDotSources() {
    const Layout& l = layout_;
    const float icon = l.iconPx;
    const D2D1_COLOR_F colors[3] = {
        Theme::ToColor(config_->theme.dotRunning),
        Theme::ToColor(config_->theme.dotActive),     // owns the foreground window
        Theme::ToColor(config_->theme.dotAttention),  // wants attention
    };
    for (int i = 0; i < 3; ++i) {
        HRESULT hr = renderer_.DrawLayer(dotSources_[i], static_cast<UINT>(icon), l.dotHeight,
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
    const Layer& source = item.windows.empty() ? none
                          : item.wantsAttention ? dotSources_[2]
                          : item.active         ? dotSources_[1]
                                                : dotSources_[0];
    const HRESULT hr = renderer_.ShareContent(item.dot, source);
    if (SUCCEEDED(hr)) item.dotDirty = false;
    return hr;
}

HRESULT DockWindow::RenderLabel() {
    if (hovered_ < 0 || hovered_ >= static_cast<int>(items_.size()) || !renderer_.DWrite()) {
        labelShown_ = false;
        renderer_.SetVisible(label_, false);
        return S_OK;
    }
    if (!labelDirty_ && labelItem_ == hovered_ && label_.width) return S_OK;

    const DockItem& item = items_[static_cast<size_t>(hovered_)];
    const std::wstring text = item.displayName.empty() ? item.key.Source() : item.displayName;
    const float s = layout_.scale;
    IDWriteFactory* dw = renderer_.DWrite();
    if (!labelFormat_ || labelScale_ != s) {
        labelFormat_ = nullptr;
        labelEllipsis_ = nullptr;
        if (FAILED(dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, kLabelFontSize * s, L"", labelFormat_.put()))) {
            labelFormat_ = nullptr;
            return S_OK;
        }
        labelFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        if (SUCCEEDED(dw->CreateEllipsisTrimmingSign(labelFormat_.get(), labelEllipsis_.put()))) {
            const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            labelFormat_->SetTrimming(&trimming, labelEllipsis_.get());
        }
        labelScale_ = s;
    }

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
    const float radius = std::min(std::round(config_->theme.labelCornerRadius * s), static_cast<float>(h) / 2.0f);
    const float stroke = std::max(1.0f, std::round(s));
    const D2D1_COLOR_F back = Theme::ToColor(config_->theme.labelBackground);
    const D2D1_COLOR_F fore = Theme::ToColor(config_->theme.labelText);
    const D2D1_COLOR_F border = Theme::ToColor(config_->theme.labelBorder);

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

HRESULT DockWindow::DrawFrame(bool all) {
    HRESULT hr = S_OK;
    const bool snap = all || snapPending_;
    const UINT bgW = static_cast<UINT>(layout_.bgSurfaceW);
    const UINT barH = static_cast<UINT>(layout_.bar.bottom - layout_.bar.top);
    if (all || bgLeft_.width != bgW || bgLeft_.height != barH) hr = RenderBackground();
    if (SUCCEEDED(hr) && (all || clockDirty_)) hr = RenderClock();
    if (SUCCEEDED(hr) && (all || dotSources_[0].width != static_cast<UINT>(layout_.iconPx) ||
                          dotSources_[0].height != layout_.dotHeight)) {
        hr = RenderDotSources();
    }
    for (DockItem& item : items_) {
        if (FAILED(hr)) break;
        if (!item.layer.visual) hr = CreateItemVisuals(item);
        if (SUCCEEDED(hr) && (all || item.dirty)) hr = RenderItem(item);
        if (SUCCEEDED(hr) && (all || item.dotDirty)) hr = RenderDot(item);
    }
    if (SUCCEEDED(hr) && labelShown_ && (all || labelDirty_)) hr = RenderLabel();
    if (FAILED(hr)) return hr;

    if (snap) {
        snapPending_ = false;
        ResetPlacementTweens();
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
    preview_.Destroy();  // its composition target belongs to the lost device; recreated on demand
    for (int attempt = 0; attempt < 3; ++attempt) {
        ResetLayers();
        icons_.Clear();  // the bitmaps belong to the lost device
        labelFormat_ = nullptr;
        labelEllipsis_ = nullptr;
        HRESULT hr = renderer_.RecreateDevice();
        if (SUCCEEDED(hr)) hr = CreateLayers();
        if (SUCCEEDED(hr)) hr = DrawFrame(true);
        if (SUCCEEDED(hr)) hr = renderer_.Commit();
        if (SUCCEEDED(hr)) return true;
        Log(L"RecreateDevice", hr);
        Sleep(100);
    }
    return false;
}

void DockWindow::OnClockTick() {
    if (!config_->clock.show) return;
    if (!clock_.UpdateText()) return;
    const float scale = static_cast<float>(dpi_) / USER_DEFAULT_SCREEN_DPI;
    const D2D1_SIZE_U size = clock_.BuildLayout(renderer_.DWrite(), scale);
    if (size.width != clockSize_.width || size.height != clockSize_.height) {
        Relayout();  // the bar width depends on the clock width
    } else {
        RenderWithRecovery([this] { return RenderClock(); });
    }
}

// --- Placement and magnification -------------------------------------------------------------
// Scales come from the cursor's distance to each item's REST slot centre (no feedback from the
// magnified layout): scale = 1 + (mag - 1) * f(d), f(d) = (1 + cos(pi d / R)) / 2 (or, with
// magnifyFalloff "linear", 1 - d / R) for d < R, R = magnifyRange slots. Magnified slots are laid
// out left to right, and the run is anchored so the point under the cursor stays under the
// cursor: the rest-layout offset u of the cursor in the bar maps to M(u) in the magnified bar
// (lead padding unscaled, slots stretched, clock area shifted by the whole growth), and the
// magnified bar starts at cursorX - M(u). So the bar grows away from the cursor on both sides
// (macOS behaviour), the hovered icon stays under the pointer, and hit-testing in rest
// coordinates always matches what is drawn.

void DockWindow::ComputeTargets() {
    const Layout& l = layout_;
    const size_t n = items_.size();
    const float stride = l.iconPx + l.gap;
    const float half = std::floor(l.gap / 2.0f);
    const bool magnify = magnifying_ && l.magnification > 1.001f && n > 0;
    const float range = static_cast<float>(config_->magnifyRange) * stride;

    // Dragging an icon: no magnification; the others make room at the insertion slot and the
    // dragged icon follows the cursor. A file dragged over the dock opens a gap (the bar grows by
    // one slot, half on each side).
    const int src = dragSource_;
    const bool dragLayout = (dragPhase_ != DragPhase::None || dragLanding_) && src >= 0 &&
                            src < static_cast<int>(n) && items_[static_cast<size_t>(src)].key == dragKey_ &&
                            dragInsert_ >= 0;
    const bool gapLayout = !dragLayout && dropGap_ >= 0 && n > 0;
    if (dragLayout || gapLayout) {
        const float shiftLeft = gapLayout ? std::floor(stride / 2.0f) : 0.0f;
        const float shiftRight = gapLayout ? stride - shiftLeft : 0.0f;
        const float base = l.itemsLeft - shiftLeft;
        for (size_t i = 0; i < n; ++i) {
            DockItem& item = items_[i];
            item.x = l.itemsLeft + static_cast<float>(i) * stride + half;
            item.y = l.itemTop;
            size_t slot = i;
            if (dragLayout) {
                const size_t s = static_cast<size_t>(src);
                if (i == s) {
                    if (dragPhase_ == DragPhase::Removing) continue;  // fades out where it was dropped
                    if (dragPhase_ == DragPhase::Dragging) {
                        const float winW = static_cast<float>(l.window.right - l.window.left);
                        item.targetScale = kDragScale;
                        item.targetX = std::clamp(cursorX_ - dragGrabDx_, 0.0f, std::max(0.0f, winW - l.iconPx));
                        continue;
                    }
                    slot = static_cast<size_t>(dragInsert_);  // landing
                } else {
                    const size_t j = i < s ? i : i - 1;
                    slot = j < static_cast<size_t>(dragInsert_) ? j : j + 1;
                }
            } else if (i >= static_cast<size_t>(dropGap_)) {
                slot = i + 1;
            }
            item.targetScale = 1.0f;
            item.targetX = base + static_cast<float>(slot) * stride + half;
        }
        bgLeftTarget_ = l.bar.left - shiftLeft;
        bgRightTarget_ = l.bar.right + shiftRight - l.bgSurfaceW;
        clockTarget_ = l.clock.left + shiftRight;
        return;
    }

    float delta = 0.0f;  // total growth of the run
    for (size_t i = 0; i < n; ++i) {
        DockItem& item = items_[i];
        item.x = l.itemsLeft + static_cast<float>(i) * stride + half;
        item.y = l.itemTop;
        float scale = 1.0f;
        if (magnify) {
            const float centre = l.itemsLeft + (static_cast<float>(i) + 0.5f) * stride;
            const float d = std::fabs(cursorX_ - centre);
            if (d < range) {
                const float f = motion_.falloff == Falloff::Linear ? 1.0f - d / range
                                                                   : 0.5f * (1.0f + std::cos(kPi * d / range));
                scale = 1.0f + (l.magnification - 1.0f) * f;
            }
        }
        item.targetScale = scale;
        delta += l.iconPx * (scale - 1.0f);
    }

    float growLeft = 0.0f;
    if (magnify) {
        const float u = cursorX_ - l.bar.left;
        const float lead = l.itemsLeft - l.bar.left;
        const float runW = static_cast<float>(n) * stride;
        float mapped;
        if (u <= lead) {
            mapped = u;
        } else if (u >= lead + runW) {
            mapped = u + delta;
        } else {
            const size_t k = std::min(n - 1, static_cast<size_t>((u - lead) / stride));
            const float frac = (u - lead - static_cast<float>(k) * stride) / stride;
            mapped = lead;
            for (size_t j = 0; j < k; ++j) mapped += l.iconPx * items_[j].targetScale + l.gap;
            mapped += frac * (l.iconPx * items_[k].targetScale + l.gap);
        }
        growLeft = std::clamp(mapped - u, 0.0f, delta);
    }
    const float growRight = delta - growLeft;

    float slotLeft = l.itemsLeft - growLeft;
    for (DockItem& item : items_) {
        const float w = l.iconPx * item.targetScale;
        // The icon box keeps its rest size and is scaled about its bottom centre, so its left
        // edge sits half an icon left of the magnified icon's centre.
        item.targetX = slotLeft + half + w / 2.0f - l.iconPx / 2.0f;
        slotLeft += w + l.gap;
    }

    bgLeftTarget_ = l.bar.left - growLeft;
    bgRightTarget_ = l.bar.right + growRight - l.bgSurfaceW;
    clockTarget_ = l.clock.left + growRight;

    if (labelShown_ && hovered_ >= 0 && hovered_ < static_cast<int>(n) && label_.width) {
        const DockItem& h = items_[static_cast<size_t>(hovered_)];
        const float winW = static_cast<float>(l.window.right - l.window.left);
        const float lw = static_cast<float>(label_.width);
        const float centre = h.targetX + l.iconPx / 2.0f;
        const float iconTop = l.itemTop + l.iconPx * (1.0f - h.targetScale);
        labelXTarget_ = std::round(std::clamp(centre - lw / 2.0f, 0.0f, std::max(0.0f, winW - lw)));
        labelYTarget_ = std::round(std::max(0.0f, iconTop - std::round(kLabelGap * l.scale) - label_.height));
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

    if (!bgLeftX_.valid) {
        // Static parts, re-applied after a snap (layout change) or a background redraw.
        renderer_.SetClip(bgLeftClip_, D2D1::RectF(-1e5f, -1e5f, l.seamX, 1e5f));
        renderer_.SetClip(bgRightClip_, D2D1::RectF(l.seamX, -1e5f, 1e5f, 1e5f));
        if (bgLeft_.visual) bgLeft_.visual->SetOffsetY(l.bar.top);
        if (bgRight_.visual) bgRight_.visual->SetOffsetY(l.bar.top);
        if (clockLayer_.visual) clockLayer_.visual->SetOffsetY(l.clock.top);
    }
    if (bgLeft_.visual) {
        renderer_.Drive(
            bgLeftX_, bgLeftTarget_, duration, now, [&](auto v) { return bgLeft_.visual->SetOffsetX(v); }, posEpsilon,
            easing);
    }
    if (bgRight_.visual) {
        renderer_.Drive(
            bgRightX_, bgRightTarget_, duration, now, [&](auto v) { return bgRight_.visual->SetOffsetX(v); },
            posEpsilon, easing);
    }
    if (clockLayer_.visual) {
        renderer_.Drive(
            clockX_, clockTarget_, duration, now, [&](auto v) { return clockLayer_.visual->SetOffsetX(v); },
            posEpsilon, easing);
    }

    for (DockItem& item : items_) {
        if (!item.layer.visual || !item.layer.translate || !item.layer.scale) continue;
        if (!item.tx.valid) {
            renderer_.EnsurePlacement(item.layer, l.iconPx / 2.0f, l.iconPx);
            item.layer.translate->SetOffsetY(item.y);
            if (item.dot.visual) item.dot.visual->SetOffsetY(l.dotTop);
        }
        IDCompositionTranslateTransform* translate = item.layer.translate.get();
        IDCompositionScaleTransform* scale = item.layer.scale.get();
        IDCompositionVisual2* dot = item.dot.visual.get();
        // The dot follows the icon box's x: one animation object drives both.
        renderer_.Drive(
            item.tx, item.targetX, duration, now,
            [&](auto v) {
                HRESULT hr = translate->SetOffsetX(v);
                if (SUCCEEDED(hr) && dot) hr = dot->SetOffsetX(v);
                return hr;
            },
            posEpsilon, easing);
        renderer_.Drive(
            item.ts, item.targetScale, duration, now,
            [&](auto v) {
                HRESULT hr = scale->SetScaleX(v);
                if (SUCCEEDED(hr)) hr = scale->SetScaleY(v);
                return hr;
            },
            scaleEpsilon, easing);
    }

    if (dropGap_ >= 0 && marker_.visual && marker_.width) {
        // Centred in the gap (the gap layout starts half a slot left of the rest layout).
        const float stride = l.iconPx + l.gap;
        const float x = l.itemsLeft - std::floor(stride / 2.0f) + static_cast<float>(dropGap_) * stride +
                        std::floor(l.gap / 2.0f) + std::round((l.iconPx - static_cast<float>(marker_.width)) / 2.0f);
        if (!markerX_.valid) marker_.visual->SetOffsetY(l.itemTop);
        renderer_.Drive(
            markerX_, x, duration, now, [&](auto v) { return marker_.visual->SetOffsetX(v); }, 0.01f, easing);
    }

    if (labelShown_ && label_.visual) {
        renderer_.Drive(
            labelX_, labelXTarget_, duration, now, [&](auto v) { return label_.visual->SetOffsetX(v); }, 0.01f,
            easing);
        renderer_.Drive(
            labelY_, labelYTarget_, duration, now, [&](auto v) { return label_.visual->SetOffsetY(v); }, 0.01f,
            easing);
    }
}

void DockWindow::ResetPlacementTweens() {
    bgLeftX_.valid = bgRightX_.valid = clockX_.valid = false;
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
    }
}

void DockWindow::LeaveZone() {
    if (!CursorInZone()) previewSuppressed_ = false;  // really left (not a menu taking the mouse)
    const bool wasActive = magnifying_ || hovered_ >= 0;
    magnifying_ = false;
    pressed_ = -1;
    pressedMiddle_ = -1;
    if (magnifyPending_) {
        KillTimer(hwnd_, kMagnifyTimerId);
        magnifyPending_ = false;
    }
    SetHover(-1);
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
    // The active zone: the bar's (rest) horizontal extent, from the bar top (or, while
    // magnified, the magnified icons' top) down to the screen edge.
    if (hideState_ == HideState::Hidden) return false;
    const Layout& l = layout_;
    const float x = static_cast<float>(pt.x);
    const float y = static_cast<float>(pt.y);
    if (x < l.bar.left || x >= l.bar.right) return false;
    const float top = magnifying_ ? std::min(l.magTop, l.bar.top) : l.bar.top;
    return y >= top && y < static_cast<float>(l.window.bottom - l.window.top);
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
    if (index == hovered_) return;
    const int previous = hovered_;
    hovered_ = index;
    UpdatePreviewHover();
    if (index < 0) {
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
        hideState_ == HideState::Hiding) {
        return;
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
// Resting on an item with two or more windows for previewDelayMs shows PreviewPopup above the
// dock (centred on the item's magnified position, kPreviewGap above the stage). While it is on
// screen it holds the dock shown (previewHold_) and the name label stays hidden. Moving to another
// multi-window item switches the popup; to a single-window item dismisses it. Leaving both the
// item and the popup starts a kPreviewLeaveMs grace timer. Menus, drags, dock clicks and the dock
// hiding dismiss it. All timers are one-shot; none runs once the popup is hidden.

bool DockWindow::PreviewEligible(int index) const {
    if (!config_->previews || index < 0 || index >= static_cast<int>(items_.size())) return false;
    if (items_[static_cast<size_t>(index)].windows.size() < 2) return false;
    // Showing counts: the hover usually starts while the dock slides in (OnPreviewTimer requires
    // Shown).
    return (hideState_ == HideState::Shown || hideState_ == HideState::Showing) && !hold_ &&
           dragPhase_ == DragPhase::None && !dropActive_;
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
    if (previewSuppressed_ && hovered_ >= 0 && items_[static_cast<size_t>(hovered_)].key != previewSuppressKey_) {
        previewSuppressed_ = false;  // moved on to another item
    }
    if (hovered_ >= 0 && !previewSuppressed_ && PreviewEligible(hovered_) && hwnd_) {
        SetTimer(hwnd_, kPreviewTimerId,
                 std::max<UINT>(USER_TIMER_MINIMUM, static_cast<UINT>(config_->previewDelayMs)), nullptr);
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
    const ThemeConfig& theme = config_->theme;
    style.tint = acrylic_ ? theme.acrylicBackground : theme.background;
    style.text = theme.labelText;
    style.border = theme.barBorder;
    style.hotBackground = theme.previewHotBackground;
    style.hotBorder = theme.previewHotBorder;
    style.closeHover = theme.closeHover;
    style.cornerRadiusDip = theme.cornerRadius;
    style.monitor = l.monitor;
    if (!keepAnchor) {
        previewAnchorX_ = l.window.left + static_cast<int>(std::lround(item.targetX + l.iconPx / 2.0f));
    }
    const int bottom = l.window.top - static_cast<int>(std::lround(kPreviewGap * l.scale));
    const AppKey key = item.key;
    if (!preview_.Show(content, style, previewAnchorX_, bottom)) {
        DismissPreview(false);
        return;
    }
    previewItem_ = index;
    previewKey_ = key;
    if (!previewHold_) {
        previewHold_ = true;
        CancelHide();
    }
    HideLabel();
    RenderWithRecovery([] { return S_OK; });  // commit the label hide
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
    if (!previewHold_) return;
    previewHold_ = false;
    if (!hwnd_ || !leaveCheck || hold_ || hideState_ == HideState::Hidden) return;
    if (!CursorInZone()) LeaveZone();  // return to rest, compact, start the hide delay
}

void DockWindow::SuppressPreview() {
    if (hovered_ >= 0 && hovered_ < static_cast<int>(items_.size())) {
        previewSuppressed_ = true;
        previewSuppressKey_ = items_[static_cast<size_t>(hovered_)].key;
    }
    DismissPreview(!hold_, false);
}

void DockWindow::RefreshPreview() {
    if (!preview_.Visible()) return;
    int index = -1;
    for (size_t i = 0; i < items_.size() && index < 0; ++i) {
        if (items_[i].key == previewKey_) index = static_cast<int>(i);
    }
    if (index < 0 || items_[static_cast<size_t>(index)].windows.empty()) {
        DismissPreview(true);  // the last window closed
        return;
    }
    ShowPreview(index, true);  // cards follow the windows (a closed one disappears)
}

// --- Acrylic backdrop ------------------------------------------------------------------------
// The blur is a separate window (Backdrop) beneath the dock window. DirectComposition animations
// cannot move a window, so while the bar's edges or the slide offset animate, a one-shot 16 ms
// timer chain samples the same Tweens and moves the window; it stops as soon as they settle, so
// nothing runs at rest. The auto-hide slide uses the same chain (the blur slides with the bar and
// is clipped at the monitor's bottom edge) instead of hiding the blur, which would make the bar
// visibly lose its blur at the start of every hide.

void DockWindow::UpdateAcrylic() {
    const bool want = config_->theme.acrylic && Backdrop::SystemAllowsTransparency();
    if (want && !backdrop_.Exists()) backdrop_.Create(instance_);
    acrylic_ = want && backdrop_.Exists();
    if (acrylic_) {
        acrylicRadiusDip_ = backdrop_.SetCornerRadius(config_->theme.cornerRadius);
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
        const float dy = value(rootY_, shown ? 0.0f : l.slide);
        r.left = l.window.left + std::lround(left);
        r.right = l.window.left + std::lround(right);
        r.top = l.window.top + std::lround(l.bar.top + dy);
        // Sliding out: clip at the monitor's edge (a monitor below must not show the blur).
        r.bottom = std::min(l.window.top + std::lround(l.bar.bottom + dy), l.monitor.bottom);
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
    if (!hwnd_ || !config_->AutoHideOn() || Held() || hidePending_) return;
    // Intelligent: re-test now (cheap; this runs on a mouse leave, not while idle) rather than
    // trusting overlapped_, which may be stale after a suppressed show (see OnOverlapCheck).
    if (config_->autoHide == AutoHideMode::Intelligent) {
        overlapped_ = ForegroundOverlaps();
        if (!overlapped_) return;
    }
    if (hideState_ == HideState::Hidden || hideState_ == HideState::Hiding) return;
    SetTimer(hwnd_, kHideTimerId, std::max<UINT>(USER_TIMER_MINIMUM, static_cast<UINT>(config_->hideDelayMs)),
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
    if (!config_->AutoHideOn() || Held()) return;
    if (config_->autoHide == AutoHideMode::Intelligent) {
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
    if (magnifyPending_) {
        KillTimer(hwnd_, kMagnifyTimerId);
        magnifyPending_ = false;
    }
    magnifying_ = false;
    SetHover(-1);
    hideState_ = HideState::Hiding;
    UpdateWindowRect();  // bar -> bar column down to the screen edge (room for the slide)
    double duration = 0.0;
    RenderWithRecovery([&] {
        ComputeTargets();
        ApplyTargets(motion_.rest);  // normally already at rest
        duration = ApplyRootPose(motion_.slide);
        return S_OK;
    });
    SetTimer(hwnd_, kSlideTimerId, TimerMs(duration + 0.02), nullptr);
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
    UpdateWindowRect();  // strip -> bar column (the content is still transparent)
    double duration = 0.0;
    RenderWithRecovery([&] {
        duration = ApplyRootPose(motion_.slide);
        return S_OK;
    });
    SetTimer(hwnd_, kSlideTimerId, TimerMs(duration + 0.02), nullptr);
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
}

void DockWindow::OnRevealTimer() {
    KillTimer(hwnd_, kRevealTimerId);
    revealPending_ = false;
    if (hideState_ != HideState::Hidden || !config_->AutoHideOn() || !CursorOnStrip()) return;
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
    if (pid == GetCurrentProcessId()) return false;  // the dock's own windows (a settings window)
    if (IsIconic(fg) || !IsWindowVisible(fg)) return false;
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(fg, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return false;
    if (WindowTracker::IsExcludedClass(fg)) return false;  // desktop, taskbars, Start / Search
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(fg, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))) && !GetWindowRect(fg, &r)) {
        return false;
    }
    // The bar's column from the resting bar's top down to the screen edge (the bottom margin
    // included: a window there sits between the bar and the reveal strip).
    const Layout& l = layout_;
    const RECT dock{l.window.left + static_cast<LONG>(std::floor(l.bar.left)),
                    l.window.top + static_cast<LONG>(std::floor(l.bar.top)),
                    l.window.left + static_cast<LONG>(std::ceil(l.bar.right)), l.monitor.bottom};
    RECT hit{};
    return IntersectRect(&hit, &r, &dock) != FALSE;
}

void DockWindow::OnOverlapCheck(bool force) {
    if (!hwnd_ || config_->autoHide != AutoHideMode::Intelligent) return;
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
    // Re-asserting the z-order is cheap and only happens on hide transitions.
    if (!hwnd_) return;
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

double DockWindow::ApplyRootPose(double duration) {
    IDCompositionVisual2* root = renderer_.Root();
    if (!root) return 0.0;
    if (!rootY_.valid) ApplyStageTransform();  // a fresh root (device recovery)
    const HideStyle style = motion_.hide;
    if (style == HideStyle::None) duration = 0.0;
    const bool moves = style != HideStyle::Fade;                             // slide, slideOnly, none
    const bool fades = style == HideStyle::Slide || style == HideStyle::Fade;  // animated opacity
    const bool shown = hideState_ == HideState::Shown || hideState_ == HideState::Showing;
    const float targetY = shown || !moves ? 0.0f : layout_.slide;
    // slideOnly keeps full opacity while it slides and is transparent only once Hidden.
    const float targetOpacity =
        shown ? 1.0f : (fades || style == HideStyle::None || hideState_ == HideState::Hidden ? 0.0f : 1.0f);
    const double now = Renderer::Now();
    if (duration > 0.0) {
        // A reversal mid-slide only travels the remaining distance, at the same speed. The
        // fraction comes from the property that animates (the opacity when it does).
        float remaining = 1.0f;
        if (fades && rootOpacity_.valid) {
            remaining = std::fabs(targetOpacity - rootOpacity_.Value(now));
        } else if (!fades && rootY_.valid && layout_.slide > 0.0f) {
            remaining = std::min(1.0f, std::fabs(targetY - rootY_.Value(now)) / layout_.slide);
        }
        if (fades ? rootOpacity_.valid : rootY_.valid) {
            duration = std::max(std::min(0.04, duration), duration * static_cast<double>(remaining));
        }
    }
    const double durationY = moves ? duration : 0.0;
    const double durationOpacity = fades ? duration : 0.0;
    renderer_.Drive(rootY_, targetY, durationY, now, [&](auto v) { return root->SetOffsetY(v); });
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
    const RECT& m = layout_.monitor;
    return pt.y >= m.bottom - 1 && pt.y < m.bottom && pt.x >= m.left && pt.x < m.right;
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
    if (config_->autoHide != AutoHideMode::Intelligent) {
        watch_.Stop();  // off / always: no hooks at all
        overlapped_ = false;
    }
    if (!config_->AutoHideOn()) {
        CancelHide();
        if (revealPending_) {
            KillTimer(hwnd_, kRevealTimerId);
            revealPending_ = false;
        }
        if (hideState_ == HideState::Hidden || hideState_ == HideState::Hiding) StartShow();
        return;
    }
    if (config_->autoHide == AutoHideMode::Intelligent) {
        watch_.Start(hwnd_, [this] { OnOverlapCheck(); });
        OnOverlapCheck(true);  // hide if overlapped now, show if hidden and not
        return;
    }
    if ((hideState_ == HideState::Shown || hideState_ == HideState::Showing) && !Held() && !CursorInZone()) {
        ScheduleHide();
    }
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
    const RECT& w = layout_.window;
    return RECT{w.left + static_cast<LONG>(std::floor(layout_.bar.left)), w.top + static_cast<LONG>(std::floor(layout_.bar.top)),
                w.left + static_cast<LONG>(std::ceil(layout_.bar.right)), w.top + static_cast<LONG>(std::ceil(layout_.bar.bottom))};
}

void DockWindow::SetHold(bool hold) {
    if (hold == hold_ || !hwnd_) return;
    hold_ = hold;
    if (hold) {
        CancelHide();
        SuppressPreview();  // a menu opens or a drag starts
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
    pins.reserve(config_->pinned.size());
    for (const PinnedEntry& e : config_->pinned) {
        const std::wstring id = e.path.empty() ? L"a:" + e.aumid : L"p:" + e.path;
        auto it = pinCache_.find(id);
        AppIdentity::ResolvedPin pin =
            it != pinCache_.end() ? it->second : AppIdentity::ResolvePin(e.path, e.path.empty() ? e.aumid : L"");
        cache[id] = pin;
        pins.push_back(std::move(pin));
    }
    pins_ = std::move(pins);
    pinCache_ = std::move(cache);  // drops entries that are no longer pinned
}

void DockWindow::ReloadPins() {
    ResolvePins();
    Rebuild(tracker_.Current());
}

void DockWindow::Rebuild(const WindowTracker::Snapshot& snap) {
    std::vector<AppKey> oldKeys;
    oldKeys.reserve(items_.size());
    for (const DockItem& it : items_) oldKeys.push_back(it.key);

    std::vector<bool> taken(items_.size(), false);
    auto takeOld = [&](const AppKey& key) -> DockItem* {
        for (size_t i = 0; i < items_.size(); ++i) {
            if (!taken[i] && items_[i].key == key) {
                taken[i] = true;
                return &items_[i];
            }
        }
        return nullptr;
    };

    std::vector<DockItem> next;
    next.reserve(pins_.size() + snap.groups.size());

    // 1. Pinned items in config order. Entries that resolve to the same app show once.
    for (const AppIdentity::ResolvedPin& pin : pins_) {
        if (std::any_of(next.begin(), next.end(), [&](const DockItem& i) { return i.key == pin.key; })) continue;
        DockItem item;
        if (DockItem* old = takeOld(pin.key)) {
            item = std::move(*old);
        } else {
            item.key = pin.key;
        }
        if (!item.pinned || item.iconFile != pin.iconFile || item.iconIndex != pin.iconIndex ||
            item.missing != pin.missing) {
            item.dirty = true;
        }
        item.pinned = true;
        item.displayName = pin.displayName;
        item.launchPath = pin.launchPath.empty() ? std::nullopt : std::optional<std::wstring>(pin.launchPath);
        item.workingDir = pin.workingDir;
        item.targetPath = pin.targetPath;
        item.iconFile = pin.iconFile;
        item.iconIndex = pin.iconIndex;
        item.missing = pin.missing;
        next.push_back(std::move(item));
    }
    const size_t pinnedCount = next.size();

    // 2. Running apps in first-seen order (the tracker keeps that order). A running app whose
    //    key matches a pin merges into the pinned item. Unpinned items keep their state.
    std::vector<const WindowTracker::Group*> groupOf(pinnedCount, nullptr);
    for (const WindowTracker::Group& g : snap.groups) {
        size_t index = SIZE_MAX;
        for (size_t p = 0; p < pinnedCount && index == SIZE_MAX; ++p) {
            if (next[p].key == g.key) index = p;
        }
        if (index == SIZE_MAX) {
            if (DockItem* old = takeOld(g.key)) {
                next.push_back(std::move(*old));
                next.back().pinned = false;  // was unpinned while running
            } else {
                DockItem fresh;
                fresh.key = g.key;
                fresh.displayName = AppIdentity::DisplayNameFor(g.key);
                next.push_back(std::move(fresh));
            }
            index = next.size() - 1;
            groupOf.push_back(nullptr);
        }
        groupOf[index] = &g;
    }

    bool landed = false;  // a launch feedback ended at once (a hop that was already over)
    for (size_t i = 0; i < next.size(); ++i) {
        DockItem& item = next[i];
        const WindowTracker::Group* g = groupOf[i];
        std::vector<HWND> windows = g ? g->windows : std::vector<HWND>{};
        if (item.windows.empty() != windows.empty()) {
            item.dotDirty = true;           // indicator dot
            if (item.missing) item.dirty = true;  // placeholder <-> live icon
        }
        item.windows = std::move(windows);
        const bool attention = g && g->wantsAttention;
        if (item.wantsAttention != attention) {
            item.wantsAttention = attention;
            item.dotDirty = true;
        }
        const bool active = snap.foreground &&
                            std::find(item.windows.begin(), item.windows.end(), snap.foreground) != item.windows.end();
        if (item.active != active) {
            item.active = active;
            item.dotDirty = true;
        }
        // The launched app's window appeared: let the icon land.
        if (item.bounce == DockItem::Bounce::Bouncing && item.windows.size() > item.bounceWindowCount) {
            FinishBounce(item);
            if (item.bounce == DockItem::Bounce::None) landed = true;
        }
    }

    // Items that went away: remove their visuals (the container takes its icon children along).
    for (size_t i = 0; i < items_.size(); ++i) {
        if (taken[i]) continue;
        renderer_.DestroyLayer(items_[i].dot);
        renderer_.DestroyLayer(items_[i].layer);
        items_[i].icon.Reset();
        items_[i].iconBig.Reset();
    }

    bool structural = next.size() != oldKeys.size();
    for (size_t i = 0; !structural && i < next.size(); ++i) structural = next[i].key != oldKeys[i];

    items_ = std::move(next);
    // Indices changed under a drag: drop it (no animation; the icons settle in the new layout).
    const bool abortDrag = structural && dragPhase_ == DragPhase::Dragging;
    if (abortDrag) AbortDrag();
    if (structural && dragPhase_ == DragPhase::Removing) dragSource_ = -1;  // keeps fading in its slot
    if (structural) {
        // The window is resized and re-centred: drop hover state. Windows sends a synthetic
        // WM_MOUSEMOVE when the window moves under the cursor, which re-magnifies.
        magnifying_ = false;
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
    UpdateIconUsage();
    ScheduleBounceTimer();
    if (landed && !CursorInZone()) TryCompact();  // no bounce timer left to do it
    watch_.Poke();  // windows came or went: the foreground one may have changed
}

int DockWindow::IndexOfWindow(HWND hwnd) const {
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
    // ShellExecuteEx may pump messages (and so rebuild items_): launch from a copy of the
    // launch data, then find the item again by key.
    DockItem copy;
    copy.key = target.key;
    copy.launchPath = target.launchPath;
    copy.workingDir = target.workingDir;
    const size_t windowsBefore = target.windows.size();
    if (!Launcher::Launch(copy)) {
        MessageBeep(MB_ICONWARNING);
        return false;
    }
    for (DockItem& item : items_) {
        if (item.key != copy.key) continue;
        if (item.windows.size() <= windowsBefore) StartBounce(item, windowsBefore);
        break;
    }
    return true;
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
            renderer_.StopOffsetYAnimation(item.layer, 0.0f);
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
        const float h = std::round(layout_.iconPx * kBounceHeight);
        // Rise: y = -h * (2u - u^2), u = s / t.   Fall: y = -h * (1 - u^2).
        anim->AddCubic(0.0, 0.0f, -2.0f * h / t, h / (t * t), 0.0f);
        anim->AddCubic(t, -h, 0.0f, h / (t * t), 0.0f);
        if (style == LaunchStyle::Hop) {
            anim->End(2.0 * t, 0.0f);
        } else {
            anim->AddRepeat(2.0 * t, 2.0 * t);
        }
        hr = renderer_.AnimateOffsetY(item.layer, anim.get());
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

    const float h = std::round(layout_.iconPx * kBounceHeight);
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
    if (!anim || remaining < 0.02f || FAILED(renderer_.AnimateOffsetY(item.layer, anim.get()))) {
        renderer_.StopOffsetYAnimation(item.layer, 0.0f);
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
                renderer_.StopOffsetYAnimation(item.layer, 0.0f);
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
    for (const DockItem& item : items_) keys.push_back(item.key);
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
// redrawn). The insertion slot is the rest slot under the cursor, limited to the pinned section.
// Release: the icon lands in its slot (120 ms) and the new pinned order goes to the config. The
// layout keeps its geometry, so the landing animation keeps running through the item rebuild.
// Released more than 1.5 bar heights above the bar, a pinned app without windows is unpinned (the
// icon fades out first). Escape (polled: the dock never has keyboard focus) or losing the capture
// cancels: everything slides back.

size_t DockWindow::PinnedCount() const {
    size_t n = 0;
    while (n < items_.size() && items_[n].pinned) ++n;
    return n;
}

size_t DockWindow::ConfigIndexOfItem(size_t itemIndex) const {
    const size_t end = config_->pinned.size();
    if (itemIndex >= items_.size() || !items_[itemIndex].pinned) return end;
    const AppKey& key = items_[itemIndex].key;
    for (size_t i = 0; i < pins_.size() && i < end; ++i) {
        if (pins_[i].key == key) return i;
    }
    return end;
}

int DockWindow::DragInsertIndex(float stageX) const {
    const int n = static_cast<int>(items_.size());
    const int s = dragSource_;
    if (s < 0 || s >= n) return -1;
    const float stride = layout_.iconPx + layout_.gap;
    const int raw = std::clamp(static_cast<int>(std::floor((stageX - layout_.itemsLeft) / stride)), 0, n - 1);
    const int pinned = static_cast<int>(PinnedCount());
    if (items_[static_cast<size_t>(s)].pinned) return std::min(raw, pinned - 1);
    // A running (unpinned) item: dropped into the pinned section (slots 0..pinned), it is pinned
    // there; anywhere else it goes back to its own slot.
    return raw <= pinned && raw != s ? raw : s;
}

int DockWindow::DropGapIndex(float stageX) const {
    const float stride = layout_.iconPx + layout_.gap;
    const int pinned = static_cast<int>(PinnedCount());
    // Between items i-1 and i when the cursor is between their centres.
    const int gap = static_cast<int>(std::floor((stageX - layout_.itemsLeft) / stride + 0.5f));
    return std::clamp(gap, 0, pinned);
}

bool DockWindow::DragInRemoveZone(POINT pt) const {
    const float barH = layout_.bar.bottom - layout_.bar.top;
    return static_cast<float>(pt.y) < layout_.bar.top - kRemoveZoneBars * barH;
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
    if (!item.pinned && item.key.IsClassFallback()) return;  // could never be pinned: click only
    if (!item.layer.visual) return;
    // A launch pulse animates the opacity the drag uses: end it first (it fades back to opaque,
    // which the drag's dimming below replaces).
    if (item.bounce == DockItem::Bounce::Bouncing && item.bounceStyle == LaunchStyle::Pulse) FinishBounce(item);

    dragPhase_ = DragPhase::Dragging;
    dragSource_ = s;
    dragKey_ = item.key;
    dragRemove_ = false;
    // Keep the grab point: the icon box's left edge stays this far left of the cursor.
    const float centre = item.targetX + layout_.iconPx / 2.0f;
    dragGrabDx_ = layout_.iconPx / 2.0f +
                  std::clamp(static_cast<float>(pressPt_.x) - centre, -layout_.iconPx / 2.0f, layout_.iconPx / 2.0f);
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

    // In front of the other icons while it passes over them.
    if (itemsGroup_.visual) {
        itemsGroup_.visual->RemoveVisual(item.layer.visual.get());
        itemsGroup_.visual->AddVisual(item.layer.visual.get(), FALSE, nullptr);
    }
    SetItemOpacity(item, kDragOpacity, 0.0);
    SetTimer(hwnd_, kDragTimerId, kDragPollMs, nullptr);
    dragPending_ = true;
    UpdateDrag(pt);
}

void DockWindow::UpdateDrag(POINT pt) {
    if (dragPhase_ != DragPhase::Dragging) return;
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8001) {
        EndDrag(false);
        return;
    }
    cursorX_ = static_cast<float>(pt.x);
    dragInsert_ = DragInsertIndex(cursorX_);
    const bool remove = DragRemovable() && DragInRemoveZone(pt);
    if (remove != dragRemove_) {
        dragRemove_ = remove;
        SetItemOpacity(items_[static_cast<size_t>(dragSource_)], remove ? kRemoveOpacity : kDragOpacity,
                       motion_.magnify);
    }
    ScheduleMagnify();  // throttled ComputeTargets + ApplyTargets (80 ms)
}

void DockWindow::EndDrag(bool commit) {
    if (dragPhase_ != DragPhase::Dragging) return;
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
    const bool remove = valid && commit && dragRemove_ && DragRemovable();

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

    // The new pinned order, if it changed.
    std::vector<AppKey> order;
    if (valid && commit && dragInsert_ >= 0 && dragInsert_ != s) {
        std::vector<size_t> seq;
        seq.reserve(items_.size());
        for (size_t i = 0; i < items_.size(); ++i) {
            if (static_cast<int>(i) != s) seq.push_back(i);
        }
        seq.insert(seq.begin() + dragInsert_, static_cast<size_t>(s));
        const size_t pinned = PinnedCount() + (items_[static_cast<size_t>(s)].pinned ? 0 : 1);
        for (size_t k = 0; k < pinned && k < seq.size(); ++k) order.push_back(items_[seq[k]].key);
    }
    if (order.empty()) dragInsert_ = s;  // cancelled / unchanged: back to its own slot

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
    dragRemove_ = false;

    KeepStage(motion_.rest + 0.04);  // the window stays at stage size until the icon has landed
    if (!order.empty() && pinRequests_.reorder) pinRequests_.reorder(order);
    SetHold(false);  // starts the hide delay if the mouse is no longer on the dock
    POINT cursor{};
    if (hwnd_ && GetCursorPos(&cursor) && PtInRect(&winRect_, cursor)) ArmMouseTracking();
}

void DockWindow::AbortDrag() {
    if (dragPhase_ != DragPhase::Dragging) return;
    if (dragPending_) {
        KillTimer(hwnd_, kDragTimerId);
        dragPending_ = false;
    }
    dragPhase_ = DragPhase::None;
    dragSource_ = -1;
    dragInsert_ = -1;
    dragRemove_ = false;
    if (GetCapture() == hwnd_) ReleaseCapture();
    for (DockItem& item : items_) {
        if (item.key == dragKey_) SetItemOpacity(item, 1.0f, 0.0);
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
// The dock HWND is an OLE drop target. A drag carrying .lnk / .exe files opens a gap at the
// insertion slot (same slot-midpoint rule, limited to the pinned section) with a thin accent
// marker in it; dropping pins the files there. Dragged onto the hidden dock's 1 px strip, the dock
// reveals itself right away.

std::vector<std::wstring> DockWindow::PinnableFiles(IDataObject* data) {
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
            const size_t dot = path.rfind(L'.');
            const std::wstring ext = dot == std::wstring::npos ? L"" : AppIdentity::Normalize(path.substr(dot));
            if (ext == L".lnk" || ext == L".exe") files.push_back(std::move(path));  // only these can be pinned
        }
        GlobalUnlock(medium.hGlobal);
    }
    ReleaseStgMedium(&medium);
    return files;
}

namespace {
DWORD DropEffect(DWORD allowed) {
    if (allowed & DROPEFFECT_LINK) return DROPEFFECT_LINK;
    if (allowed & DROPEFFECT_COPY) return DROPEFFECT_COPY;
    return DROPEFFECT_NONE;
}
}  // namespace

HRESULT DockWindow::RenderMarker() {
    const float s = layout_.scale;
    const UINT w = static_cast<UINT>(std::max(2.0f, std::round(kMarkerWidth * s)));
    const UINT h = static_cast<UINT>(layout_.iconPx);
    if (marker_.width == w && marker_.height == h) return S_OK;
    const D2D1_COLOR_F color = Theme::ToColor(config_->theme.accent);
    return renderer_.DrawLayer(marker_, w, h, [&](ID2D1DeviceContext* dc) {
        winrt::com_ptr<ID2D1SolidColorBrush> brush;
        if (FAILED(dc->CreateSolidColorBrush(color, brush.put()))) return;
        const float r = static_cast<float>(w) / 2.0f;
        dc->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(0, 0, static_cast<float>(w), static_cast<float>(h)), r, r), brush.get());
    });
}

void DockWindow::SetDropGap(int gap) {
    if (items_.empty()) gap = -1;  // nothing to make room between
    if (gap == dropGap_) return;
    const bool wasOpen = dropGap_ >= 0;
    dropGap_ = gap;
    RenderWithRecovery([&] {
        HRESULT hr = S_OK;
        if (gap >= 0) {
            hr = RenderMarker();
        } else {
            renderer_.SetVisible(marker_, false);
            markerX_.valid = false;
        }
        ComputeTargets();
        ApplyTargets(motion_.magnify, true);
        if (gap >= 0 && !wasOpen) renderer_.SetVisible(marker_, true);
        return hr;
    });
}

DWORD DockWindow::OnDragEnter(IDataObject* data, POINT screenPt, DWORD allowed) {
    dropAccept_ = !PinnableFiles(data).empty() && dragPhase_ == DragPhase::None;
    if (!dropAccept_ || DropEffect(allowed) == DROPEFFECT_NONE) return DROPEFFECT_NONE;
    dropActive_ = true;
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
    SetDropGap(shown ? DropGapIndex(static_cast<float>(screenPt.x - layout_.window.left)) : -1);
    return DropEffect(allowed);
}

void DockWindow::OnDragLeave() {
    if (!dropActive_) return;
    dropActive_ = false;
    SetDropGap(-1);
    KeepStage(motion_.magnify + 0.04);
    SetHold(false);
}

DWORD DockWindow::OnDrop(IDataObject* data, POINT screenPt, DWORD allowed) {
    if (!dropActive_) return DROPEFFECT_NONE;
    dropActive_ = false;
    const std::vector<std::wstring> files = PinnableFiles(data);
    const int gap = DropGapIndex(static_cast<float>(screenPt.x - layout_.window.left));
    const size_t at = ConfigIndexOfItem(static_cast<size_t>(gap));
    // The gap closes as the new icon takes its slot: no closing animation, the rebuilt layout
    // (one slot wider, re-centred) puts everything where the gap layout had it.
    dropGap_ = -1;
    renderer_.SetVisible(marker_, false);
    markerX_.valid = false;
    if (!files.empty() && pinRequests_.pinFiles) pinRequests_.pinFiles(files, at);
    // Nothing new was pinned (duplicates): close the gap.
    RenderWithRecovery([this] {
        ComputeTargets();
        ApplyTargets(motion_.rest);
        return S_OK;
    });
    KeepStage(motion_.rest + 0.04);
    SetHold(false);
    return files.empty() ? DROPEFFECT_NONE : DropEffect(allowed);
}

int DockWindow::HitTest(POINT pt) const {  // stage coordinates
    // Rest-slot hit-testing inside the active zone. The magnified layout is anchored at the
    // cursor, so the rest slot under the cursor is also the icon drawn under it.
    if (items_.empty() || !InZone(pt)) return -1;
    const float x = static_cast<float>(pt.x);
    const float stride = layout_.iconPx + layout_.gap;
    if (x < layout_.itemsLeft || stride <= 0.0f) return -1;
    const int index = static_cast<int>((x - layout_.itemsLeft) / stride);
    return index < static_cast<int>(items_.size()) ? index : -1;
}

const DockItem* DockWindow::ItemAt(POINT clientPt) const {
    const int i = HitTest(ToStage(clientPt));
    return i >= 0 ? &items_[static_cast<size_t>(i)] : nullptr;
}

LRESULT DockWindow::QueryItemPoint(WPARAM wp, LPARAM lp) const {
    int index = -1;
    if (wp) {
        HWND target = reinterpret_cast<HWND>(wp);
        for (size_t i = 0; i < items_.size() && index < 0; ++i) {
            if (std::find(items_[i].windows.begin(), items_[i].windows.end(), target) != items_[i].windows.end()) {
                index = static_cast<int>(i);
            }
        }
    } else if (lp >= 0 && lp < static_cast<LPARAM>(items_.size())) {
        index = static_cast<int>(lp);
    }
    if (index < 0) return -1;
    const DockItem& item = items_[static_cast<size_t>(index)];
    // Rest layout, relative to the shown window position (also while hidden).
    POINT pt{layout_.window.left + static_cast<LONG>(item.x + layout_.iconPx / 2.0f),
             layout_.window.top + static_cast<LONG>(item.y + layout_.iconPx / 2.0f)};
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
                   (watch_.Pending() ? 1 : 0);
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
            return static_cast<LRESULT>(config_->autoHide);  // 0 off, 1 always, 2 intelligent
        case 15:
            return overlapped_ ? 1 : 0;
        case 17:
            return motion_.reduce ? 1 : 0;
        case 19:
            return static_cast<LRESULT>(watch_.AcceptedEvents());
        case 20:
            return config_->themeResolveFailed ? 1 : 0;
        case 21:
            return static_cast<LRESULT>(watch_.DeliveredEvents());
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
    if (tracker_.HandleMessage(msg, wp, lp)) return 0;
    if (watch_.HandleMessage(msg, wp, lp)) return 0;
    if (msgQueryItem_ && msg == msgQueryItem_) return QueryItemPoint(wp, lp);
    if (msgQueryState_ && msg == msgQueryState_) return QueryState(wp, lp);

    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;  // clicks never focus the dock

        case WM_MOUSEMOVE: {
            ArmMouseTracking();
            const POINT pt = ToStage(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            if (dragPhase_ == DragPhase::Dragging) {
                UpdateDrag(pt);
                return 0;
            }
            if (dragPhase_ == DragPhase::Removing) return 0;
            if (pressed_ >= 0 && (wp & MK_LBUTTON) && hideState_ != HideState::Hidden &&
                (std::abs(pt.x - pressPt_.x) > GetSystemMetricsForDpi(SM_CXDRAG, dpi_) ||
                 std::abs(pt.y - pressPt_.y) > GetSystemMetricsForDpi(SM_CYDRAG, dpi_))) {
                BeginDrag(pt);
                if (dragPhase_ == DragPhase::Dragging) return 0;
            }
            if (hideState_ == HideState::Hidden) {
                // On the reveal strip.
                if (revealArmed_ && config_->AutoHideOn() && !revealPending_) {
                    SetTimer(hwnd_, kRevealTimerId,
                             std::max<UINT>(USER_TIMER_MINIMUM, static_cast<UINT>(config_->revealDelayMs)), nullptr);
                    revealPending_ = true;
                }
                return 0;
            }
            if (!InZone(pt)) {
                LeaveZone();
                return 0;
            }
            CancelHide();
            if (!expanded_) {
                // Entering the bar: give magnified icons and the label their room first. The
                // cursor is on the bar, so the larger transparent area is harmless meanwhile.
                expanded_ = true;
                UpdateWindowRect();
            }
            if (hideState_ == HideState::Hiding) StartShow();  // came back mid-slide: reverse
            magnifying_ = true;
            cursorX_ = static_cast<float>(pt.x);
            SetHover(HitTest(pt));
            ScheduleMagnify();
            return 0;
        }

        case WM_MOUSELEAVE:
            trackingMouse_ = false;
            if (dragPhase_ != DragPhase::None) return 0;  // the drag owns the mouse
            if (hideState_ == HideState::Hidden) {
                if (revealPending_) {  // left the strip before the reveal delay: stay hidden
                    KillTimer(hwnd_, kRevealTimerId);
                    revealPending_ = false;
                }
                revealArmed_ = true;
                return 0;
            }
            LeaveZone();
            return 0;

        case WM_LBUTTONDOWN:
            SuppressPreview();  // the click cycles / starts a drag: no popup over it
            pressPt_ = ToStage(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            pressed_ = dragPhase_ == DragPhase::None ? HitTest(pressPt_) : -1;
            return 0;

        case WM_LBUTTONUP: {
            if (dragPhase_ == DragPhase::Dragging) {
                EndDrag(true);
                return 0;
            }
            const int index = HitTest(ToStage(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}));
            const bool clicked = index >= 0 && index == pressed_;
            pressed_ = -1;
            if (clicked) {
                const std::vector<HWND> windows = items_[static_cast<size_t>(index)].windows;  // copy
                if (windows.empty()) {
                    LaunchItem(index, false);
                } else {
                    Activation::ActivateItem(windows);
                }
            }
            return 0;
        }

        case WM_MBUTTONDOWN:
            SuppressPreview();
            pressedMiddle_ = HitTest(ToStage(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}));
            return 0;

        case WM_MBUTTONUP: {
            const int index = HitTest(ToStage(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}));
            const bool clicked = index >= 0 && index == pressedMiddle_;
            pressedMiddle_ = -1;
            if (clicked) LaunchItem(index, true);
            return 0;
        }

        case WM_CAPTURECHANGED:
            // Someone else took the mouse (a menu, Alt+Tab...): cancel the drag.
            if (dragPhase_ == DragPhase::Dragging && reinterpret_cast<HWND>(lp) != hwnd_) EndDrag(false);
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
                    KillTimer(hwnd_, kClockTimerId);  // one-shot; rescheduled on the next boundary
                    OnClockTick();
                    if (config_->clock.show) clock_.ScheduleNextTick(hwnd_, kClockTimerId);
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
                    if (dragPhase_ != DragPhase::Dragging) {
                        KillTimer(hwnd_, kDragTimerId);
                        dragPending_ = false;
                    } else if (GetAsyncKeyState(VK_ESCAPE) & 0x8001) {
                        EndDrag(false);
                    }
                    return 0;
                case kRemoveTimerId:
                    OnRemoveTimer();
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
                case kBackdropTimerId:
                    KillTimer(hwnd_, kBackdropTimerId);  // one-shot; re-armed while the bar animates
                    backdropPending_ = false;
                    SyncBackdrop();
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
            OnClockTick();
            if (config_->clock.show) clock_.ScheduleNextTick(hwnd_, kClockTimerId);
            return 0;

        case WM_POWERBROADCAST:
            if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) {
                OnClockTick();
                if (config_->clock.show) clock_.ScheduleNextTick(hwnd_, kClockTimerId);
            }
            return TRUE;

        case WM_DPICHANGED:
            // New scale: Relayout re-renders every surface at the new pixel size (the icon cache
            // re-fetches bitmaps whose pixel size changed). The suggested rect is ignored: we
            // always place ourselves.
            dpi_ = HIWORD(wp);
            labelDirty_ = true;
            DismissPreview(false, false);
            Relayout();
            return 0;

        case WM_DISPLAYCHANGE:
            DismissPreview(false, false);
            Relayout();  // re-picks the monitor (falls back to the primary) and re-centres
            return 0;

        case WM_SETTINGCHANGE:
            if (wp == SPI_SETHIGHCONTRAST ||
                (lp && CompareStringOrdinal(reinterpret_cast<const wchar_t*>(lp), -1, L"ImmersiveColorSet", -1,
                                            TRUE) == CSTR_EQUAL)) {
                // "Transparency effects" or high contrast toggled: acrylic on/off.
                UpdateAcrylic();
                Relayout();
            } else if (wp == SPI_SETWORKAREA || wp == SPI_SETNONCLIENTMETRICS) {
                Relayout();
            } else if (wp == SPI_SETCLIENTAREAANIMATION) {
                // Animation effects toggled: reduceMotion "system" follows it from the next animation.
                RefreshMotion();
            }
            return 0;

        case WM_WINDOWPOSCHANGED:
            // Keep the blur directly beneath the dock whenever the dock's z-order changes (layout
            // passes re-assert topmost; another topmost window must not end up between the two).
            if (!(reinterpret_cast<const WINDOWPOS*>(lp)->flags & SWP_NOZORDER)) backdrop_.Restack(hwnd_);
            break;

        case WM_DESTROY:
            tracker_.Stop();
            watch_.Stop();
            overlapped_ = false;
            if (dropTarget_) {
                RevokeDragDrop(hwnd_);
                dropTarget_->Detach();
                dropTarget_->Release();
                dropTarget_ = nullptr;
            }
            dragPhase_ = DragPhase::None;
            dropActive_ = false;
            if (GetCapture() == hwnd_) ReleaseCapture();
            for (UINT_PTR id : {kClockTimerId, kIconEvictTimerId, kBounceTimerId, kHideTimerId, kRevealTimerId,
                                kSlideTimerId, kMagnifyTimerId, kLabelTimerId, kRestTimerId, kBackdropTimerId,
                                kDragTimerId, kRemoveTimerId, kPreviewTimerId, kPreviewLeaveTimerId,
                                kLabelFadeTimerId}) {
                KillTimer(hwnd_, id);
            }
            dragPending_ = removePending_ = false;
            evictPending_ = hidePending_ = revealPending_ = magnifyPending_ = labelPending_ = restPending_ = false;
            labelFading_ = false;
            backdropPending_ = false;
            previewPending_ = previewLeavePending_ = previewHold_ = false;
            preview_.Destroy();
            acrylic_ = false;
            backdrop_.Destroy();
            labelFormat_ = nullptr;
            labelEllipsis_ = nullptr;
            ResetLayers();
            items_.clear();
            icons_.Shutdown();
            clock_.ReleaseLayout();
            renderer_.Shutdown();
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
