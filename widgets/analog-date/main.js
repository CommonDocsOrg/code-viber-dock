// Calendar page: today's date as a skeuomorphic page (header with the month or weekday, a big day
// number; "flip": a split page over stacked pages with a curled corner, "flat": a plain page).
//
// Cost: one wake-up a day. timer.setRefresh(86400000) is aligned to local midnight by the host
// (and re-aligned when the clock or time zone changes, or the machine resumes); nothing runs in
// between. With settings.timeZone (round 5: a Windows zone key) the date is that zone's
// (system.localTime) and setRefresh(86400000, { timeZone }) wakes at that zone's midnight (a zone
// 5:45 or 12:45 from UTC included: the host aligns to the zone's own time of day). When the date changes while the dock is on screen the page flips (0.45 s at ~60 frames
// a second, never with reduce motion).
//
// Testing without touching the system clock: an item setting the schema does not know,
// "debugNow": "2026-12-31T23:59:57", makes the widget run on a fake clock that starts at that
// local time and advances in real time; the midnight timer is then a setTimeout to the fake
// midnight, and every date the page shows is written to the widget's storage
// ("debugShown": { date, weekday, flips }) so a test can read it back
// (%LocalAppData%\WinDock\widget-data\analog-date.json).

const FLIP_MS = 450;
const FRAME_MS = 16;

// --- the date ---------------------------------------------------------------------------------------

let debugBase = null;  // { ms (fake local time as a UTC timestamp), at (Date.now() when set) }
let debugTimer = 0;
let flips = 0;

// Local date parts (of settings.timeZone), from the host (system.localTime) or the fake clock.
function today() {
  if (debugBase) {
    const d = new Date(debugBase.ms + (Date.now() - debugBase.at));
    return {
      year: d.getUTCFullYear(), month: d.getUTCMonth() + 1, day: d.getUTCDate(), weekday: d.getUTCDay(),
      hour: d.getUTCHours(), minute: d.getUTCMinutes(), second: d.getUTCSeconds(), millisecond: d.getUTCMilliseconds(),
    };
  }
  return system.localTime(settings.timeZone || '');
}

// Milliseconds from `t` (date parts) to the next local midnight.
export function msToMidnight(t) {
  return 86400000 - (((t.hour * 60 + t.minute) * 60 + t.second) * 1000 + (t.millisecond || 0));
}

const key = (t) => t.year * 10000 + t.month * 100 + t.day;
const iso = (t) => t.year + '-' + String(t.month).padStart(2, '0') + '-' + String(t.day).padStart(2, '0');

let shown = today();
let flip = null;  // { start, from } while the page turns
let frameTimer = 0;

function arm() {
  if (debugTimer) clearTimeout(debugTimer);
  debugTimer = 0;
  if (debugBase) {
    timer.setRefresh(0);
    debugTimer = setTimeout(() => {
      debugTimer = 0;
      tick();
      requestRedraw();
      arm();
    }, msToMidnight(today()) + 30);
  } else {
    timer.setRefresh(86400000, { timeZone: settings.timeZone || '' });  // on the zone's midnight (aligned by the host)
  }
}

function configure() {
  debugBase = null;
  const m = typeof settings.debugNow === 'string' &&
    /^(\d{4})-(\d\d)-(\d\d)T(\d\d):(\d\d):(\d\d)$/.exec(settings.debugNow);
  if (m) {
    debugBase = { ms: Date.UTC(+m[1], +m[2] - 1, +m[3], +m[4], +m[5], +m[6]), at: Date.now() };
    console.log('debug clock from ' + settings.debugNow);
  }
  shown = today();
  report();
  arm();
}

function report() {
  if (debugBase) storage.set('debugShown', { date: iso(shown), weekday: shown.weekday, flips });
}

configure();

export function onSettingsChanged() {
  configure();
}

// Midnight (or a time change): a new date flips the page in (when it can be seen).
export function tick() {
  const now = today();
  if (key(now) === key(shown)) return false;  // early or no change: no redraw
  const from = shown;
  shown = now;
  ++flips;
  report();
  if (settings.style === 'flip' && dock.visible && !dock.reduceMotion) {
    flip = { start: Date.now(), from };
    if (frameTimer) clearInterval(frameTimer);
    frameTimer = setInterval(() => {
      if (flip && Date.now() - flip.start >= FLIP_MS) {
        flip = null;
        clearInterval(frameTimer);
        frameTimer = 0;
      }
      requestRedraw();
    }, FRAME_MS);
  }
  return true;
}

export function onVisibilityChange(e) {
  if (e.visible && tick() === false) return;
  if (e.visible) requestRedraw();
}

