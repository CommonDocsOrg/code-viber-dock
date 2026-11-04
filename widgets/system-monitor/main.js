// System monitor (round 6 phase 6D): one metric of the `perf` provider in one of eleven styles.
//
// The provider samples on a worker thread of the dock (one sampler for every widget, at the fastest
// rate any of them asks for: perf.setInterval) and keeps the last 600 samples of every metric, so
// this widget keeps no history of its own: graphs draw perf.series(key, n). It redraws only when a
// snapshot arrives (perf.onChange) or its settings change; no timers.
//
// Values are in the metric's own unit (perf.metrics(): bytes, bytes/s, %, °C, MHz ...); "auto"
// scale formats bytes as KB / MB / GB (1024), rates as KB/s ... GB/s and seconds as days / hours.
// The range of bars, rings, gauges and dials is the metric's (min / max from perf.metrics()), the
// settings' rangeMin / rangeMax when set, and for rates (and the graphs of counts) the highest value
// in the shown history.

const DASH = '—';
const STYLES = ['graph', 'area', 'sparkline', 'bar', 'vbar', 'ring', 'gauge', 'dial', 'percent', 'value', 'text'];

let meta = null;        // this metric's perf.metrics() entry (null: not available now)
let metaList = null;    // the list it came from (perf.metrics() returns the same frozen array until it changes)

const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
const finite = (v) => typeof v === 'number' && Number.isFinite(v);
const automatic = (c) => typeof c !== 'string' || c === '' || /^#00[0-9a-f]{6}$/i.test(c);

// '#AARRGGBB' / '#RRGGBB' with its alpha replaced (0 - 1).
function alpha(c, a) {
  const hex = String(c).replace('#', '');
  const rgb = hex.length === 8 ? hex.slice(2) : hex.length === 6 ? hex : 'FFFFFF';
  return '#' + Math.round(clamp(a, 0, 1) * 255).toString(16).padStart(2, '0') + rgb;
}

function applySettings() {
  perf.setInterval(settings.refresh || 1000);
}
applySettings();
perf.onChange(() => requestRedraw());

export function onSettingsChanged() {
  applySettings();
  meta = null;
  metaList = null;
  requestRedraw();
}

function metricMeta() {
  const list = perf.metrics();
  if (list !== metaList) {
    metaList = list;
    meta = (list || []).find((m) => m.key === settings.metric) || null;
  }
  return meta;
}

// --- values and their text ------------------------------------------------------------------------

function smoothed(series) {
  if (!settings.smooth) return series;
  const out = new Array(series.length);
  for (let i = 0; i < series.length; i++) {
    let sum = 0, n = 0;
    for (let k = Math.max(0, i - 2); k <= i; k++) {
      if (finite(series[k])) { sum += series[k]; n++; }
    }
    out[i] = n ? sum / n : NaN;
  }
  return out;
}

const BYTE_UNITS = ['B', 'KB', 'MB', 'GB', 'TB'];

// { num, unit } for value v of metric m.
function format(v, m) {
  const dec = clamp(Math.round(settings.decimals || 0), 0, 3);
  const unitOverride = String(settings.unit || '');
  if (!finite(v)) return { num: DASH, unit: unitOverride || (m ? m.unit : '') };
  if (unitOverride) return { num: v.toFixed(dec), unit: unitOverride };
  const kind = m ? m.kind : 'number';
  const unit = m ? m.unit : '';
  if (settings.scale !== 'raw' && (kind === 'bytes' || kind === 'bytesPerSecond')) {
    let i = 0, x = Math.abs(v);
    while (x >= 1024 && i < BYTE_UNITS.length - 1) { x /= 1024; i++; }
    const d = i > 0 && x < 10 ? Math.max(1, dec) : dec;
    return { num: (v < 0 ? -x : x).toFixed(d), unit: BYTE_UNITS[i] + (kind === 'bytesPerSecond' ? '/s' : '') };
  }
  if (settings.scale !== 'raw' && kind === 'seconds') {
    const s = Math.floor(v), d = Math.floor(s / 86400), h = Math.floor((s % 86400) / 3600), mi = Math.floor((s % 3600) / 60);
    if (d > 0) return { num: d + 'd ' + h + 'h', unit: '' };
    if (h > 0) return { num: h + 'h ' + mi + 'm', unit: '' };
    return { num: mi + 'm ' + (s % 60) + 's', unit: '' };
  }
  if (kind === 'bool') return { num: v ? 'Yes' : 'No', unit: '' };
  return { num: v.toFixed(dec), unit };
}

