#pragma once

// Stable control ids of the settings window (automation: FindWindowEx / GetDlgItem on the page
// windows, then WM_COMMAND / BM_CLICK / TBM_SETPOS ...). Never renumber; add new ids at the end
// of a page's range. Check boxes and radio buttons have no visible text of their own: the
// clickable caption next to them is a separate static (id + 1) that forwards clicks.
namespace SettingsIds {

// Frame (children of a WinDock.Settings window; round 5 phase B: three kinds of window, see
// SettingsKind).
inline constexpr int kNav = 100;        // Global: WinDock.SettingsNav; window text = selected page name
inline constexpr int kPageBase = 200;   // page windows: kPageBase + page index (see kPage* / kTab* / kItemPage)
inline constexpr int kHeader = 102;     // WinDock.SettingsBand: Global the subtitle line, Dock / Item the identity header (window text: its summary)
inline constexpr int kTabs = 103;       // Dock: WinDock.SettingsTabs, the tab strip; window text = selected tab name
inline constexpr int kFooter = 104;     // Item: WinDock.SettingsBand, the footer
// Children of the header / footer bands.
inline constexpr int kHeaderFlash = 110;    // Dock: "Flash" (round 8 phase 8B: was "Flash this dock")
inline constexpr int kHeaderLock = 111;     // Dock: "Lock" / "Unlock" (docks[].locked; was "Lock dock" / "Unlock dock")
inline constexpr int kHeaderGlobal = 112;   // Dock: the "⋯" menu's "App settings…" command (round 8 phase 8B; was a SysLink; round 10 10A: was "Global settings…")
inline constexpr int kHeaderDockLink = 113; // Item: SysLink "on <dock id>" (opens / activates the dock window)
inline constexpr int kFooterReset = 114;    // Item (widget / clock / weather): "Reset to defaults"
inline constexpr int kFooterConsole = 115;  // Item (widget): "Console…"
inline constexpr int kHeaderMore = 116;     // Dock: round 8 phase 8B: the "⋯" button (a menu: App settings… = kHeaderGlobal)

// Page indices of the Global window (round 5 phase B renumbered them). Round 8 phase 8D: the nav
// lists them in groups in another order (GENERAL: General, Docks and displays; LOOK: Themes,
// Animation; DATA: Calendar feeds, Notifications, Tray icons; WIDGETS: Widget library; About;
// QueryState 279 = the active page's row); the indexes below do not change.
inline constexpr int kPageGeneral = 0;        // startup, taskbar, renderer, config file
inline constexpr int kPageThemes = 1;         // the global theme (was Appearance)
inline constexpr int kPageAppearance = kPageThemes;
inline constexpr int kPageAnimation = 2;
inline constexpr int kPageCalendar = 3;       // round 4 phase 13: the ICS feeds
inline constexpr int kPageTray = 4;           // round 4 phase 8: the notify icons (tray.enabled, Put the tray in its own dock)
inline constexpr int kPageNotifications = 5;  // round 4 phase 9: balloons, clear on activate, polling, access, badges per app
inline constexpr int kPageWidgets = 6;        // "Widget library": installed widgets, Console, export
inline constexpr int kPageDocks = 7;          // "Docks and displays": the monitor canvas and the list of docks
inline constexpr int kPageLayout = kPageDocks;
inline constexpr int kPageAbout = 8;
inline constexpr int kPageCount = 9;
// Tab indices of a Dock window (a folder window entry's window has the one page kTabFolderWindow).
// Round 8 phase 8B: Appearance (the old Theme tab's combo and link plus Behaviour's Size card)
// is the third tab; the Theme tab (kTabTheme, 4) is retired and Behaviour / Running apps moved
// one place to the right (2 -> 3, 3 -> 4).
inline constexpr int kTabContents = 0;   // was the Dock contents page
inline constexpr int kTabPosition = 1;
inline constexpr int kTabAppearance = 2;  // round 8 phase 8B (the preview strip shows only here)
inline constexpr int kTabBehaviour = 3;
inline constexpr int kTabRunning = 4;
inline constexpr int kTabCount = 5;
inline constexpr int kTabFolderWindow = 0;
// Round 8 phase 8C: a standalone widget entry's window has two tabs: Widget (its widget's generated
// settings, item 0 of the entry) and Window (place, size, layer, look; until 8C its one page, 0).
inline constexpr int kTabWidgetSettings = 0;
inline constexpr int kTabWidgetWindow = 1;
// The one page of an Item window (the generated schema page or Folders and groups).
inline constexpr int kItemPage = 0;

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
// 2112 - 2118: the clock section (show, format, preview, date) until round 5 phase A; reserved.
inline constexpr int kStartWithWindows = 2119;
inline constexpr int kStartWithWindowsCaption = 2120;
inline constexpr int kHideTaskbar = 2121;
inline constexpr int kHideTaskbarCaption = 2122;
inline constexpr int kRendererLabel = 2123;
inline constexpr int kRendererCombo = 2124;   // 0 auto, 1 software
inline constexpr int kRendererNote = 2125;
inline constexpr int kSectionDock = 2126;
// 2127: the Clock section header until round 5; reserved.
inline constexpr int kSectionStartup = 2128;
inline constexpr int kSectionRenderer = 2129;
// 2130 - 2139: the clock's calendar check box (until 1c), seconds, click, target, Browse… (until
// round 5 phase A: each clock item's generated page); reserved.
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
// Round 4 phase 11a: docks[].overflow.
inline constexpr int kOverflowLabel = 2166;
inline constexpr int kOverflowCombo = 2167;  // 0 scroll, 1 shrink, 2 clip (OverflowMode order)
// Round 4 phase 11b: docks[].shape / radius / rotateIcons.
inline constexpr int kShapeLabel = 2168;
inline constexpr int kShapeCombo = 2169;     // 0 bar, 1 half circle (arc), 2 circle (DockShape order)
inline constexpr int kRadiusLabel = 2170;
inline constexpr int kRadiusEdit = 2171;     // docks[].radius DIPs, 0 = automatic, 60 - 2000
inline constexpr int kRadiusSpin = 2172;
inline constexpr int kRotateIcons = 2173;    // check box (+ caption 2174)
inline constexpr int kRotateIconsCaption = 2174;
// 2175 - 2178: the clock face drop-down, Open faces folder and its note (round 4 phase 14 until
// round 5 phase A: the clock item's generated page); reserved.
// Round 5 phase B: the Position tab's circle centre (docks[].x / y; empty: centred) and sections,
// the Global General page's config buttons.
inline constexpr int kCircleXLabel = 2179;
inline constexpr int kCircleXEdit = 2180;     // DIPs from the work area's left (empty: centred)
inline constexpr int kCircleYLabel = 2181;
inline constexpr int kCircleYEdit = 2182;     // DIPs from the work area's top (empty: centred)
inline constexpr int kCircleNote = 2183;
inline constexpr int kGeneralOpenConfig = 2184;    // = tray "Open config file"
inline constexpr int kGeneralReloadConfig = 2185;  // = tray "Reload config"
inline constexpr int kSectionDisplay = 2186;
inline constexpr int kSectionShape = 2187;
inline constexpr int kSectionConfigFile = 2188;
// Round 6 phase 6A: floating docks (Position tab; docks[].floating, x / y = kCircleXEdit /
// kCircleYEdit), the layer's note, and Global > General's Dashboard section.
inline constexpr int kFloating = 2189;          // check box: docks[].floating (+ caption 2190); bar docks only
inline constexpr int kFloatingCaption = 2190;
inline constexpr int kFloatingNote = 2191;
inline constexpr int kLayerNote = 2192;         // one line about the selected layer
inline constexpr int kSectionDashboard = 2193;
inline constexpr int kDashboardHotkeyLabel = 2194;
inline constexpr int kDashboardHotkeyEdit = 2195;  // the key-capture box: dashboard.hotkey (canonical text; "" = none)
inline constexpr int kDashboardNote = 2196;
inline constexpr int kDashboardStatus = 2197;      // "Registered" / "Not registered: ..." / "No hotkey"
inline constexpr int kDashboardShow = 2198;        // "Show the dashboard now" (= DockCommand 146)

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
// Round 5 phase D: the Dock window's Behaviour tab, "Notifications" (moved from Global >
// Notifications: they are this dock's).
inline constexpr int kSectionDockNotifications = 2253;
inline constexpr int kBadges = 2254;               // check box: docks[].badges (+ caption 2255; was 4200)
inline constexpr int kBadgesCaption = 2255;
// 2256 / 2257 ("Show the notification centre on this dock", was 4202) are retired since round 5
// phase E: the centre is a widget of the Contents tab's drawer, nothing else adds it.
inline constexpr int kDockNotificationsNote = 2258;
// Round 7 phase 7A: the Behaviour tab's "Popups" line (docks[].popupGap).
inline constexpr int kSectionPopups = 2259;
inline constexpr int kPopupGapLabel = 2260;
inline constexpr int kPopupGapEdit = 2261;         // docks[].popupGap DIPs, 0 - 64
inline constexpr int kPopupGapSpin = 2262;
inline constexpr int kPopupGapUnit = 2263;
// Round 7 phase 7A: the Position tab's "Maximum length" (docks[].maxLength; the General range
// 2100 - 2199 the Position tab grew from is full, so it continues here).
inline constexpr int kMaxLengthLabel = 2264;
inline constexpr int kMaxLengthEdit = 2265;        // DIPs, 0 (no limit) or 96 - 8000
inline constexpr int kMaxLengthSpin = 2266;
inline constexpr int kMaxLengthNote = 2267;        // "0: no limit. Longer runs scroll (see Overflow)"
// Round 7 phase 7B: the Position tab's note under Alignment.
inline constexpr int kAlignNote = 2268;            // "Where the dock sits on its edge; with full width or anchored items, …"
// Round 8 phase 8A: Behaviour > Auto-hide, the expander over Hide delay / Reveal delay
// (QueryState 276 with this id: 1 while it is open).
inline constexpr int kAutoHideMore = 2269;
// Round 8 phase 8B: the Position tab's placement kind (it sets shape + floating; the shape combo
// 2169 and the Floating check box 2189 stay as hidden controls for automation), the "More"
// expander, the dock silhouette (moved from the header) and the Layer card.
inline constexpr int kPlacementKindLabel = 2270;
inline constexpr int kPlacementKindCombo = 2271;   // 0 bar on an edge, 1 floating bar, 2 half circle, 3 circle (QueryState 277)
inline constexpr int kPositionMore = 2272;         // expander: reserve screen space, peek, maximum length, title (bars only)
inline constexpr int kPositionSilhouette = 2273;   // WinDock.SettingsSilhouette: the display with the dock on it (window text: "Where it sits: …")
inline constexpr int kSectionLayer = 2274;
// Round 9 phase 9A. The General range 2100 - 2199 has only retired ids left (and 2199), so the
// new rows of Global > General and of the Appearance tab's Size card continue here too.
inline constexpr int kSectionFullscreen = 2275;      // Global > General: the "Full-screen apps" card
inline constexpr int kFullscreenHide = 2276;         // check box: settings.fullscreen "hide" (+ caption 2277, hint 2278)
inline constexpr int kFullscreenHideCaption = 2277;
inline constexpr int kFullscreenHint = 2278;
inline constexpr int kMagnifyBarLabel = 2279;        // Dock > Appearance > Size: "Bar while magnified"
inline constexpr int kMagnifyBarCombo = 2280;        // docks[].magnifyBar: 0 steady ("Grows once and stays"), 1 follow
inline constexpr int kAutoHideFloatingNote = 2281;   // Behaviour > Auto-hide for a floating bar: why there is none
inline constexpr int kAutoHideFloatingLink = 2282;   // SysLink "Position tab": switches to the Position tab

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
// Round 8 phase 8D: 2403 - 2405 are the "⋯" menu's commands (kThemeMore; the buttons and the link
// are gone; WM_COMMAND with lParam 0 to the page runs them).
inline constexpr int kExportTheme = 2403;         // "Export current theme…" (save dialog)
inline constexpr int kThemesFolderLink = 2404;    // "Open themes folder" (was a SysLink)
inline constexpr int kResetAllTheme = 2405;       // "Reset all to preset": clears every override (config theme = preset; greyed with none)
inline constexpr int kThemeNote = 2406;           // the Theme label's hint: "3 values changed from the preset" / "The preset's values"
// Sections of the field rows (one per group). Round 8 phase 8D: each is the heading expander of
// its group's card ("Bar · 2 changed"; QueryState 276; Bar open at first, the others closed).
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
inline constexpr int kSectionFieldsBadges = 2421; // round 4 phase 9: "Notification badges" (badgeFill, badgeText, badgeSize)
// Round 5 phase B: the Dock window's Theme tab (kDockThemeCombo, then these).
inline constexpr int kDockThemeNote = 2422;
inline constexpr int kDockThemeLink = 2423;      // SysLink: opens Global settings > Themes
inline constexpr int kThemeMore = 2424;          // round 8 phase 8D: Themes' "⋯" (a menu of 2403 / 2404 / 2405)

// One row per Theme::Field (Theme::Fields() order): id = FieldRow(field index, part).
inline constexpr int kFieldRowBase = 3000;
inline constexpr int kFieldRowStride = 10;
inline constexpr int kFieldLabel = 0;    // row label (static; Color / Float / Enum rows)
inline constexpr int kFieldSwatch = 1;   // Color: owner-drawn button (ChooseColor); Bool: the check box
inline constexpr int kFieldHex = 2;      // Color: "#AARRGGBB" edit (applied on Enter / focus loss); Bool: caption
inline constexpr int kFieldValue = 3;    // Color: alpha edit 0 - 255; Float: value edit (x FieldInfo::scale); Enum: combo
inline constexpr int kFieldSpin = 4;     // up-down of kFieldValue
inline constexpr int kFieldReset = 5;    // "↺" button (24 DIPs, tooltip "Reset to the preset's value"; was "Reset"), shown only while the field is overridden
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

// Dock contents: the Dock window's Contents tab (2600 - 2699; "Pinned apps" until 1c, "Dock
// contents" list until round 5 phase C: the item strip, the widget drawer and the inspector since).
// 2600: the list (SysListView32) until round 5 phase C; reserved (the strip is kContentsStrip).
inline constexpr int kPinAdd = 2601;              // "Add app…" (open dialog, .lnk / .exe, multi-select)
inline constexpr int kPinRemove = 2602;           // inspector: "Remove from dock" (the selected item)
inline constexpr int kPinUp = 2603;               // inspector: "◀ Move left" (was Move up)
inline constexpr int kPinDown = 2604;             // inspector: "Move right ▶" (was Move down)
// 2605: the list's note until round 5 phase C; reserved (kContentsNote).
inline constexpr int kPinAddFolder = 2606;        // "Add folder…" (folder picker)
inline constexpr int kPinAddSeparator = 2607;     // a separator after the selected item (none selected: at the end)
inline constexpr int kPinAddRecycleBin = 2608;    // a { "kind": "recycleBin" } after the last pin; disabled while one is pinned
inline constexpr int kPinChooseIcon = 2609;       // inspector: "Choose icon…" (the Change Icon dialog); shown for apps, folders, groups, the bin
inline constexpr int kPinChooseImage = 2610;      // inspector: "Choose image…" (open dialog: images, .ico / .exe / .dll)
inline constexpr int kPinResetIcon = 2611;        // inspector: "Reset icon": clears `icon`; enabled when the entry has one
// 2612: "Add widget ▾" until round 5 phase C (the widget drawer); reserved.
inline constexpr int kPinSettings = 2613;         // inspector: "Open settings in its own window" (shown for items with settings)
inline constexpr int kLockDock = 2614;            // check box: docks[].locked (+ caption 2615)
inline constexpr int kLockDockCaption = 2615;
// Round 5 phase C.
inline constexpr int kContentsStrip = 2616;       // WinDock.SettingsStrip: the item strip (tiles in dock order; window text = its accessible name)
inline constexpr int kContentsNote = 2617;        // "Drag to reorder. …"
inline constexpr int kContentsInfo = 2618;        // WinDock.SettingsStrip (info mode): the selected item's icon, name, kind · details
inline constexpr int kContentsDrawerToggle = 2619;  // "Hide / Show widget drawer" until round 8 phase 8B (the drawer is always shown); reserved
inline constexpr int kDrawerTitle = 2620;         // "Add to the dock" (round 8 phase 8B; was "Widget drawer")
inline constexpr int kDrawerHint = 2621;          // "drag onto the strip"
inline constexpr int kDrawerSearch = 2622;        // edit: filters the drawer by name / note / id
inline constexpr int kDrawerList = 2623;          // WinDock.SettingsStrip (drawer mode): Apps and folders (8B), Built in, Script widgets
inline constexpr int kDrawerLibraryLink = 2624;   // SysLink: opens Global > Widget library
// Round 7 phase 7B: the inspector's placement of the selected item (every kind).
inline constexpr int kItemAnchorLabel = 2625;     // "Position on the dock"
inline constexpr int kItemAnchorCombo = 2626;     // 0 follow the dock's alignment, 1 start, 2 centre, 3 end (= ItemAnchor)
inline constexpr int kItemOffsetLabel = 2627;     // "Offset (DIPs)"
inline constexpr int kItemOffsetEdit = 2628;      // items[].offset DIPs, -4000 - 4000
inline constexpr int kItemOffsetSpin = 2629;
inline constexpr int kItemOffsetNote = 2630;      // "Shifts this item and the ones after it in its group"
// Round 8 phase 8B: the inspector's "More ▾" (pinned items: a menu of Choose icon… 2609, Choose
// image… 2610, Reset icon 2611). The add buttons 2601 / 2606 / 2607 / 2608, the drawer toggle 2619
// and the icon buttons are gone; their ids stay page commands (WM_COMMAND with lParam 0 to the page).
inline constexpr int kPinMore = 2631;

// Weather (2700 - 2799): the Weather page until round 5 phase A (retired: each weather item's place
// is a Place field of the generated page, 3610 - 3629); reserved.

// Docks and displays (2800 - 2999; round 4 phase 3 Docks + 3b Layout, one page since round 5 phase B)
// Round 8 phase 8D: the four add buttons are the "Add ▾" menu's commands (2801 Dock, 2811 Floating
// dock, 2808 Folder window…, 2812 Standalone widget ▸ (a submenu of the installed widgets; as a
// command: that menu under Add ▾)); Lock all / Unlock all are one check box (2806, checked when
// every entry is locked; as commands 2806 locks and 2807 unlocks every entry). WM_COMMAND with
// lParam 0 to the page runs any of them.
inline constexpr int kDocksList = 2800;       // SysListView32 (report): Id (with the dock's colour dot), Edge, Display, Windows; one row per docks[]; the selected row is what this page's Duplicate / Remove / Open act on (no other page reads it)
inline constexpr int kDockAdd = 2801;         // "Add ▾" > "Dock" (bottom, primary display, running apps + clock)
inline constexpr int kDockDuplicate = 2802;   // "Duplicate": copies the selected dock (new id)
inline constexpr int kDockRemove = 2803;      // "Remove": removes the selected dock (disabled for the last one)
inline constexpr int kDocksNote = 2804;       // the one-line hint under the canvas (8D: the explanation is the canvas's tooltip)
// 2805: "The other pages edit: <id>" until round 5 phase B (the canvas's kLayoutSelected line says it); reserved.
inline constexpr int kLockAllDocks = 2806;    // check box "Lock all docks" (+ caption 2813; mixed: unchecked); as a command: lock every entry (= DockCommand 124)
inline constexpr int kUnlockAllDocks = 2807;  // a command (the button is gone): unlock every entry (= DockCommand 125)
inline constexpr int kDockAddFolder = 2808;   // "Add ▾" > "Folder window…" (= DockCommand 139: a folder picker; round 4 phase 12)
// 2809: "Folder window settings…" until round 5 phase B (kDocksOpen opens a folder window's own window); reserved.
inline constexpr int kDocksOpen = 2810;       // "Open settings" (the selected row's own window; = a double click)
inline constexpr int kDockAddFloating = 2811; // round 6 phase 6A: "Add ▾" > "Floating dock" (a floating bar, layer desktop, centred)
inline constexpr int kDockAddWidget = 2812;   // round 6 phase 6B: "Add ▾" > "Standalone widget ▸" (as a command: the catalog's widgets in a menu)
inline constexpr int kLockAllDocksCaption = 2813;  // round 8 phase 8D
inline constexpr int kDockAddMenu = 2814;     // round 8 phase 8D: "Add ▾" (a menu: Dock, Floating dock, Folder window…, Standalone widget ▸)

// The canvas (round 4 phase 3b)
inline constexpr int kLayoutCanvas = 2900;    // WinDock.LayoutCanvas: the monitors and docks; its window text lists the docks' canvas rects (automation); a click selects the row, a double click opens the dock's window
inline constexpr int kLayoutNote = 2901;
inline constexpr int kLayoutSelected = 2902;  // the selected dock and where a drop would put it

// Widgets (3500 - 3599; round 4 phase 4b)
// Round 8 phase 8D: 3501 - 3505 are the "More ▾" menu's commands (kWidgetsMore; the buttons are
// gone; WM_COMMAND with lParam 0 to the page runs them).
inline constexpr int kWidgetsList = 3500;      // SysListView32 (report): Name, Id, Source, Version; one row per installed widget
inline constexpr int kWidgetsAddFolder = 3501; // "Add from folder…" (copies a folder with a widget.json into the widgets folder)
inline constexpr int kWidgetsExport = 3502;    // "Export built-in to folder" (the selected built-in -> widgets\<id>; greyed otherwise)
inline constexpr int kWidgetsReload = 3503;    // "Reload widgets" (= DockCommand 126)
inline constexpr int kWidgetsOpenFolder = 3504;  // "Open widgets folder" (created if missing)
inline constexpr int kWidgetsConsole = 3505;   // "Console…" (= DockCommand 127)
inline constexpr int kWidgetsAddToDock = 3506; // "Add to dock" / "Add to dock ▾" (with several docks: a menu of them)
inline constexpr int kWidgetsNote = 3507;
inline constexpr int kWidgetsStatus = 3508;    // the last action's result ("Exported to …", errors)
inline constexpr int kWidgetsAddToDesktop = 3509;  // round 6 phase 6B: "Add to the desktop" (a standalone widget of the selected one)
inline constexpr int kWidgetsPreview = 3510;   // round 6 phase 6E: WinDock.WidgetPreviewPanel: the selected widget's live preview + description
inline constexpr int kWidgetsMore = 3511;      // round 8 phase 8D: "More ▾" (a menu of 3501 - 3505)

// Widget settings (3600 - 3999; phase 4b). The fixed controls, then per schema field k
// (widget.json "settings" order) 3700 + 10 * k + part: 0 label, 1 the control (check box, edit,
// drop-down), 2 its caption (check box) or up-down (int), 3 the field's hint (round 8 phase 8C: a
// one-line hint under the label / check box caption; was a note under the control), 4 the card
// (Section) the field starts, 5 the unit.
// 3600 / 3601: the item picker until round 5 phase B (the Item window is the item); reserved.
inline constexpr int kWidgetSettingsNote = 3602;  // 8C: only "has no settings" / "Not installed"; a standalone widget's Widget tab: the description (one line)
inline constexpr int kWidgetReset = 3603;      // "Reset to defaults" (since phase B the footer's kFooterReset; the page handles this id)
// Round 5 phase A: the controls of a Place field (the first one of the schema: the weather item's
// place, the script weather widget's own) and the clock's extra button.
// Round 8 phase 8C: the search edit 3611, its Search button 3612 and the Results label 3613 are
// gone (retired, reserved): 3614 is the search box itself, an autocomplete.
inline constexpr int kPlaceSearchLabel = 3610;
inline constexpr int kPlaceResults = 3614;        // editable drop-down (CBS_DROPDOWN): type a place's name; Enter / focus loss / a 400 ms pause searches (Open-Meteo geocoding); its list "Name, Region, Country"; choosing one sets the place
inline constexpr int kPlaceSearchNote = 3615;     // the Find a place label's hint: "Searching…", "10 places found: choose one", errors
inline constexpr int kPlaceLocationLabel = 3616;
inline constexpr int kPlaceLocationEdit = 3617;   // the name shown in the popup (`location`)
inline constexpr int kPlaceLatLabel = 3618;
inline constexpr int kPlaceLatEdit = 3619;        // -90 - 90 (`latitude`, 4 decimals)
inline constexpr int kPlaceLonLabel = 3620;
inline constexpr int kPlaceLonEdit = 3621;        // -180 - 180 (`longitude`)
inline constexpr int kPlaceStatusLabel = 3622;
inline constexpr int kPlaceStatus = 3623;         // the place's service: "Updated 10:15", "Not shown on a dock", "Error: …"
inline constexpr int kPlaceRefreshNow = 3624;     // "Refresh now" (that place)
inline constexpr int kPlaceCredit = 3625;         // SysLink: Open-Meteo (CC BY 4.0)
inline constexpr int kPlaceNote = 3626;           // a script widget: the Place label's hint, whose place it shows (8C: was a note)
inline constexpr int kClockFacesFolder = 3627;    // a clock item: "Open faces folder"
// Round 8 phase 8C:
inline constexpr int kPlaceSummaryLabel = 3628;
inline constexpr int kPlaceSummary = 3629;        // the place shown: "Colorado Springs · 38.83, −104.82" (U+2212 for a negative), "None yet: …"
inline constexpr int kPlaceCoordsExpander = 3630; // "Edit coordinates": the name shown 3616 / 3617, latitude 3618 / 3619, longitude 3620 / 3621 (QueryState 276)
inline constexpr int kPlaceLatUnit = 3631;        // "-90 to 90"
inline constexpr int kPlaceLonUnit = 3632;        // "-180 to 180"
inline constexpr int kWidgetFieldBase = 3700;
inline constexpr int kWidgetFieldMax = 30;     // fields shown per widget

// Folders and groups (4000 - 4099; round 4 phase 7): the Item window's folder / group item.
// 4000 / 4001: the item picker until round 5 phase B; reserved.
inline constexpr int kItemViewLabel = 4002;
inline constexpr int kItemViewCombo = 4003;    // 0 automatic, 1 grid, 2 list (the item's `view`)
inline constexpr int kItemViewNote = 4004;
inline constexpr int kItemChooseIcon = 4005;   // "Choose icon…" (the Change Icon dialog)
inline constexpr int kItemChooseImage = 4006;  // "Choose image…"
inline constexpr int kItemResetIcon = 4007;    // "Reset icon" (enabled when the item has an `icon`)
inline constexpr int kItemNote = 4008;         // the folder's path / the group's member count / "none" note
// Groups only (hidden for a folder):
inline constexpr int kGroupNameLabel = 4009;
inline constexpr int kGroupNameEdit = 4010;    // the group's name (committed on Enter / focus loss; "Group" = default)
inline constexpr int kGroupMembersLabel = 4011;
inline constexpr int kGroupMembers = 4012;     // SysListView32 (report): Name, Target; one row per member
inline constexpr int kGroupAdd = 4013;         // "Add app…" (open dialog, .lnk / .exe, multi-select)
inline constexpr int kGroupAddFolder = 4014;   // "Add folder…"
inline constexpr int kGroupRemove = 4015;
inline constexpr int kGroupUp = 4016;          // "Move up"
inline constexpr int kGroupDown = 4017;        // "Move down"
// Round 8 phase 8C: the page's cards.
inline constexpr int kItemPopupSection = 4018; // "Folder" / "Group": (the group's name,) Show the contents as, the path / member note
inline constexpr int kItemIconSection = 4019;  // "Icon": Choose icon…, Choose image…, Reset icon

// Tray (4100 - 4199; round 4 phase 8): tray.enabled and the tray item. Since round 5 phase D the
// Global Tray icons page has 4100 - 4102, 4114 / 4115, 4125 / 4126 only; a tray item's own Item
// window (its page 0) has 4103 - 4113, 4118 - 4121, 4123 / 4124 (the same ids as before). 4116 /
// 4117 (Add a tray to this dock) and 4122 (which dock) are retired.
inline constexpr int kTrayEnabled = 4100;       // check box: tray.enabled (DockCommand 129 / 130)
inline constexpr int kTrayEnabledCaption = 4101;
inline constexpr int kTrayEnabledNote = 4102;
inline constexpr int kTrayRowsLabel = 4103;
inline constexpr int kTrayRowsEdit = 4104;      // 1 - 2 (the item's rows)
inline constexpr int kTrayRowsSpin = 4105;
inline constexpr int kTrayColumnsLabel = 4106;
inline constexpr int kTrayColumnsEdit = 4107;   // 1 - 20 (columns)
inline constexpr int kTrayColumnsSpin = 4108;
inline constexpr int kTrayIconSizeLabel = 4109;
inline constexpr int kTrayIconSizeCombo = 4110; // 0 = 16, 1 = 20, 2 = 24 px (iconSize)
inline constexpr int kTrayIconsLabel = 4111;
inline constexpr int kTrayIconList = 4112;      // SysListView32 (report, check boxes): Icon, App, Key; checked = shown in the grid (unchecked: in `hidden`)
inline constexpr int kTrayReset = 4113;         // "Reset to Explorer" (= DockCommand 130)
inline constexpr int kTrayOwnDock = 4114;       // 8b: "Put the tray in its own dock" (Global: the first dock's tray item that is not alone on its dock)
inline constexpr int kTrayOwnDockNote = 4115;
// 4116 / 4117: "Add a tray to this dock" and its line until round 5 phase D (the widget drawer adds one); reserved.
inline constexpr int kTrayGridSection = 4118;
inline constexpr int kTrayGridNote = 4119;
inline constexpr int kTrayIconsNote = 4120;
inline constexpr int kTrayResetNote = 4121;
// 4122: the "which dock" line of round 5 phase B; reserved.
inline constexpr int kTrayIconUp = 4123;        // a tray item's window: "Move up" (the selected icon earlier in `order`)
inline constexpr int kTrayIconDown = 4124;      // "Move down"
inline constexpr int kTrayGlobalNote = 4125;    // Global: where each tray item's grid settings are
inline constexpr int kTrayOwnDockStatus = 4126; // Global: the last Put the tray in its own dock result
inline constexpr int kTrayTakeoverSection = 4127;  // round 8 phase 8C: a tray item's "Takeover" card (Reset to Explorer)
inline constexpr int kTrayResetLabel = 4128;       // ... its label "Explorer's icons" (4121 its hint)
inline constexpr int kTraySection = 4129;          // round 8 phase 8D: Global > Tray icons' "Notification area" card

// Notifications (4200 - 4299; round 4 phase 9): config.notifications. 4200 - 4203 (the edited
// dock's badges / centre) moved to the Dock window's Behaviour tab in round 5 phase D (2254 /
// 2255; the centre check box 2256 / 2257 went in phase E: the centre is a drawer widget); 4223
// (which dock) is retired; 4204 stays: the note on the centre.
inline constexpr int kNotifCentreNote = 4204;
inline constexpr int kNotifBalloons = 4205;        // check box: notifications.balloons (+ caption 4206)
inline constexpr int kNotifBalloonsCaption = 4206;
inline constexpr int kNotifBalloonsNote = 4207;
inline constexpr int kNotifClearOnActivate = 4208; // check box: notifications.clearOnActivate (+ caption 4209)
inline constexpr int kNotifClearOnActivateCaption = 4209;
inline constexpr int kNotifPollLabel = 4210;
inline constexpr int kNotifPollEdit = 4211;        // notifications.pollS, 1 - 60
inline constexpr int kNotifPollSpin = 4212;
inline constexpr int kNotifPollUnit = 4213;
inline constexpr int kNotifPollNote = 4214;
inline constexpr int kNotifStatusSection = 4215;
inline constexpr int kNotifStatusLabel = 4216;
inline constexpr int kNotifStatus = 4217;          // "Reading notifications (n listed, m unread)", the denied reason, "Off: ..."
inline constexpr int kNotifOpenSettings = 4218;    // "Open Windows settings" (ms-settings:privacy-notifications)
inline constexpr int kNotifDndNote = 4219;
inline constexpr int kNotifAppsSection = 4220;
inline constexpr int kNotifAppList = 4221;         // SysListView32 (report, check boxes): App, Unread, Key; unchecked = in notifications.excluded
inline constexpr int kNotifAppsNote = 4222;
inline constexpr int kNotifCentreSection = 4224;   // round 8 phase 8D: the "Centre" card (4223 is retired)

// Calendar (4300 - 4399; round 4 phase 13): config.calendars.
inline constexpr int kCalFeedList = 4300;       // SysListView32 (report, check boxes): Name, URL, Every, Status; checked = enabled
inline constexpr int kCalNameLabel = 4301;
inline constexpr int kCalNameEdit = 4302;       // the feed being added / edited
inline constexpr int kCalUrlLabel = 4303;
inline constexpr int kCalUrlEdit = 4304;        // http(s) / webcal URL, a file path or file:/// URL
inline constexpr int kCalColorLabel = 4305;
inline constexpr int kCalColorEdit = 4306;      // "#AARRGGBB"
// 4307: reserved (a colour swatch)
inline constexpr int kCalRefreshLabel = 4308;
inline constexpr int kCalRefreshEdit = 4309;    // minutes, 5 - 1440
inline constexpr int kCalRefreshSpin = 4310;
inline constexpr int kCalRefreshUnit = 4311;
inline constexpr int kCalAdd = 4312;            // "Add feed" (from the fields)
inline constexpr int kCalSave = 4313;           // "Save changes" (the fields -> the selected feed)
inline constexpr int kCalRemove = 4314;         // removes the selected feed
inline constexpr int kCalRefreshNow = 4315;     // "Refresh now" (= DockCommand 135)
inline constexpr int kCalShowDots = 4316;       // check box: calendars.showDots (+ caption 4317)
inline constexpr int kCalShowDotsCaption = 4317;
inline constexpr int kCalNote = 4318;
inline constexpr int kCalFieldsNote = 4319;     // what the fields accept; the last add / save error
inline constexpr int kCalSection = 4320;        // "Feeds"
inline constexpr int kCalEditSection = 4321;    // "Add or edit a feed" (round 8 phase 8D: the heading expander of the form's card; QueryState 276)
inline constexpr int kCalOptionsSection = 4322; // "Display"
inline constexpr int kCalEnable = 4323;         // check box: the selected feed enabled (+ caption 4324)
inline constexpr int kCalEnableCaption = 4324;
// 13b: reminders.
inline constexpr int kCalReminderLabel = 4325;
inline constexpr int kCalReminderEdit = 4326;   // the feed's reminderMin (0: the default below), 0 - 1440
inline constexpr int kCalReminderSpin = 4327;
inline constexpr int kCalReminderUnit = 4328;
inline constexpr int kCalDefaultReminderLabel = 4329;
inline constexpr int kCalDefaultReminderEdit = 4330;  // calendars.defaultReminderMin (0: off), 0 - 1440
inline constexpr int kCalDefaultReminderSpin = 4331;
inline constexpr int kCalDefaultReminderUnit = 4332;
inline constexpr int kCalReminderNote = 4333;
inline constexpr int kCalNewFeed = 4334;        // round 8 phase 8D: "Add feed…" (empties the form and opens it)

// Folder window (4400 - 4499; round 4 phase 12b): the folder window entry's own window.
inline constexpr int kFwInfo = 4400;            // which folder window
inline constexpr int kFwFolderSection = 4401;
inline constexpr int kFwPathLabel = 4402;
inline constexpr int kFwPathEdit = 4403;        // folder.path (commit on Enter / focus loss)
inline constexpr int kFwBrowse = 4404;          // a folder picker
inline constexpr int kFwTitleLabel = 4405;
inline constexpr int kFwTitleEdit = 4406;       // title
inline constexpr int kFwContentsSection = 4407;
inline constexpr int kFwViewLabel = 4408;
inline constexpr int kFwViewCombo = 4409;       // 0 auto, 1 grid, 2 list
inline constexpr int kFwIconLabel = 4410;
inline constexpr int kFwIconEdit = 4411;        // iconSize 32 - 96
inline constexpr int kFwIconSpin = 4412;
inline constexpr int kFwIconUnit = 4413;
inline constexpr int kFwSortLabel = 4414;
inline constexpr int kFwSortCombo = 4415;       // 0 name, 1 date, 2 type, 3 size
inline constexpr int kFwHidden = 4416;          // showHidden (+ caption 4417)
inline constexpr int kFwHiddenCaption = 4417;
inline constexpr int kFwSingleClick = 4418;     // singleClick (+ caption 4419)
inline constexpr int kFwSingleClickCaption = 4419;
inline constexpr int kFwCollapsed = 4420;       // collapsed (+ caption 4421)
inline constexpr int kFwCollapsedCaption = 4421;
inline constexpr int kFwLookSection = 4422;
inline constexpr int kFwOpacityLabel = 4423;
inline constexpr int kFwOpacityEdit = 4424;     // opacity in % (20 - 100)
inline constexpr int kFwOpacitySpin = 4425;
inline constexpr int kFwOpacityUnit = 4426;
inline constexpr int kFwTitleColorLabel = 4427;
inline constexpr int kFwTitleColorEdit = 4428;  // titleColor "#AARRGGBB" ("": the theme's)
inline constexpr int kFwTitleColorNote = 4429;
inline constexpr int kFwLinesLabel = 4430;
inline constexpr int kFwLinesCombo = 4431;      // labelLines: 0 one, 1 two
inline constexpr int kFwHideTitle = 4432;       // hideTitle (+ caption 4433)
inline constexpr int kFwHideTitleCaption = 4433;
inline constexpr int kFwTransparent = 4434;     // transparentBackground (+ caption 4435)
inline constexpr int kFwTransparentCaption = 4435;
inline constexpr int kFwRollUp = 4436;          // rollUpOnHover (+ caption 4437)
inline constexpr int kFwRollUpCaption = 4437;
inline constexpr int kFwRulesSection = 4438;
inline constexpr int kFwRulesLabel = 4439;
inline constexpr int kFwRulesEdit = 4440;       // rules ("*.png;*.jpg")
inline constexpr int kFwAutoSort = 4441;        // autoSort (+ caption 4442)
inline constexpr int kFwAutoSortCaption = 4442;
inline constexpr int kFwSortNow = 4443;         // "Sort desktop now" (= DockCommand 140)
inline constexpr int kFwRulesNote = 4444;

// Standalone widget (4500 - 4599; round 6 phase 6B): a "widget" entry's own window. Round 8 phase
// 8C: its Window tab (kTabWidgetWindow, page 1; the Widget tab, page 0, is the generated settings
// page of item 0). Retired in 8C (reserved): 4500 the info line, 4501 - 4503 the Widget card (its
// name), 4504 Open widget settings (the Widget tab is that page).
inline constexpr int kWwPlaceSection = 4505;
inline constexpr int kWwDisplayLabel = 4506;
inline constexpr int kWwDisplayCombo = 4507;    // monitor: 0 primary, 1.. the others
inline constexpr int kWwXLabel = 4508;
inline constexpr int kWwXEdit = 4509;           // x DIPs from the work area's left (empty: centred)
inline constexpr int kWwYLabel = 4510;
inline constexpr int kWwYEdit = 4511;           // y (empty: centred)
inline constexpr int kWwPositionNote = 4512;
inline constexpr int kWwWLabel = 4513;
inline constexpr int kWwWEdit = 4514;           // w DIPs 48 - 4000
inline constexpr int kWwWSpin = 4515;
inline constexpr int kWwHLabel = 4516;
inline constexpr int kWwHEdit = 4517;           // h DIPs 48 - 4000
inline constexpr int kWwHSpin = 4518;
inline constexpr int kWwSizeNote = 4519;        // the Width label's hint (8C): the widget's minimum size / aspect
inline constexpr int kWwLookSection = 4520;
inline constexpr int kWwLayerLabel = 4521;
inline constexpr int kWwLayerCombo = 4522;      // 0 normal (above windows), 1 desktop only, 2 behind windows
inline constexpr int kWwLayerNote = 4523;
inline constexpr int kWwBackgroundLabel = 4524;
inline constexpr int kWwBackgroundCombo = 4525; // 0 none, 1 panel
inline constexpr int kWwLocked = 4526;          // check box: locked (+ caption 4527)
inline constexpr int kWwLockedCaption = 4527;
inline constexpr int kWwFlash = 4528;           // "Flash this widget" (removed in 8A: the header's Flash)
inline constexpr int kWwNote = 4529;            // how to move / resize it (8C: the Locked check box's hint)
inline constexpr int kWwWUnit = 4530;           // 8C: "DIPs" beside the width
inline constexpr int kWwHUnit = 4531;           // 8C: "DIPs" beside the height

// Round 9 phase 9B: the settings search (5000 - 5099). The entry points, then the children of the
// search dialog (WinDock.SettingsSearch, owned by the settings window it was opened from).
inline constexpr int kFindBox = 5001;       // Global: the nav column's "Find a setting…" box (WinDock.SettingsFind; a click or a key opens the dialog with that key)
inline constexpr int kHeaderFind = 5002;    // Dock: the "⋯" menu's "Find a setting…  Ctrl+F" (WM_COMMAND to the header band)
inline constexpr int kSearchEdit = 5010;    // the search box (WM_SETTEXT + WM_COMMAND(5010, EN_CHANGE) to the dialog filters)
inline constexpr int kSearchList = 5011;    // SysListView32 (report, owner data): Setting, Where, Value
inline constexpr int kSearchGo = 5012;      // "Go to" (the selected row)
inline constexpr int kSearchStatus = 5013;  // "12 of 240 settings"
inline constexpr int kSearchCombo = 5014;   // the drop-down placed over a combo row's Value cell (Space / a click there)

// Round 9 phase 9C: the dock presets (5100 - 5199). The entry points, then the children of the
// chooser (WinDock.Presets, modal to the settings window it was opened from, or the app window).
inline constexpr int kDockAddPreset = 5101;      // Global > Docks and displays: Add ▾ > "Dock from a preset…" (a page command)
inline constexpr int kHeaderPreset = 5102;       // Dock: the "⋯" menu's "Apply a preset…" (WM_COMMAND to the header band)
inline constexpr int kAboutWelcome = 5103;       // Global > About: "Show the welcome dialog"
inline constexpr int kPresetList = 5110;         // the presets (owner-drawn LISTBOX: name + tagline; LB_SETCURSEL + LBN_SELCHANGE)
inline constexpr int kPresetDescription = 5111;  // the selected preset's description
inline constexpr int kPresetPreview = 5112;      // the preview (WinDock.PresetPreview: where it sits, what it puts on the dock; its window text says both)
inline constexpr int kPresetKeep = 5113;         // check box: "Keep my pinned apps and folders" / "Copy my pinned apps from the first dock"
inline constexpr int kPresetApply = 5114;        // "Apply" / "Add dock" / "Use this dock" (the default button)
inline constexpr int kPresetCancel = 5115;       // "Cancel" / "Skip (basic dock)" (= Esc)
inline constexpr int kPresetName = 5116;         // the selected preset's name
inline constexpr int kPresetLook = 5117;         // the look strip (WinDock.SettingsPreview drawing the preset's dock and theme)
inline constexpr int kPresetKeepCaption = 5118;  // the check box's caption (a click toggles it)
inline constexpr int kPresetNote = 5119;         // instead of the check box when the preset has no pins marker
inline constexpr int kPresetLookCaption = 5120;  // "Look (sample icons): Glass theme · grows to 2.0×"
inline constexpr int kPresetHeading = 5121;      // the welcome variant: "Welcome to WinDock"
inline constexpr int kPresetIntro = 5122;        // ... and its line under it

// Round 9 phase 9D: help (5200 - 5399). The entry points, then the children of the Help window
// (WinDock.Help, one per process), then the cards' "?" buttons.
inline constexpr int kNavHelp = 5201;         // Global: the nav column's "Help" row after About (not a page; opens the quick start)
inline constexpr int kMenuHelp = 5202;        // the dock's right-click menu: "Help…" (the quick start)
inline constexpr int kAboutQuickStart = 5203; // Global > About: "Quick start guide"
inline constexpr int kHelpList = 5210;        // the guides (owner-drawn LISTBOX: LB_SETCURSEL + LBN_SELCHANGE)
inline constexpr int kHelpView = 5211;        // the guide (RichEdit, read only; EM_GETFIRSTVISIBLELINE / EM_GETLINE)
inline constexpr int kHelpButtonBase = 5300;  // a card's "?" button: 5300 + the card's index on its page (BM_CLICK opens the Help there)

}  // namespace SettingsIds