export function onClick(e) {
  if (settings.click === 'calendar') {
    if (!e.closedPopup) popup.calendar();
  } else if (settings.click === 'target' && settings.clickTarget) {
    shell.open(settings.clickTarget);
  }
}

// --- drawing ----------------------------------------------------------------------------------------

function boxSize() {
  return settings.size > 0 ? settings.size : Math.round(dock.iconSize);
}

export function measure() {
  const s = boxSize();
  return { main: s, cross: s };
}

// The page's parts for a box of w x h DIPs.
function geometry(w, h) {
  const s = Math.min(w, h);
  const flipStyle = settings.style === 'flip';
  const m = s * 0.06;
  const stack = flipStyle ? s * 0.07 : 0;  // the pages under it
  const x = (w - s) / 2 + m, y = (h - s) / 2 + m;
  const pw = s - 2 * m, ph = s - 2 * m - stack;
  const radius = s * 0.11;
  const header = ph * 0.3;
  return { s, x, y, pw, ph, radius, header, stack, flipStyle, split: y + header + (ph - header) * 0.55 };
}

function pagePath(canvas, g) {
  return canvas.path().moveTo(g.x + g.radius, g.y)
    .lineTo(g.x + g.pw - g.radius, g.y).arc(g.x + g.pw - g.radius, g.y + g.radius, g.radius, -Math.PI / 2, 0)
    .lineTo(g.x + g.pw, g.y + g.ph - g.radius).arc(g.x + g.pw - g.radius, g.y + g.ph - g.radius, g.radius, 0, Math.PI / 2)
    .lineTo(g.x + g.radius, g.y + g.ph).arc(g.x + g.radius, g.y + g.ph - g.radius, g.radius, Math.PI / 2, Math.PI)
    .lineTo(g.x, g.y + g.radius).arc(g.x + g.radius, g.y + g.radius, g.radius, Math.PI, Math.PI * 1.5).close();
}

const upper = (s) => (s || '').toUpperCase();

// One whole page for date `t` (header, number, footer), clipped to the page shape.
function drawPage(canvas, g, t) {
  const body = g.flipStyle
    ? canvas.linearGradient(0, g.y + g.header, 0, g.y + g.ph, [[0, '#FFFFFFFF'], [1, '#FFEDEDED']])
    : '#FFFAFAFA';
  canvas.save();
  canvas.clipPath(pagePath(canvas, g));
  canvas.fillRect(g.x, g.y, g.pw, g.ph, body);
  const accent = settings.accent || '#FFE0453A';
  canvas.fillRect(g.x, g.y, g.pw, g.header, g.flipStyle
    ? canvas.linearGradient(0, g.y, 0, g.y + g.header, [[0, accent], [1, accent]])
    : accent);
  if (g.flipStyle) canvas.fillRect(g.x, g.y, g.pw, g.header * 0.45, '#22FFFFFF');  // a sheen on the header
  canvas.fillRect(g.x, g.y + g.header - 0.6, g.pw, 0.6, '#30000000');
  // Header: the month, else the weekday.
  const headerText = settings.showMonth ? upper(system.shortMonthNames[t.month - 1])
    : settings.showWeekday ? upper(system.shortDayNames[t.weekday]) : '';
  if (headerText) {
    canvas.text(headerText, g.x + g.pw / 2, g.y + g.header / 2 + g.header * 0.04,
      { size: Math.max(6, g.header * 0.62), weight: 'bold', align: 'center', baseline: 'middle', color: '#FFFFFFFF' });
  }
  // Footer: the weekday (when the header shows the month) and / or the year.
  const parts = [];
  if (settings.showMonth && settings.showWeekday) parts.push(system.shortDayNames[t.weekday]);
  if (settings.showYear) parts.push(String(t.year));
  const footer = parts.join(' ');
  const bodyTop = g.y + g.header, bodyH = g.ph - g.header;
  const footerH = footer ? Math.max(6, bodyH * 0.22) : 0;
  const numberSize = (bodyH - footerH) * (footer ? 0.86 : 0.8);
  canvas.text(String(t.day), g.x + g.pw / 2, bodyTop + (bodyH - footerH) / 2 + bodyH * 0.03,
    { font: 'Segoe UI', size: numberSize, weight: g.flipStyle ? 'semibold' : 'normal', align: 'center', baseline: 'middle', color: '#FF262626' });
  if (footer) {
    canvas.text(footer, g.x + g.pw / 2, g.y + g.ph - footerH * 0.55,
      { size: footerH * 0.85, weight: 'semibold', align: 'center', baseline: 'middle', color: '#FF7A7A7A' });
  }
  canvas.restore();
}

