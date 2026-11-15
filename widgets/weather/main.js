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
// Round 33 phase 33A (1.4.0): two more layouts. "trend" puts the coming hours after the current
// conditions as a temperature curve with the chance of rain as bars under it (on a left / right
// dock, where a curve has no room, a thermometer from today's low to its high with a dot at the
// temperature now); "capsules" makes each day a rounded chip tinted by its high (or its
// condition, or the accent) on a scale between two colours of the user's.
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

// Round 28: a time (ms since 1970) on the place's own clock, in the user's 12 / 24 h format. The
// place's offset comes from the hourly data (its `hour` is the place's local hour of `time`).
function placeTime(ms) {
  const h0 = (wx.hourly || [])[0];
  let offsetMin = 0;
  if (h0) {
    const d = new Date(h0.time);
    offsetMin = (((h0.hour * 60 - (d.getUTCHours() * 60 + d.getUTCMinutes())) % 1440) + 1440 + 720) % 1440 - 720;
  }
  const t = new Date(ms + offsetMin * 60000);
  const hh = t.getUTCHours(), mm = String(t.getUTCMinutes()).padStart(2, '0');
  if (system.hour12) return `${(hh % 12) || 12}:${mm} ${hh < 12 ? 'AM' : 'PM'}`;
  return `${String(hh).padStart(2, '0')}:${mm}`;
}
const fresh = () => wx.state === 'fresh';

// settings.dockForecast (round 7 phase 7C): "none", "today" (today's high / low under the
// temperature, as showHigh + showLow) or "days" (dockDays day cells after / under the current
// conditions).
// Round 33 phase 33A: the Capsules layout is day cells by definition, so it shows them whatever
// dockForecast says (and onSettingsChanged turns "Several days" on when Capsules is chosen, so
// "Days on the dock" shows on the page).
const forecastMode = () => (settings.layout === 'capsules' ? 'days' : settings.dockForecast || 'none');

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
  // Round 33 phase 33A: the "cell" is the block after the conditions: the curve of the coming
  // hours (a thermometer on a left / right dock), or a chip per day.
  trend: { conditions: 'auto', cell: 'trend', placement: 'auto' },
  capsules: { conditions: 'auto', cell: 'capsule', placement: 'auto' },
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
  // Round 33 phase 33A: the apparent temperature, the Trend mock-up's second line (any layout).
  if (settings.showFeelsLike && wx.current && wx.current.feelsLike !== null && wx.current.feelsLike !== undefined) {
    parts.push('Feels ' + tempText(wx.current.feelsLike));
  }
  const hi =(settings.showHigh || todayMode) && today, lo = (settings.showLow || todayMode) && today;
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
  const f = A.cell === 'trend' ? trendBlock(canvas, dock, k, A)
    : A.cell === 'capsule' ? capsuleBlock(canvas, dock, k, A)
    : forecast(canvas, dock, k, A);
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

// --- round 33 phase 33A: colours of the new layouts -------------------------------------------------

const COLD = '#FF6CB8FF';  // the temperature scale's ends (the mock-up's sky blue and orange)
const HOT = '#FFFF9F43';

// "#AARRGGBB" / "#RRGGBB" -> [a, r, g, b] (0 - 255), null for anything else.
function argbOf(c) {
  const m = /^#([0-9a-f]{6}|[0-9a-f]{8})$/i.exec(typeof c === 'string' ? c : '');
  if (!m) return null;
  const h = m[1].length === 6 ? 'FF' + m[1] : m[1];
  return [0, 2, 4, 6].map((i) => parseInt(h.slice(i, i + 2), 16));
}
const hex2 = (v) => Math.max(0, Math.min(255, Math.round(v))).toString(16).padStart(2, '0').toUpperCase();
const argbText = (a, r, g, b) => '#' + hex2(a) + hex2(r) + hex2(g) + hex2(b);
// The colour with its alpha multiplied by `a` (0 - 1); fade() below takes "#AARRGGBB" only.
function alpha(c, a) {
  const p = argbOf(c);
  return p ? argbText(p[0] * Math.max(0, Math.min(1, a)), p[1], p[2], p[3]) : c;
}
// A colour setting's value unless it is fully transparent: #00000000 means "automatic", as in
// the other built-in widgets.
function own(c) {
  const p = argbOf(c);
  return p && p[0] > 0 ? c : null;
}
const clampNum = (v, lo, hi, d) => Math.max(lo, Math.min(hi, Number.isFinite(v) ? v : d));

