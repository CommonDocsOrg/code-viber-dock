#pragma once
// Round 14 phase 14B: the snapping every move shares (a floating bar, a circle, a standalone widget,
// a folder window). Each axis snaps on its own, so the corners come from two edges meeting, the
// middle of an edge from one edge and one centre line, and the screen's centre from both centre
// lines. Off (`settings.snapToScreen` false) the callers skip it altogether.
#include <windows.h>

#include <cstdlib>
#include <vector>

namespace SnapLines {

// The shift along one axis that brings the nearest of a span's ends [a, b] onto one of `lines` (the
// work area's edges, the neighbours' edges) or the span's centre onto one of `centres` (the work
// area's centre line), when the nearest is within `thr` px; else 0. Ties keep the first found (the
// edges before the centre lines, as before 14B).
inline LONG Best(LONG a, LONG b, const std::vector<LONG>& lines, const std::vector<LONG>& centres, LONG thr) {
    LONG shift = 0, dist = thr + 1;
    auto test = [&](LONG c, LONG v) {
        if (std::labs(c - v) < dist) {
            dist = std::labs(c - v);
            shift = c - v;
        }
    };
    for (LONG c : lines) {
        test(c, a);
        test(c, b);
    }
    const LONG mid = a + (b - a) / 2;
    for (LONG c : centres) test(c, mid);
    return dist <= thr ? shift : 0;
}

// The work area's centre lines: {horizontal centre x} and {vertical centre y}.
inline std::vector<LONG> CentreX(const RECT& work) { return {work.left + (work.right - work.left) / 2}; }
inline std::vector<LONG> CentreY(const RECT& work) { return {work.top + (work.bottom - work.top) / 2}; }

}  // namespace SnapLines
