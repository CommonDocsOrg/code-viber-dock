#include "WindowTracker.h"

#include <dwmapi.h>

#include <algorithm>
#include <iterator>

#include "AppIdentity.h"

WindowTracker* WindowTracker::s_instance = nullptr;

bool WindowTracker::IsExcludedClass(HWND hwnd) {
    wchar_t cls[64]{};
    GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls)));
    static const wchar_t* const kExcluded[] = {
        L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Windows.UI.Core.CoreWindow",
    };
    for (const wchar_t* e : kExcluded) {
        if (wcscmp(cls, e) == 0) return true;
    }
    return false;
}

namespace {

bool IsCloaked(HWND hwnd) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
}

struct EnumState {
    std::vector<HWND> windows;
};

BOOL CALLBACK CollectTopLevel(HWND hwnd, LPARAM lp) {
    reinterpret_cast<EnumState*>(lp)->windows.push_back(hwnd);
    return TRUE;
}

// The monitor a window's centre is on (a minimized window: its restored rect's centre).
HMONITOR MonitorOfWindow(HWND hwnd) {
    RECT r{};
    if (IsIconic(hwnd)) {
        WINDOWPLACEMENT wp{sizeof(wp)};
        if (GetWindowPlacement(hwnd, &wp)) r = wp.rcNormalPosition;  // workspace coordinates: close enough
    } else {
        GetWindowRect(hwnd, &r);
    }
    return MonitorFromPoint(POINT{r.left + (r.right - r.left) / 2, r.top + (r.bottom - r.top) / 2},
                            MONITOR_DEFAULTTONEAREST);
}

}  // namespace

void WindowTracker::Start(HWND owner, ChangeFn onChange) {
    Stop();
    owner_ = owner;
    onChange_ = std::move(onChange);
    s_instance = this;

    shellHookMsg_ = RegisterWindowMessageW(L"SHELLHOOK");
    registered_ = RegisterShellHookWindow(owner_) != FALSE;
    if (!registered_) OutputDebugStringW(L"WinDock: RegisterShellHookWindow failed\n");

    // UWP frames are created cloaked and uncloaked once their content is ready; the shell hook
    // may report them before that. Out-of-context, so the callback runs on this thread.
    cloakHook_ = SetWinEventHook(EVENT_OBJECT_UNCLOAKED, EVENT_OBJECT_UNCLOAKED, nullptr, &WindowTracker::WinEventProc,
                                 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    Reconcile();
}

void WindowTracker::Stop() {
    if (owner_) {
        KillTimer(owner_, kCoalesceTimerId);
        if (registered_) DeregisterShellHookWindow(owner_);
    }
    registered_ = false;
    if (cloakHook_) {
        UnhookWinEvent(cloakHook_);
        cloakHook_ = nullptr;
    }
    if (s_instance == this) s_instance = nullptr;
    owner_ = nullptr;
    onChange_ = nullptr;
    windows_.clear();
    index_.clear();
    appOrder_.clear();
    flashing_.clear();
    current_ = {};
}

void CALLBACK WindowTracker::WinEventProc(HWINEVENTHOOK, DWORD, HWND hwnd, LONG idObject, LONG, DWORD, DWORD) {
    if (!s_instance || idObject != OBJID_WINDOW || !hwnd) return;
    if (GetAncestor(hwnd, GA_PARENT) != GetDesktopWindow()) return;  // top-level only
    s_instance->Schedule();
}

bool WindowTracker::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    if (shellHookMsg_ && msg == shellHookMsg_) {
        OnShellEvent(wp, reinterpret_cast<HWND>(lp));
        return true;
    }
    if (msg == WM_TIMER && wp == kCoalesceTimerId) {
        KillTimer(owner_, kCoalesceTimerId);  // one-shot
        Reconcile();
        return true;
    }
    return false;
}

void WindowTracker::OnShellEvent(WPARAM code, HWND hwnd) {
    switch (code) {
        case HSHELL_WINDOWCREATED:
            Forget(hwnd);  // a recycled handle value must be re-identified
            break;
        case HSHELL_WINDOWDESTROYED:
            Forget(hwnd);
            flashing_.erase(hwnd);
            break;
        case HSHELL_WINDOWACTIVATED:
        case HSHELL_RUDEAPPACTIVATED:
            if (hwnd) flashing_.erase(hwnd);
            break;
        case HSHELL_FLASH:
            if (hwnd && hwnd != GetForegroundWindow()) {
                const HWND owner = TrackedOwnerOf(hwnd);
                flashing_.insert(owner ? owner : hwnd);
            }
            break;
        case HSHELL_REDRAW:          // title/icon change: cached identity stays valid
        case HSHELL_WINDOWREPLACED:
        case HSHELL_WINDOWREPLACING:
        case HSHELL_MONITORCHANGED:
            break;
        default:
            return;  // not interesting (HSHELL_GETMINRECT, language, app commands, ...)
    }
    Schedule();
}

