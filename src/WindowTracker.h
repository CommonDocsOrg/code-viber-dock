#pragma once

#include <windows.h>

#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "DockItems.h"

// Tracks the taskbar-worthy top-level windows and groups them by application.
// Event driven: the shell hook (same feed the Explorer taskbar uses) plus an EVENT_OBJECT_UNCLOAKED
// WinEvent hook (UWP windows appear cloaked). Every event (re)starts a 50 ms one-shot timer; when
// it fires, the window list is reconciled with EnumWindows and the change callback runs only if
// the grouped result changed. No polling.
class WindowTracker {
public:
    struct Group {
        AppKey key;
        std::vector<HWND> windows;  // first-seen order
        bool wantsAttention = false;
        bool operator==(const Group&) const = default;
    };
    struct Snapshot {
        std::vector<Group> groups;  // apps in first-seen order
        HWND foreground = nullptr;  // tracked window owning the foreground window, if any
        bool operator==(const Snapshot&) const = default;
    };
    using ChangeFn = std::function<void(const Snapshot&)>;

    static constexpr UINT_PTR kCoalesceTimerId = 3;
    static constexpr UINT kCoalesceMs = 50;

    WindowTracker() = default;
    WindowTracker(const WindowTracker&) = delete;
    WindowTracker& operator=(const WindowTracker&) = delete;
    ~WindowTracker() { Stop(); }

    // Registers the shell hook on `owner` and runs the initial enumeration synchronously
    // (the callback fires before Start returns).
    void Start(HWND owner, ChangeFn onChange);
    void Stop();

    // Feed every message of the owner window. Returns true if consumed.
    bool HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

    const Snapshot& Current() const { return current_; }

    // Tracked window that owns `hwnd` (itself or its root owner), or nullptr.
    HWND TrackedOwnerOf(HWND hwnd) const;

    // Shell windows that are never apps: the desktop (Progman / WorkerW), the taskbars, and
    // Windows.UI.Core.CoreWindow (Start, Search, Action Center and other shell flyouts).
    static bool IsExcludedClass(HWND hwnd);

private:
    struct Tracked {
        HWND hwnd;
        AppKey key;
    };

    static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD,
                                      DWORD);

    void OnShellEvent(WPARAM code, HWND hwnd);
    void Schedule();
    void Reconcile();
    bool PassesFilter(HWND hwnd, bool checkCloak) const;
    void Forget(HWND hwnd);

    HWND owner_ = nullptr;
    ChangeFn onChange_;
    UINT shellHookMsg_ = 0;
    bool registered_ = false;
    HWINEVENTHOOK cloakHook_ = nullptr;

    std::vector<Tracked> windows_;               // first-seen order
    std::unordered_map<HWND, size_t> index_;     // hwnd -> position in windows_ (rebuilt on change)
    std::vector<AppKey> appOrder_;               // first-seen order of apps with windows
    std::unordered_set<HWND> flashing_;
    Snapshot current_;

    static WindowTracker* s_instance;
};
