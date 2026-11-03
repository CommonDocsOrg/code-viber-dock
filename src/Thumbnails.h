#pragma once

#include <windows.h>
#include <d2d1_1.h>

#include <winrt/base.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "Config.h"

class Renderer;

// Window thumbnails for a dock's tile slots (docks[].thumbnails), drawn by the dock from one
// bitmap per window (a resource of the dock's D2D device, at most the tile's magnified pixel size,
// the window's aspect kept).
//
//  - Live: one Windows.Graphics.Capture session per window (GraphicsCaptureItem from the HWND via
//    IGraphicsCaptureItemInterop, a one-buffer Direct3D11CaptureFramePool on the dock's own D3D
//    device, no capture border, no cursor, MinUpdateInterval = 1 / fps). The pool is not
//    free-threaded: frames arrive on the UI thread through a DispatcherQueue (created once for the
//    thread), where each is scaled into the window's bitmap with D2D and released. DWM delivers a
//    frame only when the window's content changed, and at most fps a second. Sessions run only
//    while the dock is visible (Activate(false) closes them; the last frames stay).
//  - Snapshots: a repeating timer (intervalS) on the dock window starts a worker thread that takes
//    PrintWindow(PW_RENDERFULLCONTENT) of every window that is not minimized (nor hung), scales it
//    (HALFTONE) and posts the pixels back; the UI thread makes the bitmaps. Nothing else runs in
//    between. The timer only exists while the dock is visible and has tiles.
//
// Minimized windows keep their last frame (the dock draws it dimmed): capture delivers no frames
// for them and PrintWindow would show the minimized caption.
class Thumbnails {
public:
    static constexpr UINT_PTR kTimerId = 22;             // on the dock window (snapshot interval)
    static constexpr UINT kSnapshotMessage = WM_APP + 27;  // a worker's result (lParam: owned job)
    // Posted to the dock: start / stop the live sessions (COM calls out of the capture objects fail
    // inside another process's SendMessage, e.g. automation, so they never run there).
    static constexpr UINT kLiveMessage = WM_APP + 29;

    Thumbnails();
    ~Thumbnails();
    Thumbnails(const Thumbnails&) = delete;
    Thumbnails& operator=(const Thumbnails&) = delete;

    // `changed`: these windows got a new frame (the dock redraws their tiles).
    void Init(HWND dock, Renderer* renderer, std::function<void(const std::vector<HWND>&)> changed);
    // The mode and its settings; Off releases everything.
    void Configure(ThumbnailMode mode, int fps, int intervalS);
    // The windows that have tiles now and the bitmap size a frame is fitted into (px). Entries of
    // other windows are released (their capture closed).
    void SetWindows(const std::vector<HWND>& windows, UINT width, UINT height);
    // The dock is visible (shown / showing): live sessions and the snapshot timer run only then.
    void Activate(bool visible);
    // Device loss / the dock window goes: every session, bitmap and pending job is dropped.
    void Reset();

    // The window's latest frame (null: none yet).
    ID2D1Bitmap* FrameOf(HWND window) const;
    // Dock messages: the snapshot timer and results. True if consumed.
    bool HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

    ThumbnailMode Mode() const { return mode_; }
    size_t Sessions() const;                 // live capture sessions running
    uint64_t LiveFrames() const { return liveFrames_; }
    uint64_t Rounds() const { return rounds_; }
    HRESULT LastError() const { return lastError_; }  // the last capture start failure (S_OK: none)
    static uint64_t PrintCalls();            // PrintWindow calls (process)
    struct Job;  // a snapshot round (worker thread -> kSnapshotMessage)

private:
    struct Entry;
    Entry* Find(HWND window) const;
    bool EnsureDevice();
    void ScheduleLive();
    void ApplyLive();
    void StartLive(Entry& e);
    void StopLive(Entry& e);
    void OnFrame(HWND window);
    void UpdateTimer();
    // Starts a snapshot worker for these windows (all entries when empty); false if one runs.
    bool StartRound(const std::vector<HWND>& only);
    void OnSnapshots(Job* job);
    void Store(Entry& e, ID2D1Bitmap1* bitmap);

    HWND dock_ = nullptr;
    Renderer* renderer_ = nullptr;
    std::function<void(const std::vector<HWND>&)> changed_;
    ThumbnailMode mode_ = ThumbnailMode::Off;
    int fps_ = 10;
    int intervalS_ = 5;
    UINT width_ = 0, height_ = 0;
    bool visible_ = false;
    bool timer_ = false;
    bool roundRunning_ = false;
    bool livePosted_ = false;
    uint64_t generation_ = 1;  // a snapshot result from an older generation is dropped
    std::vector<std::unique_ptr<Entry>> entries_;
    struct Device;
    std::unique_ptr<Device> device_;  // the capture device (the dock's D3D device as WinRT)
    uint64_t liveFrames_ = 0;
    uint64_t rounds_ = 0;
    HRESULT lastError_ = S_OK;
};
