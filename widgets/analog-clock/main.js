// Analog clock: a face file (faces/<name>.json, see docs/widgets.md "Analog clock faces") drawn
// with the canvas API into a square box (the dock's icon size, or settings.size).
//
// Cost: without a second hand the clock wakes once a minute (timer.setRefresh(60000), aligned to
// the minute); with a ticking second hand once a second; with a smooth one 30 times a second, but
// only while the dock is on screen (dock.visible): onVisibilityChange stops every timer while the
// dock is hidden and redraws once when it comes back. The sweep-in (the hands turning from 12 to
// the time when the clock appears or its face changes) runs ~60 frames a second for 0.7 s and then
// stops; never with reduce motion.
//
// Time zone (round 5): settings.timeZone (a Windows zone key, "" local) goes to every
// system.localTime / formatTime / setRefresh call; every zone is a whole number of minutes from UTC,
// so the minute and second wake-ups fall on the same instants in any zone. settings.label is drawn
// under the dial ("Tokyo", or "Tokyo · 21:04" with the digital time).
//
// Hover (round 7 phase 7C): settings.hoverScale (1 - 2.5, default 1.6; 1 = off) grows the box
// while the pointer is over it: a 150 ms ease of 16 ms frames on enter and on leave, no timer at
// rest (see "growing on hover" below).

const SWEEP_MS = 700;
const FRAME_MS = 16;   // the sweep
const SMOOTH_MS = 33;  // the smooth second hand (30 frames a second)

// Every key a face may set, with the value used when the face leaves it out. Lengths and widths
// are fractions of the dial's radius (inside the rim).
const DEFAULT_FACE = {
  name: 'Default',
  dial: { fill: '#FFF8F8F8', radius: 1, rim: null, border: null, shadow: null, gloss: null },
  ticks: { major: null, minor: null },
  numerals: { style: 'none', font: 'Segoe UI', size: 0.2, weight: 600, color: '#FF222222', radius: 0.66 },
  hands: {
    hour: { length: 0.5, width: 0.08, tail: 0.1, color: '#FF222222', shape: 'taper', counterweight: 0, tipDot: 0 },
    minute: { length: 0.76, width: 0.06, tail: 0.12, color: '#FF222222', shape: 'taper', counterweight: 0, tipDot: 0 },
    second: { length: 0.84, width: 0.02, tail: 0.18, color: '#FFCC2222', shape: 'line', counterweight: 0, tipDot: 0 },
    cap: { radius: 0.05, color: '#FF222222', ring: null, ringWidth: 0.02 },
    shadow: null,
  },
  date: { fill: '#FFFFFFFF', border: '#FF999999', color: '#FF222222', font: 'Segoe UI', weight: 600, size: 0.17, radius: 0.52 },
  text: null,
};
const TICK_DEFAULT = { count: 12, length: 0.1, width: 0.03, color: '#FF222222', inset: 0.05, shape: 'line' };

const ROMAN = ['XII', 'I', 'II', 'III', 'IIII', 'V', 'VI', 'VII', 'VIII', 'IX', 'X', 'XI'];

// --- faces ----------------------------------------------------------------------------------------

const isObject = (v) => v !== null && typeof v === 'object' && !Array.isArray(v);

// `base` overridden by `over` (objects merged key by key, anything else replaced).
function merge(base, over) {
  if (!isObject(over)) return over === undefined ? base : over;
  const out = isObject(base) ? { ...base } : {};
  for (const [k, v] of Object.entries(over)) out[k] = isObject(v) && isObject(out[k]) ? merge(out[k], v) : v;
  return out;
}

function normalize(f) {
  const face = merge(DEFAULT_FACE, f);
  for (const k of ['major', 'minor']) {
    if (isObject(face.ticks[k])) face.ticks[k] = merge(TICK_DEFAULT, face.ticks[k]);
  }
  return face;
}

let face = normalize({});
let faceName = '';

