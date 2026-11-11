#pragma once

#include <windows.h>

#include <winrt/base.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Backdrop.h"
#include "CalendarPopup.h"
#include "Config.h"
#include "FolderPopup.h"
#include "IconCache.h"
#include "NotificationPopup.h"
#include "Renderer.h"
#include "ScriptPopup.h"
#include "WidgetInstance.h"

class DockShared;
namespace ShellFiles {
class DropTarget;
}

// A standalone widget (round 6 phase 6B, docks[] kind "widget"): one script widget in a top-level
// window of its own on the desktop, not attached to a dock (an analog clock or the weather by
// itself, moved and resized freely). Modelled on FolderWindow:
//
// - Window: WS_POPUP | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP on the shared desktop-windows
//   device (DockManager's renderer, also the folder windows'); device loss / restore like
//   FolderWindow. Its client area is the widget's box: the WidgetInstance (key "<id>@<entry id>")
//   measures to the window size in standalone mode (ScriptDockInfo::standalone; iconSize = the
//   shorter side / 1.5; axis "horizontal", edge "bottom") and draws into one layer over an optional
//   panel (background "panel": the theme's bar fill or acrylic, rounded, with the bar hairline).
// - Input: press / click / middle click / wheel / hover / file drops reach the widget with DIP
//   coordinates from the window's top-left. A left drag that travels SM_CXDRAG while unlocked
//   moves the window (the widget got the press; no click follows); a press and release without
//   that travel is the widget's click. An 8 DIP band inside the edges and corners resizes it while
//   unlocked (the manifest's standalone.minWidth / minHeight / aspect respected), redrawing live.
//   Moves and resizes snap within 8 DIP to the work area and to the other floating things
//   (floating docks, other standalone widgets); x / y / w / h are saved on release. A right click
//   opens the window's menu (App: <name> settings…, Window settings…, Lock / Unlock, Reload widget,
//   App settings…, Remove from desktop; round 10 phase 10A: renamed and reordered; round 16 phase
//   16G: Move to dock ▸ after Reload widget). Round 16 phase 16G: a move drag released over a
//   dock's bar puts the widget on that dock (Host::dockDrag; the dock shows its marker meanwhile);
//   Escape cancels a move / resize drag (back where it was).
// - Layers: "behind" (default) registers with DesktopLayer (under every app window, activatable
//   without rising; with background "panel" and acrylic its Backdrop window is kept beneath it);
//   "desktop" is shown only while the desktop is the foreground (DesktopLayer::DesktopForeground,
//   re-checked on the shared ForegroundWatch's events, as desktop docks do); "normal" is topmost.
//   Dashboard mode lifts "desktop" and "behind" ones to the top like those docks.
// - Popups (anchored on the window as a bottom-edge dock anchors them on a slot: above it when
//   there is room, else below): the script popup and its passive kind, the folder popup, the
//   calendar and the notification centre. popup.previews has no app slots to show here: a logged
//   no-op. The notification centre's rows have no app icons here (the dock's icon cache is not
//   available).
// - popup.editText (round 6 phase 6C): the text editor is owned by this window, placed above it
//   (below without room), topmost only when the window is ("normal", or lifted by the dashboard).
// Nothing runs while idle: no timers but a running flash and the popups' own fades.
class WidgetWindow final : private WidgetInstance::Client {
public:
    static constexpr wchar_t kClassName[] = L"WinDockWidgetWindow";

    struct Host {
        Renderer* renderer = nullptr;  // the shared desktop-windows device (DockManager)
        DockShared* shared = nullptr;  // the widget runtime, calendar, notifications, dashboard
        // The config entry changed by the window itself (moved, resized): save, refresh Settings.
        std::function<void()> changed;
        // The screen rects to snap to: the floating docks and the other standalone widgets.
        std::function<std::vector<RECT>(const WidgetWindow*)> neighbours;
        // A commit failed with a device-lost error: the shared device must be rebuilt (posted).
        std::function<void()> deviceLost;
        // The right-click menu of config entry `cfg` at a screen point (App runs it).
        std::function<void(size_t cfg, POINT screen)> menu;
        // weather.openSettings(): the entry's widget settings (App::OpenItemSettings(cfg, 0)).
        std::function<void(size_t cfg)> openSettings;
        // Round 12 phase 12I: the window's screen rect changed (a drag or resize ended, a placement
        // by the program: the config, a display / DPI change) or it was hidden / shown: its settings
        // panel goes beside it again (or closes while it is hidden). Not on each step of a drag.
        std::function<void()> moved;
        // Round 16 phase 16G: a move drag of config entry `cfg` at screen point `screen`: phase 0
        // each step (the dock under it shows its insertion marker), 1 cancelled (Escape, capture
        // lost: the marker goes), 2 released. Non-zero on 2: a dock takes the widget (the move is
        // posted; this window goes with the next sync, so it stores no rect).
        std::function<int(size_t cfg, int phase, POINT screen)> dockDrag;
    };

