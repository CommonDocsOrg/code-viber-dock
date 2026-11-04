// System overview (round 6 phase 6D): CPU, memory, GPU, disk and network from the `perf` provider.
//
// Layout "auto": on a top / bottom dock one row of cells (label, value, a mini bar or sparkline per
// metric); on a left / right dock and on the desktop a list (label and value, a sparkline or bar
// under them). The provider keeps the history (perf.series); this widget redraws when a snapshot
// arrives (perf.onChange) and has no timers. A metric that is not available (no GPU counters) shows
// a dash.

const DASH = '—';
const ROWS = [
  { set: 'showCpu', label: 'CPU', key: 'cpu.total' },
  { set: 'showMemory', label: 'Memory', short: 'RAM', key: 'memory.percent' },
  { set: 'showGpu', label: 'GPU', key: 'gpu.total' },
  { set: 'showDisk', label: 'Disk', key: 'disk.total' },
  { set: 'showNetwork', label: 'Network', short: 'Net', key: 'network.down', key2: 'network.up', rate: true },
];

const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
const finite = (v) => typeof v === 'number' && Number.isFinite(v);
const automatic = (c) => typeof c !== 'string' || c === '' || /^#00[0-9a-f]{6}$/i.test(c);
function alpha(c, a) {
  const hex = String(c).replace('#', '');
  const rgb = hex.length === 8 ? hex.slice(2) : hex.length === 6 ? hex : 'FFFFFF';
  return '#' + Math.round(clamp(a, 0, 1) * 255).toString(16).padStart(2, '0') + rgb;
}

perf.setInterval(settings.refresh || 1000);
perf.onChange(() => requestRedraw());

export function onSettingsChanged() {
  perf.setInterval(settings.refresh || 1000);
  requestRedraw();
}

const rows = () => ROWS.filter((r) => settings[r.set] !== false);

function layout(d) {
  if (settings.rows === 'row' || settings.rows === 'list') return settings.rows;
  return d.standalone || d.axis === 'vertical' ? 'list' : 'row';
}

export function measure(canvas, d) {
  const s = Math.round(d.iconSize);
  const n = Math.max(1, rows().length);
  const cell = clamp(settings.cellWidth || 44, 28, 120);
  if (d.axis === 'vertical') return { main: layout(d) === 'list' ? Math.max(s, n * 22) : n * Math.round(s * 0.8), cross: s };
  return { main: layout(d) === 'list' ? Math.max(120, cell * 3) : n * cell, cross: s };
}

// Bytes/s as "1.2M" (short) or "1.2 MB/s".
function rate(v, short) {
  if (!finite(v)) return DASH;
  const units = short ? ['', 'K', 'M', 'G'] : ['B/s', 'KB/s', 'MB/s', 'GB/s'];
  let i = 0, x = v;
  while (x >= 1024 && i < units.length - 1) { x /= 1024; i++; }
  const t = x < 10 && i > 0 ? x.toFixed(1) : Math.round(x).toString();
  return short ? t + units[i] : t + ' ' + units[i];
}
function gb(v) { return finite(v) ? (v / 1073741824).toFixed(1) : DASH; }

function nice(v) {
  if (!(v > 0)) return 1;
  const p = Math.pow(10, Math.floor(Math.log10(v)));
  for (const k of [1, 2, 2.5, 5, 10]) if (k * p >= v) return k * p;
  return 10 * p;
}