// The colours one draw uses. Phase 33A: the theme's, with the scale's ends from the settings.
function colours(dock) {
  return { text: dock.theme.text, dim: dock.theme.dateText, accent: dock.theme.accent,
    cold: own(settings.coldColor) || COLD, hot: own(settings.hotColor) || HOT };
}

function toHsl(p) {
  const r = p[1] / 255, g = p[2] / 255, b = p[3] / 255;
  const mx = Math.max(r, g, b), mn = Math.min(r, g, b), l = (mx + mn) / 2;
  if (mx === mn) return [0, 0, l];
  const d = mx - mn;
  const s = l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
  const h = mx === r ? (g - b) / d + (g < b ? 6 : 0) : mx === g ? (b - r) / d + 2 : (r - g) / d + 4;
  return [h * 60, s, l];
}
function fromHsl(a, h, s, l) {
  const q = s * Math.min(l, 1 - l);
  const f = (n) => {
    const t = (n + h / 30) % 12;
    return 255 * (l - q * Math.max(-1, Math.min(t - 3, 9 - t, 1)));
  };
  return argbText(a, f(0), f(8), f(4));
}

// The temperature scale: from the cold end at 0 °C (32 °F) to the hot end at 30 °C (86 °F), or
// (tempScale "days") between the lowest and highest of the values shown.
function scaleOf(temps) {
  const t = temps.filter((v) => v !== null && v !== undefined && Number.isFinite(v));
  if (settings.tempScale === 'days' && t.length) {
    const lo = Math.min(...t), hi = Math.max(...t);
    return hi - lo >= 1 ? [lo, hi] : [lo - 0.5, hi + 0.5];
  }
  return wx.temperatureUnit === 'F' ? [32, 86] : [0, 30];
}

// A temperature's colour between the two ends. The hue goes straight from the cold end's to the
// hot end's (no wrap: blue to orange passes teal, green and yellow, as a thermometer reads; a
// shortest-way blend would pass purple), a little less saturated in the middle so it does not
// glow green.
function tempColour(t, scale, C) {
  const a = argbOf(C.cold) || argbOf(COLD), b = argbOf(C.hot) || argbOf(HOT);
  const f = t === null || t === undefined ? 0.5 : Math.max(0, Math.min(1, (t - scale[0]) / Math.max(1e-6, scale[1] - scale[0])));
  const [h1, s1, l1] = toHsl(a), [h2, s2, l2] = toHsl(b);
  const s = (s1 + (s2 - s1) * f) * (1 - 0.3 * Math.sin(Math.PI * f));
  return fromHsl(a[0] + (b[0] - a[0]) * f, h1 + (h2 - h1) * f, s, l1 + (l2 - l1) * f);
}

// A condition's tint (Capsules "condition"): sun yellow, cloud grey, rain in the accent, snow
// white, thunder violet.
function condColour(code, C) {
  const c = Number(code);
  if (c === 0 || c === 1) return '#FFF6D365';
  if (c === 2) return '#FFE2CF85';
  if (c === 3 || c === 45 || c === 48) return '#FF9AA7B6';
  if ((c >= 71 && c <= 77) || c === 85 || c === 86) return '#FFE3EAF3';
  if (c >= 95) return '#FF9B5DE5';
  if (c >= 51) return C.accent;
  return '#FF9AA7B6';
}

// An hour as an axis label: "4 PM" / "16:00".
function timeLabel(h) {
  if (system.hour12) return `${h % 12 || 12} ${h < 12 ? 'AM' : 'PM'}`;
  return `${String(h).padStart(2, '0')}:00`;
}

// --- round 33 phase 33A: the Trend layout ----------------------------------------------------------

const TREND_H = 26;      // the curve's band (the rain bars stand in its lower part), DIPs at 100 %
const TREND_W = 120;     // settings.trendWidth's default
const TREND_LABEL = 9;   // the axis labels' size
const THERMO_W = 7, THERMO_H = 52;

