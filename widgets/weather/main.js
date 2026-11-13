// Weather: the dock's weather element and its forecast popup rewritten on the widget
// runtime (round 4 phase 14). The data comes from the dock's WeatherService of its place through
// the `weather` provider (round 5: its own place, units and interval from its settings, else the
// place of its dock's first Weather item): one fetcher per place, shared with every item of that
// place; this widget never fetches anything itself. The slot draws what
// src/WidgetElements.cpp's WeatherElement draws (the condition glyph, the temperature in the
// clock's font, dimmed while the data is stale or missing, a "set location" glyph when no place is
// chosen) with the same sizes, so the two look alike side by side. The popup follows
// src/WeatherPopup.cpp's layout: the place, the current conditions, a 24-hour strip and the daily
// rows, "Updated N min ago", the refresh and settings buttons and the Open-Meteo credit.
// Round 7 phase 7C: settings.dockForecast adds today's high / low or a few day columns to the slot.
// Round 16 phase 16D: layouts. The slot is two blocks, the current conditions (the icon beside or
// above the temperature and its text lines) and the forecast (a cell per day: a column, a tile,
// a line or just the icon), placed after or under each other; `layout` names a preset or lets
// the three fields choose ("custom"). Text options (day names, "Today", how a high / low pair is
// written, the condition's name), a size, and a second icon pack: Lucide's line icons drawn from
// their SVG path data (below), so a narrow dock can show "Mon over the icon, 22° over 14° beside
// it" instead of four values stacked.
//
// Cost: nothing polls. The widget redraws when the provider reports a change (a fetch started,
// finished or failed); while the popup is open one timeout per minute updates "N min ago".

// Round 16 phase 16F: a third icon pack, Meteocons (https://meteocons.com, MIT), coloured and
// animated: each icon is a few parts (a sun, its rays, a cloud, each raindrop) with loops of
// translate / rotate / opacity, converted from the upstream SVGs by tools/meteocons_convert.py into
// meteocons.js. On a dock every part is a canvas.layer(): a child visual of the slot whose loop
// DirectComposition runs, so the icons move while this script, the dock and the CPU stay idle. In
// the popup, a preview, or with Animated icons / reduce motion off, the same parts are drawn
// inline as a still picture.
import { ICONS as METEO, SIZE as METEO_SIZE } from './meteocons.js';

const GLYPH = 26;          // the native element's glyph (24 when the clock shows its date)
const FONT = 16;           // its temperature size (14 with the clock's date)
const GAP = 3;             // glyph -> temperature
const STALE = 0.6;         // stale / missing data are drawn at 60 % alpha
const SUN = '#FFFFC53D';   // WeatherGlyphs' sun colour (the daily range's warm end)
const CREDIT = 'Weather data by Open-Meteo';
const CREDIT_URL = 'https://open-meteo.com/';

let wx = weather.get();
weather.onChange((state) => {
  wx = state;
  requestRedraw();
  if (popupShown) showPopup();  // resizes when the data appeared / went, else redraws
});

// --- icon packs ----------------------------------------------------------------------------------
//
// "windock": the dock's own vector glyphs through weather.icon() (WeatherGlyphs.cpp).
// "lucide": Lucide's line icons (https://lucide.dev, lucide-static 0.468.0), ISC licence:
//   Copyright (c) for portions of Lucide are held by Cole Bemis 2013-2022 as part of Feather (MIT).
//   All other copyright (c) for Lucide are held by Lucide Contributors 2022.
//   Permission to use, copy, modify, and/or distribute this software for any purpose with or
//   without fee is hereby granted, provided that the above copyright notice and this permission
//   notice appear in all copies. THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
//   WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
//   AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
//   CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
//   WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
//   CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
// Each icon is its SVG path data verbatim (a <circle> / <line> written as a path), 24 x 24 units,
// stroked 2 units wide with round caps and joins, scaled to the box. The second element of an
// entry tints it: 's' the sun colour, 'w' the water (rain) colour, 'b' the bolt, '' the ink.
// Parsed into canvas paths once (LUCIDE_PATHS), since a path's geometry is built once and kept.
const LUCIDE = {
  'sun': [['M8 12a4 4 0 1 0 8 0a4 4 0 1 0-8 0', 's'], ['M12 2v2', 's'], ['M12 20v2', 's'], ['m4.93 4.93 1.41 1.41', 's'],
          ['m17.66 17.66 1.41 1.41', 's'], ['M2 12h2', 's'], ['M20 12h2', 's'], ['m6.34 17.66-1.41 1.41', 's'], ['m19.07 4.93-1.41 1.41', 's']],
  'moon': [['M12 3a6 6 0 0 0 9 9 9 9 0 1 1-9-9Z', '']],
  'cloud': [['M17.5 19H9a7 7 0 1 1 6.71-9h1.79a4.5 4.5 0 1 1 0 9Z', '']],
  'cloudy': [['M17.5 21H9a7 7 0 1 1 6.71-9h1.79a4.5 4.5 0 1 1 0 9Z', ''], ['M22 10a3 3 0 0 0-3-3h-2.207a5.502 5.502 0 0 0-10.702.5', '']],
  'cloud-sun': [['M12 2v2', 's'], ['m4.93 4.93 1.41 1.41', 's'], ['M20 12h2', 's'], ['m19.07 4.93-1.41 1.41', 's'],
                ['M15.947 12.65a4 4 0 0 0-5.925-4.128', 's'], ['M13 22H7a5 5 0 1 1 4.9-6H13a3 3 0 0 1 0 6Z', '']],
  'cloud-moon': [['M10.188 8.5A6 6 0 0 1 16 4a1 1 0 0 0 6 6 6 6 0 0 1-3 5.197', ''], ['M13 16a3 3 0 1 1 0 6H7a5 5 0 1 1 4.9-6Z', '']],
  'cloud-rain': [['M4 14.899A7 7 0 1 1 15.71 8h1.79a4.5 4.5 0 0 1 2.5 8.242', ''], ['M16 14v6', 'w'], ['M8 14v6', 'w'], ['M12 16v6', 'w']],
  'cloud-drizzle': [['M4 14.899A7 7 0 1 1 15.71 8h1.79a4.5 4.5 0 0 1 2.5 8.242', ''], ['M8 19v1', 'w'], ['M8 14v1', 'w'], ['M16 19v1', 'w'],
                    ['M16 14v1', 'w'], ['M12 21v1', 'w'], ['M12 16v1', 'w']],
  'cloud-snow': [['M4 14.899A7 7 0 1 1 15.71 8h1.79a4.5 4.5 0 0 1 2.5 8.242', ''], ['M8 15h.01', ''], ['M8 19h.01', ''], ['M12 17h.01', ''],
                 ['M12 21h.01', ''], ['M16 15h.01', ''], ['M16 19h.01', '']],
  'cloud-lightning': [['M6 16.326A7 7 0 1 1 15.71 8h1.79a4.5 4.5 0 0 1 .5 8.973', ''], ['m13 12-3 5h4l-3 5', 'b']],
  'cloud-fog': [['M4 14.899A7 7 0 1 1 15.71 8h1.79a4.5 4.5 0 0 1 2.5 8.242', ''], ['M16 17H7', ''], ['M17 21H9', '']],
  'cloud-hail': [['M4 14.899A7 7 0 1 1 15.71 8h1.79a4.5 4.5 0 0 1 2.5 8.242', ''], ['M16 14v2', 'w'], ['M8 14v2', 'w'], ['M16 20h.01', 'w'],
                 ['M8 20h.01', 'w'], ['M12 16v2', 'w'], ['M12 22h.01', 'w']],
  'cloud-sun-rain': [['M12 2v2', 's'], ['m4.93 4.93 1.41 1.41', 's'], ['M20 12h2', 's'], ['m19.07 4.93-1.41 1.41', 's'],
                     ['M15.947 12.65a4 4 0 0 0-5.925-4.128', 's'], ['M3 20a5 5 0 1 1 8.9-4H13a3 3 0 0 1 2 5.24', ''], ['M11 20v2', 'w'], ['M7 19v2', 'w']],
  'cloud-moon-rain': [['M10.188 8.5A6 6 0 0 1 16 4a1 1 0 0 0 6 6 6 6 0 0 1-3 5.197', ''], ['M11 20v2', 'w'],
                      ['M3 20a5 5 0 1 1 8.9-4H13a3 3 0 0 1 2 5.24', ''], ['M7 19v2', 'w']],
  'snowflake': [['M2 12H22', ''], ['M12 2V22', ''], ['m20 16-4-4 4-4', ''], ['m4 8 4 4-4 4', ''], ['m16 4-4 4-4-4', ''], ['m8 20 4-4 4 4', '']],
  'map-pin': [['M20 10c0 4.993-5.539 10.193-7.399 11.799a1 1 0 0 1-1.202 0C9.539 20.193 4 14.993 4 10a8 8 0 0 1 16 0', ''],
              ['M9 10a3 3 0 1 0 6 0a3 3 0 1 0-6 0', '']],
  'refresh-cw': [['M3 12a9 9 0 0 1 9-9 9.75 9.75 0 0 1 6.74 2.74L21 8', ''], ['M21 3v5h-5', ''],
                 ['M21 12a9 9 0 0 1-9 9 9.75 9.75 0 0 1-6.74-2.74L3 16', ''], ['M8 16H3v5', '']],
};

