#pragma once

#include <windows.h>
#include <commctrl.h>

#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "AppIdentity.h"
#include "Config.h"
#include "DarkMode.h"
#include "Weather.h"

// What a settings change affects; the host applies it to the dock, then saves (debounced).
enum class SettingsScope {
    Appearance,  // sizes, magnification, theme, clock, monitor: DockWindow::ApplyConfig
    AutoHide,    // the window's dock's autoHide: DockWindow::ApplyAutoHide
    Pins,        // the items of the window's dock (Contents, an Item window): DockWindow::ReloadPins
    Taskbar,     // config.hideTaskbar: TaskbarController::SetEnabled
    Autostart,   // config.startWithWindows: the HKCU Run value
    Weather,     // the weather items / config.weather: DockWindow::ApplyWeather (the services follow)
    Notifications,  // config.notifications, docks[].badges: DockManager::SyncNotifications
    Calendars,   // config.calendars: DockManager::ConfigureCalendar
    Dashboard,   // config.dashboard: App registers the hotkey again (round 6 phase 6A)
    Fullscreen,  // config.settings.fullscreenHide: DockManager::SyncFullscreenGuard (round 9 phase 9A)
    SaveOnly,    // read from the config when used (delays, renderer): just save
    WindowSize,  // round 8 phase 8A: a settings window's size (config settings.windows): just save, no refresh
};

// Docks and displays page edits of config.docks (SettingsHost::editDocks).
// TrayDock (8b): a new dock holding only a tray item at the bottom right (the edited dock's tray
// item moves into it), the takeover turned on.
// AddFloating (round 6 phase 6A): a floating bar dock (layer "desktop", centred on the primary
// display's work area, the default items).
enum class SettingsDockEdit { Add, Duplicate, Remove, TrayDock, AddFloating, ToggleEnabled /* round 11 phase 11D */ };

// One dock window for the Layout page (SettingsHost::dockRects).
struct SettingsDockRect {
    size_t cfg = 0;    // index into config.docks
    int monitor = -1;  // monitor index (DockManager order: primary first); -1 parked
    RECT bar{};        // the bar at rest, screen px (empty when parked); a folder window's / widget's rect
    bool folderWindow = false;  // a desktop folder window (round 4 phase 12), not a dock
    bool widgetWindow = false;  // a standalone widget (round 6 phase 6B), not a dock
};

// A notify icon for the Tray page (SettingsHost::trayIcons).
struct SettingsTrayIcon {
    std::wstring key;    // TrayIcon::key (the item's `hidden` / `order` entries)
    std::wstring title;  // its tooltip's first line (or the exe)
    std::wstring app;    // the owner's exe file name
};

// The notification service for the Notifications page (SettingsHost::notificationStatus).
struct SettingsNotificationStatus {
    int state = 0;        // 0 off, 1 polling, 2 denied (NotificationService::State)
    std::wstring reason;  // denied: why
    size_t listed = 0;
    size_t unread = 0;
};
// An app the Notifications page lists for its badge check box (SettingsHost::notificationApps).
struct SettingsNotificationApp {
    std::wstring key;   // AppKey::id (what notifications.excluded holds)
    std::wstring name;
    size_t unread = 0;
};

// A calendar feed's status for the Calendar page (SettingsHost::calendarFeeds), parallel to
// config.calendars.feeds.
struct SettingsCalendarFeed {
    std::wstring status;  // "Updated 10:15, 42 events", "Error: HTTP 404; retry in 2 min", "Off"
};

// A metric of the perf provider for a `"choiceFrom": "perf"` field (SettingsHost::perfMetrics).
struct SettingsPerfMetric {
    std::wstring key;    // "cpu.total"
    std::wstring name;   // "CPU total"
    std::wstring group;  // "CPU" ...; HWiNFO: the sensor's name
    bool hwinfo = false;
};

// A widget's rendered preview (SettingsHost::renderPreview; round 6 phase 6E).
struct SettingsWidgetPreview {
    HBITMAP bitmap = nullptr;  // premultiplied 32-bit top-down DIB section; null: none
    SIZE px{};
    std::wstring error;        // why there is none
    double ms = 0.0;           // what the render took
};

// Round 12 phase 12A: the app's timeline and save state as every settings window shows them
// (SettingsHost::status): the band's status line, the bar at the top of the page area, the ⋯
// menu's Undo / Redo / Revert this window.
struct SettingsStatus {
    int save = 0;             // 0 saved, 1 pending (the debounced save, or held by a preview), 2 the last save failed
    int bar = 0;              // the bar: 0 none, 1 Keep / Revert, 2 Keep / Cancel (a preview), 3 a failed save
    std::wstring barText;     // "Dock moved to the left edge.", "Previewing Modern on main.", "Couldn't save: …"
    std::wstring cancelText;  // the second button: "Revert (12 s)" / "Cancel"
    std::wstring undoLabel;   // what Undo / Redo would undo / redo ("": nothing)
    std::wstring redoLabel;
    int sinceOpen = 0;        // the window's changes since it opened (Revert this window)
};
// What a window asks of the timeline (SettingsHost::timeline).
enum class SettingsAction { Undo, Redo, Keep, Cancel, Retry, OpenFolder, RevertWindow };

