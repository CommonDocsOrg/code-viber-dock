// Timer (round 28 phase 28B): a countdown timer, a Pomodoro timer, a stopwatch or a countdown to a
// date, chosen by settings.mode. The tile shows the time left (or gone) with a progress ring or bar;
// a click opens a popup with Start / Pause, Reset, Skip (Pomodoro) and the countdown's presets; a
// middle click starts / pauses; the wheel over a stopped countdown changes it by a minute.
//
// State (storage, per item: "timer:<item key>" or the Timer id setting) holds end timestamps, not
// ticks: a running timer is { running, endAt } (a stopwatch { running, startAt }), so it survives a
// dock restart and keeps counting while the dock is quit. One that ended while the dock was not
// running says so once when it loads.
//
// Cost: while running and seen, timer.setRefresh(1000) redraws once a second (the host stops the
// refresh while the dock is hidden). The end is one setTimeout (re-armed every 6 hours for a far
// date), which also fires on an auto-hidden dock, so the banner (notify.banner) comes on time.
// Stopped: no timer at all (a countdown to a date ticks once a minute while seen, every second in
// its last hour).

const MIN = 60000, HOUR = 3600000, DAY = 86400000;
const STEP = 6 * HOUR;  // the longest single setTimeout toward the end
const POPUP_W = 300;

let st = null;     // this item's timer
let endTimer = 0;  // the setTimeout for the end (0: none)
let hits = [];

const mode = () => settings.mode || 'countdown';
const key = () => 'timer:' + (settings.timerId || dock.itemKey || 'default');
const now = () => Date.now();
const clampInt = (v, lo, hi, d) => (Number.isFinite(v) ? Math.min(hi, Math.max(lo, Math.round(v))) : d);
const custom = (c) => (c && !/^#00[0-9a-f]{6}$/i.test(c) ? c : null);
const pad = (n) => String(n).padStart(2, '0');

function phaseLength(phase) {
  if (phase === 'short') return clampInt(settings.shortBreak, 1, 60, 5) * MIN;
  if (phase === 'long') return clampInt(settings.longBreak, 1, 120, 15) * MIN;
  return clampInt(settings.work, 1, 180, 25) * MIN;
}
const rounds = () => clampInt(settings.rounds, 1, 12, 4);
const countdownLength = () => clampInt(settings.minutes, 1, 1440, 5) * MIN;

function fresh() {
  const s = { mode: mode(), running: false, endAt: 0, remaining: 0, total: 0, startAt: 0, elapsed: 0, phase: 'work', round: 1, notified: 0 };
  if (s.mode === 'countdown') s.total = s.remaining = countdownLength();
  if (s.mode === 'pomodoro') s.total = s.remaining = phaseLength('work');
  return s;
}

// The preview's sample: a countdown with 7:42 of 10:00 left, a Pomodoro's second work round, a
// stopwatch at 1:23 (none of them running: a preview never arms a timer).
function sample() {
  const s = fresh();
  if (s.mode === 'countdown') { s.total = 10 * MIN; s.remaining = 462000; }
  if (s.mode === 'pomodoro') { s.round = 2; s.total = phaseLength('work'); s.remaining = Math.round(s.total * 0.72); }
  if (s.mode === 'stopwatch') s.elapsed = 83000;
  return s;
}

function save() {
  if (!dock.preview) storage.set(key(), st);
}

// --- what the timer shows --------------------------------------------------------------------------

// The date setting, "YYYY-MM-DD HH:MM" (or "YYYY-MM-DD") in local time -> ms since 1970; NaN when
// it does not read. The local offset is today's (a target across a daylight-saving change can be
// an hour off; see docs/guide-items.md).
function targetMs() {
  const m = /^\s*(\d{4})-(\d{1,2})-(\d{1,2})(?:[ T](\d{1,2}):(\d{2}))?\s*$/.exec(settings.target || '');
  if (!m) return NaN;
  const utc = Date.UTC(+m[1], +m[2] - 1, +m[3], m[4] ? +m[4] : 0, m[5] ? +m[5] : 0);
  return utc - system.now().utcOffset * MIN;
}

function remaining() {
  if (st.mode === 'date') return Math.max(0, targetMs() - now());
  if (st.mode === 'stopwatch') return 0;
  return st.running ? Math.max(0, st.endAt - now()) : st.remaining;
}
const elapsed = () => (st.running ? Math.max(0, now() - st.startAt) : st.elapsed);

// 0 - 1 of the ring / bar; null: none (a date: a full ring would say nothing).
function progress() {
  if (st.mode === 'stopwatch') return (elapsed() % MIN) / MIN;  // the minute going round
  if (st.mode === 'date') return null;
  return st.total > 0 ? 1 - remaining() / st.total : 0;
}

function clockText(ms, up) {
  const s = up ? Math.floor(ms / 1000) : Math.ceil(ms / 1000);
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), sec = s % 60;
  return h ? `${h}:${pad(m)}:${pad(sec)}` : `${m}:${pad(sec)}`;
}