// The block after the conditions: on a top / bottom dock (and standalone) the curve of the next
// trendHours hours; on a left / right dock a thermometer (or today's high over its low).
function trendBlock(canvas, dock, k0, A) {
  if (!wx.configured) return null;
  const k = dayK(k0);
  if (A.vertical) {
    const today = (wx.daily || [])[0];
    let lo = today ? today.min : null, hi = today ? today.max : null;
    if (lo === null || lo === undefined || hi === null || hi === undefined) {
      const ts = (wx.hourly || []).slice(0, 24).map((h) => h.temp).filter((t) => t !== null && t !== undefined);
      if (!ts.length) return null;
      lo = Math.min(...ts);
      hi = Math.max(...ts);
    }
    const st = { size: EXTRA_FONT * k };
    const text = settings.trendSide === 'text';
    const hiS = text ? '↑' + tempText(hi) : tempText(hi), loS = text ? '↓' + tempText(lo) : tempText(lo);
    const mh = canvas.measureText(hiS, st), ml = canvas.measureText(loS, st);
    const hh = Math.ceil(mh.height), lh = Math.ceil(ml.height);
    const barW = Math.max(3, Math.round(THERMO_W * k)), barH = Math.round(THERMO_H * k), gap = Math.round(3 * k);
    const w = Math.ceil(Math.max(mh.width, ml.width, text ? 0 : 11 * k));
    const h = text ? hh + lh : hh + gap + barH + gap + lh;
    return { kind: text ? 'range' : 'thermo', w, h, k, st, hiS, loS, hh, lh, barW, barH, gap, lo, hi,
      now: wx.current && wx.current.temp !== null && wx.current.temp !== undefined ? wx.current.temp : null };
  }
  const n = Math.round(clampNum(settings.trendHours, 6, 24, 12));
  const hours = (wx.hourly || []).slice(0, n);
  if (hours.filter((h) => h.temp !== null && h.temp !== undefined).length < 2) return null;
  const labels = settings.trendLabels || 'peak';
  const lst = { size: TREND_LABEL * k };
  const lh = labels === 'none' ? 0 : Math.ceil(canvas.measureText('Now', lst).height);
  const ch = Math.round(TREND_H * k);
  const w = Math.round(clampNum(settings.trendWidth, 40, 300, TREND_W) * k);
  return { kind: 'trend', hours, w, h: ch + (lh ? Math.round(2 * k) + lh : 0), ch, lh, lst, labels, k };
}

