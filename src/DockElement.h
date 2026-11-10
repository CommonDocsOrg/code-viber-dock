#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "AppIdentity.h"
#include "Config.h"
#include "DockItems.h"
#include "Renderer.h"
#include "WindowTracker.h"

class TrayHost;
class WeatherService;
class WidgetInstance;
struct TrayIcon;

// The dock's contents. Every docks[].items[] entry becomes one DockElement, and the dock lays the
// elements out in config order. An element contributes 0..n slots (DockItem: the icon row's unit
// of layout, magnification, placement tweens, bounce and dot): app / folder / separator /
// recycleBin one each (an entry resolving to an app that is already on the dock: none), `running`
// one per unpinned running app, clock / weather one widget slot sized by Measure. The dock owns
// the slots, the window, layout, magnification, drags, drops, popups and auto-hide; an element
// knows its kind and config entry, measures its slots, draws non-icon content, and handles input
// on its slots. Elements are rebuilt whenever the item list changes (they hold no visuals).

// Layout axes, so that no element bakes in x or y. Main: along the bar (x on bottom / top docks, y
// on left / right docks); cross: across it, perpendicular to the screen edge (the height of a box
// on a horizontal dock, its width on a vertical one). The dock maps main / cross onto the screen
// for its edge (DockGeometry.h); an element only measures. Content an element draws (Render) stays
// upright: on a vertical axis it lays itself out to be narrow (the clock stacks its words, the
// weather puts its glyph above the temperature) and reports its height as `main`.
enum class DockAxis { Horizontal, Vertical };

struct ElementSize {
    float main = 0.0f;
    float cross = 0.0f;
    // Round 11 phase 11C: a script widget's measure() returned `flush: true` (the Show desktop
    // strip). On a full-width bar its box is the bar's whole thickness (LayoutContext::barCross;
    // `cross` is that then) and, as the first or the last slot, it takes no end padding: it touches
    // the screen's end, as Windows 7's "Show desktop" sliver does.
    bool flush = false;
    bool operator==(const ElementSize&) const = default;
};

struct LayoutContext {
    DockAxis axis = DockAxis::Horizontal;
    float scale = 1.0f;   // DPI scale (physical px per DIP)
    float iconPx = 0.0f;  // the unmagnified icon box, both axes
    IDWriteFactory* dwrite = nullptr;
    // Round 11 phase 11C: the bar's thickness at rest (px: the icon row, its padding and any
    // titles; no reflections) on a full-width bar, 0 elsewhere (a centred bar, a ring, a floating
    // bar, a standalone widget, a preview): what a flush box's cross becomes.
    float barCross = 0.0f;
    // Round 16 phase 16B: the cross room a widget box has at rest inside the bar: the icon row
    // (rowCross), the captions band and docks[].thickness's extra, px (0: unknown, treat as
    // iconPx). The tray grid fits more cells across a thick bar with it.
    float rowRoom = 0.0f;
};

// Something outside the dock changed that an element may show.
enum class DockEvent {
    ClockTick,    // the clock's timer (DockWindow::kClockTimerId) fired
    TimeChanged,  // WM_TIMECHANGE, resume from sleep
    WeatherData,  // the weather service's data or status changed
    WidgetRedraw, // a script widget asked to be redrawn (WidgetInstance::TakeChanged)
    TrayChanged,  // TrayHost's icons came, went or changed (or it started / stopped)
    Notifications,  // the notification list or the service's state changed (round 4 phase 9)
    Calendar,       // the calendar feeds' occurrences changed (round 4 phase 13)
};

// The click popups the dock hosts (one at a time; each holds the dock shown while open).
enum class ClickPopup { None, Folder, Weather, Calendar, Group, Tray, Notifications };

// What a right click on a slot opens.
enum class RightClickAction { ItemMenu, DockMenu };

// The pointer and a slot's cells (DockElement::CellAt): it entered a cell, moved within it,
// rested on it for the label delay, or left the cells.
enum class CellEvent { Enter, Move, Dwell, Leave };

// Something to do with the windows of a slot's app (ElementHost::WindowsCommand).
enum class WindowsCommand { Minimize, Close, Next, Previous };

// What elements may ask of the dock (DockWindow implements it).
class ElementHost {
public:
    static constexpr UINT_PTR kClockTimerId = 1;  // = DockWindow::kClockTimerId