function timeText() {
  if (st.mode === 'stopwatch') return clockText(elapsed(), true);
  const ms = remaining();
  if (st.mode === 'date') {
    if (!Number.isFinite(targetMs())) return '--';
    if (ms >= DAY) { const d = Math.floor(ms / DAY); return `${d}d ${Math.floor((ms % DAY) / HOUR)}h`; }
    if (ms >= HOUR) return `${Math.floor(ms / HOUR)}h ${pad(Math.floor((ms % HOUR) / MIN))}m`;
  }
  return clockText(ms, false);
}

// The widest text the tile may show in this mode, so the tile does not change its width each second.
function widestText() {
  if (st.mode === 'date') return '888d 88h';
  const longest = st.mode === 'stopwatch' ? (elapsed() >= HOUR ? HOUR : 0)
    : Math.max(st.total, remaining());
  return longest >= HOUR ? '88:88:88' : '88:88';
}

function phaseName() {
  if (st.mode !== 'pomodoro') return '';
  return st.phase === 'work' ? 'Work' : st.phase === 'short' ? 'Short break' : 'Long break';
}

function localTimeText(ms) {
  const t = new Date(ms + system.now().utcOffset * MIN);
  const h = t.getUTCHours(), mi = pad(t.getUTCMinutes());
  return system.hour12 ? `${(h % 12) || 12}:${mi} ${h < 12 ? 'AM' : 'PM'}` : `${pad(h)}:${mi}`;
}

function localDateText(ms) {
  const t = new Date(ms + system.now().utcOffset * MIN);
  return `${system.shortDayNames[t.getUTCDay()]} ${t.getUTCDate()} ${system.shortMonthNames[t.getUTCMonth()]} ${t.getUTCFullYear()}, ${localTimeText(ms)}`;
}

// --- running -----------------------------------------------------------------------------------------

function announce(title, text) {
  if (!settings.banner || dock.preview) return;
  try {
    notify.banner({ title, text, sound: !!settings.sound });
  } catch (e) {
    console.warn('notify.banner: ' + e.message);
  }
}

function nextPhase() {
  if (st.phase === 'work') {
    st.phase = st.round >= rounds() ? 'long' : 'short';
  } else {
    st.round = st.phase === 'long' ? 1 : st.round + 1;
    st.phase = 'work';
  }
  st.total = st.remaining = phaseLength(st.phase);
}

// The end came (or, `late`, it came while the dock was not running).
function finish(late) {
  const label = settings.label || '';
  const at = late ? ` at ${localTimeText(st.endAt)}` : '';
  st.running = false;
  st.remaining = 0;
  if (st.mode === 'pomodoro') {
    const was = st.phase;
    nextPhase();
    const next = st.phase === 'work' ? `Round ${st.round} of ${rounds()}: ${Math.round(st.total / MIN)} min of work`
      : `${phaseName()}: ${Math.round(st.total / MIN)} min`;
    if (settings.autoNext && !late) {
      st.endAt = now() + st.remaining;
      st.running = true;
    }
    announce(was === 'work' ? 'Time for a break' : 'Back to work', (late ? `Ended${at}. ` : '') + next +
      (st.running ? ' (started)' : ''));
  } else if (st.mode === 'countdown') {
    announce(label || 'Timer', `The ${Math.round(st.total / MIN)} min timer is done${late ? at : ''}`);
  }
  save();
}

function checkDate() {
  const t = targetMs();
  if (!Number.isFinite(t) || t > now() || st.notified === t) return;
  // Reached: once per target (a target already past when it was set says nothing).
  const recent = now() - t < 10 * MIN;
  st.notified = t;
  save();
  if (recent) announce(settings.label || 'Countdown', `${settings.label ? settings.label + ': ' : ''}${localDateText(t)} has come`);
}

