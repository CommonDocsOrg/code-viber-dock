#pragma once

#include <windows.h>

// The desktop layer: WinDock windows that live on the desktop, directly above the desktop window
// (Progman) at the bottom of the z-order, never topmost.
//
// Two users:
//  - Desktop docks (docks[].layer "desktop", round 4 phase 3b): shown only while the desktop is
//    the foreground (Win+D, a click on the wallpaper, nothing in the foreground); the dock
//    re-evaluates on every foreground change (the shared ForegroundWatch) and calls
//    PlaceAboveDesktop when it shows.
//  - Desktop windows (Phase 12's folder windows): always shown, under every app window, but
//    activatable (a click gives them the keyboard) without rising: Register() puts the window in
//    the layer's list, and the window's WM_WINDOWPOSCHANGING passes through FilterPosChanging(),
//    which drops every z-order change the layer did not make itself (the activation's
//    "bring to top" included: SWP_NOZORDER is added). The layer re-places its windows directly
//    above the desktop when they are registered, when the desktop window changed (Explorer
//    restarted: TaskbarCreated -> Reassert, or a new Progman seen by the foreground hook) and when
//    the desktop takes the foreground (one out-of-context EVENT_SYSTEM_FOREGROUND hook, installed
//    only while a desktop window exists: no polling).
//
// Spikes on build 26300 (STATUS 3b / 12a, ROADMAP Phases 3 and 12): a DirectComposition window
// can also be a child of Progman above SHELLDLL_DefView (it renders and is hit-tested), but a
// child of Explorer's window shares Explorer's input queue (AttachThreadInput: our focus window is
// Explorer's thread's focus, so a hung Explorer would hang our input and the reverse), gets no
// system backdrop (DWMWA_SYSTEMBACKDROP_TYPE fails with E_HANDLE on a child), and goes away with
// Progman when Explorer restarts. The top-level window with the z-order lock renders, takes the
// mouse, the keyboard focus and OLE drops, keeps its acrylic Backdrop window, stays under every
// app window when activated, and is simply re-placed after an Explorer restart.
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

    // --- desktop windows (folder windows) ---
    // Adds `hwnd` (and `under`, its backdrop window kept directly beneath it; may be null) to the
    // layer and places it. Unregister before destroying the window.
    static void Register(HWND hwnd, HWND under);
    static void Unregister(HWND hwnd);
    // The window's backdrop came or went.
    static void SetUnder(HWND hwnd, HWND under);
    // From the window's WM_WINDOWPOSCHANGING: z-order changes the layer did not make are dropped.
    static void FilterPosChanging(HWND hwnd, WINDOWPOS* pos);
    // Places `hwnd` (a registered window) above the desktop if a window of another process is
    // between it and Progman (or it is under Progman). Returns true if it moved.
    static bool Place(HWND hwnd);
    // Every registered window (TaskbarCreated: a new Progman).
    static void Reassert();
    // `hwnd` is above Progman with no visible window of another process between them, not topmost.
    static bool AboveDesktop(HWND hwnd);
    // Test hook (DockCommand 138): every registered window is lifted to the top of the
    // non-topmost band (out of the desktop layer: other apps' windows between it and Progman;
    // Progman itself always stays at the bottom, so it cannot get above them), then the Explorer
    // restart recovery (Reassert, with the desktop window forgotten) runs. Returns bit 0: none
    // was in the layer after the lift, bit 1: all are back above the desktop afterwards.
    static int SimulateExplorerRestart();
    // Registered windows (QueryState).
    static size_t Count();
};