// WMO code -> Lucide icon (the same families as WeatherGlyphs::FromWmo, a few finer: freezing
// rain as hail, showers with a sun by day, snow grains as a flake).
function lucideName(code, isDay) {
  if (code === 'setLocation') return 'map-pin';
  if (code === 'refresh') return 'refresh-cw';
  const c = Number(code);
  if (c === 0) return isDay ? 'sun' : 'moon';
  if (c === 1 || c === 2) return isDay ? 'cloud-sun' : 'cloud-moon';
  if (c === 3) return 'cloudy';
  if (c === 45 || c === 48) return 'cloud-fog';
  if (c >= 51 && c <= 57) return 'cloud-drizzle';
  if (c === 66 || c === 67) return 'cloud-hail';
  if (c >= 61 && c <= 65) return 'cloud-rain';
  if (c >= 80 && c <= 82) return isDay ? 'cloud-sun-rain' : 'cloud-moon-rain';
  if (c === 77) return 'snowflake';
  if ((c >= 71 && c <= 75) || c === 85 || c === 86) return 'cloud-snow';
  if (c === 95 || c === 96 || c === 99) return 'cloud-lightning';
  return 'cloudy';
}

// An SVG arc (endpoint form, circular: Lucide's are all rx = ry, no rotation) as canvas.arc
// (centre form), SVG implementation notes F.6.5. A sweep flag of 1 turns clockwise on screen.
function svgArc(p, x1, y1, r, large, sweep, x2, y2) {
  const dx = (x1 - x2) / 2, dy = (y1 - y2) / 2;
  const d2 = dx * dx + dy * dy;
  if (d2 === 0) return;
  let rr = Math.abs(r);
  if (d2 > rr * rr) rr = Math.sqrt(d2);  // too small a radius: scale it up (F.6.6)
  let k = Math.sqrt(Math.max(0, (rr * rr - d2) / d2));
  if (large === sweep) k = -k;
  const cx = k * dy + (x1 + x2) / 2, cy = -k * dx + (y1 + y2) / 2;
  p.arc(cx, cy, rr, Math.atan2(y1 - cy, x1 - cx), Math.atan2(y2 - cy, x2 - cx), sweep === 0);
}

// SVG path data (M L H V C A Z, absolute and relative, implicit repeats) -> a canvas path.
function svgPath(canvas, d) {
  const p = canvas.path();
  const tok = d.match(/[a-zA-Z]|-?\d*\.?\d+(?:e[-+]?\d+)?/g) || [];
  let i = 0, cmd = '', x = 0, y = 0, sx = 0, sy = 0;
  const num = () => parseFloat(tok[i++]);
  while (i < tok.length) {
    if (/[a-zA-Z]/.test(tok[i])) cmd = tok[i++];
    else if (cmd === 'M') cmd = 'L';
    else if (cmd === 'm') cmd = 'l';
    const rel = cmd !== cmd.toUpperCase();
    switch (cmd.toUpperCase()) {
      case 'M': { const nx = num(), ny = num(); x = rel ? x + nx : nx; y = rel ? y + ny : ny; p.moveTo(x, y); sx = x; sy = y; break; }
      case 'L': { const nx = num(), ny = num(); x = rel ? x + nx : nx; y = rel ? y + ny : ny; p.lineTo(x, y); break; }
      case 'H': { const nx = num(); x = rel ? x + nx : nx; p.lineTo(x, y); break; }
      case 'V': { const ny = num(); y = rel ? y + ny : ny; p.lineTo(x, y); break; }
      case 'C': {
        const a = num(), b = num(), c = num(), e = num(), f = num(), g = num();
        const ox = rel ? x : 0, oy = rel ? y : 0;
        p.bezierTo(ox + a, oy + b, ox + c, oy + e, ox + f, oy + g);
        x = ox + f; y = oy + g;
        break;
      }
      case 'A': {
        const r = num(); num(); num();  // ry and the rotation (circular, unrotated)
        const large = num(), sweep = num(), nx = num(), ny = num();
        const ex = rel ? x + nx : nx, ey = rel ? y + ny : ny;
        svgArc(p, x, y, r, large, sweep, ex, ey);
        x = ex; y = ey;
        break;
      }
      case 'Z': p.close(); x = sx; y = sy; break;
      default: return p;  // unknown command: stop here
    }
  }
  return p;
}

