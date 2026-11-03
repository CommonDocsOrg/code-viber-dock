#pragma once

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "AppIdentity.h"
#include "Backdrop.h"
#include "CalendarPopup.h"
#include "Config.h"
#include "DockElement.h"
#include "DockGeometry.h"
#include "DockItems.h"
#include "FolderPopup.h"
#include "ForegroundWatch.h"
#include "IconCache.h"
#include "NotificationPopup.h"
#include "NotificationService.h"
#include "PreviewPopup.h"
#include "Renderer.h"
#include "ScriptPopup.h"
#include "Thumbnails.h"
#include "TrayHost.h"
#include "Weather.h"
#include "WeatherPopup.h"
#include "WidgetInstance.h"
#include "WidgetRuntime.h"
#include "WindowTracker.h"

struct IDataObject;
class DockWindow;

// The services every dock shares (one per process, owned by DockManager): the window list, the
// weather data, the foreground watch of intelligent auto-hide.
class DockShared {
public:
    virtual WindowTracker& Tracker() = 0;
    virtual WeatherService& WeatherData() = 0;
    // Re-applies config.weather to the service; it runs while any dock has a weather item.
    virtual void ConfigureWeather() = 0;
    virtual bool AnyWeatherItem() const = 0;
    virtual ForegroundWatch& Watch() = 0;
    // A dock starts / stops needing the foreground watch (autoHide "intelligent"): the hooks run
    // while at least one does, and every watching dock gets its overlap re-checked on an event.
    virtual void SetWatching(DockWindow* dock, bool on) = 0;
    // The widget runtime: the scheduler of every script widget's timers, the live instances.
    virtual WidgetRuntime& Widgets() = 0;
    // The tray takeover (shared: one Shell_TrayWnd per process). Running() only while enabled.
    virtual TrayHost& TrayService() = 0;
    // Windows' notifications (round 4 phase 9). Active() only while some consumer wants it.
    virtual NotificationService& Notifications() = 0;
    virtual bool ActivateNotification(uint32_t id) = 0;
    virtual void SetCentreOpen(bool open) = 0;

protected:
    ~DockShared() = default;
};

// The dock HWND: placement, DPI, layout, input, magnification, hover label, auto-hide, and the
// composition layers drawn into it. What it shows comes from its elements (DockElement.h, one per
// config item, in config order); it lays out and animates the flat list of slots they contribute.
class DockWindow : private ElementHost, private WidgetInstance::Client {
public:
    static constexpr wchar_t kClassName[] = L"WinDock.DockWindow";
    static constexpr UINT_PTR kClockTimerId = ElementHost::kClockTimerId;  // a clock element's tick
    // 3 = WindowTracker::kCoalesceTimerId
    static constexpr UINT_PTR kIconEvictTimerId = 4;
    // 5 = App::kSaveTimerId
    static constexpr UINT_PTR kBounceTimerId = 6;
    // One-shot timers of the interaction state machine; none of them runs while idle.
    static constexpr UINT_PTR kHideTimerId = 7;      // hideDelayMs after the mouse left
    static constexpr UINT_PTR kRevealTimerId = 8;    // revealDelayMs on the bottom-edge strip
    static constexpr UINT_PTR kSlideTimerId = 9;     // end of the slide-in/out animation
    static constexpr UINT_PTR kMagnifyTimerId = 10;  // throttles magnification updates
    static constexpr UINT_PTR kLabelTimerId = 11;    // hover label delay
    static constexpr UINT_PTR kRestTimerId = 12;     // end of the return-to-rest animation
    static constexpr UINT_PTR kBackdropTimerId = 13; // moves the acrylic window while the bar animates
    static constexpr UINT_PTR kDragTimerId = 14;     // polls Escape while an icon is dragged
    static constexpr UINT_PTR kRemoveTimerId = 15;   // end of the fade of an icon dragged off the dock
    static constexpr UINT_PTR kPreviewTimerId = 16;  // previewDelayMs hover delay before the thumbnail popup
    static constexpr UINT_PTR kPreviewLeaveTimerId = 17;  // grace period after leaving item and popup
    // 18 = ForegroundWatch::kTimerId (intelligent hide)
    static constexpr UINT_PTR kLabelFadeTimerId = 19;     // end of the name label's fade-out
    // 20 = WeatherService::kTimerId (the next weather refresh, one-shot)
    static constexpr UINT_PTR kRecycleBinTimerId = 21;    // coalesces Recycle Bin change notifications
    // 22 = Thumbnails::kTimerId
    static constexpr UINT_PTR kGroupHoldTimerId = 23;     // a dragged icon held over a pin: make a group
    static constexpr UINT_PTR kScrollHintTimerId = 24;    // a scrolling dock: fade the scroll thumb out
    static constexpr UINT_PTR kAutoScrollTimerId = 25;    // a drag held near a viewport end scrolls the run
    // Posted to the dock window: the Recycle Bin changed (SHChangeNotifyRegister, new delivery);
    // a dropped file's open-with / recycle action (run after the OLE Drop call has returned).
    // WM_APP + 20 = WeatherService::kResultMessage (a forecast fetch finished; handled here) and
    // WM_APP + 23 = WeatherService::kGeocodeMessage (a place search finished; handled by App).
    static constexpr UINT kRecycleBinChangedMessage = WM_APP + 21;
    static constexpr UINT kDropActionMessage = WM_APP + 22;
    // The shell's app bar notifications (SHAppBarMessage ABM_NEW callback; ABN_POSCHANGED, ...).
    static constexpr UINT kAppBarMessage = WM_APP + 24;
    // Posted (coalesced): a script widget asked to be redrawn (WidgetInstance::Client).
    static constexpr UINT kWidgetRedrawMessage = WM_APP + 25;
    // Posted (coalesced): script widgets asked for their popups (DockWindow::OnWidgetPopupRequests).
    static constexpr UINT kWidgetPopupMessage = WM_APP + 26;
    // WM_APP + 27 = Thumbnails::kSnapshotMessage (a snapshot round finished). Posted (coalesced):
    // window thumbnails got new frames, the tiles redraw.
    static constexpr UINT kThumbRedrawMessage = WM_APP + 28;
    // Posted: a widget asked for the notification centre (notifications.openCentre(), from inside a
    // script call). wParam: unused.
    static constexpr UINT kCentreRequestMessage = WM_APP + 47;

    // Diagnostics / automation: SendMessage(dock, RegisterWindowMessage(kQueryItemMessage), wp, lp)
    // with wp = a top-level HWND (or 0 and lp = item index) returns the screen point of the
    // matching item's icon centre (rest layout; a widget: its box centre) as MAKELONG(x, y), or -1
    // if there is no such item. Item indexes count every slot in dock order (widgets included).
    static constexpr wchar_t kQueryItemMessage[] = L"WinDock.QueryItemPoint";
    // Diagnostics: SendMessage(dock, RegisterWindowMessage(kQueryStateMessage), what, 0):
    // what = 0: auto-hide state (0 Shown, 1 Hiding, 2 Hidden, 3 Showing); 1: the current
    // SHQueryUserNotificationState value (or -1); 2: hovered item index (-1 none); 3: 1 if the
    // hover label is visible; 4: 1 while magnified; 5: number of pending interaction timers;
    // 6: 1 while the acrylic backdrop is active; 7: drag state (0 none, 1 dragging an icon,
    // 2 fading out an icon dragged off the dock, 3 a file drag is over the dock); 8: 1 while the
    // dock window has the mouse capture; 9: insertion index of the current drag (-1 none);
    // 10: 1 if the OLE drop target is registered; 11: 1 while the thumbnail popup is visible;
    // 12: number of cards in the thumbnail popup; 13: the window shown by popup card lp (0 none);
    // 14: auto-hide mode (0 off, 1 always, 2 intelligent); 15: 1 while the foreground window
    // overlaps the dock (intelligent mode; 0 otherwise); 16: 1 while the settings window exists
    // (answered by App's message hook); 17: 1 while reduce-motion is in effect
    // (animation.reduceMotion, or "system" and Windows' animation effects are off); 19: number of
    // foreground-watch events accepted so far (they only arrive in intelligent mode); 20: 1 if the
    // theme preset failed to resolve on the last load; 21: number of foreground-watch callbacks
    // delivered so far, filtered ones included; 24: kind of item lp (0 app, 1 folder,
    // 2 separator, 3 Recycle Bin, 4 clock, 5 weather; -1 no such item); 25: 1 if item lp has a
    // custom icon (0 no, -1 no such item); 22: 1 while the folder popup is visible; 23: number of entries (cells)
    // shown in the folder popup; 26: drop mode of a file drag over the dock (0 none, 1 insertion
    // marker, 2 open with the app under the pointer, 3 recycle on the Recycle Bin); 27: weather
    // state (0 none: off, unconfigured or no data yet; 1 stale; 2 fresh; 3 error); 28: current
    // temperature x 10 in the configured units (INT_MIN without data); 29: 1 while the weather
    // popup is visible; 30: seconds since the weather data was fetched (-1 none); 31: 1 while
    // the calendar popup is visible; 32: icon effects actually applied (bit 0 glow surfaces,
    // bit 1 reflections, bit 2 the perspective tilt); 33: glow opacity target x 100 of item lp
    // (0 without a glow surface, -1 no such item); 34: height in pixels of item lp's reflection
    // surface (0 when off); 35: the dominant colour of item lp's icon as 0xAARRGGBB (0 unknown,
    // -1 no such item); 36: 1 while the dock is locked (docks[].locked); 37: the index in
    // docks[0].items[] of the config item that contributes slot lp (-1 no such slot); 38: the edge
    // the dock is laid out on (0 bottom, 1 top, 2 left, 3 right); 39: docks[0].align (0 center,
    // 1 start, 2 end); 40: docks[0].offset (DIPs); 41: 1 while laid out full width; 42:
    // docks[0].reserveSpace; 43: 1 while registered as an app bar (reserving screen space); 44: the
    // peek strip's height in px (0: off; used while auto-hide hides the dock); 45: 1 while a title is drawn; 46: dock
    // drag (0 none, 1 moving the dock along its edge, 2 over another edge's zone: dropping there
    // moves it to that edge). "docks[0]" above means this dock's own config entry. App answers 16
    // and 47 - 51 (dock count, monitor index, parked, config index, window) and routes the others
    // to the dock in HIWORD(lParam) (AppIpc in App.h).
    static constexpr wchar_t kQueryStateMessage[] = L"WinDock.QueryState";

