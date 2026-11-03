#include "Thumbnails.h"

#include <DispatcherQueue.h>
#include <dwmapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.System.h>

#include <algorithm>
#include <cmath>

#include "Renderer.h"

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;
namespace wgd3 = winrt::Windows::Graphics::DirectX::Direct3D11;

namespace {

std::atomic<uint64_t> g_printCalls{0};

// The capture frame pools are not free-threaded: their frames arrive through the UI thread's
// DispatcherQueue (one per thread, pumped by the ordinary message loop; created once and kept
// for the life of the process).
bool EnsureDispatcherQueue() {
    static bool ready = false;
    if (ready) return true;
    if (winrt::Windows::System::DispatcherQueue::GetForCurrentThread()) {
        ready = true;
        return true;
    }
    DispatcherQueueOptions options{sizeof(options), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE};
    PDISPATCHERQUEUECONTROLLER controller = nullptr;
    if (FAILED(CreateDispatcherQueueController(options, &controller)) || !controller) return false;
    ready = true;  // `controller` is kept on purpose: the queue must outlive every capture session
    return true;
}

// Unpackaged apps may capture without the yellow border (GraphicsCaptureAccessKind::Borderless);
// asked once, without waiting (the session's IsBorderRequired(false) does the rest).
void RequestBorderless() {
    static bool asked = false;
    if (asked) return;
    asked = true;
    try {
        if (winrt::Windows::Foundation::Metadata::ApiInformation::IsTypePresent(
                L"Windows.Graphics.Capture.GraphicsCaptureAccess")) {
            static auto pending = wgc::GraphicsCaptureAccess::RequestAccessAsync(wgc::GraphicsCaptureAccessKind::Borderless);
        }
    } catch (...) {
    }
}

// The window's visible frame inside its window rect (no invisible resize borders).
RECT VisibleFrame(HWND window, const RECT& windowRect) {
    RECT frame{};
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))) ||
        frame.right <= frame.left || frame.bottom <= frame.top) {
        frame = windowRect;
    }
    RECT r{};
    IntersectRect(&r, &frame, &windowRect);
    OffsetRect(&r, -windowRect.left, -windowRect.top);
    return r;
}

// Fits w x h into maxW x maxH (never enlarged).
SIZE Fit(LONG w, LONG h, UINT maxW, UINT maxH) {
    const double s = std::min({static_cast<double>(maxW) / std::max<LONG>(w, 1), static_cast<double>(maxH) / std::max<LONG>(h, 1), 1.0});
    return SIZE{std::max<LONG>(1, std::lround(w * s)), std::max<LONG>(1, std::lround(h * s))};
}

}  // namespace

struct Thumbnails::Device {
    wgd3::IDirect3DDevice device{nullptr};
};

struct Thumbnails::Entry {
    HWND window = nullptr;
    winrt::com_ptr<ID2D1Bitmap1> bitmap;  // the latest frame (fitted into the tile size)
    wgc::GraphicsCaptureItem item{nullptr};
    wgc::Direct3D11CaptureFramePool pool{nullptr};
    wgc::GraphicsCaptureSession session{nullptr};
    winrt::event_token token{};
    winrt::Windows::Graphics::SizeInt32 poolSize{};
    bool failed = false;  // capture could not start for this window (not retried while it has a tile)
};

struct Thumbnails::Job {
    HWND dock = nullptr;
    uint64_t generation = 0;
    UINT width = 0, height = 0;
    std::vector<HWND> windows;
    struct Shot {
        HWND window = nullptr;
        UINT w = 0, h = 0;
        std::vector<uint32_t> px;  // BGRA, opaque
    };
    std::vector<Shot> shots;
};