    virtual HWND Hwnd() const = 0;
    virtual const Config& Cfg() const = 0;
    // This dock's own settings (the per-dock keys: middle click, wheel, previews ...).
    virtual const DockConfig& DockCfg() const = 0;
    // The theme this dock draws with (its own themePreset, or the global theme).
    virtual const ThemeConfig& DockTheme() const = 0;
    // The weather service of a place (round 5: one per place; null while none runs for it). Look it
    // up when it is used (a service nobody needs any more is dropped).
    virtual const WeatherService* WeatherFor(const WeatherConfig& place) const = 0;
    // Brings the slot's app to the front (cycles / minimizes its windows), or starts it.
    virtual void ActivateOrLaunch(int slot) = 0;
    // Starts the slot's app (newInstance: even if it has windows) with launch feedback; folders and
    // the Recycle Bin open in Explorer.
    virtual void LaunchSlot(int slot, bool newInstance) = 0;
    // The slot's click popup (its element's Popup()) is open for this slot.
    virtual bool PopupShownFor(int slot) const = 0;
    // Opens the slot's click popup, centred on its element's PopupAnchor.
    virtual void ShowClickPopup(int slot) = 0;
    // The settings of slot `slot`'s item (round 5: the generated settings page; the unconfigured
    // weather widget was clicked).
    virtual void OpenItemSettings(int slot) = 0;
    // Minimizes / closes the slot's windows, or brings its next / previous window to the front.
    virtual void SlotWindows(int slot, WindowsCommand cmd) = 0;
    // Opens an app / file / URL the user configured (Launcher::OpenTarget; beeps on failure).
    virtual void OpenTarget(const std::wstring& target) = 0;
    // The script widget instance for a `widget` item (the `ordinal`-th item with that id): the
    // dock owns the instances and keeps them across element rebuilds. Null without a device.
    virtual WidgetInstance* AcquireWidget(const DockItemEntry& entry, size_t ordinal) = 0;
    // The slot's box where it is drawn now, in screen pixels.
    virtual D2D1_RECT_F SlotScreenBox(int slot) const = 0;
    // The dock is magnifying (the pointer is on it).
    virtual bool Magnifying() const = 0;
    // Reduce-motion is in effect (animation.reduceMotion, or the system setting).
    virtual bool ReducedMotion() const = 0;
    // The tray takeover (null: not running: config tray.enabled off, or it could not start).
    virtual TrayHost* Tray() = 0;
    // A notify icon's picture as a bitmap of the dock's device (cached by its imageSerial).
    virtual ID2D1Bitmap* TrayBitmap(const TrayIcon& icon) = 0;
    // The tray's own menu for icon `key` at the pointer (Ctrl + right click on a grid cell or a
    // pinned tray icon): Hide / Show in the grid, Pin to the dock / Unpin, Tray settings….
    // `pinnedSlot`: slot `slot` is a trayApp item.
    virtual void ShowTrayMenu(int slot, const std::wstring& key, bool pinnedSlot) = 0;
    // Notifications (round 4 phase 9): unread ones (every app; 0 while the service is off) and
    // whether access is denied.
    virtual size_t UnreadNotifications() const = 0;
    virtual bool NotificationsDenied() const = 0;
    // Calendars (round 4 phase 13): events today on the enabled feeds (0 while the service is off).
    virtual size_t CalendarEventsToday() const = 0;

protected:
    ~ElementHost() = default;
};

// Rebuild state handed to every element in dock order: the window snapshot, the resolved pins, and
// the slot list being built (slots of the previous list are reused by key, keeping their visuals,
// tweens, bounce and windows).
class SlotBuilder {
public:
    SlotBuilder(const WindowTracker::Snapshot& snap, const std::vector<AppIdentity::ResolvedPin>& pins,
                std::vector<AppKey>& runningOrder, std::vector<DockItem>& previous, std::vector<DockItem>& next);

    const WindowTracker::Snapshot& Snapshot() const { return snap_; }
    const std::vector<AppIdentity::ResolvedPin>& Pins() const { return pins_; }
    // Order of the unpinned running apps after the user dragged among them (session only).
    std::vector<AppKey>& RunningOrder() { return runningOrder_; }
    // A pinned app (kind app, not a folder / separator / bin) has this key: its windows show on its pin.
    bool IsPinnedApp(const AppKey& key) const;
    // The new list has an app-level slot (a pin, or an app slot that is not a window slot) with
    // this key.
    bool Has(const AppKey& key) const;
    // Appends a slot for `key` (owned by the current element): the previous slot of that key (and
    // window) moved over, or a fresh one (*fresh = true). `window`: a window slot (docks[].windows
    // "unstacked") for that one window of the app. `pin`: the slot is the app's pin; it also takes
    // over the previous pin of that key whatever window it held (unstacked: the pin slot is the
    // app's first window, which changes as windows come and go). The reference is valid until the
    // next Add.
    DockItem& Add(const AppKey& key, bool* fresh = nullptr, HWND window = nullptr, bool pin = false);
    // docks[].windows is "unstacked": one slot per window.
    bool unstacked = false;
    // The `running` element that expands into the unpinned running apps (the first one) is done.
    bool runningPlaced = false;

