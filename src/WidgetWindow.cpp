#include "WidgetWindow.h"

#include <dwmapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cwctype>

#include "DarkMode.h"
#include "DesktopLayer.h"
#include "DockWindow.h"
#include "JsonUtil.h"
#include "Launcher.h"
#include "ShellFiles.h"
#include "SnapLines.h"  // round 14 phase 14B
#include "Theme.h"
#include "WidgetRuntime.h"

namespace {

// Layout and behaviour (DIPs).
constexpr float kGrabEdge = 8.0f;     // the resize band inside the edges
constexpr float kGrabCorner = 16.0f;  // a corner: this far along both edges
constexpr float kSnapDip = 8.0f;
constexpr float kPopupGap = 8.0f;     // window -> popup
constexpr float kKeepReachable = 32.0f;
constexpr int kFlashSteps = 6;
constexpr UINT kFlashMs = 250;

bool SystemReducedMotion() {
    BOOL animations = TRUE;
    if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0)) return false;
    return !animations;
}

}  // namespace

// --- lifetime ------------------------------------------------------------------------------------

HRESULT WidgetWindow::Create(HINSTANCE instance, Config* config, size_t cfg, HMONITOR monitor, Host host) {
    instance_ = instance;
    config_ = config;
    cfg_ = cfg;
    monitor_ = monitor;
    host_ = std::move(host);
    if (!host_.renderer || !host_.shared || !Valid()) return E_INVALIDARG;

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &WidgetWindow::WndProcThunk;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return HRESULT_FROM_WIN32(GetLastError());

    UINT dx = 96, dy = 96;
    if (monitor_ && SUCCEEDED(GetDpiForMonitor(monitor_, MDT_EFFECTIVE_DPI, &dx, &dy))) dpi_ = dx;
    scale_ = static_cast<float>(dpi_) / 96.0f;
    applied_ = Cfg();
    keepRatio_ = static_cast<float>(Cfg().widgetWindow.w) / static_cast<float>(std::max(1, Cfg().widgetWindow.h));
    rect_ = RectFromConfig();
    const DockItemEntry* item = Cfg().WidgetItem();
    const std::wstring title = L"WinDock widget: " + (item ? Widgets::DisplayName(item->id) : std::wstring(L"(none)"));
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, kClassName, title.c_str(), WS_POPUP, rect_.left,
                            rect_.top, rect_.right - rect_.left, rect_.bottom - rect_.top, nullptr, nullptr, instance_, this);
    if (!hwnd_) return HRESULT_FROM_WIN32(GetLastError());
    if (DarkMode::DarkMenusOn()) DarkMode::AllowDarkForWindow(hwnd_, true);  // round 13 phase 13C: its menu
    if (const UINT d = GetDpiForWindow(hwnd_); d && d != dpi_) {
        dpi_ = d;
        scale_ = static_cast<float>(dpi_) / 96.0f;
        rect_ = RectFromConfig();
        SetWindowPos(hwnd_, nullptr, rect_.left, rect_.top, rect_.right - rect_.left, rect_.bottom - rect_.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    const BOOL off = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof(off));

    const HRESULT hr = EnsureTarget();
    if (FAILED(hr)) {
        Destroy();
        return hr;
    }
    // The popups (on the shared device; their windows are made on their first Show).
    popup_.Init(instance_, host_.renderer,
                ScriptPopup::Callbacks{
                    [this](ID2D1DeviceContext* dc, float w, float h, float scale) {
                        if (widget_) widget_->RenderPopup(dc, w, h, scale);
                    },
                    [this](float x, float y, int button) {
                        if (widget_) widget_->PopupClick(x, y, button);
                    },
                    [this] {
                        if (widget_) widget_->PopupClosed();  // closed by itself (Escape, deactivated)
                    },
                });
    passive_.SetMouseTransparent(true);
    passive_.Init(instance_, host_.renderer,
                  ScriptPopup::Callbacks{
                      [this](ID2D1DeviceContext* dc, float w, float h, float scale) {
                          if (widget_) widget_->RenderPopup(dc, w, h, scale);
                      },
                      {},
                      {},
                  });
    folderPopup_.Init(instance_, host_.renderer, &icons_, FolderPopup::Callbacks{[this] { folderPopup_.Hide(true); }, [] {}});
    calendar_.Init(instance_, host_.renderer,
                   CalendarPopup::Callbacks{
                       [] {},
                       [this] {
                           const std::wstring target = config_->clock.clickTarget;  // a widget's: the defaults' target
                           calendar_.Hide(true);
                           if (!Launcher::OpenTarget(target)) MessageBeep(MB_ICONWARNING);
                       },
                       [this](const std::wstring& url) {
                           const std::wstring target = url;
                           calendar_.Hide(true);
                           if (!Launcher::OpenTarget(target)) MessageBeep(MB_ICONWARNING);
                       },
                   });
    centre_.Init(instance_, host_.renderer,
                 NotificationPopup::Callbacks{
                     [this] {  // closed by itself
                         if (centreOpen_ && host_.shared) host_.shared->SetCentreOpen(false);
                         centreOpen_ = false;
                     },
                     [this](uint32_t id) {
                         DismissCentre(true);
                         if (!host_.shared) return;
                         host_.shared->ActivateNotification(id);
                         host_.shared->Notifications().Dismiss(id);
                     },
                     [this](uint32_t id) {
                         if (host_.shared) host_.shared->Notifications().Dismiss(id);
                     },
                     [this] {
                         if (host_.shared) host_.shared->Notifications().DismissAll();
                     },
                     [this] {
                         DismissCentre(true);
                         Launcher::OpenTarget(L"ms-settings:privacy-notifications");
                     },
                     [](const NotificationItem&, UINT) -> ID2D1Bitmap* { return nullptr; },  // no icon cache here
                 });
    popupsReady_ = true;

    // The layer first (a "behind" window is registered before it shows: no flash above the apps).
    hidden_ = WantHidden();
    MakeWidget();
    UpdateBackdrop();
    SyncLayer();
    Redraw(true);
    RegisterDrop();
    if (!hidden_) {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        AssertZOrder();
    }
    return S_OK;
}

void WidgetWindow::Destroy() {
    if (flashSteps_ > 0 && hwnd_) KillTimer(hwnd_, kFlashTimer);
    flashSteps_ = 0;
    if (popupsReady_) {
        if (centreOpen_ && host_.shared) host_.shared->SetCentreOpen(false);
        centreOpen_ = false;
        popup_.Destroy();
        passive_.Destroy();
        folderPopup_.Destroy();
        calendar_.Destroy();
        centre_.Destroy();
    }
    // The widget goes before the window (its last callbacks may still post to it: harmless).
    widget_.reset();
    if (dropTarget_) {
        if (hwnd_) RevokeDragDrop(hwnd_);
        dropTarget_->Detach();
        dropTarget_->Release();
        dropTarget_ = nullptr;
    }
    if (hwnd_ && registered_) DesktopLayer::Unregister(hwnd_);
    registered_ = false;
    panel_.Reset();
    content_.Reset();
    flash_.Reset();
    root_.Reset();
    if (target_) target_->SetRoot(nullptr);
    target_ = nullptr;
    icons_.Shutdown();
    backdrop_.Destroy();
    acrylic_ = false;
    if (hwnd_) {
        HWND h = hwnd_;
        hwnd_ = nullptr;
        DestroyWindow(h);
    }
}