function loadFace(name) {
  const files = widget.files('faces/*.json');
  let path = 'faces/' + name + '.json';
  if (!files.includes(path)) {
    if (files.length) console.warn('face "' + name + '" not found, using ' + files[0]);
    path = files[0];
  }
  faceName = name;
  if (!path) {
    face = normalize({});
    return;
  }
  try {
    face = normalize(widget.readJson(path));
  } catch (e) {
    console.error('face ' + path + ': ' + e.message);
    face = normalize({});
  }
}

// --- time and animation ----------------------------------------------------------------------------

let sweep = null;       // { start, last } while the hands sweep in (ms; 0 until its first frame)
let pendingSweep = false;  // sweep when the dock next comes on screen
let frameTimer = 0;     // setInterval of the sweep / the smooth second hand

const smooth = () => settings.seconds && settings.secondsMotion === 'smooth' && !dock.reduceMotion;
const zone = () => settings.timeZone || '';

// Picks the cheapest timer for what is shown now (see the cost note at the top).
function schedule() {
  if (frameTimer) clearInterval(frameTimer);
  frameTimer = 0;
  if (!dock.visible) {
    timer.setRefresh(0);  // hidden: nothing at all
    return;
  }
  if (sweep || smooth()) {
    timer.setRefresh(0);
    frameTimer = setInterval(frame, sweep ? FRAME_MS : SMOOTH_MS);
    return;
  }
  timer.setRefresh(settings.seconds ? 1000 : 60000, { timeZone: zone() });
}

function frame() {
  const now = Date.now();
  if (sweep) {
    // The sweep's clock starts with its first frame and pauses while frames stall (the dock's
    // thread busy, e.g. loading icons at startup), so the motion is seen instead of skipped.
    if (!sweep.start) sweep.start = now;
    else if (now - sweep.last > 50) sweep.start += now - sweep.last - FRAME_MS;
    sweep.last = now;
    if (now - sweep.start >= SWEEP_MS) {
      sweep = null;
      schedule();  // back to ticking / smooth / once a minute
    }
  }
  requestRedraw();
}

function startSweep() {
  if (!settings.sweep || dock.reduceMotion) return;
  if (!dock.visible) {
    pendingSweep = true;
    return;
  }
  pendingSweep = false;
  sweep = { start: 0, last: 0 };
  schedule();
}

function configure(first) {
  const changed = settings.face !== faceName;
  if (changed) loadFace(settings.face);
  if (first || changed) startSweep();
  schedule();
}
configure(true);

export function onSettingsChanged() {
  configure(false);
  // The hover size follows a changed hoverScale at once (no animation).
  stopGrow();
  grow.k = canGrow() && dock.hovered ? hoverTarget() : 1;
}

export function onVisibilityChange(e) {
  if (!e.visible && grow.timer) {  // no frames off screen: finish the hover ease at once
    stopGrow();
    grow.k = grow.to;
  }
  if (e.visible && pendingSweep) startSweep();
  schedule();
  if (e.visible) requestRedraw();  // the time moved on while it was hidden
}

// Hand angles in radians (0 = 12 o'clock, clockwise).
function angles() {
  const t = system.localTime(zone());
  const sec = t.second + (smooth() ? t.millisecond / 1000 : 0);
  const min = t.minute + (settings.seconds ? sec / 60 : 0);
  const hour = (t.hour % 12) + min / 60;
  const a = { hour: (hour / 12) * Math.PI * 2, minute: (min / 60) * Math.PI * 2, second: (sec / 60) * Math.PI * 2, t };
  if (sweep) {
    const p = sweep.start ? Math.min(1, (Date.now() - sweep.start) / SWEEP_MS) : 0;
    const k = 1 - Math.pow(1 - p, 3);  // ease-out cubic: from 12 o'clock to the time
    a.hour *= k;
    a.minute *= k;
    a.second *= k;
  }
  return a;
}

// --- drawing ----------------------------------------------------------------------------------------

function boxSize() {
  return settings.size > 0 ? settings.size : Math.round(dock.iconSize);
}

