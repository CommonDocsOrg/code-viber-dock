#include "ForegroundWatch.h"

#include <algorithm>

ForegroundWatch* ForegroundWatch::s_instance = nullptr;

namespace {

HWINEVENTHOOK Hook(DWORD first, DWORD last, WINEVENTPROC proc, DWORD pid = 0, DWORD tid = 0) {
    HWINEVENTHOOK h = SetWinEventHook(first, last, nullptr, proc, pid, tid, WINEVENT_OUTOFCONTEXT);
    if (!h) OutputDebugStringW(L"WinDock: SetWinEventHook failed (foreground watch)\n");
    return h;
}

void Unhook(HWINEVENTHOOK& h) {
    if (h) UnhookWinEvent(h);
    h = nullptr;
}

}  // namespace

void ForegroundWatch::Start(HWND owner, std::function<void()> onChange) {
    if (owner_ == owner && owner_) {
        onChange_ = std::move(onChange);
        return;
    }
    Stop();
    owner_ = owner;
    onChange_ = std::move(onChange);
    s_instance = this;
    // Three narrow system-wide hooks rather than one over EVENT_SYSTEM_FOREGROUND..MINIMIZEEND:
    // that range also carries capture, menu, scrolling and drag-drop events (every click in every
    // app), which would all be delivered to this thread only to be dropped.
    foregroundHook_ = Hook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, &ForegroundWatch::Proc);
    moveSizeHook_ = Hook(EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND, &ForegroundWatch::Proc);
    minimizeHook_ = Hook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, &ForegroundWatch::Proc);
    Rehook(GetForegroundWindow());
    // Round 26 phase 26A: the caller evaluates now; that is the state a suspension compares with.
    last_ = Take();
    dirty_ = false;
    idleRounds_ = 0;
}

void ForegroundWatch::Stop() {
    if (owner_ && pending_) KillTimer(owner_, kTimerId);
    pending_ = false;
    armedMs_ = 0;
    suspended_ = false;
    dirty_ = false;
    idleRounds_ = 0;
    Unhook(foregroundHook_);
    Unhook(moveSizeHook_);
    Unhook(minimizeHook_);
    Unhook(locationHook_);
    hookedThread_ = 0;
    fg_ = nullptr;
    if (s_instance == this) s_instance = nullptr;
    owner_ = nullptr;
    onChange_ = nullptr;
}

void CALLBACK ForegroundWatch::Proc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD,
                                    DWORD) {
    if (s_instance) s_instance->OnEvent(hook, event, hwnd, idObject, idChild);
}

void ForegroundWatch::OnEvent(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG idObject, LONG idChild) {
    ++delivered_;
    if (!owner_ || !hook) return;
    const bool windowObject = hwnd && idObject == OBJID_WINDOW && idChild == CHILDID_SELF;
    if (hook == locationHook_) {
        // Only the foreground window itself counts. The same event also reports the cursor (one
        // per mouse move over the app's windows), the caret and child objects, and a hook cannot
        // filter by object: on the first such event the hook is suspended (Suspend) instead.
        if (!windowObject || hwnd != fg_) {
            Suspend();
            return;
        }
    } else if (!windowObject) {
        return;
    } else if (hook == foregroundHook_) {
        if (event != EVENT_SYSTEM_FOREGROUND) return;
        Rehook(hwnd);
    } else if (hook == moveSizeHook_) {
        if (event != EVENT_SYSTEM_MOVESIZEEND) return;
    } else if (hook == minimizeHook_) {
        if (event != EVENT_SYSTEM_MINIMIZESTART && event != EVENT_SYSTEM_MINIMIZEEND) return;
    } else {
        return;  // queued before its hook was replaced (Rehook / Suspend) or removed
    }
    ++accepted_;
    dirty_ = true;  // round 26 phase 26A: the next firing evaluates, at the short delay
    idleRounds_ = 0;
    Schedule();
}

