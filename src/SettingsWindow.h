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
    SaveOnly,    // read from the config when used (delays, renderer): just save
};

// Docks and displays page edits of config.docks (SettingsHost::editDocks).
// TrayDock (8b): a new dock holding only a tray item at the bottom right (the edited dock's tray
// item moves into it), the takeover turned on.
// AddFloating (round 6 phase 6A): a floating bar dock (layer "desktop", centred on the primary
// display's work area, the default items).
enum class SettingsDockEdit { Add, Duplicate, Remove, TrayDock, AddFloating };

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
};

// One row of a page's layout table. Rects are in DIPs, page coordinates (unscrolled); the window
// converts them on every layout pass (creation, DPI change, scrolling).
struct Ctl {
    int id = 0;
    CtlKind kind = CtlKind::Label;
    const wchar_t* cls = nullptr;
    std::wstring text;
    DWORD style = 0;
    DWORD exStyle = 0;
    RECT dip{};
    int buddy = 0;  // Check / Radio: caption id; Caption: its button; Edit: its Spin; Spin: its Edit
    int lo = 0, hi = 0, step = 1;  // Trackbar / Spin range; Edit: max text length (hi)
    HWND hwnd = nullptr;
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
    virtual void PaintBackground(HDC /*dc*/) {}
    // Automation (SettingsWindow::QueryPages): answer `code` into `out` and return true if the
    // page knows it.
    virtual bool Query(int /*code*/, LPARAM /*arg*/, LRESULT& /*out*/) { return false; }

    // --- layout table builders (DIPs; the cursor y_ moves down row by row) ---
    static constexpr int kMargin = 24;
    static constexpr int kLabelW = 196;
    static constexpr int kColX = kMargin + kLabelW + 12;  // control column
    static constexpr int kRowH = 32;
    static constexpr int kRowGap = 2;
    static constexpr int kCaptionDx = 26;  // check box / radio -> its caption
    int Width() const;  // content width in DIPs
    Ctl& Add(int id, CtlKind kind, const wchar_t* cls, std::wstring text, DWORD style, RECT dip);
    void Title(const wchar_t* text);
    void Section(int id, const wchar_t* text);
    void Label(int id, const wchar_t* text);
    Ctl& Text(int id, CtlKind kind, std::wstring text, int x, int w, int h = kRowH);
    void Trackbar(int id, int x, int w, int lo, int hi, int page);
    void Spin(int editId, int spinId, int x, int w, int lo, int hi, int step);
    void Combo(int id, int x, int w, bool editable);
    void Check(int id, int captionId, const wchar_t* text, int x = kMargin);
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
    void NextRow(int h = kRowH + kRowGap) { y_ += h; }
    int y_ = 0;

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
    SettingsWindow& window_;
    std::wstring name_;
    wchar_t glyph_ = 0;
    HWND hwnd_ = nullptr;
    std::vector<Ctl> table_;
    int contentPx_ = 0;  // height of the content at the current DPI
    int scrollPx_ = 0;
};

// A settings window (WinDock.Settings; round 5 phase B: one of three kinds, SettingsKind): a
// fixed-size top-level window showing one page at a time on a solid content layer. Global: a
// navigation column (WinDock.SettingsNav, Mica behind it where available) and a subtitle band;
// Dock: an identity header (WinDock.SettingsBand) and a tab strip (WinDock.SettingsTabs); Item: a
// header, the one page and a footer. Windows share nothing but the Config: each finds its dock by
// id every time (DockIndex) and an Item window its item by index (App re-points it). Nothing of a
// window exists while it is closed: no timers, fonts, brushes or windows.
class SettingsWindow {
public:
    static constexpr wchar_t kClassName[] = L"WinDock.Settings";
    static constexpr wchar_t kNavClassName[] = L"WinDock.SettingsNav";
    static constexpr wchar_t kPageClassName[] = L"WinDock.SettingsPage";
    static constexpr wchar_t kBandClassName[] = L"WinDock.SettingsBand";
    static constexpr wchar_t kTabsClassName[] = L"WinDock.SettingsTabs";

    // `dock`: the config dock id (Global: the row its Docks and displays page selects first);
    // `item`: an Item window's index into that dock's items.
    SettingsWindow(SettingsHost host, SettingsKind kind, std::wstring dock, size_t item = SIZE_MAX);
    ~SettingsWindow();
    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;

