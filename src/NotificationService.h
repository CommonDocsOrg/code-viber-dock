#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "DockItems.h"

struct TrayBalloon;

// One notification the dock knows: a toast in Windows' notification centre (read through
// UserNotificationListener) or a tray balloon (NIF_INFO) the TrayHost captured.
struct NotificationItem {
    uint32_t id = 0;        // the listener's id; balloons: kBalloonIdBase + n
    std::wstring aumid;     // the toast's AppUserModelId as reported (balloons: empty)
    std::wstring app;       // display name (the toast's app, the balloon owner's exe description)
    AppKey key;             // the dock key it maps to (AppIdentity::KeyForNotificationAumid; empty id: unknown)
    std::wstring exe;       // balloons: the owner's process image
    std::wstring title;     // the first text line
    std::wstring body;      // the other lines, joined with "\n"
    int64_t time = 0;       // creation time, ms since 1970-01-01 UTC
    bool balloon = false;
    bool unread = true;     // false once its app was activated from the dock (clearOnActivate)
};

// Reads Windows' notifications for the badges, the notification centre element and the widgets'
// `notifications` provider (round 4 phase 9; spike findings in ROADMAP.md).
//
// UserNotificationListener works for an unpackaged exe (access follows the global "let apps access
// notifications" consent; GetAccessStatus is called on every poll), but its NotificationChanged
// event throws 0x80070490 without package identity: the only way to see changes is to ask again.
// So a worker thread of its own (MTA, C++/WinRT, blocking .get() calls) polls
// GetNotificationsAsync(Toast), diffs the notification ids and posts only what changed (kMessage,
// a Change with the added items and the removed ids) to the owner window. Cadence: every pollS
// seconds (default 2); after a minute without a change max(pollS, 5) s; every second while the
// notification centre popup is open (SetFast); any change, command or PollNow goes back to pollS.
// The thread exists only while the service runs (DockManager starts it while some dock shows badges
// or a notifications element, a widget uses the provider, or the centre is open): with the
// feature off nothing polls. Each notification's AppInfo / text is read in its own try (AppInfo
// throws E_NOTIMPL for some toasts). Access denied: the worker keeps asking GetAccessStatus at the
// same cadence (nothing else) and the state is Denied until it is allowed again.
//
// Tray balloons arrive on the UI thread (AddBalloon, TrayHost's balloon sink) and are kept here
// (the newest kMaxBalloons) until dismissed.
//
// Test hooks (environment, read at Start): WINDOCK_NOTIFY_FILTER = text (';'-separated parts): only
// toasts whose AUMID (balloons: whose exe path) contains one of them (case-insensitive) are listed, so a test never touches
// the user's own notifications; WINDOCK_NOTIFY_DENIED=1: behave as if access were denied.
class NotificationService {
public:
    static constexpr UINT kMessage = WM_APP + 46;  // wParam: generation, lParam: Change* (owned by the receiver)
    static constexpr uint32_t kBalloonIdBase = 0x80000000u;
    static constexpr size_t kMaxBalloons = 50;
    static constexpr int kSlowAfterMs = 60000;  // no change for this long: poll at max(pollS, 5) s
    static constexpr int kSlowMs = 5000;
    static constexpr int kFastMs = 1000;         // while the centre popup is open

    enum class State { Off = 0, Polling = 1, Denied = 2 };

    NotificationService() = default;
    NotificationService(const NotificationService&) = delete;
    NotificationService& operator=(const NotificationService&) = delete;
    ~NotificationService() { Stop(); }

    // Starts the worker (the first poll follows at once). `pollS`: 1 - 60.
    bool Start(HWND owner, int pollS);
    // Stops the worker (waits up to 2 s for it) and forgets every notification.
    void Stop();
    bool Active() const { return thread_ != nullptr; }
    void SetPollSeconds(int pollS);
    // The centre popup is open: poll every second.
    void SetFast(bool fast);
    // Poll now (automation, the centre opening).
    void PollNow();

    // Removes a notification: a toast from Windows' notification centre (RemoveNotification on
    // the worker; gone from the list at once), a balloon from the list.
    void Dismiss(uint32_t id);
    void Dismiss(const std::vector<uint32_t>& ids);
    // Every notification of the list (the dock's visible list: filtered by WINDOCK_NOTIFY_FILTER).
    void DismissAll();
    // The notifications of this app are read (its app was activated from the dock).
    bool MarkRead(const AppKey& key);
    bool MarkRead(uint32_t id);

    // A tray balloon (TrayHost's sink; UI thread).
    void AddBalloon(const TrayBalloon& b);

    // kMessage: applies a change set, then calls the changed callback. True if handled.
    bool HandleMessage(UINT msg, WPARAM wp, LPARAM lp);
    void SetChanged(std::function<void()> changed) { changed_ = std::move(changed); }

    State GetState() const { return thread_ ? state_ : State::Off; }
    const std::wstring& StateReason() const { return reason_; }
    // Newest first.
    const std::vector<NotificationItem>& Items() const { return items_; }
    const NotificationItem* ById(uint32_t id) const;
    size_t Unread() const;
    size_t UnreadFor(const AppKey& key) const;
    // Diagnostics (automation): polls done by the current worker, ms since the last one (-1 none),
    // the interval the worker waits now.
    uint64_t Polls() const;
    LONGLONG LastPollAgeMs() const;
    int IntervalMs() const;
    // CPU time the worker thread has used (µs; its listener calls, as of its last poll).
    uint64_t WorkerCpuUs() const;
    uint64_t Changes() const { return changes_; }

private:
    struct Shared;  // what the worker and the UI thread share
    struct Change;
    static DWORD WINAPI ThreadMain(void* param);
    void Sort();

    HANDLE thread_ = nullptr;
    std::shared_ptr<Shared> shared_;
    uint64_t generation_ = 0;
    State state_ = State::Off;
    std::wstring reason_;
    std::vector<NotificationItem> items_;
    uint32_t nextBalloon_ = 1;
    std::wstring filter_;  // WINDOCK_NOTIFY_FILTER, lower case
    std::function<void()> changed_;
    uint64_t changes_ = 0;
};
