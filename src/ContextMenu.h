#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "Config.h"

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
    kLastDockCommand = kAutoHideIntelligent,
    kTogglePin = 800,      // per-item: Pin to dock / Unpin from dock
    kNewWindow,            // per-item: launch (a new instance)
    kOpenFileLocation,     // per-item: explorer /select,<target>
    kCloseAllWindows = 900,
    kCloseWindowBase = 1000,  // + window index (per-item menu)
};

struct TrayMenuState {
    AutoHideMode autoHide = AutoHideMode::Off;
    bool hideTaskbar = false;       // the taskbar controller is enabled (taskbar hidden by the dock)
    bool startWithWindows = false;  // the HKCU Run value exists
};

// Shows the dock/tray menu at a screen point and returns the chosen Command (kNone if dismissed).
UINT ShowTrayMenu(HWND owner, POINT screenPt, const TrayMenuState& state);

struct ItemMenuState {
    bool pinned = false;
    bool canPin = true;            // false for apps without a launchable identity
    bool canLaunch = true;
    bool running = false;          // "New window" vs "Open"
    bool canOpenLocation = false;  // path-based items
};

// Per-item menu: the app name (disabled header), Pin/Unpin, New window (Open when not running),
// Open file location, one "Close <title>" entry per window (kCloseWindowBase + index),
// "Close all windows" when there are several, then the dock menu.
UINT ShowItemMenu(HWND owner, POINT screenPt, const std::wstring& appName, const std::vector<std::wstring>& windowTitles,
                  const ItemMenuState& item, const TrayMenuState& state);

}  // namespace ContextMenu
