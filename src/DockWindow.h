#pragma once

#include <windows.h>

#include <functional>

#include "Clock.h"
#include "Config.h"
#include "Renderer.h"

// The dock HWND: placement, DPI, layout, and the composition layers drawn into it.
class DockWindow {
public:
    static constexpr wchar_t kClassName[] = L"WinDock.DockWindow";
    static constexpr UINT_PTR kClockTimerId = 1;
    static constexpr UINT_PTR kTrimTimerId = 2;

    // Called for every message before DockWindow handles it. Return true to consume
    // the message, with the LRESULT in `result`.
    using MessageHook = std::function<bool(HWND, UINT, WPARAM, LPARAM, LRESULT& result)>;

    DockWindow() = default;
    DockWindow(const DockWindow&) = delete;
    DockWindow& operator=(const DockWindow&) = delete;
    ~DockWindow();

    // Creates the window, devices and visuals and shows the dock. Returns a failure HRESULT
    // if the composition stack can't be created.
    HRESULT Create(HINSTANCE instance, const Config* config, MessageHook hook);
    void Destroy();
    HWND Hwnd() const { return hwnd_; }

    // Recomputes the layout, moves/resizes the window and redraws everything.
    void Relayout();

    // Re-reads clock settings from the config and relayouts.
    void ApplyConfig();

private:
    // All values in physical pixels. Bar and clock rects are client-relative.
    struct Layout {
        float scale = 1.0f;
        RECT window{};         // screen coordinates
        D2D1_RECT_F bar{};     // drawn background
        D2D1_RECT_F clock{};   // clock text block
        float cornerRadius = 0.0f;
        float iconPx = 0.0f;   // unmagnified icon size (for later phases)
        float padding = 0.0f;
    };

    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);

    HMONITOR PickMonitor() const;
    void ComputeLayout();
    void ApplyWindowPos();

    HRESULT CreateLayers();
    HRESULT RenderBackground();
    HRESULT RenderClock();
    // Redraws every layer and commits; recovers from device loss.
    void RenderAll();
    HRESULT DrawAll();
    void OnClockTick();
    // Runs `step`, then commits. On device loss, rebuilds devices and redraws everything.
    void RenderWithRecovery(const std::function<HRESULT()>& step);
    bool RecoverDevice();
    void ScheduleWorkingSetTrim();

    HWND hwnd_ = nullptr;
    const Config* config_ = nullptr;
    MessageHook hook_;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    bool inLayout_ = false;

    Renderer renderer_;
    Clock clock_;
    Layer background_;
    Layer clockLayer_;
    Layout layout_;
    D2D1_SIZE_U clockSize_{};
};
