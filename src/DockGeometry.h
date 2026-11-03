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

// --- polar shapes (docks[].shape "arc" / "circle", round 4 phase 11b) ----------------------------
//
// The third mapping besides the edges: the run is laid out in canonical coordinates as for a bar
// (u along it, v across it toward the screen edge) and wrapped onto a ring. u is arc length on the
// circle of radius R (the icons' centre line, canonical v = vRing), so the angle is
// theta = theta0 + dir * u / R; v is radial, r = R + vRing - v (a smaller v lies further out: the
// cross axis points outward from the screen edge, toward the ring's centre for an arc whose
// diameter lies on the edge). Angles are the usual mathematical ones (0 = right, pi / 2 = up on
// screen). Icons, widgets and text stay upright: a box is placed at the mapped point of its centre
// (Box), and every scroll / magnification / fade computation stays in u. Not polar (`on` false):
// the edge mapping above applies.
struct Polar {
    bool on = false;
    float cx = 0.0f, cy = 0.0f;  // the ring's centre (physical stage px)
    float R = 1.0f;              // radius of the line v = vRing
    float vRing = 0.0f;
    float theta0 = 0.0f;         // the angle of u = 0
    float dir = -1.0f;           // +1: u runs counter-clockwise on screen, -1: clockwise
    float span = 3.14159265f;    // the angle the run may use: pi (a half ring) or 2 pi (a circle)

    float Angle(float u) const { return theta0 + dir * u / R; }
    float Radius(float v) const { return R + vRing - v; }
    D2D1_POINT_2F Point(float u, float v) const {
        const float t = Angle(u), r = Radius(v);
        return D2D1::Point2F(cx + r * std::cos(t), cy - r * std::sin(t));
    }
    // The unit vector pointing away from the centre at u (screen coordinates).
    D2D1_POINT_2F Outward(float u) const {
        const float t = Angle(u);
        return D2D1::Point2F(std::cos(t), -std::sin(t));
    }
    // The centre of an upright box w x h placed just outside canonical (u, v), away from the centre
    // (labels, popups, an arc's title): the box's extent along the outward direction is added.
    D2D1_POINT_2F Outside(float u, float v, float w, float h) const {
        const D2D1_POINT_2F p = Point(u, v), o = Outward(u);
        const float k = (std::fabs(o.x) * w + std::fabs(o.y) * h) / 2.0f;
        return D2D1::Point2F(p.x + o.x * k, p.y + o.y * k);
    }
    // A physical stage point -> canonical (u, v). Angles are unwrapped around the middle of the
    // run's span, so a point beyond either end gets a u before 0 / after span * R.
    D2D1_POINT_2F Canon(float x, float y) const {
        const float dx = x - cx, dy = cy - y;
        const float r = std::sqrt(dx * dx + dy * dy);
        constexpr float kTwoPi = 6.28318531f;
        float a = (std::atan2(dy, dx) - theta0) * dir;  // radians travelled from u = 0
        const float mid = span / 2.0f;
        a = mid + std::remainder(a - mid, kTwoPi);
        return D2D1::Point2F(a * R, vRing - (r - R));
    }
    // The bounding box of the ring sector [u0, u1] x [v0, v1].
    D2D1_RECT_F Bounds(float u0, float v0, float u1, float v1) const {
        float l = 1e9f, t = 1e9f, r = -1e9f, b = -1e9f;
        auto add = [&](float u, float v) {
            const D2D1_POINT_2F p = Point(u, v);
            l = std::min(l, p.x);
            t = std::min(t, p.y);
            r = std::max(r, p.x);
            b = std::max(b, p.y);
        };
        for (const float v : {v0, v1}) {
            add(u0, v);
            add(u1, v);
        }
        // The quadrant points (where x or y is extreme) inside the sector's angle range.
        const float a0 = std::min(Angle(u0), Angle(u1)), a1 = std::max(Angle(u0), Angle(u1));
        constexpr float kHalfPi = 1.57079633f;
        for (float q = std::ceil(a0 / kHalfPi) * kHalfPi; q <= a1; q += kHalfPi) {
            for (const float v : {v0, v1}) {
                const float rr = Radius(v);
                const D2D1_POINT_2F p = D2D1::Point2F(cx + rr * std::cos(q), cy - rr * std::sin(q));
                l = std::min(l, p.x);
                t = std::min(t, p.y);
                r = std::max(r, p.x);
                b = std::max(b, p.y);
            }
        }
        return D2D1::RectF(l, t, r, b);
    }
    // An upright box `main` x `cross` long centred on canonical (u, v): its physical rect.
    D2D1_RECT_F Box(float u, float v, float main, float cross) const {
        const D2D1_POINT_2F p = Point(u, v);
        return D2D1::RectF(p.x - main / 2.0f, p.y - cross / 2.0f, p.x + main / 2.0f, p.y + cross / 2.0f);
    }
};