namespace {

// Snapshot worker: PrintWindow of each window that can be drawn now, scaled down with HALFTONE.
DWORD WINAPI SnapshotThread(void* param) {
    auto* job = static_cast<Thumbnails::Job*>(param);
    HDC screen = GetDC(nullptr);
    for (HWND window : job->windows) {
        if (!IsWindow(window) || !IsWindowVisible(window) || IsIconic(window) || IsHungAppWindow(window)) continue;
        RECT wr{};
        if (!GetWindowRect(window, &wr)) continue;
        const LONG w = wr.right - wr.left, h = wr.bottom - wr.top;
        if (w <= 0 || h <= 0 || w > 16384 || h > 16384) continue;
        const RECT frame = VisibleFrame(window, wr);
        const LONG fw = frame.right - frame.left, fh = frame.bottom - frame.top;
        if (fw <= 0 || fh <= 0) continue;
        const SIZE out = Fit(fw, fh, job->width, job->height);

        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HDC full = CreateCompatibleDC(screen);
        HBITMAP fullDib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        BITMAPINFO bo = bi;
        bo.bmiHeader.biWidth = out.cx;
        bo.bmiHeader.biHeight = -out.cy;
        void* outBits = nullptr;
        HDC scaled = CreateCompatibleDC(screen);
        HBITMAP outDib = CreateDIBSection(screen, &bo, DIB_RGB_COLORS, &outBits, nullptr, 0);
        if (full && fullDib && scaled && outDib) {
            HGDIOBJ oldFull = SelectObject(full, fullDib);
            HGDIOBJ oldOut = SelectObject(scaled, outDib);
            const BOOL printed = PrintWindow(window, full, PW_RENDERFULLCONTENT);
            g_printCalls.fetch_add(1, std::memory_order_relaxed);
            if (printed) {
                SetStretchBltMode(scaled, HALFTONE);
                SetBrushOrgEx(scaled, 0, 0, nullptr);
                StretchBlt(scaled, 0, 0, out.cx, out.cy, full, frame.left, frame.top, fw, fh, SRCCOPY);
                GdiFlush();
                Thumbnails::Job::Shot shot;
                shot.window = window;
                shot.w = static_cast<UINT>(out.cx);
                shot.h = static_cast<UINT>(out.cy);
                const auto* src = static_cast<const uint32_t*>(outBits);
                shot.px.assign(src, src + static_cast<size_t>(out.cx) * static_cast<size_t>(out.cy));
                for (uint32_t& p : shot.px) p |= 0xFF000000u;  // GDI leaves alpha 0: opaque
                job->shots.push_back(std::move(shot));
            }
            SelectObject(full, oldFull);
            SelectObject(scaled, oldOut);
        }
        if (outDib) DeleteObject(outDib);
        if (fullDib) DeleteObject(fullDib);
        if (scaled) DeleteDC(scaled);
        if (full) DeleteDC(full);
    }
    ReleaseDC(nullptr, screen);
    if (!PostMessageW(job->dock, Thumbnails::kSnapshotMessage, 0, reinterpret_cast<LPARAM>(job))) delete job;
    return 0;
}

}  // namespace

Thumbnails::Thumbnails() = default;
Thumbnails::~Thumbnails() { Reset(); }

uint64_t Thumbnails::PrintCalls() { return g_printCalls.load(std::memory_order_relaxed); }

void Thumbnails::Init(HWND dock, Renderer* renderer, std::function<void(const std::vector<HWND>&)> changed) {
    dock_ = dock;
    renderer_ = renderer;
    changed_ = std::move(changed);
}

Thumbnails::Entry* Thumbnails::Find(HWND window) const {
    for (const auto& e : entries_) {
        if (e->window == window) return e.get();
    }
    return nullptr;
}

ID2D1Bitmap* Thumbnails::FrameOf(HWND window) const {
    const Entry* e = window ? Find(window) : nullptr;
    return e ? e->bitmap.get() : nullptr;
}

size_t Thumbnails::Sessions() const {
    return static_cast<size_t>(std::count_if(entries_.begin(), entries_.end(), [](const auto& e) { return e->session != nullptr; }));
}

