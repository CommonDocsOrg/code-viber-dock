#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>

// Tells the dock when the foreground window may have started or stopped overlapping it
// (autoHide "intelligent" only). The shell hook WindowTracker uses sees activation but not moves,
// resizes, minimize/restore or programmatic moves (Win+Arrow, MoveWindow), so this uses WinEvent
// hooks, out-of-context (the callbacks run on the owner's thread):
//  - EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_MOVESIZEEND and EVENT_SYSTEM_MINIMIZESTART..END,
//    system wide, for window objects only;
//  - EVENT_OBJECT_LOCATIONCHANGE scoped to the foreground window's thread (re-scoped on every
//    foreground change), accepted only for the foreground window itself. That thread also
//    reports cursor moves over its windows, caret moves and child objects through this event;
//    the first such event suspends the hook until the timer re-hooks it and re-evaluates.
// Not WINEVENT_SKIPOWNPROCESS: the dock's own windows (a settings window) can be the foreground.
// An accepted event (or a suspension) arms a one-shot kCoalesceMs timer on the owner; when it
// fires, the location hook is re-installed if suspended and `onChange` runs once. Nothing runs
// while no event arrives; Stop() removes the hooks.
class ForegroundWatch {
public:
    static constexpr UINT_PTR kTimerId = 18;
    static constexpr UINT kCoalesceMs = 100;

    ForegroundWatch() = default;
    ForegroundWatch(const ForegroundWatch&) = delete;
    ForegroundWatch& operator=(const ForegroundWatch&) = delete;
    ~ForegroundWatch() { Stop(); }

    // Installs the hooks (if not running yet; a running watch only takes the new callback).
    // Does not call onChange: the caller evaluates the current state itself.
    void Start(HWND owner, std::function<void()> onChange);
    // Unhooks and kills the timer. No-op when stopped.
    void Stop();
    bool Running() const { return owner_ != nullptr; }
    // kTimerId is armed (an evaluation is due within kCoalesceMs).
    bool Pending() const { return pending_; }

    // Feed every message of the owner window. Returns true if consumed (the coalescing timer).
    bool HandleMessage(UINT msg, WPARAM wp, LPARAM lp);
    // Schedules an evaluation (the dock's own geometry changed). No-op when stopped.
    void Poke();
    // The foreground window the location hook is scoped to.
    HWND Foreground() const { return fg_; }

    // Diagnostics: events that armed the timer, and every callback delivered (filtered or not).
    uint32_t AcceptedEvents() const { return accepted_; }
    uint32_t DeliveredEvents() const { return delivered_; }

private:
    static void CALLBACK Proc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD,
                              DWORD);
    void OnEvent(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG idObject, LONG idChild);
    // Scopes the location hook to fg's thread (none for no window or one of our own windows,
    // which never overlap the dock).
    void Rehook(HWND fg);
    // Unhooks the location hook and arms the timer, which re-hooks it (a filtered event arrived).
    void Suspend();
    void Schedule();

    HWND owner_ = nullptr;
    std::function<void()> onChange_;
    HWINEVENTHOOK foregroundHook_ = nullptr;  // EVENT_SYSTEM_FOREGROUND
    HWINEVENTHOOK moveSizeHook_ = nullptr;    // EVENT_SYSTEM_MOVESIZEEND
    HWINEVENTHOOK minimizeHook_ = nullptr;    // EVENT_SYSTEM_MINIMIZESTART..EVENT_SYSTEM_MINIMIZEEND
    HWINEVENTHOOK locationHook_ = nullptr;    // EVENT_OBJECT_LOCATIONCHANGE, foreground thread only
    DWORD hookedThread_ = 0;
    HWND fg_ = nullptr;
    bool pending_ = false;      // kTimerId armed
    bool suspended_ = false;    // the location hook is off until kTimerId fires
    uint32_t accepted_ = 0;
    uint32_t delivered_ = 0;

    static ForegroundWatch* s_instance;
};
