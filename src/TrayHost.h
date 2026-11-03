#pragma once

#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// One notify icon an app registered with Shell_NotifyIcon (TrayHost's registry).
struct TrayIcon {
    uint64_t id = 0;          // ours: unique for the session, in registration order
    std::wstring key;         // config key: "{guid}" (lower case) or "<exe file name>:<uID>" (lower case)
    HWND owner = nullptr;     // the window that gets the callbacks
    DWORD pid = 0;            // its process
    std::wstring exe;         // its process image (full path; empty if it could not be queried)
    UINT uid = 0;
    GUID guid{};
    bool hasGuid = false;
    UINT callback = 0;        // uCallbackMessage (0: none set: clicks cannot be delivered)
    UINT version = 0;         // NIM_SETVERSION (0, 3 or 4)
    UINT flags = 0;           // every NIF_* flag the app has sent so far (NIF_SHOWTIP of the last message)
    DWORD state = 0;          // NIS_HIDDEN / NIS_SHAREDICON
    HICON icon = nullptr;     // our own copy (CopyIcon), destroyed with the entry
    uint64_t imageSerial = 0; // changes whenever the picture changes (bitmap caches key on it)
    uint64_t imageHash = 0;   // of the copy's pixels (TrayHost: an identical picture is no change)
    std::wstring tip;

    // Shown in a tray grid / drawer: not hidden by the app, has a picture and a callback.
    bool Shown() const { return !(state & NIS_HIDDEN) && icon && callback; }
    // The dock's label is its tooltip: versions < 4 always; version 4 only with NIF_SHOWTIP (the
    // app draws its own popup otherwise and gets NIN_POPUPOPEN / NIN_POPUPCLOSE).
    bool StandardTip() const { return version < NOTIFYICON_VERSION_4 || (flags & NIF_SHOWTIP); }
    // A label for menus and lists: the tooltip's first line, else the exe's file name.
    std::wstring Title() const;
};

// A balloon notification (NIF_INFO) an app showed. Phase 9 routes them into the notification
// centre (SetBalloonSink); until then they are only queued: Explorer, which gets every message
// mirrored, still shows them as toasts.
struct TrayBalloon {
    std::wstring key;
    std::wstring title;
    std::wstring text;
    DWORD infoFlags = 0;  // NIIF_*
    UINT timeoutMs = 0;
    ULONGLONG at = 0;     // GetTickCount64
};

// The system tray takeover (round 4 phase 8; spike findings in ROADMAP.md): a hidden top-level
// window of class Shell_TrayWnd (with a TrayNotifyWnd child), kept above Explorer's in the
// z-order so FindWindow(L"Shell_TrayWnd") (what Shell_NotifyIcon, SHAppBarMessage and
// Shell_NotifyIconGetRect use) finds it first. Started with a TaskbarCreated broadcast: every app
// re-registers its notify icons with it.
//  - WM_COPYDATA 1 (Shell_NotifyIcon): the registry below is updated, then the message is mirrored
//    to Explorer's tray (SendMessageTimeout with the same COPYDATASTRUCT), so Explorer always has
//    every icon: quitting, crashing or a disabled tray loses nothing. The app gets our answer.
//    Icons of this process (the dock's own tray icon) are only mirrored, not registered.
//  - WM_COPYDATA 0 (SHAppBarMessage) and private messages (WM_USER .. 0x7FFF, WM_COMMAND) are
//    forwarded to Explorer's tray and answered with its result.
//  - WM_COPYDATA 3 (Shell_NotifyIconGetRect): answered with the icon's rect from the docks
//    (SetRectResolver): MAKELONG(left, top) for message 1, MAKELONG(width, height) for 2.
// Icons are keyed by GUID (matched on the GUID alone: apps address GUID icons with hWnd 0) or by
// owner window + uID; the HICON is copied at once and the copy compared, so an app re-sending the
// same picture every second (Logitech G HUB) costs no redraw. Owners that die: each owner process
// is waited on (RegisterWaitForSingleObject, a thread-pool wait, no polling) and its icons go when
// it exits; a click that cannot be delivered (the owner window is gone) removes the icon too.
// Explorer moving its tray above ours: a WinEvent hook (EVENT_OBJECT_REORDER, scoped to Explorer's
// process) re-raises our window. Stop() destroys the window and broadcasts TaskbarCreated (apps
// re-register with Explorer, which already has their icons); the crash filter does the same
// (EmergencyRelease) and a marker file makes the next start broadcast too.
// Everything runs on the UI thread; changes reach the docks through one posted, coalesced message.
class TrayHost {
public:
    enum class Click { Left, Double, Right, Middle, KeySelect, ContextKey };

