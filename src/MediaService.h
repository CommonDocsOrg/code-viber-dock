#pragma once

#include <windows.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>

// The `media` provider of the widget API (permission "media"): the system's current media session
// (GlobalSystemMediaTransportControlsSessionManager: Spotify, browsers, the Media Player app ...),
// its title / artist / album / playback state / timeline / thumbnail, and play / pause / next /
// previous. Every WinRT call runs on one worker thread of its own (MTA, C++/WinRT, blocking .get()
// calls are fine there): it waits on an event that the session manager's and the current session's
// change events set (SessionsChanged, CurrentSessionChanged, MediaPropertiesChanged,
// PlaybackInfoChanged) and on commands, re-reads the session, and posts a snapshot to the owner
// window (kMessage). Nothing polls; widgets extrapolate the position while playing. Round 23 phase
// 23A: the session's TimelinePropertiesChanged is followed too, but cheaply: such a wake (with no
// other event) reads only the timeline and posts a snapshot only when the position jumped from
// where the last snapshot's extrapolation puts it (a seek, an app-side jump: more than 1.5 s) or
// the duration changed, so an app that reports its timeline often (some do, every few seconds
// while playing) costs a worker wake and no UI work. Seek (media.seek): TryChangePlaybackPosition-
// Async; until the app reports a timeline newer than the seek, the snapshots carry the seek's
// target (the stale timeline would put the position back). The thumbnail's bytes are read only
// when the track (app + title + artist + album) changes. Started by WidgetRuntime when the first
// widget uses `media`, stopped when the last such widget goes.
class MediaService {
public:
    static constexpr UINT kMessage = WM_APP + 34;  // wParam: generation, lParam: State* (owned by the receiver)

    enum class Status { None = 0, Stopped = 1, Paused = 2, Playing = 3 };
    struct State {
        bool available = false;  // the session manager could be created
        bool session = false;    // there is a current session
        std::wstring title, artist, album;
        std::wstring app;        // the session's SourceAppUserModelId ("Spotify.exe", "Microsoft.ZuneMusic_...!...")
        Status status = Status::None;
        double position = -1.0, duration = -1.0;  // seconds (-1: not reported)
        double sampledAt = 0.0;  // WidgetNowMs when position was read
        bool canPlay = false, canPause = false, canToggle = false, canNext = false, canPrevious = false;
        bool canSeek = false;  // round 23 phase 23A: the session's IsPlaybackPositionEnabled
        uint64_t artSerial = 0;  // 0: no art; changes with the art
        std::shared_ptr<const std::string> art;  // the thumbnail's encoded bytes (PNG / JPEG)
    };
    enum class Command { Play, Pause, Toggle, Next, Previous };

    MediaService() = default;
    MediaService(const MediaService&) = delete;
    MediaService& operator=(const MediaService&) = delete;
    ~MediaService() { Stop(); }

    // Starts the worker; the first snapshot follows shortly (State::available false until then).
    bool Start(HWND owner);
    // Stops the worker (waits up to 2 s for it).
    void Stop();
    bool Active() const { return thread_ != nullptr; }
    const State& Get() const { return state_; }
    // The position now (extrapolated while playing), seconds; -1 unknown.
    double PositionNow() const;
    void Send(Command c);
    // Round 23 phase 23A: the current session to `seconds` from the track's start (clamped to the
    // duration; ignored when it cannot seek). The snapshot here takes the target at once (the
    // caller redraws; no change callback from inside its call), the worker sends it (the latest
    // of a burst wins).
    void Seek(double seconds);
    // kMessage: takes the snapshot, then calls the changed callback. True if handled.
    bool HandleMessage(UINT msg, WPARAM wp, LPARAM lp);
    void SetChanged(std::function<void()> changed) { changed_ = std::move(changed); }
    // Snapshots received since the process started (automation).
    uint64_t Updates() const { return updates_; }

private:
    struct Shared;  // what the worker and the UI thread share (commands, events, the owner)
    static DWORD WINAPI ThreadMain(void* param);

    HANDLE thread_ = nullptr;
    std::shared_ptr<Shared> shared_;
    uint64_t generation_ = 0;
    State state_;
    std::function<void()> changed_;
    uint64_t updates_ = 0;
};