// Everything a settings window may use of the app, so it never sees App internals. App binds one
// host per window (round 5 phase B): "the dock" below is the window's own dock (SettingsWindow::
// DockIndex(), looked up by id each time; the Global window: the row selected on its Docks and
// displays page, which only that page acts on), its first window.
struct SettingsHost {
    Config* config = nullptr;                              // &App::config_, edited in place (stable address)
    // A setting changed: App applies it and saves. `dock` is the config dock it is about (the
    // window's dock unless the page says otherwise, SettingsPage::ChangedFor).
    std::function<void(SettingsScope, size_t dock)> changed;
    std::function<const std::vector<AppIdentity::ResolvedPin>&()> pins;
    std::function<void(const std::vector<std::wstring>&)> pinFiles;  // App::PinFiles(paths, end)
    std::function<void(UINT)> command;                     // App::OnCommand (ContextMenu ids) for the dock
    std::function<bool()> usingWarp;
    std::function<RECT()> dockMonitor;
    std::function<UINT()> dockDpi;
    // The window became the active window (true; not while minimized) or stopped being it / is
    // destroyed (false). App keeps a dock shown while its own dock or item window is active.
    std::function<void(bool active)> activated;
    // Round 5 phase B: the other windows.
    std::function<void(size_t dock, int tab)> openDock;     // a config dock's (or folder window's) own window (tab < 0: as it is)
    std::function<void(size_t dock, size_t item)> openItem; // an item's own window (App::OpenItemSettings)
    std::function<void(int page)> openGlobal;               // the Global window (page < 0: as it is)
    std::function<void()> flashDock;                        // "Flash this dock" (DockWindow::Flash)
    std::function<RECT()> anchor;                           // an Item window: its dock's window (empty: not open)
    // Beyond the plan's interface:
    std::function<RECT()> dockBar;            // the dock's bar at rest (screen px): stay above it
    std::function<bool()> taskbarHidden;      // the taskbar controller is enabled (session state)
    std::function<bool()> autostartEnabled;   // the HKCU Run value exists
    std::function<void()> destroyed;          // the window is gone: delete the object (posted)
    // The generated settings page's Place field (round 5: per item): the status line of a place's
    // service (WeatherService::StatusText; "Not shown on a dock" while none runs for it), and
    // Refresh now for it.
    std::function<std::wstring(const WeatherConfig& place)> weatherStatus;
    std::function<void(const WeatherConfig& place)> refreshWeather;
    // Round 6 phase 6D: the perf provider's metrics (the running sampler's, else a short sample).
    std::function<std::vector<SettingsPerfMetric>()> perfMetrics;
    // Starts a place search; `done` gets the places (or an error) later, on the UI thread, unless
    // a newer search replaced it or the window closed. False if it could not be started.
    std::function<bool(const std::wstring& query,
                       std::function<void(const std::vector<WeatherService::Place>&, const std::wstring& error)> done)>
        geocode;
    // Docks and displays page. Adds a dock (index ignored; appended), duplicates (inserted after
    // it) / removes docks[index] (never the last one); the app re-syncs the dock windows, closes
    // the windows of a removed dock, refreshes every settings window and saves. False if refused.
    std::function<bool(SettingsDockEdit, size_t index)> editDocks;
    // The dock windows of docks[index] (0: parked, its monitor is not connected).
    std::function<int(size_t index)> dockWindows;
    // Layout page: every dock instance (an "each" dock once per monitor) and where its bar is.
    std::function<std::vector<SettingsDockRect>()> dockRects;
    // Tray page: the notify icons the tray host has now (empty while it is off).
    std::function<std::vector<SettingsTrayIcon>()> trayIcons;
    // Notifications page: the service's state, and every dock's apps plus every app with a
    // notification now (by key, once each).
    std::function<SettingsNotificationStatus()> notificationStatus;
    std::function<std::vector<SettingsNotificationApp>()> notificationApps;
    // Calendar page: each configured feed's status line.
    std::function<std::vector<SettingsCalendarFeed>()> calendarFeeds;
    // General page (round 6 phase 6A): the dashboard hotkey is registered (false: none, or another
    // app holds that combination) and dashboard mode is on.
    std::function<bool()> hotkeyRegistered;
    std::function<bool()> dashboardOn;
    // Round 6 phase 6B: a standalone widget of widget `id` (the manifest's default size, layer
    // "behind", centred on the monitor of config dock `nearDock`; SIZE_MAX: the primary display);
    // its Dock window opens when `open`. Returns the new entry's config index (SIZE_MAX: refused).
    std::function<size_t(const std::wstring& id, size_t nearDock, bool open)> addWidgetWindow;
    // Round 6 phase 6E: widget `id`'s live preview at `scale` px per DIP, fitted into `fitPx` (a
    // smaller widget drawn up to `maxZoom` times larger; DockManager::RenderPreview: a premultiplied 32-bit top-down DIB the window then owns; null
    // bitmap + error: it failed),
    // the stamp of what previews depend on (the global theme, units, reloads, DockCommand 148: a
    // window drops its cached previews when it changes) and the theme's background (ARGB: the
    // panel behind a preview).
    std::function<SettingsWidgetPreview(const std::wstring& id, float scale, SIZE fitPx, float maxZoom)> renderPreview;
    std::function<uint64_t()> previewStamp;
    std::function<uint32_t()> previewPanel;
    // Round 9 phase 9B: the settings search dialog owned by this window (Ctrl+F, the Global nav's
    // Find a setting… box, the Dock header's ⋯ menu), opened or brought forward with `text` typed
    // into its box (empty: as it is).
    std::function<void(const std::wstring& text)> openSearch;
    // Round 9 phase 9C: the preset chooser, modal to this window: `mode` 0 apply to config dock
    // `dock` (the Dock window's ⋯ menu), 1 the welcome variant (About), 2 a new dock (Docks and
    // displays > Add ▾).
    std::function<void(int mode, size_t dock)> openPresets;
    // Round 9 phase 9D: the Help window at `topic` ("guide-settings.md#behaviour-tab"; F1, the
    // cards' "?" buttons, the Global nav's Help row, About's Quick start guide).
    std::function<void(const std::wstring& topic)> openHelp;
    // Round 10 phase 10D: a dialog of the window's own is open (true) / closed (false): App holds
    // what a dock's dialog holds (App::modalDepth_: no Reload config, no dock removal, display
    // changes wait), so the window and its item are still there when the dialog returns.
    std::function<void(bool open)> modal;
    // Round 12 phase 12A: the timeline and the save state. `status`: what the bands show now;
    // `timeline`: Undo, Redo, the bar's buttons, Revert this window (bound to the window); `label`:
    // the one-line label of the change being made (the first label of a gesture wins: a page's own
    // ChangeLabel comes before the window's generic "<row label>: <value>").
    std::function<SettingsStatus()> status;
    std::function<bool(SettingsAction)> timeline;
    std::function<void(const std::wstring& label)> label;
    // Round 12 phase 12B: the change the window is about to make is a Keep / Cancel preview (a
    // gallery pick): `label` its entry on Keep ("Applied the Glass look to main"), `text` the bar's
    // ("Previewing the Glass look on main.").
    std::function<void(const std::wstring& label, const std::wstring& text)> preview;
    // --- round 12 phase 12C: ContextMenu::kDuplicateDock / kSaveDockLayout (its name dialog owned by
    // this window) / kRenameDock (the dock's Dock window with its name being edited) for config dock
    // `dock`. False: refused. ---
    std::function<bool(UINT command, size_t dock)> dockAction;
    // --- round 12 phase 12D ---
    // A Contents drawer tile dragged out of the window (screen px; `entry` the item it adds): phase 0
    // over (App shows the marker of the dock under the point; returns 0 nowhere, 1 a dock, 2 the
    // desktop for a script widget), 1 left / cancelled (the marker goes), 2 dropped (returns 1 when
    // something was added: on that dock where the marker was, or a standalone widget at the point).
    std::function<int(int phase, POINT screen, const DockItemEntry* entry)> drawerDrag;
    // --- end round 12 phase 12D ---
};

class SettingsWindow;
class SettingsPreview;

// The three kinds of settings window (round 5 phase B), each a top-level WinDock.Settings window:
// Global (at most one: a nav column of the pages that apply to every dock), Dock (one per config
// dock id, a folder window entry included: an identity header and a tab strip of that dock's
// pages) and Item (one per (dock, item): a header, the item's one page and a footer).
enum class SettingsKind { Global, Dock, Item };

// "Bottom" ... and "Primary" / "Display 2" / "Every display", as the Docks and displays list
// shows them (SettingsBands.cpp).
const wchar_t* SettingsEdgeName(DockEdge edge);
std::wstring SettingsDisplayName(int monitor);

// What a control is: decides its font, colours and visual-style class.
enum class CtlKind {
    Title,     // page title (large semibold)
    Section,   // section header (semibold)
    Label,     // row label (left column)
    Note,      // secondary text
    Value,     // plain text in the control column
    Caption,   // owner-drawn clickable caption of a check box / radio button (forwards BM_CLICK)
    Button,
    Check,     // caption-less BS_AUTOCHECKBOX (its window text is the accessible name, clipped)
    Radio,     // caption-less BS_AUTORADIOBUTTON
    Edit,
    Spin,      // up-down attached to the preceding Edit
    Combo,
    Trackbar,
    Link,      // SysLink
    Swatch,    // owner-drawn push button (BS_OWNERDRAW): the page paints it in OnDrawItem
    List,      // SysListView32 (report view): dark items view theme, palette colours
    Custom,    // a window class of the page's own (it paints itself in the palette colours)
    Expander,  // round 8 phase 8A: owner-drawn push button painted as a link ("▸ More…" / "▾ Less")
    Help,      // round 9 phase 9D: a card's round "?" button (owner-drawn; opens the Help at Ctl::topic)
};

