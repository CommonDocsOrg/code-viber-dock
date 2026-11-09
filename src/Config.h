#pragma once

#include <climits>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// docks[].items[].kind values this version knows (case-insensitive); Unknown for anything else.
enum class ItemKind { Unknown, App, Folder, Separator, RecycleBin, Running, Clock, Weather, Widget, Group, Tray, TrayApp,
                      Notifications };
ItemKind ItemKindOf(std::wstring_view kind);
const wchar_t* ToString(ItemKind v);  // the canonical JSON name ("" for Unknown)
// The kinds that are "pins" (the pinned section of the dock): app, folder, separator, recycleBin,
// group (a shortcut group holds pins of its own).
inline bool IsPinKind(ItemKind k) {
    return k == ItemKind::App || k == ItemKind::Folder || k == ItemKind::Separator || k == ItemKind::RecycleBin ||
           k == ItemKind::Group;
}

// How a folder / group item's popup shows its entries: "auto" (default: a grid up to
// FolderPopup::kAutoGridMax = 48 entries, a list above), "grid" or "list".
enum class ItemView { Auto, Grid, List };

// Round 7 phase 7B: where an item sits along a bar dock ("anchor"): with the dock's `align` (none,
// the default: no key), or in the start / centre / end group of its own. The groups are laid out
// in the bar as DockConfig::Anchored() describes.
enum class ItemAnchor { None, Start, Center, End };

// One element of a dock, in dock order. App / folder entries set path (or aumid); the other
// kinds set neither.
struct DockItemEntry {
    // "app", "folder", "separator", "recycleBin", "running" (the unpinned running apps), "clock",
    // "weather", "widget" (a script widget, docs/widgets.md). Kept verbatim, so a kind this version
    // does not know survives a save (such an item is not shown).
    std::wstring kind = L"app";
    std::wstring path;   // .lnk, .exe or a folder (a folder is also detected from a .lnk's target)
    std::wstring aumid;  // AppUserModelID for packaged apps
    // Custom icon: an image (.png .jpg .jpeg .bmp .gif), an .ico, or "file.exe,3" / "file.dll,-120"
    // (icon index, negative = resource id). Environment variables are expanded. Empty: the
    // default icon; a file that does not exist also keeps the default (the entry is kept as is).
    std::wstring icon;
    // "widget" items: the widget's id (its folder under %LocalAppData%\WinDock\widgets, or a
    // built-in) and its settings object, kept as compact JSON text ("" = none: the defaults).
    // "clock" / "weather" items keep their own settings here too (round 5 phase A: the keys of
    // ClockConfig / WeatherConfig, read over the top-level clock / weather defaults by ClockOf /
    // WeatherOf; Config::EnsureItemSettings fills a new item from those defaults).
    std::wstring id;
    std::wstring settings;
    // "folder" / "group" items: the popup's layout ("view"; saved only when not auto).
    ItemView view = ItemView::Auto;
    // "group" items (a shortcut group: one slot, its members in a popup): the name ("" = "Group")
    // and the members, app / folder entries saved as the group's own "items" array. Older
    // versions keep a group's kind and icon only (its name and members are lost if they save).
    std::wstring name;
    std::vector<DockItemEntry> members;
    // "tray" items (the notify icons of TrayHost as a grid, round 4 phase 8): rows across the bar
    // (1 - 2), columns along it (1 - 20; on a vertical dock the grid is `rows` wide and `columns`
    // tall), the icon size in DIPs (16 / 20 / 24), the icon keys never shown in the grid (they are
    // still in the drawer) and the user's order of icon keys (8b; icons not listed follow in
    // registration order). Saved only when not the default. "trayApp" items use `id` (the icon key:
    // "{guid}" or "exe-name.exe:uID").
    int rows = 1;
    int columns = 8;
    int trayIconSize = 20;
    std::vector<std::wstring> hidden;
    std::vector<std::wstring> order;
    // Round 7 phase 7B, every kind: the item's group along the bar ("anchor": "start" / "center" /
    // "end"; none = the group the dock's `align` names) and a shift of it and the items after it in
    // its group ("offset", DIPs along the edge, positive toward the end; -4000 - 4000). Each saved
    // only when set.
    ItemAnchor anchor = ItemAnchor::None;
    int offset = 0;

    ItemKind Kind() const { return ItemKindOf(kind); }
    bool IsPin() const { return IsPinKind(Kind()); }
    bool operator==(const DockItemEntry&) const = default;
};

// "off": always shown. "always": hides when the mouse leaves. "intelligent": hides only while
// the foreground window overlaps the dock.
enum class AutoHideMode { Off, Always, Intelligent };
// The screen edge a dock sits on: "bottom" (default), "top", "left", "right". Left / right docks
// are vertical (the icons stack along the edge).
enum class DockEdge { Bottom, Top, Left, Right };
inline bool IsVertical(DockEdge e) { return e == DockEdge::Left || e == DockEdge::Right; }
// Where along its edge a dock sits: "center" (default), "start" (left / top end), "end".
enum class DockAlign { Center, Start, End };
// A middle click on an app: "newInstance" (start another copy), "minimize" / "close" its windows,
// "none". Folders and the Recycle Bin always open in Explorer.
enum class MiddleClickAction { NewInstance, Minimize, Close, None };
// The mouse wheel over an app: "none", "cycle" (bring its next / previous window to the front).
enum class WheelAction { None, Cycle };
// Which windows a dock shows (its running apps, and the dots / window lists of its pinned apps):
// "all" (default), or "monitor": only windows whose centre is on the dock's monitor.
enum class RunningFilter { All, Monitor };
// Where a dock lives in the z-order: "normal" (topmost, default), "desktop": shown only while the
// desktop is visible (no window in the foreground), at the bottom of the z-order, or (round 6
// phase 6A) "behind": always shown, on the desktop layer under every app window (as folder windows
// are). The dashboard hotkey brings "desktop" and "behind" docks to the top until it is dismissed.
enum class DockLayer { Normal, Desktop, Behind };
// How a dock shows running windows: "stacked" (default: one slot per app, its windows behind it)
// or "unstacked" (one slot per window, grouped by app; a pinned app's pin slot is its first window
// and its other windows follow it as extra slots).
enum class WindowsMode { Stacked, Unstacked };
// Text under each icon: "off" (default), "one" (one line) or "two" (two lines), ellipsized; the
// window title for a window slot, else the app / item name.
enum class TitlesMode { Off, One, Two };
// Titles on left / right docks: "beside" (default: next to the icon, toward the screen edge) or
// "off".
enum class TitlesVertical { Off, Beside };
// Window thumbnails as dock items: "off" (default), "live" (Windows.Graphics.Capture: the tile
// follows the window while the dock is visible, at most thumbnailFps frames a second; GPU work in
// DWM and buffers the size of the window) or "snapshots" (PrintWindow every thumbnailIntervalS
// seconds; nothing runs in between).
enum class ThumbnailMode { Off, Live, Snapshots };
// When the run of items is longer than the room along the edge (the monitor's edge minus the
// margins): "scroll" (default: the bar keeps that length and the run scrolls inside it, with the
// wheel, a drag on the bar and edge fades), "shrink" (the icons get smaller until it fits, 16 DIP
// at least) or "clip" (the run goes on past the monitor's end, as before round 4 phase 11).
enum class OverflowMode { Scroll, Shrink, Clip };
// How the bar grows while magnified (round 9 phase 9A, docks[].magnifyBar): "steady" (default: it
// grows once to its magnified length, symmetrically about its resting centre, when the pointer
// first reaches something that zooms (an app, a folder, a group, the Recycle Bin, tray apps; a
// widget only when its hover makes it larger), and keeps that length, every widget and separator
// in place, until the pointer leaves it), "follow" (before round 9: anchored at the pointer, its ends move as the
// pointer walks the run) or "always" (the bar always has its magnified length: room for the
// magnified icons at both ends of the icons, nothing grows or shrinks). Rings, a scrolling bar and
// a full-width or anchored bar always grow as before.
enum class MagnifyBar { Steady, Follow, Always };
// The dock's shape (round 4 phase 11b): "bar" (default), "arc" (a half ring whose diameter lies
// on the screen edge, the items along the arc) or "circle" (a full ring floating on the monitor at
// x / y; it ignores the edge, alignment and offset).
enum class DockShape { Bar, Arc, Circle };
// What a docks[] entry is (round 4 phase 12, "kind"): "dock" (default), "folderWindow" (a
// titled panel on the desktop showing a folder's contents, FolderWindow) or (round 6 phase 6B)
// "widget" (a standalone script widget in a window of its own on the desktop, WidgetWindow). A
// docks[] entry with a kind this version does not know is kept verbatim (Config::extraDocks) and
// not shown.
enum class DockKind { Dock, FolderWindow, Widget };
// What a standalone widget's window draws under the widget: "none" (default: the widget over the
// wallpaper) or "panel" (the theme's bar fill / acrylic, rounded, with the bar hairline).
enum class WidgetBackground { None, Panel };
// A folder window's order: "name" (default; A-Z in Explorer's logical order), "date" (modified,
// newest first), "type" (extension A-Z, then name), "size" (largest first). Folders come first.
enum class FolderSort { Name, Date, Type, Size };

