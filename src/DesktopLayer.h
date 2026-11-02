#pragma once

#include <windows.h>

// The desktop layer (docks[].layer "desktop"; the base for Phase 12's desktop folder windows): a
// top-level window that is shown only while the desktop is the foreground (Win+D, a click on the
// wallpaper, nothing in the foreground) and sits at the bottom of the z-order, directly above the
// desktop window (Progman), never topmost. The owner re-evaluates on every foreground change
// (the shared ForegroundWatch's WinEvent hooks; no polling).
//
// Spike on build 26300 (STATUS 3b, ROADMAP Phase 3): a DirectComposition window parented into the
// desktop renders (a child of the WorkerW inside Progman, or of Progman itself), but the WorkerW
// child lies under SHELLDLL_DefView (the icon layer covers the whole monitor and takes the mouse),
// and a child window would need parent-relative placement, no topmost handling and no separate
// acrylic window; this top-level placement keeps the dock's own placement code and input as they
// are and stays directly above Progman when the desktop takes the foreground.
class DesktopLayer {
public:
    // The desktop window (Progman), or null when Explorer's desktop does not exist.
    static HWND Desktop();
    // The desktop "is visible": the foreground window is the desktop (Progman / WorkerW or a
    // window inside them), there is none, or it is one of this process's windows (a dock popup,
    // the settings window: they belong to the desktop dock's session).
    static bool DesktopForeground();
    // Puts `hwnd` directly above the desktop window in the z-order (not topmost). No-op when it is
    // there already; without a desktop window: the bottom of the z-order.
    static void PlaceAboveDesktop(HWND hwnd);
};