void WidgetWindow::ReleaseDevice() {
    DismissPopups(false);
    DismissPassive(false);
    DismissCentre(false);
    popup_.Destroy();
    passive_.Destroy();
    folderPopup_.Destroy();
    calendar_.Destroy();
    centre_.Destroy();
    icons_.Clear();
    panel_.Reset();
    content_.Reset();
    flash_.Reset();
    root_.Reset();
    if (target_) target_->SetRoot(nullptr);
    target_ = nullptr;
}

HRESULT WidgetWindow::RestoreDevice() {
    if (!hwnd_) return E_UNEXPECTED;
    const HRESULT hr = EnsureTarget();
    if (SUCCEEDED(hr)) Redraw(true);
    return hr;
}

HRESULT WidgetWindow::EnsureTarget() {
    if (target_) return S_OK;
    HRESULT hr = host_.renderer->CreateWindowTarget(hwnd_, target_, root_);
    if (SUCCEEDED(hr)) hr = host_.renderer->CreateLayer(panel_, &root_);
    if (SUCCEEDED(hr)) hr = host_.renderer->CreateLayer(content_, &root_);
    if (SUCCEEDED(hr)) hr = host_.renderer->CreateLayer(flash_, &root_);
    if (FAILED(hr)) {
        target_ = nullptr;
        root_.Reset();
        panel_.Reset();
        content_.Reset();
        flash_.Reset();
    }
    return hr;
}

// --- the widget ----------------------------------------------------------------------------------

std::wstring WidgetWindow::WidgetKey() const {
    const DockItemEntry* e = Cfg().WidgetItem();
    return (e ? e->id : std::wstring()) + L"@" + Cfg().id;
}

ScriptDockInfo WidgetWindow::DockInfo() const {
    const DockConfig& d = Cfg();
    const ThemeConfig& t = ThemeCfg();
    ScriptDockInfo info;
    info.axis = "horizontal";
    info.edge = "bottom";
    info.dockId = JsonUtil::WideToUtf8(d.id);
    info.locked = d.locked;
    const ReduceMotion rm = config_->animation.reduceMotion;
    info.reduceMotion = rm == ReduceMotion::On || (rm == ReduceMotion::System && SystemReducedMotion());
    info.metric = config_->weather.units == Units::Metric;
    info.clockDate = false;
    info.text = t.clockText;
    info.dateText = t.clockDateText;
    info.accent = t.accent;
    info.background = t.background;
    info.labelBackground = t.labelBackground;
    info.labelText = t.labelText;
    info.separator = t.separator;
    info.border = t.barBorder;
    info.standalone = true;
    info.scale = scale_;
    const float w = static_cast<float>(rect_.right - rect_.left) / scale_, h = static_cast<float>(rect_.bottom - rect_.top) / scale_;
    info.width = info.main = w;
    info.height = info.cross = h;
    info.iconSize = std::min(w, h) / 1.5f;
    info.visible = !hidden_;
    return info;
}

void WidgetWindow::MakeWidget() {
    widget_.reset();
    const DockItemEntry* e = Cfg().WidgetItem();
    if (!e || !host_.renderer->D2DFactory() || !host_.renderer->DWrite()) return;
    widgetKey_ = WidgetKey();
    const WeatherConfig place = ScriptWeatherOf(*config_, Cfg(), *e);
    widget_ = std::make_unique<WidgetInstance>(host_.shared->Widgets(), static_cast<WidgetInstance::Client&>(*this),
                                               host_.renderer->D2DFactory(), host_.renderer->DWrite(), e->id, widgetKey_,
                                               e->settings, DockInfo(), place);
    widget_->SetStandaloneBox(static_cast<float>(rect_.right - rect_.left) / scale_,
                              static_cast<float>(rect_.bottom - rect_.top) / scale_);
}

bool WidgetWindow::ReloadWidget() {
    if (!widget_) return false;
    widget_->Reload();
    return true;
}

void WidgetWindow::TimeChanged() {
    if (widget_) widget_->TimeChanged();
}

void WidgetWindow::PointDips(POINT client, float& x, float& y) const {
    x = static_cast<float>(client.x) / scale_;
    y = static_cast<float>(client.y) / scale_;
}

void WidgetWindow::WidgetChanged(WidgetInstance&) {
    if (redrawPosted_ || !hwnd_) return;
    redrawPosted_ = PostMessageW(hwnd_, kRedrawMessage, 0, 0) != FALSE;
}

void WidgetWindow::WidgetPopupRequest(WidgetInstance&) {
    if (popupPosted_ || !hwnd_) return;
    popupPosted_ = PostMessageW(hwnd_, kPopupMessage, 0, 0) != FALSE;
}

bool WidgetWindow::WidgetPopupShown(const WidgetInstance&) const {
    return popup_.Visible() || calendar_.Visible() || folderPopup_.Visible();
}

bool WidgetWindow::WidgetPassiveShown(const WidgetInstance&) const { return passive_.OnScreen(); }

bool WidgetWindow::WidgetActivateNotification(uint32_t id) { return host_.shared && host_.shared->ActivateNotification(id); }

bool WidgetWindow::WidgetOpenCentre(WidgetInstance&) {
    if (!host_.shared || hidden_) return false;
    if (centre_.Visible()) {
        DismissCentre(true);
    } else {
        ShowCentre();
    }
    return true;
}

WidgetInstance::Client::EditorPlace WidgetWindow::WidgetEditorPlace(const WidgetInstance&) const {
    EditorPlace p;
    if (!hwnd_ || hidden_) return p;
    p.owner = hwnd_;
    p.box = rect_;
    p.topmost = !registered_ && !DesktopLayered();  // "normal", or lifted by the dashboard
    p.side = 0;                                     // above it (below without room)
    return p;
}

// --- placement -----------------------------------------------------------------------------------

RECT WidgetWindow::WorkArea() const {
    MONITORINFO mi{sizeof(mi)};
    if (monitor_ && GetMonitorInfoW(monitor_, &mi)) return mi.rcWork;
    if (GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi)) return mi.rcWork;
    return RECT{0, 0, 1024, 768};
}