function onEndTimer() {
  endTimer = 0;
  if (st.mode === 'date') {
    checkDate();
  } else if (st.running && st.mode !== 'stopwatch' && st.endAt <= now() + 20) {
    finish(false);
  }
  arm();
  sync();
  if (popup.isOpen) popup.redraw();
}

// One setTimeout toward the end (none while stopped, in a preview, or for a stopwatch).
function arm() {
  if (endTimer) { clearTimeout(endTimer); endTimer = 0; }
  if (dock.preview) return;
  let due = NaN;
  if (st.mode === 'date') {
    const t = targetMs();
    if (Number.isFinite(t) && st.notified !== t) due = t;
  } else if (st.running && st.mode !== 'stopwatch') {
    due = st.endAt;
  }
  if (!Number.isFinite(due)) return;
  endTimer = setTimeout(onEndTimer, Math.max(0, Math.min(STEP, due - now() + 20)));
}

// The redraw rate: once a second while it changes each second, once a minute for a date further
// than an hour away, else none (the host pauses it while the widget cannot be seen).
function sync() {
  if (dock.preview) return;
  let ms = 0;
  if (st.mode === 'date') ms = Number.isFinite(targetMs()) && remaining() > 0 ? (remaining() > HOUR + MIN ? MIN : 1000) : 0;
  else if (st.running) ms = 1000;
  timer.setRefresh(ms);
  requestRedraw();
}

function start() {
  if (st.mode === 'stopwatch') {
    st.startAt = now() - st.elapsed;
  } else {
    if (st.remaining <= 0) st.total = st.remaining = st.mode === 'pomodoro' ? phaseLength(st.phase) : countdownLength();
    st.endAt = now() + st.remaining;
  }
  st.running = true;
}

function pause() {
  if (st.mode === 'stopwatch') st.elapsed = elapsed();
  else st.remaining = remaining();
  st.running = false;
}

function reset() {
  const keepNotified = st.notified;
  st = fresh();
  st.notified = keepNotified;
}

function act(what) {
  if (dock.preview || st.mode === 'date') return;
  if (what === 'toggle') { if (st.running) pause(); else start(); }
  if (what === 'reset') reset();
  if (what === 'skip' && st.mode === 'pomodoro') { st.running = false; nextPhase(); }
  save();
  arm();
  sync();
  if (popup.isOpen) popup.redraw();
}

function load() {
  if (endTimer) { clearTimeout(endTimer); endTimer = 0; }
  if (dock.preview) {
    st = sample();
    return;
  }
  const s = storage.get(key(), null);
  st = s && typeof s === 'object' && s.mode === mode() ? Object.assign(fresh(), s) : fresh();
  if (st.running && st.mode !== 'stopwatch' && st.mode !== 'date' && st.endAt <= now()) finish(true);
  if (st.mode === 'date') checkDate();
  arm();
  sync();
}

load();

export function onSettingsChanged() {
  const before = st;
  if (!before || before.mode !== mode()) {
    load();  // another mode: that mode's own state (a fresh one)
    return;
  }
  // A stopped countdown that was not started takes a new length at once; so does a stopped
  // Pomodoro phase.
  if (!st.running && st.mode === 'countdown' && st.remaining === st.total) st.total = st.remaining = countdownLength();
  if (!st.running && st.mode === 'pomodoro' && st.remaining === st.total) st.total = st.remaining = phaseLength(st.phase);
  save();
  arm();
  sync();
}

storage.onChange((e) => {
  if (e.key === key()) load();  // another item with the same Timer id
});

export function tick() {
  if (st.mode === 'date') {
    checkDate();
    sync();  // the rate goes from a minute to a second in the last hour
  }
  if (popup.isOpen) popup.redraw();
}

// --- the tile ----------------------------------------------------------------------------------------

function fontOf(size, weight) {
  return { size, weight: weight || 'semibold', font: settings.font || undefined };
}
const textInk = (dock) => custom(settings.textColor) || dock.theme.text;
function ringInk(dock) {
  if (st.mode === 'pomodoro' && st.phase !== 'work') return custom(settings.breakColor) || '#FF4CAF50';
  return custom(settings.ringColor) || dock.theme.accent;
}