// --- growing on hover (round 7 phase 7C) -----------------------------------------------------------
// measure() returns the box times `grow.k`; on hover k eases to settings.hoverScale over HOVER_MS
// (16 ms frames only during the ease; the host re-measures after every redraw request and lays the
// dock out again), on leave back to 1. Reduce motion (or a hidden dock): it jumps. Never standalone
// (the window is the size) or in a preview.

const HOVER_MS = 150;
const grow = { k: 1, from: 1, to: 1, start: 0, timer: 0 };

const hoverTarget = () => Math.max(1, Math.min(2.5, Number(settings.hoverScale) || 1));
const canGrow = () => !dock.standalone && !dock.preview && hoverTarget() > 1.001;

function stopGrow() {
  if (grow.timer) clearInterval(grow.timer);
  grow.timer = 0;
}

function growFrame() {
  const p = Math.min(1, (Date.now() - grow.start) / HOVER_MS);
  const eased = 1 - Math.pow(1 - p, 3);  // ease-out cubic
  grow.k = grow.from + (grow.to - grow.from) * eased;
  if (p >= 1) {
    grow.k = grow.to;
    stopGrow();
  }
  requestRedraw();
}

function growTo(target) {
  stopGrow();
  if (Math.abs(grow.k - target) < 0.001) return;
  if (dock.reduceMotion || !dock.visible) {
    grow.k = target;
    requestRedraw();
    return;
  }
  grow.from = grow.k;
  grow.to = target;
  grow.start = Date.now();
  grow.timer = setInterval(growFrame, FRAME_MS);
}

export function onHover(e) {
  growTo(e.inside && canGrow() ? hoverTarget() : 1);
}

export function measure() {
  const s = boxSize() * (canGrow() ? grow.k : 1);
  return { main: s, cross: s };
}

// A face paint: a colour, or { radial: stops, focusY } / { linear: stops, angle } in dial terms.
function paint(canvas, p, cx, cy, r) {
  if (!isObject(p)) return p;
  if (Array.isArray(p.radial)) {
    return canvas.radialGradient(cx, cy, r, r, p.radial, (p.focusX ?? 0) * r, (p.focusY ?? -0.3) * r);
  }
  if (Array.isArray(p.linear)) {
    const a = ((p.angle ?? 90) * Math.PI) / 180;  // 90: top to bottom
    const dx = Math.cos(a) * r, dy = Math.sin(a) * r;
    return canvas.linearGradient(cx - dx, cy - dy, cx + dx, cy + dy, p.linear);
  }
  return '#FF808080';
}

function drawDial(canvas, cx, cy, R) {
  const d = face.dial;
  if (isObject(d.shadow)) {
    const s = d.shadow, rr = R * (1 + (s.size ?? 0.08)), oy = cy + R * (s.offsetY ?? 0.04);
    canvas.fillCircle(cx, oy, rr, canvas.radialGradient(cx, oy, rr, rr, [[R / rr * 0.9, s.color ?? '#55000000'], [1, '#00000000']]));
  }
  let inner = R;
  if (isObject(d.rim) && d.rim.width > 0) {
    canvas.fillCircle(cx, cy, R, paint(canvas, d.rim.color ?? '#FF808080', cx, cy, R));
    inner = R * (1 - d.rim.width);
    if (d.rim.highlight) {
      canvas.strokeCircle(cx, cy, R - (R - inner) * 0.3,
        canvas.linearGradient(cx, cy - R, cx, cy + R, [[0, d.rim.highlight], [0.5, '#00FFFFFF']]), Math.max(0.6, (R - inner) * 0.35));
    }
  }
  canvas.fillCircle(cx, cy, inner, paint(canvas, d.fill, cx, cy, inner));
  if (isObject(d.border) && d.border.width > 0) {
    canvas.strokeCircle(cx, cy, inner - (d.border.width * inner) / 2, d.border.color ?? '#40000000', Math.max(0.5, d.border.width * inner));
  }
  if (d.gloss) {
    canvas.fillEllipse(cx, cy - inner * 0.45, inner * 0.75, inner * 0.45,
      canvas.linearGradient(cx, cy - inner, cx, cy, [[0, d.gloss], [1, '#00FFFFFF']]));
  }
  return inner;
}