RECT WidgetWindow::RectFromConfig() const {
    const DockConfig& d = Cfg();
    const RECT work = WorkArea();
    const LONG workW = work.right - work.left, workH = work.bottom - work.top;
    const LONG w = std::max<LONG>(1, static_cast<LONG>(Px(static_cast<float>(d.widgetWindow.w))));
    const LONG h = std::max<LONG>(1, static_cast<LONG>(Px(static_cast<float>(d.widgetWindow.h))));
    LONG x = d.x == DockConfig::kCentred ? work.left + (workW - w) / 2 : work.left + static_cast<LONG>(Px(static_cast<float>(d.x)));
    LONG y = d.y == DockConfig::kCentred ? work.top + (workH - h) / 2 : work.top + static_cast<LONG>(Px(static_cast<float>(d.y)));
    // Kept reachable: part of it on the work area.
    const LONG keep = std::min<LONG>(static_cast<LONG>(Px(kKeepReachable)), std::min(w, h));
    x = std::clamp(x, work.left - w + keep, std::max(work.left - w + keep, work.right - keep));
    y = std::clamp(y, work.top - h + keep, std::max(work.top - h + keep, work.bottom - keep));
    return RECT{x, y, x + w, y + h};
}

void WidgetWindow::StoreRect() {
    DockConfig& d = Cfg();
    const RECT work = WorkArea();
    d.x = static_cast<int>(std::lround(static_cast<float>(rect_.left - work.left) / scale_));
    d.y = static_cast<int>(std::lround(static_cast<float>(rect_.top - work.top) / scale_));
    d.widgetWindow.w = std::clamp(static_cast<int>(std::lround(static_cast<float>(rect_.right - rect_.left) / scale_)),
                                  WidgetWindowConfig::kMinSize, WidgetWindowConfig::kMaxSize);
    d.widgetWindow.h = std::clamp(static_cast<int>(std::lround(static_cast<float>(rect_.bottom - rect_.top) / scale_)),
                                  WidgetWindowConfig::kMinSize, WidgetWindowConfig::kMaxSize);
    applied_ = d;
}

void WidgetWindow::Place(const RECT& r) {
    const bool resized = r.right - r.left != rect_.right - rect_.left || r.bottom - r.top != rect_.bottom - rect_.top;
    const bool moved = !EqualRect(&r, &rect_);
    rect_ = r;
    if (!hwnd_) return;
    SetWindowPos(hwnd_, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    if (acrylic_ && !hidden_) backdrop_.Place(hwnd_, rect_);
    if (resized) Redraw(true);  // the widget's box is the window: measured and drawn again
    // Round 12 phase 12I: a placement by the program; a drag's steps wait for its end (WM_LBUTTONUP).
    if (moved && grab_ == Grab::None && host_.moved) host_.moved();
}

void WidgetWindow::SetMonitor(HMONITOR monitor) {
    if (!hwnd_) return;
    if (monitor != monitor_) {
        monitor_ = monitor;
        UINT dx = 96, dy = 96;
        if (monitor_ && SUCCEEDED(GetDpiForMonitor(monitor_, MDT_EFFECTIVE_DPI, &dx, &dy)) && dx != dpi_) {
            dpi_ = dx;
            scale_ = static_cast<float>(dpi_) / 96.0f;
        }
    }
    const RECT r = RectFromConfig();
    if (!EqualRect(&r, &rect_)) Place(r);
}

WidgetWindow::Grab WidgetWindow::GrabAt(POINT pt) const {
    if (!Valid() || Cfg().locked) return Grab::None;  // locked: neither moved nor resized
    const LONG w = rect_.right - rect_.left, h = rect_.bottom - rect_.top;
    const LONG e = static_cast<LONG>(Px(kGrabEdge)), c = static_cast<LONG>(Px(kGrabCorner));
    const bool left = pt.x < e, right = pt.x >= w - e, top = pt.y < e, bottom = pt.y >= h - e;
    const bool nearL = pt.x < c, nearR = pt.x >= w - c, nearT = pt.y < c, nearB = pt.y >= h - c;
    if ((top && nearL) || (left && nearT)) return Grab::TopLeft;
    if ((top && nearR) || (right && nearT)) return Grab::TopRight;
    if ((bottom && nearL) || (left && nearB)) return Grab::BottomLeft;
    if ((bottom && nearR) || (right && nearB)) return Grab::BottomRight;
    if (top) return Grab::Top;
    if (bottom) return Grab::Bottom;
    if (left) return Grab::Left;
    if (right) return Grab::Right;
    return Grab::None;
}

void WidgetWindow::Snap(RECT& r, Grab g) const {
    // Round 14 phase 14B: `settings.snapToScreen` off: no snapping at all (moves and resizes, as
    // the switch is one for every snapping surface).
    if (!config_ || !config_->settings.snapToScreen) return;
    const RECT work = WorkArea();
    const LONG thr = static_cast<LONG>(Px(kSnapDip));
    std::vector<LONG> xs{work.left, work.right}, ys{work.top, work.bottom};
    if (host_.neighbours) {
        for (const RECT& n : host_.neighbours(this)) {
            xs.push_back(n.left);
            xs.push_back(n.right);
            ys.push_back(n.top);
            ys.push_back(n.bottom);
        }
    }
    if (g == Grab::Move) {
        // Round 14 phase 14B: a move also snaps the window's centre to the work area's centre lines
        // (resizes snap their edges only).
        OffsetRect(&r, SnapLines::Best(r.left, r.right, xs, SnapLines::CentreX(work), thr),
                   SnapLines::Best(r.top, r.bottom, ys, SnapLines::CentreY(work), thr));
        return;
    }
    auto snap = [&](LONG& v, const std::vector<LONG>& cs) {
        LONG dist = thr + 1, to = v;
        for (LONG c : cs) {
            if (std::labs(c - v) < dist) {
                dist = std::labs(c - v);
                to = c;
            }
        }
        if (dist <= thr) v = to;
    };
    if (g == Grab::Left || g == Grab::TopLeft || g == Grab::BottomLeft) snap(r.left, xs);
    if (g == Grab::Right || g == Grab::TopRight || g == Grab::BottomRight) snap(r.right, xs);
    if (g == Grab::Top || g == Grab::TopLeft || g == Grab::TopRight) snap(r.top, ys);
    if (g == Grab::Bottom || g == Grab::BottomLeft || g == Grab::BottomRight) snap(r.bottom, ys);
}

float WidgetWindow::MinWDip() const {
    const Widgets::Package* p = widget_ ? widget_->Package() : nullptr;
    return std::max(static_cast<float>(WidgetWindowConfig::kMinSize), p ? p->manifest.standalone.minWidth : 0.0f);
}

float WidgetWindow::MinHDip() const {
    const Widgets::Package* p = widget_ ? widget_->Package() : nullptr;
    return std::max(static_cast<float>(WidgetWindowConfig::kMinSize), p ? p->manifest.standalone.minHeight : 0.0f);
}

void WidgetWindow::Constrain(RECT& r, Grab g) const {
    const bool movesLeft = g == Grab::Left || g == Grab::TopLeft || g == Grab::BottomLeft;
    const bool movesTop = g == Grab::Top || g == Grab::TopLeft || g == Grab::TopRight;
    const bool horizontal = g != Grab::Top && g != Grab::Bottom;  // the width is what the pointer sets
    const bool vertical = g != Grab::Left && g != Grab::Right;
    LONG w = r.right - r.left, h = r.bottom - r.top;
    const LONG minW = static_cast<LONG>(Px(MinWDip())), minH = static_cast<LONG>(Px(MinHDip()));
    const LONG maxPx = static_cast<LONG>(Px(static_cast<float>(WidgetWindowConfig::kMaxSize)));
    w = std::clamp(w, minW, std::max(minW, maxPx));
    h = std::clamp(h, minH, std::max(minH, maxPx));
    // The aspect: "keep" (the shape at creation) or a fixed w / h.
    float ratio = 0.0f;
    if (const Widgets::Package* p = widget_ ? widget_->Package() : nullptr) {
        if (p->manifest.standalone.aspect == Widgets::StandaloneHint::Aspect::Ratio) ratio = p->manifest.standalone.ratio;
        if (p->manifest.standalone.aspect == Widgets::StandaloneHint::Aspect::Keep) ratio = keepRatio_;
    }
    if (ratio > 0.0f) {
        if (horizontal && vertical) {
            // A corner: the larger relative change wins.
            const float fw = static_cast<float>(w) / std::max(1.0f, static_cast<float>(grabRect_.right - grabRect_.left));
            const float fh = static_cast<float>(h) / std::max(1.0f, static_cast<float>(grabRect_.bottom - grabRect_.top));
            if (fw >= fh) {
                h = static_cast<LONG>(std::lround(static_cast<float>(w) / ratio));
            } else {
                w = static_cast<LONG>(std::lround(static_cast<float>(h) * ratio));
            }
        } else if (horizontal) {
            h = static_cast<LONG>(std::lround(static_cast<float>(w) / ratio));
        } else {
            w = static_cast<LONG>(std::lround(static_cast<float>(h) * ratio));
        }
        // Back over the minimum (in the ratio).
        if (w < minW) {
            w = minW;
            h = static_cast<LONG>(std::lround(static_cast<float>(w) / ratio));
        }
        if (h < minH) {
            h = minH;
            w = static_cast<LONG>(std::lround(static_cast<float>(h) * ratio));
        }
    }
    // The fixed edges stay where they were.
    if (movesLeft) {
        r.left = r.right - w;
    } else {
        r.right = r.left + w;
    }
    if (movesTop) {
        r.top = r.bottom - h;
    } else {
        r.bottom = r.top + h;
    }
}

// --- the layers ----------------------------------------------------------------------------------

bool WidgetWindow::InDashboard() const {
    return Valid() && Cfg().layer != DockLayer::Normal && host_.shared && host_.shared->DashboardOn();
}

bool WidgetWindow::WantsWatch() const { return hwnd_ && Valid() && Cfg().layer == DockLayer::Desktop; }

void WidgetWindow::SyncLayer() {
    if (!hwnd_) return;
    if (BehindLayered()) {
        if (!registered_) {
            // Out of the topmost band first (the layer then places it after another app's window).
            SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            registered_ = true;
        }
        DesktopLayer::Register(hwnd_, acrylic_ && !hidden_ ? backdrop_.Hwnd() : nullptr);
        return;
    }
    if (registered_) {
        DesktopLayer::Unregister(hwnd_);
        registered_ = false;
    }
}

void WidgetWindow::AssertZOrder() {
    if (!hwnd_ || hidden_) return;
    constexpr UINT kFlags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    if (registered_) {
        DesktopLayer::Place(hwnd_);
    } else if (DesktopLayered()) {
        SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0, kFlags);
        DesktopLayer::PlaceAboveDesktop(hwnd_);
    } else if (!widget_ || !widget_->EditorOpen()) {
        // "normal", or lifted by the dashboard. Not while its text editor is open: the window would
        // go above its own editor (HWND_TOPMOST with SWP_NOOWNERZORDER).
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, kFlags);
    }
    if (acrylic_) backdrop_.Place(hwnd_, rect_);
}

