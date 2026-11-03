#pragma once

#include <windows.h>

#include <atomic>

// Hides and restores the Explorer taskbar(s): Shell_TrayWnd on the primary monitor and every
// Shell_SecondaryTrayWnd on the others.
//
// Hide: save the original appbar state and per-monitor work areas (once), switch the taskbar to
// auto-hide so Explorer releases the work area, SW_HIDE every taskbar window, then make sure the
// work area really is the full monitor. A crash marker file holding the saved state is written
// so a later run (or `--restore-taskbar`) can undo the hide after a crash.
//
// Re-hide is event driven: an out-of-context WinEvent hook scoped to Explorer's process catches
// EVENT_OBJECT_SHOW on a taskbar window, and App forwards "TaskbarCreated" (Explorer restart) to
// OnTaskbarCreated(), which re-finds the windows and re-installs the hook for the new PID.
//
// Restore() performs no heap allocation, so it is safe to call from an unhandled-exception
// filter via EmergencyRestore(). Everything runs on the UI thread.
class TaskbarController {
public:
    TaskbarController();
    ~TaskbarController();
    TaskbarController(const TaskbarController&) = delete;
    TaskbarController& operator=(const TaskbarController&) = delete;

    // Config `hideTaskbar`. Enabling hides now; disabling restores now.
    void SetEnabled(bool enabled);
    bool Enabled() const { return enabled_; }

    // Idempotent. No-op while disabled. If Explorer's taskbar does not exist yet (early logon),
    // the hide is applied when OnTaskbarCreated() arrives.
    void Hide();
    // Idempotent. Shows the taskbar, restores the saved appbar state and work areas, removes the
    // hook and deletes the crash marker. The controller stays enabled, so a later Hide() (or
    // SetEnabled(true)) hides again; Explorer restarts do not re-hide after a manual Restore().
    void Restore();
    bool IsHidden() const { return active_; }

    // Explorer (re)created its taskbar: re-find windows, re-hook the new process, re-hide.
    void OnTaskbarCreated();
    // WM_SETTINGCHANGE(SPI_SETWORKAREA) arrived: re-check the work area while hidden.
    void OnWorkAreaChanged();
    // Docks reserve part of these monitors as app bars (docks[].reserveSpace): while hidden, the
    // shell keeps those monitors' work areas, the controller does not force them to the full
    // monitor. An empty list: nothing is reserved.
    void SetReservedMonitors(const RECT* monitors, int count);

    // Startup crash recovery: if a crash marker exists, restore from the state it holds. The
    // recovered original state is carried into the next Hide() so it is not re-captured from
    // the half-restored system. Returns true if a marker was found.
    bool RecoverFromMarker();

    // `--restore-taskbar` with no running dock: restore from the marker, or (no marker) show
    // every taskbar window and turn auto-hide off.
    static void RestoreStandalone();

    // For the unhandled-exception filter: restores the live instance (if any) once.
    static void EmergencyRestore();

private:
    static constexpr int kMaxMonitors = 16;
    static constexpr int kMaxTaskbars = 16;

    // Plain data so it can be used without allocating (crash path) and serialized to the marker.
    struct SavedState {
        bool valid = false;
        UINT abmState = 0;
        int monitorCount = 0;
        RECT monitor[kMaxMonitors]{};
        RECT work[kMaxMonitors]{};
    };

    struct TaskbarList {
        int count = 0;
        HWND hwnd[kMaxTaskbars]{};
    };

    static TaskbarList FindTaskbars();
    static bool IsTaskbarClass(HWND hwnd);
    static HWND PrimaryTaskbar();
    // The monitor edge (ABE_*) the taskbar window `bar` sits on, -1 when unknown.
    static int TaskbarEdge(HWND bar, const RECT& monitor);
    static void CaptureState(SavedState& s);
    static void SetAppBarState(UINT state);

    void ApplyHide();
    void VerifyWorkArea();
    void InstallHook();
    void RemoveHook();
    void ScheduleVerify(UINT delayMs);
    void CancelVerify();

    bool WriteMarker() const;
    bool ReadMarker(SavedState& s) const;
    void DeleteMarker() const;

    static void CALLBACK WinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG idObject, LONG idChild,
                                      DWORD thread, DWORD time);
    static void CALLBACK VerifyTimerProc(HWND, UINT, UINT_PTR, DWORD);

    static std::atomic<TaskbarController*> instance_;

    bool enabled_ = false;
    bool active_ = false;        // taskbar is (meant to be) hidden by us; state + marker saved
    bool pendingHide_ = false;   // Hide() requested before Explorer's taskbar existed
    SavedState saved_;
    SavedState carried_;         // from RecoverFromMarker(), consumed by the next Hide()
    HWINEVENTHOOK hook_ = nullptr;
    DWORD hookPid_ = 0;
    UINT_PTR verifyTimer_ = 0;
    int verifyStep_ = 0;
    int workAreaFixes_ = 0;      // bounded so we can never ping-pong with Explorer
    int reservedCount_ = 0;      // SetReservedMonitors
    RECT reservedMonitors_[kMaxMonitors]{};
    std::atomic<bool> restoring_{false};
    wchar_t markerPath_[MAX_PATH]{};
};
