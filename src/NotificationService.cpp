#include "NotificationService.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.UI.Notifications.h>
#include <winrt/Windows.UI.Notifications.Management.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <set>

#include "AppIdentity.h"
#include "TrayHost.h"

namespace {

namespace wun = winrt::Windows::UI::Notifications;
using Listener = wun::Management::UserNotificationListener;
using Access = wun::Management::UserNotificationListenerAccessStatus;

std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

std::wstring EnvText(const wchar_t* name) {
    wchar_t buf[512];
    const DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    return n > 0 && n < std::size(buf) ? std::wstring(buf, n) : std::wstring();
}

// WINDOCK_NOTIFY_FILTER: ';'-separated parts (lower case); `text` passes when it contains one.
bool Passes(const std::wstring& filter, const std::wstring& text) {
    if (filter.empty()) return true;
    const std::wstring t = Lower(text);
    size_t start = 0;
    while (start <= filter.size()) {
        size_t end = filter.find(L';', start);
        if (end == std::wstring::npos) end = filter.size();
        if (end > start && t.find(filter.substr(start, end - start)) != std::wstring::npos) return true;
        start = end + 1;
    }
    return false;
}

int64_t UnixMs(winrt::Windows::Foundation::DateTime t) {
    // winrt::clock counts 100 ns ticks since 1601 (FILETIME).
    const int64_t ticks = t.time_since_epoch().count();
    return (ticks - 116444736000000000LL) / 10000;
}

int64_t NowUnixMs() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    const int64_t ticks = (static_cast<int64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return (ticks - 116444736000000000LL) / 10000;
}

}  // namespace

struct NotificationService::Shared {
    HWND owner = nullptr;
    uint64_t generation = 0;
    std::wstring filter;  // lower case; empty: everything
    bool simulateDenied = false;
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);   // manual reset: the worker ends
    HANDLE work = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto reset: poll now / a command
    SRWLOCK lock = SRWLOCK_INIT;
    std::vector<uint32_t> dismiss;  // ids to RemoveNotification
    std::atomic<int> pollMs{2000};
    std::atomic<bool> fast{false};
    std::atomic<uint64_t> polls{0};
    std::atomic<ULONGLONG> lastPoll{0};
    std::atomic<int> interval{2000};
    std::atomic<uint64_t> cpuUs{0};  // the worker thread's CPU time (user + kernel)
    ~Shared() {
        if (stop) CloseHandle(stop);
        if (work) CloseHandle(work);
    }
};

struct NotificationService::Change {
    State state = State::Polling;
    std::wstring reason;
    bool reset = false;  // drop every toast first (the state changed)
    std::vector<NotificationItem> added;
    std::vector<uint32_t> removed;
};

bool NotificationService::Start(HWND owner, int pollS) {
    if (Active()) return true;
    auto shared = std::make_shared<Shared>();
    if (!shared->stop || !shared->work || !owner) return false;
    shared->owner = owner;
    shared->generation = ++generation_;
    shared->pollMs = std::clamp(pollS, 1, 60) * 1000;
    shared->interval = shared->pollMs.load();
    filter_ = Lower(EnvText(L"WINDOCK_NOTIFY_FILTER"));
    shared->filter = filter_;
    shared->simulateDenied = EnvText(L"WINDOCK_NOTIFY_DENIED") == L"1";
    auto* param = new std::shared_ptr<Shared>(shared);
    HANDLE t = CreateThread(nullptr, 0, &NotificationService::ThreadMain, param, 0, nullptr);
    if (!t) {
        delete param;
        return false;
    }
    thread_ = t;
    shared_ = std::move(shared);
    state_ = State::Polling;
    reason_.clear();
    return true;
}

void NotificationService::Stop() {
    if (!thread_) return;
    SetEvent(shared_->stop);
    // A listener call in progress finishes first; past 2 s the thread is left to end by itself (its
    // last change set carries an old generation and is dropped).
    WaitForSingleObject(thread_, 2000);
    CloseHandle(thread_);
    thread_ = nullptr;
    shared_.reset();
    ++generation_;
    const bool had = !items_.empty();
    items_.clear();
    state_ = State::Off;
    reason_.clear();
    if (had && changed_) changed_();
}