// The keys of a folderWindow entry beyond id / title / x / y / monitor (DockConfig). Rects are
// DIPs relative to the work area of its monitor; a folder window whose monitor is not connected
// is parked like a dock.
struct FolderWindowConfig {
    std::wstring path;           // the folder (environment variables expanded)
    int w = 400;                 // DIPs, 160 - 8000
    int h = 300;                 // DIPs, 80 - 8000 (the expanded height; collapsed shows the title bar)
    ItemView view = ItemView::Auto;  // auto: a grid up to 48 entries, a list above
    int iconSize = 48;           // DIPs, 32 - 96 (grid icons; list icons are half, 16 - 32)
    FolderSort sort = FolderSort::Name;
    bool showHidden = false;     // hidden entries too (never the protected system ones)
    bool collapsed = false;      // only the title bar (double-click the title)
    bool singleClick = false;    // a single click opens an entry (default: a double click)
    // Round 4 phase 12b. Rules: file patterns ("*.png;*.jpg", PathMatchSpec) for files on the
    // Desktop that belong in this window's folder; autoSort moves them there whenever the Desktop
    // changes (off: only "Sort desktop now"). Moves are undoable and never overwrite ("name (2)").
    std::wstring rules;
    bool autoSort = false;
    // Appearance: the whole panel's opacity (0.2 - 1), the title's colour (ARGB; 0 = the theme's
    // clock text), no title text (a 10 DIP strip to move it by), 1 or 2 lines of name under a grid
    // icon, no fill and no acrylic (the entries over the wallpaper, their text shadowed), and a
    // collapsed window that expands while the pointer is over it.
    double opacity = 1.0;
    uint32_t titleColor = 0;
    bool hideTitle = false;
    int labelLines = 2;
    bool transparentBackground = false;
    bool rollUpOnHover = false;
    // The number of patterns in `rules`.
    size_t RuleCount() const;
    bool operator==(const FolderWindowConfig&) const = default;
};

// The keys of a "widget" docks[] entry (round 6 phase 6B) beyond the DockConfig keys it shares with
// folder windows (id, x / y in DIPs from its monitor's work area (kCentred: centred), monitor,
// locked, layer (default "behind" for this kind), color). The widget itself (its id: the JSON key
// "widget"; its settings object: "settings") is kept as the entry's one item, DockConfig::items[0]
// (a { "kind": "widget" } item), so the generated Item settings page, Item windows and every item
// path work on it unchanged: item 0 of a widget entry is "the entry's own widget".
struct WidgetWindowConfig {
    static constexpr int kMinSize = 48;
    static constexpr int kMaxSize = 4000;
    int w = 200;  // DIPs, 48 - 4000
    int h = 200;
    WidgetBackground background = WidgetBackground::None;
    bool operator==(const WidgetWindowConfig&) const = default;
};