// The page drawn squashed vertically about the split line (scale 1: as is; clipped to a half).
function drawHalf(canvas, g, t, top, scale) {
  if (scale <= 0.01) return;
  canvas.save();
  canvas.translate(0, g.split);
  canvas.scaleBy(1, scale);
  canvas.translate(0, -g.split);
  if (top) canvas.clip(g.x - 1, g.y - 1, g.pw + 2, g.split - g.y + 1);
  else canvas.clip(g.x - 1, g.split, g.pw + 2, g.y + g.ph - g.split + 1);
  drawPage(canvas, g, t);
  // The turning flap darkens as it lies flatter.
  if (scale < 1) canvas.fillRect(g.x, top ? g.y : g.split, g.pw, top ? g.split - g.y : g.y + g.ph - g.split, 'rgba(0, 0, 0, ' + ((1 - scale) * 0.35).toFixed(3) + ')');
  canvas.restore();
}

export function draw(canvas) {
  const g = geometry(canvas.width, canvas.height);
  // Drop shadow and, for the flip calendar, the pages underneath.
  canvas.fillRoundRect(g.x + 0.5, g.y + 1.2, g.pw, g.ph + g.stack, g.radius, '#40000000');
  if (g.flipStyle) {
    for (let i = 2; i >= 1; i--) {
      const dy = (g.stack / 2) * i, inset = i * 1.2;
      canvas.fillRoundRect(g.x + inset, g.y + dy, g.pw - 2 * inset, g.ph, g.radius, i === 2 ? '#FFC8C8C8' : '#FFDCDCDC');
      canvas.strokeRoundRect(g.x + inset, g.y + dy, g.pw - 2 * inset, g.ph, g.radius, '#30000000', 0.5);
    }
  }
  const t = shown;
  if (flip) {
    // The old top half folds down over the split, then the new bottom half comes up under it.
    const p = Math.min(1, (Date.now() - flip.start) / FLIP_MS);
    const eased = p < 0.5 ? 2 * p * p : 1 - Math.pow(-2 * p + 2, 2) / 2;
    drawHalf(canvas, g, t, true, 1);
    drawHalf(canvas, g, eased < 0.5 ? flip.from : t, false, 1);
    if (eased < 0.5) drawHalf(canvas, g, flip.from, true, 1 - eased * 2);
    else drawHalf(canvas, g, t, false, eased * 2 - 1);
  } else {
    drawPage(canvas, g, t);
  }
  canvas.strokeRoundRect(g.x, g.y, g.pw, g.ph, g.radius, '#40000000', 0.6);
  if (g.flipStyle) {
    // The split of a flip calendar with its hinges, and a curled corner.
    canvas.line(g.x, g.split, g.x + g.pw, g.split, '#38000000', 0.8);
    canvas.line(g.x, g.split + 0.8, g.x + g.pw, g.split + 0.8, '#50FFFFFF', 0.6);
    const hr = Math.max(1, g.s * 0.025);
    canvas.fillRoundRect(g.x - hr * 0.4, g.split - hr * 1.6, hr * 1.6, hr * 3.2, hr * 0.6, '#FF6A6A6A');
    canvas.fillRoundRect(g.x + g.pw - hr * 1.2, g.split - hr * 1.6, hr * 1.6, hr * 3.2, hr * 0.6, '#FF6A6A6A');
    const c = g.s * 0.16, cx = g.x + g.pw, cy = g.y + g.ph;
    canvas.fill(canvas.path().moveTo(cx - c, cy).lineTo(cx, cy - c).lineTo(cx - c * 0.15, cy - c * 0.15).close(), '#30000000');
    canvas.fill(canvas.path().moveTo(cx - c, cy - 0.3).lineTo(cx - 0.3, cy - c).quadTo(cx - c * 0.55, cy - c * 0.55, cx - c, cy - 0.3).close(),
      canvas.linearGradient(cx - c, cy - c, cx - c * 0.4, cy - c * 0.4, [[0, '#FFF6F6F6'], [1, '#FFBDBDBD']]));
  }
  // Binding rings on the header.
  const rw = Math.max(1.2, g.s * 0.035), rh = Math.max(2.5, g.header * 0.45);
  for (const fx of [0.3, 0.7]) {
    canvas.fillRoundRect(g.x + g.pw * fx - rw / 2, g.y - rh * 0.45, rw, rh, rw / 2, '#FF4A4A4A');
    canvas.fillRoundRect(g.x + g.pw * fx - rw / 2 + rw * 0.25, g.y - rh * 0.45, rw * 0.25, rh * 0.8, rw / 4, '#60FFFFFF');
  }
}