    // For the dock: the element Add assigns slots to, and which previous slots were reused.
    void SetElement(size_t index) { element_ = index; }
    bool Taken(size_t previousIndex) const { return taken_[previousIndex]; }

private:
    const WindowTracker::Snapshot& snap_;
    const std::vector<AppIdentity::ResolvedPin>& pins_;
    std::vector<AppKey>& runningOrder_;
    std::vector<DockItem>& previous_;
    std::vector<DockItem>& next_;
    std::vector<bool> taken_;
    size_t element_ = 0;
};

class DockElement {
public:
    DockElement(ElementHost& host, const DockItemEntry& entry) : host_(host), entry_(entry), kind_(entry.Kind()) {}
    DockElement(const DockElement&) = delete;
    DockElement& operator=(const DockElement&) = delete;
    virtual ~DockElement() = default;

    ItemKind Kind() const { return kind_; }
    const DockItemEntry& Entry() const { return entry_; }

    // --- slots ---
    // Appends this element's slots for the current state (0..n), in order.
    virtual void AppendSlots(SlotBuilder& out) = 0;

    // --- layout ---
    // Brings the content up to date before a layout (the clock's text). True if it changed (the
    // dock redraws the element's slots).
    virtual bool Update() { return false; }
    // The content box of each of its slots. Icon slots: the icon (the dock adds the gap around it).
    virtual ElementSize Measure(const LayoutContext& ctx) = 0;
    // How far beyond the box (main axis, rest layout) a press still hits the slot.
    virtual float HitSlack(const LayoutContext&) const { return 0.0f; }
    // Round 13 phase 13D: the gap (px) around each of its icon slots (half on each side, so between
    // two slots of different elements it is the mean of their gaps): `themeGap` (the theme's
    // iconGap, scaled) unless the element has its own (the running item's `gap`). RunPadding: px
    // before its first slot and after its last (the running item's `padding`); a spacer like a
    // slot's lead, never magnified.
    virtual float SlotGap(const LayoutContext&, float themeGap) const { return themeGap; }
    virtual float RunPadding(const LayoutContext&) const { return 0.0f; }

    // --- rendering ---
    // Draws a slot's non-icon content (Measure's size) into `content`. Icon and separator slots are
    // drawn by the dock (icon cache, effects), so the default does nothing.
    virtual HRESULT Render(Renderer& /*renderer*/, Layer& /*content*/, const LayoutContext&) { return S_OK; }

    // --- input (`slot`: index in the dock's slot list) ---
    // Left button down on the slot. True when the press is on the slot whose click popup is open:
    // the dock closes the popup and the release must not open it again (a click toggles).
    virtual bool OnPress(int slot) { return Popup() != ClickPopup::None && host_.PopupShownFor(slot); }
    // Left click (press and release on the slot). `pressClosedPopup`: what OnPress returned.
    virtual void OnClick(int slot, bool pressClosedPopup) = 0;
    virtual void OnMiddleClick(int) {}
    // A wheel turn over the slot (WHEEL_DELTA units; horizontal: the tilt wheel). True if consumed.
    virtual bool OnWheel(int, int /*delta*/, bool /*horizontal*/) { return false; }
    virtual RightClickAction OnRightClick(int) const { return RightClickAction::ItemMenu; }
    // The pointer entered / left the slot (widget slots only: icons have the hover label).
    virtual void OnHover(int /*slot*/, bool /*inside*/) {}
    // A script widget element's instance (null for every other kind).
    virtual WidgetInstance* Widget() const { return nullptr; }
    // A clock element: the minutes its zone is ahead of the local time (QueryState 219). False for
    // every other kind.
    virtual bool ClockOffset(int& /*minutes*/) const { return false; }
    // A weather element: its place (WeatherOf its item). False for every other kind.
    virtual bool WeatherPlace(WeatherConfig& /*place*/) const { return false; }