// Every field is listed in Theme::Fields() (Theme.h), which drives reading, writing and the UI.
// The defaults are the built-in "dark" preset.
struct ThemeConfig {
    uint32_t background = 0xCC202020;       // ARGB; the bar fill without acrylic
    float cornerRadius = 16.0f;             // DIPs (with acrylic: snapped to a system radius, 0/4/8)
    // Real blur behind the bar (a DWM system backdrop on a second, bar-sized window). Falls back
    // to `background` when unsupported or when Windows has transparency effects turned off.
    bool acrylic = true;
    uint32_t acrylicBackground = 0x66202020;  // tint drawn over the blur while acrylic is active
    uint32_t labelBackground = 0xE6303030;  // hover name label
    uint32_t labelText = 0xFFFFFFFF;
    uint32_t labelBorder = 0x1FFFFFFF;
    float labelCornerRadius = 7.0f;         // DIPs, 0 - 32
    uint32_t accent = 0xFF4CC2FF;           // insertion marker while a file is dragged onto the dock
    uint32_t barBorder = 0x24FFFFFF;        // hairline around the bar and the preview popup
    uint32_t separator = 0x38FFFFFF;        // between the app icons and the clock
    uint32_t dotRunning = 0x80FFFFFF;       // indicator dots
    uint32_t dotActive = 0xFFFFFFFF;        // the app owns the foreground window
    uint32_t dotAttention = 0xFFFF9E26;     // the app flashes for attention
    uint32_t clockText = 0xF2FFFFFF;
    uint32_t clockDateText = 0xB6FFFFFF;    // = clockText alpha 0.95 x 0.75, as drawn before
    uint32_t placeholderFill = 0xFF6B7385;  // icon placeholder (missing file / no icon)
    uint32_t placeholderText = 0xF2FFFFFF;
    uint32_t previewHotBackground = 0x1AFFFFFF;  // hovered preview card (half alpha: icon-only box)
    uint32_t previewHotBorder = 0x33FFFFFF;
    uint32_t closeHover = 0xFFC42B1C;       // preview card close button while hovered
    // Glass shelf: instead of a panel, the bar is drawn as a shelf in perspective under the icons
    // (top surface in `background`, sides angled in by `shelfSlant`, a front lip in `shelfEdge`,
    // highlights in `barBorder`). Acrylic is not used with it: the blur window is a rectangle.
    bool shelf = false;
    float shelfSlant = 14.0f;               // DIPs, 0 - 64
    uint32_t shelfEdge = 0xE6A0CDBE;
    // Icon effects, baked into each icon's surfaces when it is drawn (magnification, bounce and
    // drags only move them). Glow: a soft halo behind the icon (GlowMode as int: off / hover /
    // active app / always).
    int glow = 0;
    uint32_t glowColor = 0x00000000;        // alpha 0: the icon's dominant colour
    float glowRadius = 14.0f;               // DIPs, 4 - 32 (blur radius)
    float glowStrength = 0.6f;              // 0 - 1, halo opacity
    // A mirrored, fading copy of each icon under it (the bar grows by its height).
    bool reflection = false;
    float reflectionHeight = 0.5f;          // 0.2 - 1, fraction of the icon height
    float reflectionOpacity = 0.35f;        // 0 - 1, at the icon's foot (fading to 0)
    // The reflection is cut at the bar's edge instead of growing the bar: the dock keeps its height
    // and shows the part of the reflection that fits between the icons and the bar's edge.
    bool reflectionClip = false;
    // The icon row and the bar tilted back about the bar's bottom edge (cosmetic: hit-testing uses
    // the untilted layout). Acrylic is not used with it.
    bool perspective = false;
    float perspectiveAngle = 18.0f;         // degrees, 0 - 35
    // Notification badges on app slots (docks[].badges, round 4 phase 9): the bubble's fill, its
    // count's colour, and its height as a fraction of the icon size.
    uint32_t badgeFill = 0xFFFF3B30;
    uint32_t badgeText = 0xFFFFFFFF;
    float badgeSize = 0.36f;                // 0.2 - 0.6 of the icon
    // Round 11 phase 11A, the taskbar-button look. slotStyle (SlotStyle as int: none / buttons):
    // "buttons" draws a frame behind each app / folder / group / Recycle Bin / pinned tray icon slot
    // by its state (running, active, attention, hovered, pressed, stacked windows) and no
    // indicator dots. The colours are the frame fills (a vertical gradient to 60 % of the alpha at
    // the bottom), the hover fill added on top, and the hairline. Hot tracking: a radial light in
    // the icon's colour under the pointer while it moves inside the hovered frame.
    int slotStyle = 0;
    uint32_t slotRunning = 0x30FFFFFF;
    uint32_t slotActive = 0x55FFFFFF;
    uint32_t slotAttention = 0xD0F0A030;
    uint32_t slotHover = 0x28FFFFFF;
    uint32_t slotBorder = 0x66FFFFFF;
    float slotCornerRadius = 3.0f;          // DIPs, 0 - 16
    bool slotHotTrack = true;
    // A glossy highlight over the panel's upper 55 % (white at sheen x 35 % fading to 0) and a
    // 1 px white line at sheen x 60 % along that edge; 0 = none (not on the shelf, not on a ring).
    float sheen = 0.0f;                     // 0 - 1
    float iconGap = 8.0f;                   // DIPs, 0 - 48: between icon slots (a slot is iconSize + iconGap)
    // DIPs, 0 - 24: between the icon row and the bar's long edges (the bar is iconSize + 2 x this;
    // with the dots the icons sit at 5/8 of it from the far edge, as before: 5 of 8; with buttons
    // they are centred).
    float barPadding = 8.0f;
};

// ThemeConfig::slotStyle (the "slotStyle" key: "none", "buttons"; round 11 phase 11A).
enum class SlotStyle { None = 0, Buttons = 1 };
inline SlotStyle SlotStyleOf(const ThemeConfig& t) { return t.slotStyle == 1 ? SlotStyle::Buttons : SlotStyle::None; }

// ThemeConfig::glow (the "glow" key: "off", "hover", "active", "always").
enum class GlowMode { Off = 0, Hover = 1, Active = 2, Always = 3 };
inline GlowMode GlowOf(const ThemeConfig& t) {
    return t.glow >= 0 && t.glow <= 3 ? static_cast<GlowMode>(t.glow) : GlowMode::Off;
}


// --- round 12 phase 12C ---
// docks[].titleShow: whether the title (docks[].title, or the dock's name with titleUseName) is
// drawn: "off" (the text is kept, nothing is drawn), "hover" (it fades in while the pointer is over
// the dock), "always" (the default; written only when not the default). The order is QueryState
// 305's and the Position tab's "Show the title" combo's.
enum class TitleShow { Off, Hover, Always };
// docks[].titlePlace: "outside" (beyond the bar toward the middle of the screen, the default) or
// "inside" (a band inside the bar above the icon row; horizontal bars only: a vertical dock or a
// ring draws it outside). QueryState 306.
enum class TitlePlace { Outside, Inside };
// --- end round 12 phase 12C ---

