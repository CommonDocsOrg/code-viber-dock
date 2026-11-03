#pragma once

#include <Unknwn.h>
#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>

#include <winrt/base.h>

#include <functional>
#include <string>

#include "Clock.h"
#include "Config.h"

// The dock preview strip of the settings window (WinDock.SettingsPreview): a child window under
// every page that draws a miniature dock from the current Config with Direct2D. It has its own
// factory and DC render target (never the dock's Renderer or its DirectComposition device), it
// paints only on WM_PAINT (after Invalidate() or a mouse move: the magnification follows the
// cursor) and runs no timers. Everything is released when the window is destroyed.
class SettingsPreview {
public:
    static constexpr wchar_t kClassName[] = L"WinDock.SettingsPreview";

    SettingsPreview() = default;
    ~SettingsPreview();
    SettingsPreview(const SettingsPreview&) = delete;
    SettingsPreview& operator=(const SettingsPreview&) = delete;

    bool Create(HINSTANCE instance, HWND parent, int id, const Config* config, UINT dpi);
    HWND Hwnd() const { return hwnd_; }
    // The dock it draws (its items and sizes; looked up at every paint; null: docks[0]) and whether
    // in the global theme (the Global window's Themes / Animation pages) or the dock's own.
    void SetDock(std::function<const DockConfig*()> dock, bool globalTheme) {
        dock_ = std::move(dock);
        globalTheme_ = globalTheme;
    }
    // Background colours of the strip (the settings window's content layer and a divider).
    void SetColors(COLORREF layer, COLORREF divider);
    // The window moved to a monitor with another DPI: re-create the target at that DPI.
    void SetDpi(UINT dpi);
    // The config changed: repaint.
    void Invalidate();

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);
    void Paint();
    HRESULT EnsureTarget();
    void ReleaseTarget();
    void ReleaseAll();
    void Draw(ID2D1DeviceContext* dc);
    IDWriteTextFormat* Format(winrt::com_ptr<IDWriteTextFormat>& slot, float& slotSize, float size,
                              DWRITE_FONT_WEIGHT weight);

    HWND hwnd_ = nullptr;
    const Config* config_ = nullptr;
    std::function<const DockConfig*()> dock_;
    bool globalTheme_ = false;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    COLORREF layer_ = RGB(0x2B, 0x2B, 0x2B);
    COLORREF divider_ = RGB(0x3A, 0x3A, 0x3A);
    // Cursor x in client pixels while the mouse is over the strip; < 0: the default focus.
    int mouseX_ = -1;
    bool tracking_ = false;

    winrt::com_ptr<ID2D1Factory1> factory_;
    winrt::com_ptr<IDWriteFactory> dwrite_;
    winrt::com_ptr<ID2D1DCRenderTarget> target_;  // bound to the WM_PAINT DC
    winrt::com_ptr<IDWriteTextFormat> labelFormat_;
    winrt::com_ptr<IDWriteTextFormat> letterFormat_;
    winrt::com_ptr<IDWriteTextFormat> titleFormat_;
    float labelSize_ = 0.0f, letterSize_ = 0.0f, titleSize_ = 0.0f;
    Clock clock_;
    std::wstring clockFormat_;  // what clock_ was configured with
    bool clockDate_ = false;
    std::wstring clockZone_;
    std::wstring clockLabel_;
    bool clockConfigured_ = false;
};