const LUCIDE_PATHS = new Map();  // name -> [[path, tint]]
function lucidePaths(canvas, name) {
  let paths = LUCIDE_PATHS.get(name);
  if (!paths) {
    paths = (LUCIDE[name] || LUCIDE.cloudy).map(([d, tint]) => [svgPath(canvas, d), tint]);
    LUCIDE_PATHS.set(name, paths);
  }
  return paths;
}

// --- Meteocons -----------------------------------------------------------------------------------

// WMO code -> Meteocons icon (null for the pin and the refresh arrow, which Lucide draws).
function meteoconName(code, isDay) {
  if (code === 'setLocation' || code === 'refresh') return null;
  const c = Number(code), dn = isDay ? 'day' : 'night';
  if (c === 0) return 'clear-' + dn;
  if (c === 1 || c === 2) return 'partly-cloudy-' + dn;
  if (c === 3) return 'overcast';
  if (c === 45 || c === 48) return 'fog-' + dn;
  if (c >= 51 && c <= 55) return 'drizzle';
  if (c === 56 || c === 57 || c === 66 || c === 67) return 'sleet';
  if (c >= 61 && c <= 65) return 'rain';
  if ((c >= 71 && c <= 75) || c === 77) return 'snow';
  if (c >= 80 && c <= 82) return 'partly-cloudy-' + dn + '-rain';
  if (c === 85 || c === 86) return 'partly-cloudy-' + dn + '-snow';
  if (c === 95) return 'thunderstorms-' + dn;
  if (c === 96 || c === 99) return 'thunderstorms-' + dn + '-rain';
  return 'cloudy';
}

const SVG_PATHS = new Map();  // path data -> canvas path (built once; a path's geometry is kept)
function cachedPath(canvas, d) {
  let p = SVG_PATHS.get(d);
  if (!p) {
    p = svgPath(canvas, d);
    SVG_PATHS.set(d, p);
  }
  return p;
}

// A track of the converter's shape at scale s (icon units -> DIPs) as canvas.layer() wants it.
function trackOf(t, s) {
  const px = t.p === 'x' || t.p === 'y';
  const track = { prop: t.p, duration: t.d, begin: t.b, once: !!t.once, keys: t.k.map(([time, v]) => [time, px ? v * s : v]) };
  if (t.e) track.easing = t.e;
  if (t.c) track.center = [t.c[0] * s, t.c[1] * s];
  return track;
}

// The still pose of tracks the host was not given (Animated icons off): their first keys.
function applyPose(canvas, tracks, s) {
  for (const t of tracks) {
    const v = t.k[0][1];
    if (t.p === 'x') canvas.translate(v * s, 0);
    else if (t.p === 'y') canvas.translate(0, v * s);
    else if (t.p === 'rotate') canvas.rotate((v * Math.PI) / 180, (t.c ? t.c[0] : 0) * s, (t.c ? t.c[1] : 0) * s);
    else if (t.p === 'opacity') canvas.opacity(v);
  }
}

function drawMeteoconElements(canvas, els, grads) {
  const paintOf = (ref) => {
    if (!ref) return null;
    if (ref.startsWith('g:')) {
      const g = grads[ref.slice(2)];
      return g ? canvas.linearGradient(g[0], g[1], g[2], g[3], g[4]) : '#888888';
    }
    return ref;
  };
  for (const el of els) {
    const op = el[el.length - 1];
    if (op < 1) {
      canvas.save();
      canvas.opacity(op);
    }
    if (el[0] === 'p') {
      const [, d, fill, stroke, sw, cap, join] = el;
      const path = cachedPath(canvas, d);
      if (fill) canvas.fill(path, paintOf(fill));
      if (stroke) canvas.stroke(path, paintOf(stroke), { width: sw, cap, join });
    } else {
      const [, cx, cy, r, fill, stroke, sw] = el;
      if (fill) canvas.fillCircle(cx, cy, r, paintOf(fill));
      if (stroke) canvas.strokeCircle(cx, cy, r, paintOf(stroke), sw);
    }
    if (op < 1) canvas.restore();
  }
}

// The icon `name` in the square at (x, y): a clipped group layer (its opacity is `dim`), then a
// layer per part under it (a clipped sub-group for the fog's masked sun). Layer ids come from
// `key` (the slot's place for this icon: "cur", "d2"), so an icon that stays the same across
// redraws keeps its loops running without a restart.
function drawMeteocon(canvas, dock, name, x, y, size, dim, key) {
  const icon = METEO[name] || METEO.cloudy;
  const s = size / METEO_SIZE;
  const motion = settings.iconMotion !== false;
  const group = 'm:' + key;
  canvas.layer(group, x, y, size, size, { clip: true, opacity: dim });
  canvas.endLayer();
  let n = 0;
  const drawParts = (parts, parent, ox, oy) => {
    for (const p of parts) {
      const id = group + '/' + n++;
      if (p.ch) {
        const [cx, cy, cw, ch] = p.clip;
        canvas.layer(id, ox + cx * s, oy + cy * s, cw * s, ch * s, { parent, clip: true });
        canvas.endLayer();
        drawParts(p.ch, id, -cx * s, -cy * s);
        continue;
      }
      const opts = { parent };
      if (p.tr && motion) opts.tracks = p.tr.map((t) => trackOf(t, s));
      canvas.layer(id, ox, oy, size, size, opts);
      canvas.save();
      if (p.tr && !motion) applyPose(canvas, p.tr, s);
      canvas.scaleBy(s, s);
      drawMeteoconElements(canvas, p.el, icon.g);
      canvas.restore();
      canvas.endLayer();
    }
  };
  drawParts(icon.parts, group, 0, 0);
}