void Thumbnails::Configure(ThumbnailMode mode, int fps, int intervalS) {
    const bool restartLive = mode != mode_ || (mode == ThumbnailMode::Live && fps != fps_);
    const bool restartTimer = intervalS != intervalS_;
    if (mode != mode_) {
        ++generation_;  // a snapshot round in flight no longer applies
        roundRunning_ = false;
    }
    if (restartLive) {
        for (auto& e : entries_) StopLive(*e);
    }
    mode_ = mode;
    fps_ = std::clamp(fps, 1, 60);
    intervalS_ = std::clamp(intervalS, 1, 3600);
    if (mode_ == ThumbnailMode::Off) {
        entries_.clear();
        device_.reset();
    }
    if (restartTimer && timer_ && dock_) {
        KillTimer(dock_, kTimerId);
        timer_ = false;
    }
    if (restartLive && mode_ == ThumbnailMode::Live) {
        for (auto& e : entries_) e->failed = false;
        ScheduleLive();
    }
    UpdateTimer();
}

void Thumbnails::SetWindows(const std::vector<HWND>& windows, UINT width, UINT height) {
    width_ = std::max<UINT>(width, 1);
    height_ = std::max<UINT>(height, 1);
    if (mode_ == ThumbnailMode::Off) return;
    // Closed / no longer shown windows: their capture goes with them.
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (std::find(windows.begin(), windows.end(), (*it)->window) == windows.end()) {
            StopLive(**it);
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
    std::vector<HWND> added;
    for (HWND w : windows) {
        if (!w || Find(w)) continue;
        auto e = std::make_unique<Entry>();
        e->window = w;
        entries_.push_back(std::move(e));
        added.push_back(w);
    }
    if (!added.empty() && mode_ == ThumbnailMode::Live) ScheduleLive();
    // A new tile gets its first snapshot now (later ones with everybody else, every interval).
    if (mode_ == ThumbnailMode::Snapshots && visible_ && timer_ && !added.empty()) StartRound(added);
    UpdateTimer();
}

void Thumbnails::Activate(bool visible) {
    if (visible == visible_) return;
    visible_ = visible;
    if (mode_ == ThumbnailMode::Live) {
        // Hidden: stop now (no more frames), unless inside another process's SendMessage, where
        // the capture objects cannot call out; starting always waits for the posted message.
        if (!visible && !InSendMessage()) {
            for (auto& e : entries_) StopLive(*e);
        }
        ScheduleLive();
    }
    UpdateTimer();
}

void Thumbnails::Reset() {
    for (auto& e : entries_) StopLive(*e);
    entries_.clear();
    device_.reset();
    if (timer_ && dock_) KillTimer(dock_, kTimerId);
    timer_ = false;
    ++generation_;
    roundRunning_ = false;
    // A posted kLiveMessage still arrives: ApplyLive then finds nothing to start.
}

bool Thumbnails::EnsureDevice() {
    if (device_) return true;
    if (!renderer_ || !renderer_->Dxgi()) return false;
    try {
        winrt::com_ptr<::IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(renderer_->Dxgi(), inspectable.put()));
        auto d = std::make_unique<Device>();
        d->device = inspectable.as<wgd3::IDirect3DDevice>();
        device_ = std::move(d);
        return true;
    } catch (...) {
        return false;
    }
}

void Thumbnails::ScheduleLive() {
    if (livePosted_ || !dock_) return;
    livePosted_ = PostMessageW(dock_, kLiveMessage, 0, 0) != FALSE;
}

void Thumbnails::ApplyLive() {
    livePosted_ = false;
    const bool run = mode_ == ThumbnailMode::Live && visible_;
    for (auto& e : entries_) {
        if (run) {
            StartLive(*e);
        } else {
            StopLive(*e);
        }
    }
}

