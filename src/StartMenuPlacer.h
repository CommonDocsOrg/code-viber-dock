#pragma once

#include <windows.h>

#include <cstdint>

struct IUIAutomation;

// Round 16 phase 16A: opens the Windows Start menu beside the Start widget that was clicked instead
// of where the hidden taskbar's Start button would be. Windows lays the menu out inside one
// full-screen host window (StartMenuExperienceHost's "Start" CoreWindow); moving that window before
// the Windows key is pressed moves the menu with it (measured, see StartMenuPlacer.cpp). Every path
// that leaves the host shifted puts it back: the watch (the menu closed), Shutdown, a failed or
// dry-run key press, and EmergencyRestore from the crash filter.
//
// One instance, owned by App, used on the UI thread only. Nothing runs while Start is closed: no
// hook, no timer; a 250 ms one-shot poll (kWatchTimerId on the app window) only while the menu is
// open beside a button.
class StartMenuPlacer {
public:
    // The watch's timer on the app window (DockWindow.h's table).
    static constexpr UINT_PTR kWatchTimerId = 45;

    StartMenuPlacer() = default;
    StartMenuPlacer(const StartMenuPlacer&) = delete;
    StartMenuPlacer& operator=(const StartMenuPlacer&) = delete;
    ~StartMenuPlacer() { Shutdown(); }

    // `owner`: the window that gets the watch's WM_TIMER (App's window).
    void Initialize(HWND owner) { owner_ = owner; }
    // Restore(), then the UI Automation client is released.
    void Shutdown();

    // The Start host window (null: not found: another shell, a future build; then Place does
    // nothing and the caller just sends the key).
    HWND Host();
    // The menu is open: the host is not cloaked.
    bool IsOpen();
    // The menu panel's rect in screen px at the host's natural place (the shift taken out): from UI
    // Automation while the menu is open, else the last rect read that way (its place inside the
    // host), else the anchor model's guess. Empty when there is no host.
    RECT PanelRect();
    // Shifts the host so that the menu opens beside `boxPx` (screen px) on `side` (0 above, 1 below,
    // 2 right, 3 left: WidgetInstance::Client::EditorPlace), clamped into `monitor`. Called BEFORE
    // the key is pressed. `dryRun`: computes the shift (LastShift) and moves nothing. Returns true
    // when the host was moved (false: nothing to do, the menu is open already, no host, a dry run).
    bool Place(const RECT& boxPx, int side, HMONITOR monitor, bool dryRun = false);
    // The host back at its natural place, the watch stopped. Cheap when nothing is shifted.
    void Restore();
    // The app window's WM_TIMER: true when it was the watch's.
    bool OnTimer(UINT_PTR id);

    // QueryState 331: 0 no host window found, 1 found and at its natural place, 2 shifted.
    int State();
    // QueryState 332: the last shift Place computed (also on a dry run), MAKELONG(dx, dy).
    LPARAM LastShift() const {
        return static_cast<LPARAM>(static_cast<uint32_t>(MAKELONG(static_cast<uint16_t>(static_cast<int16_t>(lastDx_)),
                                                                  static_cast<uint16_t>(static_cast<int16_t>(lastDy_)))));
    }

    // The crash path (main.cpp CrashRestoreTaskbar): one SetWindowPos from static values written by
    // Place / Restore; no allocation, no COM.
    static void EmergencyRestore();

private:
    // Reads the panel's rect from UI Automation (screen px, the host where it is now). False while
    // the menu is closed (the cloaked tree has no rectangles) or nothing sensible came back.
    bool ReadPanel(HWND host, RECT& panel);
    void StartWatch();
    void StopWatch();

    HWND owner_ = nullptr;
    HWND host_ = nullptr;          // the cached host window (IsWindow-checked per use)
    IUIAutomation* uia_ = nullptr; // created on first use, released at Shutdown
    bool uiaFailed_ = false;       // CoCreateInstance failed once: not tried again
    // The last good panel rect read from UI Automation, relative to the host's top-left.
    bool panelKnown_ = false;
    RECT panelInHost_{};
    // The shift in force (shifted_), the host it applies to and its natural top-left.
    bool shifted_ = false;
    HWND shiftedHost_ = nullptr;
    POINT natural_{};
    int dx_ = 0, dy_ = 0;
    int lastDx_ = 0, lastDy_ = 0;
    // The watch: ticks since Place, whether the menu was seen open since, ticks it has been open.
    bool watching_ = false;
    bool seenOpen_ = false;
    int ticks_ = 0;
    int openTicks_ = 0;
    bool readThisOpen_ = false;
};