const joined = (f) => (f.unit ? (f.unit === '%' || f.unit.startsWith('°') ? f.num + f.unit : f.num + ' ' + f.unit) : f.num);

// The range [lo, hi] for value fractions; `series` for the auto range of rates and counts.
function range(m, series) {
  const rMin = Number(settings.rangeMin) || 0, rMax = Number(settings.rangeMax) || 0;
  if (rMax > rMin) return [rMin, rMax];
  let lo = m ? m.min : 0, hi = m ? m.max : 100;
  const auto = !m || m.kind === 'bytesPerSecond' || (m.kind === 'number' && !m.key.startsWith('hwinfo.'));
  if (auto && series) {
    let peak = 0;
    for (const v of series) if (finite(v) && v > peak) peak = v;
    const floor = m && m.kind === 'bytesPerSecond' ? 1024 : 1;
    hi = nice(Math.max(peak * 1.1, floor));
    lo = Math.min(0, lo);
  }
  if (!(hi > lo)) hi = lo + 1;
  return [lo, hi];
}

// The smallest 1, 2, 2.5, 5 x 10^k at least v.
function nice(v) {
  if (!(v > 0)) return 1;
  const p = Math.pow(10, Math.floor(Math.log10(v)));
  for (const k of [1, 2, 2.5, 5, 10]) if (k * p >= v) return k * p;
  return 10 * p;
}

function colours(d, v) {
  const main = automatic(settings.color) ? d.theme.accent : settings.color;
  const text = automatic(settings.textColor) ? d.theme.text : settings.textColor;
  let value = main;
  if (finite(v) && settings.criticalAt && v >= settings.criticalAt) value = settings.criticalColor;
  else if (finite(v) && settings.warnAt && v >= settings.warnAt) value = settings.warnColor;
  return { main: value, text, dim: alpha(text, 0.6), track: alpha(text, 0.16), grid: alpha(text, 0.1) };
}

// The largest font size (at most `max`) at which `text` fits `w` DIPs.
function fitSize(canvas, text, w, max, weight) {
  const probe = canvas.measureText(text, { size: 20, weight: weight || 'normal' }).width;
  return probe > 0 ? clamp(Math.floor((20 * w) / probe * 10) / 10, 6, max) : max;
}

// --- measuring ------------------------------------------------------------------------------------

export function measure(canvas, d) {
  const s = Math.round(d.iconSize);
  if (d.axis === 'vertical') return { main: s, cross: s };
  return { main: clamp(Math.round(settings.width || 96), 40, 400), cross: s };
}

// --- drawing --------------------------------------------------------------------------------------

export function draw(canvas, d) {
  const w = canvas.width, h = canvas.height;
  const pad = d.standalone ? Math.max(6, Math.min(w, h) * 0.06) : 3;
  if (settings.background === 'panel' || settings.background === 'theme') {
    const fill = settings.background === 'theme' ? d.theme.labelBackground : '#99101010';
    canvas.fillRoundRect(0.5, 0.5, w - 1, h - 1, Math.min(10, h * 0.2), fill);
    canvas.strokeRoundRect(0.5, 0.5, w - 1, h - 1, Math.min(10, h * 0.2), d.theme.border, 1);
  }
  const m = metricMeta();
  const snap = perf.get();
  const key = settings.metric;
  const raw = snap && snap.metrics ? snap.metrics[key] : undefined;
  const style = STYLES.includes(settings.style) ? settings.style : 'graph';
  const needsSeries = style === 'graph' || style === 'area' || style === 'sparkline' || settings.smooth ||
    (!(Number(settings.rangeMax) > Number(settings.rangeMin)) && m && (m.kind === 'bytesPerSecond' || m.kind === 'number'));
  let series = null;
  if (needsSeries) {
    const interval = snap && snap.interval ? snap.interval : 1000;
    const n = clamp(Math.ceil((settings.historySeconds || 60) * 1000 / interval) + 1, 2, 600);
    series = smoothed(perf.series(key, n));
  }
  const v = settings.smooth && series && series.length ? series[series.length - 1] : finite(raw) ? raw : NaN;
  const [lo, hi] = range(m, series);
  const c = colours(d, v);
  const label = String(settings.label || '') || (m ? m.name : key);
  const box = { x: pad, y: pad, w: Math.max(1, w - 2 * pad), h: Math.max(1, h - 2 * pad) };
  const ctx = { canvas, d, m, v, lo, hi, c, label, series, box, frac: finite(v) ? clamp((v - lo) / (hi - lo), 0, 1) : NaN };
  switch (style) {
    case 'graph': case 'area': drawGraph(ctx, style === 'area'); break;
    case 'sparkline': drawSparkline(ctx); break;
    case 'bar': drawBar(ctx); break;
    case 'vbar': drawVBar(ctx); break;
    case 'ring': drawArcStyle(ctx, 'ring'); break;
    case 'gauge': drawArcStyle(ctx, 'gauge'); break;
    case 'dial': drawArcStyle(ctx, 'dial'); break;
    case 'percent': drawBig(ctx, true); break;
    case 'value': drawBig(ctx, false); break;
    case 'text': drawText(ctx); break;
  }
}