// One dock: its contents and the settings that are per dock. Config::docks always holds at least
// one (docks[0], Config::Main()).
struct DockConfig {
    std::wstring id = L"main";
    // The dock's identity colour (ARGB; round 5): the settings windows' header and caption, the
    // Docks and displays page. 0 = not set: Config::DockColor picks one from a palette by the
    // dock's index. Written only when set.
    uint32_t color = 0;
    // Round 11 phase 11D (docks[].enabled, any kind): false turns the entry off without losing it:
    // no window (it is parked like a dock whose display is gone), no weather fetch, no tray
    // takeover or desktop rules for it; the settings windows still edit it. Written only when
    // false. At least one enabled dock (kind "dock") stays (App::EditDocks refuses the last).
    bool enabled = true;
    // "dock" or "folderWindow" (round 4 phase 12). A folder window uses id, title (empty: the
    // folder's name), x / y (DIPs from its monitor's work area's top-left; kCentred: centred),
    // monitor and `folder`; it has no items and ignores the other dock keys.
    DockKind dockKind = DockKind::Dock;
    FolderWindowConfig folder;
    bool IsFolderWindow() const { return dockKind == DockKind::FolderWindow; }
    // Round 6 phase 6B: a "widget" entry (a standalone widget, WidgetWindow): `widgetWindow`, the
    // widget as items[0] (WidgetItem), x / y, monitor, locked, layer, color.
    WidgetWindowConfig widgetWindow;
    bool IsWidgetWindow() const { return dockKind == DockKind::Widget; }
    // A real dock (not a folder window, not a standalone widget).
    bool IsDock() const { return dockKind == DockKind::Dock; }
    // A standalone widget entry's widget item (items[0]; null when it is not one or has none).
    const DockItemEntry* WidgetItem() const {
        return IsWidgetWindow() && !items.empty() && items.front().Kind() == ItemKind::Widget ? &items.front() : nullptr;
    }
    DockItemEntry* WidgetItem() {
        return IsWidgetWindow() && !items.empty() && items.front().Kind() == ItemKind::Widget ? &items.front() : nullptr;
    }
    // Defaults (first run, a malformed file, a dock object without "items"): the running apps and
    // the clock. The first run also seeds pins in front of them.
    std::vector<DockItemEntry> items = {DockItemEntry{L"running"}, DockItemEntry{L"clock"}};
    DockEdge edge = DockEdge::Bottom;  // unknown text reads as bottom
    DockAlign align = DockAlign::Center;
    // DIPs (pixels at 100 % scale) added to the aligned position along the edge (positive: right /
    // down). The dock is kept on its monitor.
    int offset = 0;
    // --- round 12 phase 12E ---
    // DIPs between the resting bar and its screen edge on top of the bar's own 8 DIP margin (0 -
    // 200, default 0; saved only when set): the bar moves off its edge toward the screen's middle.
    // Only for a bar on an edge that is not full width (a full-width bar sits on its edge; rings and
    // floating bars ignore it). The window grows by it, so auto-hide slides from there, the peek
    // strip and the reveal strip stay at the screen edge, and a reserved band includes the gap.
    int edgeGap = 0;
    static constexpr int kMaxEdgeGap = 200;
    // --- end round 12 phase 12E ---
    // The bar spans the whole edge of the monitor (flush with the screen edge, square corners);
    // the items sit in it per `align` / `offset`.
    bool fullWidth = false;
    // Round 7 phase 7A: the longest the bar gets along the edge, DIPs (0 = no limit, the default;
    // else 96 - 8000). A longer run overflows there as at the monitor's ends (docks[].overflow:
    // scroll / shrink; "clip" is not capped). Ignored for a ring and a full-width bar. Saved only
    // when set.
    int maxLength = 0;
    // With autoHide "off" (any edge, full width or not): the dock registers as an app bar so
    // maximized windows stop at its resting bar. The band spans the whole edge of the monitor (an
    // app bar cannot reserve part of an edge). Auto-hide docks reserve nothing (like an auto-hide
    // taskbar).
    bool reserveSpace = true;
    // Hidden by auto-hide: peekPx DIPs of the bar stay visible at peekOpacity (0: the invisible
    // 1 px reveal strip). Hovering the visible part reveals the dock.
    int peekPx = 0;
    double peekOpacity = 0.5;
    // Drawn outside the bar on the side facing the middle of the screen; empty: none.
    std::wstring title;
    // No drag reorder, no pinning by file drop, no Add separator / Add widget from the dock menu.
    // The settings window still edits everything.
    bool locked = false;
    int iconSize = 48;
    double magnification = 1.5;  // hovered icon scale; 1.0 disables magnification (1.0 - 2.5)
    int magnifyRange = 2;        // falloff radius in icon slots (1 - 4)
    // Round 9 phase 9A: how the bar grows while magnified ("steady" / "follow" / "always"). Saved
    // only when not "steady".
    MagnifyBar magnifyBar = MagnifyBar::Steady;
    AutoHideMode autoHide = AutoHideMode::Always;
    int hideDelayMs = 400;
    int revealDelayMs = 150;
    // Hover popup with live window thumbnails for apps with two or more windows (with
    // previewSingle: one or more).
    bool previews = true;
    bool previewSingle = false;  // "previewsSingleWindow"
    int previewDelayMs = 600;
    // Round 7 phase 7A: DIPs between a fully magnified icon (or the hover label above it) and the
    // popups the dock opens (window previews, calendar, folders, widget popups; 0 - 64). Saved only
    // when not 6.
    static constexpr int kDefaultPopupGap = 6;
    int popupGap = kDefaultPopupGap;
    // The monitor: an index (0 = the primary, 1.. = the others in enumeration order; a monitor
    // that is not connected parks the dock until it is), kMonitorPrimary ("primary" in JSON) or
    // kMonitorEach ("each": one dock per monitor, all from this config).
    static constexpr int kMonitorPrimary = -1;
    static constexpr int kMonitorEach = -2;
    int monitor = 0;
    MiddleClickAction middleClick = MiddleClickAction::NewInstance;
    WheelAction wheel = WheelAction::None;
    RunningFilter runningFilter = RunningFilter::All;
    // A theme preset for this dock only ("" = the app look: the top-level theme, its preset and its
    // inline fields). Round 12 phase 12B: a dock with a preset has its own look: that preset plus
    // its own inline fields (`theme`, the docks[] entry's "theme" object, with `themeOverrides`);
    // the app's inline fields no longer apply to it (round 12 decision 2: no layering).
    std::wstring themePreset;
    uint64_t themeOverrides = 0;  // round 12 phase 12B: one bit per Theme::Field set in docks[].theme
    bool OwnLook() const { return !themePreset.empty(); }
    DockLayer layer = DockLayer::Normal;
    WindowsMode windows = WindowsMode::Stacked;
    TitlesMode titles = TitlesMode::Off;
    TitlesVertical titlesVertical = TitlesVertical::Beside;
    // Thumbnails: each window slot (unstacked; stacked: an app slot with windows, its active
    // window) shows the window as a tile with the app icon in a corner.
    ThumbnailMode thumbnails = ThumbnailMode::Off;
    int thumbnailIntervalS = 5;  // snapshots: seconds between pictures (1 - 3600)
    int thumbnailSize = 0;       // the tile's length along the dock in DIPs (0: twice iconSize; 16 - 512)
    int thumbnailFps = 10;       // live: at most this many frames a second per window (1 - 60)
    // Round 4 phase 9: a bubble with the unread notification count on the app slots
    // (NotificationService; the per-app toast count). Saved only when on.
    bool badges = false;
    // Round 4 phase 11a: a run longer than the edge scrolls / shrinks / is clipped. Saved only when
    // not "scroll".
    OverflowMode overflow = OverflowMode::Scroll;
    // Round 4 phase 11b: the shape, the ring's radius (DIPs to the icons' centre line; 0 = from the
    // items so they do not overlap, else 60 - 2000, both kept on the monitor), icons turned with
    // the ring (default upright), and a circle's centre in DIPs from its monitor's work area's
    // top-left (kCentred: the work area's middle). Each is written only when not the default.
    static constexpr int kCentred = INT_MIN;
    DockShape shape = DockShape::Bar;
    int radius = 0;
    bool rotateIcons = false;
    int x = kCentred;
    int y = kCentred;
    bool Polar() const { return shape != DockShape::Bar; }
    // Round 6 phase 6A: a floating dock ("floating": true, written only when true) is a bar placed
    // at x / y (DIPs from its monitor's work area's top-left: the resting bar's top-left corner;
    // kCentred: centred) instead of on an edge; `edge` is the way it faces (where magnification
    // grows and popups open). align / offset / fullWidth / reserveSpace / peek do not apply and
    // auto-hide is off. Moved by dragging its bar background (unless locked). A dock with another
    // shape is not floating (Sanitize clears the flag).
    bool floating = false;
    bool Floating() const { return floating && shape == DockShape::Bar && dockKind == DockKind::Dock; }

