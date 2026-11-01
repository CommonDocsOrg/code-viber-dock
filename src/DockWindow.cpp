#include "DockWindow.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

constexpr DWORD kExStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP;
constexpr DWORD kStyle = WS_POPUP;

// Layout constants in DIPs (scaled by DPI).
constexpr float kBarExtraHeight = 16.0f;   // bar height = iconSize + this
constexpr float kWindowExtraHeight = 40.0f; // window height = iconSize * magnification + this
constexpr float kBottomMargin = 8.0f;       // gap between the bar and the screen edge
constexpr float kPadding = 12.0f;           // inner horizontal padding of the bar
constexpr float kItemsPlaceholder = 200.0f; // stands in for dock items until Phase 3

D2D1_COLOR_F ArgbToColor(uint32_t argb) {
    return D2D1::ColorF(static_cast<float>((argb >> 16) & 0xFF) / 255.0f,
                        static_cast<float>((argb >> 8) & 0xFF) / 255.0f,
                        static_cast<float>(argb & 0xFF) / 255.0f,
                        static_cast<float>((argb >> 24) & 0xFF) / 255.0f);
}

BOOL CALLBACK CollectMonitor(HMONITOR mon, HDC, LPRECT, LPARAM lp) {
    reinterpret_cast<std::vector<HMONITOR>*>(lp)->push_back(mon);
    return TRUE;
}

void Log(const wchar_t* what, HRESULT hr) {
    wchar_t buf[160];
    swprintf_s(buf, L"WinDock: %s failed (0x%08X)\n", what, static_cast<unsigned>(hr));
    OutputDebugStringW(buf);
}

}  // namespace

DockWindow::~DockWindow() { Destroy(); }

HRESULT DockWindow::Create(HINSTANCE instance, const Config* config, MessageHook hook) {
    config_ = config;
    hook_ = std::move(hook);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &DockWindow::WndProcThunk;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    // Start on the target monitor so GetDpiForWindow reports that monitor's DPI.
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(PickMonitor(), &mi);
    hwnd_ = CreateWindowExW(kExStyle, kClassName, L"WinDock", kStyle, mi.rcMonitor.left, mi.rcMonitor.bottom - 1, 1, 1,
                            nullptr, nullptr, instance, this);
    if (!hwnd_) return HRESULT_FROM_WIN32(GetLastError());
    dpi_ = GetDpiForWindow(hwnd_);

    HRESULT hr = renderer_.Initialize(hwnd_, config_->renderer == L"software");
    if (FAILED(hr)) {
        Log(L"Renderer::Initialize", hr);
        return hr;
    }
    hr = CreateLayers();
    if (FAILED(hr)) {
        Log(L"CreateLayers", hr);
        return hr;
    }

    ApplyConfig();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    return S_OK;
}

void DockWindow::Destroy() {
    if (hwnd_) DestroyWindow(hwnd_);
}

void DockWindow::ApplyConfig() {
    clock_.Configure(config_->clock.format, config_->clock.showDate);
    Relayout();
    if (config_->clock.show) {
        clock_.ScheduleNextTick(hwnd_, kClockTimerId);
    } else {
        Clock::Stop(hwnd_, kClockTimerId);
    }
}

HMONITOR DockWindow::PickMonitor() const {
    // Index 0 = primary; 1..n = the other monitors in enumeration order.
    std::vector<HMONITOR> monitors;
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&monitors));
    HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    std::erase(monitors, primary);
    monitors.insert(monitors.begin(), primary);
    const size_t index = config_ ? static_cast<size_t>(std::max(config_->monitor, 0)) : 0;
    return index < monitors.size() ? monitors[index] : primary;
}