function layout(canvas, dock) {
  const icon = dock.standalone ? Math.min(canvas.width, canvas.height) / 1.2 : dock.iconSize;
  const k = settings.scale || 1;
  const style = settings.style || 'ring';
  const vertical = dock.axis === 'vertical' && !dock.standalone;
  if (style === 'ring') {
    const d = Math.round(icon * 0.84 * k);
    return { style, d, main: d + 6, icon, vertical };
  }
  const size = Math.max(9, icon * 0.3 * k);
  const w = Math.ceil(canvas.measureText(widestText(), fontOf(size)).width);
  return { style, size, w, main: vertical ? Math.max(w + 6, icon) : w + 12, icon, vertical };
}

export function measure(canvas, dock) {
  const l = layout(canvas, dock);
  const icon = Math.round(dock.iconSize);
  if (l.vertical) return { main: l.style === 'ring' ? l.main : Math.round(icon * 0.8), cross: icon };
  return { main: Math.ceil(l.main), cross: icon };
}

function ring(canvas, cx, cy, r, sw, p, ink, track) {
  canvas.strokeCircle(cx, cy, r, track, sw);
  if (p === null || p <= 0) return;
  if (p >= 0.999) { canvas.strokeCircle(cx, cy, r, ink, sw); return; }
  const a0 = -Math.PI / 2;
  canvas.stroke(canvas.path().arc(cx, cy, r, a0, a0 + p * 2 * Math.PI, false), ink, { width: sw, cap: 'round' });
}

// The largest text size up to `size` at which `str` fits `w`.
function fit(canvas, str, w, size, weight) {
  const m = canvas.measureText(str, fontOf(size, weight)).width;
  return m > w ? Math.max(6, size * (w / m)) : size;
}

export function draw(canvas, dock) {
  const W = canvas.width, H = canvas.height;
  const l = layout(canvas, dock);
  const ink = textInk(dock);
  const str = timeText();
  const dim = st.running || st.mode === 'date' ? 1 : 0.7;  // stopped: the text a little dimmer
  if (l.style === 'ring') {
    const d = Math.min(l.d, W - 2, H - 2);
    const sw = Math.max(1.5, d * 0.08), r = d / 2 - sw / 2;
    ring(canvas, W / 2, H / 2, r, sw, progress(), ringInk(dock), '#33808080');
    const size = fit(canvas, str, (r - sw) * 1.75, d * 0.24);
    canvas.save();
    canvas.opacity(dim);
    canvas.text(str, W / 2, H / 2, { ...fontOf(size), align: 'center', baseline: 'middle', color: ink });
    canvas.restore();
    return;
  }
  const size = fit(canvas, str, W - 6, dock.standalone ? Math.min(W / 4, H / 2.2) : l.size);
  const barH = l.style === 'bar' ? Math.max(2, Math.round(size * 0.16)) : 0;
  const ty = l.style === 'bar' ? H / 2 - barH : H / 2;
  canvas.save();
  canvas.opacity(dim);
  canvas.text(str, W / 2, ty, { ...fontOf(size), align: 'center', baseline: 'middle', color: ink });
  canvas.restore();
  if (barH) {
    const bw = Math.min(W - 8, Math.max(l.w, size * 2.5)), bx = (W - bw) / 2, by = ty + size * 0.62;
    canvas.fillRoundRect(bx, by, bw, barH, barH / 2, '#33808080');
    const p = progress();
    if (p !== null && p > 0) canvas.fillRoundRect(bx, by, Math.max(barH, bw * Math.min(1, p)), barH, barH / 2, ringInk(dock));
  }
}

// --- input -------------------------------------------------------------------------------------------

export function onClick(e) {
  if (e.closedPopup) return;
  popup.open(POPUP_W, popupHeight());
}

export function onMiddleClick() {
  act('toggle');
}

export function onWheel(e) {
  if (dock.preview || st.mode !== 'countdown' || st.running) return false;
  const step = e.notches > 0 ? MIN : -MIN;
  const ms = Math.min(1440 * MIN, Math.max(MIN, Math.round((st.remaining || st.total) / MIN) * MIN + step));
  st.total = st.remaining = ms;
  save();
  requestRedraw();
  if (popup.isOpen) popup.redraw();
  return true;
}

// --- the popup ---------------------------------------------------------------------------------------