function drawTrend(canvas, dock, f, x, y, dim, C) {
  const k = f.k, hours = f.hours, n = hours.length;
  const pad = 3 * k;
  const step = (f.w - 2 * pad) / Math.max(1, n - 1);
  const xs = (i) => x + pad + i * step;
  const has = (t) => t !== null && t !== undefined;
  const temps = hours.map((h) => h.temp).filter(has);
  const lo = Math.min(...temps), hi = Math.max(...temps);
  const rain = settings.trendRain !== false;
  // The curve keeps to the band's upper three quarters when the bars are on, so a wet hour's bar
  // stands under its point instead of hiding it.
  const bandT = y + pad, bandB = y + f.ch * (rain ? 0.72 : 1) - pad;
  const cy = (t) => (hi > lo ? bandB - ((t - lo) / (hi - lo)) * (bandB - bandT) : (bandT + bandB) / 2);
  canvas.save();
  canvas.opacity(dim);
  if (rain) {
    const bw = Math.max(2, Math.min(6 * k, step * 0.55));
    hours.forEach((h, i) => {
      if (!has(h.precip) || h.precip < 1) return;
      const bh = Math.max(k, (Math.min(100, h.precip) / 100) * f.ch * 0.45);
      canvas.fillRoundRect(xs(i) - bw / 2, y + f.ch - bh, bw, bh, Math.min(k, bw / 2), alpha(C.accent, 0.55));
    });
  }
  const p = canvas.path();
  let first = -1;
  hours.forEach((h, i) => {
    if (!has(h.temp)) return;
    if (first < 0) {
      p.moveTo(xs(i), cy(h.temp));
      first = i;
    } else {
      p.lineTo(xs(i), cy(h.temp));
    }
  });
  let paint = C.text;
  if (settings.trendLine === 'accent') {
    paint = C.accent;
  } else if (settings.trendLine === 'temperature') {
    const sc = scaleOf(temps);
    paint = hi > lo
      ? canvas.linearGradient(0, cy(hi), 0, cy(lo), [[0, tempColour(hi, sc, C)], [0.5, tempColour((hi + lo) / 2, sc, C)], [1, tempColour(lo, sc, C)]])
      : tempColour(hi, sc, C);
  }
  canvas.stroke(p, paint, { width: 1.8 * k, cap: 'round', join: 'round' });
  canvas.fillCircle(xs(first), cy(hours[first].temp), 2.6 * k, C.accent);
  if (f.labels !== 'none') {
    const ly = y + f.ch + Math.round(2 * k);
    const st = { ...f.lst, baseline: 'top', color: C.dim };
    const left = 'Now', right = timeLabel(hours[n - 1].hour);
    const lw = canvas.measureText(left, st).width, rw = canvas.measureText(right, st).width;
    canvas.text(left, x, ly, st);
    canvas.text(right, x + f.w, ly, { ...st, align: 'right' });
    if (f.labels === 'peak') {
      // The extreme ahead: the highest point, or the lowest when the hours fall further than they
      // rise ("9° at 5 AM" in the evening).
      const t0 = hours[first].temp;
      let iMax = first, iMin = first;
      hours.forEach((h, i) => {
        if (!has(h.temp)) return;
        if (h.temp > hours[iMax].temp) iMax = i;
        if (h.temp < hours[iMin].temp) iMin = i;
      });
      const i = hours[iMax].temp - t0 >= t0 - hours[iMin].temp ? iMax : iMin;
      if (i !== first) {
        const s = `${tempText(hours[i].temp)} at ${timeLabel(hours[i].hour)}`;
        const sw = canvas.measureText(s, st).width, gap = 6 * k;
        const minX = x + lw + gap + sw / 2, maxX = x + f.w - rw - gap - sw / 2;
        if (minX <= maxX) canvas.text(s, Math.max(minX, Math.min(maxX, xs(i))), ly, { ...st, align: 'center' });
      }
    }
  }
  canvas.restore();
}

// The thermometer (or the high over the low as text) of a left / right dock.
function drawThermo(canvas, dock, f, x, y, dim, C) {
  const cx = x + f.w / 2;
  canvas.save();
  canvas.opacity(dim);
  if (f.kind === 'range') {
    canvas.text(f.hiS, cx, y, { ...f.st, align: 'center', color: C.text });
    canvas.text(f.loS, cx, y + f.hh, { ...f.st, align: 'center', color: C.dim });
    canvas.restore();
    return;
  }
  const k = f.k;
  canvas.text(f.hiS, cx, y, { ...f.st, align: 'center', color: C.dim });
  const by = y + f.hh + f.gap;
  const sc = scaleOf([f.lo, f.hi]);
  const grad = canvas.linearGradient(0, by + f.barH, 0, by,
    [[0, tempColour(f.lo, sc, C)], [0.5, tempColour((f.lo + f.hi) / 2, sc, C)], [1, tempColour(f.hi, sc, C)]]);
  canvas.fillRoundRect(cx - f.barW / 2, by, f.barW, f.barH, f.barW / 2, grad);
  if (f.now !== null) {
    const t = f.hi > f.lo ? Math.max(0, Math.min(1, (f.now - f.lo) / (f.hi - f.lo))) : 0.5;
    const dy = by + f.barH - t * f.barH;
    canvas.fillCircle(cx, dy, 4.5 * k, '#FFFFFFFF');
    canvas.strokeCircle(cx, dy, 4.5 * k, '#FF1A2130', 1.5 * k);
  }
  canvas.text(f.loS, cx, by + f.barH + f.gap, { ...f.st, align: 'center', color: C.dim });
  canvas.restore();
}

// --- round 33 phase 33A: the Capsules layout -------------------------------------------------------

const CAP_GLYPH = 16;  // DIPs at 100 % (with daySize)

