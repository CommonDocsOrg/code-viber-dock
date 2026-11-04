#pragma once

#include <Unknwn.h>
#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>

#include <winrt/base.h>

#include <cstdint>
#include <set>
#include <string>

#include "Config.h"
#include "ScriptEngine.h"

class WidgetRuntime;

// Round 6 phase 6E: renders a widget's live preview for the settings windows (the Contents tab's
// widget drawer and inspector, Global > Widget library). Owned by DockManager (next to the widget
// runtime it reads the storage of); nothing of it exists until the first preview is asked for.
//
// A render: a WidgetInstance in preview mode (ScriptDockInfo::preview: never registered with the
// runtime, no timers, http, storage writes, shell, popups or providers; the providers answer with
// WidgetSamples; the manifest's preview.settings over the defaults) on a virtual dock (horizontal,
// bottom edge, icon size 48, the global theme's colours, reduce motion on, the requested scale),
// measured (or the manifest's preview.width / height), drawn once into a WIC bitmap through a
// software Direct2D render target (no GPU device, no DirectComposition: CreateWicBitmapRenderTarget
// and its ID2D1DeviceContext), copied into a premultiplied 32-bit top-down DIB section for GDI
// (GdiAlphaBlend), and destroyed (its script runtime freed) before Render returns. A widget that
// fails to load, measure or draw gives no bitmap: the caller keeps its letter tile, and the error
// goes to the debugger output once per id. Test hook: the environment variable
// WINDOCK_PREVIEW_LOG names a file that gets one line per render ("ok <id> <w>x<h> <ms>" or
// "fail <id> <error>").
class WidgetPreview {
public:
    struct Result {
        HBITMAP bitmap = nullptr;  // the caller owns it (DeleteObject); null: failed
        SIZE px{};                 // its size
        std::wstring error;        // failed: why (the widget's error text)
        double ms = 0.0;           // the render's wall time
    };

    explicit WidgetPreview(WidgetRuntime& runtime) : runtime_(runtime) {}
    ~WidgetPreview() { Release(); }
    WidgetPreview(const WidgetPreview&) = delete;
    WidgetPreview& operator=(const WidgetPreview&) = delete;

    // Widget `id` on the virtual dock `dock` (DockInfoFor), its weather place `place` (units and
    // label of the sample forecast), fitted into `fitW` x `fitH` px: a box larger than that is drawn
    // at a smaller scale (dock.scale shrinks; the canvas is vector, so it stays sharp); a smaller
    // one is drawn larger, up to `maxZoom` times (1: never larger).
    Result Render(const std::wstring& id, const ScriptDockInfo& dock, const WeatherConfig& place, UINT fitW, UINT fitH,
                  float maxZoom = 1.0f);
    // Drops the factories (the last settings window closed).
    void Release();

    uint64_t Renders() const { return renders_; }
    uint64_t Failures() const { return failures_; }
    // Preview instances alive now (0 outside Render: they never outlive it).
    int Alive() const { return alive_; }

    // The virtual dock of a preview: horizontal, bottom, icon size 48, the global theme, `scale`.
    static ScriptDockInfo DockInfoFor(const Config& config, float scale);
    // A hash of what a preview depends on in the config (the global theme's colours, the units).
    static uint64_t ThemeStamp(const Config& config);

private:
    bool EnsureFactories();
    void Report(const std::wstring& id, const Result& r);

    WidgetRuntime& runtime_;
    winrt::com_ptr<ID2D1Factory1> d2d_;
    winrt::com_ptr<IDWriteFactory> dwrite_;
    winrt::com_ptr<IWICImagingFactory> wic_;
    uint64_t renders_ = 0;
    uint64_t failures_ = 0;
    int alive_ = 0;
    std::set<std::wstring> reported_;  // ids whose failure went to the debugger output
};