function presets() {
  return String(settings.presets || '').split(/[,;\s]+/).map(Number)
    .filter((n) => Number.isFinite(n) && n >= 1 && n <= 1440).slice(0, 6);
}

function popupHeight() {
  if (st.mode === 'date') return 118;
  return st.mode === 'countdown' && presets().length ? 182 : 146;
}

function button(canvas, dock, label, x, y, w, what, primary) {
  const ink = dock.theme.labelText;
  canvas.fillRoundRect(x, y, w, 28, 6, primary ? dock.theme.accent : '#20FFFFFF');
  canvas.strokeRoundRect(x + 0.5, y + 0.5, w - 1, 27, 6, '#30808080', 1);
  canvas.text(label, x + w / 2, y + 14, { size: 12.5, align: 'center', baseline: 'middle', color: primary ? '#FFFFFFFF' : ink });
  hits.push({ x, y, w, h: 28, what });
}

export function drawPopup(canvas, dock) {
  hits = [];
  const W = canvas.width, P = 16;
  const ink = dock.theme.labelText, dim = dock.theme.dateText;
  const titles = { countdown: 'Timer', pomodoro: 'Pomodoro', stopwatch: 'Stopwatch', date: 'Countdown' };
  let title = settings.label || titles[st.mode] || 'Timer';
  let sub = '';
  if (st.mode === 'pomodoro') sub = `${phaseName()} · round ${st.round} of ${rounds()}`;
  else if (st.mode === 'countdown') sub = `${Math.round(st.total / MIN)} min` + (st.running ? ` · ends at ${localTimeText(st.endAt)}` : remaining() <= 0 ? ' · done' : '');
  else if (st.mode === 'stopwatch') sub = st.running ? 'Running' : elapsed() > 0 ? 'Paused' : 'Ready';
  else sub = Number.isFinite(targetMs()) ? localDateText(targetMs()) : 'Set the date in the widget\'s settings (YYYY-MM-DD HH:MM)';
  canvas.text(title, P, 12, { size: 14, weight: 'semibold', color: ink, maxWidth: W - 2 * P });
  canvas.text(sub, P, 32, { size: 12, color: dim, maxWidth: W - 2 * P });
  canvas.text(timeText(), P, 52, { size: 30, weight: 'light', color: ink, font: settings.font || undefined });
  const p = progress();
  if (p !== null) {
    canvas.fillRoundRect(P, 92, W - 2 * P, 4, 2, '#33808080');
    if (p > 0) canvas.fillRoundRect(P, 92, Math.max(4, (W - 2 * P) * Math.min(1, p)), 4, 2, ringInk(dock));
  }
  if (st.mode === 'date') return;
  const y = 106, gap = 8;
  const n = st.mode === 'pomodoro' ? 3 : 2;
  const bw = (W - 2 * P - gap * (n - 1)) / n;
  button(canvas, dock, st.running ? 'Pause' : remaining() <= 0 && st.mode === 'countdown' ? 'Start again' : 'Start', P, y, bw, 'toggle', true);
  button(canvas, dock, 'Reset', P + bw + gap, y, bw, 'reset');
  if (n === 3) button(canvas, dock, 'Skip', P + 2 * (bw + gap), y, bw, 'skip');
  const list = st.mode === 'countdown' ? presets() : [];
  if (list.length) {
    const cw = (W - 2 * P - gap * (list.length - 1)) / list.length;
    list.forEach((m, i) => {
      const x = P + i * (cw + gap);
      canvas.fillRoundRect(x, 144, cw, 24, 12, '#18FFFFFF');
      canvas.strokeRoundRect(x + 0.5, 144.5, cw - 1, 23, 12, '#30808080', 1);
      canvas.text(m + ' min', x + cw / 2, 156, { size: 12, align: 'center', baseline: 'middle', color: ink });
      hits.push({ x, y: 144, w: cw, h: 24, what: 'preset', minutes: m });
    });
  }
}

export function onPopupClick(e) {
  if (e.button !== 'left') return;
  const h = hits.find((t) => e.x >= t.x && e.x < t.x + t.w && e.y >= t.y && e.y < t.y + t.h);
  if (!h) return;
  if (h.what === 'preset') {
    if (dock.preview) return;
    st.running = false;
    st.total = st.remaining = h.minutes * MIN;
    start();
    save();
    arm();
    sync();
    popup.redraw();
    return;
  }
  act(h.what);
}
