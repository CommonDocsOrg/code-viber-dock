// Moon phase (round 28 phase 28B): today's moon drawn as it looks (the lit part and the terminator),
// optionally its phase name or illumination beside it; a click opens a popup with the phase, the
// illumination, the moon's age and the next new, first quarter, full and last quarter moons.
//
// Computed here, no data source: the times of the principal phases are J. Meeus' (Astronomical
// Algorithms, 2nd ed., chapter 49) with the larger periodic terms (within a few minutes of the
// published times); the illumination between them is the mean-phase approximation (1 - cos)/2 of
// the elapsed fraction of the lunation (within about a percent). Local dates use today's UTC offset
// (an event across a daylight-saving change can show an hour off).
//
// Cost: the manifest's refreshMs (3600000) redraws hourly, aligned, only while the widget is seen.

const SYN = 29.530588861;
const RAD = Math.PI / 180;

let hits = [];

const custom = (c) => (c && !/^#00[0-9a-f]{6}$/i.test(c) ? c : null);
const jdOf = (ms) => ms / 86400000 + 2440587.5;
const msOf = (jd) => (jd - 2440587.5) * 86400000;

// Meeus 49.1 with the corrections for a phase: q 0 new, 0.25 first quarter, 0.5 full, 0.75 last.
function phaseJde(k, q) {
  k += q;
  const T = k / 1236.85, T2 = T * T, T3 = T2 * T, T4 = T3 * T;
  let jde = 2451550.09766 + SYN * k + 0.00015437 * T2 - 0.00000015 * T3 + 0.00000000073 * T4;
  const E = 1 - 0.002516 * T - 0.0000074 * T2;
  const M = (2.5534 + 29.1053567 * k - 0.0000014 * T2 - 0.00000011 * T3) * RAD;
  const Mp = (201.5643 + 385.81693528 * k + 0.0107582 * T2 + 0.00001238 * T3 - 0.000000058 * T4) * RAD;
  const F = (160.7108 + 390.67050284 * k - 0.0016118 * T2 - 0.00000227 * T3 + 0.000000011 * T4) * RAD;
  const Om = (124.7746 - 1.56375588 * k + 0.0020672 * T2 + 0.00000215 * T3) * RAD;
  const s = Math.sin;
  if (q === 0 || q === 0.5) {
    const n = q === 0;
    jde += (n ? -0.4072 : -0.40614) * s(Mp) + (n ? 0.17241 : 0.17302) * E * s(M) + (n ? 0.01608 : 0.01614) * s(2 * Mp) +
      (n ? 0.01039 : 0.01043) * s(2 * F) + (n ? 0.00739 : 0.00734) * E * s(Mp - M) - (n ? 0.00514 : 0.00515) * E * s(Mp + M) +
      (n ? 0.00208 : 0.00209) * E * E * s(2 * M) - 0.00111 * s(Mp - 2 * F) - 0.00057 * s(Mp + 2 * F) + 0.00056 * E * s(2 * Mp + M) -
      0.00042 * s(3 * Mp) + 0.00042 * E * s(M + 2 * F) + 0.00038 * E * s(M - 2 * F) - 0.00024 * E * s(2 * Mp - M) - 0.00017 * s(Om);
  } else {
    jde += -0.62801 * s(Mp) + 0.17172 * E * s(M) - 0.01183 * E * s(Mp + M) + 0.00862 * s(2 * Mp) + 0.00804 * s(2 * F) +
      0.00454 * E * s(Mp - M) + 0.00204 * E * E * s(2 * M) - 0.0018 * s(Mp - 2 * F) - 0.0007 * s(Mp + 2 * F) - 0.0004 * s(3 * Mp) -
      0.00034 * E * s(2 * Mp - M) + 0.00032 * E * s(M + 2 * F) + 0.00032 * E * s(M - 2 * F) - 0.00028 * E * E * s(Mp + 2 * M);
    const W = 0.00306 - 0.00038 * E * Math.cos(M) + 0.00026 * Math.cos(Mp) - 0.00002 * Math.cos(Mp - M) +
      0.00002 * Math.cos(Mp + M) + 0.00002 * Math.cos(2 * F);
    jde += q === 0.25 ? W : -W;
  }
  return jde;
}

// The lunation around `ms`: the new moon before it and the one after, and the phases between.
function lunation(ms) {
  const jd = jdOf(ms);
  let k = Math.floor((jd - 2451550.09766) / SYN);
  while (phaseJde(k, 0) > jd) k--;
  while (phaseJde(k + 1, 0) <= jd) k++;
  const at = (kk, q) => msOf(phaseJde(kk, q));
  return { k, prevNew: at(k, 0), first: at(k, 0.25), full: at(k, 0.5), last: at(k, 0.75), nextNew: at(k + 1, 0), at };
}

function moonNow(ms) {
  const l = lunation(ms);
  const f = (ms - l.prevNew) / (l.nextNew - l.prevNew);  // 0 new, 0.5 full
  const illum = (1 - Math.cos(2 * Math.PI * f)) / 2;
  const day = 86400000;
  // Named for the principal phases within a day of them, else by the quarter it is in.
  const near = (t) => Math.abs(ms - t) < day;
  let name;
  if (near(l.prevNew) || near(l.nextNew)) name = 'New moon';
  else if (near(l.first)) name = 'First quarter';
  else if (near(l.full)) name = 'Full moon';
  else if (near(l.last)) name = 'Last quarter';
  else if (ms < l.first) name = 'Waxing crescent';
  else if (ms < l.full) name = 'Waxing gibbous';
  else if (ms < l.last) name = 'Waning gibbous';
  else name = 'Waning crescent';
  return { f, illum, name, age: (ms - l.prevNew) / day, l };
}

// The next `n` principal phases after `ms`, in order.
function upcoming(ms, n) {
  const l = lunation(ms);
  const names = ['New moon', 'First quarter', 'Full moon', 'Last quarter'];
  const out = [];
  for (let kk = l.k; out.length < n && kk < l.k + 3; kk++) {
    [0, 0.25, 0.5, 0.75].forEach((q, i) => {
      const t = l.at(kk, q);
      if (t > ms) out.push({ name: names[i], t });
    });
  }
  return out.sort((a, b) => a.t - b.t).slice(0, n);
}

// --- drawing -------------------------------------------------------------------------------------------

// The moon in a circle of radius r at (cx, cy): the dark disc, then the lit part bounded by a
// semicircle and the terminator (a half ellipse, cubic beziers). `f` the lunation fraction.
function moon(canvas, cx, cy, r, f, lit, dark) {
  canvas.fillCircle(cx, cy, r, dark);
  const south = settings.hemisphere === 'south';
  const waxing = f < 0.5;
  // Lit on the right while waxing (north), on the left while waning; mirrored in the south.
  const right = waxing !== south;
  const c = Math.cos(2 * Math.PI * f);
  const rx = r * (waxing ? c : -c) * (south ? -1 : 1);  // the terminator's bulge (+ toward the right)
  const K = 0.5523;
  const p = canvas.path().moveTo(cx, cy - r).arc(cx, cy, r, -Math.PI / 2, Math.PI / 2, !right);
  p.bezierTo(cx + rx * K, cy + r, cx + rx, cy + r * K, cx + rx, cy)
    .bezierTo(cx + rx, cy - r * K, cx + rx * K, cy - r, cx, cy - r).close();
  canvas.fill(p, lit);  // a full moon: the terminator is the far semicircle (rx = ±r)
  canvas.strokeCircle(cx, cy, r, '#30808080', Math.max(0.75, r * 0.03));
}

function inks(dock) {
  return { lit: custom(settings.litColor) || '#FFF1EBD6', dark: custom(settings.darkColor) || '#3A808080', text: custom(settings.textColor) || dock.theme.dateText };
}

function labelText(m) {
  if (settings.label === 'name') return m.name;
  if (settings.label === 'percent') return Math.round(m.illum * 100) + '%';
  return '';
}

function layout(canvas, dock, k = 1) {
  const icon = dock.standalone ? 48 : dock.iconSize;
  const d = Math.max(10, Math.round(icon * 0.62 * (settings.scale || 1) * k));
  const vertical = dock.axis === 'vertical' && !dock.standalone;
  const m = moonNow(Date.now());
  const str = labelText(m);
  const style = { size: Math.max(9, icon * 0.24 * k), font: settings.font || undefined };
  let tw = str ? Math.ceil(canvas.measureText(str, style).width) : 0;
  if (vertical && tw > icon) tw = 0;
  const gap = Math.round(6 * k);
  return { d, vertical, m, str: tw ? str : '', style, tw, gap,
    w: vertical ? Math.max(d, tw) : d + (tw ? gap + tw : 0), h: vertical ? d + (tw ? gap + style.size : 0) : d };
}

export function measure(canvas, dock) {
  const l = layout(canvas, dock);
  const icon = Math.round(dock.iconSize);
  return l.vertical ? { main: Math.ceil(l.h + 8), cross: icon } : { main: Math.ceil(l.w + 10), cross: icon };
}

export function draw(canvas, dock) {
  const W = canvas.width, H = canvas.height;
  let l = layout(canvas, dock);
  if (dock.standalone) l = layout(canvas, dock, Math.max(0.5, Math.min((W * 0.85) / Math.max(1, l.w), (H * 0.85) / Math.max(1, l.h))));
  const c = inks(dock);
  if (l.vertical) {
    const top = (H - l.h) / 2;
    moon(canvas, W / 2, top + l.d / 2, l.d / 2, l.m.f, c.lit, c.dark);
    if (l.str) canvas.text(l.str, W / 2, top + l.d + l.gap, { ...l.style, align: 'center', baseline: 'top', color: c.text });
    return;
  }
  const x = (W - l.w) / 2;
  moon(canvas, x + l.d / 2, H / 2, l.d / 2, l.m.f, c.lit, c.dark);
  if (l.str) canvas.text(l.str, x + l.d + l.gap, H / 2, { ...l.style, baseline: 'middle', color: c.text });
}

export function onSettingsChanged() {
  requestRedraw();
  if (popup.isOpen) popup.redraw();
}

export function tick() {
  if (popup.isOpen) popup.redraw();
}

// --- the popup ---------------------------------------------------------------------------------------

function when(ms) {
  const off = system.now().utcOffset * 60000;
  const t = new Date(ms + off);
  const h = t.getUTCHours(), mi = String(t.getUTCMinutes()).padStart(2, '0');
  const time = system.hour12 ? `${(h % 12) || 12}:${mi} ${h < 12 ? 'AM' : 'PM'}` : `${String(h).padStart(2, '0')}:${mi}`;
  const days = Math.round((Date.UTC(t.getUTCFullYear(), t.getUTCMonth(), t.getUTCDate()) -
    Math.floor((Date.now() + off) / 86400000) * 86400000) / 86400000);
  const rel = days === 0 ? 'today' : days === 1 ? 'tomorrow' : `in ${days} days`;
  return `${system.shortDayNames[t.getUTCDay()]} ${t.getUTCDate()} ${system.shortMonthNames[t.getUTCMonth()]}, ${time} (${rel})`;
}

export function onClick(e) {
  if (e.closedPopup) return;
  popup.open(300, 210);
}

export function drawPopup(canvas, dock) {
  hits = [];
  const W = canvas.width, P = 16;
  const ink = dock.theme.labelText, dim = dock.theme.dateText;
  const c = inks(dock);
  const m = moonNow(Date.now());
  moon(canvas, P + 30, 46, 30, m.f, c.lit, c.dark);
  canvas.text(m.name, P + 76, 18, { size: 15, weight: 'semibold', color: ink, maxWidth: W - P - 92 });
  canvas.text(`${Math.round(m.illum * 100)}% lit · ${m.f < 0.5 ? 'waxing' : 'waning'}`, P + 76, 42, { size: 12.5, color: dim });
  canvas.text(`Day ${m.age.toFixed(1)} of ${((m.l.nextNew - m.l.prevNew) / 86400000).toFixed(1)}`, P + 76, 60, { size: 12.5, color: dim });
  canvas.line(P, 92.5, W - P, 92.5, '#26808080', 1);
  upcoming(Date.now(), 4).forEach((u, i) => {
    const y = 102 + i * 26;
    canvas.text(u.name, P, y + 11, { size: 12.5, baseline: 'middle', color: ink });
    canvas.text(when(u.t), W - P, y + 11, { size: 12, align: 'right', baseline: 'middle', color: dim });
  });
}