bool WidgetWindow::WantHidden() const {
    return fullscreen_ || (DesktopLayered() && !DesktopLayer::DesktopForeground());
}

bool WidgetWindow::TopmostBand() const { return hwnd_ && Valid() && (Cfg().layer == DockLayer::Normal || InDashboard()); }

void WidgetWindow::SetFullscreenHidden(bool hidden) {
    if (!hwnd_ || hidden == fullscreen_) return;
    fullscreen_ = hidden;
    UpdateShown();  // hides (popups closed, no z-order call) / shows (topmost once: AssertZOrder)
}

void WidgetWindow::UpdateShown() {
    if (!hwnd_) return;
    const bool hide = WantHidden();
    if (hide == hidden_) return;
    hidden_ = hide;
    if (hide) {
        // Another window came to the foreground: the widget leaves with its popups.
        DismissPopups(false);
        DismissPassive(false);
        DismissCentre(false);
        if (grab_ != Grab::None && GetCapture() == hwnd_) ReleaseCapture();
        ShowWindow(hwnd_, SW_HIDE);
        if (acrylic_) backdrop_.Hide();
    } else {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        AssertZOrder();
    }
    if (widget_) widget_->SetVisible(!hidden_);
    // Round 12 phase 12I: hidden (the "desktop" rule, the full-screen guard): its settings panel
    // closes, as a parked dock's does (App checks Shown()).
    if (host_.moved) host_.moved();
}

void WidgetWindow::OnWatchEvent() {
    ++watchEvents_;
    if (Valid() && Cfg().layer == DockLayer::Desktop) UpdateShown();
}

void WidgetWindow::OnDashboardChanged() {
    if (!hwnd_ || !Valid() || Cfg().layer == DockLayer::Normal) return;
    SyncLayer();     // a "behind" window leaves the layer while lifted (its z-order lock would drop the raise)
    UpdateShown();   // a "desktop" one: shown while lifted, its foreground rule again after
    AssertZOrder();  // topmost while lifted; back above the desktop window after
}

void WidgetWindow::ReassertLayer() {
    if (!hwnd_ || hidden_) return;
    if (DesktopLayered()) DesktopLayer::PlaceAboveDesktop(hwnd_);  // registered ones: DesktopLayer::Reassert
}

// --- config --------------------------------------------------------------------------------------

