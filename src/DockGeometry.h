#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <d2d1helper.h>

#include <algorithm>

#include "Config.h"

// The one place that knows how a dock's edge maps its layout onto the screen.
//
// The dock lays itself out in canonical coordinates: u along the bar (the main axis: left -> right
// on bottom / top docks, top -> bottom on left / right docks) and v across it (the cross axis),
// growing TOWARD the screen edge, in a frame `cross` long on the cross axis (v = 0 is the side
// facing the middle of the screen, v = cross the screen edge). A bottom dock is the identity:
//
//   bottom: x = u,          y = v              top:   x = u,          y = cross - v
//   left:   x = cross - v,  y = u              right: x = v,          y = u
//
// The main axis is never flipped, so a main coordinate is an x (horizontal docks) or a y (vertical
// docks) as it is. Content that is drawn "in the dock's orientation" (the bar, hairlines, the
// separator, the dots, the insertion marker) is drawn in canonical coordinates through Matrix();
// icons, widgets and text stay upright and are only placed through these maps.
namespace DockGeometry {

inline D2D1_POINT_2F Point(DockEdge e, float u, float v, float cross) {
    switch (e) {
        case DockEdge::Top: return D2D1::Point2F(u, cross - v);
        case DockEdge::Left: return D2D1::Point2F(cross - v, u);
        case DockEdge::Right: return D2D1::Point2F(v, u);
        case DockEdge::Bottom:
        default: return D2D1::Point2F(u, v);
    }
}

// The canonical box [u0, u1] x [v0, v1] in physical coordinates (normalised: left <= right).
inline D2D1_RECT_F Rect(DockEdge e, float u0, float v0, float u1, float v1, float cross) {
    const D2D1_POINT_2F a = Point(e, u0, v0, cross);
    const D2D1_POINT_2F b = Point(e, u1, v1, cross);
    return D2D1::RectF(std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y));
}

// The physical coordinate (on the cross axis: y for horizontal docks, x for vertical ones) of the
// low side of the band [v, v + length].
inline float CrossOrigin(DockEdge e, float v, float length, float cross) {
    return e == DockEdge::Top || e == DockEdge::Left ? cross - v - length : v;
}

// Point() as a transform: canonical drawing -> physical surface pixels.
inline D2D1_MATRIX_3X2_F Matrix(DockEdge e, float cross) {
    switch (e) {
        case DockEdge::Top: return D2D1::Matrix3x2F(1.0f, 0.0f, 0.0f, -1.0f, 0.0f, cross);
        case DockEdge::Left: return D2D1::Matrix3x2F(0.0f, 1.0f, -1.0f, 0.0f, cross, 0.0f);
        case DockEdge::Right: return D2D1::Matrix3x2F(0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f);
        case DockEdge::Bottom:
        default: return D2D1::Matrix3x2F::Identity();
    }
}

// A pixel (x, y) of a frame `cross` long on the cross axis -> canonical (u, v). The flipped axes
// mirror pixel indexes (v = cross - 1 - y), so a bottom dock is exactly the identity.
inline D2D1_POINT_2F Canon(DockEdge e, LONG x, LONG y, float cross) {
    const float fx = static_cast<float>(x), fy = static_cast<float>(y);
    switch (e) {
        case DockEdge::Top: return D2D1::Point2F(fx, cross - 1.0f - fy);
        case DockEdge::Left: return D2D1::Point2F(fy, cross - 1.0f - fx);
        case DockEdge::Right: return D2D1::Point2F(fy, fx);
        case DockEdge::Bottom:
        default: return D2D1::Point2F(fx, fy);
    }
}

// Physical width / height of a box `main` x `cross` long.
inline D2D1_SIZE_F Size(DockEdge e, float main, float cross) {
    return IsVertical(e) ? D2D1::SizeF(cross, main) : D2D1::SizeF(main, cross);
}

// The direction a dock moves to go off its edge, as a sign on the physical cross axis (+1: toward
// larger x / y): bottom and right docks slide toward +, top and left docks toward -.
inline float OutwardSign(DockEdge e) { return e == DockEdge::Bottom || e == DockEdge::Right ? 1.0f : -1.0f; }

// The monitor's extent along the dock's main axis, and its cross-axis side at the edge.
inline LONG MainStart(DockEdge e, const RECT& r) { return IsVertical(e) ? r.top : r.left; }
inline LONG MainEnd(DockEdge e, const RECT& r) { return IsVertical(e) ? r.bottom : r.right; }

}  // namespace DockGeometry
