#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <d2d1helper.h>

#include <algorithm>
#include <cmath>

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
//
// On top of the edge mapping, a scrolling dock adds a viewport transform on the main axis
// (Viewport below): the run of slots scrolls inside the bar.
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

// --- the viewport (docks[].overflow "scroll", round 4 phase 11a) ---------------------------------
//
// A run of slots longer than the room along the edge keeps the bar at that room and scrolls inside
// it. The run is laid out in content coordinates; the viewport shows content main coordinate c at
// stage coordinate View(c) = c - scroll. The layout's rest positions (DockItem::slotX / x, the
// targets) are VIEW coordinates, so hit testing, popups, labels, drags and drops need no change:
// a scroll shifts them all by the same amount (DockWindow::ShiftScroll). The slot visuals are clipped
// to [clipStart, clipEnd) along the main axis and fade out over `fade` px at an end that has more
// content beyond it (an opacity per slot that follows the slot's own placement curve exactly:
// FadeSegments). Not scrolling (`on` false) is the identity, with nothing clipped or faded.
struct Viewport {
    bool on = false;
    float start = 0.0f, end = 0.0f;          // the run's room on the main axis (stage px)
    float clipStart = 0.0f, clipEnd = 0.0f;  // where slot visuals are cut (the bar's ends)
    float scroll = 0.0f;                     // content px scrolled past `start` (0 .. max, whole px)
    float max = 0.0f;                        // content - (end - start)
    float content = 0.0f;                    // the run's length
    float fade = 0.0f;                       // ramp length of an edge fade
    bool fadeStart = false, fadeEnd = false; // the ends that fade now (content hidden beyond them)

    float Length() const { return end - start; }
    float View(float c) const { return on ? c - scroll : c; }
    float Content(float view) const { return on ? view + scroll : view; }
    // A box [lo, hi] (view) lies entirely outside the clip, more than `margin` beyond its ends.
    bool Outside(float lo, float hi, float margin) const {
        return on && (hi < clipStart - margin || lo > clipEnd + margin);
    }
    // The opacity of a slot whose box is centred on view coordinate `c`: 1, ramping down to 0 at
    // an end that fades.
    float Opacity(float c) const {
        if (!on || fade <= 0.0f) return 1.0f;
        float o = 1.0f;
        if (fadeStart) o = std::min(o, std::clamp((c - start) / fade, 0.0f, 1.0f));
        if (fadeEnd) o = std::min(o, std::clamp((end - c) / fade, 0.0f, 1.0f));
        return o;
    }
};

// One segment of a curve for IDCompositionAnimation::AddCubic: from `begin` seconds on, the value
// is c0 + c1 t + c2 t^2 + c3 t^3 with t = seconds since `begin`.
struct CubicSegment {
    double begin = 0.0;
    float c0 = 0.0f, c1 = 0.0f, c2 = 0.0f, c3 = 0.0f;
};

// The edge-fade opacity of a box whose leading edge moves from p0 to p1 over `duration` seconds on
// Renderer::Drive's curve (an ease-out cubic; `linear`: a straight line), centred `half` px after
// that edge: Viewport::Opacity(p(t) + half) exactly. It is constant outside the ramps and a linear
// function of the (cubic) position inside one, so it is a piecewise cubic with a break wherever the
// box centre crosses a ramp's end. The curve is given from `elapsed` seconds into the motion on
// (segment times are relative to that point). Returns the segment count (at most 5); the curve
// ends at Opacity(p1 + half) at duration - elapsed.
inline int FadeSegments(const Viewport& v, float p0, float p1, double duration, bool linear, float half,
                        double elapsed, CubicSegment out[5]) {
    const double D = duration > 0.0 ? duration : 1.0;
    const double d = static_cast<double>(p1) - static_cast<double>(p0);
    // p(s) = p0 + k1 s + k2 s^2 + k3 s^3
    const double k1 = linear ? d / D : 3.0 * d / D;
    const double k2 = linear ? 0.0 : -3.0 * d / (D * D);
    const double k3 = linear ? 0.0 : d / (D * D * D);
    auto pos = [&](double s) { return p0 + ((k3 * s + k2) * s + k1) * s; };
    // Breaks: the times the centre crosses a ramp boundary strictly between p0 and p1.
    const double from = std::clamp(elapsed, 0.0, D);
    double times[6];
    int n = 0;
    times[n++] = from;
    const float bounds[4] = {v.start, v.start + v.fade, v.end - v.fade, v.end};
    const bool active[4] = {v.fadeStart, v.fadeStart, v.fadeEnd, v.fadeEnd};
    const double lo = std::min(p0, p1), hi = std::max(p0, p1);
    for (int i = 0; i < 4 && d != 0.0; ++i) {
        const double b = static_cast<double>(bounds[i]) - half;
        if (!active[i] || b <= lo || b >= hi) continue;
        const double q = (b - p0) / d;  // fraction of the way (0..1)
        const double s = linear ? D * q : D * (1.0 - std::cbrt(1.0 - q));
        if (s > from) times[n++] = s;
    }
    std::sort(times, times + n);
    int count = 0;
    for (int i = 0; i < n; ++i) {
        const double s0 = times[i];
        const double s1 = i + 1 < n ? times[i + 1] : D;
        if (s1 - s0 < 1e-6 && i + 1 < n) continue;
        const float cm = static_cast<float>(pos((s0 + s1) / 2.0)) + half;  // which piece: by its middle
        CubicSegment seg;
        seg.begin = s0 - from;
        const double P0 = pos(s0), P1 = k1 + (2.0 * k2 + 3.0 * k3 * s0) * s0, P2 = k2 + 3.0 * k3 * s0;
        const bool inStart = v.fadeStart && cm > v.start && cm < v.start + v.fade;
        const bool inEnd = v.fadeEnd && cm > v.end - v.fade && cm < v.end;
        // Both ramps at once (a viewport shorter than two fades): the nearer end wins.
        const bool useStart = inStart && (!inEnd || cm - v.start < v.end - cm);
        if (useStart || inEnd) {
            const double sign = useStart ? 1.0 : -1.0;
            const double base = useStart ? (P0 + half - v.start) : (v.end - P0 - half);
            seg.c0 = static_cast<float>(base / v.fade);
            seg.c1 = static_cast<float>(sign * P1 / v.fade);
            seg.c2 = static_cast<float>(sign * P2 / v.fade);
            seg.c3 = static_cast<float>(sign * k3 / v.fade);
        } else {
            seg.c0 = v.Opacity(cm);
        }
        out[count++] = seg;
        if (count == 5) break;
    }
    return count;
}

}  // namespace DockGeometry
