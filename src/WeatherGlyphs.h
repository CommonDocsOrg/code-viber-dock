#pragma once

#include <Unknwn.h>
#include <windows.h>
#include <d2d1_1.h>

#include <cstdint>

// Vector weather glyphs (no image files, crisp at any DPI): WMO weather code -> one of a few
// pictures drawn with Direct2D geometry. The cloud is one ID2D1PathGeometry (three circular
// arcs over a flat base), the sun an ellipse and 8 rays, the moon a crescent of two arcs, and
// drops / flakes / the bolt small strokes and polygons. Drawn only when a surface is redrawn
// (a data, theme or DPI change; a popup paint), never per frame.
namespace WeatherGlyphs {

enum class Glyph { Clear, PartlyCloudy, Overcast, Fog, Drizzle, Rain, Snow, Thunder, SetLocation };

struct Colors {
    uint32_t ink = 0xF2FFFFFF;    // clouds, moon, snow, outlines (ARGB)
    uint32_t sun = 0xFFFFC53D;
    uint32_t water = 0xFF4CC2FF;  // rain and drizzle
    uint32_t bolt = 0xFFFFC53D;
    float opacity = 1.0f;         // multiplies every colour's alpha (stale data: 0.6)
};

// WMO code (Open-Meteo `weather_code`) -> glyph; unknown codes: Overcast.
Glyph FromWmo(int code);
// "Partly cloudy", "Light rain" ... ("Unknown" for codes outside the WMO table).
const wchar_t* Describe(int code);

// Draws `glyph` into the square `box` (px). `isDay` picks the sun or the moon for Clear and
// PartlyCloudy. Works on any render target (the dock's DComp surfaces, the settings preview).
void Draw(ID2D1RenderTarget* rt, Glyph glyph, bool isDay, const D2D1_RECT_F& box, const Colors& colors);
// A circular "refresh" arrow in `color`, centred in `box`.
void DrawRefresh(ID2D1RenderTarget* rt, const D2D1_RECT_F& box, uint32_t color);

}  // namespace WeatherGlyphs