void WidgetWindow::ApplyConfig() {
    if (!hwnd_ || !Valid()) return;
    const DockConfig& d = Cfg();
    const DockItemEntry* now = d.WidgetItem();
    const DockItemEntry* was = applied_.WidgetItem();
    const bool widgetChanged = !now || !was || now->id != was->id || d.id != applied_.id;
    const bool rectChanged = d.x != applied_.x || d.y != applied_.y || d.widgetWindow.w != applied_.widgetWindow.w ||
                             d.widgetWindow.h != applied_.widgetWindow.h || d.monitor != applied_.monitor;
    const bool layerChanged = d.layer != applied_.layer;
    const bool panelChanged = d.widgetWindow.background != applied_.widgetWindow.background;
    if (rectChanged && (d.widgetWindow.w != applied_.widgetWindow.w || d.widgetWindow.h != applied_.widgetWindow.h)) {
        keepRatio_ = static_cast<float>(d.widgetWindow.w) / static_cast<float>(std::max(1, d.widgetWindow.h));
    }
    applied_ = d;
    if (widgetChanged) {
        DismissPopups(false);
        DismissPassive(false);
        MakeWidget();
        const std::wstring title = L"WinDock widget: " + (now ? Widgets::DisplayName(now->id) : std::wstring(L"(none)"));
        SetWindowTextW(hwnd_, title.c_str());
    } else if (widget_ && now) {
        widget_->SetWeatherConfig(ScriptWeatherOf(*config_, d, *now));
        widget_->SetSettings(now->settings);  // onSettingsChanged + a redraw when they differ
        widget_->SetDock(DockInfo());          // locked, theme, reduce motion
    }
    if (rectChanged) {
        const RECT r = RectFromConfig();
        const bool moved = !EqualRect(&r, &rect_);
        rect_ = r;
        SetWindowPos(hwnd_, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        // Round 12 phase 12I: its Placement tab's X / Y / size moved it: the panel follows.
        if (moved && host_.moved) host_.moved();
    }
    UpdateBackdrop();  // the theme or the background may have changed
    if (layerChanged) {
        SyncLayer();
        const bool hide = WantHidden();
        if (hide != hidden_) {
            UpdateShown();
        } else {
            AssertZOrder();
        }
    } else if (panelChanged || rectChanged) {
        if (registered_) DesktopLayer::SetUnder(hwnd_, acrylic_ && !hidden_ ? backdrop_.Hwnd() : nullptr);
        if (acrylic_ && !hidden_) backdrop_.Place(hwnd_, rect_);
    }
    Redraw(true);
}

// --- drawing -------------------------------------------------------------------------------------

void WidgetWindow::UpdateBackdrop() {
    if (!hwnd_ || !Valid()) return;
    const bool want = PanelOn() && ThemeCfg().acrylic && Backdrop::SystemAllowsTransparency();
    const bool had = acrylic_;
    acrylic_ = want && (backdrop_.Exists() || backdrop_.Create(instance_));
    panelRadius_ = std::clamp(ThemeCfg().cornerRadius, 0.0f, 32.0f);
    if (acrylic_) {
        panelRadius_ = backdrop_.SetCornerRadius(panelRadius_);
        if (!hidden_) backdrop_.Place(hwnd_, rect_);
    } else {
        backdrop_.Destroy();
    }
    if (registered_ && had != acrylic_) DesktopLayer::SetUnder(hwnd_, acrylic_ && !hidden_ ? backdrop_.Hwnd() : nullptr);
}

void WidgetWindow::DrawPanel() {
    if (!panel_.visual || !host_.renderer) return;
    if (!PanelOn()) {
        host_.renderer->DrawLayer(panel_, 0, 0, {});
        return;
    }
    const UINT w = static_cast<UINT>(std::max<LONG>(1, rect_.right - rect_.left));
    const UINT h = static_cast<UINT>(std::max<LONG>(1, rect_.bottom - rect_.top));
    const ThemeConfig& t = ThemeCfg();
    host_.renderer->DrawLayer(panel_, w, h, [&](ID2D1DeviceContext* dc) {
        winrt::com_ptr<ID2D1SolidColorBrush> brush;
        if (FAILED(dc->CreateSolidColorBrush(Theme::ToColor(acrylic_ ? t.acrylicBackground : t.background), brush.put()))) return;
        const float fw = static_cast<float>(w), fh = static_cast<float>(h);
        const float radius = std::min(Px(panelRadius_), std::min(fw, fh) / 2.0f);
        const float stroke = std::max(1.0f, std::round(scale_));
        const float inset = stroke / 2.0f;
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, fw, fh), radius, radius), brush.get());
        brush->SetColor(Theme::ToColor(t.barBorder));
        dc->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(inset, inset, fw - inset, fh - inset), std::max(0.0f, radius - inset), std::max(0.0f, radius - inset)),
            brush.get(), stroke);
    });
}

void WidgetWindow::DrawFlash() {
    if (!flash_.visual || !host_.renderer) return;
    if (flashSteps_ <= 0) {
        host_.renderer->DrawLayer(flash_, 0, 0, {});
        return;
    }
    const UINT w = static_cast<UINT>(std::max<LONG>(1, rect_.right - rect_.left));
    const UINT h = static_cast<UINT>(std::max<LONG>(1, rect_.bottom - rect_.top));
    const float thick = std::max(1.0f, std::round((flashSteps_ % 2 == 0 ? 4.0f : 2.0f) * scale_));
    const uint32_t color = flashColor_ | 0xFF000000u;
    host_.renderer->DrawLayer(flash_, w, h, [&](ID2D1DeviceContext* dc) {
        winrt::com_ptr<ID2D1SolidColorBrush> brush;
        if (FAILED(dc->CreateSolidColorBrush(Theme::ToColor(color), brush.put()))) return;
        const float fw = static_cast<float>(w), fh = static_cast<float>(h), i = thick / 2.0f;
        const float radius = std::min(Px(8.0f), std::min(fw, fh) / 2.0f);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(i, i, fw - i, fh - i), radius, radius), brush.get(), thick);
    });
}

void WidgetWindow::Redraw(bool panel) {
    if (!hwnd_ || !target_ || !Valid()) return;
    if (panel) {
        DrawPanel();
        DrawFlash();
    }
    if (widget_) {
        const float w = static_cast<float>(rect_.right - rect_.left) / scale_, h = static_cast<float>(rect_.bottom - rect_.top) / scale_;
        widget_->SetStandaloneBox(w, h);
        const LayoutContext ctx{DockAxis::Horizontal, scale_, Px(std::min(w, h) / 1.5f), host_.renderer->DWrite()};
        widget_->Measure(ctx);
        widget_->Render(*host_.renderer, content_, ctx);
    } else {
        host_.renderer->DrawLayer(content_, 0, 0, {});
    }
    Commit();
}

void WidgetWindow::Commit() {
    if (!host_.renderer) return;
    const HRESULT hr = host_.renderer->Commit();
    if (FAILED(hr) && Renderer::IsDeviceLost(hr) && host_.deviceLost) host_.deviceLost();
}

void WidgetWindow::Flash(uint32_t argb) {
    if (!hwnd_) return;
    flashColor_ = argb;
    flashSteps_ = kFlashSteps;
    SetTimer(hwnd_, kFlashTimer, kFlashMs, nullptr);
    DrawFlash();
    Commit();
}

// --- popups --------------------------------------------------------------------------------------

RECT WidgetWindow::PopupMonitor() const { return WorkArea(); }

PopupAnchor WidgetWindow::AnchorFor(float heightDip) const {
    // As a bottom-edge dock anchors a slot's popup: above the window, centred on it; below it when
    // there is no room above (and more below).
    const RECT work = PopupMonitor();
    const LONG gap = static_cast<LONG>(Px(kPopupGap));
    const LONG h = static_cast<LONG>(Px(heightDip));
    const LONG above = rect_.top - gap - work.top, below = work.bottom - (rect_.bottom + gap);
    PopupAnchor a;
    a.along = (rect_.left + rect_.right) / 2;
    if (above >= h || above >= below) {
        a.edge = DockEdge::Bottom;
        a.side = rect_.top - gap;
    } else {
        a.edge = DockEdge::Top;
        a.side = rect_.bottom + gap;
    }
    return a;
}