// A header line: the label on the left, the value on the right (either may be off). Returns its height.
function header(ctx, x, y, w, size) {
  const { canvas, c } = ctx;
  const showL = settings.showLabel, showV = settings.showValue;
  if (!showL && !showV) return 0;
  const value = joined(format(ctx.v, ctx.m));
  let vw = 0;
  if (showV) {
    const st = { size, weight: 'semibold', align: 'right', baseline: 'top', color: finite(ctx.v) ? c.text : c.dim };
    vw = canvas.measureText(value, st).width;
    canvas.text(value, x + w, y, st);
  }
  if (showL) {
    const room = w - vw - (showV ? 4 : 0);
    if (room > size) canvas.text(fitText(canvas, ctx.label, room, size * 0.92), x, y, { size: size * 0.92, align: 'left', baseline: 'top', color: c.dim });
  }
  return size * 1.3;
}

// The series as a polyline in the rectangle (gaps where a sample is missing), filled when `fill`.
function plot(ctx, x, y, w, h, fill) {
  const { canvas, c, series } = ctx;
  const [lo, hi] = ctx.series ? [ctx.lo, ctx.hi] : [0, 1];
  canvas.line(x, y + h - 0.5, x + w, y + h - 0.5, c.grid, 1);
  if (!series || series.length < 2) return;
  const n = series.length;
  const total = Math.max(2, Math.ceil((settings.historySeconds || 60) * 1000 / ((perf.get() || {}).interval || 1000)) + 1);
  const step = w / (Math.min(total, 600) - 1);
  const lw = clamp(settings.lineWidth || 2, 1, 6);
  const yOf = (v) => y + h - clamp((v - lo) / (hi - lo), 0, 1) * (h - lw / 2) - lw / 4;
  let path = null, area = null, started = false, firstX = 0, lastX = 0;
  for (let i = 0; i < n; i++) {
    const v = series[i];
    const px = x + w - (n - 1 - i) * step;
    if (!finite(v)) {
      if (started && area) area.lineTo(lastX, y + h).lineTo(firstX, y + h).close();
      started = false;
      continue;
    }
    const py = yOf(v);
    if (!started) {
      path = (path || canvas.path()).moveTo(px, py);
      if (fill) area = (area || canvas.path()).moveTo(px, y + h).lineTo(px, py);
      firstX = px;
      started = true;
    } else {
      path.lineTo(px, py);
      if (area) area.lineTo(px, py);
    }
    lastX = px;
  }
  if (started && area) area.lineTo(lastX, y + h).lineTo(firstX, y + h).close();
  if (area) canvas.fill(area, canvas.linearGradient(0, y, 0, y + h, [[0, alpha(c.main, 0.45)], [1, alpha(c.main, 0.08)]]));
  if (path) canvas.stroke(path, c.main, { width: lw, join: 'round', cap: 'round' });
  if (settings.showMinMax && h >= 24) {
    let mn = Infinity, mx = -Infinity;
    for (const v of series) if (finite(v)) { mn = Math.min(mn, v); mx = Math.max(mx, v); }
    if (mx >= mn) {
      const size = clamp(h * 0.22, 7, 11);
      canvas.text(joined(format(mx, ctx.m)), x + 1, y + 1, { size, baseline: 'top', color: c.dim });
      canvas.text(joined(format(mn, ctx.m)), x + 1, y + h - 2, { size, baseline: 'bottom', color: c.dim });
    }
  }
}