// Draws the condition icon of `code` ("setLocation" / "refresh" too) into the square at (x, y)
// with the chosen pack and colours. `ink` overrides the ink (the popup's dimmed buttons). `key`
// names the icon's place for the Meteocons pack's layers (unique per icon drawn).
function drawIcon(canvas, dock, code, isDay, x, y, size, dim = 1, ink, key = 'icon') {
  ink = ink || dock.theme.text;
  const mono = settings.iconStyle === 'mono';
  const meteocon = settings.iconPack === 'meteocons' ? meteoconName(code, isDay) : null;
  if (meteocon) {
    drawMeteocon(canvas, dock, meteocon, x, y, size, dim, key);
    return;
  }
  if (settings.iconPack !== 'lucide' && settings.iconPack !== 'meteocons') {
    canvas.image(weather.icon(code, isDay, mono ? { ink, water: ink } : { ink }), x, y, size, size, dim);
    return;
  }
  const tints = { '': ink, s: mono ? ink : SUN, w: mono ? ink : dock.theme.accent, b: mono ? ink : SUN };
  const s = size / 24;
  canvas.save();
  canvas.opacity(dim);
  canvas.translate(x, y);
  canvas.scaleBy(s, s);
  for (const [path, tint] of lucidePaths(canvas, lucideName(code, isDay))) {
    canvas.stroke(path, tints[tint], { width: 2, cap: 'round', join: 'round' });
  }
  canvas.restore();
}

// --- the slot ------------------------------------------------------------------------------------

// As C++ std::lround (halves away from zero; JS Math.round rounds -0.5 up).
const lround = (x) => (x < 0 ? -Math.round(-x) : Math.round(x));
const tempText = (t) => (t === null || t === undefined ? '--°' : `${lround(t) || 0}°`);
const fresh = () => wx.state === 'fresh';

// settings.dockForecast (round 7 phase 7C): "none", "today" (today's high / low under the
// temperature, as showHigh + showLow) or "days" (dockDays day cells after / under the current
// conditions).
const forecastMode = () => settings.dockForecast || 'none';

// The presets of `layout` (16D): how the current conditions are arranged (the icon beside or
// above the temperature), what a day's cell is, and where the cells go. "auto" of each field
// follows the dock's edge: beside / column / after on a top or bottom dock (and a desktop
// widget), stacked / row / under on a left or right dock, which is what rounds 4 - 7 drew.
const PRESETS = {
  auto: { conditions: 'auto', cell: 'auto', placement: 'auto' },
  strip: { conditions: 'beside', cell: 'column', placement: 'beside' },
  tiles: { conditions: 'auto', cell: 'tile', placement: 'auto' },
  list: { conditions: 'stacked', cell: 'row', placement: 'below' },
  card: { conditions: 'stacked', cell: 'tile', placement: 'below' },
};

function arrangement(dock) {
  const vertical = dock.axis === 'vertical';
  const p = settings.layout === 'custom'
    ? { conditions: settings.conditions || 'auto', cell: settings.dayCell || 'auto', placement: settings.forecastPlacement || 'auto' }
    : PRESETS[settings.layout] || PRESETS.auto;
  const conditions = p.conditions === 'auto' ? (vertical ? 'stacked' : 'beside') : p.conditions;
  const placement = p.placement === 'auto' ? (vertical ? 'below' : 'beside') : p.placement;
  const cell = p.cell === 'auto' ? (vertical ? 'row' : 'column') : p.cell;
  // Lines always stack; tiles and icon cells stack too when they go under the conditions on a
  // narrow (vertical) dock; columns are always a row of them (that is what a column is for).
  const stack = cell === 'row' || (vertical && placement === 'below' && cell !== 'column');
  return { vertical, conditions, placement, cell, stack };
}

// A high / low pair as two runs: [the high, the separator + the low] (the low in the dimmer
// colour). `where`: "under" (the line under the temperature) or "cell" (a day cell's line).
function rangeRuns(hi, lo, where) {
  const style = !settings.rangeStyle || settings.rangeStyle === 'auto' ? (where === 'under' ? 'arrows' : 'slash') : settings.rangeStyle;
  if (style === 'arrows') return ['↑' + hi, ' ↓' + lo];
  if (style === 'slash') return [hi, '/' + lo];
  return [hi, ' ' + lo];
}

function dayName(d, i, cell) {
  const mode = !settings.dayNames || settings.dayNames === 'auto' ? (cell === 'row' ? 'initial' : 'short') : settings.dayNames;
  if (i === 0 && settings.todayLabel !== false && mode !== 'initial') return 'Today';
  const name = (mode === 'full' ? system.dayNames[d.weekday] : system.shortDayNames[d.weekday]) || '?';
  return mode === 'initial' ? name.charAt(0) : name;
}

// The text lines of the current conditions: the temperature, then the extra lines (the label,
// today's high / low, the place, the condition's name). Beside the icon the small parts share one
// line; above / below it (stacked) each part is a line of its own.
function slotLines(dock, A) {
  if (!wx.configured || settings.compact) return { main: '', extra: [] };
  const main = wx.current ? tempText(wx.current.temp) : '--°';
  const today = wx.daily && wx.daily[0];
  const todayMode = forecastMode() === 'today';
  const parts = [];
  if (settings.label) parts.push(settings.label);
  const hi = (settings.showHigh || todayMode) && today, lo = (settings.showLow || todayMode) && today;
  if (hi && lo) parts.push(rangeRuns(tempText(today.max), tempText(today.min), 'under').join(''));
  else if (hi) parts.push('↑' + tempText(today.max));
  else if (lo) parts.push('↓' + tempText(today.min));
  if (settings.showPlace && wx.place) parts.push(wx.place);
  const extra = A.conditions === 'stacked' ? parts : (parts.length ? [parts.join('  ')] : []);
  if (settings.showCondition && wx.current && wx.current.description) extra.push(wx.current.description);
  return { main, extra };
}

const mainStyle = (dock, k = 1) => ({ size: (dock.clockDate ? FONT - 2 : FONT) * k, weight: 'semibold' });
const EXTRA_FONT = 10;