// One row of a page's layout table. Rects are in DIPs, page coordinates (unscrolled); the window
// converts them on every layout pass (creation, DPI change, resizing, scrolling).
// Round 8 phase 8A: `base` is the rect as built (at the build width) or as MoveCtl set it; the
// reflow pass (SettingsWindow::Reflow) derives `dip` from it: rows move down / up by the measured
// height of the notes above them, rows whose controls are all gone (Collapse, a closed expander)
// take no height, and controls at the right margin follow the page's width.
struct Ctl {
    int id = 0;
    CtlKind kind = CtlKind::Label;
    const wchar_t* cls = nullptr;
    std::wstring text;
    DWORD style = 0;
    DWORD exStyle = 0;
    RECT dip{};
    RECT base{};
    int buddy = 0;  // Check / Radio: caption id; Caption: its button; Edit: its Spin; Spin: its Edit; Label: its hint
    int lo = 0, hi = 0, step = 1;  // Trackbar / Spin range; Edit: max text length (hi)
    HWND hwnd = nullptr;
    int row = -1;            // reflow row (-1: fixed: never moved by the reflow)
    int card = -1;           // the card it sits in (SettingsPage::cards_; -1 none)
    int expander = 0;        // the expander whose rows it is in (0 none)
    int maxLines = 0;        // a measured note: at most this many lines (0 any)
    bool measured = false;   // a multi-line Note / Value: its height is measured at layout time
    bool collapsed = false;  // Collapse(): hidden and takes no height
    bool shown = true;       // Show()
    bool goneApplied = false;  // hidden by the window because it is gone (collapsed / closed expander)
    // Round 8 phase 8D: an Expander that is a card's heading ("▸ Bar · 2 changed" / "▾ Bar · 2
    // changed", semibold in the text colour, as wide as its row; HeadingExpander).
    bool heading = false;
    std::wstring tip;       // Tip(): its tooltip (and accessible description)
    // Round 9 phase 9D: a Help button's topic ("guide-settings.md#auto-hide"); `hint`: the muted
    // one-line hint of a Label(id, text, hint) / Check(..., hint) row (QueryState 285 counts rows
    // with neither a tooltip nor a hint).
    std::wstring topic;
    bool hint = false;
};

// A page: a child window (WinDock.SettingsPage, WS_EX_CONTROLPARENT) holding the controls of its
// table. Subclasses (SettingsPages.cpp) build the table, load the config into the controls and
// handle notifications; the window does creation, fonts, colours, theming, layout and scrolling.
class SettingsPage {
public:
    SettingsPage(SettingsWindow& window, const wchar_t* name, wchar_t glyph);
    virtual ~SettingsPage() = default;
    SettingsPage(const SettingsPage&) = delete;
    SettingsPage& operator=(const SettingsPage&) = delete;

    const std::wstring& Name() const { return name_; }
    wchar_t Glyph() const { return glyph_; }  // Segoe Fluent Icons code point (0: none)
    // Listed in the nav column. A retired page (round 5: Weather, index 6) keeps its index so the
    // later pages keep theirs, but is never listed or selected.
    virtual bool InNav() const { return true; }
    HWND Hwnd() const { return hwnd_; }
    HWND Item(int id) const;
    const Ctl* Find(int id) const;
    const Ctl* Find(HWND hwnd) const;

protected:
    friend class SettingsWindow;

    // --- content (SettingsPages.cpp) ---
    // Fills the control table with the builders below. Called once, before the controls exist.
    virtual void Build() = 0;
    // Config -> controls. Runs under the window's loading guard (notifications are ignored).
    virtual void Load() = 0;
    // After the controls were created (once): fill lists that do not depend on the config.
    virtual void Created() {}
    virtual void OnCommand(int /*id*/, int /*code*/, HWND /*ctl*/) {}
    virtual LRESULT OnNotify(const NMHDR& /*nm*/) { return 0; }
    // WM_HSCROLL from a trackbar (TB_* code) or WM_VSCROLL from an up-down (SB_* code).
    virtual void OnScroll(int /*id*/, int /*code*/) {}
    // Enter in edit / editable combo `id`, or it lost the focus: commit its text.
    virtual void OnCommit(int /*id*/) {}
    virtual void OnDisplayChange() {}
    // WM_DRAWITEM for an owner-drawn button (CtlKind::Swatch). Return true if drawn.
    virtual bool OnDrawItem(const DRAWITEMSTRUCT& /*di*/) { return false; }
    // WM_SETTINGCHANGE reached the window (after its own handling).
    virtual void OnSettingChange(WPARAM /*wp*/, LPARAM /*lp*/) {}
    // The DPI, the fonts or the palette changed (after the window updated them): rebuild image
    // lists and other DPI / colour dependent resources of the page.
    virtual void OnStyleChanged() {}
    // The dock's weather data / status changed (SettingsWindow::WeatherChanged).
    virtual void OnWeatherChanged() {}
    // The notification list / service state changed (SettingsWindow::NotificationsChanged).
    virtual void OnNotificationsChanged() {}
    // The calendar feeds' status changed (SettingsWindow::CalendarChanged).
    virtual void OnCalendarChanged() {}
    // An Item window's page: the item (an index into the window's dock's items) it shows; called
    // before the first Build and when App re-points the window (the item moved).
    virtual void SelectItem(int /*item*/) {}
    // Paints on the page's background after it was filled (WM_ERASEBKGND / WM_PRINTCLIENT; page
    // client px, scrolled by ScrollPx()): frames and separators around controls (round 5 phase C).
    // Round 8 phase 8A: the base paints the cards and their hairlines; a page that overrides it
    // calls SettingsPage::PaintBackground first.
    virtual void PaintBackground(HDC dc);
    // The page's width changed (the window was resized), after its table was laid out again: list
    // columns, custom layouts (round 8 phase 8A).
    virtual void OnResized() {}
    // Automation (SettingsWindow::QueryPages): answer `code` into `out` and return true if the
    // page knows it.
    virtual bool Query(int /*code*/, LPARAM /*arg*/, LRESULT& /*out*/) { return false; }
    // WM_TIMER of a timer the page set on its own window (SetTimer(Hwnd(), id, ...); round 8
    // phase 8C: the place search's typing pause). The page kills it.
    virtual void OnTimer(UINT_PTR /*id*/) {}
    // Round 9 phase 9A: another page of the same window changed the config (after the host applied
    // it; under the loading guard): controls that show what that change touches follow (a full
    // Load would overwrite half-typed values and rebuild lists, so pages only re-sync dependents).
    virtual void OnPeerChanged() {}
    // Round 12 phase 12B: the height in DIPs of control `id` (a CtlKind::Custom whose Ctl::measured
    // the page set) at `widthDip`, measured by the reflow (<= 0: as built).
    virtual int MeasureCustomDip(int /*id*/, int /*widthDip*/) const { return -1; }