function drawGraph(ctx, area) {
  const { box, d } = ctx;
  const size = d.standalone ? clamp(box.h * 0.12, 9, 18) : clamp(box.h * 0.26, 8, 12);
  const top = header(ctx, box.x, box.y, box.w, size);
  plot(ctx, box.x, box.y + top, box.w, Math.max(4, box.h - top), area || settings.fill);
}

function drawSparkline(ctx) {
  const { canvas, box, c, d } = ctx;
  const f = format(ctx.v, ctx.m);
  const textW = settings.showValue || settings.showLabel ? box.w * 0.45 : 0;
  const gw = box.w - textW - (textW ? 4 : 0);
  const gh = Math.min(box.h, d.standalone ? box.h : box.h * 0.8);
  plot(ctx, box.x, box.y + (box.h - gh) / 2, Math.max(4, gw), gh, settings.fill);
  if (!textW) return;
  const tx = box.x + gw + 4;
  let top = box.y;
  if (settings.showLabel) {
    const ls = clamp(box.h * 0.24, 7, d.standalone ? 16 : 11);
    canvas.text(fitText(canvas, ctx.label, textW, ls), tx, box.y, { size: ls, baseline: 'top', color: c.dim });
    top += ls * 1.3;
  }
  if (settings.showValue) {
    const text = joined(f);
    const size = fitSize(canvas, text, textW, Math.max(8, box.y + box.h - top), 'semibold');
    canvas.text(text, tx, top + (box.y + box.h - top) / 2, { size, weight: 'semibold', baseline: 'middle', color: finite(ctx.v) ? c.text : c.dim });
  }
}

function drawBar(ctx) {
  const { canvas, box, c, d } = ctx;
  const size = d.standalone ? clamp(box.h * 0.2, 9, 22) : clamp(box.h * 0.3, 8, 13);
  const top = header(ctx, box.x, box.y, box.w, size);
  const minmax = settings.showMinMax ? size * 1.1 : 0;
  const bh = clamp((box.h - top - minmax) * 0.7, 3, d.standalone ? 40 : 14);
  const by = box.y + top + (box.h - top - minmax - bh) / 2;
  const r = bh / 2;
  canvas.fillRoundRect(box.x, by, box.w, bh, r, c.track);
  if (finite(ctx.frac) && ctx.frac > 0) canvas.fillRoundRect(box.x, by, Math.max(bh, box.w * ctx.frac), bh, r, c.main);
  if (minmax) {
    const st = { size: size * 0.8, baseline: 'top', color: c.dim };
    canvas.text(joined(format(ctx.lo, ctx.m)), box.x, by + bh + 2, st);
    canvas.text(joined(format(ctx.hi, ctx.m)), box.x + box.w, by + bh + 2, Object.assign({}, st, { align: 'right' }));
  }
}

function drawVBar(ctx) {
  const { canvas, box, c, d } = ctx;
  const showText = settings.showValue || settings.showLabel;
  const narrow = box.w < box.h * 0.9;
  let bx, bw, bt = box.y, bb = box.y + box.h;
  if (narrow || !showText) {
    bw = clamp(box.w * 0.4, 4, 40);
    bx = box.x + (box.w - bw) / 2;
    if (showText) bb -= clamp(box.h * 0.22, 8, 16);
  } else {
    bw = clamp(box.h * 0.32, 4, d.standalone ? 48 : 14);
    bx = box.x;
  }
  const bh = Math.max(2, bb - bt);
  canvas.fillRoundRect(bx, bt, bw, bh, Math.min(bw / 2, 4), c.track);
  if (finite(ctx.frac) && ctx.frac > 0) {
    const fh = Math.max(Math.min(bw, 4), bh * ctx.frac);
    canvas.fillRoundRect(bx, bt + bh - fh, bw, fh, Math.min(bw / 2, 4), c.main);
  }
  if (!showText) return;
  const text = joined(format(ctx.v, ctx.m));
  const vcol = finite(ctx.v) ? c.text : c.dim;
  if (narrow) {
    const size = fitSize(canvas, settings.showValue ? text : ctx.label, box.w, box.y + box.h - bb, 'semibold');
    canvas.text(settings.showValue ? text : ctx.label, box.x + box.w / 2, box.y + box.h, { size, weight: 'semibold', align: 'center', baseline: 'bottom', color: vcol });
    return;
  }
  const tx = bx + bw + 6, tw = Math.max(4, box.x + box.w - tx);
  let top = box.y;
  if (settings.showLabel) {
    const ls = clamp(box.h * 0.26, 7, d.standalone ? 18 : 11);
    canvas.text(fitText(canvas, ctx.label, tw, ls), tx, box.y, { size: ls, baseline: 'top', color: c.dim });
    top += ls * 1.3;
  }
  if (settings.showValue) {
    const size = fitSize(canvas, text, tw, Math.max(8, (box.y + box.h - top) * 0.8), 'semibold');
    canvas.text(text, tx, top + (box.y + box.h - top) / 2, { size, weight: 'semibold', baseline: 'middle', color: vcol });
  }
}

