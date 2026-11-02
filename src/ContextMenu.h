#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "Config.h"
#include "DockItems.h"

namespace ContextMenu {

// Dock commands 100 - kLastDockCommand are stable automation ids (WinDock.DockCommand).
enum Command : UINT {
    kNone = 0,
    kAutoHide = 100,  // toggles Off <-> the last on-mode (Always / Intelligent)
    kHideTaskbar,
    kStartWithWindows,
    kOpenConfig,
    kRestoreTaskbar,  // show the taskbar for this session (unchecks "Hide taskbar"; config unchanged)
    kQuit,
    kReloadConfig,
    kOpenSettings = 107,  // opens (or activates) the settings window
    kAutoHideOff = 108,
    kAutoHideAlways = 109,
    kAutoHideIntelligent = 110,
    kRefreshWeather = 111,  // fetches the weather now
    kAddSeparator = 112,  // appends a separator item to the pinned list
    kWeatherSettings = 113,     // opens the settings window on the Weather page
    kToggleCalendar = 114,      // shows / hides the month calendar over the clock
    kToggleWeatherPopup = 115,  // shows / hides the forecast popup over the weather widget
    kLockDock = 116,            // toggles docks[0].locked
    kAddClock = 117,            // Add widget > Clock (DockConfig::AddWidget; not while locked)
    kAddWeather = 118,          // Add widget > Weather
    kAddRunning = 119,          // Add widget > Running apps (only when the dock has none)
    kLastDockCommand = kAddRunning,
    kTogglePin = 800,      // per-item: Pin to dock / Unpin from dock (separators: Remove)
    kNewWindow,            // per-item: launch (a new instance); the Recycle Bin's Open
    kOpenFileLocation,     // per-item: explorer /select,<target>
    kOpenInExplorer = 803,      // folders: open the folder in Explorer
    kChooseIcon = 804,          // pins: the Change Icon dialog (an icon from an .exe / .dll / .ico)
    kResetIcon = 805,           // pins: drop the custom icon (the entry's `icon`)
    kToggleFolderPopup = 806,   // folders: show / hide the contents popup
    kEmptyRecycleBin = 807,     // the Recycle Bin: empty it (the shell's confirmation)
    kChooseImage = 808,         // pins: an open-file dialog for an image / icon file
    kCloseAllWindows = 900,
    kCloseWindowBase = 1000,  // + window index (per-item menu)
};

struct TrayMenuState {
    AutoHideMode autoHide = AutoHideMode::Off;
    bool hideTaskbar = false;       // the taskbar controller is enabled (taskbar hidden by the dock)
    bool startWithWindows = false;  // the HKCU Run value exists
    bool weatherShown = false;      // the dock has a weather item: Refresh weather / Weather settings… entries
    bool weatherConfigured = false; // ... and a location is set (Refresh weather enabled)
    bool locked = false;            // the dock is locked: Lock dock checked, Add separator / widget greyed
    bool hasRunning = false;        // the dock has a running item (Add widget > Running apps greyed)
};

// Shows the dock/tray menu at a screen point and returns the chosen Command (kNone if dismissed).
UINT ShowTrayMenu(HWND owner, POINT screenPt, const TrayMenuState& state);

struct ItemMenuState {
    DockItem::Kind kind = DockItem::Kind::App;
    bool pinned = false;
    bool canPin = true;            // false for apps without a launchable identity
    bool canLaunch = true;
    bool running = false;          // "New window" vs "Open"
    bool canOpenLocation = false;  // path-based items
    bool binEmpty = false;         // the Recycle Bin has nothing in it (Empty is greyed)
    bool hasIcon = false;          // the pin's config entry names an `icon` (Reset icon is greyed otherwise)
};

// Per-item menu, then the dock menu. Apps: the app name (disabled header), Pin/Unpin, the icon
// entries (pinned apps only), New window (Open when not running), Open file location, one
// "Close <title>" entry per window (kCloseWindowBase + index), "Close all windows" when there
// are several. Folders: the name, Unpin, the icon entries, Open in Explorer. Separators: Remove.
// The Recycle Bin: Open, Empty Recycle Bin, Unpin, the icon entries. The icon entries: Choose
// icon…, Choose image…, Reset icon.
UINT ShowItemMenu(HWND owner, POINT screenPt, const std::wstring& appName, const std::vector<std::wstring>& windowTitles,
                  const ItemMenuState& item, const TrayMenuState& state);

}  // namespace ContextMenu