    // --- cells (a widget slot with several targets of its own: the tray grid) ---
    // The cell of slot `slot` under screen point `screen` (-1: none), its hover label (empty: the
    // dock shows none, the rest still counts as a dwell) and the cell's centre along the bar,
    // relative to the start of the slot's box (px).
    virtual int CellAt(int /*slot*/, POINT /*screen*/, std::wstring* /*label*/, float* /*centre*/) const { return -1; }
    // The pointer and cell `cell` of the slot (Leave: `cell` is the one it left).
    virtual void OnCellHover(int /*slot*/, int /*cell*/, CellEvent) {}
    // A right click on the slot (`ctrl`: with Ctrl held). True: handled (the dock opens no menu).
    virtual bool OnRightClickAt(int /*slot*/, bool /*ctrl*/) { return false; }
    // Tray: where notify icon `id` is shown by this element (its cell, or the overflow button when
    // it is not in the grid), in screen px. False: this element does not show it.
    virtual bool TrayIconRect(int /*slot*/, uint64_t /*id*/, RECT& /*screen*/) const { return false; }
    // Tray automation: icons drawn in the grid (-1: not a tray), whether the overflow button shows.
    virtual int TrayCells() const { return -1; }
    virtual bool TrayOverflow() const { return false; }
    // Tray: the key of the icon under screen point `screen` (a grid cell, a pinned tray icon;
    // empty: none). Pressed and dragged, the dock starts the icon's drag (pin it, reorder the grid).
    virtual std::wstring TrayKeyAt(int /*slot*/, POINT /*screen*/) const { return {}; }

    // --- popups ---
    // The click popup this element opens (the dock hosts it).
    virtual ClickPopup Popup() const { return ClickPopup::None; }
    // Main-axis stage coordinate a slot's popup is centred on: the icon where it is drawn now.
    virtual float PopupAnchor(const DockItem& slot, const LayoutContext& ctx) const {
        return slot.targetX + ctx.iconPx / 2.0f;
    }

    // --- config, timers, outside changes ---
    // The config was re-read (sizes, theme, clock format...).
    virtual void Configure() {}
    // Starts the element's timers (the dock stops them all first: the clock's tick).
    virtual void Start() {}
    // Something outside changed. True if the content changed: the dock re-measures the element,
    // redraws only its slots, and relays the dock out if the size changed.
    virtual bool OnEvent(DockEvent) { return false; }

protected:
    ElementHost& host_;

private:
    DockItemEntry entry_;
    ItemKind kind_;
};

// The element for a config item: `pinIndex` is its position among the pins (DockConfig::PinIndexes,
// for app / folder / separator / recycleBin), `ordinal` its position among the items of its kind.
// nullptr for a kind this version does not know (such an item is not shown).
std::unique_ptr<DockElement> MakeElement(ElementHost& host, const DockItemEntry& entry, size_t pinIndex,
                                         size_t ordinal);

// WidgetElements.cpp
std::unique_ptr<DockElement> MakeClockElement(ElementHost& host, const DockItemEntry& entry, size_t ordinal);
std::unique_ptr<DockElement> MakeWeatherElement(ElementHost& host, const DockItemEntry& entry, size_t ordinal);
// TrayElements.cpp: the tray grid (`tray` item) and a pinned notify icon (`trayApp`, 8b).
std::unique_ptr<DockElement> MakeTrayElement(ElementHost& host, const DockItemEntry& entry, size_t ordinal);
std::unique_ptr<DockElement> MakeTrayAppElement(ElementHost& host, const DockItemEntry& entry);
// Every notify icon a dock's tray drawer lists, in the tray item's order (`order` keys first, then
// registration order): the icons TrayIcon::Shown(); the grid leaves out `hidden` ones and those
// pinned as trayApp items. `inGrid`: only those the grid shows.
std::vector<const TrayIcon*> TrayIconsFor(const TrayHost& host, const DockConfig& dock, const DockItemEntry& tray,
                                          bool inGrid);
// NotificationElements.cpp: the notification centre's bell (`notifications` item, phase 9b).
std::unique_ptr<DockElement> MakeNotificationsElement(ElementHost& host, const DockItemEntry& entry, size_t ordinal);
// A script widget (`widget` item, docs/widgets.md); `ordinal`: among the items with its id.
std::unique_ptr<DockElement> MakeScriptWidgetElement(ElementHost& host, const DockItemEntry& entry, size_t ordinal);
// What a script widget on this dock sees as `dock` (edge, axis, theme colours, reduce motion ...;
// the box and the hover / visible state are the instance's own).
struct ScriptDockInfo WidgetDockInfo(const ElementHost& host);