    enum class HideState { Shown, Hiding, Hidden, Showing };

    // Called for every message before DockWindow handles it. Return true to consume
    // the message, with the LRESULT in `result`.
    using MessageHook = std::function<bool(HWND, UINT, WPARAM, LPARAM, LRESULT& result)>;

    // Changes to the pins (config->Main().items) the dock asks for (the dock only reads the config; the owner
    // edits it, calls ReloadPins and saves).
    struct PinRequests {
        // New order of the pinned section as item keys. A key that is not pinned yet (a running
        // item dragged into the pinned section) is pinned at that position.
        std::function<void(const std::vector<AppKey>& order)> reorder;
        // Remove every pin of this app (an icon dragged off the dock).
        std::function<void(const AppKey& key)> unpin;
        // Pin .lnk / .exe files (dropped on the dock) as pin `at` onwards (at == pin count: append).
        std::function<void(const std::vector<std::wstring>& paths, size_t at)> pinFiles;
        // Move config item `from` (a widget dragged on the dock) right before item `to` (>= the item
        // count: to the end) of config->Main().items.
        std::function<void(size_t from, size_t to)> moveItem;
        // The dock was dragged (Ctrl + drag): put it on `edge` at `align` + `offset` DIPs.
        std::function<void(DockEdge edge, DockAlign align, int offset)> moveDock;
        // A circle was dragged: its centre goes to x / y DIPs from the work area's top-left.
        std::function<void(int x, int y)> moveCircle;
        // Shortcut groups. `source` (a pin or a running app) held over `target`: a group of the two
        // at the target's place, or `source` added to `target` when that is a group.
        std::function<void(const AppKey& source, const AppKey& target)> group;
        // .lnk / .exe / folder files dropped on a group (its icon or its popup): new members.
        std::function<void(const AppKey& group, const std::vector<std::wstring>& paths)> addToGroup;
        // Member `member` of `group` becomes a pin at pin index `at` (SIZE_MAX: right after the group).
        std::function<void(const AppKey& group, size_t member, size_t at)> memberOut;
        // Member `member` of `group` is removed.
        std::function<void(const AppKey& group, size_t member)> removeMember;
        // The tray (8b). Icon `key` dropped on tray item `item` (an index into this dock's items):
        // it moves right before icon `before` in the grid (empty: to the end) and is shown there.
        std::function<void(size_t item, const std::wstring& key, const std::wstring& before)> trayOrder;
        // Icon `key` becomes a trayApp item at pin index `at` (as InsertPin places it).
        std::function<void(const std::wstring& key, size_t at)> pinTray;
        // Its trayApp items go (it is shown in the grid again).
        std::function<void(const std::wstring& key)> unpinTray;
        // Icon `key` hidden from / shown in the grid of tray item `item`.
        std::function<void(size_t item, const std::wstring& key, bool hide)> trayHide;
        // Settings > Tray.
        std::function<void()> traySettings;
    };

    DockWindow() = default;
    DockWindow(const DockWindow&) = delete;
    DockWindow& operator=(const DockWindow&) = delete;
    ~DockWindow();

    // Creates the window, devices and visuals and shows the dock for config->docks[dockIndex] on
    // `monitor`. Returns a failure HRESULT if the composition stack can't be created.
    HRESULT Create(HINSTANCE instance, const Config* config, size_t dockIndex, HMONITOR monitor, DockShared* shared,
                   MessageHook hook);
    void Destroy();
    HWND Hwnd() const override { return hwnd_; }
    // The config dock this window shows (an index into config->docks; the manager re-points it
    // when docks are added / removed).
    size_t ConfigIndex() const { return dockIndex_; }
    void SetConfigIndex(size_t index) { dockIndex_ = index; }
    // The monitor the dock is laid out on. A new one relayouts (ApplyConfig).
    HMONITOR Monitor() const { return monitor_; }
    void SetMonitor(HMONITOR monitor);
    // DockManager: the tracker's snapshot changed / the foreground watch fired / the weather changed.
    void OnSnapshot(const WindowTracker::Snapshot& snap);
    // The tracker saw these windows' captions change: window slots (their hover label and title
    // under the icon) and the open preview popup's cards follow.
    void OnWindowTitles(const std::vector<HWND>& windows);
    // Automation (ItemCommand 810): what a left click on slot `index` does (activate / minimize its
    // window or app, launch, open its popup). False without such a slot.
    bool ClickSlot(int index);
    void OnWatchEvent();
    // The weather service's data or status changed: the weather elements redraw (DockEvent), the
    // popup updates.
    void OnWeatherChanged();
    // Automation (WinDock.QueryItemPoint / WinDock.QueryState for this dock).
    LRESULT QueryItemPoint(WPARAM wp, LPARAM lp) const;
    LRESULT QueryState(WPARAM wp, LPARAM lp) const;
    void SetPinRequests(PinRequests requests) { pinRequests_ = std::move(requests); }
    // An icon is being dragged (or fading out after being dragged off the dock), or the dock itself.
    bool InDrag() const { return dragPhase_ != DragPhase::None || dockDrag_; }

    // --- app bar (fullWidth + reserveSpace with autoHide "off") ---
    // The owner allows the registration once the taskbar controller has saved the work area
    // (and withdraws it around every taskbar hide / restore, and before quitting), so the
    // controller never records our reservation as the original work area.
    void AllowAppBar(bool allow);
    bool AppBarRegistered() const { return appBar_; }
    // Told whenever the reservation changes: on (with the dock's monitor and the work area it
    // leaves) or off.
    void SetAppBarCallback(std::function<void(bool on, const RECT& monitor, const RECT& work)> cb) {
        appBarChanged_ = std::move(cb);
    }
    // Crash path (unhandled exception filter): removes the registration of the dock window that
    // registered one, if any. Best effort, no allocation of our own.
    static void EmergencyRemoveAppBar();

    // Recomputes the layout, moves/resizes the window and redraws everything.
    void Relayout();

    // Re-reads every setting from the config (sizes, theme, clock, monitor, auto-hide) and
    // redraws. Pins are separate: ReloadPins().
    void ApplyConfig();
    // Applies config->Main().autoHide: off shows the dock and keeps it shown; always hides it once the
    // mouse is away; intelligent does too, but only while the foreground window overlaps the dock
    // (and shows it again when it no longer does). Only intelligent installs the WinEvent hooks.
    void ApplyAutoHide();
    // While held (a menu is open, a drag is in progress) the dock never hides. Releasing the
    // hold starts the hide delay if the mouse is outside the dock.
    void SetHold(bool hold);
    // While kept shown (the settings window is active) the dock never hides and is revealed if
    // hidden (unless a fullscreen app runs). Unlike SetHold it does not keep the HWND at stage
    // size and does not suppress the label or previews. Releasing it re-applies auto-hide.
    void SetKeepShown(bool keep);

    // The monitor the dock is on (screen px), its DPI, and the bar at rest (screen px).
    RECT MonitorRect() const { return layout_.monitor; }
    UINT Dpi() const { return dpi_; }
    RECT RestBarRect() const;
    // The renderer runs on WARP (software) rather than a hardware device.
    bool UsingWarp() const { return renderer_.UsingWarp(); }

    // The item under a client-area point (relative to the current window rect), or nullptr.
    const DockItem* ItemAt(POINT clientPt) const;
    // Index of the item under a client-area point (relative to the current window rect), or -1.
    int IndexAt(POINT clientPt) const { return HitTest(ToStage(clientPt)); }
    // A right click at a client-area point: the index of the item whose item menu opens, or -1
    // for the dock menu (no item there, or a widget).
    int MenuIndexAt(POINT clientPt) const;
    // Index of the item owning a tracked top-level window (its window slot when the dock is
    // unstacked), or -1.
    int IndexOfWindow(HWND hwnd) const;
    const std::vector<DockItem>& Items() const { return items_; }

    // Re-resolves the pins of config->Main() (cached per entry), rebuilds the elements from its
    // items and the item list, and starts / stops the weather service when a weather item came or
    // went. Call after changing the item list.
    void ReloadPins();
    // Resolved pins, parallel to the pin items of config->Main() (DockConfig::PinIndexes()).
    const std::vector<AppIdentity::ResolvedPin>& Pins() const { return pins_; }

    // Starts the item's app (newInstance: even if it already has windows) and bounces its icon
    // until a new window appears. Returns false if the launch failed. Folders and the Recycle Bin
    // open in Explorer (no bounce).
    bool LaunchItem(int index, bool newInstance);
    // Shows the folder popup of the folder item at `index`, or hides it if it is showing that item.
    void ToggleFolderPopup(int index);
    // Automation (ItemCommand 814 - 816) on the open folder popup: select / scroll to / open.
    FolderPopup& Folder() { return folder_; }
    // The resolved group pin behind a group slot's key (null: no such group).
    const AppIdentity::ResolvedPin* GroupPin(const AppKey& key) const;
    // Opens / activates member `member` of the group `group` (as a click in its popup does).
    void OpenGroupMember(const AppKey& group, size_t member);