void ForegroundWatch::Rehook(HWND fg) {
    fg_ = fg;
    if (suspended_) return;  // the timer re-hooks for the then-current foreground window
    DWORD pid = 0;
    DWORD tid = fg ? GetWindowThreadProcessId(fg, &pid) : 0;
    if (pid == GetCurrentProcessId()) tid = 0;  // our own windows never count as overlapping
    if (tid == hookedThread_) return;  // nonzero only while locationHook_ is installed
    Unhook(locationHook_);
    hookedThread_ = 0;
    if (!tid) return;
    locationHook_ = Hook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, &ForegroundWatch::Proc, pid, tid);
    if (locationHook_) hookedThread_ = tid;
}

void ForegroundWatch::Suspend() {
    // While the mouse moves over the foreground app (or it moves its caret), its thread reports a
    // cursor / caret LOCATIONCHANGE per step. Unhooking at the first one and re-hooking (plus one
    // evaluation, which catches a window move missed meanwhile) when the timer fires bounds that
    // to one delivered event and one evaluation per kCoalesceMs instead of one per mouse move.
    // Round 26 phase 26A: that still woke the dock (and ran every consumer: DWM queries, the
    // monitors, SHQueryUserNotificationState, each dock's overlap test) ten times a second for as
    // long as the pointer moved over any app or the user typed. Now the firing compares a cheap
    // Probe first (HandleMessage), and the re-hook waits longer while suspensions keep finding
    // nothing changed (kBackoffMs; the latency this accepts is explained there).
    Unhook(locationHook_);
    hookedThread_ = 0;
    suspended_ = true;
    const uint32_t step = std::min<uint32_t>(idleRounds_ / kBackoffAfter, 2);
    Schedule(kBackoffMs[step]);
}

void ForegroundWatch::Schedule(UINT delayMs) {
    // Throttle, not debounce: a window that keeps moving (dragged across the bar) still gets an
    // evaluation every kCoalesceMs instead of none until it stops.
    if (!owner_) return;
    // Round 26 phase 26A: armed for a longer back-off delay, a real event shortens it (SetTimer on
    // the same id replaces the countdown).
    if (pending_ && armedMs_ <= delayMs) return;
    pending_ = SetTimer(owner_, kTimerId, delayMs, nullptr) != 0;
    armedMs_ = delayMs;
}

void ForegroundWatch::Poke() {
    dirty_ = true;  // round 26 phase 26A: the dock's own geometry changed: always evaluate
    idleRounds_ = 0;
    Schedule();
}

ForegroundWatch::Probe ForegroundWatch::Take() {
    Probe p;
    p.fg = GetForegroundWindow();
    if (p.fg) {
        GetWindowRect(p.fg, &p.rect);
        p.style = GetWindowLongW(p.fg, GWL_STYLE) & static_cast<LONG>(WS_VISIBLE | WS_MINIMIZE | WS_MAXIMIZE | WS_CAPTION);
    }
    return p;
}

void ForegroundWatch::Evaluate() {
    last_ = Take();
    dirty_ = false;
    ++evaluations_;
    if (onChange_) onChange_();
}

bool ForegroundWatch::HandleMessage(UINT msg, WPARAM wp, LPARAM) {
    if (msg != WM_TIMER || wp != kTimerId) return false;
    if (!owner_) return true;  // queued before Stop()
    KillTimer(owner_, kTimerId);  // one-shot
    pending_ = false;
    suspended_ = false;
    // Re-hooks after a suspension, and follows the real foreground window after a missed or
    // reordered foreground event.
    const HWND fg = GetForegroundWindow();
    if (fg != fg_ || !locationHook_) Rehook(fg);
    // Round 26 phase 26A: armed only by a suspension (cursor / caret events): the consumers run
    // only when what they read can have changed. The suspension window is where a move of the
    // foreground window could have been missed; the Probe catches it.
    if (!dirty_) {
        const Probe now = Take();
        if (now == last_) {
            ++skipped_;
            ++idleRounds_;
            return true;
        }
    }
    idleRounds_ = 0;
    Evaluate();
    return true;
}
