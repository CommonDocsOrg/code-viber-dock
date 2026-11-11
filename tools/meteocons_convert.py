"""Converts Meteocons' animated SVG icons (https://meteocons.com, @meteocons/svg, MIT, Bas Milius)
into widgets/weather/meteocons.js: a compact scene per icon that the Weather widget draws with
canvas.layer() (round 16 phase 16F), one composition layer per animated part, so DirectComposition
runs the loops and the dock never wakes up for a frame.

Usage: python tools/meteocons_convert.py <folder with the fill/*.svg files> [<out .js>]

What is kept of each SVG (everything the 20 icons the widget uses contain): <path> (d with M L H
V C Z; fill / stroke colours or linear gradients, stroke width, caps, joins, opacity), <circle>,
<linearGradient> in user space, a <mask> or <clipPath> that is one rectangle (a clip on the
group), <animateTransform type="translate|rotate"> and <animate attributeName="opacity"> with
values, keyTimes, calcMode="spline" keySplines, dur, begin and repeatCount="indefinite". An
animated element becomes a part; its drawables belong to it (the icons never nest animated
elements); the static drawables between animated ones become static parts in document order so
the z-order stays. Anything else is reported and skipped.

Output (an ES module): SIZE (128), VERSION, and ICONS: { name: { g: { gradient: [x1, y1, x2, y2,
[[offset, colour], ...]] }, parts: [part] } }, part = { el: [element], tr: [track], clip: [x, y,
w, h], ch: [part] } (clip and ch only on a clipped group), element = ["p", d, fill, stroke,
strokeWidth, cap, join, opacity] or ["c", cx, cy, r, fill, stroke, strokeWidth, opacity] (fill /
stroke: "#rrggbb", "g:<gradient>" or null), track = { p: "x" | "y" | "rotate" | "opacity",
c: [cx, cy] (rotate), d: seconds, b: begin seconds, k: [[t seconds, value], ...], e: [[x1, y1, x2,
y2], ...] (one per key span; left out: linear) }. Numbers are rounded to 2 decimals.
"""
import json
import os
import re
import sys
import xml.etree.ElementTree as ET

NS = "{http://www.w3.org/2000/svg}"
NUM = re.compile(r"-?\d*\.?\d+(?:e[-+]?\d+)?")


def tag(e):
    return e.tag[len(NS):] if e.tag.startswith(NS) else e.tag


def num(s, default=0.0):
    if s is None or s == "":
        return default
    return float(s.rstrip("s").rstrip("px"))


def rnd(v):
    r = round(v, 2)
    return int(r) if r == int(r) else r


def round_path(d):
    return NUM.sub(lambda m: str(rnd(float(m.group(0)))), d)


def paint(value, grads, skipped):
    if value is None or value == "none":
        return None
    m = re.match(r"url\(#(.+)\)", value)
    if m:
        if m.group(1) not in grads:
            skipped.append("paint " + value)
            return None
        return "g:" + m.group(1)
    if re.match(r"^#[0-9a-fA-F]{6}$", value):
        return value.upper()
    if re.match(r"^#[0-9a-fA-F]{3}$", value):
        return "#" + "".join(c * 2 for c in value[1:]).upper()
    skipped.append("paint " + value)
    return None


def element(e, grads, skipped):
    t = tag(e)
    op = num(e.get("opacity"), 1.0)
    fill = paint(e.get("fill"), grads, skipped)
    stroke = paint(e.get("stroke"), grads, skipped)
    sw = num(e.get("stroke-width"), 1.0) if stroke else 0
    if t == "path":
        d = e.get("d", "")
        if re.search(r"[^MLHVCZmlhvcz0-9 .,\-e]", d):
            skipped.append("path command in " + d[:30])
        return ["p", round_path(d), fill, stroke, rnd(sw), e.get("stroke-linecap", "butt"), e.get("stroke-linejoin", "miter"), rnd(op)]
    if t == "circle":
        return ["c", rnd(num(e.get("cx"))), rnd(num(e.get("cy"))), rnd(num(e.get("r"))), fill, stroke, rnd(sw), rnd(op)]
    if t == "rect":
        x, y, w, h = (num(e.get(k)) for k in ("x", "y", "width", "height"))
        return ["p", f"M{rnd(x)} {rnd(y)}H{rnd(x + w)}V{rnd(y + h)}H{rnd(x)}Z", fill, stroke, rnd(sw), "butt", "miter", rnd(op)]
    skipped.append("element <" + t + ">")
    return None