// Where a ring of radius R starts and which way it runs, per shape and edge: an arc on the bottom
// edge runs from the left end over the top to the right end (pi -> 0), on the top edge from the left
// over the bottom (pi -> 2 pi), on the left edge from the top over the right (pi / 2 -> -pi / 2), on
// the right edge from the top over the left (pi / 2 -> 3 pi / 2); a circle starts at the top and runs
// clockwise.
inline void PolarDirection(bool circle, DockEdge e, Polar& p) {
    constexpr float kPi = 3.14159265f;
    p.span = circle ? 2.0f * kPi : kPi;
    if (circle) {
        p.theta0 = kPi / 2.0f;
        p.dir = -1.0f;
        return;
    }
    switch (e) {
        case DockEdge::Top: p.theta0 = kPi; p.dir = 1.0f; break;
        case DockEdge::Left: p.theta0 = kPi / 2.0f; p.dir = -1.0f; break;
        case DockEdge::Right: p.theta0 = kPi / 2.0f; p.dir = 1.0f; break;
        case DockEdge::Bottom:
        default: p.theta0 = kPi; p.dir = -1.0f; break;
    }
}

// An animation curve along a mapped path: the property f(u(t)) for u following the tween's curve
// (ease-out / linear; a spring is followed as an ease-out) from `elapsed` seconds into it, as
// piecewise cubic Hermite segments (values and slopes at the ends of each piece, slopes by central
// differences). Used to move upright boxes along a ring (x and y separately) so they follow the
// arc, not its chord. Returns the segment count (<= `maxSegments`).
template <class F>
int PathSegments(float from, float to, double duration, bool linear, double elapsed, int pieces, F&& f,
                 CubicSegment* out, int maxSegments) {
    const double D = duration > 0.0 ? duration : 1.0;
    const double d = static_cast<double>(to) - static_cast<double>(from);
    const double k1 = linear ? d / D : 3.0 * d / D;
    const double k2 = linear ? 0.0 : -3.0 * d / (D * D);
    const double k3 = linear ? 0.0 : d / (D * D * D);
    auto u = [&](double s) {
        s = std::clamp(s, 0.0, D);
        return static_cast<float>(from + ((k3 * s + k2) * s + k1) * s);
    };
    const double s0 = std::clamp(elapsed, 0.0, D);
    const int n = std::clamp(pieces, 1, maxSegments);
    const double h = (D - s0) / n;
    const double e = std::max(1e-4, h * 1e-3);
    auto slope = [&](double s) { return (static_cast<double>(f(u(s + e))) - static_cast<double>(f(u(s - e)))) / (2.0 * e); };
    for (int i = 0; i < n; ++i) {
        const double a = s0 + h * i, b = a + h;
        const double x0 = f(u(a)), x1 = f(u(b));
        const double m0 = slope(a), m1 = slope(b);
        CubicSegment seg;
        seg.begin = a - s0;
        seg.c0 = static_cast<float>(x0);
        seg.c1 = static_cast<float>(m0);
        seg.c2 = static_cast<float>((3.0 * (x1 - x0) / h - 2.0 * m0 - m1) / h);
        seg.c3 = static_cast<float>((2.0 * (x0 - x1) / h + m0 + m1) / (h * h));
        out[i] = seg;
    }
    return n;
}

}  // namespace DockGeometry