    // --- layout table builders (DIPs; the cursor y_ moves down row by row) ---
    static constexpr int kMargin = 24;
    static constexpr int kLabelW = 196;
    static constexpr int kColX = kMargin + kLabelW + 12;  // control column
    static constexpr int kRowH = 32;
    static constexpr int kRowGap = 2;
    static constexpr int kCaptionDx = 26;  // check box / radio -> its caption
    static constexpr int kHintRowH = 46;   // a row whose label has a hint (Label(id, text, hint))
    static constexpr int kCardPadX = 12;   // a card's edge from the controls' margin
    static constexpr int kHelpDip = 20;    // round 9 phase 9D: a card's round "?" button
    int Width() const;  // content width in DIPs
    Ctl& Add(int id, CtlKind kind, const wchar_t* cls, std::wstring text, DWORD style, RECT dip);
    // The page's title (Global window only; round 8 phase 8A: a no-op in Dock and Item windows,
    // where the tab or the header names the page: the content then starts at y = 16).
    void Title(const wchar_t* text);
    // A section header; it opens a card (a rounded panel) that the next Section, EndCard() or the
    // end of the table closes. Rows before the first Section get an untitled card of their own.
    // Round 9 phase 9D: `topic` (a guide and heading, "guide-settings.md#auto-hide"): a round "?"
    // button at the right end of the header row opens the Help there (HelpButton).
    void Section(int id, const wchar_t* text, const wchar_t* topic = nullptr);
    // Closes the open card: the rows after it are outside any card until the next Section.
    void EndCard();
    // Round 8 phase 8B: opens an untitled card (26 DIPs under the last one), like Section without
    // its header (the Position tab's "More" expander). Round 9 phase 9D: with a topic, the "?"
    // button sits at the right end of the card's first row (the rows' controls there end before it).
    void UntitledCard(const wchar_t* topic = nullptr);
    // Round 9 phase 9D: the open card's "?" button in the current row (id kHelpButtonBase + the
    // card's index; tooltip "Help: <title>", the page's name when null): for a card whose first row
    // has no room for it (Docks and displays' canvas). Section / HeadingExpander / UntitledCard
    // with a topic call it.
    void HelpButton(const wchar_t* topic, const wchar_t* title = nullptr);
    // Round 9 phase 9D: the page's own help topic (F1 opens the Help there; else its first card's).
    void HelpTopic(const wchar_t* topic) { topic_ = topic ? topic : L""; }
    const std::wstring& Topic() const;
    void Label(int id, const wchar_t* text);
    // A label with a one-line muted hint under it (the row is kHintRowH tall; the row's other
    // controls are centred in it). `hintId` 0: an id of the page's own (not stable).
    void Label(int id, const wchar_t* text, const wchar_t* hint, int hintId = 0);
    // The full explanation of control `id` (and of its label's hint) as a multi-line tooltip.
    void Tip(int id, std::wstring text);
    // Round 10 phase 10B: a new tooltip for a control that already exists (it had a Tip at Build;
    // the Contents tab's inspector rows change meaning with the selection).
    void SetTip(int id, const std::wstring& text);
    // A link-styled toggle ("▸ <text>" / "▾ Less", Space / Enter) whose rows (up to EndExpander)
    // are collapsed while it is closed (closed at first; per page instance, not saved).
    void Expander(int id, const wchar_t* text);
    // Round 8 phase 8D: an expander that heads its card (a group of Themes fields, Calendar's
    // "Add or edit a feed"): it keeps its text when open, drawn like a section header with a
    // chevron. `openAtFirst`: open when the page is built the first time (then as the user left it).
    void HeadingExpander(int id, const wchar_t* text, bool openAtFirst = false, const wchar_t* topic = nullptr);
    void EndExpander();
    // Lets the page lay itself out (MoveCtl): no reflow, no cards (the Contents tab).
    void NoReflow() { reflow_ = false; }
    Ctl& Text(int id, CtlKind kind, std::wstring text, int x, int w, int h = kRowH);
    void Trackbar(int id, int x, int w, int lo, int hi, int page);
    void Spin(int editId, int spinId, int x, int w, int lo, int hi, int step);
    void Combo(int id, int x, int w, bool editable);
    void Check(int id, int captionId, const wchar_t* text, int x = kMargin);
    // Round 8 phase 8C: a check box with a one-line muted hint under its caption (a kHintRowH row,
    // like Label(id, text, hint)). `hintId` 0: an id of the page's own.
    void Check(int id, int captionId, const wchar_t* text, const wchar_t* hint, int hintId, int x = kMargin);
    struct RadioDef {
        int id;
        int captionId;
        const wchar_t* text;
    };
    // Consecutive radio buttons, one row each, in one WS_GROUP. Their captions are created after
    // them so the group stays contiguous (auto-radio unchecking, arrow-key navigation).
    void Radios(std::initializer_list<RadioDef> radios, int x = kMargin);
    void Button(int id, const wchar_t* text, int x, int w);
    void Link(int id, std::wstring markup, int x, int w);
    // The next row: the cursor moves down by `h` (a hint row: by kHintRowH - kRowH more) and
    // the controls added after it belong to the next reflow row.
    void NextRow(int h = kRowH + kRowGap) {
        if (hintRow_ == row_) h += kHintRowH - kRowH;
        y_ += h;
        ++row_;
    }
    int y_ = 0;
    int labelW_ = kLabelW;  // the label column's width (a page may narrow it: Themes)

    // --- helpers ---
    Config& Cfg();
    SettingsHost& Host();
    SettingsWindow& Window() { return window_; }
    // The window's dock (SettingsWindow::Dock / DockIndex).
    DockConfig& Dock();
    size_t DockIndex() const;
    bool Loading() const;
    // Tells the host (about the window's dock) and repaints the preview.
    void Changed(SettingsScope scope);
    // ... about config dock `dock` (a page of the Global window editing another dock).
    void ChangedFor(SettingsScope scope, size_t dock);
    // Round 12 phase 12A: the undo label of the change the page is about to make ("Removed Clock
    // from main"); call it before Changed. Without one the window derives "<row label>: <value>"
    // from the control that fired.
    void ChangeLabel(const std::wstring& label);
    // A dock item's name for such a label ("Clock", "Paint", "a separator").
    std::wstring EntryLabel(size_t item);
    void SetText(int id, const std::wstring& text);
    std::wstring GetText(int id) const;
    void SetCheck(int id, bool checked);
    bool GetCheck(int id) const;
    void SetSpin(int spinId, int value);
    int GetSpin(int spinId) const;
    void SetTrack(int id, int pos);
    int GetTrack(int id) const;
    void Enable(int id, bool enable);
    void Show(int id, bool show);
    // Hides control `id` and lets it take no height (a row whose controls are all collapsed
    // disappears; the rows under it move up), or brings it back. Relayouts the page when it changed.
    void Collapse(int id, bool hidden);
    void Collapse(std::initializer_list<int> ids, bool hidden);
    bool ExpanderOpen(int id) const;
    void SetExpander(int id, bool open);
    // Round 8 phase 8D: an expander's text ("Bar · 2 changed").
    void SetExpanderText(int id, const std::wstring& text);
    // Round 10 phase 10D: "<title> · n changed" (n > 0), else the title.
    static std::wstring ChangedTitle(const wchar_t* title, int n);
    // Round 10 phase 10D: the page's heading expander `id` was opened or closed (a page that lays
    // itself out, NoReflow, places its rows again).
    virtual void OnExpanderToggled(int /*id*/) {}
    // Scrolls the page so control `id` is in view (as the Tab key does).
    void ScrollIntoView(int id);
    // A note's measured height in DIPs at `widthDip` (the page's own layouts, e.g. the Contents
    // tab's inspector).
    int NoteHeightDip(int id, int widthDip) const;
    // Moves control `id` to `dip` (page DIPs) in the table; Relayout() then applies the table to
    // the windows (and repaints the page's background).
    void MoveCtl(int id, RECT dip);
    void Relayout();
    int ScrollPx() const { return scrollPx_; }
    // The top-level settings window (owner for dialogs).
    HWND Owner() const;
    // Parses the edit's integer, clamps it to [lo, hi] and writes the clamped text back.
    // Returns false (the text is restored by the caller's Load) if it is not a number.
    bool ReadInt(int editId, int lo, int hi, int& value);

private:
    // A card (round 8 phase 8A): its controls carry its index (Ctl::card); `dip` is computed by
    // the reflow from its visible controls (empty: nothing visible, not painted).
    struct Card {
        int section = 0;  // the header's control id (0: untitled)
        RECT dip{};
    };
    // A hairline between two rows of a card (page DIPs, computed by the reflow).
    struct Hairline {
        int y = 0, left = 0, right = 0;
    };
    void ResetTable();  // before Build (again)

