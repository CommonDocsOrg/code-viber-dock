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
inline constexpr int kPagePinned = 4;
inline constexpr int kPageAbout = 5;
inline constexpr int kPageCount = 6;

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

// Appearance (2400 - 2499; the theme field rows: 3000 - 3299, see FieldRow)
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

// One row per Theme::Field (Theme::Fields() order): id = FieldRow(field index, part).
inline constexpr int kFieldRowBase = 3000;
inline constexpr int kFieldRowStride = 10;
inline constexpr int kFieldLabel = 0;    // row label (static; Color / Float rows)
inline constexpr int kFieldSwatch = 1;   // Color: owner-drawn button (ChooseColor); Bool: the check box
inline constexpr int kFieldHex = 2;      // Color: "#AARRGGBB" edit (applied on Enter / focus loss); Bool: caption
inline constexpr int kFieldValue = 3;    // Color: alpha edit 0 - 255; Float: value edit
inline constexpr int kFieldSpin = 4;     // up-down of kFieldValue
inline constexpr int kFieldReset = 5;    // "Reset" button, shown only while the field is overridden
inline constexpr int kFieldUnit = 6;     // Float: the unit note ("DIPs")
constexpr int FieldRow(int field, int part) { return kFieldRowBase + field * kFieldRowStride + part; }

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

// Pinned apps (2600 - 2699)
inline constexpr int kPinnedList = 2600;          // SysListView32 (report): Name, Target
inline constexpr int kPinAdd = 2601;              // "Add…" (open dialog, .lnk / .exe, multi-select)
inline constexpr int kPinRemove = 2602;
inline constexpr int kPinUp = 2603;
inline constexpr int kPinDown = 2604;
inline constexpr int kPinnedNote = 2605;

}  // namespace SettingsIds