// The day cells: a column (the weekday on top, the icon, the high over the low), a tile (the
// weekday over the icon on the left, the high over the low on the right), a line (the weekday,
// the icon, "high/low") or the icon with "high/low".
const DAY_GLYPH = 16, ROW_GLYPH = 14;  // DIPs
const DAY_FONT = 9, DAY_LINE = 10;      // the cells' text size and line pitch
const DAY_GAP = 6;                      // between cells of a row (settings.dayGap, 16D)
const TILE_GAP = 4;                     // a tile's left half -> its right half
const FORECAST_GAP = 8;                 // the current conditions -> the cells (settings.forecastGap; half when under)
const DAY_ROW_H = 16;

// 16D: the day cells have a scale of their own (settings.daySize) over the slot's, and the gaps
// are settings, so a forecast can be spread out and read larger than the current conditions.
const clampPct = (v, d) => Math.max(0.6, Math.min(2.5, (Number.isFinite(v) ? v : d) / 100));
const dayK = (k) => k * clampPct(settings.daySize, 100);
const dayGap = (k) => Math.round((Number.isFinite(settings.dayGap) ? settings.dayGap : DAY_GAP) * k);
const forecastGap = (k) => Math.round((Number.isFinite(settings.forecastGap) ? settings.forecastGap : FORECAST_GAP) * k);

const dayDeg = (t) => (t === null || t === undefined ? '–' : (lround(t) || 0) + '°');
const dayNum = (t) => (t === null || t === undefined ? '–' : String(lround(t) || 0));

function forecast(canvas, dock, k0, A) {
  if (forecastMode() !== 'days' || !wx.configured || !wx.daily || !wx.daily.length) return null;
  const k = dayK(k0);  // the cells' own scale; the gap between cells follows the slot's
  const days = wx.daily.slice(0, Math.max(2, Math.min(7, settings.dockDays || 3)));
  const label = { size: DAY_FONT * k, weight: 'semibold' };
  const small = { size: DAY_FONT * k };
  const width = (s, st) => canvas.measureText(s, st).width;
  const cell = A.cell;
  const g = (cell === 'row' || cell === 'icon' ? ROW_GLYPH : DAY_GLYPH) * k;
  const line = DAY_LINE * k, gap = Math.round(2 * k);
  const cells = days.map((d, i) => {
    const name = cell === 'icon' ? '' : dayName(d, i, cell);
    const c = { d, name, nameW: name ? Math.ceil(width(name, label)) : 0 };
    if (cell === 'column' || cell === 'tile') {
      c.hi = dayDeg(d.max);
      c.lo = dayDeg(d.min);
      c.hiW = Math.ceil(width(c.hi, small));
      c.loW = Math.ceil(width(c.lo, small));
    } else {
      [c.hi, c.lo] = rangeRuns(dayNum(d.max), dayNum(d.min), 'cell');
      c.hiW = Math.ceil(width(c.hi, small));
      c.loW = Math.ceil(width(c.lo, small));
    }
    if (cell === 'column') {
      c.w = Math.max(c.nameW, c.hiW, c.loW, g);
      c.h = Math.ceil(3 * line + g);
    } else if (cell === 'tile') {
      c.leftW = Math.max(c.nameW, g);
      c.w = c.leftW + Math.round(TILE_GAP * k) + Math.max(c.hiW, c.loW);
      c.h = Math.ceil(line + g);
    } else {
      c.w = (c.nameW ? c.nameW + gap : 0) + g + gap + c.hiW + c.loW;
      c.h = Math.ceil(DAY_ROW_H * k);
    }
    return c;
  });
  // Lines share one name column so the icons line up.
  if (cell === 'row') {
    const nameW = Math.max(...cells.map((c) => c.nameW));
    for (const c of cells) { c.nameW = nameW; c.w = nameW + gap + g + gap + c.hiW + c.loW; }
  }
  let w, h;
  if (A.stack) {
    w = Math.max(...cells.map((c) => c.w));
    h = cells.reduce((s, c) => s + c.h, 0);
  } else {
    w = cells.reduce((s, c) => s + c.w, 0) + (cells.length - 1) * dayGap(k0);
    h = Math.max(...cells.map((c) => c.h));
  }
  return { cells, w, h, g, line, gap, label, small, cell, stack: A.stack, k, cellGap: dayGap(k0) };
}

// The slot's parts at scale k (1 on a dock, settings.size applied by sizeK).
function layout(canvas, dock, k = sizeK(dock)) {
  const A = arrangement(dock);
  const glyph = (dock.clockDate ? GLYPH - 2 : GLYPH) * k;
  const t = slotLines(dock, A);
  const a = t.main ? canvas.measureText(t.main, mainStyle(dock, k)) : { width: 0, height: 0 };
  const extraStyle = { size: EXTRA_FONT * k };
  const extras = t.extra.map((s) => ({ s, m: canvas.measureText(s, extraStyle) }));
  const textW = Math.ceil(Math.max(a.width, ...extras.map((e) => e.m.width)));
  const textH = Math.ceil(a.height) + extras.reduce((s, e) => s + Math.ceil(e.m.height), 0);
  const gap = textW > 0 ? GAP * k : 0;
  const stacked = A.conditions === 'stacked';
  const cw = stacked ? Math.max(glyph, textW) : glyph + gap + textW;  // the current conditions
  const ch = stacked ? glyph + gap + textH : Math.max(glyph, textH);
  const f = forecast(canvas, dock, k, A);
  let w = cw, h = ch;
  if (f && A.placement === 'below') {
    w = Math.max(cw, f.w);
    h = ch + Math.round(forecastGap(k) / 2) + f.h;
  } else if (f) {
    w = cw + forecastGap(k) + f.w;
    h = Math.max(ch, f.h);
  }
  return { A, t, a, extras, extraStyle, glyph, gap, cw, ch, textW, textH, f, w, h, vertical: A.vertical, k };
}

const sizeK = (dock) => (dock.standalone ? 1 : clampPct(settings.size, 100));