void Thumbnails::StartLive(Entry& e) {
    if (e.session || e.failed || !visible_ || mode_ != ThumbnailMode::Live) return;
    if (!IsWindow(e.window) || !EnsureDevice() || !EnsureDispatcherQueue()) {
        lastError_ = !IsWindow(e.window) ? E_HANDLE : !device_ ? E_NOINTERFACE : E_NOT_VALID_STATE;
        e.failed = true;
        return;
    }
    RequestBorderless();
    try {
        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        wgc::GraphicsCaptureItem item{nullptr};
        winrt::check_hresult(
            interop->CreateForWindow(e.window, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item)));
        const auto size = item.Size();
        // One buffer: the frame is copied (scaled) and released at once.
        auto pool = wgc::Direct3D11CaptureFramePool::Create(device_->device, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                                            1, size);
        auto session = pool.CreateCaptureSession(item);
        try {
            session.IsBorderRequired(false);
        } catch (...) {
        }
        try {
            session.IsCursorCaptureEnabled(false);
        } catch (...) {
        }
        try {
            // At most fps frames a second (Windows 11 24H2+); DWM only sends changed content anyway.
            session.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan(10'000'000LL / fps_));
        } catch (...) {
        }
        const HWND window = e.window;
        e.token = pool.FrameArrived([this, window](wgc::Direct3D11CaptureFramePool const&, winrt::Windows::Foundation::IInspectable const&) {
            OnFrame(window);
        });
        e.item = item;
        e.pool = pool;
        e.session = session;
        e.poolSize = size;
        session.StartCapture();
    } catch (winrt::hresult_error const& err) {
        lastError_ = err.code().value;
        StopLive(e);
        e.failed = true;
    } catch (...) {
        lastError_ = E_FAIL;
        StopLive(e);
        e.failed = true;
    }
}

void Thumbnails::StopLive(Entry& e) {
    try {
        if (e.pool && e.token.value) e.pool.FrameArrived(e.token);
        if (e.session) e.session.Close();
        if (e.pool) e.pool.Close();
    } catch (...) {
    }
    e.token = {};
    e.session = nullptr;
    e.pool = nullptr;
    e.item = nullptr;
}

void Thumbnails::OnFrame(HWND window) {
    Entry* e = Find(window);
    if (!e || !e->pool || !renderer_) return;
    ID2D1DeviceContext* dc = renderer_->ResourceContext();
    try {
        wgc::Direct3D11CaptureFrame frame = e->pool.TryGetNextFrame();
        if (!frame) return;
        ++liveFrames_;
        const auto content = frame.ContentSize();
        bool stored = false;
        if (dc && content.Width > 0 && content.Height > 0) {
            auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            winrt::com_ptr<IDXGISurface> surface;
            winrt::com_ptr<ID2D1Bitmap1> src;
            const D2D1_BITMAP_PROPERTIES1 sp = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            if (SUCCEEDED(access->GetInterface(IID_PPV_ARGS(surface.put()))) &&
                SUCCEEDED(dc->CreateBitmapFromDxgiSurface(surface.get(), sp, src.put()))) {
                const D2D1_SIZE_U pool = src->GetPixelSize();
                const UINT cw = std::min<UINT>(static_cast<UINT>(content.Width), pool.width);
                const UINT ch = std::min<UINT>(static_cast<UINT>(content.Height), pool.height);
                const SIZE out = Fit(static_cast<LONG>(cw), static_cast<LONG>(ch), width_, height_);
                winrt::com_ptr<ID2D1Bitmap1> target = e->bitmap;
                if (!target || target->GetPixelSize().width != static_cast<UINT32>(out.cx) ||
                    target->GetPixelSize().height != static_cast<UINT32>(out.cy) ||
                    !(target->GetOptions() & D2D1_BITMAP_OPTIONS_TARGET)) {
                    target = nullptr;
                    const D2D1_BITMAP_PROPERTIES1 tp = D2D1::BitmapProperties1(
                        D2D1_BITMAP_OPTIONS_TARGET,
                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                    if (FAILED(dc->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(out.cx), static_cast<UINT32>(out.cy)), nullptr,
                                                0, tp, target.put()))) {
                        target = nullptr;
                    }
                }
                if (target) {
                    winrt::com_ptr<ID2D1Image> old;
                    dc->GetTarget(old.put());
                    dc->SetTarget(target.get());
                    dc->BeginDraw();
                    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
                    const D2D1_RECT_F srcRect = D2D1::RectF(0, 0, static_cast<float>(cw), static_cast<float>(ch));
                    dc->DrawBitmap(src.get(), D2D1::RectF(0, 0, static_cast<float>(out.cx), static_cast<float>(out.cy)), 1.0f,
                                   D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, &srcRect, nullptr);
                    const HRESULT hr = dc->EndDraw();
                    dc->SetTarget(old.get());
                    if (SUCCEEDED(hr)) {
                        Store(*e, target.get());
                        stored = true;
                    }
                }
            }
        }
        frame.Close();
        // The window was resized: the pool follows (the next frame has the new size).
        if (content.Width > 0 && content.Height > 0 &&
            (content.Width != e->poolSize.Width || content.Height != e->poolSize.Height)) {
            e->poolSize = content;
            e->pool.Recreate(device_->device, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 1, content);
        }
        if (stored && changed_) changed_({window});
    } catch (...) {
        // The window went away or the device was lost: the dock drops the entry on its next sync.
    }
}