// One row's numbers: { v (0 - 1 for the bar), text, series, color }.
function rowData(r, snap, c, list, n, compact) {
  const m = snap && snap.metrics ? snap.metrics : {};
  const v = m[r.key];
  const want = settings.style === 'sparklines' || (settings.style !== 'bars' && list);
  const series = want || r.rate ? perf.series(r.key, n) : null;
  if (r.rate) {
    let peak = 0;
    for (const x of series) if (finite(x) && x > peak) peak = x;
    const top = nice(Math.max(peak * 1.1, 64 * 1024));
    const up = m[r.key2];
    const text = !finite(v) ? DASH : list && !compact ? '↓ ' + rate(v) + '  ↑ ' + rate(up) : '↓' + rate(v, true);
    return { frac: finite(v) ? clamp(v / top, 0, 1) : NaN, text, series: want ? series : null, lo: 0, hi: top, color: c.main };
  }
  let text = finite(v) ? Math.round(v) + '%' : DASH;
  if (list && !compact && r.key === 'memory.percent' && finite(m['memory.used'])) text = gb(m['memory.used']) + ' / ' + gb(m['memory.total']) + ' GB';
  let color = c.main;
  if (finite(v) && settings.criticalAt && v >= settings.criticalAt) color = settings.criticalColor;
  else if (finite(v) && settings.warnAt && v >= settings.warnAt) color = settings.warnColor;
  return { frac: finite(v) ? clamp(v / 100, 0, 1) : NaN, text, series: want ? series : null, lo: 0, hi: 100, color };
}

function spark(canvas, x, y, w, h, data, c) {
  canvas.line(x, y + h - 0.5, x + w, y + h - 0.5, c.grid, 1);
  const s = data.series;
  if (!s || s.length < 2) return;
  const step = w / (s.length - 1);
  let path = null, area = null, on = false, x0 = 0, x1 = 0;
  for (let i = 0; i < s.length; i++) {
    const v = s[i];
    const px = x + i * step;
    if (!finite(v)) {
      if (on && area) area.lineTo(x1, y + h).lineTo(x0, y + h).close();
      on = false;
      continue;
    }
    const py = y + h - clamp((v - data.lo) / (data.hi - data.lo), 0, 1) * (h - 1) - 0.5;
    if (!on) {
      path = (path || canvas.path()).moveTo(px, py);
      area = (area || canvas.path()).moveTo(px, y + h).lineTo(px, py);
      x0 = px;
      on = true;
    } else {
      path.lineTo(px, py);
      area.lineTo(px, py);
    }
    x1 = px;
  }
  if (on && area) area.lineTo(x1, y + h).lineTo(x0, y + h).close();
  if (area) canvas.fill(area, alpha(data.color, 0.25));
  if (path) canvas.stroke(path, data.color, { width: 1.5, join: 'round' });
}

function bar(canvas, x, y, w, h, data, c) {
  canvas.fillRoundRect(x, y, w, h, h / 2, c.track);
  if (finite(data.frac) && data.frac > 0) canvas.fillRoundRect(x, y, Math.max(h, w * data.frac), h, h / 2, data.color);
}