    TrayHost() = default;
    TrayHost(const TrayHost&) = delete;
    TrayHost& operator=(const TrayHost&) = delete;
    ~TrayHost() { Stop(); }

    // Creates the window and broadcasts TaskbarCreated. False if the window could not be made.
    bool Start(HINSTANCE instance);
    // Hands the icons back to Explorer: destroys the window, broadcasts TaskbarCreated, forgets
    // every icon. No-op when not running.
    void Stop();
    bool Running() const { return hwnd_ != nullptr; }
    HWND Hwnd() const { return hwnd_; }
    // Inside one of our own TaskbarCreated broadcasts (App ignores those).
    bool Broadcasting() const { return broadcasting_; }
    // Explorer was restarted (its own TaskbarCreated): re-hook its process, raise our window and
    // ask the apps again (they registered with the new Explorer tray first).
    void OnExplorerRestarted();

    // --- registry ---
    const std::vector<TrayIcon>& Icons() const { return icons_; }
    const TrayIcon* ById(uint64_t id) const;
    const TrayIcon* ByKey(const std::wstring& key) const;
    // Called (posted, coalesced) after icons came, went or changed (picture, tip, state).
    void SetChangedCallback(std::function<void()> cb) { changed_ = std::move(cb); }
    // Where icon `icon` is on screen (a grid cell, the overflow button, a pinned slot), for
    // Shell_NotifyIconGetRect. False: not shown anywhere (the query fails).
    void SetRectResolver(std::function<bool(const TrayIcon& icon, RECT& screen)> resolver) {
        resolver_ = std::move(resolver);
    }
    // Phase 9: every balloon as it arrives (also kept in Balloons(), the last 32).
    void SetBalloonSink(std::function<void(const TrayBalloon&)> sink) { balloonSink_ = std::move(sink); }
    const std::deque<TrayBalloon>& Balloons() const { return balloons_; }
    size_t BalloonCount() const { return balloonCount_; }

    // --- input forwarded to an icon's owner (version 3 / 4 semantics) ---
    // `anchor`: screen px (version 4 sends it in wParam; apps place their menu there). The
    // owner gets the right to take the foreground first. False if the owner is gone (the icon is
    // removed).
    bool Forward(uint64_t id, Click click, POINT anchor);
    // The pointer moved over the icon (WM_MOUSEMOVE: apps refresh their tooltip).
    void MouseMove(uint64_t id, POINT anchor);
    // Version 4 icons without NIF_SHOWTIP: the pointer rested on the icon / left it (at most one
    // is open: opening another closes it; PopupClose of another icon does nothing).
    void PopupOpen(uint64_t id, POINT anchor);
    void PopupClose(uint64_t id, POINT anchor);
    uint64_t PopupShown() const { return popupOpen_; }

    // --- diagnostics (QueryState) ---
    size_t RectQueries() const { return rectQueries_; }
    size_t Raises() const { return raises_; }
    size_t Rebroadcasts() const { return rebroadcasts_; }

