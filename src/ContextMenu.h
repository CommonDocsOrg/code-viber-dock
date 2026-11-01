#pragma once

#include <windows.h>

namespace ContextMenu {

enum Command : UINT {
    kNone = 0,
    kAutoHide = 100,
    kHideTaskbar,
    kStartWithWindows,
    kOpenConfig,
    kRestoreTaskbar,
    kQuit,
};

struct TrayMenuState {
    bool autoHide = false;
    bool hideTaskbar = false;
    bool startWithWindows = false;
};

// Shows the dock/tray menu at a screen point and returns the chosen Command (kNone if dismissed).
UINT ShowTrayMenu(HWND owner, POINT screenPt, const TrayMenuState& state);

}  // namespace ContextMenu