// A chip per day: after the conditions on a top / bottom dock (the icon, then the day's name over
// its high and low), stacked on a left / right dock and upright (the name, the icon, the high).
function capsuleBlock(canvas, dock, k0, A) {
  if (!wx.configured || !wx.daily || !wx.daily.length) return null;
  const k = dayK(k0);
  const days = wx.daily.slice(0, Math.max(2, Math.min(7, settings.dockDays || 3)));
  const nameSt = { size: 9.5 * k }, hiSt = { size: 10.5 * k, weight: 'semibold' }, loSt = { size: 9 * k };
  const g = Math.round(CAP_GLYPH * k);
  const vertical = A.vertical;
  const showLow = settings.capsuleLow !== false && !vertical;
  const cells = days.map((d, i) => {
    const name = dayName(d, i, 'column');
    const hi = dayDeg(d.max), lo = showLow ? ' ' + dayDeg(d.min) : '';
    return { d, name, hi, lo, mn: canvas.measureText(name, nameSt), mh: canvas.measureText(hi, hiSt),
      ml: lo ? canvas.measureText(lo, loSt) : { width: 0, height: 0 } };
  });
  const gap = dayGap(k0);
  let w, h;
  if (vertical) {
    const inner = Math.ceil(Math.max(g, ...cells.map((c) => Math.max(c.mn.width, c.mh.width))));
    w = inner + Math.round(10 * k);
    for (const c of cells) {
      c.w = w;
      c.h = Math.ceil(10 * k + c.mn.height + g + 2 * k + c.mh.height);
    }
    h = cells.reduce((s, c) => s + c.h, 0) + (cells.length - 1) * gap;
  } else {
    const ch = Math.ceil(Math.max(g, ...cells.map((c) => c.mn.height + c.mh.height)) + 8 * k);
    for (const c of cells) {
      c.w = Math.ceil(6 * k + g + 4 * k + Math.max(c.mn.width, c.mh.width + c.ml.width) + 8 * k);
      c.h = ch;
    }
    w = cells.reduce((s, c) => s + c.w, 0) + (cells.length - 1) * gap;
    h = ch;
  }
  return { kind: 'capsules', cells, w, h, k, g, nameSt, hiSt, loSt, vertical, cellGap: gap };
}

function drawCapsules(canvas, dock, f, x, y, dim, C) {
  const k = f.k, g = f.g;
  const tint = settings.capsuleTint || 'temperature';
  const strength = clampNum(settings.capsuleStrength, 0, 100, 25) / 100;
  const radius = clampNum(settings.capsuleRadius, 0, 30, 30) * k;
  const sc = scaleOf(f.cells.map((c) => c.d.max));
  const icons = [];
  let cx = x, cy = y;
  canvas.save();
  canvas.opacity(dim);
  f.cells.forEach((c, i) => {
    const r = Math.min(radius, c.h / 2, c.w / 2);
    const base = tint === 'temperature' ? (c.d.max !== null && c.d.max !== undefined ? tempColour(c.d.max, sc, C) : null)
      : tint === 'condition' ? condColour(c.d.code, C)
      : tint === 'accent' ? C.accent : null;
    if (base && strength > 0) canvas.fillRoundRect(cx, cy, c.w, c.h, r, alpha(base, strength));
    if (settings.capsuleOutline !== false) {
      // The mock-up's 26 % fill had a 48 % outline: the outline follows the strength.
      canvas.strokeRoundRect(cx + 0.5, cy + 0.5, c.w - 1, c.h - 1, Math.max(0, r - 0.5),
        base ? alpha(base, Math.min(1, 0.2 + strength * 1.1)) : alpha(C.text, 0.25), 1);
    }
    if (f.vertical) {
      const mid = cx + c.w / 2;
      let ty = cy + 5 * k;
      canvas.text(c.name, mid, ty, { ...f.nameSt, align: 'center', color: C.dim });
      ty += c.mn.height;
      icons.push([c, Math.round(mid - g / 2), Math.round(ty), i]);
      canvas.text(c.hi, mid, ty + g + 2 * k, { ...f.hiSt, align: 'center', color: C.text });
      cy += c.h + f.cellGap;
    } else {
      icons.push([c, Math.round(cx + 6 * k), Math.round(cy + (c.h - g) / 2), i]);
      const tx = cx + 6 * k + g + 4 * k;
      const ty = cy + (c.h - (c.mn.height + c.mh.height)) / 2;
      canvas.text(c.name, tx, ty, { ...f.nameSt, color: C.text });
      canvas.text(c.hi, tx, ty + c.mn.height, { ...f.hiSt, color: C.text });
      // The low on the high's baseline (it is smaller).
      if (c.lo) canvas.text(c.lo, tx + c.mh.width, ty + c.mn.height + c.mh.baseline, { ...f.loSt, baseline: 'alphabetic', color: C.dim });
      cx += c.w + f.cellGap;
    }
  });
  canvas.restore();
  // The icons outside the dimmed group: they take `dim` themselves (a Meteocons layer would not
  // inherit the group's opacity, a picture would get it twice).
  for (const [c, ix, iy, i] of icons) {
    if (c.d.code !== null && c.d.code >= 0) drawIcon(canvas, dock, c.d.code, true, ix, iy, g, dim, C.text, 'cap' + i);
  }
}