    SettingsWindow& window_;
    std::wstring name_;
    wchar_t glyph_ = 0;
    HWND hwnd_ = nullptr;
    HWND tip_ = nullptr;  // the page's tooltip window (Tip; null: none)
    // Round 9 phase 9A: the disabled control the page's rect tool (kDisabledTipId) is over (null: none).
    HWND tipProxy_ = nullptr;
    static constexpr UINT_PTR kDisabledTipId = 0x9A01;
    // Round 9 phase 9B: a row flashed by the settings search's Go to (the accent at 30 % fading
    // out over kFlashMs; one timer on the page window that kills itself at the end).
    static constexpr UINT_PTR kFlashTimerId = 0x9B01;
    static constexpr DWORD kFlashMs = 1200;
    int flashRow_ = -1;        // the reflow row (-1: none)
    RECT flashDip_{};          // its band (page DIPs)
    ULONGLONG flashStart_ = 0;
    std::vector<Ctl> table_;
    std::vector<Card> cards_;
    std::vector<Hairline> hairlines_;
    int contentPx_ = 0;  // height of the content at the current DPI
    int scrollPx_ = 0;
    int row_ = 0;           // the reflow row the builders add to
    int hintRow_ = -1;      // the row whose label has a hint
    int card_ = -1;         // the open card (-1 none)
    bool autoCard_ = true;  // a control added with no card open opens an untitled one
    int expander_ = 0;      // the open Expander (its rows: until EndExpander)
    std::vector<int> openExpanders_;  // expanders the user opened (kept across a rebuild)
    std::vector<int> expanderDefaults_;  // HeadingExpander(openAtFirst): applied once per page instance
    int nextAutoId_ = 0x6000;  // ids of hints without one of their own
    // Round 9 phase 9D: the page's help topic (HelpTopic), and an UntitledCard(topic)'s "?" button
    // waiting for the card's first control (it goes into that control's row).
    std::wstring topic_;
    std::wstring pendingHelp_;
    int pendingHelpCard_ = -1;
    mutable std::wstring firstTopic_;  // Topic() without HelpTopic: the first card's
    int buildW_ = 0;        // Width() when the table was built (DIPs)
    int layoutW_ = 0;       // Width() of the last layout (OnResized when it changes)
    bool reflow_ = true;
    bool scrollBar_ = false;  // the page scrolls: Width() leaves its scroll bar out (round 8 phase 8A)
};

// A settings window (WinDock.Settings; round 5 phase B: one of three kinds, SettingsKind): a
// resizable top-level window (round 8 phase 8A: its last size per kind in config settings.windows)
// showing one page at a time on a solid content layer. Global: a
// navigation column (WinDock.SettingsNav, Mica behind it where available) and a subtitle band;
// Dock: an identity header (WinDock.SettingsBand) and a tab strip (WinDock.SettingsTabs); Item: a
// header, the one page (round 10 phase 10C: or two, Content / Appearance, under a tab strip) and a
// footer. Windows share nothing but the Config: each finds its dock by
// id every time (DockIndex) and an Item window its item by index (App re-points it). Nothing of a
// window exists while it is closed: no timers, fonts, brushes or windows.
class SettingsWindow {
public:
    static constexpr wchar_t kClassName[] = L"WinDock.Settings";
    static constexpr wchar_t kNavClassName[] = L"WinDock.SettingsNav";
    static constexpr wchar_t kPageClassName[] = L"WinDock.SettingsPage";
    static constexpr wchar_t kBandClassName[] = L"WinDock.SettingsBand";
    static constexpr wchar_t kTabsClassName[] = L"WinDock.SettingsTabs";
    static constexpr wchar_t kFindClassName[] = L"WinDock.SettingsFind";  // round 9 phase 9B

    // `dock`: the config dock id (Global: the row its Docks and displays page selects first);
    // `item`: an Item window's index into that dock's items.
    SettingsWindow(SettingsHost host, SettingsKind kind, std::wstring dock, size_t item = SIZE_MAX);
    ~SettingsWindow();
    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;

    // Creates and shows the window (Global / Dock: centred on the dock's monitor, above its bar;
    // Item: 40 DIPs right / down of its dock's window, else like a Dock window) and activates it.
    // Returns false if the window could not be created. Round 9 phase 9B: `show` false creates it
    // hidden (for the settings search's index): never shown, activated or focused until Activate.
    bool Create(HINSTANCE instance, bool show = true);
    // Restores (if minimized; shows it if it was created hidden) and activates the existing window.
    void Activate();
    HWND Hwnd() const { return hwnd_; }
    // Keyboard navigation (IsDialogMessage; Ctrl+Tab / Ctrl+Shift+Tab: the next / previous page)
    // for messages to this window or its children. Returns true if the message was handled.
    bool PreTranslateMessage(MSG& msg);
    // Reloads every control from *host.config (after tray toggles, Reload config) and the header.
    void Refresh();
    // Round 12 phase 12A: the timeline or the save state changed (host.status): the band's status
    // line and the bar at the top of the page area follow (the pages move down while it shows).
    void StatusChanged();
    // ... the bar is shown in this window (App's BarKind says what it is).
    bool BarShown() const { return barShown_; }
    // Round 10 phase 10D: "Reset to factory defaults…" asks first: a Yes / No MessageBox owned by
    // this window (under host.modal's hold) saying what resets (all of `name`'s settings on this
    // window's dock, `kind` words it) and that app defaults are untouched. True: Yes.
    bool ConfirmFactoryReset(const std::wstring& name, ItemKind kind);
    // Round 10 phase 10D: the Item footer's "Reset to app default" (kFooterResetApp, a script widget
    // with a place of its own) shown beside Reset to factory defaults… or hidden.
    void ShowFooterResetApp(bool show);
    // Selects a page (Global: SettingsIds::kPage*, Dock: kTab*), as a click in the nav / tabs would.
    void ShowPage(int index);
    int ActivePageIndex() const { return active_; }
    // Round 8 phase 8D: where page `index` is listed in the nav column / tab strip (0 the first
    // row; -1 not listed). The Global window lists its pages in groups (NavOrder), not by index.
    int NavDisplayIndex(int index) const;
    // Round 10 phase 10C: the tabs of the tab strip (0: the window has none; QueryState 291).
    int TabCount() const;
    // The dock's weather data / status changed: pages update what shows it (no config reload).
    void WeatherChanged();
    // The notification list or service state changed: the Notifications page's status line.
    void NotificationsChanged();
    // The calendar feeds' data / status changed: the Calendar page's status column.
    void CalendarChanged();
    // A page whose controls depend on the config (the generated widget settings page) builds its
    // table again: posted, so a page may ask from inside its own notification.
    void RequestRebuild(const SettingsPage& page);
    // Automation: the first page that answers QueryState `code` (SettingsPage::Query).
    bool QueryPages(int code, LPARAM arg, LRESULT& out);