void DockWindow::ComputeLayout() {
    const Config& c = *config_;
    Layout l;
    l.scale = static_cast<float>(dpi_) / USER_DEFAULT_SCREEN_DPI;
    const float s = l.scale;

    l.iconPx = std::round(static_cast<float>(c.iconSize) * s);
    l.padding = std::round(kPadding * s);
    l.cornerRadius = std::round(c.theme.cornerRadius * s);

    const float barH = std::round(l.iconPx + kBarExtraHeight * s);
    const float margin = std::round(kBottomMargin * s);
    float winH = std::round(l.iconPx * static_cast<float>(c.magnification) + kWindowExtraHeight * s);
    winH = std::max(winH, barH + margin);

    // Clock text block (already laid out at this scale).
    const float clockW = c.clock.show ? static_cast<float>(clockSize_.width) : 0.0f;
    const float clockH = c.clock.show ? static_cast<float>(clockSize_.height) : 0.0f;

    const float barW = std::round(clockW + 2.0f * l.padding + kItemsPlaceholder * s);
    // Horizontal slack so magnified edge icons (later phases) are not clipped.
    const float slack = std::round(l.iconPx * (static_cast<float>(c.magnification) - 1.0f));
    const float winW = barW + 2.0f * slack;

    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(PickMonitor(), &mi);
    const RECT& m = mi.rcMonitor;  // the taskbar is hidden in Phase 2, so use the full monitor
    const LONG left = m.left + ((m.right - m.left) - static_cast<LONG>(winW)) / 2;
    l.window = RECT{left, m.bottom - static_cast<LONG>(winH), left + static_cast<LONG>(winW), m.bottom};

    l.bar = D2D1::RectF(slack, winH - margin - barH, slack + barW, winH - margin);
    const float clockX = l.bar.right - l.padding - clockW;
    const float clockY = std::round(l.bar.top + (barH - clockH) / 2.0f);
    l.clock = D2D1::RectF(clockX, clockY, clockX + clockW, clockY + clockH);

    layout_ = l;
}

void DockWindow::ApplyWindowPos() {
    const RECT& r = layout_.window;
    SetWindowPos(hwnd_, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void DockWindow::Relayout() {
    if (!hwnd_ || inLayout_) return;
    inLayout_ = true;

    for (int pass = 0; pass < 2; ++pass) {
        const UINT before = dpi_;
        const float scale = static_cast<float>(dpi_) / USER_DEFAULT_SCREEN_DPI;
        clock_.UpdateText();
        clockSize_ = config_->clock.show ? clock_.BuildLayout(renderer_.DWrite(), scale) : D2D1_SIZE_U{};
        ComputeLayout();
        ApplyWindowPos();
        // Moving to another monitor may change our DPI (WM_DPICHANGED arrives re-entrantly
        // during SetWindowPos); lay out once more if it did.
        const UINT now = GetDpiForWindow(hwnd_);
        dpi_ = now;
        if (now == before) break;
    }

    inLayout_ = false;
    RenderAll();
}

HRESULT DockWindow::CreateLayers() {
    background_.Reset();
    clockLayer_.Reset();
    HRESULT hr = renderer_.CreateLayer(background_);
    if (SUCCEEDED(hr)) hr = renderer_.CreateLayer(clockLayer_);
    return hr;
}

HRESULT DockWindow::RenderBackground() {
    const D2D1_RECT_F& b = layout_.bar;
    const UINT w = static_cast<UINT>(b.right - b.left);
    const UINT h = static_cast<UINT>(b.bottom - b.top);
    const D2D1_COLOR_F fill = ArgbToColor(config_->theme.background);
    const float radius = std::min(layout_.cornerRadius, static_cast<float>(std::min(w, h)) / 2.0f);
    const float stroke = std::max(1.0f, std::round(layout_.scale));

    HRESULT hr = renderer_.DrawLayer(background_, w, h, [&](ID2D1DeviceContext* dc) {
        winrt::com_ptr<ID2D1SolidColorBrush> brush;
        if (FAILED(dc->CreateSolidColorBrush(fill, brush.put()))) return;
        const D2D1_ROUNDED_RECT rr{D2D1::RectF(0, 0, static_cast<float>(w), static_cast<float>(h)), radius, radius};
        dc->FillRoundedRectangle(rr, brush.get());
        // Hairline highlight so the bar reads against dark wallpapers.
        const float inset = stroke / 2.0f;
        const D2D1_ROUNDED_RECT edge{
            D2D1::RectF(inset, inset, static_cast<float>(w) - inset, static_cast<float>(h) - inset),
            std::max(0.0f, radius - inset), std::max(0.0f, radius - inset)};
        brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f));
        dc->DrawRoundedRectangle(edge, brush.get(), stroke);
    });
    if (FAILED(hr)) return hr;
    renderer_.SetOffset(background_, b.left, b.top);
    return S_OK;
}

HRESULT DockWindow::RenderClock() {
    if (!config_->clock.show) return renderer_.DrawLayer(clockLayer_, 0, 0, {});
    HRESULT hr = renderer_.DrawLayer(clockLayer_, clockSize_.width, clockSize_.height,
                                     [&](ID2D1DeviceContext* dc) { clock_.Draw(dc, 0.0f, 0.0f); });
    if (FAILED(hr)) return hr;
    renderer_.SetOffset(clockLayer_, layout_.clock.left, layout_.clock.top);
    return S_OK;
}