ScriptPopup::Style WidgetWindow::PopupStyle() const {
    const ThemeConfig& t = ThemeCfg();
    ScriptPopup::Style s;
    s.scale = scale_;
    s.acrylic = t.acrylic && Backdrop::SystemAllowsTransparency();
    s.tint = s.acrylic ? t.acrylicBackground : t.background;
    s.border = t.barBorder;
    s.cornerRadiusDip = t.cornerRadius;
    s.monitor = PopupMonitor();
    return s;
}

void WidgetWindow::DismissScriptPopup(bool animate) {
    if (!popup_.OnScreen()) return;
    popup_.Hide(animate);
    if (widget_) widget_->PopupClosed();
}

void WidgetWindow::DismissPassive(bool animate) {
    if (!passive_.OnScreen()) return;
    passive_.Hide(animate);
    if (widget_) widget_->PopupClosed(true);
}

void WidgetWindow::DismissPopups(bool animate) {
    DismissScriptPopup(animate);
    if (folderPopup_.OnScreen()) folderPopup_.Hide(animate);
    if (calendar_.OnScreen()) calendar_.Hide(animate);
}

void WidgetWindow::DismissCentre(bool animate) {
    const bool was = centre_.OnScreen();
    centre_.Hide(animate);
    if ((was || centreOpen_) && host_.shared) host_.shared->SetCentreOpen(false);
    centreOpen_ = false;
}

void WidgetWindow::ShowCalendar(int64_t day) {
    DismissScriptPopup(false);
    DismissPassive(false);
    DismissCentre(false);
    if (folderPopup_.OnScreen()) folderPopup_.Hide(false);
    const ThemeConfig& t = ThemeCfg();
    CalendarPopup::Style style;
    style.scale = scale_;
    style.acrylic = t.acrylic && Backdrop::SystemAllowsTransparency();
    style.tint = style.acrylic ? t.acrylicBackground : t.background;
    style.text = t.labelText;
    style.secondary = t.clockDateText;
    style.border = t.barBorder;
    style.hotBackground = t.previewHotBackground;
    style.hotBorder = t.previewHotBorder;
    style.accent = t.accent;
    style.cornerRadiusDip = t.cornerRadius;
    style.monitor = PopupMonitor();
    if (const std::wstring name = Launcher::TargetName(config_->clock.clickTarget); !name.empty()) style.footer = L"Open " + name;
    style.dots = config_->calendars.showDots;
    style.reduceMotion = DockInfo().reduceMotion;
    CalendarService* service = host_.shared->Calendar().Active() ? &host_.shared->Calendar() : nullptr;
    if (!calendar_.Show(style, AnchorFor(330.0f), service, day)) MessageBeep(MB_ICONWARNING);
}

void WidgetWindow::ShowCentre() {
    DismissPopups(false);
    DismissPassive(false);
    const ThemeConfig& t = ThemeCfg();
    NotificationPopup::Style style;
    style.scale = scale_;
    style.acrylic = t.acrylic && Backdrop::SystemAllowsTransparency();
    style.tint = style.acrylic ? t.acrylicBackground : t.background;
    style.text = t.labelText;
    style.secondary = t.clockDateText;
    style.border = t.barBorder;
    style.hotBackground = t.previewHotBackground;
    style.hotBorder = t.previewHotBorder;
    style.accent = t.accent;
    style.cornerRadiusDip = t.cornerRadius;
    style.monitor = PopupMonitor();
    style.reduceMotion = DockInfo().reduceMotion;
    host_.shared->SetCentreOpen(true);  // the service runs (and polls every second) while it is open
    centreOpen_ = true;
    if (!centre_.Show(style, AnchorFor(420.0f), host_.shared->Notifications())) {
        DismissCentre(false);
        MessageBeep(MB_ICONWARNING);
    }
}

void WidgetWindow::OnNotificationsChanged() {
    if (centre_.Visible() && host_.shared) centre_.Update(host_.shared->Notifications());
}

void WidgetWindow::OnCalendarChanged() {
    if (calendar_.Visible()) calendar_.Update();
}

void WidgetWindow::OnPopupRequest() {
    popupPosted_ = false;
    if (!widget_) return;
    const WidgetInstance::PopupRequest r = widget_->TakePopupRequest();
    using Kind = WidgetInstance::PopupRequest::Kind;
    switch (r.kind) {
        case Kind::Close:
            DismissPassive(true);
            DismissPopups(true);
            break;
        case Kind::Redraw:
            if (popup_.OnScreen()) popup_.Repaint();
            if (passive_.OnScreen()) passive_.Repaint();
            break;
        case Kind::Open:
            if (hidden_) break;
            if (r.passive) {
                if (popup_.OnScreen()) {
                    popup_.Repaint();  // its ordinary popup is open: that one shows the change
                    break;
                }
                if (!passive_.Show(r.width, r.height, PopupStyle(), AnchorFor(r.height))) passive_.Hide(false);
                break;
            }
            DismissPassive(false);
            DismissCentre(false);
            if (folderPopup_.OnScreen()) folderPopup_.Hide(false);
            if (calendar_.OnScreen()) calendar_.Hide(false);
            if (!popup_.Show(r.width, r.height, PopupStyle(), AnchorFor(r.height))) MessageBeep(MB_ICONWARNING);
            break;
        case Kind::Folder: {
            if (hidden_) break;
            std::wstring name = r.arg;
            while (name.size() > 3 && (name.back() == L'\\' || name.back() == L'/')) name.pop_back();
            const size_t cut = name.find_last_of(L"\\/");
            if (cut != std::wstring::npos && cut + 1 < name.size()) name = name.substr(cut + 1);
            ItemView view = ItemView::Auto;
            FromString(r.view, view);
            DismissScriptPopup(false);
            DismissPassive(false);
            DismissCentre(false);
            if (calendar_.OnScreen()) calendar_.Hide(false);
            const ThemeConfig& t = ThemeCfg();
            FolderPopup::Style style;
            style.scale = scale_;
            style.acrylic = t.acrylic && Backdrop::SystemAllowsTransparency();
            style.tint = style.acrylic ? t.acrylicBackground : t.background;
            style.text = t.labelText;
            style.secondary = t.clockDateText;
            style.border = t.barBorder;
            style.hotBackground = t.previewHotBackground;
            style.hotBorder = t.previewHotBorder;
            style.accent = t.accent;
            style.cornerRadiusDip = t.cornerRadius;
            style.reduceMotion = DockInfo().reduceMotion;
            style.monitor = PopupMonitor();
            if (!folderPopup_.Show(r.arg, name, view, style, AnchorFor(360.0f))) MessageBeep(MB_ICONWARNING);
            break;
        }
        case Kind::Calendar:
            if (!hidden_) ShowCalendar(r.day);
            break;
        case Kind::WeatherSettings:
            DismissScriptPopup(true);
            if (host_.openSettings) host_.openSettings(cfg_);
            break;
        case Kind::Previews:
            // A standalone widget has no app slots whose windows it could preview.
            widget_->Note(L"popup.previews: a standalone widget has no app windows to preview (ignored)");
            break;
        case Kind::None:
        default:
            break;
    }
    if (widget_) widget_->OpenPendingEditor();  // popup.editText (round 6 phase 6C)
}