function drawForecast(canvas, dock, l, x, y, dim) {
  const f = l.f, k = f.k;  // the cells' scale (TILE_GAP goes with it; the gap between cells is f.cellGap)
  const text = dock.theme.text, low = dock.theme.dateText;
  const centred = (s, cx, cy, st, color) => canvas.text(s, cx, cy, { ...st, align: 'center', baseline: 'middle', color });
  const icon = (c, ix, iy, g) => {
    if (c.d.code !== null && c.d.code >= 0) drawIcon(canvas, dock, c.d.code, true, Math.round(ix), Math.round(iy), g, dim, undefined, 'd' + f.cells.indexOf(c));
  };
  canvas.save();
  canvas.opacity(dim);
  const g = f.g, line = f.line;
  let cx = x, cy = y;
  for (const c of f.cells) {
    // Each cell is drawn in its own box (c.w x c.h); a row of cells centres them on the row.
    const top = f.stack ? cy : cy + Math.round((f.h - c.h) / 2);
    if (f.cell === 'column') {
      const mid = cx + c.w / 2;
      centred(c.name, mid, top + line / 2, f.label, text);
      icon(c, mid - g / 2, top + line, g);
      centred(c.hi, mid, top + line + g + line / 2, f.small, text);
      centred(c.lo, mid, top + 2 * line + g + line / 2, f.small, low);
    } else if (f.cell === 'tile') {
      const mid = cx + c.leftW / 2;
      centred(c.name, mid, top + line / 2, f.label, text);
      icon(c, mid - g / 2, top + line, g);
      const rx = cx + c.leftW + Math.round(TILE_GAP * k);
      const rh = 2 * line;
      const rtop = top + (c.h - rh) / 2;
      canvas.text(c.hi, rx, rtop + line / 2, { ...f.small, baseline: 'middle', color: text });
      canvas.text(c.lo, rx, rtop + line + line / 2, { ...f.small, baseline: 'middle', color: low });
    } else {
      const my = top + c.h / 2;
      let tx = cx;
      if (c.nameW) {
        centred(c.name, tx + c.nameW / 2, my, f.label, text);
        tx += c.nameW + f.gap;
      }
      icon(c, tx, my - g / 2, g);
      tx += g + f.gap;
      canvas.text(c.hi, tx, my, { ...f.small, baseline: 'middle', color: text });
      canvas.text(c.lo, tx + c.hiW, my, { ...f.small, baseline: 'middle', color: low });
    }
    if (f.stack) cy += c.h;
    else cx += c.w + f.cellGap;
  }
  canvas.restore();
}

// The current conditions block at (x, y), cw x ch.
function drawConditions(canvas, dock, l, x, y, dim) {
  const code = !wx.configured ? 'setLocation' : wx.current ? wx.current.code : 3;  // no data yet: overcast
  const isDay = !wx.current || wx.current.isDay;
  const color = dock.theme.text;
  const lineY = (ty) => {
    canvas.save();
    canvas.opacity(dim);
    if (l.t.main) {
      const tx = l.A.conditions === 'stacked' ? Math.round(x + (l.cw - l.a.width) / 2) : x + l.glyph + l.gap;
      canvas.text(l.t.main, tx, ty, { ...mainStyle(dock, l.k), color });
    }
    canvas.restore();
    let ey = ty + Math.ceil(l.a.height);
    for (const e of l.extras) {
      if (l.A.conditions === 'stacked') canvas.text(e.s, x + l.cw / 2, ey, { ...l.extraStyle, align: 'center', color: dock.theme.dateText });
      else canvas.text(e.s, x + l.glyph + l.gap, ey, { ...l.extraStyle, color: dock.theme.dateText });
      ey += Math.ceil(e.m.height);
    }
  };
  if (l.A.conditions === 'stacked') {
    drawIcon(canvas, dock, code, isDay, Math.round(x + (l.cw - l.glyph) / 2), Math.round(y), l.glyph, dim, undefined, 'cur');
    if (l.t.main || l.extras.length) lineY(y + l.glyph + l.gap);
  } else {
    drawIcon(canvas, dock, code, isDay, Math.round(x), Math.round(y + (l.ch - l.glyph) / 2), l.glyph, dim, undefined, 'cur');
    if (l.t.main || l.extras.length) lineY(Math.round(y + (l.ch - l.textH) / 2));
  }
}

// Standalone (a window of its own, round 6 phase 6B): the slot's layout grown to fill the window.
function fitLayout(canvas, dock) {
  const l = layout(canvas, dock);
  if (!dock.standalone) return l;
  const k = Math.max(0.5, Math.min((canvas.width * 0.9) / Math.max(1, l.w), (canvas.height * 0.9) / Math.max(1, l.h)));
  return layout(canvas, dock, k);
}

export function measure(canvas, dock) {
  const l = layout(canvas, dock);
  return l.vertical ? { main: l.h, cross: l.w } : { main: l.w, cross: l.h };
}

export function draw(canvas, dock) {
  const l = fitLayout(canvas, dock);
  const dim = wx.configured && (!wx.current || !fresh()) ? STALE : 1;
  // Test hook (an item setting, not in the schema): what the slot shows, into the storage.
  if (settings.debugStore) {
    storage.set('slot', { state: wx.state, temp: wx.current ? wx.current.temp : null, code: wx.current ? wx.current.code : null, dim,
      text: l.t.main, extra: l.t.extra, w: l.w, h: l.h, days: l.f ? l.f.cells.length : 0,
      conditions: l.A.conditions, cell: l.A.cell, placement: l.A.placement, stack: l.A.stack, pack: settings.iconPack || 'windock' });
  }
  // The block is centred in the box; on a dock the box is the block (measure), so this is 0, 0
  // along the dock and the centring across it; standalone the window.
  const ox = Math.round((canvas.width - l.w) / 2), oy = Math.round((canvas.height - l.h) / 2);
  if (l.f && l.A.placement === 'below') {
    drawConditions(canvas, dock, l, ox + Math.round((l.w - l.cw) / 2), oy, dim);
    drawForecast(canvas, dock, l, ox + Math.round((l.w - l.f.w) / 2), oy + l.ch + Math.round(forecastGap(l.k) / 2), dim);
  } else if (l.f) {
    drawConditions(canvas, dock, l, ox, oy + Math.round((l.h - l.ch) / 2), dim);
    drawForecast(canvas, dock, l, ox + l.cw + forecastGap(l.k), oy + Math.round((l.h - l.f.h) / 2), dim);
  } else {
    drawConditions(canvas, dock, l, ox, oy, dim);
  }
}