    // --- the settings search (round 9 phase 9B, SettingsSearch) ---
    // One setting of a page as the search lists it, read from the live controls: a check box or a
    // radio button (its caption), or a row's first input control (a drop-down list, an edit, a
    // trackbar; its label). Collapsed and hidden controls are left out (they do not apply now); a
    // control behind a closed expander is in (Go to opens it); pages laid out by themselves (the
    // Contents tab) are not indexed.
    struct SearchEntry {
        int page = 0;            // page index (SettingsIds::kPage* / kTab*)
        int id = 0;              // the control Go to focuses
        CtlKind kind = CtlKind::Label;
        std::wstring setting;    // the label / caption
        std::wstring value;      // "On" / "Off", the chosen radio's caption, the combo's text, the edit's text, the number
        std::wstring card;       // the card's title ("" none)
        std::wstring extra;      // searched, not shown: the hint and the tooltip
        std::wstring tip;        // the tooltip (the list's info tip)
        std::vector<std::wstring> choices;  // a drop-down list's rows (in-place change)
        int choice = -1;         // its selection
        bool enabled = true;
    };
    std::vector<SearchEntry> SearchEntries();
    // The window as the search's Where column names it: "App" (round 10 phase 10A: was "Global"), "Dock main", "Widget sw",
    // "Folder window fw", "<item> on <dock>".
    std::wstring SearchName();
    std::wstring PageName(int index) const;
    // Go to: selects page `page`, opens the expander that holds control `id`, scrolls it into view,
    // focuses it and flashes its row. `screen`: the control's rect afterwards (screen px).
    bool SearchReveal(int page, int id, RECT* screen = nullptr);
    // A change in place, exactly as a click would make it: a check box toggles, a radio button is
    // chosen, a drop-down list's row `choice` is selected (the page's own notification follows).
    // False (nothing changed) for a greyed control or another kind.
    bool SearchChange(int page, int id, int choice);
    // Starts the flash of the row of control `id` on page `page`.
    void FlashRow(int page, int id);

    // --- help (round 9 phase 9D) ---
    // The help topic of the page shown now (F1): its HelpTopic, else its first card's, else the
    // settings guide.
    std::wstring CurrentHelpTopic() const;
    // QueryState 285: the input controls (check box, radio group, drop-down, edit, up-down,
    // trackbar, list, custom) whose row has neither a tooltip nor a hint, plus input rows with no
    // label or caption at all. `gaps` (optional) gets one line per gap ("Behaviour: 2228 no tip").
    int UnexplainedInputs(std::vector<std::wstring>* gaps = nullptr, std::vector<int>* ids = nullptr) const;
    // QueryState 285 with LOWORD bit 3: the cards (of pages that have cards) without a "?" button;
    // `ids` gets page index * 100 + the card's index for each.
    int CardsWithoutHelp(std::vector<int>* ids = nullptr) const;

    // --- the target ---
    SettingsKind Kind() const { return kind_; }
    const std::wstring& DockId() const { return dockId_; }
    // The window's config dock (by id; SIZE_MAX: it is gone, App closes the window; the Global
    // window falls back to docks[0]).
    size_t DockIndex() const;
    // That dock (a gone dock: a scratch entry nobody reads).
    DockConfig& Dock();
    // A Dock window of a folder window entry (round 4 phase 12: only the Folder window page).
    bool IsFolderWindow() const;
    // A Dock window of a standalone widget entry (round 6 phase 6B: only the Standalone widget page).
    bool IsWidgetWindow() const;
    // The Global window: the row selected on its Docks and displays page (Duplicate / Remove / Open
    // act on it; round 5 phase D: no other Global page reads it).
    void SelectDock(const std::wstring& id);
    // An Item window's item: its index, the entry (null: out of range).
    size_t ItemIndex() const { return item_; }
    DockItemEntry* TargetItem();
    // App: the item moved to index `item` (its pages show it there; nothing else changes).
    void Retarget(size_t item);
    // App: what the Item window's item looked like after the last change, and how many items its
    // dock had then (re-finding the item after the dock's items changed).
    const DockItemEntry& ItemCopy() const { return itemCopy_; }
    size_t ItemCountCopy() const { return itemCount_; }
    void SetItemCopy(const DockItemEntry& entry, size_t count) {
        itemCopy_ = entry;
        itemCount_ = count;
    }
    // The title bar, the caption / border colour, the header and the tab strip follow the config
    // (the dock's id, edge, display, items, lock; the item's name and place).
    void UpdateIdentity();
    // --- round 12 phase 12C ---
    // The window's dock as people see it (its name, else its id; Global: the selected row's).
    std::wstring DockName() const;
    // A Dock window of a dock: the header's name becomes an edit (kHeaderNameEdit), focused, its
    // text selected (the window is brought forward first). False for another kind of window.
    bool BeginRename();
    // --- end round 12 phase 12C ---

    // --- for pages ---
    SettingsHost& Host() { return host_; }
    bool Loading() const { return loading_; }
    UINT Dpi() const { return dpi_; }
    int Px(int dip) const { return MulDiv(dip, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI); }
    const DarkMode::Palette& Colors() const { return palette_; }
    // A setting was applied to the dock: repaint the preview strip.
    void ConfigApplied();
    // Round 9 phase 9A: page `source` changed the config: every other page re-syncs its dependents
    // (SettingsPage::OnPeerChanged), so the window's tabs never show a stale state.
    void PeersChanged(const SettingsPage* source);
    // The pages' width in DIPs (the client width less the nav column).
    int ContentWidthDip() const { return ClientDip().cx - (kind_ == SettingsKind::Global ? kNavWidthDip : 0); }
    // Round 8 phase 8A: the live client size in DIPs (QueryState 275; the restored size while
    // minimized).
    SIZE ClientSizeDip() const { return ClientDip(); }
    // QueryState 276: 1 while expander `id` of any page is open, 0 closed, -1 no such expander.
    int ExpanderState(int id) const;
    // What is under control `hwnd` of a page: a card's fill or the content layer (custom controls
    // paint their background in it).
    COLORREF SurfaceColor(HWND control) const;
    // The height of `text` in the note font wrapped at `widthDip` (DIPs; DrawText DT_CALCRECT).
    int MeasureTextDip(const std::wstring& text, int widthDip, int maxLines = 0) const;
    // The dock's identity colour (Config::DockColor) as a COLORREF.
    COLORREF DockColor() const;
    // Round 8 phase 8B (moved from the header to the Position tab): a small monitor with the
    // window's dock drawn where it is (its edge, alignment and full width, a floating bar at its
    // place, a half circle / circle; a standalone widget's box, a folder window's window) and a
    // caption line ("Primary · 5120 × 1440") under it, in `rc` over `back` (SettingsBands.cpp).
    void PaintSilhouette(HDC dc, const RECT& rc, COLORREF back);
    // "Where it sits: Primary · 5120 × 1440" (the silhouette's caption / accessible name).
    std::wstring SilhouetteCaption();

    // --- widget previews (round 6 phase 6E) ---
    // A rendered preview, cached per window by (widget id, the box it is fitted into) for the
    // host's preview stamp and this window's DPI: when either changes, the whole cache goes and the
    // previews render again as they are painted. A box smaller than the widget's renders it at a
    // smaller scale (sharp, not shrunk).
    struct PreviewImage {
        HBITMAP bitmap = nullptr;  // owned by the cache
        SIZE px{};
        bool failed = false;       // the widget failed: no bitmap (the caller keeps its letter tile)
        double ms = 0.0;
    };
    // Widget `id`'s preview fitted into `fit` px; renders it now when it is not cached and `render`
    // is true (else null: the caller paints its placeholder and asks again on a later paint). Null
    // when it failed.
    // `zoom` > 1: a widget smaller than the box is drawn up to that many times larger (the
    // inspector, the Widget library).
    const PreviewImage* Preview(const std::wstring& id, SIZE fit, bool render, float zoom = 1.0f);
    // The cache entry as it is (rendered or failed; null: never rendered).
    const PreviewImage* PreviewEntry(const std::wstring& id, SIZE fit, float zoom = 1.0f);
    // Drops every cached preview and repaints the window (they render again when painted).
    void InvalidatePreviews();
    int PreviewsRendered() const { return previewRenders_; }  // renders since the window opened (QueryState 263)
    int PreviewFailures() const;                               // widget ids whose preview failed (QueryState 265)
    // Paints `img` fitted into `box` (aspect kept, centred, GdiAlphaBlend) over a rounded panel in
    // the theme's background mixed 60 % over `under` (transparent widgets stay readable).
    void PaintPreview(HDC dc, const RECT& box, const PreviewImage& img, COLORREF under) const;

private:
    friend class SettingsPage;

