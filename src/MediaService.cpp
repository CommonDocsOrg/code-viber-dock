#include "MediaService.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <chrono>
#include <vector>

#include "WidgetRuntime.h"  // WidgetNowMs

namespace {

namespace wmc = winrt::Windows::Media::Control;
namespace wss = winrt::Windows::Storage::Streams;
using Manager = wmc::GlobalSystemMediaTransportControlsSessionManager;
using Session = wmc::GlobalSystemMediaTransportControlsSession;

constexpr uint64_t kMaxArtBytes = 8u << 20;
constexpr DWORD kSettleMs = 60;  // a track change raises several events in a row: one snapshot

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
        winrt::event_token sessionsToken{}, currentToken{};
        if (manager) {
            try {
                sessionsToken = manager.SessionsChanged([s](Manager const&, wmc::SessionsChangedEventArgs const&) { SetEvent(s->work); });
                currentToken = manager.CurrentSessionChanged(
                    [s](Manager const&, wmc::CurrentSessionChangedEventArgs const&) { SetEvent(s->work); });
            } catch (...) {
            }
        }
        Session session{nullptr};
        winrt::event_token propsToken{}, playbackToken{};
        auto unbind = [&] {
            if (!session) return;
            try {
                session.MediaPropertiesChanged(propsToken);
                session.PlaybackInfoChanged(playbackToken);
            } catch (...) {
            }
            session = nullptr;
        };
        std::wstring artKey;
        std::shared_ptr<const std::string> art;
        uint64_t artSerial = 0, serials = 0;

        for (;;) {
            // Commands first (on the session they were meant for).
            std::deque<Command> commands;
            AcquireSRWLockExclusive(&s->lock);
            commands.swap(s->commands);
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
            // The current session; its events follow it.
            Session current{nullptr};
            if (manager) {
                try {
                    current = manager.GetCurrentSession();
                } catch (...) {
                    current = nullptr;
                }
            }
            if (current != session) {
                unbind();
                session = current;
                if (session) {
                    try {
                        propsToken = session.MediaPropertiesChanged(
                            [s](Session const&, wmc::MediaPropertiesChangedEventArgs const&) { SetEvent(s->work); });
                        playbackToken = session.PlaybackInfoChanged(
                            [s](Session const&, wmc::PlaybackInfoChangedEventArgs const&) { SetEvent(s->work); });
                    } catch (...) {
                    }
                }
            }
            auto st = std::make_unique<State>();
            st->available = manager != nullptr;
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
                        }
                    }
                    if (const auto tl = session.GetTimelineProperties()) {
                        const double end = Seconds(tl.EndTime() - tl.StartTime());
                        if (end > 0.0) {
                            st->duration = end;
                            double pos = Seconds(tl.Position() - tl.StartTime());
                            // The position was reported at LastUpdatedTime: carry it to now.
                            if (st->status == Status::Playing) {
                                const auto age = winrt::clock::now() - tl.LastUpdatedTime();
                                const double ageS = std::chrono::duration<double>(age).count();
                                if (ageS > 0.0 && ageS < 86400.0) pos += ageS;
                            }
                            st->position = std::clamp(pos, 0.0, end);
                        }
                    }
                    // The thumbnail: read when the track changes (or arrives after its title).
                    const std::wstring key = st->app + L"\n" + st->title + L"\n" + st->artist + L"\n" + st->album;
                    const bool newTrack = key != artKey;
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
            st->sampledAt = WidgetNowMs();
            State* raw = st.release();
            if (!PostMessageW(s->owner, kMessage, static_cast<WPARAM>(s->generation), reinterpret_cast<LPARAM>(raw))) {
                delete raw;
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