// Ring, gauge and dial: an arc of `sweep` degrees centred on the top.
function drawArcStyle(ctx, kind) {
  const { canvas, box, c, d } = ctx;
  const own = kind === 'gauge' ? 180 : kind === 'dial' ? 240 : 270;
  const sweepDeg = settings.sweep ? clamp(settings.sweep, 90, 300) : own;
  const sweep = sweepDeg * Math.PI / 180;
  const start = -Math.PI / 2 - sweep / 2;
  const below = Math.max(0, -Math.cos(sweep / 2));  // how far the arc reaches below its centre (x r)
  const wide = sweep >= Math.PI ? 2 : 2 * Math.sin(sweep / 2);
  const text = joined(format(ctx.v, ctx.m));
  const vcol = finite(ctx.v) ? c.text : c.dim;
  // Room under the arc for the value line (the gauge: always; a ring / dial put it inside).
  // The value line under the arc: a gauge, and a dial too flat to hold it under its hub.
  const lineUnder = kind === 'gauge' || (kind === 'dial' && sweepDeg < 220);
  const underText = lineUnder && (settings.showValue || settings.showLabel);
  const lineH = underText ? clamp(box.h * (d.standalone ? 0.16 : 0.24), 8, 26) : 0;
  const hub = kind === 'ring' ? 0 : 0.1;
  let r = Math.min(box.w / wide, (box.h - lineH) / (1 + Math.max(below, hub)));
  r = Math.max(4, r);
  const cx = box.x + box.w / 2;
  const cy = box.y + r + ((box.h - lineH) - r * (1 + Math.max(below, hub))) / 2;
  const thick = kind === 'ring' ? clamp(r * 0.2, 2.5, 18) : clamp(r * 0.12, 2, 12);
  const ar = r - thick / 2;
  canvas.stroke(canvas.path().arc(cx, cy, ar, start, start + sweep), c.track, { width: thick, cap: kind === 'ring' ? 'round' : 'butt' });
  if (finite(ctx.frac) && ctx.frac > 0.001) {
    canvas.stroke(canvas.path().arc(cx, cy, ar, start, start + sweep * ctx.frac), c.main, { width: thick, cap: kind === 'ring' ? 'round' : 'butt' });
  }
  // Warning / critical zones on the scale of a gauge / dial (thin, inside the track).
  if (kind !== 'ring') {
    const zone = (from, color) => {
      if (!from) return;
      const f = clamp((from - ctx.lo) / (ctx.hi - ctx.lo), 0, 1);
      if (f >= 1) return;
      canvas.stroke(canvas.path().arc(cx, cy, r - thick - 1.5, start + sweep * f, start + sweep), alpha(color, 0.7), { width: Math.max(1, thick * 0.35) });
    };
    zone(settings.warnAt, settings.warnColor);
    zone(settings.criticalAt, settings.criticalColor);
    // Ticks: 5 major, 4 minor between each.
    const majors = 4, minors = 5;
    for (let i = 0; i <= majors * minors; i++) {
      const a = start + sweep * i / (majors * minors);
      const major = i % minors === 0;
      const r0 = r - thick - (major ? r * 0.16 : r * 0.08), r1 = r - thick - 1;
      if (!major && r < 24) continue;
      canvas.line(cx + Math.cos(a) * r0, cy + Math.sin(a) * r0, cx + Math.cos(a) * r1, cy + Math.sin(a) * r1,
        major ? c.dim : alpha(c.text, 0.35), Math.max(1, r * (major ? 0.025 : 0.015)));
    }
    if (kind === 'dial' && r >= 30) {
      const ns = clamp(r * 0.13, 6, 14);
      for (let i = 0; i <= majors; i++) {
        const a = start + sweep * i / majors;
        const rr = r - thick - r * 0.3;
        const val = ctx.lo + (ctx.hi - ctx.lo) * i / majors;
        canvas.text(short(val, ctx.m), cx + Math.cos(a) * rr, cy + Math.sin(a) * rr, { size: ns, align: 'center', baseline: 'middle', color: c.dim });
      }
    }
    // The needle.
    const f = finite(ctx.frac) ? ctx.frac : 0;
    const a = start + sweep * f;
    const len = r - thick - (kind === 'dial' ? r * 0.06 : r * 0.04);
    const base = Math.max(1.5, r * 0.05);
    const nx = Math.cos(a), ny = Math.sin(a), px = -ny, py = nx;
    canvas.fill(canvas.path().moveTo(cx + nx * len, cy + ny * len).lineTo(cx + px * base - nx * base * 1.6, cy + py * base - ny * base * 1.6)
      .lineTo(cx - px * base - nx * base * 1.6, cy - py * base - ny * base * 1.6).close(), finite(ctx.v) ? c.main : c.dim);
    canvas.fillCircle(cx, cy, Math.max(2, r * 0.08), c.text);
  }
  // The range's ends: a ring in the gap between its ends; a gauge at the corners of its value line
  // (when the line keeps half the width) or, without that line, under the arc's ends.
  const ms = clamp(r * 0.16, 6, 12);
  const ea = start + sweep;
  const loText = short(ctx.lo, ctx.m), hiText = short(ctx.hi, ctx.m);
  let lineW = box.w;
  if (settings.showMinMax && kind === 'ring' && r >= 32 && sweep < 1.95 * Math.PI) {
    const y0 = cy + Math.sin(start) * ar, y1 = cy + Math.sin(ea) * ar;
    canvas.text(loText, cx + Math.cos(start) * ar + thick / 2 + 2, y0, { size: ms, baseline: 'middle', color: c.dim });
    canvas.text(hiText, cx + Math.cos(ea) * ar - thick / 2 - 2, y1, { size: ms, align: 'right', baseline: 'middle', color: c.dim });
  } else if (settings.showMinMax && lineUnder && kind === 'gauge') {
    if (underText) {
      const ls = clamp(lineH * 0.6, 6, 14);
      const edge = Math.max(canvas.measureText(loText, { size: ls }).width, canvas.measureText(hiText, { size: ls }).width) + 4;
      if (box.w - 2 * edge >= box.w * 0.5) {
        canvas.text(loText, box.x, box.y + box.h, { size: ls, baseline: 'bottom', color: c.dim });
        canvas.text(hiText, box.x + box.w, box.y + box.h, { size: ls, align: 'right', baseline: 'bottom', color: c.dim });
        lineW = box.w - 2 * edge;
      }
    } else if (r >= 16) {
      canvas.text(loText, cx + Math.cos(start) * ar, cy + Math.sin(start) * ar + thick / 2 + 1, { size: ms, align: 'center', baseline: 'top', color: c.dim });
      canvas.text(hiText, cx + Math.cos(ea) * ar, cy + Math.sin(ea) * ar + thick / 2 + 1, { size: ms, align: 'center', baseline: 'top', color: c.dim });
    }
  }
  if (lineUnder) {
    if (!underText) return;
    const line = settings.showLabel && settings.showValue ? ctx.label + '  ' + text : settings.showValue ? text : ctx.label;
    const size = fitSize(canvas, line, lineW, lineH * 0.85, 'semibold');
    canvas.text(line, cx, box.y + box.h, { size, weight: 'semibold', align: 'center', baseline: 'bottom', color: vcol });
    return;
  }
  // Ring: the value in the middle, the label under it. Dial: both in the gap under the hub.
  const inner = kind === 'ring' ? (ar - thick / 2) * 2 * 0.86 : r * 1.1;
  const vy = kind === 'ring' ? cy : cy + r * 0.42;
  if (settings.showValue) {
    const size = fitSize(canvas, text, inner, kind === 'ring' ? r * 0.5 : r * 0.28, 'semibold');
    const ty = settings.showLabel && kind === 'ring' && r >= 22 ? vy - size * 0.2 : vy;
    canvas.text(text, cx, ty, { size, weight: 'semibold', align: 'center', baseline: 'middle', color: vcol });
    if (settings.showLabel && r >= 22) {
      const ls = clamp(size * 0.42, 6, 14);
      canvas.text(fitText(canvas, ctx.label, inner, ls), cx, ty + size * 0.62, { size: ls, align: 'center', baseline: 'top', color: c.dim });
    }
  } else if (settings.showLabel) {
    const ls = fitSize(canvas, ctx.label, inner, r * 0.24);
    canvas.text(ctx.label, cx, vy, { size: ls, align: 'center', baseline: 'middle', color: c.dim });
  }
}