    WidgetWindow() = default;
    WidgetWindow(const WidgetWindow&) = delete;
    WidgetWindow& operator=(const WidgetWindow&) = delete;
    ~WidgetWindow() { Destroy(); }

    HRESULT Create(HINSTANCE instance, Config* config, size_t cfg, HMONITOR monitor, Host host);
    void Destroy();
    HWND Hwnd() const { return hwnd_; }
    size_t ConfigIndex() const { return cfg_; }
    void SetConfigIndex(size_t cfg) { cfg_ = cfg; }
    // Another monitor (or the same one after a display change): placed again.
    void SetMonitor(HMONITOR monitor);
    // The config entry may have changed (Settings, Reload config, its widget's settings): applies
    // what differs (cheap when nothing did).
    void ApplyConfig();
    // The shared device is about to be rebuilt / was rebuilt.
    void ReleaseDevice();
    HRESULT RestoreDevice();
    RECT ScreenRect() const { return rect_; }
    // Shown now (not hidden by the "desktop" layer's rule).
    bool Shown() const { return hwnd_ && !hidden_; }

    // --- the layers (DockManager) ---
    // Needs the shared foreground watch (layer "desktop").
    bool WantsWatch() const;
    // A foreground change: the "desktop" layer's rule again.
    void OnWatchEvent();
    // Dashboard mode began / ended (DockShared::DashboardOn): lifted to the top or back.
    void OnDashboardChanged();
    // Explorer restarted: a shown "desktop" window goes above the new desktop window ("behind"
    // ones are re-placed by DesktopLayer::Reassert).
    void ReassertLayer();
    // Round 9 phase 9A, the full-screen guard (DockManager): the window is in the topmost band
    // (layer "normal", or lifted by the dashboard); hidden while a full-screen app is in front on
    // its monitor (popups closed, no z-order call until it is back, then topmost once).
    bool TopmostBand() const;
    void SetFullscreenHidden(bool hidden);
    bool FullscreenHidden() const { return fullscreen_; }
    // Pulses a border in `argb` (6 steps of 250 ms, as a dock's Flash).
    void Flash(uint32_t argb);
    // The system time / zone changed or the machine resumed.
    void TimeChanged();
    // The notification list / the calendar's data changed: the open centre / calendar follow.
    void OnNotificationsChanged();
    void OnCalendarChanged();