// The block after (or under) the conditions, by its kind.
function drawBlock(canvas, dock, l, x, y, dim) {
  const f = l.f;
  if (!f.kind) {
    drawForecast(canvas, dock, l, x, y, dim);
    return;
  }
  const C = colours(dock);
  if (f.kind === 'trend') drawTrend(canvas, dock, f, x, y, dim, C);
  else if (f.kind === 'thermo' || f.kind === 'range') drawThermo(canvas, dock, f, x, y, dim, C);
  else if (f.kind === 'capsules') drawCapsules(canvas, dock, f, x, y, dim, C);
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
      text: l.t.main, extra: l.t.extra, w: l.w, h: l.h, days: l.f && l.f.cells ? l.f.cells.length : 0,
      conditions: l.A.conditions, cell: l.A.cell, placement: l.A.placement, stack: l.A.stack, pack: settings.iconPack || 'windock',
      layout: settings.layout || 'auto', block: l.f ? l.f.kind || 'days' : 'none', hours: l.f && l.f.hours ? l.f.hours.length : 0 });  // round 33
  }
  // The block is centred in the box; on a dock the box is the block (measure), so this is 0, 0
  // along the dock and the centring across it; standalone the window.
  const ox = Math.round((canvas.width - l.w) / 2), oy = Math.round((canvas.height - l.h) / 2);
  if (l.f && l.A.placement === 'below') {
    drawConditions(canvas, dock, l, ox + Math.round((l.w - l.cw) / 2), oy, dim);
    drawBlock(canvas, dock, l, ox + Math.round((l.w - l.f.w) / 2), oy + l.ch + Math.round(forecastGap(l.k) / 2), dim);
  } else if (l.f) {
    drawConditions(canvas, dock, l, ox, oy + Math.round((l.h - l.ch) / 2), dim);
    drawBlock(canvas, dock, l, ox + l.cw + forecastGap(l.k), oy + Math.round((l.h - l.f.h) / 2), dim);
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

// Round 33: reactions to a choice the user just made (never to a value merely loaded: only a
// change from the value seen before counts, so a config reload does nothing).
let lastLayout = settings.layout || 'auto';
export function onSettingsChanged() {
  const layoutNow = settings.layout || 'auto';
  if (layoutNow !== lastLayout) {
    lastLayout = layoutNow;
    // 33A: Capsules are day cells; with "Several days" on, the page shows "Days on the dock".
    if (layoutNow === 'capsules' && settings.dockForecast !== 'days') settings.set('dockForecast', 'days');
  }
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
  if (c.feelsLike !== null && c.feelsLike !== undefined) details.push(`Feels like ${lround(c.feelsLike) || 0}°${unit}`);  // round 28
  if (c.humidity !== null) details.push(`Humidity ${lround(c.humidity)}%`);
  if (c.wind !== null) details.push(`Wind ${lround(c.wind)} ${wx.windUnit}`);
  line(canvas, details.join(' · '), condX, MAIN_TOP + 28, 18, { size: 12, color: secondary });
  // Round 28: today's sunrise / sunset (the place's own clock) and the UV index, a second line.
  const sun = [];
  const today = (wx.daily || [])[0];
  if (today && today.sunrise !== null && today.sunrise !== undefined) sun.push('Sunrise ' + placeTime(today.sunrise));
  if (today && today.sunset !== null && today.sunset !== undefined) sun.push('Sunset ' + placeTime(today.sunset));
  if (c.uv !== null && c.uv !== undefined) sun.push('UV ' + lround(c.uv));
  if (sun.length) line(canvas, sun.join(' · '), condX, MAIN_TOP + 45, 16, { size: 12, color: secondary });
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