    // Config::ResolveDockThemes: ownTheme = the dock has its own look (not saved), `theme` its values:
    // the fields in themeOverrides are the dock's own (saved as docks[].theme, round 12 phase 12B),
    // the others its preset's. ownTheme false: the app's theme applies (`theme` unused).
    bool ownTheme = false;
    ThemeConfig theme;

    bool AutoHideOn() const { return autoHide != AutoHideMode::Off && !Floating(); }
    bool Each() const { return monitor == kMonitorEach; }
    // An item of this kind is in `items` (the clock / weather widget is shown).
    bool Has(ItemKind kind) const;
    // Adds an item of `kind` at the end when there is none (on), or removes every item of that
    // kind (off). Returns true if `items` changed.
    bool Show(ItemKind kind, bool on);
    // Adds a clock, weather, tray, notifications or running item (Add widget): the clock at the end, the weather
    // before a clock that ends the list (else at the end), the running apps after the last pin
    // (no pins: first). A second `running` item is refused (only the first one expands). Returns
    // the new item's index, or SIZE_MAX if nothing was added.
    size_t AddWidget(ItemKind kind);
    // Adds a script widget item with this id where AddWidget puts a weather item (before a clock
    // that ends the list, else at the end). Returns its index.
    size_t AddScriptWidget(const std::wstring& widgetId);
    // Moves item `from` so it comes right before the item now at `to` (to >= items.size(): to the
    // end). Returns true if the order changed.
    bool MoveItem(size_t from, size_t to);

    // Round 7 phase 7B: item anchors. An item's group: 0 start, 1 centre, 2 end (its own anchor, else
    // the one the dock's `align` names). Anchored(): some item has an anchor or an offset; only then
    // does a bar dock lay its items out as groups (start items packed from the bar's start, end items
    // packed to its end, centre items centred as a block, each offset shifting its item and the ones
    // after it in the group) in a bar as long as the room along the edge. AnchorFor(group): the anchor
    // that puts an item into `group` (None for the group `align` names: untouched docks never gain
    // anchor keys). SortByGroup(): stably reorders `items` start -> centre -> end (the order the
    // layout needs; Sanitize and every item edit keep it); `track` (an index into items) follows its
    // item. Returns true if the order changed.
    static constexpr int kMaxItemOffset = 4000;
    int AlignGroup() const { return align == DockAlign::Start ? 0 : align == DockAlign::End ? 2 : 1; }
    int GroupOf(const DockItemEntry& e) const {
        return e.anchor == ItemAnchor::Start ? 0 : e.anchor == ItemAnchor::Center ? 1 : e.anchor == ItemAnchor::End ? 2 : AlignGroup();
    }
    int GroupOf(size_t item) const { return item < items.size() ? GroupOf(items[item]) : AlignGroup(); }
    ItemAnchor AnchorFor(int group) const {
        if (group == AlignGroup()) return ItemAnchor::None;
        return group == 0 ? ItemAnchor::Start : group == 2 ? ItemAnchor::End : ItemAnchor::Center;
    }
    bool Anchored() const;
    bool SortByGroup(size_t* track = nullptr);
    // Item `at` was just put there (inserted, moved): on a dock with anchors it joins the group of
    // its neighbours when neither is in its own group (the one before it, else the one after it).
    void JoinNeighbourGroup(size_t at);

    // The pins (IsPin() items) in order, and their indexes into `items`. A "pin index" k below
    // is the position among the pins, which is what the dock's pinned section and its resolved
    // pins (DockWindow::Pins()) are parallel to.
    std::vector<size_t> PinIndexes() const;
    size_t PinCount() const;
    std::vector<DockItemEntry> Pins() const;
    DockItemEntry& PinAt(size_t k);  // k < PinCount()
    // Inserts `e` so it becomes pin k (k >= PinCount(): after the last pin; without any pin,
    // before the first `running` item, else at the end).
    void InsertPin(size_t k, DockItemEntry e);
    void ErasePin(size_t k);
    // Replaces the pins with `pins`: pin slots are refilled in order, missing ones are removed,
    // extra pins go after the last pin (as InsertPin does). The other items keep their places.
    void SetPins(std::vector<DockItemEntry> pins);

    // --- round 12 phase 12C: names and titles ---
    // docks[].name: what people see (the Dock header, menus, Docks and displays, window titles,
    // undo labels); empty = the id. The id stays the stable key (automation, widget data, config
    // references), so a rename never breaks anything. Written only when set; at most 64 characters.
    std::wstring name;
    TitleShow titleShow = TitleShow::Always;
    TitlePlace titlePlace = TitlePlace::Outside;
    // docks[].titleUseName: the title's text is the dock's display name instead of `title`.
    bool titleUseName = false;
    const std::wstring& DisplayName() const { return name.empty() ? id : name; }
    // The text the dock draws as its title: the display name (titleUseName) or `title`; "" when
    // titleShow is off or there is none.
    std::wstring TitleText() const {
        if (titleShow == TitleShow::Off) return {};
        return titleUseName ? DisplayName() : title;
    }
    // --- end round 12 phase 12C ---
};

// What a click on the clock does: "calendar" (the month calendar popup), "launch" (open
// clock.clickTarget), "none".
enum class ClockClick { Calendar, Launch, None };

struct ClockConfig {
    // Shown when the dock has a "clock" item (DockConfig::Has(ItemKind::Clock)).
    std::wstring format = L"%H:%M";
    // Adds ":%S" after the minutes of `format` (Clock::EffectiveFormat); a format that shows
    // seconds already is used as it is.
    bool showSeconds = false;
    bool showDate = false;
    // Without a "click" key, the older "calendarPopup": false reads as "none".
    ClockClick click = ClockClick::Calendar;
    // An app / file path or a URL, opened by ShellExecute ("%VAR%" expanded; a quoted path may be
    // followed by arguments). Used by click "launch" and by the calendar popup's footer button.
    std::wstring clickTarget;
    // "digital" (the classic rendering) or the id of a digital face file (ClockFaces: shipped, or
    // %LocalAppData%\WinDock\clock-faces\<id>.json). Written only when not "digital".
    std::wstring face = L"digital";
    // Round 5 phase A. The zone the clock shows: "" = the local time, else a Windows time zone key
    // name ("Tokyo Standard Time", the list of EnumDynamicTimeZoneInformation; an unknown name reads
    // as local). A label drawn on the date's line in its colour (before the date, or alone under the
    // time without one; "" = none, e.g. "Tokyo"). Both written only when set.
    std::wstring timeZone;
    std::wstring label;
    bool operator==(const ClockConfig&) const = default;
};