    // Crash path (unhandled-exception filter): destroys the window when on its thread and
    // broadcasts TaskbarCreated (posted). No allocation of our own.
    static void EmergencyRelease();
    // Startup: a marker left by a run that did not stop cleanly: broadcast TaskbarCreated once
    // (apps re-register with Explorer, or with a TrayHost started right after) and delete it.
    static void RecoverFromMarker();

private:
    static constexpr UINT kChangedMessage = WM_APP + 1;    // posted: tell the docks (coalesced)
    static constexpr UINT kOwnerGoneMessage = WM_APP + 2;  // posted by a wait: wParam = pid
    static constexpr UINT kMirrorMessage = WM_APP + 3;     // posted: send the queued messages to Explorer
    static constexpr UINT_PTR kRebroadcastTimer = 1;       // one-shot: TaskbarCreated again after a raise
    static constexpr ULONGLONG kRebroadcastMs = 1500;      // at most this often ...
    static constexpr int kRebroadcastsPerMinute = 3;       // ... and this many times a minute
    void ScheduleRebroadcast();

    // A notify message for Explorer (copied: the sender's data is only valid during its call).
    struct Mirror {
        HWND sender = nullptr;
        std::vector<BYTE> data;
    };
    void QueueMirror(HWND sender, const COPYDATASTRUCT& cds);
    void FlushMirrors();

    struct OwnerWait {
        HANDLE process = nullptr;
        HANDLE wait = nullptr;
    };

    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT OnNotifyIcon(HWND sender, const COPYDATASTRUCT& cds);
    LRESULT OnIconRect(const COPYDATASTRUCT& cds);
    // SHAppBarMessage (WM_COPYDATA 0) for Explorer; Relay sends any WM_COPYDATA on to Explorer's tray.
    LRESULT OnAppBar(WPARAM wp, const COPYDATASTRUCT& cds);
    static LRESULT Relay(WPARAM wp, LPARAM lp);
    // Explorer's own Shell_TrayWnd (not ours), or null.
    static HWND ExplorerTray();
    // TaskbarCreated: taking over (not to Explorer's taskbars or our own windows) / handing back
    // (HWND_BROADCAST).
    void Broadcast(bool takeOver);
    void Raise();
    void HookExplorer();
    void Unhook();
    static void CALLBACK OnReorder(HWINEVENTHOOK, DWORD, HWND hwnd, LONG idObject, LONG, DWORD, DWORD);
    int Find(HWND owner, UINT uid, const GUID* guid) const;
    void Remove(size_t index);
    void RemoveOwner(DWORD pid);
    void WatchOwner(DWORD pid);
    static void CALLBACK OnOwnerExit(PVOID context, BOOLEAN timedOut);
    void PostChanged();
    // Posts a callback message to the owner (version semantics); false if it is gone.
    bool Post(const TrayIcon& icon, UINT event, POINT anchor) const;
    static std::wstring ProcessImage(DWORD pid);
    static void WriteMarker(bool on);

    static std::atomic<HWND> live_;  // for the crash path
    static TrayHost* self_;          // the running instance (WinEvent callback)

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWINEVENTHOOK reorderHook_ = nullptr;
    DWORD explorerPid_ = 0;
    UINT msgTaskbarCreated_ = 0;
    bool broadcasting_ = false;
    bool changedPosted_ = false;
    uint64_t nextId_ = 1;
    uint64_t nextSerial_ = 1;
    std::vector<TrayIcon> icons_;
    std::unordered_map<DWORD, OwnerWait> waits_;
    std::unordered_map<DWORD, std::wstring> images_;  // pid -> process image (session cache)
    std::function<void()> changed_;
    std::function<bool(const TrayIcon&, RECT&)> resolver_;
    std::function<void(const TrayBalloon&)> balloonSink_;
    std::deque<TrayBalloon> balloons_;
    size_t balloonCount_ = 0;
    size_t rectQueries_ = 0;
    size_t raises_ = 0;
    uint64_t popupOpen_ = 0;  // the icon NIN_POPUPOPEN was sent to (0: none)
    std::deque<Mirror> mirrors_;
    bool mirrorPosted_ = false;
    ULONGLONG lastBroadcast_ = 0;
    bool rebroadcastPending_ = false;
    size_t rebroadcasts_ = 0;
    ULONGLONG rebroadcastWindow_ = 0;  // start of the current minute of rebroadcasts
    int rebroadcastsInWindow_ = 0;
};