export function onClick(e) {
  if (e.closedPopup) return;  // the press closed the popup: a click toggles it
  if (!wx.configured) {
    weather.openSettings();   // nothing to show yet: choose a place
    return;
  }
  if (settings.clickOpens === 'settings') weather.openSettings();
  else if (settings.clickOpens === 'popup') showPopup();
}

// --- the popup -----------------------------------------------------------------------------------

const PAD = 14;
const TITLE_TOP = 12, TITLE_H = 22;
const MAIN_TOP = 38;
const BIG_GLYPH = 50;
const RIGHT_W = 180;
const RULE1 = 100;
const HOURLY_TOP = 108, HOURLY_H = 92, RULE_GAP = 6;
const ROW_H = 24;
const BOTTOM = 12;
const BUTTON = 28;
const MESSAGE_W = 320;

let popupShown = false;
let hits = [];            // [{ x, y, w, h, what }] of the last drawPopup
let minuteTimer = 0;      // "N min ago" while the popup is open

const hourCount = () => Math.max(12, Math.min(24, settings.hours || 24));
const dayCount = () => Math.max(3, Math.min(7, settings.days || 7));

function popupSize() {
  if (!wx.current) {
    // No data: the message (and the settings button when no place is chosen).
    return { w: MESSAGE_W, h: MAIN_TOP + 40 + (wx.configured ? 0 : 28) + BOTTOM };
  }
  const days = Math.min(dayCount(), (wx.daily || []).length);
  return { w: 2 * PAD + 24 * 21, h: HOURLY_TOP + HOURLY_H + 2 * RULE_GAP + days * ROW_H + BOTTOM };
}

let shownSize = null;
function showPopup() {
  const s = popupSize();
  if (popupShown && shownSize && shownSize.w === s.w && shownSize.h === s.h) {
    popup.redraw();
  } else {
    popup.open(s.w, s.h);  // opening again resizes and redraws
  }
  shownSize = s;
  popupShown = true;
}

export function onPopupClose() {
  popupShown = false;
  shownSize = null;
  if (minuteTimer) clearTimeout(minuteTimer);
  minuteTimer = 0;
}

// "#AARRGGBB" with its alpha multiplied.
function fade(color, k) {
  const m = /^#([0-9a-f]{2})([0-9a-f]{6})$/i.exec(color);
  if (!m) return color;
  const a = Math.min(255, Math.round(parseInt(m[1], 16) * k));
  return '#' + a.toString(16).padStart(2, '0').toUpperCase() + m[2];
}

function ago(seconds) {
  if (seconds < 0) return '';
  const min = Math.floor(seconds / 60);
  if (min < 1) return 'Updated just now';
  if (min < 60) return `Updated ${min} min ago`;
  const h = Math.floor(min / 60);
  return h < 24 ? `Updated ${h} h ${min % 60} min ago` : `Updated ${wx.updated}`;
}

function message() {
  if (!wx.configured) return 'No location is set.';
  if (wx.busy) return 'Loading the forecast…';
  if (wx.error) return 'Weather unavailable: ' + wx.error;
  return 'No weather data yet.';
}

function hourLabel(h) {
  if (!system.hour12) return String(h);
  return (h % 12 === 0 ? 12 : h % 12) + (h < 12 ? 'a' : 'p');
}

// Text vertically centred in [top, top + height] (the native popup's paragraph alignment).
function line(canvas, text, x, top, height, style) {
  if (text) canvas.text(text, x, top + height / 2, { ...style, baseline: 'middle' });
}

