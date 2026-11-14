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
// Round 26 phase 26A: a timer armed only by a suspension (the pointer or caret moving over the
// foreground app: no accepted event, no Poke) runs `onChange` only when the foreground window's
// cheap state (Probe: its handle, GetWindowRect, its style bits) differs from the last evaluation's,
// and its delay backs off while the suspensions keep coming with nothing changed (kBackoffMs).
class ForegroundWatch {
public:
    static constexpr UINT_PTR kTimerId = 18;
    static constexpr UINT kCoalesceMs = 100;
    // Round 26 phase 26A: the suspension's re-hook delay after 0, kBackoffAfter, 2 x kBackoffAfter
    // ... consecutive idle suspensions (nothing the consumers read had changed when the timer
    // fired): 100 ms for the first half second of movement, then 500 ms for two seconds, then 1 s.
    // A real change (an accepted event, Poke, a differing Probe) starts over at 100 ms. What the
    // longer delay can hold back is only a change the system-wide hooks do not report and that
    // happens while the pointer keeps moving over the app (a maximize / restore by its button or
    // keyboard, Win+Arrow snapping, an app moving itself): it is noticed when the timer fires, at
    // most 1 s late. Foreground changes, drags (MOVESIZEEND) and minimize / restore are hooked
    // system wide and never wait. 2 s (the plan's first idea) would make a maximize click that
    // follows a long pointer movement take up to two seconds to hide the dock; 1 s keeps that
    // short and still cuts the wake-ups of a moving pointer from 10 to about 1 a second.
    static constexpr UINT kBackoffMs[3] = {kCoalesceMs, 500, 1000};
    static constexpr uint32_t kBackoffAfter = 5;

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
    // Round 26 phase 26A (QueryState 359 / 360): `onChange` runs, and timer firings that skipped it
    // (a suspension's re-hook with nothing changed).
    uint32_t Evaluations() const { return evaluations_; }
    uint32_t SkippedEvaluations() const { return skipped_; }

private:
    static void CALLBACK Proc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD,
                              DWORD);
    void OnEvent(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG idObject, LONG idChild);
    // Scopes the location hook to fg's thread (none for no window or one of our own windows,
    // which never overlap the dock).
    void Rehook(HWND fg);
    // Unhooks the location hook and arms the timer, which re-hooks it (a filtered event arrived).
    void Suspend();
    void Schedule(UINT delayMs = kCoalesceMs);

    // Round 26 phase 26A: what the consumers read of the foreground window (DockManager's
    // full-screen guard and dashboard check, DockWindow::ForegroundOverlaps, the desktop layer's
    // rule), reduced to what can change without the system-wide hooks seeing it: the window, its
    // rect and its style's visible / minimized / maximized / caption bits. GetWindowRect instead of
    // the DWM frame bounds the consumers use: the frame cannot move without the window rect moving.
    // Cloaking comes with a foreground change (a virtual desktop switch) and is hooked.
    struct Probe {
        HWND fg = nullptr;
        RECT rect{};
        LONG style = 0;
        bool operator==(const Probe& o) const {
            return fg == o.fg && EqualRect(&rect, &o.rect) && style == o.style;
        }
    };
    static Probe Take();
    void Evaluate();

    HWND owner_ = nullptr;
    std::function<void()> onChange_;
    HWINEVENTHOOK foregroundHook_ = nullptr;  // EVENT_SYSTEM_FOREGROUND
    HWINEVENTHOOK moveSizeHook_ = nullptr;    // EVENT_SYSTEM_MOVESIZEEND
    HWINEVENTHOOK minimizeHook_ = nullptr;    // EVENT_SYSTEM_MINIMIZESTART..EVENT_SYSTEM_MINIMIZEEND
    HWINEVENTHOOK locationHook_ = nullptr;    // EVENT_OBJECT_LOCATIONCHANGE, foreground thread only
    DWORD hookedThread_ = 0;
    HWND fg_ = nullptr;
    bool pending_ = false;      // kTimerId armed
    UINT armedMs_ = 0;          // round 26 phase 26A: the delay it was armed with
    bool suspended_ = false;    // the location hook is off until kTimerId fires
    bool dirty_ = false;        // round 26 phase 26A: an accepted event or Poke since the last evaluation
    uint32_t idleRounds_ = 0;   // consecutive suspension-only firings that found nothing changed
    Probe last_;                // what the last evaluation saw
    uint32_t accepted_ = 0;
    uint32_t delivered_ = 0;
    uint32_t evaluations_ = 0;
    uint32_t skipped_ = 0;

    static ForegroundWatch* s_instance;
};