function drawTicks(canvas, cx, cy, r) {
  const { major, minor } = face.ticks;
  const one = (spec, i, count) => {
    const a = (i / count) * Math.PI * 2, sin = Math.sin(a), cos = -Math.cos(a);
    const outer = r * (1 - spec.inset), len = Math.max(0.8, spec.length * r), w = Math.max(0.6, spec.width * r);
    if (spec.shape === 'dot') {
      canvas.fillCircle(cx + sin * (outer - len / 2), cy + cos * (outer - len / 2), w / 2, spec.color);
      return;
    }
    const inner = outer - len;
    canvas.line(cx + sin * inner, cy + cos * inner, cx + sin * outer, cy + cos * outer, spec.color,
      { width: w, cap: spec.shape === 'block' ? 'butt' : 'round' });
  };
  if (minor && minor.count > 0) {
    for (let i = 0; i < minor.count; i++) {
      // Skip the places of the major ticks (when the counts line up).
      if (major && major.count > 0 && (i * major.count) % minor.count === 0) continue;
      one(minor, i, minor.count);
    }
  }
  if (major && major.count > 0) for (let i = 0; i < major.count; i++) one(major, i, major.count);
}

function drawNumerals(canvas, cx, cy, r, skip) {
  const n = face.numerals;
  let style = n.style;
  if (style === 'none' || !style) return;
  // Twelve numerals do not fit a small dial: below 26 DIPs of radius only 12, 3, 6 and 9 are drawn.
  if (r < 26 && style !== 'cardinal') style = style + '-cardinal';
  const size = Math.max(5.5, n.size * r * (style.endsWith('cardinal') && n.style !== 'cardinal' ? 1.15 : 1));
  const rr = n.radius * r;
  for (let i = 0; i < 12; i++) {
    if (style.endsWith('cardinal') && i % 3 !== 0) continue;
    if (skip.includes(i)) continue;
    const label = style.startsWith('roman') ? ROMAN[i] : String(i === 0 ? 12 : i);
    const a = (i / 12) * Math.PI * 2;
    canvas.text(label, cx + Math.sin(a) * rr, cy - Math.cos(a) * rr,
      { font: n.font, size, weight: n.weight, align: 'center', baseline: 'middle', color: n.color });
  }
}

// The date window(s) for settings.dateOnFace; returns the numerals they cover (0 = 12).
function dateSpots(t) {
  const mode = settings.dateOnFace;
  const mon = (system.shortMonthNames[t.month - 1] || '').toUpperCase();
  const wd = (system.shortDayNames[t.weekday] || '').toUpperCase();
  switch (mode) {
    case 'day': return [{ at: 3, text: String(t.day) }];
    case 'month-day': return [{ at: 6, text: mon + ' ' + t.day }];
    case 'year': return [{ at: 6, text: String(t.year) }];
    case 'full': return [{ at: 0, text: wd + ' ' + t.day }, { at: 6, text: mon + ' ' + t.year }];
    default: return [];
  }
}

function drawDates(canvas, cx, cy, r, spots) {
  const d = face.date;
  const size = Math.max(6.5, d.size * r);
  const style = { font: d.font, size, weight: d.weight, align: 'center', baseline: 'middle', color: d.color };
  for (const s of spots) {
    const m = canvas.measureText(s.text, style);
    const w = m.width + size * 0.5, h = size * 1.3;
    const dist = d.radius * r;
    let x = cx, y = cy;
    if (s.at === 3) x = cx + dist;
    else if (s.at === 6) y = cy + dist;
    else if (s.at === 0) y = cy - dist;
    // Keep the window on the dial.
    if (s.at === 3) x = Math.min(x, cx + r - w / 2 - 1);
    canvas.fillRoundRect(x - w / 2, y - h / 2, w, h, Math.min(2, h / 4), d.fill);
    if (d.border) canvas.strokeRoundRect(x - w / 2, y - h / 2, w, h, Math.min(2, h / 4), d.border, 0.6);
    canvas.text(s.text, x, y, style);
  }
}