    // --- the weather service (the weather elements' data) and widget popups ---
    // Re-reads config->weather (start / stop / restart the service) and relayouts if the widget's
    // size changed. Also part of ApplyConfig.
    void ApplyWeather();
    // Shows / hides the click popup of the first item of `kind`: the forecast over the weather
    // widget, the month calendar over the clock (no-op without such an item).
    void TogglePopup(ItemKind kind);
    const WeatherService& Weather() const override { return shared_->WeatherData(); }
    struct WeatherCallbacks {
        std::function<void()> openSettings;  // the unconfigured widget was clicked: Settings > Weather
    };
    void SetWeatherCallbacks(WeatherCallbacks callbacks) { weatherCallbacks_ = std::move(callbacks); }

    // --- script widgets ---
    // The script widget shown by slot `index` (null: not a script widget).
    WidgetInstance* WidgetAt(int index) const;
    // Reloads the script widget of slot `index` (false: not one).
    bool ReloadWidget(int index);
    size_t WidgetCount() const { return widgets_.size(); }

    // --- the tray (round 4 phase 8) ---
    // TrayHost's icons changed (or it started / stopped): the tray elements re-measure and redraw,
    // an open drawer follows.
    void OnTrayChanged();
    // Where this dock shows notify icon `id` (a grid cell, the overflow button, the drawer's cell),
    // in screen px. False: not on this dock.
    bool TrayIconRect(uint64_t id, RECT& screen) const;
    // A right click at a client point that an element handles itself (a tray icon: forwarded to
    // its app). False: the dock / item menu as before.
    bool RightClickAt(POINT clientPt, bool ctrl);
    // Automation (ItemCommand 822 / 823): a left / right click on the drawer's icon `icon` of the
    // tray slot `slot`, forwarded as if clicked there. False without such a slot / icon.
    bool ClickTrayIcon(int slot, size_t icon, bool right);
    // The drawer's icons of the tray slot `slot` (empty: not a tray slot / the tray is off).
    std::vector<const TrayIcon*> TrayDrawerIcons(int slot) const;

    // --- notifications (round 4 phase 9) ---
    // The notification list or the service's state changed (or docks[].badges / the exclusions):
    // the badges follow.
    void OnNotificationsChanged();
    // Brings the app with this key to the front from its slot (the app-level slot, else its first
    // window slot): as a click on it does. False: this dock has no slot for it.
    bool ActivateKey(const AppKey& key);
    // Automation (ItemCommand 828 - 830) on the open notification centre: dismiss / activate row
    // `row`, clear all. False when it is not open or there is no such row.
    bool CentreCommand(UINT cmd, size_t row);

    // --- scrolling (round 4 phase 11a) ---
    // Automation (ItemCommand 831 - 833): scroll the run to offset `arg` px at once (831) or with
    // the wheel's glide (832), or just far enough to show slot `arg` (833). 834 (any dock): magnify
    // as if the pointer rested at stage main coordinate `arg` (0xFFFF: as if it left; for tests of
    // the magnified layout without a real pointer). False: the run does not scroll (or no such slot).
    bool ScrollCommand(UINT cmd, int arg);

private:
    class DropTarget;
    friend class DropTarget;

    // All values in physical pixels. Rects are relative to the "stage" (layout.window) unless
    // noted. The stage is the full area the dock can draw into while shown (bar, magnified icons,
    // label, bar growth); the HWND covers all of it only while the mouse is on the dock (see
    // DesiredRect). Visuals are positioned in stage coordinates; the root visual's transform
    // maps them into the HWND's client area.
    //
    // Everything below except `monitor`, `work` and `window` is CANONICAL (DockGeometry.h): x / left
    // / right / "X" names are the main axis (along the bar), y / top / bottom / "Y" names the cross
    // axis, with "bottom" the side at the screen edge, whatever the dock's edge. Phys / Canon map
    // between canonical stage coordinates and physical stage coordinates (identity for a bottom dock).
    struct Layout {
        float scale = 1.0f;
        DockEdge edge = DockEdge::Bottom;
        bool vertical = false; // left / right: the main axis is y
        RECT monitor{};        // screen coordinates (the dock's monitor; DockWindow::MonitorRects)
        RECT work{};           // its work area (popups are kept inside it)
        RECT window{};         // screen coordinates: the stage (physical)
        float stageMain = 0.0f;   // the stage's canonical size
        float stageCross = 0.0f;
        D2D1_RECT_F bar{};     // drawn background at rest
        float cornerRadius = 0.0f;
        float iconPx = 0.0f;   // unmagnified icon size
        UINT bitmapPx = 0;     // icon bitmap size (iconPx * magnification, for crisp magnification)
        float magnification = 1.0f;
        float padding = 0.0f;
        float gap = 0.0f;      // between icons
        float itemsLeft = 0.0f;  // left edge of slot 0 (a slot is iconPx + gap wide; separators, widgets: their box)
        float itemsW = 0.0f;     // all slots and the spacers between them
        int firstMag = -1;       // first / last slot that magnifies (-1: none); the slots before /
        int lastMag = -1;        // after them move with the bar's left / right end
        float itemTop = 0.0f;    // top of the icon box
        float itemHeight = 0.0f; // icon top -> bar bottom
        float magTop = 0.0f;     // top of a fully magnified icon
        float dotTop = 0.0f;     // top of the dot layers
        UINT dotHeight = 0;
        float dotCenterY = 0.0f; // dot-layer-local
        float dotRadius = 0.0f;
        // Hairlines between the icons and the widgets, drawn into the background (bar-local; < 0:
        // none): before the widgets at the end of the dock (moves with the right half) and after
        // the widgets at its start (left half). Widgets between icons get no hairline.
        float separatorX = -1.0f;
        float leftSeparatorX = -1.0f;
        float growMax = 0.0f;     // most the bar can grow while magnified
        float bgSurfaceW = 0.0f;  // background surface width (bar width + growMax)
        float seamX = 0.0f;       // where the two background halves meet (fixed)
        float slide = 0.0f;       // auto-hide slide distance
        float reflH = 0.0f;       // icon reflections under the icon row (the bar's extra height)
        float glowPad = 0.0f;     // how far the glow reaches beyond the icon box (0: no glow)
        float restTopExtra = 0.0f; // the resting HWND reaches this far above the bar (a glow shown at rest)
        // How far the bar's ends may grow toward the stage's start / end while magnified (the stage
        // is kept on the monitor, so a dock at a monitor end grows away from it).
        float growRoomStart = 0.0f;
        float growRoomEnd = 0.0f;
        float popupY = 0.0f;      // popups open beyond this cross coordinate (with kPreviewGap)
        bool fullWidth = false;   // the bar spans the monitor's edge (docks[].fullWidth)
        float slack = 0.0f;       // stage room beside the bar on each side (before a monitor end cuts it)
        float peek = 0.0f;        // px of the bar left visible while hidden (0: the 1 px strip)
        float hiddenSlide = 0.0f; // slide distance of the hidden pose (`slide`, or to the peek strip)
        bool reflClip = false;    // reflections are clipped at the bar's edge (theme.reflectionClip)
        D2D1_RECT_F title{};      // the title's physical stage rect (empty: none)
        // Titles under the icons (docks[].titles; 0 lines: none). Each slot's caption box is
        // captionMain x captionCross (canonical), captionTop on the cross axis (under the icon and
        // any reflection, toward the screen edge), along the bar at the icon box + captionShift.
        int captionLines = 0;
        float captionTop = 0.0f;
        float captionMain = 0.0f;
        float captionCross = 0.0f;
        float captionShift = 0.0f;
        float captionBand = 0.0f;  // what the captions add to the bar's cross size
        // docks[].overflow (round 4 phase 11a). `view`: the scrolling viewport (off: the identity);
        // iconDip: the icon size laid out (docks[].iconSize, smaller with "shrink").
        DockGeometry::Viewport view;
        int iconDip = 0;
        // docks[].shape (round 4 phase 11b): the run wrapped onto a ring (polar.on), the ring at
        // rest (physical stage rect of its band), its inner / outer radius, the hub a hidden circle
        // shrinks to (physical stage rect; empty for an arc) and the scale that takes it there.
        DockShape shape = DockShape::Bar;
        DockGeometry::Polar polar;
        D2D1_RECT_F ring{};
        float ringIn = 0.0f, ringOut = 0.0f;
        D2D1_RECT_F hub{};
        float hubScale = 1.0f;
        bool rotateIcons = false;