// Temperatures in °C and wind in km/h ("metric") or °F and mph ("imperial").
enum class Units { Metric, Imperial };

// The weather widget (Open-Meteo, no API key). Unconfigured (latitude and longitude both 0) it
// shows a "set location" glyph instead of the conditions.
// Shown when the dock has a "weather" item (DockConfig::Has(ItemKind::Weather)).
struct WeatherConfig {
    double latitude = 0.0;   // -90 - 90
    double longitude = 0.0;  // -180 - 180
    std::wstring location;   // display name (header of the popup); not used to look anything up
    Units units = Units::Metric;  // without a "units" key: from the locale (LOCALE_IMEASURE)
    int refreshMinutes = 30;  // 5 - 360
    // Round 5 phase A: the name the widget shows under its temperature ("" = none). Written only
    // when set.
    std::wstring label;
    bool Configured() const { return latitude != 0.0 || longitude != 0.0; }
    bool operator==(const WeatherConfig&) const = default;
};
// The user's locale measurement system (LOCALE_IMEASURE: 1 = U.S.) as Units.
Units LocaleUnits();

// How the dock hides: "slide" (slide + fade), "slideOnly", "fade", "none" (instant).
enum class HideStyle { Slide, SlideOnly, Fade, None };
// Feedback while an app launches: "bounce" (repeating), "hop" (one bounce), "pulse" (opacity), "none".
enum class LaunchStyle { Bounce, Hop, Pulse, None };
// Magnification falloff over `magnifyRange` slots.
enum class Falloff { Cosine, Linear };
// Settle animations (rest, slide, label). Also used by the Renderer.
enum class Easing { EaseOut, Linear, Spring };
// "system": follow Settings > Accessibility > Visual effects > Animation effects.
enum class ReduceMotion { System, On, Off };

// JSON strings for the enums above (shared by config.json, README and the settings UI).
// FromString is case-insensitive and leaves `v` unchanged (returning false) for unknown text.
const wchar_t* ToString(AutoHideMode v);
const wchar_t* ToString(HideStyle v);
const wchar_t* ToString(LaunchStyle v);
const wchar_t* ToString(Falloff v);
const wchar_t* ToString(Easing v);
const wchar_t* ToString(ReduceMotion v);
const wchar_t* ToString(Units v);
const wchar_t* ToString(MiddleClickAction v);
const wchar_t* ToString(WheelAction v);
const wchar_t* ToString(ClockClick v);
const wchar_t* ToString(DockEdge v);
const wchar_t* ToString(DockAlign v);
const wchar_t* ToString(RunningFilter v);
const wchar_t* ToString(DockLayer v);
const wchar_t* ToString(WindowsMode v);
const wchar_t* ToString(TitlesMode v);
const wchar_t* ToString(TitlesVertical v);
const wchar_t* ToString(ThumbnailMode v);
bool FromString(std::wstring_view s, ThumbnailMode& v);
const wchar_t* ToString(OverflowMode v);
bool FromString(std::wstring_view s, OverflowMode& v);
const wchar_t* ToString(MagnifyBar v);
bool FromString(std::wstring_view s, MagnifyBar& v);
const wchar_t* ToString(DockShape v);
bool FromString(std::wstring_view s, DockShape& v);
const wchar_t* ToString(ItemView v);
bool FromString(std::wstring_view s, ItemView& v);
const wchar_t* ToString(ItemAnchor v);  // "none" for None (never written)
bool FromString(std::wstring_view s, ItemAnchor& v);
const wchar_t* ToString(FolderSort v);
bool FromString(std::wstring_view s, FolderSort& v);
const wchar_t* ToString(WidgetBackground v);
bool FromString(std::wstring_view s, WidgetBackground& v);
bool FromString(std::wstring_view s, WindowsMode& v);
bool FromString(std::wstring_view s, TitlesMode& v);
bool FromString(std::wstring_view s, TitlesVertical& v);
bool FromString(std::wstring_view s, RunningFilter& v);
bool FromString(std::wstring_view s, DockLayer& v);
bool FromString(std::wstring_view s, DockEdge& v);
bool FromString(std::wstring_view s, DockAlign& v);
bool FromString(std::wstring_view s, MiddleClickAction& v);
bool FromString(std::wstring_view s, WheelAction& v);
bool FromString(std::wstring_view s, ClockClick& v);
bool FromString(std::wstring_view s, AutoHideMode& v);
bool FromString(std::wstring_view s, HideStyle& v);
bool FromString(std::wstring_view s, LaunchStyle& v);
bool FromString(std::wstring_view s, Falloff& v);
bool FromString(std::wstring_view s, Easing& v);
bool FromString(std::wstring_view s, ReduceMotion& v);
bool FromString(std::wstring_view s, Units& v);

struct AnimationConfig {
    double speed = 1.0;  // 0 = animations off; otherwise 0.25 - 4.0 (durations are divided by it)
    HideStyle hideStyle = HideStyle::Slide;
    LaunchStyle launchStyle = LaunchStyle::Bounce;
    Falloff magnifyFalloff = Falloff::Cosine;
    Easing easing = Easing::EaseOut;
    int labelFadeMs = 120;  // 0 = instant; 0 - 1000
    ReduceMotion reduceMotion = ReduceMotion::System;
};

// The system tray takeover (TrayHost, round 4 phase 8): off by default. When on and a dock has a
// "tray" or "trayApp" item, the dock owns Shell_TrayWnd: every app's notify icon registers with
// it (and is mirrored to Explorer). Saved only when on.
struct TrayConfig {
    bool enabled = false;
};

// Windows' notifications (round 4 phase 9: NotificationService). The service runs only while some
// dock shows badges or a notifications element, a widget uses the `notifications` provider or the
// centre popup is open. Saved only when something differs from the defaults.
struct NotificationsConfig {
    int pollS = 2;               // seconds between polls of the notification list (1 - 60; see NotificationService.h)
    bool clearOnActivate = true; // activating an app from the dock marks its notifications read (its badge goes)
    // Tray balloons (NIF_INFO) go to the dock's notification centre only: Explorer gets them
    // without NIF_INFO, so Windows does not show them as toasts too.
    bool balloons = false;
    // App keys (AppKey ids: a lower-case exe path or AppUserModelID) that never show a badge.
    std::vector<std::wstring> excluded;
    bool operator==(const NotificationsConfig&) const = default;
};