function handShape(canvas, spec, cx, cy, r, color) {
  const len = spec.length * r, tail = spec.tail * r, w = Math.max(0.8, spec.width * r);
  switch (spec.shape) {
    case 'line':
      canvas.line(cx, cy + tail, cx, cy - len, color, { width: w, cap: 'round' });
      break;
    case 'block':
      canvas.fillRect(cx - w / 2, cy - len, w, len + tail, color);
      break;
    case 'leaf':
      canvas.fill(canvas.path().moveTo(cx, cy + tail)
        .quadTo(cx - w, cy - len * 0.35, cx, cy - len)
        .quadTo(cx + w, cy - len * 0.35, cx, cy + tail).close(), color);
      break;
    case 'taper':
    default:
      canvas.fill(canvas.path().moveTo(cx - w / 2, cy + tail).lineTo(cx - w * 0.2, cy - len)
        .lineTo(cx + w * 0.2, cy - len).lineTo(cx + w / 2, cy + tail).close(), color);
      break;
  }
  if (spec.counterweight > 0) canvas.fillCircle(cx, cy + tail, spec.counterweight * r, color);
  if (spec.tipDot > 0) canvas.fillCircle(cx, cy - len, spec.tipDot * r, color);
}

function drawHand(canvas, spec, angle, cx, cy, r) {
  const shadow = face.hands.shadow;
  if (shadow) {
    canvas.save();
    canvas.translate(Math.max(0.5, r * 0.02), Math.max(0.7, r * 0.035));
    canvas.rotate(angle, cx, cy);
    handShape(canvas, spec, cx, cy, r, shadow);
    canvas.restore();
  }
  canvas.save();
  canvas.rotate(angle, cx, cy);
  handShape(canvas, spec, cx, cy, r, spec.color);
  canvas.restore();
}

export function draw(canvas, dock) {
  const w = canvas.width, h = canvas.height;
  const under = [settings.label || '', settings.showDigital ? system.formatTime(false, zone()) : ''].filter((s) => s).join(' · ');
  const digital = under !== '';
  const textSize = dock.standalone ? Math.max(9, Math.min(28, Math.min(w, h) * 0.1))  // a window of its own
                                    : Math.max(7, Math.min(12, Math.min(w, h) * 0.2));
  const dialBox = Math.min(w, digital ? h - textSize - 1 : h);
  const cx = w / 2, cy = digital ? dialBox / 2 : h / 2;
  // Room for the shadow below the dial.
  const R = (dialBox / 2) * (face.dial.radius ?? 1) * (face.dial.shadow ? 0.93 : 0.98);
  const a = angles();
  const r = drawDial(canvas, cx, cy, R);
  drawTicks(canvas, cx, cy, r);
  const spots = dateSpots(a.t);
  drawNumerals(canvas, cx, cy, r, spots.map((s) => s.at));
  if (isObject(face.text) && face.text.value) {
    const t = face.text;
    canvas.text(String(t.value), cx, cy + (t.y ?? -0.35) * r,
      { font: t.font ?? 'Segoe UI', size: Math.max(5, (t.size ?? 0.08) * r), weight: t.weight ?? 600, align: 'center', baseline: 'middle', color: t.color ?? '#FF666666' });
  }
  drawDates(canvas, cx, cy, r, spots);
  const hands = face.hands;
  drawHand(canvas, hands.hour, a.hour, cx, cy, r);
  drawHand(canvas, hands.minute, a.minute, cx, cy, r);
  if (settings.seconds) drawHand(canvas, hands.second, a.second, cx, cy, r);
  const cap = hands.cap;
  if (isObject(cap) && cap.radius > 0) {
    canvas.fillCircle(cx, cy, Math.max(1, cap.radius * r), cap.color);
    if (cap.ring) canvas.strokeCircle(cx, cy, Math.max(1, cap.radius * r), cap.ring, Math.max(0.5, (cap.ringWidth ?? 0.02) * r));
  }
  if (digital) {
    canvas.text(under, w / 2, h, { size: textSize, weight: 'semibold', align: 'center', baseline: 'bottom', color: dock.theme.text });
  }
}