void WindowTracker::Schedule() {
    if (owner_) SetTimer(owner_, kCoalesceTimerId, kCoalesceMs, nullptr);
}

void WindowTracker::Forget(HWND hwnd) {
    auto it = std::find_if(windows_.begin(), windows_.end(), [&](const Tracked& t) { return t.hwnd == hwnd; });
    if (it == windows_.end()) return;
    windows_.erase(it);
    index_.clear();
    for (size_t i = 0; i < windows_.size(); ++i) index_[windows_[i].hwnd] = i;
}

bool WindowTracker::PassesFilter(HWND hwnd, bool checkCloak) const {
    if (hwnd == owner_ || !IsWindow(hwnd) || !IsWindowVisible(hwnd)) return false;
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return false;
    // Like the taskbar: WS_EX_NOACTIVATE windows (search bars, OSDs) only count with WS_EX_APPWINDOW.
    if ((ex & WS_EX_NOACTIVATE) && !(ex & WS_EX_APPWINDOW)) return false;
    if (GetAncestor(hwnd, GA_ROOTOWNER) != hwnd && !(ex & WS_EX_APPWINDOW)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return false;
    if (IsExcludedClass(hwnd)) return false;
    if (checkCloak) {
        // Only for windows we don't track yet: UWP windows are cloaked while minimized.
        if (IsCloaked(hwnd)) return false;
        if (GetWindowTextLengthW(hwnd) == 0) return false;
    }
    return true;
}

HWND WindowTracker::TrackedOwnerOf(HWND hwnd) const {
    if (!hwnd) return nullptr;
    if (index_.contains(hwnd)) return hwnd;
    HWND root = GetAncestor(hwnd, GA_ROOTOWNER);
    if (root && index_.contains(root)) return root;
    return nullptr;
}

void WindowTracker::Reconcile() {
    if (!owner_) return;

    // 1. Drop windows that are gone or no longer taskbar-worthy (no cloak/title check for them).
    std::erase_if(windows_, [&](const Tracked& t) { return !PassesFilter(t.hwnd, false); });
    index_.clear();
    for (size_t i = 0; i < windows_.size(); ++i) index_[windows_[i].hwnd] = i;

    // 2. Add new windows in z-order. Identity is cached per window, so this is cheap.
    EnumState state;
    EnumWindows(CollectTopLevel, reinterpret_cast<LPARAM>(&state));
    for (HWND hwnd : state.windows) {
        if (index_.contains(hwnd) || !PassesFilter(hwnd, true)) continue;
        std::optional<AppKey> key = AppIdentity::IdentifyWindow(hwnd);
        if (!key) continue;  // retried on the next event
        index_[hwnd] = windows_.size();
        windows_.push_back(Tracked{hwnd, std::move(*key)});
    }

    // 3. Attention state: only for tracked, non-foreground windows.
    const HWND fgOwner = TrackedOwnerOf(GetForegroundWindow());
    std::erase_if(flashing_, [&](HWND h) { return h == fgOwner || !index_.contains(h); });

    // 4. App order: keep first-seen order; drop apps without windows, append new ones.
    std::erase_if(appOrder_, [&](const AppKey& k) {
        return std::none_of(windows_.begin(), windows_.end(), [&](const Tracked& t) { return t.key == k; });
    });
    for (const Tracked& t : windows_) {
        if (std::find(appOrder_.begin(), appOrder_.end(), t.key) == appOrder_.end()) appOrder_.push_back(t.key);
    }

    Snapshot snap;
    snap.foreground = fgOwner;
    snap.groups.reserve(appOrder_.size());
    for (const AppKey& k : appOrder_) {
        Group g;
        g.key = k;
        for (const Tracked& t : windows_) {
            if (t.key != k) continue;
            g.windows.push_back(t.hwnd);
            g.monitors.push_back(MonitorOfWindow(t.hwnd));
            if (flashing_.contains(t.hwnd)) g.wantsAttention = true;
        }
        snap.groups.push_back(std::move(g));
    }

    if (snap == current_) return;
    current_ = std::move(snap);
    if (onChange_) onChange_(current_);
}