// --- drops ---------------------------------------------------------------------------------------

void WidgetWindow::RegisterDrop() {
    if (dropTarget_ || !hwnd_) return;
    auto local = [this](POINT screen, float& x, float& y) {
        POINT c = screen;
        ScreenToClient(hwnd_, &c);
        PointDips(c, x, y);
    };
    dropTarget_ = new ShellFiles::DropTarget(ShellFiles::DropTarget::Sink{
        [this](IDataObject* d, POINT, DWORD, DWORD allowed) -> DWORD {
            dragFiles_ = ShellFiles::FilesOf(d);
            return widget_ && widget_->AcceptsDrops() && !dragFiles_.empty() ? (allowed & DROPEFFECT_COPY ? DROPEFFECT_COPY : allowed & DROPEFFECT_LINK)
                                                                              : DROPEFFECT_NONE;
        },
        [this](POINT, DWORD, DWORD allowed) -> DWORD {
            return widget_ && widget_->AcceptsDrops() && !dragFiles_.empty() ? (allowed & DROPEFFECT_COPY ? DROPEFFECT_COPY : allowed & DROPEFFECT_LINK)
                                                                              : DROPEFFECT_NONE;
        },
        [this] { dragFiles_.clear(); },
        [this, local](IDataObject* d, POINT screen, DWORD, DWORD allowed) -> DWORD {
            std::vector<std::wstring> files = ShellFiles::FilesOf(d);
            dragFiles_.clear();
            if (!widget_ || !widget_->AcceptsDrops() || files.empty()) return DROPEFFECT_NONE;
            float x = 0, y = 0;
            local(screen, x, y);
            widget_->Drop(files, x, y);
            return allowed & DROPEFFECT_COPY ? DROPEFFECT_COPY : allowed & DROPEFFECT_LINK;
        },
    });
    if (FAILED(RegisterDragDrop(hwnd_, dropTarget_))) {
        dropTarget_->Detach();
        dropTarget_->Release();
        dropTarget_ = nullptr;
    }
}

// --- automation ----------------------------------------------------------------------------------

LRESULT WidgetWindow::QueryState(WPARAM what, LPARAM) const {
    const auto pack = [](LONG a, LONG b) { return static_cast<LRESULT>(MAKELONG(static_cast<WORD>(a), static_cast<WORD>(b))); };
    switch (what) {
        case 5:  // pending timers (the flash; a still widget window has none)
            return flashSteps_ > 0 ? 1 : 0;
        case 51:
            return reinterpret_cast<LRESULT>(hwnd_);
        case 54:
        case 244:  // the widget's state: 0 ok, 1 error, 2 suspended (-1 none)
            return widget_ ? static_cast<LRESULT>(widget_->GetState()) : -1;
        case 58:
        case 247:  // the widget's draw count
            return widget_ ? static_cast<LRESULT>(widget_->DrawCount()) : -1;
        case 235:
        case 245:  // the layer: 0 normal, 1 desktop, 2 behind
            return Valid() ? static_cast<LRESULT>(Cfg().layer) : -1;
        case 239:
        case 246:  // above the desktop (DesktopLayer::AboveDesktop)
            return DesktopLayer::AboveDesktop(hwnd_) ? 1 : 0;
        case 241:  // the kind of docks[] entry (App answers it; here for completeness)
            return 2;
        case 242:  // the window's screen rect: left / top
            return pack(rect_.left, rect_.top);
        case 243:  // width / height (px)
            return pack(rect_.right - rect_.left, rect_.bottom - rect_.top);
        case 248:  // its script popup is visible
            return popup_.Visible() ? 1 : 0;
        case 249:  // locked
            return Valid() && Cfg().locked ? 1 : 0;
        case 250:  // shown (not hidden by the "desktop" layer's rule)
            return hwnd_ && IsWindowVisible(hwnd_) ? 1 : 0;
        case 251:  // dock.width / dock.height as the script sees them (DIPs, rounded)
            if (!widget_) return -1;
            return pack(static_cast<LONG>(std::lround(widget_->DockInfo().width)), static_cast<LONG>(std::lround(widget_->DockInfo().height)));
        case 253:  // foreground-watch evaluations this window got (diagnostics)
            return static_cast<LRESULT>(watchEvents_);
        case 254:  // hidden by the "desktop" layer's rule (the window's own flag)
            return hidden_ ? 1 : 0;
        case 255:  // round 6 phase 6C: the widget's text editor is open (1 / 0; -1 no widget)
            return widget_ ? (widget_->EditorOpen() ? 1 : 0) : -1;
        case 256:  // the editor's HWND (0: none)
            return widget_ ? reinterpret_cast<LRESULT>(widget_->EditorHwnd()) : 0;
        default:
            return -1;
    }
}

// --- window --------------------------------------------------------------------------------------

LRESULT CALLBACK WidgetWindow::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    WidgetWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<WidgetWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<WidgetWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self || self->hwnd_ != hwnd) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->WndProc(msg, wp, lp);
}