export function drawPopup(canvas, dock) {
  const w = canvas.width;
  const text = dock.theme.text;
  const secondary = fade(text, 0.62);
  const water = dock.theme.accent;
  const border = dock.theme.border;
  hits = [];

  // Title row: the place, the settings and refresh buttons.
  const refresh = { x: w - PAD - BUTTON, y: 9, w: BUTTON, h: BUTTON, what: 'refresh' };
  const gear = { x: refresh.x - 4 - BUTTON, y: 9, w: BUTTON, h: BUTTON, what: 'settings' };
  line(canvas, settings.label || wx.place || 'Weather', PAD, TITLE_TOP, TITLE_H, { size: 14, weight: 'semibold', color: text });
  canvas.fillRoundRect(gear.x, gear.y, gear.w, gear.h, 6, fade(text, 0.08));
  canvas.text('', gear.x + gear.w / 2, gear.y + gear.h / 2, { font: 'Segoe Fluent Icons', size: 14, align: 'center', baseline: 'middle', color: secondary });
  hits.push(gear);
  if (wx.configured) {
    canvas.fillRoundRect(refresh.x, refresh.y, refresh.w, refresh.h, 6, fade(text, 0.08));
    const g = 16;
    drawIcon(canvas, dock, 'refresh', true, refresh.x + (BUTTON - g) / 2, refresh.y + (BUTTON - g) / 2, g, 1, wx.busy ? fade(text, 0.4) : secondary, 'pr');
    hits.push(refresh);
  }

  if (!wx.current) {
    if (settings.debugStore) storage.set('popup', { message: message(), width: w, height: canvas.height, state: wx.state });
    line(canvas, message(), PAD, MAIN_TOP, 28, { size: 13, color: secondary, maxWidth: w - 2 * PAD });
    if (!wx.configured) {
      const label = 'Weather settings…';
      const bw = Math.ceil(canvas.measureText(label, { size: 13 }).width) + 24;
      const b = { x: Math.round((w - bw) / 2), y: MAIN_TOP + 40, w: bw, h: 28, what: 'settings' };
      canvas.fillRoundRect(b.x, b.y, b.w, b.h, 6, fade(text, 0.12));
      line(canvas, label, b.x + b.w / 2, b.y, b.h, { size: 13, align: 'center', color: text });
      hits.push(b);
    }
    return;
  }

  // Header: glyph, temperature, condition, details; "Updated" and the credit on the right.
  const c = wx.current;
  const stale = wx.state !== 'fresh';
  drawIcon(canvas, dock, c.code, c.isDay, PAD, MAIN_TOP + 2, BIG_GLYPH, stale ? STALE : 1, undefined, 'pc');
  const unit = wx.temperatureUnit;
  const temp = (c.temp === null ? '–' : (lround(c.temp) || 0) + '°') + unit;
  const tempX = PAD + BIG_GLYPH + 8;
  const tempStyle = { size: 36, weight: 'light', color: stale ? fade(text, STALE) : text };
  const tempW = Math.ceil(canvas.measureText(temp, tempStyle).width);
  line(canvas, temp, tempX, MAIN_TOP, 54, tempStyle);
  const rightCol = w - PAD - RIGHT_W;
  const condX = tempX + tempW + 14;
  line(canvas, c.description, condX, MAIN_TOP + 6, 20, { size: 13, color: text });
  const details = [];
  if (c.humidity !== null) details.push(`Humidity ${lround(c.humidity)}%`);
  if (c.wind !== null) details.push(`Wind ${lround(c.wind)} ${wx.windUnit}`);
  line(canvas, details.join(' · '), condX, MAIN_TOP + 28, 18, { size: 12, color: secondary });
  line(canvas, ago(wx.age) + (wx.state === 'stale' ? ' (stale)' : ''), w - PAD, MAIN_TOP + 6, 18, { size: 12, align: 'right', color: secondary });
  const creditW = Math.ceil(canvas.measureText(CREDIT, { size: 12 }).width);
  const credit = { x: w - PAD - creditW, y: MAIN_TOP + 28, w: creditW, h: 18, what: 'credit' };
  line(canvas, CREDIT, w - PAD, credit.y, credit.h, { size: 12, align: 'right', color: secondary });
  hits.push(credit);
  canvas.line(PAD, RULE1 - 0.5, w - PAD, RULE1 - 0.5, border);

  // Hourly strip.
  const hours = (wx.hourly || []).slice(0, hourCount());
  const colW = (w - 2 * PAD) / hours.length;
  const top = HOURLY_TOP;
  const temps = hours.map((h) => h.temp).filter((t) => t !== null);
  const lo = Math.min(...temps), hi = Math.max(...temps);
  const bandTop = top + 54, bandBottom = top + 74;
  const bandY = (t) => (hi > lo ? bandBottom - ((t - lo) / (hi - lo)) * (bandBottom - bandTop) : (bandTop + bandBottom) / 2);
  const path = canvas.path();
  let first = true;
  hours.forEach((h, i) => {
    if (h.temp === null) return;
    const x = PAD + (i + 0.5) * colW, y = bandY(h.temp);
    if (first) path.moveTo(x, y); else path.lineTo(x, y);
    first = false;
  });
  if (!first) canvas.stroke(path, fade(water, 0.9), 1.5);
  hours.forEach((h, i) => {
    const x = PAD + i * colW, cx = x + colW / 2;
    line(canvas, i === 0 ? 'Now' : hourLabel(h.hour), cx, top, 14, { size: 10.5, align: 'center', color: i === 0 ? text : secondary });
    if (h.code >= 0) drawIcon(canvas, dock, h.code, h.isDay, cx - 8, top + 17, 16, 1, undefined, 'ph' + i);
    line(canvas, h.temp === null ? '–' : (lround(h.temp) || 0) + '°', cx, top + 35, 15, { size: 10.5, align: 'center', color: text });
    if (h.temp !== null) canvas.fillCircle(cx, bandY(h.temp), 1.8, water);
    if (h.precip !== null && h.precip >= 1) {
      line(canvas, lround(h.precip) + '%', cx, top + 77, 13, { size: 9.5, align: 'center', color: water });
    }
  });
  canvas.line(PAD, HOURLY_TOP + HOURLY_H + RULE_GAP / 2 - 0.5, w - PAD, HOURLY_TOP + HOURLY_H + RULE_GAP / 2 - 0.5, border);

  // Daily rows: name, glyph, low, the week's range bar with this day's span, high.
  const days = (wx.daily || []).slice(0, dayCount());
  const weekLo = Math.min(...days.map((d) => d.min).filter((t) => t !== null));
  const weekHi = Math.max(...days.map((d) => d.max).filter((t) => t !== null));
  const dailyTop = HOURLY_TOP + HOURLY_H + 2 * RULE_GAP;
  const trackL = PAD + 158, trackR = w - PAD - 58, trackH = 4, r = trackH / 2;
  const gradient = canvas.linearGradient(trackL, 0, trackR, 0, [[0, water], [1, SUN]]);
  days.forEach((d, i) => {
    const y = dailyTop + i * ROW_H, cy = y + ROW_H / 2;
    line(canvas, i === 0 ? 'Today' : system.shortDayNames[d.weekday], PAD, y, ROW_H, { size: 13, color: text });
    if (d.code >= 0) drawIcon(canvas, dock, d.code, true, PAD + 74, cy - 9, 18, 1, undefined, 'pd' + i);
    line(canvas, d.min === null ? '–' : (lround(d.min) || 0) + '°', PAD + 148, y, ROW_H, { size: 13, align: 'right', color: secondary });
    line(canvas, d.max === null ? '–' : (lround(d.max) || 0) + '°', w - PAD, y, ROW_H, { size: 13, align: 'right', color: text });
    canvas.fillRoundRect(trackL, cy - r, trackR - trackL, trackH, r, fade(border, 1.6));
    if (d.min !== null && d.max !== null && weekHi > weekLo) {
      const a = trackL + ((d.min - weekLo) / (weekHi - weekLo)) * (trackR - trackL);
      const b = trackL + ((d.max - weekLo) / (weekHi - weekLo)) * (trackR - trackL);
      canvas.fillRoundRect(a, cy - r, Math.max(b - a, trackH), trackH, r, gradient);
    }
  });

  if (settings.debugStore) {
    storage.set('popup', { hours: hours.length, days: days.length, state: wx.state, updated: ago(wx.age), width: w, height: canvas.height, temp: c.temp, unit });
  }

  // "N min ago" stays current while the popup is open: one timeout to the next minute of age.
  if (minuteTimer) clearTimeout(minuteTimer);
  minuteTimer = wx.age >= 0 ? setTimeout(() => { minuteTimer = 0; wx = weather.get(); popup.redraw(); }, (60 - (wx.age % 60)) * 1000 + 50) : 0;
}

export function onPopupClick(e) {
  if (e.button !== 'left') return;
  const hit = hits.find((h) => e.x >= h.x && e.x < h.x + h.w && e.y >= h.y && e.y < h.y + h.h);
  if (!hit) return;
  if (hit.what === 'refresh') weather.refresh();  // the dock's one service fetches; onChange follows
  else if (hit.what === 'settings') weather.openSettings();
  else if (hit.what === 'credit') shell.open(CREDIT_URL);
}