    WidgetInstance* Widget() const { return widget_.get(); }
    // Reloads the widget's package and script (ItemCommand 809).
    bool ReloadWidget();
    // Round 6 phase 6C: closes the widget's text editor as OK / Cancel would (ItemCommand 857 / 858).
    bool EditorCommand(bool ok) { return widget_ && widget_->CloseEditor(ok); }
    // Automation (README "Standalone widgets"): QueryState ids 5, 51, 54, 58, 235, 239, 241 - 256.
    LRESULT QueryState(WPARAM what, LPARAM arg) const;

private:
    static constexpr UINT kRedrawMessage = WM_APP + 30;  // posted: the widget asked to be redrawn
    static constexpr UINT kPopupMessage = WM_APP + 31;   // posted: the widget asked for its popup
    static constexpr UINT_PTR kFlashTimer = 1;
    enum class Grab { None, Press, Move, Left, Right, Top, Bottom, TopLeft, TopRight, BottomLeft, BottomRight };

    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);

    const DockConfig& Cfg() const { return config_->docks[cfg_]; }
    DockConfig& Cfg() { return config_->docks[cfg_]; }
    bool Valid() const { return config_ && cfg_ < config_->docks.size() && config_->docks[cfg_].IsWidgetWindow(); }
    float Px(float dip) const { return std::round(dip * scale_); }
    const ThemeConfig& ThemeCfg() const { return config_->theme; }
    // The layer's state now (dashboard mode lifts "desktop" / "behind").
    bool InDashboard() const;
    bool BehindLayered() const { return Cfg().layer == DockLayer::Behind && !InDashboard(); }
    bool DesktopLayered() const { return Cfg().layer == DockLayer::Desktop && !InDashboard(); }

    // --- the widget ---
    void MakeWidget();
    ScriptDockInfo DockInfo() const;
    std::wstring WidgetKey() const;
    void PointDips(POINT client, float& x, float& y) const;

    // --- WidgetInstance::Client ---
    void WidgetChanged(WidgetInstance& widget) override;
    void WidgetPopupRequest(WidgetInstance& widget) override;
    bool WidgetPopupShown(const WidgetInstance& widget) const override;
    bool WidgetPassiveShown(const WidgetInstance& widget) const override;
    bool WidgetActivateNotification(uint32_t id) override;
    bool WidgetOpenCentre(WidgetInstance& widget) override;
    EditorPlace WidgetEditorPlace(const WidgetInstance& widget) const override;
    void WidgetEditorChanged(WidgetInstance&, bool) override {}
    StartMenuPlacer* WidgetStartPlacer() override;  // round 16 phase 16A
    void OnPopupRequest();

    // --- placement ---
    RECT WorkArea() const;
    RECT RectFromConfig() const;
    void StoreRect();
    void Place(const RECT& r);
    Grab GrabAt(POINT client) const;
    void Snap(RECT& r, Grab g) const;
    // Applies the manifest's minimum size and aspect to a resize of `grabRect_` toward `r`.
    void Constrain(RECT& r, Grab g) const;
    float MinWDip() const;
    float MinHDip() const;
    // The layer: registration with DesktopLayer, topmost, the "desktop" rule.
    void SyncLayer();
    void AssertZOrder();
    void UpdateShown();
    // Hidden by the "desktop" layer's rule or by the full-screen guard.
    bool WantHidden() const;

    // --- drawing ---
    HRESULT EnsureTarget();
    void Redraw(bool panel);
    void DrawPanel();
    void DrawFlash();
    void Commit();
    void UpdateBackdrop();
    bool PanelOn() const { return Cfg().widgetWindow.background == WidgetBackground::Panel; }

    // --- popups ---
    PopupAnchor AnchorFor(float heightDip) const;
    RECT PopupMonitor() const;
    ScriptPopup::Style PopupStyle() const;
    void DismissPopups(bool animate);
    void DismissScriptPopup(bool animate);
    void DismissPassive(bool animate);
    void ShowCalendar(int64_t day);
    void ShowCentre();
    void DismissCentre(bool animate);

    // --- drops ---
    void RegisterDrop();

    HINSTANCE instance_ = nullptr;
    Config* config_ = nullptr;
    size_t cfg_ = 0;
    HMONITOR monitor_ = nullptr;
    Host host_;
    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    float scale_ = 1.0f;
    RECT rect_{};
    DockConfig applied_;  // the entry as last applied (ApplyConfig compares)
    std::unique_ptr<WidgetInstance> widget_;
    std::wstring widgetKey_;

    // Composition (on the shared device).
    winrt::com_ptr<IDCompositionTarget> target_;
    Layer root_;
    Layer panel_;    // the background panel (background "panel")
    Layer content_;  // the widget
    Layer flash_;    // the flash border
    Backdrop backdrop_;
    bool acrylic_ = false;
    float panelRadius_ = 8.0f;  // DIPs (acrylic: the system radius)
    bool redrawPosted_ = false;
    bool popupPosted_ = false;

    // Layer state.
    bool registered_ = false;  // with DesktopLayer ("behind", not lifted)
    bool hidden_ = false;      // the "desktop" rule (or the full-screen guard) hides it
    bool fullscreen_ = false;  // round 9 phase 9A: the full-screen guard hides it
    size_t watchEvents_ = 0;   // OnWatchEvent calls (QueryState 253)

    // Move / resize / press.
    Grab grab_ = Grab::None;
    POINT grabPt_{};  // screen
    RECT grabRect_{};
    bool grabMoved_ = false;
    bool pressClosedPopup_ = false;
    bool popupAtActivate_ = false;  // WM_MOUSEACTIVATE saw the widget's popup open
    float keepRatio_ = 0.0f;        // aspect "keep": w / h at creation (0: not yet)
    bool tracking_ = false;
    bool hovered_ = false;
    bool middlePressed_ = false;

    // Flash.
    int flashSteps_ = 0;
    uint32_t flashColor_ = 0;

    // Popups (one script popup, one passive one, the folder, calendar and centre).
    ScriptPopup popup_;
    ScriptPopup passive_{PopupWindow::Mode::Hover, ScriptPopup::kPassiveClassName};
    FolderPopup folderPopup_;
    CalendarPopup calendar_;
    NotificationPopup centre_;
    IconCache icons_;  // the folder popup's (FromHicon on the shared device)
    bool popupsReady_ = false;
    bool centreOpen_ = false;

    ShellFiles::DropTarget* dropTarget_ = nullptr;
    std::vector<std::wstring> dragFiles_;
};
