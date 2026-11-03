#pragma once

// Stable control ids of the settings window (automation: FindWindowEx / GetDlgItem on the page
// windows, then WM_COMMAND / BM_CLICK / TBM_SETPOS ...). Never renumber; add new ids at the end
// of a page's range. Check boxes and radio buttons have no visible text of their own: the
// clickable caption next to them is a separate static (id + 1) that forwards clicks.
namespace SettingsIds {

// Frame (children of the WinDock.Settings window).
inline constexpr int kNav = 100;        // WinDock.SettingsNav; window text = selected page name
inline constexpr int kPageBase = 200;   // page windows: kPageBase + page index (see kPage*)

// Page indices (nav order).
inline constexpr int kPageGeneral = 0;
inline constexpr int kPageAppearance = 1;
inline constexpr int kPageBehaviour = 2;
inline constexpr int kPageAnimation = 3;
inline constexpr int kPagePinned = 4;   // "Dock contents" (was "Pinned apps")
inline constexpr int kPageDockContents = kPagePinned;
inline constexpr int kPageAbout = 5;
inline constexpr int kPageWeather = 6;  // appended after About (pages are shown in index order)
inline constexpr int kPageDocks = 7;    // round 4 phase 3: the list of docks; picks the dock the other pages edit
inline constexpr int kPageLayout = 8;   // round 4 phase 3b: the monitor canvas (drag the docks)
inline constexpr int kPageWidgets = 9;  // round 4 phase 4b: installed widgets, Console, export
inline constexpr int kPageWidgetSettings = 10;  // phase 4b: one widget item's settings, generated from its schema
inline constexpr int kPageCount = 11;

// Every page: its title static.
inline constexpr int kPageTitle = 2000;
// Placeholder pages of B1 ("Coming in the next update"); unused since B2, kept reserved.
inline constexpr int kPlaceholderText = 2001;

// General (2100 - 2199)
inline constexpr int kIconSizeLabel = 2100;
inline constexpr int kIconSizeTrack = 2101;   // trackbar 16 - 128 (applies on release)
inline constexpr int kIconSizeEdit = 2102;    // 16 - 256
inline constexpr int kIconSizeSpin = 2103;
inline constexpr int kMagnifyLabel = 2104;
inline constexpr int kMagnifyTrack = 2105;    // positions 20 - 50 = magnification x 20 (1.00 - 2.50)
inline constexpr int kMagnifyValue = 2106;    // "1.50x"
inline constexpr int kRangeLabel = 2107;
inline constexpr int kRangeEdit = 2108;       // 1 - 4
inline constexpr int kRangeSpin = 2109;
inline constexpr int kMonitorLabel = 2110;
inline constexpr int kMonitorCombo = 2111;    // item index = config "monitor"
inline constexpr int kClockShow = 2112;       // check box (+ caption 2113)
inline constexpr int kClockShowCaption = 2113;
inline constexpr int kClockFormatLabel = 2114;
inline constexpr int kClockFormatCombo = 2115;  // editable; applies on selection, Enter or focus loss
inline constexpr int kClockPreview = 2116;
inline constexpr int kClockDate = 2117;
inline constexpr int kClockDateCaption = 2118;
inline constexpr int kStartWithWindows = 2119;
inline constexpr int kStartWithWindowsCaption = 2120;
inline constexpr int kHideTaskbar = 2121;
inline constexpr int kHideTaskbarCaption = 2122;
inline constexpr int kRendererLabel = 2123;
inline constexpr int kRendererCombo = 2124;   // 0 auto, 1 software
inline constexpr int kRendererNote = 2125;
inline constexpr int kSectionDock = 2126;
inline constexpr int kSectionClock = 2127;
inline constexpr int kSectionStartup = 2128;
inline constexpr int kSectionRenderer = 2129;
// 2130 / 2131: the "Click the clock to show a month calendar" check box until 1c (replaced by
// kClockClickCombo); reserved.
inline constexpr int kClockCalendar = 2130;
inline constexpr int kClockCalendarCaption = 2131;
inline constexpr int kClockSeconds = 2132;     // check box: clock.showSeconds (+ caption 2133)
inline constexpr int kClockSecondsCaption = 2133;
inline constexpr int kClockClickLabel = 2134;
inline constexpr int kClockClickCombo = 2135;  // clock.click: 0 calendar, 1 launch, 2 none
inline constexpr int kClockTargetLabel = 2136;
inline constexpr int kClockTargetEdit = 2137;  // clock.clickTarget; applied on Enter / focus loss
inline constexpr int kClockTargetBrowse = 2138;  // "Browse…" (open dialog for an app)
inline constexpr int kClockTargetNote = 2139;
// Position (round 4, phase 2): docks[0].edge / align / offset.
inline constexpr int kSectionPosition = 2140;
inline constexpr int kEdgeLabel = 2141;
inline constexpr int kEdgeCombo = 2142;        // 0 bottom, 1 top, 2 left, 3 right
inline constexpr int kAlignLabel = 2143;
inline constexpr int kAlignCombo = 2144;       // 0 center, 1 start, 2 end
inline constexpr int kOffsetLabel = 2145;
inline constexpr int kOffsetEdit = 2146;       // DIPs, -16000 - 16000
inline constexpr int kOffsetSpin = 2147;
inline constexpr int kPositionNote = 2148;
// Round 4 phase 2b: full width, reserve space, peek strip, title.
inline constexpr int kFullWidth = 2149;        // check box (+ caption 2150)
inline constexpr int kFullWidthCaption = 2150;
inline constexpr int kReserveSpace = 2151;     // check box (+ caption 2152); with full width and auto-hide off
inline constexpr int kReserveSpaceCaption = 2152;
inline constexpr int kPeekLabel = 2153;
inline constexpr int kPeekEdit = 2154;         // docks[].peekPx, 0 - 64 (0: off)
inline constexpr int kPeekSpin = 2155;
inline constexpr int kPeekOpacityLabel = 2156;
inline constexpr int kPeekOpacityEdit = 2157;  // docks[].peekOpacity x 100, 5 - 100
inline constexpr int kPeekOpacitySpin = 2158;
inline constexpr int kTitleLabel = 2159;
inline constexpr int kTitleEdit = 2160;        // docks[].title; applied on Enter / focus loss
inline constexpr int kPositionNote2 = 2161;
// Round 4 phase 3.
inline constexpr int kRunningMonitorOnly = 2162;        // check box: docks[].runningFilter "monitor" (+ caption 2163)
inline constexpr int kRunningMonitorOnlyCaption = 2163;
// Round 4 phase 3b: docks[].layer.
inline constexpr int kLayerLabel = 2164;
inline constexpr int kLayerCombo = 2165;  // 0 normal (above windows), 1 desktop only

// Behaviour (2200 - 2299)
inline constexpr int kSectionAutoHide = 2200;
inline constexpr int kAutoHideOff = 2201;          // radio (+ caption 2202)
inline constexpr int kAutoHideOffCaption = 2202;
inline constexpr int kAutoHideAlways = 2203;
inline constexpr int kAutoHideAlwaysCaption = 2204;
inline constexpr int kAutoHideIntelligent = 2205;
inline constexpr int kAutoHideIntelligentCaption = 2206;
inline constexpr int kHideDelayLabel = 2207;
inline constexpr int kHideDelayEdit = 2208;        // ms, 0 - 10000
inline constexpr int kHideDelaySpin = 2209;
inline constexpr int kRevealDelayLabel = 2210;
inline constexpr int kRevealDelayEdit = 2211;
inline constexpr int kRevealDelaySpin = 2212;
inline constexpr int kSectionPreviews = 2213;
inline constexpr int kPreviews = 2214;
inline constexpr int kPreviewsCaption = 2215;
inline constexpr int kPreviewDelayLabel = 2216;
inline constexpr int kPreviewDelayEdit = 2217;
inline constexpr int kPreviewDelaySpin = 2218;
inline constexpr int kHideDelayUnit = 2219;
inline constexpr int kRevealDelayUnit = 2220;
inline constexpr int kPreviewDelayUnit = 2221;
inline constexpr int kAutoHideNote = 2222;
inline constexpr int kPreviewSingle = 2223;        // check box: previewsSingleWindow (+ caption 2224)
inline constexpr int kPreviewSingleCaption = 2224;
inline constexpr int kSectionMouse = 2225;
inline constexpr int kMiddleClickLabel = 2226;
inline constexpr int kMiddleClickCombo = 2227;     // middleClick: 0 newInstance, 1 minimize, 2 close, 3 none
inline constexpr int kWheelLabel = 2228;
inline constexpr int kWheelCombo = 2229;           // wheel: 0 none, 1 cycle
// Round 4 phase 6: task list options.
inline constexpr int kSectionTaskList = 2230;
inline constexpr int kWindowsLabel = 2231;
inline constexpr int kWindowsCombo = 2232;         // docks[].windows: 0 stacked, 1 unstacked
inline constexpr int kTitlesLabel = 2233;
inline constexpr int kTitlesCombo = 2234;          // docks[].titles: 0 off, 1 one line, 2 two lines
inline constexpr int kTitlesVerticalLabel = 2235;
inline constexpr int kTitlesVerticalCombo = 2236;  // docks[].titlesVertical: 0 off, 1 beside
inline constexpr int kTaskListNote = 2237;
inline constexpr int kThumbnailsLabel = 2238;
inline constexpr int kThumbnailsCombo = 2239;      // docks[].thumbnails: 0 off, 1 live, 2 snapshots
inline constexpr int kThumbnailsNote = 2240;       // the trade-off between live and snapshots
inline constexpr int kThumbIntervalLabel = 2241;
inline constexpr int kThumbIntervalEdit = 2242;    // thumbnailIntervalS, 1 - 3600
inline constexpr int kThumbIntervalSpin = 2243;
inline constexpr int kThumbIntervalUnit = 2244;
inline constexpr int kThumbFpsLabel = 2245;
inline constexpr int kThumbFpsEdit = 2246;         // thumbnailFps, 1 - 60
inline constexpr int kThumbFpsSpin = 2247;
inline constexpr int kThumbFpsUnit = 2248;
inline constexpr int kThumbSizeLabel = 2249;
inline constexpr int kThumbSizeEdit = 2250;        // thumbnailSize DIPs, 0 (twice the icon) or 16 - 512
inline constexpr int kThumbSizeSpin = 2251;
inline constexpr int kThumbSizeUnit = 2252;

// About (2300 - 2399)
inline constexpr int kAboutName = 2300;
inline constexpr int kAboutVersion = 2301;
inline constexpr int kSectionConfig = 2302;
inline constexpr int kConfigPathLabel = 2303;
inline constexpr int kConfigPathLink = 2304;   // SysLink: opens the folder with the file selected
inline constexpr int kOpenConfig = 2305;       // = tray "Open config file"
inline constexpr int kReloadConfig = 2306;     // = tray "Reload config"
inline constexpr int kAboutRendererLabel = 2307;
inline constexpr int kAboutRenderer = 2308;
inline constexpr int kAboutNote = 2309;

// Frame: the dock preview strip under every page (WinDock.SettingsPreview, child of the window).
inline constexpr int kPreview = 101;

// Appearance (2400 - 2499; the theme field rows: 3000 - 3399, see FieldRow)
inline constexpr int kSectionTheme = 2400;
inline constexpr int kThemeLabel = 2401;
inline constexpr int kThemeCombo = 2402;          // built-ins, then user theme files by name
inline constexpr int kExportTheme = 2403;         // "Export current theme…" (save dialog)
inline constexpr int kThemesFolderLink = 2404;    // SysLink: opens the themes folder
inline constexpr int kResetAllTheme = 2405;       // clears every override (config theme = preset)
inline constexpr int kThemeNote = 2406;
// Sections of the field rows (one per group).
inline constexpr int kSectionFieldsBar = 2410;
inline constexpr int kSectionFieldsLabel = 2411;
inline constexpr int kSectionFieldsDots = 2412;
inline constexpr int kSectionFieldsClock = 2413;
inline constexpr int kSectionFieldsIcons = 2414;
inline constexpr int kSectionFieldsPreviews = 2415;
inline constexpr int kSectionFieldsOther = 2416;
inline constexpr int kSectionFieldsEffects = 2417;  // "Icon effects": glow, reflection, perspective
inline constexpr int kEffectsNote = 2418;
// Round 4 phase 3: the edited dock's own theme preset (docks[].themePreset).
inline constexpr int kDockThemeLabel = 2419;
inline constexpr int kDockThemeCombo = 2420;     // 0 "Same as all docks" (""), then the presets / theme files of kThemeCombo

// One row per Theme::Field (Theme::Fields() order): id = FieldRow(field index, part).
inline constexpr int kFieldRowBase = 3000;
inline constexpr int kFieldRowStride = 10;
inline constexpr int kFieldLabel = 0;    // row label (static; Color / Float / Enum rows)
inline constexpr int kFieldSwatch = 1;   // Color: owner-drawn button (ChooseColor); Bool: the check box
inline constexpr int kFieldHex = 2;      // Color: "#AARRGGBB" edit (applied on Enter / focus loss); Bool: caption
inline constexpr int kFieldValue = 3;    // Color: alpha edit 0 - 255; Float: value edit (x FieldInfo::scale); Enum: combo
inline constexpr int kFieldSpin = 4;     // up-down of kFieldValue
inline constexpr int kFieldReset = 5;    // "Reset" button, shown only while the field is overridden
inline constexpr int kFieldUnit = 6;     // Float: the unit note (FieldInfo::unit: "DIPs", "%", ...)
constexpr int FieldRow(int field, int part) { return kFieldRowBase + field * kFieldRowStride + part; }
// 40 rows fit 3000 - 3399.

// Animation (2500 - 2599)
inline constexpr int kSectionSpeed = 2500;
inline constexpr int kSpeedLabel = 2501;
inline constexpr int kSpeedTrack = 2502;          // 0 = off, 1 - 33 = 0.25x - 4x (log2, 1/8 octave steps)
inline constexpr int kSpeedValue = 2503;          // "1.00×" / "Off"
inline constexpr int kSectionHide = 2504;
inline constexpr int kHideSlide = 2505;           // radio (+ caption id + 1)
inline constexpr int kHideSlideCaption = 2506;
inline constexpr int kHideSlideOnly = 2507;
inline constexpr int kHideSlideOnlyCaption = 2508;
inline constexpr int kHideFade = 2509;
inline constexpr int kHideFadeCaption = 2510;
inline constexpr int kHideNone = 2511;
inline constexpr int kHideNoneCaption = 2512;
inline constexpr int kSectionLaunch = 2513;
inline constexpr int kLaunchBounce = 2514;
inline constexpr int kLaunchBounceCaption = 2515;
inline constexpr int kLaunchHop = 2516;
inline constexpr int kLaunchHopCaption = 2517;
inline constexpr int kLaunchPulse = 2518;
inline constexpr int kLaunchPulseCaption = 2519;
inline constexpr int kLaunchNone = 2520;
inline constexpr int kLaunchNoneCaption = 2521;
inline constexpr int kSectionMotion = 2522;
inline constexpr int kFalloffLabel = 2523;
inline constexpr int kFalloffCombo = 2524;        // 0 cosine, 1 linear
inline constexpr int kEasingLabel = 2525;
inline constexpr int kEasingCombo = 2526;         // 0 easeOut, 1 linear, 2 spring
inline constexpr int kLabelFadeLabel = 2527;
inline constexpr int kLabelFadeEdit = 2528;       // ms, 0 - 1000
inline constexpr int kLabelFadeSpin = 2529;
inline constexpr int kLabelFadeUnit = 2530;
inline constexpr int kSectionAccessibility = 2531;
inline constexpr int kReduceMotionLabel = 2532;
inline constexpr int kReduceMotionCombo = 2533;   // 0 system, 1 on, 2 off
inline constexpr int kReduceMotionNote = 2534;    // "Windows animation effects: on / off"
inline constexpr int kSpeedNote = 2535;

// Dock contents (2600 - 2699; "Pinned apps" until 1c, the ids are kept)
inline constexpr int kPinnedList = 2600;          // SysListView32 (report): Name, Kind, Details; one row per docks[0].items[] entry
inline constexpr int kPinAdd = 2601;              // "Add app…" (open dialog, .lnk / .exe, multi-select)
inline constexpr int kPinRemove = 2602;
inline constexpr int kPinUp = 2603;
inline constexpr int kPinDown = 2604;
inline constexpr int kPinnedNote = 2605;
inline constexpr int kPinAddFolder = 2606;        // "Add folder…" (folder picker)
inline constexpr int kPinAddSeparator = 2607;     // appends { "kind": "separator" }
inline constexpr int kPinAddRecycleBin = 2608;    // appends { "kind": "recycleBin" }; disabled while one is pinned
inline constexpr int kPinChooseIcon = 2609;       // "Choose icon…" (the Change Icon dialog); disabled for separators
inline constexpr int kPinChooseImage = 2610;      // "Choose image…" (open dialog: images, .ico / .exe / .dll)
inline constexpr int kPinResetIcon = 2611;        // "Reset icon": clears `icon`; enabled when the entry has one
inline constexpr int kPinAddWidget = 2612;        // "Add widget" (menu: Clock, Weather, Running apps)
inline constexpr int kPinSettings = 2613;         // "Settings…" of the selected item (see the page)
inline constexpr int kLockDock = 2614;            // check box: docks[0].locked (+ caption 2615)
inline constexpr int kLockDockCaption = 2615;

// Weather (2700 - 2799)
inline constexpr int kSectionWeather = 2700;
inline constexpr int kWeatherShow = 2701;          // check box (+ caption 2702)
inline constexpr int kWeatherShowCaption = 2702;
inline constexpr int kSectionWeatherPlace = 2703;
inline constexpr int kWeatherSearchLabel = 2704;
inline constexpr int kWeatherSearchEdit = 2705;    // place name; Enter searches
inline constexpr int kWeatherSearch = 2706;        // "Search" (Open-Meteo geocoding, 10 results)
inline constexpr int kWeatherResultsLabel = 2707;
inline constexpr int kWeatherResultsCombo = 2708;  // "Name, Admin1, Country"; choosing one sets the place
inline constexpr int kWeatherSearchNote = 2709;    // "Searching…", "10 places found", "No places found", errors
inline constexpr int kWeatherLocationLabel = 2710;
inline constexpr int kWeatherLocationEdit = 2711;  // display name; applied on Enter / focus loss
inline constexpr int kWeatherLatLabel = 2712;
inline constexpr int kWeatherLatEdit = 2713;       // -90 - 90; applied on Enter / focus loss
inline constexpr int kWeatherLonLabel = 2714;
inline constexpr int kWeatherLonEdit = 2715;       // -180 - 180
inline constexpr int kSectionWeatherUpdates = 2716;
inline constexpr int kWeatherUnitsLabel = 2717;
inline constexpr int kWeatherUnitsCombo = 2718;    // 0 metric (°C, km/h), 1 imperial (°F, mph)
inline constexpr int kWeatherRefreshLabel = 2719;
inline constexpr int kWeatherRefreshEdit = 2720;   // minutes, 5 - 360
inline constexpr int kWeatherRefreshSpin = 2721;
inline constexpr int kWeatherRefreshUnit = 2722;
inline constexpr int kWeatherRefreshNow = 2723;    // "Refresh now" (= DockCommand 111)
inline constexpr int kWeatherStatusLabel = 2724;
inline constexpr int kWeatherStatus = 2725;        // "Updated 10:15", "(stale)", "Not configured", "Error: …"
inline constexpr int kWeatherCredit = 2726;        // SysLink: Open-Meteo (CC BY 4.0)

// Docks (2800 - 2899; round 4 phase 3)
inline constexpr int kDocksList = 2800;       // SysListView32 (report): Id, Edge, Display, Windows; one row per docks[]; the selected row is the dock the other pages edit
inline constexpr int kDockAdd = 2801;         // "Add dock" (bottom, primary display, running apps + clock)
inline constexpr int kDockDuplicate = 2802;   // copies the selected dock (new id)
inline constexpr int kDockRemove = 2803;      // removes the selected dock (disabled for the last one)
inline constexpr int kDocksNote = 2804;
inline constexpr int kDockEditing = 2805;     // "The other pages edit: <id>"
inline constexpr int kLockAllDocks = 2806;    // "Lock all docks" (= DockCommand 124)
inline constexpr int kUnlockAllDocks = 2807;  // "Unlock all docks" (= DockCommand 125)

// Layout (2900 - 2999; round 4 phase 3b)
inline constexpr int kLayoutCanvas = 2900;    // WinDock.LayoutCanvas: the monitors and docks; its window text lists the docks' canvas rects (automation)
inline constexpr int kLayoutNote = 2901;
inline constexpr int kLayoutSelected = 2902;  // the selected dock and where a drop would put it

// Widgets (3500 - 3599; round 4 phase 4b)
inline constexpr int kWidgetsList = 3500;      // SysListView32 (report): Name, Id, Source, Version; one row per installed widget
inline constexpr int kWidgetsAddFolder = 3501; // "Add from folder…" (copies a folder with a widget.json into the widgets folder)
inline constexpr int kWidgetsExport = 3502;    // "Export built-in to folder" (the selected built-in -> widgets\<id>)
inline constexpr int kWidgetsReload = 3503;    // "Reload widgets" (= DockCommand 126)
inline constexpr int kWidgetsOpenFolder = 3504;  // "Open widgets folder" (created if missing)
inline constexpr int kWidgetsConsole = 3505;   // "Console…" (= DockCommand 127)
inline constexpr int kWidgetsAddToDock = 3506; // "Add to dock" (the selected widget, on the dock the pages edit)
inline constexpr int kWidgetsNote = 3507;
inline constexpr int kWidgetsStatus = 3508;    // the last action's result ("Exported to …", errors)

// Widget settings (3600 - 3999; phase 4b). The fixed controls, then per schema field k
// (widget.json "settings" order) 3700 + 10 * k + part: 0 label, 1 the control (check box, edit,
// drop-down), 2 its caption (check box) or up-down (int), 3 the field's note.
inline constexpr int kWidgetPickLabel = 3600;
inline constexpr int kWidgetPick = 3601;       // drop-down: the dock's widget items ("Name (item n)")
inline constexpr int kWidgetSettingsNote = 3602;
inline constexpr int kWidgetReset = 3603;      // "Reset to defaults" (removes the item's settings)
inline constexpr int kWidgetFieldBase = 3700;
inline constexpr int kWidgetFieldMax = 30;     // fields shown per widget

}  // namespace SettingsIds