void NotificationService::SetPollSeconds(int pollS) {
    if (!shared_) return;
    const int ms = std::clamp(pollS, 1, 60) * 1000;
    if (shared_->pollMs.exchange(ms) != ms) SetEvent(shared_->work);
}

void NotificationService::SetFast(bool fast) {
    if (!shared_) return;
    if (shared_->fast.exchange(fast) != fast && fast) SetEvent(shared_->work);
}

void NotificationService::PollNow() {
    if (shared_) SetEvent(shared_->work);
}

void NotificationService::Dismiss(uint32_t id) { Dismiss(std::vector<uint32_t>{id}); }

void NotificationService::Dismiss(const std::vector<uint32_t>& ids) {
    std::vector<uint32_t> toasts;
    bool changed = false;
    for (uint32_t id : ids) {
        const auto it = std::find_if(items_.begin(), items_.end(), [&](const NotificationItem& n) { return n.id == id; });
        if (it == items_.end()) continue;
        if (!it->balloon) toasts.push_back(id);
        items_.erase(it);
        changed = true;
    }
    if (!toasts.empty() && shared_) {
        AcquireSRWLockExclusive(&shared_->lock);
        if (shared_->dismiss.size() < 4096) shared_->dismiss.insert(shared_->dismiss.end(), toasts.begin(), toasts.end());
        ReleaseSRWLockExclusive(&shared_->lock);
        SetEvent(shared_->work);
    }
    if (changed) {
        ++changes_;
        if (changed_) changed_();
    }
}

void NotificationService::DismissAll() {
    std::vector<uint32_t> ids;
    ids.reserve(items_.size());
    for (const NotificationItem& n : items_) ids.push_back(n.id);
    Dismiss(ids);
}

bool NotificationService::MarkRead(const AppKey& key) {
    if (key.id.empty()) return false;
    bool changed = false;
    for (NotificationItem& n : items_) {
        if (n.unread && n.key == key) {
            n.unread = false;
            changed = true;
        }
    }
    if (changed) {
        ++changes_;
        if (changed_) changed_();
    }
    return changed;
}

bool NotificationService::MarkRead(uint32_t id) {
    for (NotificationItem& n : items_) {
        if (n.id != id || !n.unread) continue;
        n.unread = false;
        ++changes_;
        if (changed_) changed_();
        return true;
    }
    return false;
}

void NotificationService::AddBalloon(const TrayBalloon& b) {
    if (!Active()) return;
    if (!Passes(filter_, b.exe)) return;
    NotificationItem n;
    n.id = kBalloonIdBase | (nextBalloon_++ & 0x7FFFFFFFu);
    n.balloon = true;
    n.exe = b.exe;
    if (!b.exe.empty()) n.key = AppIdentity::Canonicalize(AppIdentity::MakePathKey(b.exe));
    n.app = n.key.id.empty() ? std::wstring() : AppIdentity::DisplayNameFor(n.key);
    if (n.app.empty()) n.app = b.key;
    n.title = b.title;
    n.body = b.text;
    n.time = NowUnixMs();
    items_.push_back(std::move(n));
    // Only the newest balloons are kept.
    size_t balloons = 0;
    for (auto it = items_.rbegin(); it != items_.rend(); ++it) balloons += it->balloon ? 1 : 0;
    Sort();
    while (balloons > kMaxBalloons) {
        const auto oldest = std::find_if(items_.rbegin(), items_.rend(), [](const NotificationItem& x) { return x.balloon; });
        if (oldest == items_.rend()) break;
        items_.erase(std::next(oldest).base());
        --balloons;
    }
    ++changes_;
    if (changed_) changed_();
}

void NotificationService::Sort() {
    std::stable_sort(items_.begin(), items_.end(), [](const NotificationItem& a, const NotificationItem& b) {
        return a.time != b.time ? a.time > b.time : a.id > b.id;
    });
}