    // Client sizes in DIPs (round 8 phase 8A: resizable): the live size once the window exists,
    // before that the saved one (config settings.windows) or the default (Global 760 x 560, Dock
    // 860 x 620, Item 600 x 600); at least the minimum (Global 700 x 500, Dock 780 x 540, Item
    // 520 x 480).
    SIZE ClientDip() const;
    int KindIndex() const { return kind_ == SettingsKind::Global ? 0 : kind_ == SettingsKind::Dock ? 1 : 2; }
    SIZE MinClientDip() const;
    // Saves the restored client size in config settings.windows (SaveOnly) when it changed.
    void SaveClientSize();
    // The reflow pass (Ctl::base -> Ctl::dip, the cards, the hairlines); `dc` has the note font.
    void Reflow(SettingsPage& page, HDC dc);
    int MeasureDip(HDC dc, const std::wstring& text, int widthDip, int maxLines) const;
    // The page's tooltip window (Tip): created after its controls, themed with the palette.
    void CreateTips(SettingsPage& page);
    void ThemeTips(HWND tip) const;
    void DrawExpander(const SettingsPage& page, const DRAWITEMSTRUCT& di);
    // Round 9 phase 9D: a card's "?" button (muted ring and glyph, the accent while hot), and its
    // hover tracking (a subclass: owner-drawn buttons get no hot state).
    void DrawHelp(const SettingsPage& page, const DRAWITEMSTRUCT& di);
    static LRESULT CALLBACK HelpSubclassProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    void OpenHelp(const std::wstring& topic);
    HWND helpHot_ = nullptr;
    void ToggleExpander(SettingsPage& page, int id);
    const SettingsPage* PageOfConst(HWND control) const;
    static constexpr int kNavWidthDip = 200;
    static constexpr int kNavTopDip = 12;
    static constexpr int kNavItemDip = 36;
    static constexpr int kNavCaptionDip = 22;  // round 8 phase 8D: a group caption ("GENERAL", "LOOK" ...)
    static constexpr int kNavGapDip = 12;      // the gap before About
    // Round 9 phase 9B: the "Find a setting…" box at the top of the nav column (its band's height;
    // the page list starts under it).
    static constexpr int kNavFindDip = 48;
    static constexpr int kAboveBarDip = 24;    // gap between the window and the dock's bar
    static constexpr int kItemOffsetDip = 40;  // an Item window from its dock's window
    // The dock preview strip (SettingsPreview) under the Global Themes / Animation pages and a Dock
    // window's Appearance tab (round 8 phase 8B: only there; the Position tab has the silhouette,
    // Contents its item strip), content-pane wide.
    static constexpr int kFooterDip = 96;
    static constexpr int kSubtitleDip = 34;     // Global: the subtitle band over the pages
    static constexpr int kDockHeaderDip = 72;   // Dock: the identity header (round 8 phase 8B: was 128)
    // Item: the header (round 8 phase 8C: three lines, the name, "on <dock> · item k of n[ · widget
    // id version]", the description; was 88)
    static constexpr int kItemHeaderDip = 92;
    static constexpr int kTabsDip = 42;         // Dock: the tab strip
    static constexpr int kItemFooterDip = 52;   // Item: the footer
    static constexpr int kBarDip = 44;          // round 12 phase 12A: the bar over the pages (while shown)
    static constexpr int kStatusDip = 132;      // ... the band's status line's width
    // Set to false to drop the Mica nav column (frame extension) if a driver misbehaves: the
    // nav column is then painted solid, as when transparency effects are off.
    static constexpr bool kNavMica = true;
    static constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX |
                                    WS_THICKFRAME | WS_CLIPCHILDREN;

    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK NavProcThunk(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK PageProcThunk(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK BandProcThunk(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK TabsProcThunk(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK FindProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT FindProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void PaintFind(HWND hwnd, HDC hdc);
    void OpenSearch(const std::wstring& text);
    // The flash's next frame (its timer), the band repainted; the end kills the timer.
    void FlashStep(SettingsPage& page);
    HBRUSH FlashBrush(const SettingsPage& page, const Ctl* c);  // null: the control is not in the flashed row
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);
    LRESULT NavProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT PageProc(SettingsPage& page, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT BandProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT TabsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    static bool RegisterClasses(HINSTANCE instance);
    RECT InitialRect() const;
    SIZE WindowSizeFor(UINT dpi) const;
    bool CreatePage(SettingsPage& page, int index);
    void CreateControls(SettingsPage& page);
    // Destroys the page's controls, builds its table again, creates, styles, lays out and loads them.
    void RebuildPage(int index);
    static constexpr UINT kRebuildMessage = WM_APP + 40;  // posted: wParam = page index
    void CreateFonts();
    void DestroyFonts();
    void CreateBrushes();
    void DestroyBrushes();
    COLORREF BandColorFor() const;
    void UpdateIcons();
    void DestroyIcons();
    // Dark / light, high contrast, Mica, palette, control themes; repaints everything.
    void ApplyTheme();
    void ApplyBackdrop();
    // DWMWA_CAPTION_COLOR / DWMWA_BORDER_COLOR / DWMWA_TEXT_COLOR in the dock's colour (Dock and
    // Item windows; Windows 11, failures ignored).
    void ApplyCaptionColor();
    void Layout();
    void LayoutPage(SettingsPage& page);
    void ScrollPage(SettingsPage& page, int px);
    void EnsureVisible(HWND control);
    void SelectPage(int index);
    // The preview strip shows under page `index`.
    bool PreviewOn(int index) const;
    // Where the keyboard focus goes when nothing in a page has it: the nav column / the tab strip
    // / the page's first control.
    HWND DefaultFocus() const;
    int CheckGlyphPx(bool radio) const;
    SettingsPage* ActivePage() const;
    SettingsPage* PageOf(HWND control) const;
    void OnEnterKey();

    // Nav pane and tab strip (SettingsNav.cpp)
    static bool FocusCuesHidden(HWND hwnd);
    // Round 8 phase 8D: the listed pages in display order (the Global window: GENERAL General,
    // Docks and displays; LOOK Themes, Animation; DATA Calendar feeds, Notifications, Tray icons;
    // WIDGETS Widget library; then About after a gap; other kinds: by index), each with the group
    // caption above it (null: none) and whether a gap comes before it.
    struct NavSlot {
        int page = 0;
        const wchar_t* caption = nullptr;
        bool gap = false;
    };
    std::vector<NavSlot> NavOrder() const;
    int NavItemDip(const std::vector<NavSlot>& order) const;  // a row's height in DIPs
    // A caption's rect in the nav column (client px) for slot `k` of NavOrder (empty: none).
    RECT NavCaptionRect(size_t k) const;
    RECT NavItemRect(int index) const;
    // The next listed page from `from` in direction `step` (+1 / -1); `from` when there is none.
    int NavStep(int from, int step) const;
    int NavHitTest(POINT pt) const;
    void PaintNav(HWND hwnd, HDC hdc);
    RECT TabRect(int index) const;  // tab strip client px
    int TabHitTest(POINT pt) const;
    void PaintTabs(HWND hwnd, HDC hdc);
    // Header / footer bands (SettingsBands.cpp). The header before the pages, the footer after them
    // (the tab order follows the creation order).
    void CreateBands(bool footer);
    void LayoutBands();
    void PaintBand(HWND hwnd, HDC hdc);
    void OnBandCommand(int id);
    // The header's "⋯" menu (Find a setting, Apply a preset) under its button.
    void ShowHeaderMenu();
    // The header's id and chips end here (client px): left of its action buttons.
    int HeaderTextRight() const;
    // The header's chips and summary: "Bottom edge", "Primary", "5 items", "Shown".
    std::vector<std::wstring> HeaderChips();
    std::wstring WindowTitle();
    std::wstring ItemName();  // round 10 phase 10A: with an ordinal among same-named items
public:
    std::wstring ItemNameOf(size_t index);  // item `index` of the dock, without the ordinal (round 12 phase 12A: public, undo labels)
private:
    // Round 12 phase 12A: the generic undo label of a change made with control `c` of `page`
    // ("Icon size 48 → 64", "Theme: Glass", "Auto-hide: Always"; "" none), `before` the row's value
    // text before the control's notification ran (a slider's, from its first scroll).
    std::wstring ChangeLabelFor(const SettingsPage& page, const Ctl& c, const std::wstring& before);
    std::wstring RowValueText(const SettingsPage& page, const Ctl& c);
    // After a page handled a control's notification: when it changed the config, the label goes
    // to the host.
    void NoteChange(const SettingsPage& page, const Ctl* c, const std::wstring& before);
    bool changedFlag_ = false;      // SettingsPage::ChangedFor ran since it was cleared
    HWND trackCtl_ = nullptr;       // the slider being dragged / stepped ...
    std::wstring trackBefore_;      // ... and its row's value text when it started
    HWND bar_ = nullptr;            // the Keep / Revert, Keep / Cancel, save error bar (kKeepBar)
    bool barShown_ = false;
    std::wstring barTextShown_;     // what it shows (unchanged: nothing redrawn)
    void LayoutBar();
    // The ⋯ menu's Revert this window… (asks first).
    void RevertWindow();
    // The item's icon for the header (the same source as the Contents tab's strip; null: a letter tile).
    void UpdateItemIcon();
    // Page colours
    LRESULT CtlColor(UINT msg, HDC hdc, HWND control, const SettingsPage& page);
    void DrawCaption(const SettingsPage& page, const DRAWITEMSTRUCT& di);
    // Dark mode trackbars (no dark visual style): channel and thumb in palette colours.
    LRESULT DrawTrackbar(NMCUSTOMDRAW& cd);
    void ThemeList(HWND list);
    static LRESULT CALLBACK ListSubclassProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    void StyleChanged();

    SettingsHost host_;
    SettingsKind kind_ = SettingsKind::Global;
    std::wstring dockId_;
    size_t item_ = SIZE_MAX;
    DockItemEntry itemCopy_;
    size_t itemCount_ = 0;
    DockConfig scratch_;  // Dock() of a window whose dock is gone (until App closes it)
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND nav_ = nullptr;     // Global
    HWND find_ = nullptr;    // Global: the nav column's Find a setting… box (round 9 phase 9B)
    bool findHot_ = false;
    bool findTracking_ = false;
    HBRUSH flashBrush_ = nullptr;  // the flashed row's statics (FlashBrush)
    COLORREF flashColor_ = 0;
    HWND tabs_ = nullptr;    // Dock / Item (round 10 phase 10C) with more than one page
    HWND header_ = nullptr;  // every kind (Global: the subtitle)
    HWND footer_ = nullptr;  // Item
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    std::vector<std::unique_ptr<SettingsPage>> pages_;
    std::unique_ptr<SettingsPreview> preview_;  // the strip under the pages; freed with the window
    int active_ = 0;
    int navHot_ = -1;
    bool navTracking_ = false;
    int tabHot_ = -1;
    bool tabsTracking_ = false;
    HWND lastFocus_ = nullptr;
    bool loading_ = false;
    bool mica_ = false;        // Mica behind the caption and the nav column (frame extended)
    bool darkPopups_ = false;  // DarkMode::TryEnableDarkPopups succeeded (undone when the last window closes)

    DarkMode::Palette palette_;
    COLORREF bandColor_ = 0;       // the header / footer background
    HFONT font_ = nullptr;         // message font
    HFONT fontSemibold_ = nullptr; // section headers
    HFONT fontTitle_ = nullptr;    // page titles, the header's id / name
    HFONT fontIcons_ = nullptr;    // nav glyphs (Segoe Fluent Icons / MDL2 Assets); null if absent
    HFONT fontCaption_ = nullptr;  // round 8 phase 8D: the nav's group captions (small semibold)
    HBRUSH layerBrush_ = nullptr;
    HBRUSH baseBrush_ = nullptr;
    HBRUSH controlBrush_ = nullptr;
    HBRUSH bandBrush_ = nullptr;
    HBRUSH cardBrush_ = nullptr;   // round 8 phase 8A: a card's fill
    COLORREF cardColor_ = 0;
    HBRUSH barBrush_ = nullptr;    // round 12 phase 12A: the bar's fill (the accent over the layer)
    COLORREF barColor_ = 0;
    SIZE clientDip_{};             // the last non-minimized client size (DIPs; 0: not shown yet)
    bool inSizeMove_ = false;      // between WM_ENTERSIZEMOVE and WM_EXITSIZEMOVE
    HICON iconSmall_ = nullptr;
    HICON iconBig_ = nullptr;
    HICON itemIcon_ = nullptr;     // Item: the header's icon (null: a letter tile)
    DockItemEntry iconFor_;        // the entry itemIcon_ was made for
    bool iconValid_ = false;
    UINT iconDpi_ = 0;
    std::wstring widgetLine_;      // Item (a widget): "id version" (on the header's second line)
    std::wstring widgetLineFor_;   // the widget id it was read for
    // Round 8 phase 8C: the Item header's third line (the manifest description, the clock's /
    // weather's sentence, a folder's path ...), one line with an ellipsis; the whole text is the
    // tooltip of that line (headerTip_, owned by the window: one tool over the line's rect).
    std::wstring itemDescription_;
    std::wstring widgetDescription_;  // the manifest description read with widgetLine_
    HWND headerTip_ = nullptr;
    RECT DescriptionRect() const;   // the third line in header client px
    std::wstring ItemDescription(); // what the third line says now
    void UpdateHeaderTip();
    // Widget previews (round 6 phase 6E): by "<widget id>|<w>x<h>"; valid for previewStamp_ / previewDpi_.
    std::map<std::wstring, PreviewImage> previews_;
    uint64_t previewStamp_ = 0;
    UINT previewDpi_ = 0;
    int previewRenders_ = 0;
    void ClearPreviews();
    void CheckPreviewStamp();
    // --- round 12 phase 12C: the Dock header's name edited in place ---
    HWND nameEdit_ = nullptr;        // kHeaderNameEdit, a child of the header (hidden while not renaming)
    bool renaming_ = false;
    RECT NameRect() const;           // the name's place in the header (client px)
    // Enter / focus loss: `commit` the edit's text as docks[].name; Esc: back to the name as it was.
    void EndRename(bool commit);
    static LRESULT CALLBACK NameEditSubclass(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    // --- end round 12 phase 12C ---
};

// The pages of a window (SettingsPages.cpp): Global in nav order (SettingsIds::kPage*), Dock in tab
// order (kTab*; a folder window entry: its one page; a standalone widget entry, round 8 phase 8C:
// Widget and Window, kTabWidgetSettings / kTabWidgetWindow; round 10 phase 10C: Content, Placement
// and, with both sections, Appearance as page 2), Item: its one page (kItemPage; round 10 phase 10C:
// a widget / clock / weather item with fields of both sections, Content 0 and Appearance 1).
std::vector<std::unique_ptr<SettingsPage>> CreateSettingsPages(SettingsWindow& window);