LRESULT WidgetWindow::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    if (!Valid()) return DefWindowProcW(hwnd_, msg, wp, lp);
    const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    switch (msg) {
        case WM_WINDOWPOSCHANGING:
            if (registered_) DesktopLayer::FilterPosChanging(hwnd_, reinterpret_cast<WINDOWPOS*>(lp));
            return 0;
        case WM_WINDOWPOSCHANGED:
            if (acrylic_ && !hidden_ && !(reinterpret_cast<const WINDOWPOS*>(lp)->flags & SWP_NOZORDER)) backdrop_.Restack(hwnd_);
            return 0;
        case WM_MOUSEACTIVATE:
            // The click that follows closes the widget's popup and says so (a click toggles it).
            popupAtActivate_ = widget_ && WidgetPopupShown(*widget_);
            // A "desktop" window stays right above the desktop (activation would raise it).
            return DesktopLayered() ? MA_NOACTIVATE : MA_ACTIVATE;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                POINT c{};
                GetCursorPos(&c);
                ScreenToClient(hwnd_, &c);
                const wchar_t* cursor = IDC_ARROW;
                switch (grab_ != Grab::None ? grab_ : GrabAt(c)) {
                    case Grab::Left: case Grab::Right: cursor = IDC_SIZEWE; break;
                    case Grab::Top: case Grab::Bottom: cursor = IDC_SIZENS; break;
                    case Grab::TopLeft: case Grab::BottomRight: cursor = IDC_SIZENWSE; break;
                    case Grab::TopRight: case Grab::BottomLeft: cursor = IDC_SIZENESW; break;
                    case Grab::Move: cursor = IDC_SIZEALL; break;
                    default: break;
                }
                SetCursor(LoadCursorW(nullptr, cursor));
                return TRUE;
            }
            break;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            const bool hadPopup = popupAtActivate_ || (widget_ && WidgetPopupShown(*widget_));
            popupAtActivate_ = false;
            if (hadPopup) DismissPopups(true);  // a click on the widget toggles its popup
            const Grab g = GrabAt(pt);
            grabPt_ = pt;
            ClientToScreen(hwnd_, &grabPt_);
            grabRect_ = rect_;
            grabMoved_ = false;
            SetCapture(hwnd_);
            if (g != Grab::None) {
                grab_ = g;  // the resize band: the widget does not see this press
                return 0;
            }
            grab_ = Grab::Press;
            pressClosedPopup_ = hadPopup;
            float x = 0, y = 0;
            PointDips(pt, x, y);
            if (widget_) widget_->Press(x, y);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!tracking_) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
                tracking_ = TrackMouseEvent(&tme) != FALSE;
            }
            if (!hovered_ && grab_ == Grab::None) {
                hovered_ = true;
                float x = 0, y = 0;
                PointDips(pt, x, y);
                if (widget_) widget_->Hover(true, x, y);
            }
            if (grab_ == Grab::None || GetCapture() != hwnd_) return 0;
            POINT s = pt;
            ClientToScreen(hwnd_, &s);
            const LONG dx = s.x - grabPt_.x, dy = s.y - grabPt_.y;
            if (!grabMoved_ && std::labs(dx) < GetSystemMetrics(SM_CXDRAG) && std::labs(dy) < GetSystemMetrics(SM_CYDRAG)) return 0;
            if (grab_ == Grab::Press) {
                if (Cfg().locked) return 0;  // locked: never moved (a release is still a click)
                grab_ = Grab::Move;          // a left drag moves the window
                SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            }
            grabMoved_ = true;
            RECT r = grabRect_;
            if (grab_ == Grab::Move) {
                OffsetRect(&r, dx, dy);
                Snap(r, Grab::Move);
            } else {
                if (grab_ == Grab::Left || grab_ == Grab::TopLeft || grab_ == Grab::BottomLeft) r.left += dx;
                if (grab_ == Grab::Right || grab_ == Grab::TopRight || grab_ == Grab::BottomRight) r.right += dx;
                if (grab_ == Grab::Top || grab_ == Grab::TopLeft || grab_ == Grab::TopRight) r.top += dy;
                if (grab_ == Grab::Bottom || grab_ == Grab::BottomLeft || grab_ == Grab::BottomRight) r.bottom += dy;
                Snap(r, grab_);
                Constrain(r, grab_);
            }
            Place(r);
            return 0;
        }
        case WM_LBUTTONUP: {
            if (grab_ == Grab::None) return 0;
            const Grab g = grab_;
            const bool moved = grabMoved_;
            grab_ = Grab::None;
            grabMoved_ = false;
            if (GetCapture() == hwnd_) ReleaseCapture();
            if (moved) {
                StoreRect();
                if (host_.changed) host_.changed();
                if (host_.moved) host_.moved();  // round 12 phase 12I: the drag ended
            } else if (g == Grab::Press && widget_) {
                float x = 0, y = 0;
                PointDips(pt, x, y);
                widget_->Click(x, y, pressClosedPopup_);
            }
            pressClosedPopup_ = false;
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (grab_ != Grab::None && reinterpret_cast<HWND>(lp) != hwnd_) {
                const bool moved = grabMoved_;
                grab_ = Grab::None;
                grabMoved_ = false;
                if (moved) {
                    StoreRect();
                    if (host_.changed) host_.changed();
                    if (host_.moved) host_.moved();  // round 12 phase 12I
                }
            }
            return 0;
        case WM_MBUTTONDOWN:
            middlePressed_ = true;
            return 0;
        case WM_MBUTTONUP:
            if (middlePressed_ && widget_) {
                float x = 0, y = 0;
                PointDips(pt, x, y);
                widget_->MiddleClick(x, y);
            }
            middlePressed_ = false;
            return 0;
        case WM_RBUTTONUP: {
            if (grab_ != Grab::None) return 0;
            POINT s = pt;
            ClientToScreen(hwnd_, &s);
            if (host_.menu) host_.menu(cfg_, s);
            return 0;
        }
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL: {
            POINT c = pt;  // screen coordinates
            ScreenToClient(hwnd_, &c);
            float x = 0, y = 0;
            PointDips(c, x, y);
            if (widget_) widget_->Wheel(x, y, GET_WHEEL_DELTA_WPARAM(wp), msg == WM_MOUSEHWHEEL);
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_ = false;
            if (hovered_) {
                hovered_ = false;
                POINT c{};
                GetCursorPos(&c);
                ScreenToClient(hwnd_, &c);
                float x = 0, y = 0;
                PointDips(c, x, y);
                if (widget_) widget_->Hover(false, x, y);
            }
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (wp == VK_ESCAPE && host_.shared && host_.shared->DashboardOn()) {
                host_.shared->EndDashboard();
                return 0;
            }
            break;
        case WM_TIMER:
            if (wp == kFlashTimer) {
                if (--flashSteps_ <= 0) {
                    flashSteps_ = 0;
                    KillTimer(hwnd_, kFlashTimer);
                }
                DrawFlash();
                Commit();
                return 0;
            }
            break;
        case kRedrawMessage:
            redrawPosted_ = false;
            if (widget_) widget_->TakeChanged();
            Redraw(false);
            return 0;
        case kPopupMessage:
            OnPopupRequest();
            return 0;
        case WM_TIMECHANGE:
            TimeChanged();
            return 0;
        case WM_POWERBROADCAST:
            if (wp == PBT_APMRESUMEAUTOMATIC) TimeChanged();
            return TRUE;
        case WM_DPICHANGED:
            dpi_ = HIWORD(wp);
            scale_ = static_cast<float>(dpi_) / 96.0f;
            if (widget_) widget_->SetDock(DockInfo());
            Place(RectFromConfig());
            Redraw(true);
            return 0;
        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE:
            if (msg == WM_DISPLAYCHANGE || wp == SPI_SETWORKAREA) {
                const RECT r = RectFromConfig();
                if (!EqualRect(&r, &rect_)) Place(r);
            }
            return 0;
        case WM_SYSCOMMAND:
            // Never minimized, maximized or closed by the system menu / Alt+F4 (the window belongs to
            // the config; "Remove from desktop" removes it).
            if ((wp & 0xFFF0) == SC_CLOSE || (wp & 0xFFF0) == SC_MINIMIZE || (wp & 0xFFF0) == SC_MAXIMIZE) return 0;
            break;
        case WM_CLOSE:
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            ValidateRect(hwnd_, nullptr);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
