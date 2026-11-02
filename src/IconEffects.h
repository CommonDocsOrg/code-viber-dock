#pragma once

#include <Unknwn.h>
#include <windows.h>
#include <d2d1_1.h>

#include <winrt/base.h>

#include <cstdint>
#include <functional>

#include "Config.h"

// Icon glow and reflection, drawn once into an icon's surfaces when it is (re)drawn: the dock
// (DockWindow::RenderItem) and the settings preview strip use the same helpers. Every D2D effect
// is created inside the call and released before it returns; nothing is kept between draws.
namespace IconEffects {

// Draws the icon at (0, 0) - (size, size) into `dc`.
using DrawIconFn = std::function<void(ID2D1DeviceContext* dc, float size)>;

// Records `drawIcon` at `size` into a command list on dc's device (through a separate device
// context, so the caller's target and transform are untouched). Null on failure.
winrt::com_ptr<ID2D1CommandList> Record(ID2D1DeviceContext* dc, float size, const DrawIconFn& drawIcon);

// Glow surface padding per side for a blur radius in pixels: ceil(3 sigma), sigma = radius / 3.
UINT GlowPad(float radiusPx);
// The glow colour: theme.glowColor when its alpha is > 0, else the icon's dominant colour, else
// `fallback` (0xAARRGGBB). The alpha of the result scales the strength.
uint32_t GlowColor(const ThemeConfig& theme, uint32_t dominant, uint32_t fallback);
// The halo: `icon` (recorded at size x size) blurred with sigma = radiusPx / 3 (soft border,
// speed optimisation), its coverage boosted (alpha x 3, clamped) so it stays solid near the
// silhouette, then recoloured to `argb` with its alpha multiplied by `strength` x the colour's
// alpha (colour matrices on premultiplied values). Drawn with the icon's top-left at `at` (the
// caller leaves GlowPad(radiusPx) around it). Returns false if an effect could not be created
// (nothing is drawn).
bool DrawGlow(ID2D1DeviceContext* dc, ID2D1Image* icon, float radiusPx, uint32_t argb, float strength,
              D2D1_POINT_2F at = D2D1::Point2F());

// Reflection surface height for an icon of `size` pixels: ceil(size * heightFrac).
UINT ReflectionHeight(UINT size, float heightFrac);
// The mirrored icon below its foot: `icon` (recorded at size x size) flipped vertically so the
// icon's bottom row lands at at.y, shown through a linear opacity gradient from `opacity` at
// at.y to 0 at at.y + height (clipped there).
void DrawReflection(ID2D1DeviceContext* dc, ID2D1Image* icon, float size, float height, float opacity,
                    D2D1_POINT_2F at = D2D1::Point2F());

}  // namespace IconEffects