// `text` cut with an ellipsis to fit `w` at `size`.
function fitText(canvas, text, w, size) {
  if (canvas.measureText(text, { size }).width <= w) return text;
  let lo = 0, hi = text.length;
  while (lo < hi) {
    const mid = (lo + hi + 1) >> 1;
    if (canvas.measureText(text.slice(0, mid) + '…', { size }).width <= w) lo = mid; else hi = mid - 1;
  }
  return text.slice(0, lo) + '…';
}

// A short scale number: 1.5K, 20M (bytes: 1024-based like format()).
function short(v, m) {
  if (!finite(v)) return '';
  const bytes = m && (m.kind === 'bytes' || m.kind === 'bytesPerSecond') && settings.scale !== 'raw' && !settings.unit;
  const base = bytes ? 1024 : 1000;
  const units = bytes ? ['', 'K', 'M', 'G', 'T'] : ['', 'k', 'M', 'G', 'T'];
  let i = 0, x = Math.abs(v);
  while (x >= base && i < units.length - 1) { x /= base; i++; }
  const t = x >= 100 || Number.isInteger(x) ? Math.round(x).toString() : x.toFixed(1).replace(/\.0$/, '');
  return (v < 0 ? '-' : '') + t + units[i];
}

function drawBig(ctx, percent) {
  const { canvas, box, c, d } = ctx;
  let top = box.y;
  if (settings.showLabel) {
    const ls = clamp(box.h * 0.24, 7, d.standalone ? 20 : 11);
    canvas.text(fitText(canvas, ctx.label, box.w, ls), box.x + box.w / 2, box.y, { size: ls, align: 'center', baseline: 'top', color: c.dim });
    top += ls * 1.3;
  }
  if (!settings.showValue) return;
  let num, unit;
  if (percent) {
    const p = ctx.m && ctx.m.kind === 'percent' && !(Number(settings.rangeMax) > Number(settings.rangeMin)) ? ctx.v : ctx.frac * 100;
    num = finite(p) ? p.toFixed(clamp(Math.round(settings.decimals || 0), 0, 3)) : DASH;
    unit = '%';
  } else {
    const f = format(ctx.v, ctx.m);
    num = f.num;
    unit = f.unit;
  }
  const avail = box.y + box.h - top;
  let size = Math.max(8, avail * 0.92);
  const unitW = (s) => (unit ? canvas.measureText(unit, { size: s * 0.45, weight: 'semibold' }).width + s * 0.08 : 0);
  const numW = canvas.measureText(num, { size, weight: 'semibold' }).width;
  const total = numW + unitW(size);
  if (total > box.w) size = Math.max(6, size * box.w / total);
  const nw = canvas.measureText(num, { size, weight: 'semibold' }).width, uw = unitW(size);
  const x0 = box.x + (box.w - nw - uw) / 2;
  const base = top + avail / 2 + size * 0.36;
  canvas.text(num, x0, base, { size, weight: 'semibold', baseline: 'alphabetic', color: finite(ctx.v) ? ctx.c.main : c.dim });
  if (unit) canvas.text(unit, x0 + nw + size * 0.08, base, { size: size * 0.45, weight: 'semibold', baseline: 'alphabetic', color: c.dim });
}

function drawText(ctx) {
  const { canvas, box, c } = ctx;
  const value = joined(format(ctx.v, ctx.m));
  const line = settings.showLabel && settings.showValue ? ctx.label + ': ' + value : settings.showValue ? value : ctx.label;
  const size = fitSize(canvas, line, box.w, Math.min(box.h * 0.6, ctx.d.standalone ? 40 : 14), 'normal');
  canvas.text(line, box.x + box.w / 2, box.y + box.h / 2, { size, align: 'center', baseline: 'middle', color: finite(ctx.v) ? c.text : c.dim });
}