        // The mappings (DockGeometry.h): the edge mapping for a bar, the ring for "arc" / "circle".
        D2D1_POINT_2F Phys(float u, float v) const {
            return polar.on ? polar.Point(u, v) : DockGeometry::Point(edge, u, v, stageCross);
        }
        // A canonical rect's physical bounds (a ring sector's bounding box on a polar dock).
        D2D1_RECT_F PhysRect(float u0, float v0, float u1, float v1) const {
            return polar.on ? polar.Bounds(u0, v0, u1, v1) : DockGeometry::Rect(edge, u0, v0, u1, v1, stageCross);
        }
        // An upright box [u0, u1] x [v0, v1] (an icon, a widget, the label): on a bar its PhysRect,
        // on a ring the box's size centred on its mapped centre.
        D2D1_RECT_F BoxRect(float u0, float v0, float u1, float v1) const {
            return polar.on ? polar.Box((u0 + u1) / 2.0f, (v0 + v1) / 2.0f, u1 - u0, v1 - v0) : PhysRect(u0, v0, u1, v1);
        }
        // Physical cross offset (y, or x on vertical docks) of the band [v, v + length] (bars only).
        float CrossOrigin(float v, float length) const { return DockGeometry::CrossOrigin(edge, v, length, stageCross); }
        // A stage pixel -> canonical.
        D2D1_POINT_2F Canon(POINT stagePt) const {
            return polar.on ? polar.Canon(static_cast<float>(stagePt.x) + 0.5f, static_cast<float>(stagePt.y) + 0.5f)
                            : DockGeometry::Canon(edge, stagePt.x, stagePt.y, stageCross);
        }
        // Physical size of a box main x cross long (upright on a ring: main is its width).
        D2D1_SIZE_F PhysSize(float main, float cross) const {
            return polar.on ? D2D1::SizeF(main, cross) : DockGeometry::Size(edge, main, cross);
        }
        // The edge surfaces are drawn for (DrawCanonical): a ring's are drawn as for a bottom dock.
        DockEdge DrawEdge() const { return polar.on ? DockEdge::Bottom : edge; }
    };
    // A canonical stage rect -> screen (truncating, like the bottom dock always did; `outer`: floor /
    // ceil so the rect covers the box).
    RECT StageToScreen(float u0, float v0, float u1, float v1, bool outer = false) const;
    // Draws `layer` (main x cross long) in the dock's orientation: `draw` paints canonical coordinates.
    HRESULT DrawCanonical(Layer& layer, float main, float cross, const std::function<void(ID2D1DeviceContext*)>& draw);
    // The physical offsets of a visual / translate transform along the main / cross axis.
    template <class T, class V>
    HRESULT SetMainOffset(T* target, V v) const {
        return layout_.vertical ? target->SetOffsetY(v) : target->SetOffsetX(v);
    }
    template <class T, class V>
    HRESULT SetCrossOffset(T* target, V v) const {
        return layout_.vertical ? target->SetOffsetX(v) : target->SetOffsetY(v);
    }
    // Where a popup opens for a slot centred on stage main coordinate `anchor`.
    PopupAnchor PopupAnchorAt(float anchor) const;
    // The HWND rect while hidden: the 1 px reveal strip along the monitor's edge, or the peek strip.
    RECT HiddenRect() const;
    // The label's text format (labels and the title), at scale `s`. False without DirectWrite.
    bool EnsureLabelFormat(float s);
    HRESULT RenderTitle();
    // Clips the icons at the bar's edge while reflections are clipped (theme.reflectionClip), and
    // both slot groups at the viewport's ends while the run scrolls.
    void ApplyReflectionClip();

    // --- scrolling (docks[].overflow "scroll"; layout_.view) ---
    // Moves the run to offset `to` (clamped, whole px): every rest position shifts (the layout is in
    // view coordinates), the hover follows the pointer. No visual is touched. False: no change.
    bool ShiftScroll(float to);
    // ShiftScroll, then the slots glide there over `duration` (0: at once; an ease-out, the curve
    // inertia is computed for) with the scroll thumb shown.
    void ScrollTo(float to, double duration);
    // Wheel notches (delta / WHEEL_DELTA; + = toward the end) scroll the run.
    void ScrollByWheel(int delta);
    // Scrolls just far enough to show slot `index` whole. False: no such slot / not scrolling.
    bool ScrollSlotIntoView(int index);
    // Press on the bar's background and move: the run follows the pointer; the release flings it on.
    void BeginScrollDrag();
    void UpdateScrollDrag(POINT stagePt);
    void EndScrollDrag(bool fling);
    // A drag (an icon, or an OLE drag over the dock) at main coordinate `main`: near a viewport end
    // the run scrolls that way (kAutoScrollTimerId) until it leaves the zone or the run ends.
    void UpdateAutoScroll(float main);
    void StopAutoScroll();
    void OnAutoScrollTimer();
    // The scroll thumb: drawn per size, placed along the viewport, shown while scrolling and faded
    // out kScrollHint after the last move.
    HRESULT RenderScrollThumb();
    void PlaceScrollThumb(double duration);
    void ShowScrollHint();
    void OnScrollHintTimer();
    // The edge-fade opacity of slot `item` for its current placement curve (scrolling docks; a
    // non-scrolling dock touches nothing unless an earlier layout faded the slot).
    void DriveEdgeFade(DockItem& item, size_t index, double now);
    void SetEdgeOpacity(DockItem& item, float value, IDCompositionAnimation* anim);

    // --- rings (docks[].shape "arc" / "circle"; layout_.polar) ---
    // Places slot `item`'s visuals (the icon container or the widget, the dot) on the ring for its
    // `tx` curve: upright boxes centred on the mapped centre, moving along the arc (sampled curves,
    // DockGeometry::PathSegments), or set at once. `fresh`: also its scale centre (the side facing
    // the ring's centre, so it grows outward) and, with rotateIcons, its turn.
    void PlaceOnRing(DockItem& item, double now, bool fresh);
    // A ring at rest: the window region is the band, so the hole inside it goes to the windows
    // below (no region otherwise).
    void UpdateWindowRegion();
    // The ring's box at rest on screen.
    RECT RingScreenRect() const;
    // rotateIcons: the icon surfaces' turn (item.turn) about the box centre.
    void ApplyIconTurn(DockItem& item);
    // Registers / moves / removes the app bar per config (after a layout pass).
    void UpdateAppBar();
    void RemoveAppBar();
    void OnAppBarNotify(WPARAM wp, LPARAM lp);
    // The reserved screen rect (the bar's band along its edge).
    RECT AppBarRect() const;

