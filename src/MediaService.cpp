#include "MediaService.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <vector>

#include "WidgetRuntime.h"  // WidgetNowMs

namespace {

namespace wmc = winrt::Windows::Media::Control;
namespace wss = winrt::Windows::Storage::Streams;
using Manager = wmc::GlobalSystemMediaTransportControlsSessionManager;
using Session = wmc::GlobalSystemMediaTransportControlsSession;

constexpr uint64_t kMaxArtBytes = 8u << 20;
constexpr DWORD kSettleMs = 60;  // a track change raises several events in a row: one snapshot
// Round 23 phase 23A: a timeline report this far (seconds) from the extrapolated position is a
// jump (a seek, the app moved); closer, it is the same playback reported again: no snapshot.
constexpr double kJumpS = 1.5;

double Seconds(winrt::Windows::Foundation::TimeSpan t) {
    return std::chrono::duration<double>(t).count();
}

}  // namespace

struct MediaService::Shared {
    HWND owner = nullptr;
    uint64_t generation = 0;
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);   // manual reset: the worker ends
    HANDLE work = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto reset: something changed / a command
    SRWLOCK lock = SRWLOCK_INIT;
    std::deque<Command> commands;
    double seekTo = -1.0;  // round 23 phase 23A: a seek waiting for the worker (seconds; -1 none)
    // Round 23 phase 23A: something other than the timeline changed (or a command / a seek is
    // queued): the next wake re-reads everything. Without it a wake is a timeline event only.
    std::atomic<bool> full{true};
    ~Shared() {
        if (stop) CloseHandle(stop);
        if (work) CloseHandle(work);
    }
};

bool MediaService::Start(HWND owner) {
    if (Active()) return true;
    auto shared = std::make_shared<Shared>();
    if (!shared->stop || !shared->work) return false;
    shared->owner = owner;
    shared->generation = ++generation_;
    auto* param = new std::shared_ptr<Shared>(shared);
    HANDLE t = CreateThread(nullptr, 0, &MediaService::ThreadMain, param, 0, nullptr);
    if (!t) {
        delete param;
        return false;
    }
    thread_ = t;
    shared_ = std::move(shared);
    return true;
}

void MediaService::Stop() {
    if (!thread_) return;
    SetEvent(shared_->stop);
    // A WinRT call in progress finishes first; past 2 s the thread is left to end by itself (its
    // last snapshot carries an old generation and is dropped).
    WaitForSingleObject(thread_, 2000);
    CloseHandle(thread_);
    thread_ = nullptr;
    shared_.reset();
    ++generation_;
    state_ = State{};
}

void MediaService::Send(Command c) {
    if (!shared_) return;
    AcquireSRWLockExclusive(&shared_->lock);
    if (shared_->commands.size() < 16) shared_->commands.push_back(c);
    ReleaseSRWLockExclusive(&shared_->lock);
    shared_->full = true;
    SetEvent(shared_->work);
}

void MediaService::Seek(double seconds) {
    if (!shared_ || !state_.session || !state_.canSeek || state_.duration <= 0.0 || !std::isfinite(seconds)) return;
    const double to = std::clamp(seconds, 0.0, state_.duration);
    state_.position = to;
    state_.sampledAt = WidgetNowMs();
    AcquireSRWLockExclusive(&shared_->lock);
    shared_->seekTo = to;
    ReleaseSRWLockExclusive(&shared_->lock);
    shared_->full = true;
    SetEvent(shared_->work);
}

double MediaService::PositionNow() const {
    if (state_.position < 0.0) return -1.0;
    double p = state_.position;
    if (state_.status == Status::Playing) p += (WidgetNowMs() - state_.sampledAt) / 1000.0;
    if (state_.duration > 0.0) p = std::min(p, state_.duration);
    return p;
}

bool MediaService::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg != kMessage) return false;
    std::unique_ptr<State> st(reinterpret_cast<State*>(lp));
    if (!st || !thread_ || static_cast<uint64_t>(wp) != generation_) return true;  // an old worker's
    state_ = std::move(*st);
    ++updates_;
    if (changed_) changed_();
    return true;
}

