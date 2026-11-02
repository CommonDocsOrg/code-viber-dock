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
// Placeholder pages (B1): the "Coming in the next update" text.
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

// Appearance (2400 - 2499), Animation (2500 - 2599), Pinned apps (2600 - 2699): Phase B2.

}  // namespace SettingsIds