void Thumbnails::Store(Entry& e, ID2D1Bitmap1* bitmap) { e.bitmap.copy_from(bitmap); }

void Thumbnails::UpdateTimer() {
    if (!dock_) return;
    const bool want = mode_ == ThumbnailMode::Snapshots && visible_ && !entries_.empty();
    if (want && !timer_) {
        timer_ = SetTimer(dock_, kTimerId, static_cast<UINT>(intervalS_) * 1000u, nullptr) != 0;
        StartRound({});  // the first pictures now, then every interval
    } else if (!want && timer_) {
        KillTimer(dock_, kTimerId);
        timer_ = false;
    }
}

bool Thumbnails::StartRound(const std::vector<HWND>& only) {
    if (roundRunning_ || !dock_ || mode_ != ThumbnailMode::Snapshots || entries_.empty()) return false;
    auto* job = new Job;
    job->dock = dock_;
    job->generation = generation_;
    job->width = width_;
    job->height = height_;
    if (only.empty()) {
        for (const auto& e : entries_) job->windows.push_back(e->window);
    } else {
        job->windows = only;
    }
    HANDLE thread = CreateThread(nullptr, 0, &SnapshotThread, job, 0, nullptr);
    if (!thread) {
        delete job;
        return false;
    }
    CloseHandle(thread);
    roundRunning_ = true;
    ++rounds_;
    return true;
}

void Thumbnails::OnSnapshots(Job* job) {
    std::unique_ptr<Job> owned(job);
    if (job->generation != generation_) return;  // the mode changed (or a reset) meanwhile
    roundRunning_ = false;
    if (mode_ != ThumbnailMode::Snapshots || !renderer_) return;
    ID2D1DeviceContext* dc = renderer_->ResourceContext();
    if (!dc) return;
    std::vector<HWND> changed;
    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    for (const Job::Shot& s : job->shots) {
        Entry* e = Find(s.window);
        if (!e || s.px.empty()) continue;
        winrt::com_ptr<ID2D1Bitmap1> bmp;
        if (FAILED(dc->CreateBitmap(D2D1::SizeU(s.w, s.h), s.px.data(), s.w * 4, props, bmp.put()))) continue;
        Store(*e, bmp.get());
        changed.push_back(s.window);
    }
    if (!changed.empty() && changed_) changed_(changed);
}

bool Thumbnails::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TIMER && wp == kTimerId) {
        if (timer_) StartRound({});
        return true;
    }
    if (msg == kLiveMessage) {
        ApplyLive();
        return true;
    }
    if (msg == kSnapshotMessage) {
        OnSnapshots(reinterpret_cast<Job*>(lp));
        return true;
    }
    return false;
}