bool NotificationService::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg != kMessage) return false;
    std::unique_ptr<Change> c(reinterpret_cast<Change*>(lp));
    if (!c || !thread_ || static_cast<uint64_t>(wp) != generation_) return true;  // an old worker's
    state_ = c->state;
    reason_ = c->reason;
    if (c->reset) std::erase_if(items_, [](const NotificationItem& n) { return !n.balloon; });
    if (!c->removed.empty()) {
        const std::set<uint32_t> gone(c->removed.begin(), c->removed.end());
        std::erase_if(items_, [&](const NotificationItem& n) { return !n.balloon && gone.count(n.id); });
    }
    for (NotificationItem& n : c->added) {
        if (std::any_of(items_.begin(), items_.end(), [&](const NotificationItem& x) { return x.id == n.id; })) continue;
        n.key = n.aumid.empty() ? AppKey{} : AppIdentity::KeyForNotificationAumid(n.aumid);
        items_.push_back(std::move(n));
    }
    Sort();
    ++changes_;
    if (changed_) changed_();
    return true;
}

const NotificationItem* NotificationService::ById(uint32_t id) const {
    for (const NotificationItem& n : items_) {
        if (n.id == id) return &n;
    }
    return nullptr;
}

size_t NotificationService::Unread() const {
    return static_cast<size_t>(std::count_if(items_.begin(), items_.end(), [](const NotificationItem& n) { return n.unread; }));
}

size_t NotificationService::UnreadFor(const AppKey& key) const {
    if (key.id.empty()) return 0;
    return static_cast<size_t>(
        std::count_if(items_.begin(), items_.end(), [&](const NotificationItem& n) { return n.unread && n.key == key; }));
}

uint64_t NotificationService::Polls() const { return shared_ ? shared_->polls.load() : 0; }

LONGLONG NotificationService::LastPollAgeMs() const {
    if (!shared_) return -1;
    const ULONGLONG last = shared_->lastPoll.load();
    return last ? static_cast<LONGLONG>(GetTickCount64() - last) : -1;
}

int NotificationService::IntervalMs() const { return shared_ ? shared_->interval.load() : 0; }

uint64_t NotificationService::WorkerCpuUs() const { return shared_ ? shared_->cpuUs.load() : 0; }

namespace {

// The text lines of a toast's generic binding: the first is the title, the rest the body.
void ReadText(const wun::UserNotification& n, NotificationItem& out) {
    try {
        const auto binding = n.Notification().Visual().GetBinding(wun::KnownNotificationBindings::ToastGeneric());
        if (!binding) return;
        bool first = true;
        for (const auto& te : binding.GetTextElements()) {
            std::wstring text(te.Text());
            if (first) {
                out.title = std::move(text);
                first = false;
            } else if (!text.empty()) {
                if (!out.body.empty()) out.body += L"\n";
                out.body += text;
            }
        }
    } catch (...) {
    }
}

}  // namespace