HRESULT DockWindow::DrawAll() {
    HRESULT hr = RenderBackground();
    if (SUCCEEDED(hr)) hr = RenderClock();
    return hr;
}

void DockWindow::RenderAll() {
    RenderWithRecovery([this] { return DrawAll(); });
}

void DockWindow::RenderWithRecovery(const std::function<HRESULT()>& step) {
    HRESULT hr = renderer_.CheckDevice();
    if (SUCCEEDED(hr)) hr = step();
    if (SUCCEEDED(hr)) hr = renderer_.Commit();
    ScheduleWorkingSetTrim();
    if (SUCCEEDED(hr)) return;

    if (!Renderer::IsDeviceLost(hr)) {
        Log(L"render", hr);
        return;
    }
    if (!RecoverDevice()) Log(L"device recovery", hr);
}

bool DockWindow::RecoverDevice() {
    // Device removed/reset: rebuild the whole chain and every surface. Retry a few times
    // because a driver update can keep the adapter unavailable for a moment.
    for (int attempt = 0; attempt < 3; ++attempt) {
        background_.Reset();
        clockLayer_.Reset();
        HRESULT hr = renderer_.RecreateDevice();
        if (SUCCEEDED(hr)) hr = CreateLayers();
        if (SUCCEEDED(hr)) hr = DrawAll();
        if (SUCCEEDED(hr)) hr = renderer_.Commit();
        if (SUCCEEDED(hr)) return true;
        Log(L"RecreateDevice", hr);
        Sleep(100);
    }
    return false;
}

void DockWindow::ScheduleWorkingSetTrim() {
    // The GPU user-mode driver touches tens of MB while drawing and then sits idle until the
    // next redraw (at most once a minute). Shortly after a burst of drawing, hand those pages
    // back to the OS so the idle working set stays small. Re-arming coalesces bursts.
    if (hwnd_) SetTimer(hwnd_, kTrimTimerId, 1000, nullptr);
}

void DockWindow::OnClockTick() {
    if (!config_->clock.show) return;
    if (!clock_.UpdateText()) return;
    const float scale = static_cast<float>(dpi_) / USER_DEFAULT_SCREEN_DPI;
    const D2D1_SIZE_U size = clock_.BuildLayout(renderer_.DWrite(), scale);
    if (size.width != clockSize_.width || size.height != clockSize_.height) {
        Relayout();  // the bar width depends on the clock width
    } else {
        RenderWithRecovery([this] { return RenderClock(); });
    }
}

LRESULT CALLBACK DockWindow::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    DockWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<DockWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<DockWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    LRESULT result = self->WndProc(msg, wp, lp);
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        self->hwnd_ = nullptr;
    }
    return result;
}

LRESULT DockWindow::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    if (hook_) {
        LRESULT result = 0;
        if (hook_(hwnd_, msg, wp, lp, result)) return result;
    }

    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            // Content comes from DirectComposition; there is no redirection bitmap to paint.
            ValidateRect(hwnd_, nullptr);
            return 0;

        case WM_TIMER:
            if (wp == kClockTimerId) {
                KillTimer(hwnd_, kClockTimerId);  // one-shot; rescheduled on the next boundary
                OnClockTick();
                if (config_->clock.show) clock_.ScheduleNextTick(hwnd_, kClockTimerId);
                return 0;
            }
            if (wp == kTrimTimerId) {
                KillTimer(hwnd_, kTrimTimerId);
                SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
                return 0;
            }
            break;

        case WM_TIMECHANGE:
            OnClockTick();
            if (config_->clock.show) clock_.ScheduleNextTick(hwnd_, kClockTimerId);
            return 0;

        case WM_POWERBROADCAST:
            if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) {
                OnClockTick();
                if (config_->clock.show) clock_.ScheduleNextTick(hwnd_, kClockTimerId);
            }
            return TRUE;

        case WM_DPICHANGED:
            dpi_ = HIWORD(wp);
            Relayout();  // ignores the suggested rect: we always place ourselves
            return 0;

        case WM_DISPLAYCHANGE:
            Relayout();
            return 0;

        case WM_SETTINGCHANGE:
            if (wp == SPI_SETWORKAREA || wp == SPI_SETNONCLIENTMETRICS) Relayout();
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd_, kClockTimerId);
            KillTimer(hwnd_, kTrimTimerId);
            background_.Reset();
            clockLayer_.Reset();
            clock_.ReleaseLayout();
            renderer_.Shutdown();
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