DWORD WINAPI MediaService::ThreadMain(void* param) {
    std::shared_ptr<Shared> s = std::move(*static_cast<std::shared_ptr<Shared>*>(param));
    delete static_cast<std::shared_ptr<Shared>*>(param);
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
        return 1;
    }
    {
        Manager manager{nullptr};
        try {
            manager = Manager::RequestAsync().get();
        } catch (...) {
            manager = nullptr;
        }
        // Every event but the timeline's asks for a full re-read.
        auto wakeFull = [s] {
            s->full = true;
            SetEvent(s->work);
        };
        winrt::event_token sessionsToken{}, currentToken{};
        if (manager) {
            try {
                sessionsToken = manager.SessionsChanged([wakeFull](Manager const&, wmc::SessionsChangedEventArgs const&) { wakeFull(); });
                currentToken = manager.CurrentSessionChanged(
                    [wakeFull](Manager const&, wmc::CurrentSessionChangedEventArgs const&) { wakeFull(); });
            } catch (...) {
            }
        }
        Session session{nullptr};
        winrt::event_token propsToken{}, playbackToken{}, timelineToken{};
        auto unbind = [&] {
            if (!session) return;
            try {
                session.MediaPropertiesChanged(propsToken);
                session.PlaybackInfoChanged(playbackToken);
                session.TimelinePropertiesChanged(timelineToken);
            } catch (...) {
            }
            session = nullptr;
        };
        std::wstring artKey;
        std::shared_ptr<const std::string> art;
        uint64_t artSerial = 0, serials = 0;
        // Round 23 phase 23A: the last snapshot posted (a timeline-only wake compares with it) and
        // the last seek sent (its target stands in for a timeline older than it: right after
        // TryChangePlaybackPositionAsync the session still reports the old position until the app
        // updates it, which would put the bar back for a moment).
        State last;
        bool posted = false;
        bool seekHeld = false;
        double seekTarget = 0.0;
        winrt::clock::time_point seekAt{};
        // The session's position / duration (seconds); false: no timeline reported.
        auto readTimeline = [&](Status status, double& pos, double& dur) -> bool {
            const auto tl = session.GetTimelineProperties();
            if (!tl) return false;
            const double end = Seconds(tl.EndTime() - tl.StartTime());
            if (end <= 0.0) return false;
            dur = end;
            const auto now = winrt::clock::now();
            if (seekHeld && tl.LastUpdatedTime() < seekAt) {
                pos = seekTarget;
                if (status == Status::Playing) pos += std::chrono::duration<double>(now - seekAt).count();
            } else {
                seekHeld = false;
                pos = Seconds(tl.Position() - tl.StartTime());
                // The position was reported at LastUpdatedTime: carry it to now.
                if (status == Status::Playing) {
                    const double ageS = std::chrono::duration<double>(now - tl.LastUpdatedTime()).count();
                    if (ageS > 0.0 && ageS < 86400.0) pos += ageS;
                }
            }
            pos = std::clamp(pos, 0.0, end);
            return true;
        };
        auto post = [&](std::unique_ptr<State> st) {
            last = *st;
            posted = true;
            State* raw = st.release();
            if (!PostMessageW(s->owner, kMessage, static_cast<WPARAM>(s->generation), reinterpret_cast<LPARAM>(raw))) {
                delete raw;
            }
        };

        for (;;) {
            const bool full = s->full.exchange(false);
            // Commands first (on the session they were meant for).
            std::deque<Command> commands;
            double seekTo = -1.0;
            AcquireSRWLockExclusive(&s->lock);
            commands.swap(s->commands);
            std::swap(seekTo, s->seekTo);
            ReleaseSRWLockExclusive(&s->lock);
            for (Command c : commands) {
                if (!session) break;
                try {
                    switch (c) {
                        case Command::Play: session.TryPlayAsync().get(); break;
                        case Command::Pause: session.TryPauseAsync().get(); break;
                        case Command::Toggle: session.TryTogglePlayPauseAsync().get(); break;
                        case Command::Next: session.TrySkipNextAsync().get(); break;
                        case Command::Previous: session.TrySkipPreviousAsync().get(); break;
                    }
                } catch (...) {
                }
            }
            if (seekTo >= 0.0 && session) {
                // Round 23 phase 23A: the requested position is on the timeline's own scale (from
                // StartTime, in 100 ns ticks).
                try {
                    const auto tl = session.GetTimelineProperties();
                    const winrt::Windows::Foundation::TimeSpan start = tl ? tl.StartTime() : winrt::Windows::Foundation::TimeSpan{};
                    const auto to = start + std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(
                                                std::chrono::duration<double>(seekTo));
                    if (session.TryChangePlaybackPositionAsync(to.count()).get()) {
                        seekHeld = true;
                        seekTarget = seekTo;
                        seekAt = winrt::clock::now();
                    }
                } catch (...) {
                }
            }
            // The current session; its events follow it.
            Session current{nullptr};
            if (manager) {
                try {
                    current = manager.GetCurrentSession();
                } catch (...) {
                    current = nullptr;
                }
            }
            const bool sameSession = current == session;
            if (!sameSession) {
                unbind();
                session = current;
                seekHeld = false;
                if (session) {
                    try {
                        propsToken = session.MediaPropertiesChanged(
                            [wakeFull](Session const&, wmc::MediaPropertiesChangedEventArgs const&) { wakeFull(); });
                        playbackToken = session.PlaybackInfoChanged(
                            [wakeFull](Session const&, wmc::PlaybackInfoChangedEventArgs const&) { wakeFull(); });
                        // Round 23 phase 23A: a timeline report only wakes the worker (see below).
                        timelineToken = session.TimelinePropertiesChanged(
                            [s](Session const&, wmc::TimelinePropertiesChangedEventArgs const&) { SetEvent(s->work); });
                    } catch (...) {
                    }
                }
            }
            if (!full && sameSession && session && posted && last.session) {
                // Round 23 phase 23A: a timeline event alone: the timeline only, posted when it jumped.
                try {
                    double pos = -1.0, dur = -1.0;
                    if (readTimeline(last.status, pos, dur)) {
                        double expected = -1.0;
                        if (last.position >= 0.0) {
                            expected = last.position;
                            if (last.status == Status::Playing) expected += (WidgetNowMs() - last.sampledAt) / 1000.0;
                            expected = std::min(expected, dur);
                        }
                        if (expected < 0.0 || std::fabs(pos - expected) > kJumpS || std::fabs(dur - last.duration) > 0.5) {
                            auto st = std::make_unique<State>(last);
                            st->position = pos;
                            st->duration = dur;
                            st->sampledAt = WidgetNowMs();
                            post(std::move(st));
                        }
                    }
                } catch (...) {
                }
            } else {
                auto st = std::make_unique<State>();
                st->available = manager != nullptr;
                st->sampledAt = WidgetNowMs();
                if (session) {
                    try {
                        st->session = true;
                        st->app = session.SourceAppUserModelId();
                        wmc::GlobalSystemMediaTransportControlsSessionMediaProperties props{nullptr};
                        try {
                            props = session.TryGetMediaPropertiesAsync().get();
                        } catch (...) {
                            props = nullptr;
                        }
                        if (props) {
                            st->title = props.Title();
                            st->artist = props.Artist();
                            st->album = props.AlbumTitle();
                        }
                        const std::wstring key = st->app + L"\n" + st->title + L"\n" + st->artist + L"\n" + st->album;
                        const bool newTrack = key != artKey;
                        if (newTrack) seekHeld = false;  // a seek of the previous track says nothing here
                        if (const auto pb = session.GetPlaybackInfo()) {
                            using PS = wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus;
                            switch (pb.PlaybackStatus()) {
                                case PS::Playing: st->status = Status::Playing; break;
                                case PS::Paused: st->status = Status::Paused; break;
                                default: st->status = Status::Stopped; break;
                            }
                            if (const auto c = pb.Controls()) {
                                st->canPlay = c.IsPlayEnabled();
                                st->canPause = c.IsPauseEnabled();
                                st->canToggle = c.IsPlayPauseToggleEnabled();
                                st->canNext = c.IsNextEnabled();
                                st->canPrevious = c.IsPreviousEnabled();
                                st->canSeek = c.IsPlaybackPositionEnabled();
                            }
                        }
                        double pos = -1.0, dur = -1.0;
                        if (readTimeline(st->status, pos, dur)) {
                            st->duration = dur;
                            st->position = pos;
                        }
                        st->sampledAt = WidgetNowMs();  // before the thumbnail's read (it can take a while)
                        // The thumbnail: read when the track changes (or arrives after its title).
                        if (newTrack) {
                            artKey = key;
                            art.reset();
                            artSerial = 0;
                        }
                        if ((newTrack || !art) && props) {
                            if (const auto ref = props.Thumbnail()) {
                                try {
                                    const auto stream = ref.OpenReadAsync().get();
                                    const uint64_t size = stream.Size();
                                    if (size > 0 && size <= kMaxArtBytes) {
                                        wss::Buffer buffer(static_cast<uint32_t>(size));
                                        const auto read =
                                            stream.ReadAsync(buffer, static_cast<uint32_t>(size), wss::InputStreamOptions::None).get();
                                        if (read && read.Length() > 0) {
                                            art = std::make_shared<const std::string>(reinterpret_cast<const char*>(read.data()),
                                                                                      read.Length());
                                            artSerial = ++serials;
                                        }
                                    }
                                } catch (...) {
                                }
                            }
                        }
                        st->art = art;
                        st->artSerial = art ? artSerial : 0;
                    } catch (...) {
                    }
                }
                post(std::move(st));
            }
            HANDLE events[2] = {s->stop, s->work};
            if (WaitForMultipleObjects(2, events, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) break;
            if (WaitForSingleObject(s->stop, kSettleMs) == WAIT_OBJECT_0) break;
        }
        unbind();
        if (manager) {
            try {
                manager.SessionsChanged(sessionsToken);
                manager.CurrentSessionChanged(currentToken);
            } catch (...) {
            }
        }
    }
    winrt::uninit_apartment();
    return 0;
}
