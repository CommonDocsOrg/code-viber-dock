#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>

#include "PopupWindow.h"
#include "Renderer.h"
#include "Weather.h"
#include "WeatherGlyphs.h"

// The weather widget's forecast: a click popup (PopupWindow, class WinDockWeather) above the
// widget, one surface. Header: the location, a large glyph and temperature, the condition,
// humidity and wind, "Updated hh:mm", the Open-Meteo credit (a link) and a refresh button. Then
// 24 hourly columns from the current hour (hour, glyph, temperature, the precipitation chance
// when above 0, a thin temperature line) and 7 daily rows (weekday, glyph, low and high, a range
// bar on the week's scale). Without data it shows a short message instead (loading, error, no
// location). Redrawn only when the data changes or the hot button changes; nothing runs while it
// is open.
class WeatherPopup : public PopupWindow {
public:
    static constexpr wchar_t kClassName[] = L"WinDockWeather";

    struct Callbacks {
        std::function<void()> refresh;       // the refresh button / F5
        std::function<void()> openSettings;  // "Weather settings…" (no location yet)
        std::function<void()> closed;        // closed by itself (deactivated, Escape)
    };
    struct Style {
        float scale = 1.0f;
        bool acrylic = false;
        uint32_t tint = 0xCC202020;
        uint32_t text = 0xFFFFFFFF;           // theme.labelText
        uint32_t secondary = 0xB6FFFFFF;      // theme.clockDateText
        uint32_t border = 0x24FFFFFF;         // theme.barBorder (panel edge, hairlines, bar tracks)
        uint32_t hotBackground = 0x1AFFFFFF;
        uint32_t hotBorder = 0x33FFFFFF;
        float cornerRadiusDip = 8.0f;
        RECT monitor{};
        WeatherGlyphs::Colors glyphs;
    };

    WeatherPopup() : PopupWindow(kClassName, L"WinDock weather", Mode::Click) {}
    ~WeatherPopup() override { Destroy(); }

    void Init(HINSTANCE instance, Renderer* renderer, const WeatherService* weather, Callbacks callbacks);
    // Shows the popup at `anchor` (beside the dock, away from its edge).
    bool Show(const Style& style, const PopupAnchor& anchor);
    // The weather data / status changed: lay out and redraw in place (no-op unless visible).
    void Update();

protected:
    SIZE Measure() override;
    void Paint(ID2D1DeviceContext* dc) override;
    int HitAt(POINT pt, int* part) const override;
    void OnClick(int index, int part, Button button) override;
    void OnDestroying() override;
    bool OnMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) override;

private:
    static constexpr int kHitRefresh = 1;
    static constexpr int kHitCredit = 2;
    static constexpr int kHitSettings = 3;

    void EnsureFormats();
    // Draws `text` in `rect` with `format` (alignment as given) in `color` x `alpha`.
    void Text(ID2D1DeviceContext* dc, IDWriteTextFormat* format, const std::wstring& text, const D2D1_RECT_F& rect,
              uint32_t color, float alpha, DWRITE_TEXT_ALIGNMENT align) const;
    float TextWidth(IDWriteTextFormat* format, const std::wstring& text) const;
    std::wstring Temp(double value, bool unit) const;
    std::wstring HourLabel(int hour) const;
    std::wstring Message() const;

    Callbacks callbacks_;
    const WeatherService* weather_ = nullptr;
    Style style_;
    Frame frame_;
    PopupAnchor anchor_;
    bool full_ = false;      // the forecast layout (else the message layout)

    RECT refreshRect_{};
    RECT creditRect_{};
    RECT settingsRect_{};    // message layout without a location: "Weather settings…"

    float formatScale_ = 0.0f;
    bool hours24_ = true;    // the locale's time format uses a 24-hour clock
    winrt::com_ptr<IDWriteTextFormat> titleFormat_;  // location
    winrt::com_ptr<IDWriteTextFormat> bigFormat_;    // current temperature
    winrt::com_ptr<IDWriteTextFormat> bodyFormat_;   // condition, weekday, highs
    winrt::com_ptr<IDWriteTextFormat> smallFormat_;  // details, updated, credit
    winrt::com_ptr<IDWriteTextFormat> tinyFormat_;   // hourly labels
    winrt::com_ptr<IDWriteTextFormat> microFormat_;  // precipitation chance
};