def splines(text, n):
    if not text:
        return None
    out = []
    for part in text.split(";"):
        v = [float(x) for x in NUM.findall(part)]
        if len(v) == 4:
            out.append([rnd(x) for x in v])
    return out if len(out) == n else None


def tracks_of(e, skipped):
    out = []
    for a in e:
        t = tag(a)
        if t not in ("animate", "animateTransform"):
            continue
        dur = num(a.get("dur"), 0.0)
        begin = num(a.get("begin"), 0.0)
        repeat = a.get("repeatCount") == "indefinite"
        values = [[float(x) for x in NUM.findall(v)] for v in a.get("values", "").split(";") if v.strip()]
        n = len(values)
        if dur <= 0 or n < 2:
            skipped.append("animation without dur / values")
            continue
        times = [float(x) for x in a.get("keyTimes", "").split(";") if x.strip()] if a.get("keyTimes") else [i / (n - 1) for i in range(n)]
        if len(times) != n:
            skipped.append("keyTimes count")
            continue
        ease = splines(a.get("keySplines"), n - 1) if a.get("calcMode") == "spline" else None

        def track(prop, vals, center=None):
            if all(abs(v - vals[0]) < 1e-9 for v in vals):
                return  # a constant component (the x of a vertical bob)
            tr = {"p": prop, "d": rnd(dur), "b": rnd(begin), "k": [[rnd(times[i] * dur), rnd(vals[i])] for i in range(n)]}
            if ease:
                tr["e"] = ease
            if not repeat:
                tr["once"] = True
            if center:
                tr["c"] = [rnd(center[0]), rnd(center[1])]
            out.append(tr)

        if t == "animate" and a.get("attributeName") == "opacity":
            track("opacity", [v[0] for v in values])
        elif t == "animateTransform" and a.get("type") == "translate":
            track("x", [v[0] for v in values])
            track("y", [v[1] if len(v) > 1 else 0.0 for v in values])
        elif t == "animateTransform" and a.get("type") == "rotate":
            track("rotate", [v[0] for v in values], (values[0][1], values[0][2]) if len(values[0]) >= 3 else (0.0, 0.0))
        else:
            skipped.append("animation " + t + " " + str(a.get("attributeName")) + " " + str(a.get("type")))
    return out


def rect_clip(ref, defs, view):
    """A mask / clipPath that is one rectangle -> [x, y, w, h], or None for the whole view box."""
    m = re.match(r"url\(#(.+)\)", ref or "")
    if not m or m.group(1) not in defs:
        return None
    rects = [c for c in defs[m.group(1)] if tag(c) == "rect"]
    if len(rects) != 1:
        return "unsupported"
    r = rects[0]
    box = [num(r.get("x")), num(r.get("y")), num(r.get("width")), num(r.get("height"))]
    if box == view:
        return None
    return [rnd(v) for v in box]