// Calendar feeds (round 4 phase 13: CalendarService). ICS only: an http(s) URL, a webcal:// URL
// (fetched over https), a file path or a file:/// URL. The service runs only while at least one
// feed is enabled. Saved only when something differs from the defaults.
struct CalendarFeed {
    std::wstring name;
    std::wstring url;
    uint32_t color = 0xFF4CC2FF;  // "#AARRGGBB"; without a "color" key: the next colour of a fixed palette
    bool enabled = true;
    int refreshMin = 30;          // 5 - 1440 (web feeds; a file is re-read when it changes, never polled)
    int reminderMin = 0;          // a reminder this many minutes before each timed event (0: calendars.defaultReminderMin)
    bool operator==(const CalendarFeed&) const = default;
};
struct CalendarsConfig {
    std::vector<CalendarFeed> feeds;  // at most 32
    bool showDots = true;             // dots on event days (the calendar popup, the clock's date)
    int defaultReminderMin = 0;       // reminders before events, minutes (0: off; 0 - 1440)
    bool operator==(const CalendarsConfig&) const = default;
};
// The palette for feeds without a colour (index modulo its size).
uint32_t DefaultFeedColor(size_t index);

// The dashboard (round 6 phase 6A): one global hotkey brings every "desktop" / "behind" dock to the
// top of the z-order until it is pressed again, Escape is pressed in one of them or another
// window takes the foreground. `hotkey`: "Mod+Mod+Key" in Hotkey's canonical spelling (Win, Ctrl,
// Alt, Shift; a key name), "" = none. Saved only when not the default.
struct DashboardConfig {
    std::wstring hotkey = L"Win+Shift+D";
    bool operator==(const DashboardConfig&) const = default;
};

// The settings windows (round 8 phase 8A): the last client size of each kind of window in DIPs,
// `settings.windows` = {"global": [w, h], "dock": [w, h], "item": [w, h]}. {0, 0} = the default
// size; a kind is saved only when its size is not the default (Sanitize clamps to the kind's
// minimum and 4000, and a size equal to the default becomes {0, 0}).
struct SettingsWindowSize {
    int w = 0;
    int h = 0;
    bool operator==(const SettingsWindowSize&) const = default;
};
struct SettingsUiConfig {
    // Index: 0 Global, 1 Dock, 2 Item (SettingsKind order).
    static constexpr int kKinds = 3;
    // Round 12 phase 12F: kDefault is the floor of the size a window without a saved one opens at:
    // App and Item windows take 60 % x 70 % of their display's work area, at least kDefault and at
    // most kLargest (SettingsWindow::DefaultClientDip). The Dock slot: a dock's panel opens at
    // 470 x 640 (12E); round 12 phase 12I: so does a folder window's / standalone widget's (their
    // captioned window, which opened at this slot's kDefault / kMinimum, is gone; the values stay
    // for the table's shape). A saved size always wins (clamped to the panel's room).
    static constexpr SettingsWindowSize kDefault[kKinds] = {{760, 560}, {860, 620}, {600, 600}};
    static constexpr SettingsWindowSize kLargest[kKinds] = {{1280, 860}, {860, 620}, {800, 760}};
    static constexpr SettingsWindowSize kMinimum[kKinds] = {{700, 500}, {780, 540}, {520, 480}};
    // Round 12 phase 12F: what Sanitize clamps a saved size to: the Dock slot also holds the panel's
    // size since 12E, whose minimum is 400 x 360 (kMinimum's 780 x 540 widened a narrow panel on load).
    static constexpr SettingsWindowSize kSavedMinimum[kKinds] = {{700, 500}, {400, 360}, {520, 480}};
    static constexpr int kMaximum = 4000;
    static constexpr const wchar_t* kNames[kKinds] = {L"global", L"dock", L"item"};
    SettingsWindowSize windows[kKinds];
    // Round 9 phase 9A: `settings.fullscreen` "hide" (default: true) hides every topmost dock and
    // standalone widget on a display while a full-screen app is in front there (DockManager's
    // full-screen guard), "ignore" (false) keeps them as they are. Saved only when "ignore".
    bool fullscreenHide = true;
    // Round 12 phase 12A: `settings.confirmPlacement` (default true; written only when false): a
    // change that moves or hides a dock (edge, floating, display, auto-hide, layer, turned off)
    // shows Keep / Revert for 12 s (kept when left alone). False: no bar.
    bool confirmPlacement = true;
    // --- round 12 phase 12G ---
    // `settings.tipsShown` (an int bitmask, written only when not 0): bit n = tip n on the dock was
    // seen, skipped, or its gesture was done already (0 the dock menu opened, 1 customize mode
    // entered, 2 a dock dragged). UI state like the windows' sizes: not part of an undo snapshot
    // (ToJson(false) leaves it out, App::ApplySnapshot keeps the current bits).
    static constexpr int kTipCount = 3;
    int tipsShown = 0;
    // --- end round 12 phase 12G ---
    bool operator==(const SettingsUiConfig& o) const {
        for (int i = 0; i < kKinds; ++i) {
            if (!(windows[i] == o.windows[i])) return false;
        }
        return fullscreenHide == o.fullscreenHide && confirmPlacement == o.confirmPlacement && tipsShown == o.tipsShown;
    }
};

struct Config {
    // At least one dock after LoadFile / Sanitize; ids are unique (Sanitize). Every dock is shown
    // (DockManager), docks[].monitor "each" once per monitor.
    std::vector<DockConfig> docks = std::vector<DockConfig>(1);
    bool hideTaskbar = true;
    bool startWithWindows = false;
    // "auto": hardware D3D11 device, WARP if that fails. "software": WARP only, which avoids the
    // GPU user-mode driver's per-process memory (the dock redraws rarely, so WARP is cheap).
    std::wstring renderer = L"auto";
    // The defaults for new clock / weather items (round 5 phase A): every clock / weather item
    // keeps its own settings (DockItemEntry::settings; ClockOf / WeatherOf read them over these).
    // Still read and written as the top-level "clock" / "weather" objects.
    ClockConfig clock;
    WeatherConfig weather;
    // Theme layering: built-in preset -> user theme file (themes\<id>.json, which names its own
    // built-in `base`) -> fields set inline in config.json. `themePreset` is a built-in id or a
    // user theme file stem; "" means none (inline fields over the dark defaults).
    std::wstring themePreset = L"dark";
    uint64_t themeOverrides = 0;  // one bit per Theme::Field set inline (only those are saved)
    ThemeConfig theme;            // RESOLVED values: what the dock draws with
    bool themeResolveFailed = false;  // the last ResolveTheme() fell back to dark (not saved)
    AnimationConfig animation;
    TrayConfig tray;
    NotificationsConfig notifications;
    CalendarsConfig calendars;
    DashboardConfig dashboard;
    SettingsUiConfig settings;  // round 8 phase 8A: `settings.windows`
    // Top-level keys this version does not know, kept verbatim (compact JSON) so a save does not
    // drop them. Unknown keys inside known objects (docks, items, clock, theme, animation, weather)
    // are not kept, nor are the legacy pre-`docks` keys.
    std::vector<std::pair<std::wstring, std::wstring>> extraFields;
    // docks[] entries of a kind this version does not know (compact JSON), written back after the
    // known ones.
    std::vector<std::wstring> extraDocks;
    // The last LoadFile() built `docks` from the legacy keys (pinned, iconSize, clock.show ...), or
    // gave clock / weather items their own settings (EnsureItemSettings, round 5): the caller
    // saves, so the file is rewritten in the current format. Not saved.
    bool migrated = false;