    // Creates and shows the window (Global / Dock: centred on the dock's monitor, above its bar;
    // Item: 40 DIPs right / down of its dock's window, else like a Dock window) and activates it.
    // Returns false if the window could not be created.
    bool Create(HINSTANCE instance);
    // Restores (if minimized) and activates the existing window.
    void Activate();
    HWND Hwnd() const { return hwnd_; }
    // Keyboard navigation (IsDialogMessage; Ctrl+Tab / Ctrl+Shift+Tab: the next / previous page)
    // for messages to this window or its children. Returns true if the message was handled.
    bool PreTranslateMessage(MSG& msg);
    // Reloads every control from *host.config (after tray toggles, Reload config) and the header.
    void Refresh();
    // Selects a page (Global: SettingsIds::kPage*, Dock: kTab*), as a click in the nav / tabs would.
    void ShowPage(int index);
    int ActivePageIndex() const { return active_; }
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

    // --- for pages ---
    SettingsHost& Host() { return host_; }
    bool Loading() const { return loading_; }
    UINT Dpi() const { return dpi_; }
    int Px(int dip) const { return MulDiv(dip, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI); }
    const DarkMode::Palette& Colors() const { return palette_; }
    // A setting was applied to the dock: repaint the preview strip.
    void ConfigApplied();
    // The pages' width in DIPs (the client width less the nav column).
    int ContentWidthDip() const { return ClientDip().cx - (kind_ == SettingsKind::Global ? kNavWidthDip : 0); }
    // The dock's identity colour (Config::DockColor) as a COLORREF.
    COLORREF DockColor() const;

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

    // Client sizes in DIPs: Global 760 x 560, Dock 860 x 620, Item 480 x 560.
    SIZE ClientDip() const;
    static constexpr int kNavWidthDip = 200;
    static constexpr int kNavTopDip = 12;
    static constexpr int kNavItemDip = 36;
    static constexpr int kAboveBarDip = 24;    // gap between the window and the dock's bar
    static constexpr int kItemOffsetDip = 40;  // an Item window from its dock's window
    // The dock preview strip (SettingsPreview) under the Global Themes / Animation pages and every
    // tab of a Dock window but Contents (round 5 phase C: its item strip shows the items), content-
    // pane wide.
    static constexpr int kFooterDip = 96;
    static constexpr int kSubtitleDip = 34;     // Global: the subtitle band over the pages
    static constexpr int kDockHeaderDip = 128;  // Dock: the identity header
    static constexpr int kItemHeaderDip = 88;   // Item: the header
    static constexpr int kTabsDip = 42;         // Dock: the tab strip
    static constexpr int kItemFooterDip = 52;   // Item: the footer
    // Set to false to drop the Mica nav column (frame extension) if a driver misbehaves: the
    // nav column is then painted solid, as when transparency effects are off.
    static constexpr bool kNavMica = true;
    static constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;

    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK NavProcThunk(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK PageProcThunk(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK BandProcThunk(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK TabsProcThunk(HWND, UINT, WPARAM, LPARAM);
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
    // The header's chips and summary: "Bottom edge", "Primary", "5 items", "Shown".
    std::vector<std::wstring> HeaderChips();
    std::wstring WindowTitle();
    std::wstring ItemName();
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
    HWND tabs_ = nullptr;    // Dock with more than one page
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
    HBRUSH layerBrush_ = nullptr;
    HBRUSH baseBrush_ = nullptr;
    HBRUSH controlBrush_ = nullptr;
    HBRUSH bandBrush_ = nullptr;
    HICON iconSmall_ = nullptr;
    HICON iconBig_ = nullptr;
    HICON itemIcon_ = nullptr;     // Item: the header's icon (null: a letter tile)
    DockItemEntry iconFor_;        // the entry itemIcon_ was made for
    bool iconValid_ = false;
    UINT iconDpi_ = 0;
    std::wstring widgetLine_;      // Item (a widget): "id · version"
    std::wstring widgetLineFor_;   // the widget id it was read for
    // Widget previews (round 6 phase 6E): by "<widget id>|<w>x<h>"; valid for previewStamp_ / previewDpi_.
    std::map<std::wstring, PreviewImage> previews_;
    uint64_t previewStamp_ = 0;
    UINT previewDpi_ = 0;
    int previewRenders_ = 0;
    void ClearPreviews();
    void CheckPreviewStamp();
};

// The pages of a window (SettingsPages.cpp): Global in nav order (SettingsIds::kPage*), Dock in tab
// order (kTab*; a folder window entry: its one page), Item: its one page (kItemPage).
std::vector<std::unique_ptr<SettingsPage>> CreateSettingsPages(SettingsWindow& window);
