#include "DockWindow.h"

#include <dwmapi.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <windowsx.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cwctype>
#include <vector>

#include "Activation.h"
#include "AppIdentity.h"
#include "IconEffects.h"
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
constexpr float kDropScale = 1.15f;          // an app / the bin under a file drag (open with / recycle)
constexpr float kDropEdge = 0.15f;           // slot fraction per side that still means "between items"
constexpr UINT kRecycleBinCoalesceMs = 200;  // bursts of bin change notifications -> one redraw
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
    folder_.Init(instance, &renderer_, &icons_,
                 FolderPopup::Callbacks{
                     [this] { DismissFolderPopup(true); },
                     [this] { OnFolderPopupClosed(); },
                 });
    weatherPopup_.Init(instance, &renderer_, &weather_,
                       WeatherPopup::Callbacks{
                           [this] { weather_.RefreshNow(); },
                           [this] {
                               DismissWeatherPopup(true);
                               if (weatherCallbacks_.openSettings) weatherCallbacks_.openSettings();
                           },
                           [this] { ReleasePopupHold(kHoldWeather, true); },
                       });
    calendar_.Init(instance, &renderer_, [this] { ReleasePopupHold(kHoldCalendar, true); });
    weather_.SetChangedCallback([this] { OnWeatherChanged(); });

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
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    // The initial enumeration (synchronous) then builds and lays out the items with their windows.
    tracking_ = true;
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
    DismissClickPopups(false, false);
    FinishLabelFade();             // its text / position may no longer match
    SyncElements();                // the clock may have been turned on / off; config re-read, timers
    // The drop marker is only redrawn when its size changes: force it (the accent may differ).
    if (marker_.visual) renderer_.DrawLayer(marker_, 0, 0, {});
    for (DockItem& item : items_) {
        item.dirty = true;
        item.dotDirty = true;
    }
    labelDirty_ = true;
    labelFormat_ = nullptr;  // theme / size may have changed
    labelEllipsis_ = nullptr;
    weatherQuiet_ = true;  // the relayout below measures and draws the widget
    weather_.Configure(config_->weather, config_->Main().Has(ItemKind::Weather), hwnd_);
    weatherQuiet_ = false;
    UpdateAcrylic();
    Relayout();
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
    const size_t index = config_ ? static_cast<size_t>(std::max(config_->Main().monitor, 0)) : 0;
    return index < monitors.size() ? monitors[index] : primary;
}