    // --- drag the dock (Ctrl + drag on the bar, unlocked) ---
    void BeginDockDrag();
    void UpdateDockDrag();
    void EndDockDrag(bool commit);
    // The edge whose zone the screen point is in: the current edge, or another one when the point
    // is in the outer quarter of the monitor next to it (and nearer to it than to the others).
    DockEdge DockZoneAt(POINT screenPt) const;

    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);

    HMONITOR PickMonitor() const;
    // The dock's monitor and its work area (screen px): the one place the layout gets them from.
    void MonitorRects(RECT& monitor, RECT& work) const;
    // ComputeLayoutAt with the configured icon size, and with "shrink" a smaller one until it fits.
    void ComputeLayout();
    void ComputeLayoutAt(int iconDip);
    // The HWND rect (screen) for the current state: the 1 px strip while Hidden, the whole stage
    // while expanded_ (mouse on the dock), the bar column down to the screen edge while sliding,
    // and just the drawn bar while Shown at rest.
    RECT DesiredRect() const;
    // Places the HWND at DesiredRect() and re-asserts topmost (layout passes). The caller commits.
    void ApplyWindowPos();
    // Resizes the HWND to DesiredRect() if it differs, committing the matching root translation
    // first (no z-order change, no activation). Re-arms mouse-leave tracking.
    void UpdateWindowRect();
    // Sets the root visual's transform so stage coordinates land at the right screen place in
    // the HWND at winRect_.
    void ApplyStageTransform();
    // Client point of the HWND at winRect_ -> stage point.
    POINT ToStage(POINT clientPt) const {
        return POINT{clientPt.x + winRect_.left - layout_.window.left, clientPt.y + winRect_.top - layout_.window.top};
    }
    // Something still draws outside the bar (magnification, label, return-to-rest, bounce, menu).
    bool NeedsStage() const;
    // Shrinks the HWND back to the bar once nothing needs the stage.
    void TryCompact();
    // Layout + window placement (handles DPI changes caused by moving monitors).
    void LayoutPass();
    // Layout after the item set changed: redraws only what changed.
    void RelayoutItems();

    HRESULT CreateLayers();
    HRESULT CreateItemVisuals(DockItem& item);
    void ResetLayers();
    HRESULT RenderBackground();
    HRESULT RenderItem(DockItem& item);
    // A widget slot: its element draws the content into the slot's icon surface.
    HRESULT RenderWidget(DockItem& item);
    // The item's glow and reflection surfaces (theme icon effects; empty when off), from the icon
    // as `drawIcon` draws it at a size.
    HRESULT RenderEffects(DockItem& item, const std::function<void(ID2D1DeviceContext*, float)>& drawIcon);
    // The glow opacity the item should show now (glow mode, hover, active app; 0 without a surface).
    float GlowTarget(const DockItem& item) const;
    // Drives the item's glow opacity to GlowTarget over `duration` (0: static). Committed by the caller.
    void DriveGlow(DockItem& item, double duration);
    void UpdateGlows(double duration);
    // Sets / removes the theme's perspective tilt (a static DirectComposition 3D transform) on the
    // background, the widgets and the items. Called after every layout snap.
    void ApplyPerspective();
    HRESULT RenderSeparator(DockItem& item);
    // A thumbnail tile (docks[].thumbnails): the window's latest frame (dimmed while minimized),
    // the app icon in a corner; the app icon alone until a frame exists.
    HRESULT RenderTile(DockItem& item, ID2D1Bitmap* icon);
    // --- window thumbnails ---
    // The tile's length along the bar (px) for this layout.
    float TilePx() const;
    // Hands the tile windows and the frame size to thumbs_ (after a rebuild / config change) and
    // whether the dock is visible.
    void SyncThumbnails();
    void OnThumbnailFrames(const std::vector<HWND>& windows);
    HRESULT RenderDotSources();
    HRESULT RenderDot(DockItem& item);
    // --- notification badges (docks[].badges) ---
    // Recomputes every slot's badgeCount from the notification service (marks changed slots
    // badgeDirty). True if any changed.
    bool UpdateBadges();
    // The slot's badge surfaces: the count in a bubble at the icon's outer corner, or nothing.
    HRESULT RenderBadge(DockItem& item);
    // --- titles under the icons (docks[].titles) ---
    // The text a slot shows under its icon / in its hover label: a window slot's caption, else the
    // app / item name (the Recycle Bin's label has its count; separators and widgets: none).
    std::wstring SlotTitle(const DockItem& item) const;
    std::wstring CaptionFor(const DockItem& item) const;
    // Lines of title text under the icons for this layout (0: off; vertical docks with
    // titlesVertical "off": 0).
    int CaptionLines() const;
    bool EnsureCaptionFormat(float s);
    // Creates / removes the item's caption visual (titles on / off) and draws its text.
    HRESULT RenderCaption(DockItem& item);
    HRESULT RenderLabel();
    // Redraws every layer (all) or only dirty ones, and positions everything.
    HRESULT DrawFrame(bool all);
    void RenderAll();
    // Hands `e` to every element; the ones whose content changed are re-measured and only their
    // slots redrawn (a new size relays the dock out).
    void DispatchEvent(DockEvent e);
    // Re-measures the dirty widget slots: relays the dock out when a size changed, else redraws them.
    void RefreshDirtyWidgets();
    // Runs `step`, then commits. On device loss, rebuilds devices and redraws everything.
    void RenderWithRecovery(const std::function<HRESULT()>& step);
    bool RecoverDevice();

    // --- placement / magnification ---
    // Computes every target position (slots, background halves, label) from the rest
    // layout and, while magnifying, the cursor x.
    void ComputeTargets();
    // Drives all placement tweens to their targets (duration 0: static snap). `trackingMove`: a
    // retarget while the mouse moves (motion_.magnify; always ease-out, coarser epsilons);
    // otherwise a settle animation (rest, landing; the configured easing, exact targets).
    void ApplyTargets(double duration, bool trackingMove = false);
    void ResetPlacementTweens();
    void ScheduleMagnify();
    void ApplyMagnify();
    void SetBigIcons(bool big);
    // The mouse left the dock (window or active zone): return to rest, hide the label, start
    // the hide delay.
    void LeaveZone();
    bool InZone(POINT stagePt) const;
    bool CursorInZone() const;

    // --- hover label ---
    void SetHover(int index);
    void OnLabelTimer();
    // Hides the label: fades it out over motion_.labelFade (kLabelFadeTimerId hides the visual at
    // the end) or at once. Committed by the caller.
    void HideLabel();
    void OnLabelFadeTimer();
    // Ends a running fade-out now (the visual is hidden). Committed by the caller.
    void FinishLabelFade();
    // Drives the label visual's opacity (no-op without IDCompositionVisual3).
    void DriveLabelOpacity(float to, double duration);

    // --- thumbnail popup (multi-window items) ---
    // The hovered item can show the popup now (previews on, 2+ windows, dock shown, not held).
    bool PreviewEligible(int index) const;
    // Hover changed (called by SetHover): arm the delay, switch the popup's item, or dismiss.
    void UpdatePreviewHover();
    void OnPreviewTimer();
    void OnPreviewLeaveTimer();
    void StartPreviewLeave();
    void CancelPreviewLeave();
    // Shows the popup for items_[index] (or switches it to that item).
    void ShowPreview(int index, bool keepAnchor = false);
    // Hides the popup and releases its hold on the dock. With `leaveCheck`, a cursor outside the
    // dock then counts as having left it (return to rest, hide delay).
    void DismissPreview(bool animate, bool leaveCheck = true);
    // Dismisses the popup because of a click / menu on the dock and suppresses it for the
    // hovered item (see previewSuppressed_).
    void SuppressPreview();
    // The item list changed: follow the popup's app (its windows) or dismiss it.
    void RefreshPreview();
    void OnPreviewHover(bool inside);

    // --- folder popup (folder items) ---
    // Shows the popup for items_[index] (a folder), centred on stage x `anchor`; takes kHoldFolder.
    void ShowFolderPopup(int index, float anchor);
    // Hides the popup and releases its hold (leaveCheck: see ReleasePopupHold).
    void DismissFolderPopup(bool animate, bool leaveCheck = true);
    // The popup closed by itself (deactivated, Escape).
    void OnFolderPopupClosed();
    // The item list changed: dismiss the popup if its folder item is gone (a group's popup
    // follows its members).
    void RefreshFolderPopup();
    // --- group popup (group items: the same popup window) ---
    void ShowGroupPopup(int index, float anchor);
    // The members as the popup shows them (icons from the dock's cache).
    std::vector<FolderPopup::GroupEntry> GroupEntries(const AppIdentity::ResolvedPin& group);
    // Name + member keys / names: the popup is refreshed when this changes.
    static std::wstring GroupSignature(const AppIdentity::ResolvedPin& group);
    FolderPopup::Style PopupStyle() const;
    // A member dragged out of the popup: an OLE drag the dock (any dock) takes as "pull it out".
    void DragGroupMember(const AppKey& group, size_t member);

    // --- weather / calendar popups (centred on stage x `anchor`) ---
    void ShowWeatherPopup(float anchor);
    void DismissWeatherPopup(bool animate, bool leaveCheck = true);
    void ShowCalendarPopup(float anchor);
    void DismissCalendarPopup(bool animate, bool leaveCheck = true);
    // Every click popup (folder, weather, calendar).
    void DismissClickPopups(bool animate, bool leaveCheck = true);
    bool ClickPopupOnScreen() const {
        return folder_.OnScreen() || weatherPopup_.OnScreen() || calendar_.OnScreen() || widgetPopup_.OnScreen() ||
               centre_.OnScreen();
    }
    // --- the notification centre (round 4 phase 9b), centred on stage main coordinate `anchor` ---
    void ShowCentre(float anchor);
    void DismissCentre(bool animate, bool leaveCheck = true);
    NotificationPopup::Style CentreStyle() const;
    // --- script widget popups (posted requests, see DockWindow.cpp) ---
    void OnWidgetPopupRequests();
    void DismissWidgetPopup(bool animate, bool leaveCheck = true);
    void ShowFolderPopupAt(const std::wstring& path, const std::wstring& name, float anchor, const AppKey& key,
                           ItemView view);
    // The slot that shows widget `w` (-1: none) / the instance with this key (null: none).
    int SlotOfWidget(const WidgetInstance& w) const;
    WidgetInstance* WidgetByKey(const std::wstring& key) const;
    // Popup chrome from the theme (scale, acrylic, tint, colours, monitor).
    WeatherPopup::Style WeatherPopupStyle() const;

    // --- Recycle Bin item ---
    // Registers for the bin's shell change notifications while a Recycle Bin item exists.
    void UpdateRecycleBinWatch();
    void OnRecycleBinChanged();
    // "Recycle Bin (N items, 12.3 MB)" / "Recycle Bin (empty)".
    static std::wstring RecycleBinLabel();

    // --- popup holds ---
    // An open popup keeps the dock shown (part of Held()): one bit per popup in popupHolds_.
    enum PopupHold : uint32_t {
        kHoldPreview = 1u << 0,
        kHoldFolder = 1u << 1,
        kHoldWeather = 1u << 2,
        kHoldCalendar = 1u << 3,
        kHoldWidget = 1u << 4,  // a script widget's popup
        kHoldCentre = 1u << 5,  // the notification centre
    };
    // Takes the hold for `bit` (cancels a pending hide).
    void TakePopupHold(uint32_t bit);
    // Releases it. With `leaveCheck`, once no hold remains a cursor outside the dock counts as
    // having left it (return to rest, compact, start the hide delay).
    void ReleasePopupHold(uint32_t bit, bool leaveCheck);
    bool Held() const { return hold_ || popupHolds_ != 0 || keepShown_; }

    // --- acrylic backdrop ---
    // Creates / destroys the backdrop window per config and system settings (call before a
    // relayout: the bar's tint and corner radius depend on it).
    void UpdateAcrylic();
    // Moves the backdrop window to where the bar is drawn right now (following the bar-edge and
    // slide tweens), hides it while the dock is hidden, and keeps a ~60 Hz one-shot timer chain
    // running only while one of those tweens is still animating.
    void SyncBackdrop();

    // --- animation settings ---
    // Re-reads config->animation and the system's animation setting into motion_ (and the
    // renderer's default easing, the popup's fade). Called by ApplyConfig and on
    // WM_SETTINGCHANGE(SPI_SETCLIENTAREAANIMATION).
    void RefreshMotion();
    // Settings > Accessibility > Visual effects > Animation effects is off.
    static bool SystemReducesMotion();

    // --- auto-hide ---
    void ScheduleHide();
    void CancelHide();
    void OnHideTimer();
    void OnRevealTimer();
    void OnSlideTimer();
    void StartHide();
    void StartShow();
    // Drives the root visual's slide offset / opacity to the pose of hideState_ for
    // motion_.hide (slide: both; slideOnly: offset, the opacity snaps to 0 once Hidden; fade:
    // opacity; none: instant). Returns the duration actually used (shorter for a partial reversal).
    double ApplyRootPose(double duration);
    bool CursorOnStrip() const;
    void ArmMouseTracking();
    // A fullscreen app, game or presentation runs (SHQueryUserNotificationState): no reveal.
    bool RevealSuppressed() const;
    // Intelligent hide: the foreground window (not ours, not minimized / hidden / cloaked, not
    // the desktop or a shell surface) intersects the bar's column from the rest bar top down to
    // the monitor bottom.
    bool ForegroundOverlaps() const;
    // Re-tests the overlap (watch_ events, ApplyAutoHide) and hides / shows on a change (always
    // with `force`). A show blocked by RevealSuppressed() leaves overlapped_ untouched so the next
    // event re-tests; ScheduleHide / OnHideTimer re-test on their own anyway.
    void OnOverlapCheck(bool force = false);
    // Puts the (hidden) dock back on top of the z-order: a monitor-sized window that was just
    // activated can be placed above a topmost window and would bury the reveal strip.
    void AssertTopmost();
    // docks[].layer "desktop" (DesktopLayer.h): shown only while the desktop is the foreground,
    // directly above the desktop window, never topmost.
    bool DesktopLayered() const { return DockCfg().layer == DockLayer::Desktop; }
    // Shows / hides a desktop dock per the foreground (and shows a normal one that was hidden).
    void UpdateDesktopShown();
    // Tells every script widget whether the dock is on screen (dock.visible, onVisibilityChange):
    // shown or showing, and not a hidden desktop dock.
    void SyncWidgetVisibility();
    // Subscribes to / leaves the shared foreground watch.
    void SetWatchingForeground(bool on) {
        if (on == watching_ || !shared_) return;
        watching_ = on;
        shared_->SetWatching(this, on);
    }

    void ResolvePins();
    // The shared snapshot as this dock shows it (runningFilter "monitor": only the windows on its
    // monitor), then Rebuild.
    void RebuildFrom(const WindowTracker::Snapshot& snap);
    // Recreates elements_ from config->Main().items (call ResolvePins first; Rebuild after).
    void BuildElements();
    // ApplyConfig / ApplyWeather: the item list may have changed (the clock / weather toggles).
    // If it did (or with `force`), re-resolves the pins, rebuilds the elements and the item list;
    // otherwise every element re-reads the config. Either way the elements' timers restart.
    void SyncElements(bool force = false);
    // Rebuilds items_: every element's slots in config order (the running element expands into
    // the unpinned running apps).
    void Rebuild(const WindowTracker::Snapshot& snap);
    // The element that contributes slot `index`.
    DockElement& ElementOf(size_t index) const { return *elements_[items_[index].element]; }
    LayoutContext Context() const;

    // --- ElementHost (what elements ask of the dock; Hwnd and Weather are public) ---
    const Config& Cfg() const override { return *config_; }
    const DockConfig& DockCfg() const override {
        return config_->docks[dockIndex_ < config_->docks.size() ? dockIndex_ : 0];
    }
    const ThemeConfig& DockTheme() const override { return config_->ThemeOf(DockCfg()); }
    void SlotWindows(int slot, WindowsCommand cmd) override;
    void OpenTarget(const std::wstring& target) override;
    void ActivateOrLaunch(int slot) override;
    void LaunchSlot(int slot, bool newInstance) override { LaunchItem(slot, newInstance); }
    bool PopupShownFor(int slot) const override;
    void ShowClickPopup(int slot) override;
    void OpenWeatherSettings() override {
        if (weatherCallbacks_.openSettings) weatherCallbacks_.openSettings();
    }
    WidgetInstance* AcquireWidget(const DockItemEntry& entry, size_t ordinal) override;
    D2D1_RECT_F SlotScreenBox(int slot) const override;
    TrayHost* Tray() override;
    ID2D1Bitmap* TrayBitmap(const TrayIcon& icon) override;
    // The pointer over a slot's cell (DockElement::CellAt; -1: none): the hover label shows the
    // cell's label, centred on it; the element hears Enter / Move / Leave.
    void SetHoverCell(int slot, int cell, std::wstring label, float centre);
    // --- the tray drawer (the group popup in tray mode) ---
    void ShowTrayDrawer(int index, float anchor);
    void ShowTrayMenu(int slot, const std::wstring& key, bool pinnedSlot) override;
    // An in-process OLE drag of tray icon `key` (private format): a dock of this process takes it
    // as a trayApp pin at its insertion marker, a tray grid as a new place in its order.
    void DragTrayIcon(const std::wstring& key);
    // The key of a tray icon drag of this process (empty: not one).
    static std::wstring ReadTrayDrag(IDataObject* data);
    // A trayApp slot's picture at the icon bitmap size (the HICON scaled; the app's own larger
    // icon resource when the handle still knows it). Null: its app does not run (placeholder).
    ID2D1Bitmap* TrayAppBitmap(const DockItem& item);
    std::unordered_map<uint64_t, winrt::com_ptr<ID2D1Bitmap>> trayBigBitmaps_;
    std::unordered_map<std::wstring, uint64_t> trayAppSerials_;  // slot key -> the picture last drawn
    size_t UnreadNotifications() const override;
    bool NotificationsDenied() const override;
    bool Magnifying() const override { return magnifying_; }
    bool ReducedMotion() const override { return motion_.reduce; }
    // --- WidgetInstance::Client ---
    void WidgetChanged(WidgetInstance& widget) override;
    void WidgetPopupRequest(WidgetInstance& widget) override;
    bool WidgetPopupShown(const WidgetInstance& widget) const override;
    bool WidgetActivateNotification(uint32_t id) override;
    bool WidgetOpenCentre(WidgetInstance& widget) override;
    // The pointer is over widget slot `index` now (-1: none): onHover leave / enter.
    void UpdateWidgetHover(int index);

    // Launch feedback per motion_.launch: bounce / hop (DirectComposition OffsetY animation) or
    // pulse (opacity animation); no timers while idle.
    void StartBounce(DockItem& item, size_t windowsBeforeLaunch);
    void FinishBounce(DockItem& item);
    void OnBounceTimer();
    void ScheduleBounceTimer();
    void UpdateIconUsage();
    // The slot under a stage point (rest layout, inside the active zone; widgets with their hit
    // slack), or -1.
    int HitTest(POINT stagePt) const;
    // Index of the rest slot containing stage x (-1 outside every slot: beyond the ends, in a spacer).
    int SlotAt(float stageX) const;
    // Width of an item's box: the icon, or a separator's / widget's whole slot.
    float BoxW(const DockItem& item) const {
        return item.Magnifies() ? (item.boxMain > 0.0f ? item.boxMain : layout_.iconPx) : item.slotW;
    }
    // Height (cross axis) of an item's box: the icon, or a widget's content.
    float BoxCross(const DockItem& item) const { return item.IsWidget() ? item.boxCross : layout_.iconPx; }
    // The point (box-local, physical) an icon box scales about: the middle of its edge-facing side.
    D2D1_POINT_2F FootOf(const DockItem& item) const {
        return DockGeometry::Point(layout_.edge, BoxW(item) / 2.0f, layout_.iconPx, layout_.iconPx);
    }
    // Rest slot left edge -> icon box left edge.
    float BoxInset(const DockItem& item) const { return item.Magnifies() ? std::floor(layout_.gap / 2.0f) : 0.0f; }

    // --- drag to reorder (Goal: macOS-style icon dragging) ---
    // Press on an item and move past the system drag threshold: the icon follows the cursor,
    // the others open a gap at the insertion slot. Release commits the order (or, far above the
    // bar, unpins a pinned app with no windows); Escape or losing the capture cancels.
    enum class DragPhase { None, Dragging, Removing };
    void BeginDrag(POINT stagePt);
    void UpdateDrag(POINT stagePt);
    void EndDrag(bool commit);
    // Abandons a drag without animation (the item list changed under it).
    void AbortDrag();
    // Hold-to-group: the slot a drag at stage point `pt` would group with (-1: none). A pin (app /
    // folder) or a group whose rest slot's middle half is under the pointer, on the icon row;
    // the dragged item must be an app or folder (not a group, a widget or a window slot).
    int GroupCandidateAt(POINT stagePt) const;
    void OnGroupHoldTimer();
    void ResetGroupTarget();
    void OnRemoveTimer();
    // Where an internal drag at stage x would put the dragged item (see DockWindow.cpp): sets
    // dragToPins_, dragPins_ / dragRunning_ (the new orders), dragOrder_ (the slots in the order
    // they would be shown) and dragInsert_ (the dragged item's index in it).
    void UpdateDragTarget(float stageX);
    // The same for a dragged widget: it moves among the other elements (the running apps are one
    // block), dragMoveTo_ is the config item it would go before.
    void UpdateElementDragTarget(float stageX);
    // The dragged item back in its own place (dragOrder_ = the rest order).
    void ResetDragOrder();
    // An item a pin / running-app drag can be dropped on (a pin or an unpinned running app; not a
    // widget).
    bool Droppable(size_t index) const { return !items_[index].IsWidget(); }
    // A press on slot `index` may start a drag: every slot but a window slot (unstacked docks
    // keep their app order), unless the dock is locked.
    bool Draggable(size_t index) const {
        // A pinned tray icon's press drags the icon itself (DragTrayIcon); reorder it in Settings.
        return index < items_.size() && !DockCfg().locked && !items_[index].window &&
               items_[index].kind != DockItem::Kind::TrayApp;
    }
    // Gap slot (an index into items_, at most items_.size()) for an external drop at stage x: before
    // a pin, after the last pin, or (no pins) before the first running app; -1 if there is none.
    int DropGapIndex(float stageX) const;
    bool DragInRemoveZone(POINT stagePt) const;
    bool DragRemovable() const;
    // The pin index (DockConfig::PinIndexes()) of the item at `itemIndex` (the pin count past the pins).
    size_t ConfigIndexOfItem(size_t itemIndex) const;
    void SetItemOpacity(DockItem& item, float to, double duration);
    // A dragged widget is shown in front of the icons: its visual moves into itemsGroup_ for the
    // drag (front) and back into widgetsGroup_ after it (both carry the same perspective tilt).
    void SetWidgetFront(DockItem& item, bool front);
    // Keeps the HWND at stage size until an animation of `seconds` has ended.
    void KeepStage(double seconds);

    // --- external file drops (OLE IDropTarget on the dock HWND) ---
    // What a drop at the pointer would do (QueryState 26): pin pinnable files at an insertion
    // marker, open the files with the app item under the pointer, or recycle them on the bin.
    // Widget: onto a script widget that takes files (onDrop + the "shell" permission).
    // Group: onto a group's icon (pinnable files become members).
    enum class DropMode { None = 0, Insert = 1, OpenWith = 2, Recycle = 3, Widget = 4, Group = 5, TrayOrder = 6 };
    HRESULT RenderMarker();
    DWORD OnDragEnter(IDataObject* data, POINT screenPt, DWORD allowed);
    DWORD OnDragOver(POINT screenPt, DWORD allowed);
    void OnDragLeave();
    DWORD OnDrop(IDataObject* data, POINT screenPt, DWORD allowed);
    // The main (along the bar) stage coordinate of a screen point.
    float DropMainAt(POINT screenPt) const {
        return layout_.Canon(POINT{screenPt.x - layout_.window.left, screenPt.y - layout_.window.top}).x;
    }
    // The mode, target item (OpenWith / Recycle) and gap (Insert) for a drop at stage x.
    DropMode DropModeAt(float stageX, int* item, int* gap) const;
    // Switches the drop feedback: the marker in a gap (Insert), the target icon scaled up
    // (OpenWith / Recycle), or nothing.
    void SetDropTarget(DropMode mode, int item, int gap);
    // The dropped files' open-with / recycle action (kDropActionMessage).
    void OnDropAction();
    // Every file of a CF_HDROP drag.
    static std::vector<std::wstring> DroppedFiles(IDataObject* data);
    // The ones that can be pinned: .lnk, .exe and folders.
    static std::vector<std::wstring> PinnableFiles(const std::vector<std::wstring>& files);

    // Animation timing at speed 1 (seconds).
    static constexpr double kMagnifyDurS = 0.08;  // retarget animation while the mouse moves
    static constexpr double kRestDurS = 0.12;     // back to rest after the mouse leaves; landing
    static constexpr double kSlideDurS = 0.18;    // auto-hide slide + fade
    static constexpr double kBounceHalfS = 0.3;   // rise (= fall) time of one launch bounce
    static constexpr double kRemoveFadeS = 0.15;  // fade-out of an icon dragged off the dock
    static constexpr double kPreviewFadeS = 0.12; // thumbnail popup fade in / out

    // config->animation resolved against the system setting (RefreshMotion). Durations are in
    // seconds, already divided by animation.speed; 0 means instant.
    struct Motion {
        bool reduce = false;     // effective reduce-motion
        double scale = 1.0;      // 1 / speed; 0 when speed == 0 or reduce-motion is in effect
        HideStyle hide = HideStyle::Slide;
        LaunchStyle launch = LaunchStyle::Bounce;  // None when scale == 0
        Falloff falloff = Falloff::Cosine;
        Easing easing = Easing::EaseOut;
        // Input smoothing while the mouse moves: NOT scaled, kept with reduce-motion; 0 only
        // when speed == 0.
        double magnify = kMagnifyDurS;
        double rest = kRestDurS;
        double slide = kSlideDurS;  // 0 for hideStyle "none"
        double bounceHalf = kBounceHalfS;
        double removeFade = kRemoveFadeS;
        double labelFade = 0.12;
        double previewFade = kPreviewFadeS;
    };

    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    const Config* config_ = nullptr;
    MessageHook hook_;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    Motion motion_;
    bool inLayout_ = false;
    bool snapPending_ = true;
    UINT msgQueryItem_ = 0;
    UINT msgQueryState_ = 0;
    RECT winRect_{};          // where the HWND currently is (screen)
    bool expanded_ = false;   // the HWND covers the whole stage (mouse on the dock)

    Renderer renderer_;
    // Background: one surface (bar width + growMax wide) shown by two visuals, each inside a
    // container clipped at a fixed seam: the left half moves with the bar's left edge and the
    // right half with its right edge, so the bar grows/shrinks without redrawing.
    Layer bgLeftClip_;
    Layer bgLeft_;
    Layer bgRightClip_;
    Layer bgRight_;
    float bgSeparators_[2] = {-2.0f, -2.0f};  // the hairlines the background was drawn with (layout_ values)
    Layer widgetsGroup_;  // container of the widget slots' visuals (stage origin), behind the icons
    Layer itemsGroup_;    // container of the icon / separator slots' visuals
    bool perspectiveApplied_ = false;  // the 3D tilt is set on the groups (QueryState 32 bit 2)
    // The three indicator-dot looks (inactive, active, attention), drawn once and shared by every
    // item's dot visual (each DComp surface has a sizeable fixed cost). The source visuals are hidden.
    Layer dotSources_[3];
    Layer label_;
    Layer title_;
    winrt::com_ptr<IDWriteTextLayout> titleLayout_;  // docks[].title laid out (null: none)
    std::wstring titleText_;
    float titleScale_ = 0.0f;
    bool titleDirty_ = true;
    Layout layout_;
    Tween bgLeftX_, bgRightX_, labelX_, labelY_;
    float bgLeftTarget_ = 0.0f, bgRightTarget_ = 0.0f;
    float labelXTarget_ = 0.0f, labelYTarget_ = 0.0f;

    size_t dockIndex_ = 0;          // config_->docks[dockIndex_] (DockCfg)
    HMONITOR monitor_ = nullptr;    // the dock's monitor (null: the primary)
    DockShared* shared_ = nullptr;  // the manager's shared services
    bool tracking_ = false;  // Create has taken the tracker's first snapshot: item rebuilds wait for it
    // Per dock (not shared): the bitmaps are resources of this dock's own D2D device.
    IconCache icons_;
    // One per config item of config->Main() (an unknown kind: null), in config order.
    std::vector<std::unique_ptr<DockElement>> elements_;
    // The script widgets of the `widget` items, keyed "<id>#<n>" (the n-th item with that id):
    // they outlive the elements (BuildElements drops the ones no item claims any more).
    std::unordered_map<std::wstring, std::unique_ptr<WidgetInstance>> widgets_;
    std::vector<std::wstring> widgetsClaimed_;  // keys AcquireWidget handed out during BuildElements
    bool widgetRedrawPosted_ = false;           // kWidgetRedrawMessage is posted
    AppKey hoverWidget_;                        // the widget slot under the pointer (empty: none)
    ScriptPopup widgetPopup_;                   // the script popup (one at a time)
    std::wstring popupWidget_;                  // the instance key it belongs to ("" none)
    std::wstring previewOpener_;                // the widget that opened the preview popup (popup.previews)
    std::wstring calendarOpener_;               // the widget that opened the calendar popup (popup.calendar)
    std::vector<std::wstring> popupRequests_;   // instance keys with a pending popup request
    bool popupRequestPosted_ = false;           // kWidgetPopupMessage is posted
    std::vector<DockItem> items_;  // the slots, in dock order
    bool evictPending_ = false;
    std::vector<AppIdentity::ResolvedPin> pins_;                       // parallel to the pin items
    // Keyed by kind + icon + "p:"+path / "a:"+aumid (the special kinds are not cached).
    std::unordered_map<std::wstring, AppIdentity::ResolvedPin> pinCache_;

    int hovered_ = -1;            // never a widget slot (no label, no preview)
    int pressed_ = -1;            // the slot the current left press started on
    // That press was on the slot whose click popup was open (DockElement::OnPress): the release
    // must not open it again (a click on a folder / the clock / the weather toggles).
    bool pressClosedPopup_ = false;
    int pressedMiddle_ = -1;
    bool trackingMouse_ = false;

    // Magnification
    bool magnifying_ = false;
    float cursorX_ = 0.0f;        // stage main coordinate (canonical x) of the last mouse position in the zone
    double lastMagnify_ = 0.0;    // Renderer::Now() of the last applied update
    bool magnifyPending_ = false;
    bool restPending_ = false;
    bool bigIcons_ = false;       // magnified-size icon surfaces are the visible ones

    // Hover label
    bool labelShown_ = false;
    bool labelDirty_ = true;
    bool labelPending_ = false;
    bool labelFading_ = false;    // kLabelFadeTimerId running (fading out; the visual is still shown)
    Tween labelOpacity_;
    winrt::com_ptr<IDCompositionVisual3> label3_;  // label_'s opacity (null: no fade)
    int labelItem_ = -1;          // item the label surface was drawn for
    float labelScale_ = 0.0f;
    winrt::com_ptr<IDWriteTextFormat> labelFormat_;
    winrt::com_ptr<IDWriteInlineObject> labelEllipsis_;
    size_t labelChars_ = 0;       // length of the text the label shows (QueryState 74)

    // Titles under the icons (docks[].titles): the format (the label's font, smaller), its line
    // height, and the caption band in the layout (canonical: from captionTop, captionCross long;
    // main length captionMain per slot, centred on the icon box).
    winrt::com_ptr<IDWriteTextFormat> captionFormat_;
    winrt::com_ptr<IDWriteInlineObject> captionEllipsis_;
    float captionScale_ = 0.0f;
    float captionLineH_ = 0.0f;
    bool builtUnstacked_ = false;  // the last Rebuild made window slots (docks[].windows)
    bool builtThumbnails_ = false; // the last Rebuild made thumbnail tiles (docks[].thumbnails)
    size_t titleUpdates_ = 0;      // live caption changes applied (QueryState 76)

    // Window thumbnails (docks[].thumbnails): the frames per window, and the coalesced redraw.
    Thumbnails thumbs_;
    bool thumbRedrawPosted_ = false;
    std::vector<HWND> thumbDirty_;  // windows with a new frame since the last redraw

    // Acrylic backdrop
    Backdrop backdrop_;
    bool acrylic_ = false;            // the backdrop window exists and the bar uses the acrylic tint
    float acrylicRadiusDip_ = 8.0f;   // corner radius DWM rounds the blur with
    bool backdropPending_ = false;

    // Auto-hide
    HideState hideState_ = HideState::Shown;
    Tween rootY_, rootOpacity_;
    bool hold_ = false;
    bool keepShown_ = false;      // SetKeepShown: part of Held(), not of NeedsStage()
    uint32_t popupHolds_ = 0;     // PopupHold bits of the popups that keep the dock shown
    bool hidePending_ = false;
    bool revealPending_ = false;
    bool revealArmed_ = true;     // false until the cursor leaves the strip (no reveal loops)
    bool watching_ = false;       // subscribed to the shared foreground watch (intelligent / desktop layer)
    bool desktopHidden_ = false;  // a desktop-layer dock hidden because the desktop is not the foreground
    bool overlapped_ = false;     // intelligent: the foreground window overlaps the dock

    PinRequests pinRequests_;

    // App bar
    bool appBarAllowed_ = false;  // AllowAppBar
    bool appBar_ = false;         // registered (ABM_NEW)
    bool appBarBusy_ = false;     // inside UpdateAppBar (ABM_SETPOS broadcasts a relayout)
    RECT appBarRect_{};           // the rect last set (ABM_SETPOS)
    bool fullscreenApp_ = false;  // ABN_FULLSCREENAPP: a fullscreen app is open, the dock is not topmost
    std::function<void(bool, const RECT&, const RECT&)> appBarChanged_;

    // Dragging the dock
    bool dockPress_ = false;      // a Ctrl + left press on the bar (a drag may start)
    POINT dockPressScreen_{};     // where (screen)
    bool dockDrag_ = false;       // moving the dock
    LONG dockShift_ = 0;          // how far the window has been moved along the edge (px)
    DockEdge dockZone_ = DockEdge::Bottom;  // the edge a drop would put the dock on

    // Drag to reorder
    POINT pressPt_{};             // stage point of the last left press on an item
    DragPhase dragPhase_ = DragPhase::None;
    int dragSource_ = -1;         // item index of the dragged icon
    AppKey dragKey_;
    float dragGrabDx_ = 0.0f;     // cursor x - icon box left edge when the drag started
    int dragInsert_ = -1;         // final item index of the dragged icon if dropped now
    bool dragToPins_ = false;     // dropped now, the new order is the pins' (else the running apps')
    bool dragWidget_ = false;     // a widget is dragged: it moves its config item (dragMoveTo_)
    size_t dragMoveTo_ = SIZE_MAX;  // the config item the dragged widget would go before (SIZE_MAX: the end)
    std::vector<size_t> dragPins_;     // pin slots (item indexes) in their new order
    std::vector<size_t> dragRunning_;  // unpinned running slots in their new order
    std::vector<size_t> dragOrder_;    // every slot in the order the drag layout shows them
    bool dragRemove_ = false;     // released now, the pin would be removed
    bool dragLanding_ = false;    // targets for the dropped icon landing in its slot
    bool dragPending_ = false;    // kDragTimerId running
    bool removePending_ = false;  // kRemoveTimerId running
    AppKey removeKey_;
    // Order of the running (unpinned) section after the user dragged within it; empty until then.
    // Session only (not saved): Rebuild sorts that section by it and appends new apps at the end.
    std::vector<AppKey> runningOrder_;

    // Thumbnail popup
    PreviewPopup preview_;
    bool previewPending_ = false;       // kPreviewTimerId running
    bool previewLeavePending_ = false;  // kPreviewLeaveTimerId running
    int previewItem_ = -1;              // item the popup shows
    AppKey previewKey_;
    HWND previewWindow_ = nullptr;      // ... and its window slot's window (null: an app's slot)
    PopupAnchor previewAnchor_{};       // where the popup opens (centred on its `along`)
    // Dismissed by a click or a menu while the mouse stays on the item: no popup for that item
    // again until the mouse hovers another item or leaves the dock.
    bool previewSuppressed_ = false;
    AppKey previewSuppressKey_;
    HWND previewSuppressWindow_ = nullptr;

    // Folder popup (also a group's)
    FolderPopup folder_;
    AppKey folderKey_;            // the folder / group item the popup shows (empty: none)
    std::vector<uint64_t> drawerIds_;  // the tray drawer's members (icon ids), when it shows the drawer
    std::wstring drawerSignature_;     // ... their ids + picture serials + labels when it was filled
    // Hover cells (a tray slot): the slot and cell under the pointer (-1: none), its label, its
    // centre along the bar from the slot box's start (px).
    int hoverCellSlot_ = -1;
    int hoverCell_ = -1;
    std::wstring hoverCellLabel_;
    float hoverCellCentre_ = 0.0f;
    // Notify icon pictures on this dock's device, keyed by TrayIcon::imageSerial.
    std::unordered_map<uint64_t, winrt::com_ptr<ID2D1Bitmap>> trayBitmaps_;
    std::wstring groupSignature_; // the group popup's GroupSignature when it was filled

    // Hold-to-group (a pin dragged onto another pin and held)
    int groupCandidate_ = -1;     // the slot under the dragged icon that it may group with
    int groupTarget_ = -1;        // ... armed (held long enough, or a group): a drop groups
    bool groupHoldPending_ = false;  // kGroupHoldTimerId running
    // A group member dragged out of the popup (OLE, from any dock of this process) is over the dock.
    AppKey memberDragGroup_;
    size_t memberDragIndex_ = SIZE_MAX;
    // A tray icon dragged (DragTrayIcon, from a grid cell, a trayApp slot or the drawer): its key
    // while it is over this dock; the key under the press that may start one.
    std::wstring trayDragKey_;
    std::wstring pressTrayKey_;

    // The notification centre and the widget that asked for it ("": the element's).
    NotificationPopup centre_;
    std::wstring centreOpener_;
    std::wstring centreRequest_;  // a widget's pending openCentre (kCentreRequestMessage)
    // The forecast and calendar popups (the weather data is shared_->WeatherData())
    WeatherPopup weatherPopup_;
    CalendarPopup calendar_;
    WeatherCallbacks weatherCallbacks_;
    bool weatherQuiet_ = false;   // Configure is running inside ApplyConfig / ApplyWeather

    // Recycle Bin item
    ULONG binNotify_ = 0;         // SHChangeNotifyRegister id (0: not registered)
    bool binPending_ = false;     // kRecycleBinTimerId running

    // External drops
    DropTarget* dropTarget_ = nullptr;     // the registered IDropTarget (one reference held)
    bool dropActive_ = false;     // a file drag is over the dock
    bool dropAccept_ = false;     // the current drag carries files
    bool dropPinnable_ = false;   // ... and some of them can be pinned (Insert mode)
    DropMode dropMode_ = DropMode::None;
    int dropItem_ = -1;           // OpenWith / Recycle: the target item (scaled up)
    // A drop on an item, run once Drop has returned (kDropActionMessage).
    struct DropAction {
        DropMode mode = DropMode::None;
        AppKey key;
        std::vector<std::wstring> files;
        POINT screen{};  // where it was dropped (a widget gets box-local coordinates)
    };
    DropAction dropAction_;
    int dropGap_ = -1;            // slot the gap is open at (-1: none)
    Layer marker_;                // insertion marker
    Tween markerX_;
    float dropMain_ = 0.0f;       // main coordinate of the last OLE drag-over point (auto-scroll)

    // Scrolling (docks[].overflow "scroll"): the offset survives relayouts (clamped to the new
    // maximum); scrollShown_ is its bookkeeping curve (what the slots show now, QueryState 140).
    float scroll_ = 0.0f;
    Tween scrollShown_;
    unsigned fadeSerial_ = 1;     // bumped when the viewport or its fading ends change
    bool fadeStartShown_ = false, fadeEndShown_ = false;  // the ends the slots were faded for
    // Wheel deltas below a notch add up (precision touchpads).
    int wheelRemainder_ = 0;
    // Drag-scrolling the bar.
    bool scrollPress_ = false;    // a left press on the bar's background of a scrolling dock
    bool scrollDrag_ = false;     // ... moved: the run follows the pointer
    float scrollPressMain_ = 0.0f;
    float scrollPressOffset_ = 0.0f;
    struct ScrollSample {
        double t;
        float main;
    };
    ScrollSample scrollSamples_[8]{};
    size_t scrollSampleCount_ = 0;
    // Auto-scroll during a drag: px per second (signed, + = toward the end), its last tick.
    float autoScrollSpeed_ = 0.0f;
    double autoScrollLast_ = 0.0;
    bool autoScrollPending_ = false;
    // The scroll thumb (created on first use: a dock that never scrolls has no visual for it).
    Layer scrollThumb_;
    Tween thumbMain_, thumbOpacity_;
    float thumbLen_ = 0.0f;
    bool hintShown_ = false;
    bool hintPending_ = false;
    // Slot placements driven / only tracked (scrolled out) in the last ApplyTargets (QueryState 137).
    size_t placedDriven_ = 0, placedSkipped_ = 0;

    // Rings: a circle's root transform (scale about its centre, then the stage translation) and
    // its scale's curve; the window region is set.
    winrt::com_ptr<IDCompositionScaleTransform> rootScale_;
    winrt::com_ptr<IDCompositionTranslateTransform> rootTranslate_;
    Tween rootScaleT_;
    bool regionSet_ = false;
    bool ringShown_ = false;    // the background shows a ring (its right half is hidden)
    bool bgRing_ = false;       // the background surface was drawn as a ring
    RECT backdropRegion_{};     // the blur window's rect its ring region was made for
    LONG dockShiftY_ = 0;  // a circle dragged (Ctrl + drag): the vertical part of its move
};