def convert(path, skipped):
    root = ET.parse(path).getroot()
    view = [float(x) for x in root.get("viewBox", "0 0 128 128").split()]
    grads, defs = {}, {}
    for d in root.iter(NS + "defs"):
        for g in d:
            if tag(g) == "linearGradient":
                stops = []
                for s in g:
                    if tag(s) == "stop":
                        stops.append([rnd(num(s.get("offset"), 0.0)), paint(s.get("stop-color", "#000000"), {}, skipped)])
                grads[g.get("id")] = [rnd(num(g.get("x1"))), rnd(num(g.get("y1"))), rnd(num(g.get("x2"))), rnd(num(g.get("y2"))), stops]
            elif tag(g) in ("clipPath", "mask"):
                defs[g.get("id")] = list(g)
    for m in root.iter(NS + "mask"):
        defs[m.get("id")] = list(m)
    for c in root.iter(NS + "clipPath"):
        defs[c.get("id")] = list(c)

    def walk(container, parts):
        # parts: the output list of this container; static drawables go into a running static part.
        static = None
        for e in container:
            t = tag(e)
            if t in ("defs", "mask", "clipPath", "animate", "animateTransform"):
                continue
            clip = rect_clip(e.get("mask") or e.get("clip-path"), defs, view) if t == "g" else None
            if clip == "unsupported":
                skipped.append("mask / clip-path that is not one rectangle")
                clip = None
            tr = tracks_of(e, skipped)
            if clip:
                group = {"el": [], "clip": clip, "ch": []}
                if tr:
                    group["tr"] = tr
                walk(e, group["ch"])
                parts.append(group)
                static = None
            elif tr:
                part = {"el": [], "tr": tr}
                if t == "g":
                    collect(e, part["el"])
                else:
                    el = element(e, grads, skipped)
                    if el:
                        part["el"].append(el)
                parts.append(part)
                static = None
            elif t == "g":
                # A plain group: its children join the current static run, unless one of them
                # is animated, in which case walk it (its own static runs are appended here).
                if any(tracks_of(c, []) or (tag(c) == "g" and c.get("mask")) for c in e.iter() if c is not e):
                    walk(e, parts)
                    static = None
                else:
                    if static is None:
                        static = {"el": []}
                        parts.append(static)
                    collect(e, static["el"])
            else:
                el = element(e, grads, skipped)
                if el:
                    if static is None:
                        static = {"el": []}
                        parts.append(static)
                    static["el"].append(el)

    def collect(container, out):
        for e in container:
            t = tag(e)
            if t in ("defs", "mask", "clipPath", "animate", "animateTransform"):
                continue
            if t == "g":
                collect(e, out)
            else:
                el = element(e, grads, skipped)
                if el:
                    out.append(el)

    parts = []
    walk(root, parts)
    # Only the gradients that are used.
    used = set()

    def scan(ps):
        for p in ps:
            for el in p["el"]:
                for v in el:
                    if isinstance(v, str) and v.startswith("g:"):
                        used.add(v[2:])
            scan(p.get("ch", []))

    scan(parts)
    return {"g": {k: v for k, v in grads.items() if k in used}, "parts": parts}


def main():
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "..", "widgets", "weather", "meteocons.js")
    version = "unknown"
    pkg = os.path.join(src, "..", "package.json")
    if os.path.exists(pkg):
        with open(pkg, encoding="utf-8") as f:
            version = json.load(f).get("version", version)
    icons = {}
    problems = {}
    for name in sorted(os.listdir(src)):
        if not name.endswith(".svg"):
            continue
        skipped = []
        icons[name[:-4]] = convert(os.path.join(src, name), skipped)
        if skipped:
            problems[name] = sorted(set(skipped))
    body = json.dumps(icons, separators=(",", ":"), ensure_ascii=True)
    # One icon per line keeps the module diffable.
    body = body.replace('},"', '},\n"', 0)
    lines = ["// Generated by tools/meteocons_convert.py from @meteocons/svg " + version + " (fill style): do not edit.",
             "// Meteocons by Bas Milius, https://meteocons.com, MIT licence:",
             "//   Copyright (c) 2020-present Bas Milius. Permission is hereby granted, free of charge, to any",
             "//   person obtaining a copy of this software and associated documentation files (the \"Software\"),",
             "//   to deal in the Software without restriction, including without limitation the rights to use,",
             "//   copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and",
             "//   to permit persons to whom the Software is furnished to do so, subject to the following",
             "//   conditions: The above copyright notice and this permission notice shall be included in all",
             "//   copies or substantial portions of the Software. THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT",
             "//   WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF",
             "//   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE",
             "//   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN",
             "//   AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE",
             "//   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.",
             "// The shape of ICONS is documented at the top of tools/meteocons_convert.py.",
             "export const VERSION = " + json.dumps(version) + ";",
             "export const SIZE = 128;",
             "export const ICONS = {"]
    for name, icon in icons.items():
        lines.append(json.dumps(name) + ":" + json.dumps(icon, separators=(",", ":")) + ",")
    lines.append("};")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    parts = sum(len(i["parts"]) for i in icons.values())
    tracks = 0

    def count(ps):
        nonlocal tracks
        for p in ps:
            tracks += len(p.get("tr", []))
            count(p.get("ch", []))

    for i in icons.values():
        count(i["parts"])
    print(f"{len(icons)} icons, {parts} parts, {tracks} tracks, {os.path.getsize(out)} bytes -> {out}")
    for name, s in problems.items():
        print("  skipped in", name + ":", "; ".join(s))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