void DockWindow::ComputeLayout() {
    const Config& c = *config_;
    const DockConfig& d = c.Main();
    Layout l;
    l.scale = static_cast<float>(dpi_) / USER_DEFAULT_SCREEN_DPI;
    const float s = l.scale;

    l.iconPx = std::round(static_cast<float>(d.iconSize) * s);
    l.padding = std::round(kPadding * s);
    // With acrylic, DWM rounds the blur with its own radius; the tint matches it.
    l.cornerRadius = std::round((acrylic_ ? acrylicRadiusDip_ : c.theme.cornerRadius) * s);
    l.magnification = static_cast<float>(d.magnification);
    l.bitmapPx = static_cast<UINT>(std::lround(static_cast<double>(d.iconSize) * d.magnification * s));

    // Icon effects. The reflection surface is drawn at the bitmap size and shown at rest scale
    // under each icon: the bar grows by its height. The glow reaches glowPad beyond the icon box.
    const ThemeConfig& t = c.theme;
    const UINT reflSurface = t.reflection ? IconEffects::ReflectionHeight(l.bitmapPx, t.reflectionHeight) : 0;
    l.reflH = reflSurface && l.bitmapPx ? std::ceil(static_cast<float>(reflSurface) * l.iconPx /
                                                    static_cast<float>(l.bitmapPx))
                                        : 0.0f;
    const GlowMode glow = GlowOf(t);
    l.glowPad = glow != GlowMode::Off ? std::ceil(t.glowRadius * s) + 1.0f : 0.0f;

    const float iconBarH = std::round(l.iconPx + kBarExtraHeight * s);  // the bar without reflections
    const float barH = iconBarH + l.reflH;
    const float margin = std::round(kBottomMargin * s);
    float winH = std::round(l.iconPx * l.magnification + kWindowExtraHeight * s) + l.reflH;
    winH = std::max(winH, barH + margin);
    if (l.glowPad > 0.0f) {
        // Room above a fully magnified icon for its halo.
        const float magTopRel =
            winH - margin - barH + std::round(kIconTop * s) + l.iconPx - l.iconPx * l.magnification;
        winH += std::max(0.0f, std::ceil(l.glowPad * l.magnification) - magTopRel);
    }

    // Slots, in dock order, as their elements measure them: an icon slot is iconPx + gap (the icon
    // centred), a separator a narrow slot, a widget its content box. Spacers (`lead`, never
    // scaled) separate icons from widgets (with a hairline when the widgets are at an end of the
    // dock) and widgets from each other; the bar adds padding at both ends (half a gap less beside
    // an icon slot, whose slot already holds half a gap on each side).
    l.gap = std::round(kGap * s);
    const float half = std::floor(l.gap / 2.0f);
    const float stride = l.iconPx + l.gap;
    const float sepW = std::round(kSeparatorWidth * s);
    const float widgetGap = std::round(kWidgetGap * s);
    const LayoutContext ctx{DockAxis::Horizontal, s, l.iconPx, renderer_.DWrite()};
    enum class Run { None, Icon, Widget };
    Run first = Run::None, prev = Run::None;
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
            run = box.main > 0.0f ? Run::Widget : Run::None;  // nothing to show: takes no room
        } else {
            const ElementSize box = element.Measure(ctx);
            item.slotW = item.Magnifies() ? box.main + l.gap : box.main;
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
        if (prev != Run::None && (prev != Run::Icon || run != Run::Icon)) {
            item.lead = prev == Run::Widget && run == Run::Widget ? widgetGap : sepW;
        }
        if (first == Run::None) first = run;
        prev = run;
        itemsW += item.lead + item.slotW;
    }
    l.itemsW = itemsW;
    const float lead = first == Run::Icon ? l.padding - half : l.padding;
    const float trail = prev == Run::Icon ? l.padding - half : l.padding;
    const size_t n = icons;
    float barW = lead + itemsW + trail;
    barW = std::round(std::max(barW, l.iconPx + 2.0f * l.padding));

    // Magnified icons widen the run by sum((scale - 1) * iconPx). Both falloffs (cosine, linear) sum
    // to at most magnifyRange over all items (and each term is at most 1), which bounds the growth.
    l.growMax = 0.0f;
    if (n && l.magnification > 1.001f) {
        const float terms = static_cast<float>(std::min<size_t>(n, static_cast<size_t>(d.magnifyRange)));
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
    l.itemTop = l.bar.top + std::round(kIconTop * s);

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
    float slotX = l.itemsLeft;
    for (size_t i = 0; i < items_.size(); ++i) {
        DockItem& item = items_[i];
        const float spacerLeft = slotX - l.bar.left;  // bar-local
        slotX += item.lead;
        item.slotX = slotX;
        slotX += item.slotW;
        item.x = item.slotX + BoxInset(item);
        // Icons stand on the icon row; widgets are centred on it (above any reflections).
        item.y = item.IsWidget() ? std::round(l.bar.top + (iconBarH - item.boxCross) / 2.0f) : l.itemTop;
        if (item.lead <= 0.0f || firstIcon < 0) continue;
        const int k = static_cast<int>(i);
        if (k > lastIcon && l.separatorX < 0.0f) l.separatorX = std::round(spacerLeft + item.lead / 2.0f);
        if (k == firstIcon) l.leftSeparatorX = std::round(spacerLeft + item.lead / 2.0f);
    }
    l.itemHeight = l.bar.bottom - l.itemTop;
    l.magTop = l.itemTop + l.iconPx - l.iconPx * l.magnification;
    l.dotRadius = std::max(1.5f, kDotRadius * s);
    l.dotHeight = static_cast<UINT>(std::ceil(2.0f * l.dotRadius + 2.0f));
    // With reflections the dot sits at the icon's foot, on the reflection.
    const float dotCenter = l.reflH > 0.0f ? l.itemTop + l.iconPx + std::round(kDotFromBottom * s)
                                           : l.bar.bottom - std::round(kDotFromBottom * s);
    l.dotTop = std::round(dotCenter - static_cast<float>(l.dotHeight) / 2.0f);
    l.dotCenterY = dotCenter - l.dotTop;
    // A glow that shows at rest (always / the active app) reaches above the bar: the resting HWND
    // grows by that much so it is not cut off (only that band; the bar stays the active zone).
    l.restTopExtra = (glow == GlowMode::Always || glow == GlowMode::Active)
                         ? std::max(0.0f, l.glowPad - std::round(kIconTop * s))
                         : 0.0f;

    // Background halves: the seam must sit where both halves show plain fill (between the
    // rounded left end / left hairline and the hairline / rounded right end) for every amount of
    // growth.
    l.bgSurfaceW = barW + l.growMax;
    float radius = std::ceil(std::min(l.cornerRadius, barH / 2.0f));
    if (c.theme.shelf) radius = std::max(radius, std::ceil(c.theme.shelfSlant * s));  // the angled ends
    // A spring settle overshoots: returning from growth g, the bar briefly gets narrower than at
    // rest by up to kSpringOvershoot * g per side. Keep the seam that much further in.
    const float undershoot =
        motion_.easing == Easing::Spring ? std::ceil(static_cast<float>(Tween::kSpringOvershoot) * l.growMax) : 0.0f;
    const float stroke = std::max(1.0f, std::round(s));  // the hairlines' width (RenderBackground)
    const float seamMin = std::max(radius, l.leftSeparatorX >= 0.0f ? l.leftSeparatorX + stroke : 0.0f) + 2.0f + undershoot;
    const float seamMax = (l.separatorX >= 0.0f ? l.separatorX - 2.0f : barW - radius - 2.0f) - undershoot;
    l.seamX = l.bar.left + (seamMin <= seamMax ? seamMin : std::floor(barW / 2.0f));

    // Moves the bar (and a glow shown above it at rest) just below the window's bottom edge.
    l.slide = winH - (l.bar.top - l.restTopExtra) + 2.0f;

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
    LONG top = l.window.top + static_cast<LONG>(l.bar.top - l.restTopExtra);
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
    // While a modal dialog owned by the dock is open (an icon picker; it disables the dock), the
    // z-order stays: HWND_TOPMOST with SWP_NOOWNERZORDER would put the dock above its own dialog.
    const UINT keepOrder = IsWindowEnabled(hwnd_) ? 0 : SWP_NOZORDER;
    SetWindowPos(hwnd_, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | keepOrder);
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
    if (!EqualRect(&old.window, &l.window) || old.scale != l.scale || old.bar.left != l.bar.left ||
        old.bar.top != l.bar.top || old.bar.right != l.bar.right || old.bar.bottom != l.bar.bottom ||
        old.itemsLeft != l.itemsLeft || old.iconPx != l.iconPx || old.gap != l.gap || old.growMax != l.growMax ||
        old.itemsW != l.itemsW) {
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
    widgetsGroup_.Reset();
    perspectiveApplied_ = false;
    itemsGroup_.Reset();
    for (Layer& d : dotSources_) d.Reset();
    marker_.Reset();
    markerX_.valid = false;
    label3_ = nullptr;
    labelOpacity_.valid = false;
    label_.Reset();
    for (DockItem& item : items_) {
        item.layer.Reset();
        item.glow.Reset();
        item.reflection.Reset();
        item.icon.Reset();
        item.iconBig.Reset();
        item.dot.Reset();
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
    snapPending_ = true;
    rootY_.valid = false;
    rootOpacity_.valid = false;
    return hr;
}

HRESULT DockWindow::CreateItemVisuals(DockItem& item) {
    item.layer.Reset();
    item.glow.Reset();
    item.reflection.Reset();
    item.icon.Reset();
    item.iconBig.Reset();
    item.dot.Reset();
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
    // The dot goes in front of its icon container: with reflections it sits on the reflection.
    HRESULT hr = renderer_.CreateLayer(item.layer, &itemsGroup_);
    if (SUCCEEDED(hr)) hr = renderer_.EnsurePlacement(item.layer, BoxW(item) / 2.0f, layout_.iconPx);
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
    const UINT w = static_cast<UINT>(l.bgSurfaceW);
    const UINT h = static_cast<UINT>(l.bar.bottom - l.bar.top);
    const ThemeConfig& theme = config_->theme;
    const D2D1_COLOR_F fill = Theme::ToColor(acrylic_ ? theme.acrylicBackground : theme.background);
    const float radius = std::min(l.cornerRadius, static_cast<float>(std::min(w, h)) / 2.0f);
    const float stroke = std::max(1.0f, std::round(l.scale));
    // The hairline before trailing widgets keeps its distance from the right end (it moves with
    // the right half); the one after leading widgets from the left end (left half).
    const float separatorX = l.separatorX >= 0.0f ? l.separatorX + l.growMax : -1.0f;
    const float leftSeparatorX = l.leftSeparatorX;
    const float rowH = static_cast<float>(h) - l.reflH;  // the icon row, above any reflections

    HRESULT hr = renderer_.DrawLayer(bgLeft_, w, h, [&](ID2D1DeviceContext* dc) {
        if (theme.shelf) {
            Theme::DrawShelf(dc, D2D1::RectF(0, 0, static_cast<float>(w), static_cast<float>(h)), theme, l.scale,
                             stroke, separatorX, l.reflH, leftSeparatorX);
            return;
        }
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
    if (weatherCallbacks_.changed) weatherCallbacks_.changed();
}

void DockWindow::ApplyWeather() {
    // The weather item may have been added / removed (Settings > Weather "Show").
    SyncElements();
    if (!config_->Main().Has(ItemKind::Weather)) DismissWeatherPopup(false, true);
    weatherQuiet_ = true;
    weather_.Configure(config_->weather, config_->Main().Has(ItemKind::Weather), hwnd_);
    weatherQuiet_ = false;
    OnWeatherChanged();  // redraw (relayout if the widget's size changed); once, whatever changed
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
    const D2D1_COLOR_F color = Theme::ToColor(config_->theme.separator);
    HRESULT hr = renderer_.DrawLayer(item.icon, w, h, [&](ID2D1DeviceContext* dc) {
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

HRESULT DockWindow::RenderEffects(DockItem& item, const std::function<void(ID2D1DeviceContext*, float)>& drawIcon) {
    // Drawn here, once per icon draw; nothing in them changes with magnification, bounces or
    // drags (the static scale maps the bitmap size to the rest size, the layer transform does the rest).
    const Layout& l = layout_;
    const ThemeConfig& t = config_->theme;
    const UINT bitmapPx = l.bitmapPx;
    const float k = bitmapPx ? l.iconPx / static_cast<float>(bitmapPx) : 1.0f;
    const bool wantGlow = GlowOf(t) != GlowMode::Off && t.glowStrength > 0.0f && bitmapPx;
    const UINT reflH = t.reflection && bitmapPx ? IconEffects::ReflectionHeight(bitmapPx, t.reflectionHeight) : 0;
    winrt::com_ptr<ID2D1CommandList> icon;
    if ((wantGlow || reflH) && renderer_.ResourceContext()) {
        icon = IconEffects::Record(renderer_.ResourceContext(), static_cast<float>(bitmapPx), drawIcon);
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

    if (reflH && icon) {
        const float size = static_cast<float>(bitmapPx);
        hr = renderer_.DrawLayer(item.reflection, bitmapPx, reflH, [&](ID2D1DeviceContext* dc) {
            IconEffects::DrawReflection(dc, icon.get(), size, static_cast<float>(reflH), t.reflectionOpacity);
        });
        // One pixel below the icon's foot, inside the container: it scales with the icon about the
        // foot, so it grows downward while magnified.
        if (SUCCEEDED(hr)) {
            renderer_.SetStaticTransform(item.reflection, D2D1::Matrix3x2F::Scale(k, k) *
                                                              D2D1::Matrix3x2F::Translation(0.0f, l.iconPx + 1.0f));
        }
    } else {
        hr = renderer_.DrawLayer(item.reflection, 0, 0, {});
    }
    return hr;
}

float DockWindow::GlowTarget(const DockItem& item) const {
    if (!item.glow.width) return 0.0f;
    switch (GlowOf(config_->theme)) {
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
    if (GlowOf(config_->theme) == GlowMode::Off) return;
    for (DockItem& item : items_) DriveGlow(item, duration);
}

void DockWindow::ApplyPerspective() {
    // One static 3D effect shared by the background halves' clips, the clock / weather container
    // and the items group (all at the stage origin): a rotation about the X axis through the bar's
    // bottom edge, then a perspective projection centred on the same point. The children's 2D
    // placement animations keep running inside it; nothing is redrawn.
    Layer* targets[] = {&bgLeftClip_, &bgRightClip_, &widgetsGroup_, &itemsGroup_};
    const ThemeConfig& t = config_->theme;
    IDCompositionDesktopDevice* dcomp = renderer_.DComp();
    const bool want = t.perspective && t.perspectiveAngle > 0.0f && dcomp;
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
    const float stageW = static_cast<float>(l.window.right - l.window.left);
    const float cx = stageW / 2.0f;
    const float cy = l.bar.bottom;
    // Viewer distance: one stage width. (Four stage widths, as first planned, made the convergence
    // ~0.5 % at the default 18 degrees: only the vertical squash was visible.)
    const float d = std::max(stageW, 1.0f);
    winrt::com_ptr<IDCompositionRotateTransform3D> rotate;
    winrt::com_ptr<IDCompositionMatrixTransform3D> project;
    winrt::com_ptr<IDCompositionTransform3D> group;
    HRESULT hr = dcomp->CreateRotateTransform3D(rotate.put());
    if (SUCCEEDED(hr)) {
        rotate->SetAxisX(1.0f);
        rotate->SetAxisY(0.0f);
        rotate->SetAxisZ(0.0f);
        // Positive: the top edge recedes (the bar leans back, its top narrower), as measured.
        rotate->SetAngle(t.perspectiveAngle);
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
    const std::wstring text = item.kind == DockItem::Kind::RecycleBin ? RecycleBinLabel()
                              : item.displayName.empty()             ? item.key.Source()
                                                                     : item.displayName;
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
    if (all || bgLeft_.width != bgW || bgLeft_.height != barH || bgSeparators_[0] != layout_.separatorX ||
        bgSeparators_[1] != layout_.leftSeparatorX) {
        hr = RenderBackground();
    }
    if (SUCCEEDED(hr) && (all || dotSources_[0].width != static_cast<UINT>(layout_.iconPx) ||
                          dotSources_[0].height != layout_.dotHeight)) {
        hr = RenderDotSources();
    }
    for (DockItem& item : items_) {
        if (FAILED(hr)) break;
        if (!item.layer.visual) hr = CreateItemVisuals(item);
        if (SUCCEEDED(hr) && (all || item.dirty)) hr = RenderItem(item);
        if (SUCCEEDED(hr) && (all || item.dotDirty) && !item.IsWidget()) hr = RenderDot(item);  // widgets: no dot
    }
    if (SUCCEEDED(hr) && labelShown_ && (all || labelDirty_)) hr = RenderLabel();
    if (FAILED(hr)) return hr;

    if (snap) {
        snapPending_ = false;
        ResetPlacementTweens();
        ApplyPerspective();  // its centre follows the stage size and the bar
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
    preview_.Destroy();  // its composition target belongs to the lost device; recreated on demand
    folder_.Destroy();
    weatherPopup_.Destroy();
    calendar_.Destroy();
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

void DockWindow::DispatchEvent(DockEvent e) {
    for (size_t i = 0; i < elements_.size(); ++i) {
        if (!elements_[i] || !elements_[i]->OnEvent(e)) continue;
        for (DockItem& item : items_) {
            if (item.element == i) item.dirty = true;
        }
    }
    // ApplyConfig / ApplyWeather: their relayout measures and draws the dirty slots.
    if (!hwnd_ || inLayout_ || weatherQuiet_) return;
    // Re-measure the changed widgets: a new size needs a relayout (the bar width depends on it),
    // otherwise only their slots are redrawn.
    const LayoutContext ctx = Context();
    bool resized = false, any = false;
    for (DockItem& item : items_) {
        if (!item.dirty || !item.IsWidget()) continue;
        any = true;
        const ElementSize box = elements_[item.element]->Measure(ctx);
        if (box.main != item.slotW || box.cross != item.boxCross) resized = true;
    }
    if (!any) return;
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
// the cursor on both sides (macOS behaviour), the hovered icon stays under the pointer, and
// hit-testing in rest coordinates always matches what is drawn. Widgets before the first / after
// the last icon move with the bar's left / right end.

void DockWindow::ComputeTargets() {
    const Layout& l = layout_;
    const size_t n = items_.size();
    const float stride = l.iconPx + l.gap;  // an icon's slot
    const float half = std::floor(l.gap / 2.0f);
    const bool magnify = magnifying_ && l.magnification > 1.001f && n > 0;
    const float range = static_cast<float>(config_->Main().magnifyRange) * stride;

    // Dragging an icon: no magnification; the others make room at the insertion slot and the
    // dragged icon follows the cursor. A file dragged over the dock opens a gap (the bar grows by
    // one slot, half on each side). Slots keep their own widths (separators are narrow, widgets
    // their content), so the targets come from walking the new order.
    const int src = dragSource_;
    const bool dragLayout = (dragPhase_ != DragPhase::None || dragLanding_) && src >= 0 &&
                            src < static_cast<int>(n) && items_[static_cast<size_t>(src)].key == dragKey_ &&
                            dragInsert_ >= 0 && dragOrder_.size() == n;
    const bool gapLayout = !dragLayout && dropGap_ >= 0 && n > 0;
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
                    const float winW = static_cast<float>(l.window.right - l.window.left);
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
            // Slots trade places; every place keeps its rest spacer (the drag only reorders pins
            // among pin places and running apps among running places, widgets stay put).
            for (size_t k = 0; k < n; ++k) place(dragOrder_[k], items_[k].lead);
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
        bgLeftTarget_ = l.bar.left - shiftLeft;
        bgRightTarget_ = l.bar.right + shiftRight - l.bgSurfaceW;
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
        delta += l.iconPx * (scale - 1.0f);
    }
    // Width of an item's slot in the magnified layout.
    auto magnifiedW = [&](const DockItem& item) {
        return item.Magnifies() ? l.iconPx * item.targetScale + l.gap : item.slotW;
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
            const float w = l.iconPx * item.targetScale;
            item.targetX = slotLeft + half + w / 2.0f - l.iconPx / 2.0f;
        } else if (item.IsWidget() && k > l.lastMag) {
            item.targetX = item.x + growRight;  // moves with the bar's right end
        } else if (item.IsWidget() && k < l.firstMag) {
            item.targetX = item.x - growLeft;   // moves with the bar's left end
        } else {
            item.targetX = slotLeft;  // separators, widgets between icons
        }
        slotLeft += magnifiedW(item);
    }
    // A file dragged onto an app (open with) or the bin (recycle): that icon grows in place.
    if (dropItem_ >= 0 && dropItem_ < static_cast<int>(n) &&
        (dropMode_ == DropMode::OpenWith || dropMode_ == DropMode::Recycle)) {
        items_[static_cast<size_t>(dropItem_)].targetScale = kDropScale;
    }

    bgLeftTarget_ = l.bar.left - growLeft;
    bgRightTarget_ = l.bar.right + growRight - l.bgSurfaceW;

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
    for (DockItem& item : items_) {
        if (item.IsWidget() && item.layer.visual) {
            // A widget: the visual's own offsets. Its height can change without a snap (the first
            // weather data arriving): follow it here.
            IDCompositionVisual2* visual = item.layer.visual.get();
            if (item.appliedY != item.y) {
                visual->SetOffsetY(item.y);
                item.appliedY = item.y;
            }
            renderer_.Drive(
                item.tx, item.targetX, duration, now, [&](auto v) { return visual->SetOffsetX(v); }, posEpsilon,
                easing);
            continue;
        }
        if (!item.layer.visual || !item.layer.translate || !item.layer.scale) continue;
        if (!item.tx.valid) {
            renderer_.EnsurePlacement(item.layer, BoxW(item) / 2.0f, l.iconPx);
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
        // Centred in the gap (the gap layout starts half a slot left of the rest layout; the gap is
        // an icon slot wide and, before an icon, comes after that icon's spacer: ComputeTargets).
        const float stride = l.iconPx + l.gap;
        float gapLeft = l.itemsLeft - std::floor(stride / 2.0f);
        const size_t gap = static_cast<size_t>(dropGap_);
        for (size_t i = 0; i < items_.size() && i < gap; ++i) gapLeft += items_[i].lead + items_[i].slotW;
        if (gap < items_.size() && !items_[gap].IsWidget()) gapLeft += items_[gap].lead;
        const float x =
            gapLeft + std::floor(l.gap / 2.0f) + std::round((l.iconPx - static_cast<float>(marker_.width)) / 2.0f);
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
    // Hover glow: fades with the label's duration (committed by the caller).
    if (GlowOf(config_->theme) == GlowMode::Hover) UpdateGlows(motion_.labelFade);
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
// screen it holds the dock shown (kHoldPreview) and the name label stays hidden. Moving to another
// multi-window item switches the popup; to a single-window item dismisses it. Leaving both the
// item and the popup starts a kPreviewLeaveMs grace timer. Menus, drags, dock clicks and the dock
// hiding dismiss it. All timers are one-shot; none runs once the popup is hidden.

bool DockWindow::PreviewEligible(int index) const {
    if (!config_->Main().previews || index < 0 || index >= static_cast<int>(items_.size())) return false;
    const DockItem& item = items_[static_cast<size_t>(index)];
    if (item.kind != DockItem::Kind::App || item.windows.size() < 2) return false;
    // Showing counts: the hover usually starts while the dock slides in (OnPreviewTimer requires
    // Shown).
    return (hideState_ == HideState::Shown || hideState_ == HideState::Showing) && !hold_ &&
           dragPhase_ == DragPhase::None && !dropActive_ && !ClickPopupOnScreen();
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
                 std::max<UINT>(USER_TIMER_MINIMUM, static_cast<UINT>(config_->Main().previewDelayMs)), nullptr);
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
    TakePopupHold(kHoldPreview);
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
    // Another popup (or a menu / drag) still holds the dock: it does the leave check when it ends.
    if (!hwnd_ || !leaveCheck || hold_ || popupHolds_ || hideState_ == HideState::Hidden) return;
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

// --- Folder popup ----------------------------------------------------------------------------
// A left click on a folder item toggles FolderPopup (a click popup: it takes the foreground and
// closes itself when it loses it or on Escape) above the icon, anchored like the thumbnail popup.
// While it is open it holds the dock shown (kHoldFolder). The dock's own button-down, a drag, a
// menu, a hide and any relayout dismiss it. Opening an entry from it dismisses it too.

void DockWindow::ToggleFolderPopup(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    const DockItem& item = items_[static_cast<size_t>(index)];
    if (item.kind != DockItem::Kind::Folder) return;
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
    DismissPreview(false, false);
    DismissWeatherPopup(false, false);
    DismissCalendarPopup(false, false);
    const Layout& l = layout_;
    FolderPopup::Style style;
    style.scale = l.scale;
    style.acrylic = acrylic_;
    const ThemeConfig& theme = config_->theme;
    style.tint = acrylic_ ? theme.acrylicBackground : theme.background;
    style.text = theme.labelText;
    style.border = theme.barBorder;
    style.hotBackground = theme.previewHotBackground;
    style.hotBorder = theme.previewHotBorder;
    style.cornerRadiusDip = theme.cornerRadius;
    style.monitor = l.monitor;
    const int anchorX = l.window.left + static_cast<int>(std::lround(anchor));
    const int bottom = l.window.top - static_cast<int>(std::lround(kPreviewGap * l.scale));
    const std::wstring path = !item.targetPath.empty() ? item.targetPath : item.launchPath.value_or(item.key.Source());
    const AppKey key = item.key;
    if (!folder_.Show(path, item.displayName, style, anchorX, bottom)) {
        DismissFolderPopup(false);
        MessageBeep(MB_ICONWARNING);
        return;
    }
    folderKey_ = key;
    TakePopupHold(kHoldFolder);
    HideLabel();
    RenderWithRecovery([] { return S_OK; });  // commit the label hide
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
    const bool present = std::any_of(items_.begin(), items_.end(), [&](const DockItem& item) {
        return item.key == folderKey_ && item.kind == DockItem::Kind::Folder;
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
        case ClickPopup::Folder: return folder_.Visible() && items_[static_cast<size_t>(slot)].key == folderKey_;
        case ClickPopup::Weather: return weatherPopup_.Visible();
        case ClickPopup::Calendar: return calendar_.Visible();
        case ClickPopup::None:
        default: return false;
    }
}

void DockWindow::ShowClickPopup(int slot) {
    if (slot < 0 || slot >= static_cast<int>(items_.size())) return;
    const DockElement& element = ElementOf(static_cast<size_t>(slot));
    const float anchor = element.PopupAnchor(items_[static_cast<size_t>(slot)], Context());
    switch (element.Popup()) {
        case ClickPopup::Folder: ShowFolderPopup(slot, anchor); return;
        case ClickPopup::Weather: ShowWeatherPopup(anchor); return;
        case ClickPopup::Calendar: ShowCalendarPopup(anchor); return;
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

WeatherPopup::Style DockWindow::WeatherPopupStyle() const {
    const Layout& l = layout_;
    const ThemeConfig& theme = config_->theme;
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
    style.monitor = l.monitor;
    style.glyphs.ink = theme.labelText;
    style.glyphs.water = theme.accent;
    return style;
}

void DockWindow::ShowWeatherPopup(float anchor) {
    if (hideState_ == HideState::Hidden) return;
    DismissPreview(false, false);
    DismissFolderPopup(false, false);
    DismissCalendarPopup(false, false);
    const Layout& l = layout_;
    const int anchorX = l.window.left + static_cast<int>(std::lround(anchor));
    const int bottom = l.window.top - static_cast<int>(std::lround(kPreviewGap * l.scale));
    if (!weatherPopup_.Show(WeatherPopupStyle(), anchorX, bottom)) {
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

void DockWindow::ShowCalendarPopup(float anchor) {
    if (hideState_ == HideState::Hidden) return;
    DismissPreview(false, false);
    DismissFolderPopup(false, false);
    DismissWeatherPopup(false, false);
    const Layout& l = layout_;
    const ThemeConfig& theme = config_->theme;
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
    style.monitor = l.monitor;
    const int anchorX = l.window.left + static_cast<int>(std::lround(anchor));
    const int bottom = l.window.top - static_cast<int>(std::lround(kPreviewGap * l.scale));
    if (!calendar_.Show(style, anchorX, bottom)) {
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
    ReleasePopupHold(kHoldCalendar, leaveCheck);
}

void DockWindow::DismissClickPopups(bool animate, bool leaveCheck) {
    DismissFolderPopup(animate, leaveCheck);
    DismissWeatherPopup(animate, leaveCheck);
    DismissCalendarPopup(animate, leaveCheck);
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
    const bool want = config_->theme.acrylic && !config_->theme.shelf && !config_->theme.perspective &&
                      Backdrop::SystemAllowsTransparency();
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
    if (!hwnd_ || !config_->Main().AutoHideOn() || Held() || hidePending_) return;
    // Intelligent: re-test now (cheap; this runs on a mouse leave, not while idle) rather than
    // trusting overlapped_, which may be stale after a suppressed show (see OnOverlapCheck).
    if (config_->Main().autoHide == AutoHideMode::Intelligent) {
        overlapped_ = ForegroundOverlaps();
        if (!overlapped_) return;
    }
    if (hideState_ == HideState::Hidden || hideState_ == HideState::Hiding) return;
    SetTimer(hwnd_, kHideTimerId, std::max<UINT>(USER_TIMER_MINIMUM, static_cast<UINT>(config_->Main().hideDelayMs)),
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
    if (!config_->Main().AutoHideOn() || Held()) return;
    if (config_->Main().autoHide == AutoHideMode::Intelligent) {
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
    if (hideState_ != HideState::Hidden || !config_->Main().AutoHideOn() || !CursorOnStrip()) return;
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
    if (!hwnd_ || config_->Main().autoHide != AutoHideMode::Intelligent) return;
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
    // dialog owned by the dock is open (see ApplyWindowPos).
    if (!hwnd_ || !IsWindowEnabled(hwnd_)) return;
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
    if (config_->Main().autoHide != AutoHideMode::Intelligent) {
        watch_.Stop();  // off / always: no hooks at all
        overlapped_ = false;
    }
    if (!config_->Main().AutoHideOn()) {
        CancelHide();
        if (revealPending_) {
            KillTimer(hwnd_, kRevealTimerId);
            revealPending_ = false;
        }
        if (hideState_ == HideState::Hidden || hideState_ == HideState::Hiding) StartShow();
        return;
    }
    if (config_->Main().autoHide == AutoHideMode::Intelligent) {
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
    const std::vector<DockItemEntry>& entries = config_->Main().items;
    pins.reserve(entries.size());
    size_t separators = 0;
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
        // The icon and kind are part of the id: editing either re-resolves the entry.
        std::wstring id = e.kind + L"|" + e.icon + L"|" + (e.path.empty() ? L"a:" + e.aumid : L"p:" + e.path);
        auto it = pinCache_.find(id);
        AppIdentity::ResolvedPin pin = it != pinCache_.end() ? it->second : AppIdentity::ResolvePin(e);
        cache[std::move(id)] = pin;
        pins.push_back(std::move(pin));
    }
    pins_ = std::move(pins);
    pinCache_ = std::move(cache);  // drops entries that are no longer pinned
}

void DockWindow::BuildElements() {
    // One element per item, in order (an unknown kind gets none and is not shown). The elements
    // hold no visuals: the slots (items_) keep them across a rebuild, matched by key.
    std::vector<std::unique_ptr<DockElement>> elements;
    const std::vector<DockItemEntry>& entries = config_->Main().items;
    elements.reserve(entries.size());
    size_t pinIndex = 0;
    size_t clocks = 0, weathers = 0;
    for (const DockItemEntry& e : entries) {
        const ItemKind kind = e.Kind();
        const size_t ordinal = kind == ItemKind::Clock ? clocks++ : kind == ItemKind::Weather ? weathers++ : 0;
        elements.push_back(MakeElement(*this, e, pinIndex, ordinal));
        if (IsPinKind(kind)) ++pinIndex;
    }
    elements_ = std::move(elements);
}

void DockWindow::SyncElements(bool force) {
    const std::vector<DockItemEntry>& entries = config_->Main().items;
    bool same = !force && entries.size() == elements_.size();
    for (size_t i = 0; same && i < entries.size(); ++i) {
        const DockItemEntry& a = entries[i];
        const DockItemEntry* b = elements_[i] ? &elements_[i]->Entry() : nullptr;
        same = b ? a.kind == b->kind && a.path == b->path && a.aumid == b->aumid && a.icon == b->icon
                 : a.Kind() == ItemKind::Unknown;
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
    if (!same && tracking_) Rebuild(tracker_.Current());
}

void DockWindow::ReloadPins() { SyncElements(true); }

LayoutContext DockWindow::Context() const {
    return LayoutContext{DockAxis::Horizontal, layout_.scale, layout_.iconPx, renderer_.DWrite()};
}

void DockWindow::Rebuild(const WindowTracker::Snapshot& snap) {
    std::vector<AppKey> oldKeys;
    oldKeys.reserve(items_.size());
    for (const DockItem& it : items_) oldKeys.push_back(it.key);

    // 1. Every element's slots, in config order: a pin's slot (entries that resolve to the same
    //    app show once), the unpinned running apps for the `running` element (first-seen order, or
    //    the order the user dragged them into), one slot per widget. Previous slots are reused by
    //    key (visuals, animation state, windows).
    std::vector<DockItem> next;
    next.reserve(pins_.size() + snap.groups.size() + elements_.size());
    SlotBuilder builder(snap, pins_, runningOrder_, items_, next);
    for (size_t i = 0; i < elements_.size(); ++i) {
        if (!elements_[i]) continue;
        builder.SetElement(i);
        elements_[i]->AppendSlots(builder);
    }

    // 2. Windows: a running app's group goes to the app slot with its key (a pinned app or its
    //    running slot; folders, separators, the Recycle Bin and widgets never take windows).
    bool landed = false;  // a launch feedback ended at once (a hop that was already over)
    for (DockItem& item : next) {
        const WindowTracker::Group* g = nullptr;
        if (item.kind == DockItem::Kind::App) {
            for (const WindowTracker::Group& group : snap.groups) {
                if (group.key == item.key) {
                    g = &group;
                    break;
                }
            }
        }
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
        if (builder.Taken(i)) continue;
        renderer_.DestroyLayer(items_[i].dot);
        renderer_.DestroyLayer(items_[i].layer);
        items_[i].glow.Reset();
        items_[i].reflection.Reset();
        items_[i].icon.Reset();
        items_[i].iconBig.Reset();
    }

    bool structural = next.size() != oldKeys.size();
    for (size_t i = 0; !structural && i < next.size(); ++i) structural = next[i].key != oldKeys[i];

    items_ = std::move(next);
    // Active-app glow follows the foreground (committed by the redraw below).
    if (GlowOf(config_->theme) == GlowMode::Active) UpdateGlows(motion_.labelFade);
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
    RefreshFolderPopup();
    UpdateRecycleBinWatch();
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
    if (target.kind == DockItem::Kind::Separator || target.IsWidget()) return true;  // nothing to launch
    // ShellExecuteEx may pump messages (and so rebuild items_): launch from a copy of the
    // launch data, then find the item again by key.
    DockItem copy;
    copy.key = target.key;
    copy.kind = target.kind;
    copy.launchPath = target.launchPath;
    copy.workingDir = target.workingDir;
    const size_t windowsBefore = target.windows.size();
    if (!Launcher::Launch(copy)) {
        MessageBeep(MB_ICONWARNING);
        return false;
    }
    // Folders and the Recycle Bin open in Explorer, whose windows belong to explorer.exe: no
    // window of this item will ever appear, so there is no launch feedback.
    if (copy.kind != DockItem::Kind::App) return true;
    for (DockItem& item : items_) {
        if (item.key != copy.key) continue;
        if (item.windows.size() <= windowsBefore) StartBounce(item, windowsBefore);
        break;
    }
    return true;
}

void DockWindow::ActivateOrLaunch(int slot) {
    if (slot < 0 || slot >= static_cast<int>(items_.size())) return;
    const std::vector<HWND> windows = items_[static_cast<size_t>(slot)].windows;  // copy
    if (windows.empty()) {
        LaunchItem(slot, false);
    } else {
        Activation::ActivateItem(windows);
    }
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
// redrawn). The insertion slot is the rest slot under the cursor (over a widget or a spacer: the
// nearest pin / running app on the cursor's side; widgets are never dragged and never drop
// places); pinned icons stay among the pins, running ones move anywhere (a pinned slot pins them).
// The pins keep their places in the item list (pin k always shows at the place of the k-th pin
// item), so the drag layout shows the pins and the running apps refilled into their places.
// Release: the icon lands in its slot (120 ms) and the new pinned order goes to the config (or,
// among the running apps, to runningOrder_ for this session). The
// layout keeps its geometry, so the landing animation keeps running through the item rebuild.
// Released more than 1.5 bar heights above the bar, a pinned app without windows is unpinned (the
// icon fades out first). Escape (polled: the dock never has keyboard focus) or losing the capture
// cancels: everything slides back.

size_t DockWindow::ConfigIndexOfItem(size_t itemIndex) const {
    const size_t end = config_->Main().PinCount();
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
}

void DockWindow::UpdateDragTarget(float stageX) {
    const size_t n = items_.size();
    const int s = dragSource_;
    dragInsert_ = -1;
    if (s < 0 || static_cast<size_t>(s) >= n) return;
    const size_t src = static_cast<size_t>(s);

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
    if (!item.layer.visual || !Droppable(static_cast<size_t>(s))) return;  // widgets are not dragged (Phase 1c)
    // A launch pulse animates the opacity the drag uses: end it first (it fades back to opaque,
    // which the drag's dimming below replaces).
    if (item.bounce == DockItem::Bounce::Bouncing && item.bounceStyle == LaunchStyle::Pulse) FinishBounce(item);

    dragPhase_ = DragPhase::Dragging;
    dragSource_ = s;
    dragKey_ = item.key;
    dragRemove_ = false;
    // Keep the grab point: the icon box's left edge stays this far left of the cursor.
    const float boxW = BoxW(item);
    const float centre = item.targetX + boxW / 2.0f;
    dragGrabDx_ = boxW / 2.0f + std::clamp(static_cast<float>(pressPt_.x) - centre, -boxW / 2.0f, boxW / 2.0f);
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
    UpdateDragTarget(cursorX_);
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

    // The new pinned order, or the new order of the running apps, if either changed.
    std::vector<AppKey> order;
    std::vector<AppKey> running;
    bool moved = false;
    for (size_t k = 0; k < dragOrder_.size(); ++k) moved = moved || dragOrder_[k] != k;
    if (valid && commit && dragInsert_ >= 0 && moved && dragOrder_.size() == items_.size()) {
        for (const size_t i : dragToPins_ ? dragPins_ : dragRunning_) {
            (dragToPins_ ? order : running).push_back(items_[i].key);
        }
    }
    if (order.empty() && running.empty()) {  // cancelled / unchanged: back to its own slot
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
    dragRemove_ = false;

    KeepStage(motion_.rest + 0.04);  // the window stays at stage size until the icon has landed
    if (!order.empty() && pinRequests_.reorder) pinRequests_.reorder(order);
    if (!running.empty()) {
        runningOrder_ = std::move(running);
        Rebuild(tracker_.Current());
    }
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

DockWindow::DropMode DockWindow::DropModeAt(float stageX, int* item, int* gap) const {
    *item = -1;
    *gap = -1;
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
    }
    if (!dropPinnable_ || items_.empty()) return DropMode::None;
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
    const std::vector<std::wstring> files = DroppedFiles(data);
    dropAccept_ = !files.empty() && dragPhase_ == DragPhase::None;
    dropPinnable_ = dropAccept_ && !PinnableFiles(files).empty();
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
    const DropMode mode =
        shown ? DropModeAt(static_cast<float>(screenPt.x - layout_.window.left), &item, &gap) : DropMode::None;
    SetDropTarget(mode, item, gap);
    switch (mode) {
        case DropMode::Insert:
            return DropEffect(allowed);
        case DropMode::OpenWith:
            return OpenEffect(allowed);
        case DropMode::Recycle:
            return RecycleEffect(allowed);
        default:
            return DROPEFFECT_NONE;
    }
}

void DockWindow::OnDragLeave() {
    if (!dropActive_) return;
    SetDropTarget(DropMode::None, -1, -1);
    dropActive_ = false;
    KeepStage(motion_.magnify + 0.04);
    SetHold(false);
}

DWORD DockWindow::OnDrop(IDataObject* data, POINT screenPt, DWORD allowed) {
    if (!dropActive_) return DROPEFFECT_NONE;
    const std::vector<std::wstring> files = DroppedFiles(data);
    int item = -1, gap = -1;
    const bool shown = hideState_ == HideState::Shown || hideState_ == HideState::Showing;
    const DropMode mode =
        shown ? DropModeAt(static_cast<float>(screenPt.x - layout_.window.left), &item, &gap) : DropMode::None;
    DWORD effect = DROPEFFECT_NONE;
    if (mode == DropMode::Insert) {
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
        if ((mode == DropMode::OpenWith || mode == DropMode::Recycle) && !files.empty() && item >= 0 && hwnd_) {
            dropAction_ = DropAction{mode, items_[static_cast<size_t>(item)].key, files};
            PostMessageW(hwnd_, kDropActionMessage, 0, 0);
            // A recycle is done by us (the files are deleted, not moved by the source): report no
            // effect so the source does not try to delete them itself.
            effect = mode == DropMode::OpenWith ? OpenEffect(allowed) : DROPEFFECT_NONE;
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

int DockWindow::HitTest(POINT pt) const {  // stage coordinates
    // Rest-slot hit-testing inside the active zone. The magnified layout is anchored at the
    // cursor, so the rest slot under the cursor is also the icon drawn under it.
    if (items_.empty() || !InZone(pt)) return -1;
    const float x = static_cast<float>(pt.x);
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

int DockWindow::SlotAt(float x) const {
    if (items_.empty() || x < layout_.itemsLeft) return -1;
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
    POINT pt{layout_.window.left + static_cast<LONG>(item.x + BoxW(item) / 2.0f),
             layout_.window.top + static_cast<LONG>(item.y + BoxCross(item) / 2.0f)};
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
                   (folder_.FadePending() ? 1 : 0) + (weatherPopup_.FadePending() ? 1 : 0) +
                   (calendar_.FadePending() ? 1 : 0) + (binPending_ ? 1 : 0) + (watch_.Pending() ? 1 : 0);
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
            return static_cast<LRESULT>(config_->Main().autoHide);  // 0 off, 1 always, 2 intelligent
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
        case 22:
            return folder_.Visible() ? 1 : 0;
        case 23:
            return static_cast<LRESULT>(folder_.EntryCount());
        case 26:
            return dropActive_ ? static_cast<LRESULT>(dropMode_) : 0;
        case 27:
            return static_cast<LRESULT>(weather_.State());
        case 28: {
            const WeatherService::Forecast& data = weather_.Data();
            if (!weather_.Active() || !data.valid || !std::isfinite(data.temp)) return INT_MIN;
            return static_cast<LRESULT>(std::lround(data.temp * 10.0));
        }
        case 29:
            return weatherPopup_.Visible() ? 1 : 0;
        case 30:
            return weather_.Active() ? static_cast<LRESULT>(weather_.Age()) : -1;
        case 31:
            return calendar_.Visible() ? 1 : 0;
        case 32: {
            LRESULT mask = 0;
            const bool glow = std::any_of(items_.begin(), items_.end(), [](const DockItem& i) { return i.glow.width > 0; });
            const bool reflection =
                std::any_of(items_.begin(), items_.end(), [](const DockItem& i) { return i.reflection.width > 0; });
            if (glow && GlowOf(config_->theme) != GlowMode::Off) mask |= 1;
            if (reflection && layout_.reflH > 0.0f) mask |= 2;
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
    if (weather_.HandleMessage(msg, wp, lp)) return 0;  // fetch results, the refresh timer
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
            if (pressed_ >= 0 && Droppable(static_cast<size_t>(pressed_)) && (wp & MK_LBUTTON) &&
                hideState_ != HideState::Hidden &&
                (std::abs(pt.x - pressPt_.x) > GetSystemMetricsForDpi(SM_CXDRAG, dpi_) ||
                 std::abs(pt.y - pressPt_.y) > GetSystemMetricsForDpi(SM_CYDRAG, dpi_))) {
                BeginDrag(pt);
                if (dragPhase_ == DragPhase::Dragging) return 0;
            }
            if (hideState_ == HideState::Hidden) {
                // On the reveal strip.
                if (revealArmed_ && config_->Main().AutoHideOn() && !revealPending_) {
                    SetTimer(hwnd_, kRevealTimerId,
                             std::max<UINT>(USER_TIMER_MINIMUM, static_cast<UINT>(config_->Main().revealDelayMs)), nullptr);
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
            {
                const int hit = HitTest(pt);  // a widget is not hovered: no label, no preview
                SetHover(hit >= 0 && items_[static_cast<size_t>(hit)].IsWidget() ? -1 : hit);
            }
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
            // A press on the dock closes the click popup; on the slot whose popup it is (a folder,
            // the clock, the weather) the release then does not open it again (the click toggles).
            pressClosedPopup_ = pressed_ >= 0 && ElementOf(static_cast<size_t>(pressed_)).OnPress(pressed_);
            if (ClickPopupOnScreen()) DismissClickPopups(true, false);
            return 0;

        case WM_LBUTTONUP: {
            if (dragPhase_ == DragPhase::Dragging) {
                EndDrag(true);
                return 0;
            }
            const POINT upPt = ToStage(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            const int index = HitTest(upPt);
            const bool clicked = index >= 0 && index == pressed_;
            pressed_ = -1;
            const bool pressClosed = pressClosedPopup_;
            pressClosedPopup_ = false;
            if (clicked) ElementOf(static_cast<size_t>(index)).OnClick(index, pressClosed);
            return 0;
        }

        case WM_MBUTTONDOWN:
            SuppressPreview();
            if (ClickPopupOnScreen()) DismissClickPopups(true, false);
            pressedMiddle_ = HitTest(ToStage(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}));
            return 0;

        case WM_MBUTTONUP: {
            const int index = HitTest(ToStage(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}));
            const bool clicked = index >= 0 && index == pressedMiddle_;
            pressedMiddle_ = -1;
            if (clicked) ElementOf(static_cast<size_t>(index)).OnMiddleClick(index);  // apps: a new instance
            return 0;
        }

        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL: {
            // Routed to the element under the pointer (none consumes it yet: Phase 0 / widgets).
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};  // screen coordinates
            ScreenToClient(hwnd_, &pt);
            const int index = HitTest(ToStage(pt));
            if (index >= 0 && ElementOf(static_cast<size_t>(index))
                                  .OnWheel(index, GET_WHEEL_DELTA_WPARAM(wp), msg == WM_MOUSEHWHEEL)) {
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
            Relayout();
            return 0;

        case WM_DISPLAYCHANGE:
            DismissPreview(false, false);
            DismissClickPopups(false, false);
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
            weather_.Stop();  // a fetch still in flight posts to a window that is gone (and frees it)
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
            acrylic_ = false;
            backdrop_.Destroy();
            labelFormat_ = nullptr;
            labelEllipsis_ = nullptr;
            ResetLayers();
            items_.clear();
            elements_.clear();  // their text layouts (the clock's, the weather's)
            icons_.Shutdown();
            renderer_.Shutdown();
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