DWORD WINAPI NotificationService::ThreadMain(void* param) {
    std::shared_ptr<Shared> s = std::move(*static_cast<std::shared_ptr<Shared>*>(param));
    delete static_cast<std::shared_ptr<Shared>*>(param);
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
        return 1;
    }
    {
        auto post = [&](std::unique_ptr<Change> c) {
            Change* raw = c.release();
            if (!PostMessageW(s->owner, kMessage, static_cast<WPARAM>(s->generation), reinterpret_cast<LPARAM>(raw))) {
                delete raw;
            }
        };
        Listener listener{nullptr};
        try {
            listener = Listener::Current();
        } catch (...) {
            listener = nullptr;
        }
        std::set<uint32_t> known;    // the ids the UI thread has
        std::set<uint32_t> ignored;  // ids the test filter left out (not read again)
        bool haveState = false;
        State lastState = State::Polling;
        ULONGLONG lastChange = GetTickCount64();
        for (;;) {
            // Dismissals first (the poll below then sees them gone).
            std::vector<uint32_t> dismiss;
            AcquireSRWLockExclusive(&s->lock);
            dismiss.swap(s->dismiss);
            ReleaseSRWLockExclusive(&s->lock);
            if (listener) {
                for (uint32_t id : dismiss) {
                    try {
                        listener.RemoveNotification(id);
                    } catch (...) {
                    }
                    known.erase(id);  // still there after all: listed again by the poll below
                }
            }

            auto c = std::make_unique<Change>();
            Access access = Access::Unspecified;
            if (!listener) {
                c->state = State::Denied;
                c->reason = L"Windows' notification listener is not available on this system.";
            } else if (s->simulateDenied) {
                c->state = State::Denied;
                c->reason = L"Access to notifications is denied (simulated: WINDOCK_NOTIFY_DENIED).";
            } else {
                try {
                    access = listener.GetAccessStatus();
                } catch (...) {
                    access = Access::Unspecified;
                }
                if (access == Access::Allowed) {
                    c->state = State::Polling;
                } else {
                    c->state = State::Denied;
                    c->reason = access == Access::Denied
                                    ? L"Access to notifications is denied: Settings > Privacy & security > Notifications "
                                      L"(\"Let apps access your notifications\")."
                                    : L"Access to notifications is not decided (Settings > Privacy & security > "
                                      L"Notifications).";
                }
            }
            bool changed = !haveState || c->state != lastState;
            if (changed && haveState) c->reset = true;
            if (c->state != State::Polling) {
                if (!known.empty()) {
                    c->reset = true;
                    changed = true;
                }
                known.clear();
            } else {
                std::set<uint32_t> now;
                try {
                    const auto list = listener.GetNotificationsAsync(wun::NotificationKinds::Toast).get();
                    for (const auto& n : list) {
                        uint32_t id = 0;
                        try {
                            id = n.Id();
                        } catch (...) {
                            continue;
                        }
                        if (known.count(id)) {
                            now.insert(id);
                            continue;
                        }
                        if (ignored.count(id)) continue;
                        NotificationItem item;
                        item.id = id;
                        try {
                            if (const auto ai = n.AppInfo()) {
                                item.aumid = ai.AppUserModelId();
                                try {
                                    item.app = ai.DisplayInfo().DisplayName();
                                } catch (...) {
                                }
                            }
                        } catch (...) {
                        }
                        // A test filter: the others are not listed at all (never touched).
                        if (!Passes(s->filter, item.aumid)) {
                            ignored.insert(id);
                            continue;
                        }
                        try {
                            item.time = UnixMs(n.CreationTime());
                        } catch (...) {
                            item.time = 0;
                        }
                        ReadText(n, item);
                        now.insert(id);
                        c->added.push_back(std::move(item));
                    }
                    for (uint32_t id : known) {
                        if (!now.count(id)) c->removed.push_back(id);
                    }
                    known = std::move(now);
                } catch (...) {
                    // The listener failed this time (it does now and then right after logon): try again next poll.
                }
                if (!c->added.empty() || !c->removed.empty()) changed = true;
            }
            haveState = true;
            lastState = c->state;
            const ULONGLONG tick = GetTickCount64();
            s->polls.fetch_add(1);
            s->lastPoll = tick;
            FILETIME created{}, exited{}, kernel{}, user{};
            if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
                const uint64_t k = (static_cast<uint64_t>(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime;
                const uint64_t u = (static_cast<uint64_t>(user.dwHighDateTime) << 32) | user.dwLowDateTime;
                s->cpuUs = (k + u) / 10;
            }
            if (changed) {
                lastChange = tick;
                post(std::move(c));
            }
            const int base = s->pollMs.load();
            const int wait = s->fast.load()                ? kFastMs
                             : tick - lastChange > kSlowAfterMs ? std::max(base, kSlowMs)
                                                                : base;
            s->interval = wait;
            HANDLE events[2] = {s->stop, s->work};
            const DWORD r = WaitForMultipleObjects(2, events, FALSE, static_cast<DWORD>(wait));
            if (r == WAIT_OBJECT_0 || r == WAIT_FAILED) break;
            if (r == WAIT_OBJECT_0 + 1) lastChange = GetTickCount64();  // asked for: back to pollS
        }
    }
    winrt::uninit_apartment();
    return 0;
}