    DockConfig& Main() { return docks.front(); }
    const DockConfig& Main() const { return docks.front(); }
    // The theme a dock draws with: its own preset's (DockConfig::ownTheme), else the global one.
    const ThemeConfig& ThemeOf(const DockConfig& d) const { return d.ownTheme ? d.theme : theme; }
    // A dock id that no dock has yet: `base`, else base2, base3 ...
    std::wstring UniqueDockId(const std::wstring& base) const;
    // Dock `index`'s identity colour: its `color`, else the palette by index (DockPaletteColor).
    uint32_t DockColor(size_t index) const;
    // Legacy native weather items become weather script widgets, preserving their settings.
    // Every clock / weather item without settings gets them from the defaults (`clock` /
    // `weather`). Returns true if an item changed. Load runs it (a file written before round 5:
    // `migrated`, the caller saves); the app runs it after items are added.
    bool EnsureItemSettings();

    // Re-bases `theme` on `themePreset` for every field whose override bit is clear. Returns
    // false (and uses dark for those fields, keeping `themePreset` so the user can fix it) when
    // the preset is unknown or its theme file is unreadable / malformed. Also ResolveDockThemes.
    bool ResolveTheme();
    // Every dock with its own look (themePreset set): its preset with its own inline fields
    // (DockConfig::themeOverrides) on top; round 12 phase 12B: the app's inline fields do not apply
    // to it. A preset that does not resolve (unknown, a broken file) is based on the app's resolved
    // theme instead (what such a dock showed before 12B).
    void ResolveDockThemes();
    // Round 12 phase 12B: the preset values a look is based on (the app's: Theme::Resolve of
    // themePreset; a dock's own look: its preset, else the app's resolved theme as above).
    ThemeConfig PresetOf(const DockConfig* dock) const;
    // "Give this dock its own look": the app's current look (preset and inline fields) becomes the
    // dock's own (the app's "" preset, the dark defaults, as "dark").
    void GiveOwnLook(DockConfig& d) const;
    // "Use the app look again": no preset, no fields of its own.
    static void UseAppLook(DockConfig& d);
    // `target` gets the look `source` shows (null: the app's) as its own (Apply to other docks).
    void CopyLook(const DockConfig* source, DockConfig& target) const;

    // %LocalAppData%\WinDock
    static std::wstring Directory();
    // %LocalAppData%\WinDock\config.json
    static std::wstring FilePath();

    enum class LoadResult { Ok, Missing, Malformed };
    // Loads the config file into *this (defaults for anything missing). Missing file: defaults.
    // Malformed file: defaults (the file is left untouched so the user can fix it). Never throws.
    LoadResult LoadFile();
    // Round 12 phase 12A: the same from the file's text (an undo snapshot; ToJson's output): Ok,
    // or Malformed (defaults). Never throws. LoadFile reads the file and calls it.
    LoadResult LoadJson(const std::wstring& text);
    // LoadFile() == Missing, i.e. first run: the caller seeds defaults and saves.
    bool Load() { return LoadFile() == LoadResult::Missing; }
    // Round 12 phase 12A: the file's text, exactly what Save writes; `windowSizes` false leaves the
    // settings windows' sizes out (`settings.windows`: an undo snapshot must not record a resize).
    // May throw (winrt string quoting); Save catches.
    std::wstring ToJson(bool windowSizes = true) const;
    // Writes the config file atomically (temp file + rename). Never throws. Round 12 phase 12A:
    // `error` (optional) gets the Win32 error when it fails (JsonUtil::WriteFileAtomic).
    bool Save(unsigned long* error = nullptr) const;  // unsigned long: DWORD (this header has no windows.h)

    // Clamp values to sane ranges.
    void Sanitize();

    // --- round 12 phase 12C ---
    // docks[index]'s name as people see it (its `name`, else its id; "" for no such entry).
    std::wstring DockDisplayName(size_t index) const {
        return index < docks.size() ? docks[index].DisplayName() : std::wstring();
    }
    // %LocalAppData%\WinDock\layouts (saved layouts: Presets::Mine, Presets::SaveLayout).
    static std::wstring LayoutsDirectory();
    // One docks[] entry of kind "dock" as the JSON object the config file holds for it (exactly the
    // keys Save writes for it, its "items" included). "" when it cannot be written.
    std::wstring DockJson(size_t index) const;
    // --- end round 12 phase 12C ---
};

// Round 9 phase 9C: a dock object as JSON text (the keys of a docks[] entry of kind "dock" and its
// "items" array) read over `d` exactly as LoadFile reads a docks[] entry (unknown keys ignored,
// an unknown item kind kept as written: a preset's { "kind": "pins" } marker). False when the text
// is not a JSON object. Not sanitized (the caller runs Config::Sanitize).
bool ReadDockJson(const std::wstring& text, DockConfig& d);

// "#AARRGGBB" or "#RRGGBB" -> ARGB. Returns false on parse failure.
bool ParseArgbColor(const std::wstring& text, uint32_t& argb);
std::wstring FormatArgbColor(uint32_t argb);

// The identity colour palette for docks without `color` (blue, orange, green, purple, pink, teal,
// repeating by index).
uint32_t DockPaletteColor(size_t index);

// Per-item clock / weather settings (round 5 phase A). The effective config of a clock / weather
// item: its `settings` object read over the defaults (config.clock / config.weather), sanitized
// (unknown or out-of-range values keep the default). ClockOf of another kind: the defaults;
// WeatherOf reads any item's settings (a script weather widget's use the same keys).
ClockConfig ClockOf(const Config& config, const DockItemEntry& item);
WeatherConfig WeatherOf(const Config& config, const DockItemEntry& item);
// The weather a script widget item shows (the `weather` provider): its own place (latitude /
// longitude in its settings) with its units / refreshMinutes / location / label over the
// defaults, else the dock's first native weather item, else the defaults.
WeatherConfig ScriptWeatherOf(const Config& config, const DockConfig& dock, const DockItemEntry& item);
// A widget item's settings name a place (`latitude` / `longitude` numbers, not both 0).
bool SettingsHavePlace(const std::wstring& settingsJson);
// The settings objects (compact JSON) of a clock / weather item with every key of the config:
// what migration and new items get, what the settings page starts from.
std::wstring ClockSettingsJson(const ClockConfig& c);
std::wstring WeatherSettingsJson(const WeatherConfig& w);