export function draw(canvas, d) {
  const w = canvas.width, h = canvas.height;
  if (settings.background === 'panel' || settings.background === 'theme') {
    const fill = settings.background === 'theme' ? d.theme.labelBackground : '#99101010';
    canvas.fillRoundRect(0.5, 0.5, w - 1, h - 1, Math.min(10, h * 0.2), fill);
    canvas.strokeRoundRect(0.5, 0.5, w - 1, h - 1, Math.min(10, h * 0.2), d.theme.border, 1);
  }
  const text = automatic(settings.textColor) ? d.theme.text : settings.textColor;
  const c = {
    main: automatic(settings.color) ? d.theme.accent : settings.color,
    text, dim: alpha(text, 0.6), track: alpha(text, 0.16), grid: alpha(text, 0.1),
  };
  const list = layout(d) === 'list';
  const rs = rows();
  if (!rs.length) return;
  const snap = perf.get();
  const interval = snap && snap.interval ? snap.interval : 1000;
  const n = clamp(Math.ceil((settings.historySeconds || 60) * 1000 / interval) + 1, 2, 600);
  const pad = d.standalone ? Math.max(6, Math.min(w, h) * 0.05) : 2;
  const bx = pad, by = pad, bw = Math.max(1, w - 2 * pad), bh = Math.max(1, h - 2 * pad);
  if (!list) {
    // One row of cells (a vertical dock: the cells stacked).
    const vertical = d.axis === 'vertical' && !d.standalone;
    const cw = vertical ? bw : bw / rs.length, ch = vertical ? bh / rs.length : bh;
    rs.forEach((r, i) => {
      const x = bx + (vertical ? 0 : i * cw), y = by + (vertical ? i * ch : 0);
      const data = rowData(r, snap, c, false, n);
      const ls = clamp(ch * 0.22, 6, 12);
      const label = cw < 40 && r.short ? r.short : r.label;
      canvas.text(label, x + cw / 2, y, { size: ls, align: 'center', baseline: 'top', color: c.dim, maxWidth: 0 });
      const graphH = data.series ? ch * 0.3 : clamp(ch * 0.13, 3, 8);
      const gy = y + ch - graphH - 1;
      if (settings.showValues) {
        const avail = gy - (y + ls * 1.15);
        let vs = clamp(avail * 0.9, 6, 18);
        const tw = canvas.measureText(data.text, { size: vs, weight: 'semibold' }).width;
        if (tw > cw - 4) vs = Math.max(6, vs * (cw - 4) / tw);
        canvas.text(data.text, x + cw / 2, y + ls * 1.15 + avail / 2, { size: vs, weight: 'semibold', align: 'center', baseline: 'middle', color: finite(data.frac) ? c.text : c.dim });
      }
      const gx = x + 3, gw = Math.max(2, cw - 6);
      if (data.series) spark(canvas, gx, gy, gw, graphH, data, c); else bar(canvas, gx, gy, gw, graphH, data, c);
    });
    return;
  }
  // A list: per row the label and the value, the sparkline or bar under them (or beside them when short).
  const rh = bh / rs.length;
  const compact = bw < 110;  // a narrow tile (a left / right dock): short labels and values, no graph
  rs.forEach((r, i) => {
    const y = by + i * rh;
    const data = rowData(r, snap, c, true, n, compact);
    const two = rh >= 26 && !compact;
    const ls = clamp(two ? rh * 0.3 : Math.min(rh * 0.62, compact ? bw / 5 : 99), 6, 16);
    const label = (compact || (!two && bw < 160)) && r.short ? r.short : r.label;
    if (two) {
      canvas.text(label, bx, y, { size: ls, baseline: 'top', color: c.dim });
      if (settings.showValues) canvas.text(data.text, bx + bw, y, { size: ls, weight: 'semibold', align: 'right', baseline: 'top', color: finite(data.frac) ? c.text : c.dim });
      const gy = y + ls * 1.3, gh = Math.max(3, rh - ls * 1.3 - Math.min(4, rh * 0.1));
      if (data.series) spark(canvas, bx, gy, bw, gh, data, c); else bar(canvas, bx, gy + (gh - Math.min(gh, 8)) / 2, bw, Math.min(gh, 8), data, c);
    } else {
      const lw = canvas.measureText(compact ? 'Disk' : 'Memory', { size: ls }).width + 4;
      canvas.text(label, bx, y + rh / 2, { size: ls, baseline: 'middle', color: c.dim });
      let vw = 0;
      if (settings.showValues) {
        vw = Math.min(bw * 0.45, canvas.measureText(data.text, { size: ls, weight: 'semibold' }).width + 4);
        canvas.text(data.text, bx + bw, y + rh / 2, { size: ls, weight: 'semibold', align: 'right', baseline: 'middle', color: finite(data.frac) ? c.text : c.dim });
      }
      const gx = bx + lw, gw = bw - lw - vw, gh = Math.max(2, rh * 0.5);
      if (gw < 12) return;
      if (data.series) spark(canvas, gx, y + (rh - gh) / 2, gw, gh, data, c); else bar(canvas, gx, y + (rh - Math.min(gh, 6)) / 2, gw, Math.min(gh, 6), data, c);
    }
  });
}
