// World clock (round 28 phase 28B): the time in up to six places. The tile shows one or two of them
// (a name and the time each); a click opens the whole list with each place's day ("Tomorrow") and
// its difference from here.
//
// Time zones are Windows' own (the `timezone` setting type: a key such as "Tokyo Standard Time"),
// converted by Windows with each zone's daylight-saving rules (system.localTime / formatTime). No
// time-zone data ships with the widget, and QuickJS's own Date is not used for other zones.
//
// Cost: the manifest's refreshMs (60000) ticks on the minute through the host's shared scheduler,
// only while the widget can be seen; nothing else runs.

const MAX = 6;
let hits = [];

const custom = (c) => (c && !/^#00[0-9a-f]{6}$/i.test(c) ? c : null);

// "Tokyo Standard Time" -> "Tokyo"; "W. Europe Standard Time" -> "W. Europe".
function nameOfZone(z) {
  return z.replace(/ (Standard|Daylight) Time$/, '').replace(/ Time$/, '');
}

function places() {
  const out = [];
  for (let i = 1; i <= MAX; i++) {
    const zone = settings['zone' + i];
    if (typeof zone !== 'string' || !zone) continue;
    const label = (settings['label' + i] || '').trim() || nameOfZone(zone);
    out.push({ zone, label });
  }
  return out;
}

// The time there in the user's own format, or 24-hour / 12-hour when chosen.
function timeAt(zone) {
  const fmt = settings.hours || 'auto';
  if (fmt === 'auto') return system.formatTime(false, zone);
  const t = system.localTime(zone);
  const mm = String(t.minute).padStart(2, '0');
  if (fmt === '24') return `${String(t.hour).padStart(2, '0')}:${mm}`;
  return `${(t.hour % 12) || 12}:${mm} ${t.hour < 12 ? 'AM' : 'PM'}`;
}

// Days from here to there (-1 yesterday, 0 today, 1 tomorrow) and the difference in minutes.
function offsetOf(zone) {
  const here = system.now(), there = system.localTime(zone);
  const dh = Date.UTC(here.year, here.month - 1, here.day), dt = Date.UTC(there.year, there.month - 1, there.day);
  return { days: Math.round((dt - dh) / 86400000), minutes: there.utcOffset - here.utcOffset, there };
}

function dayText(days) {
  if (days === 0) return 'Today';
  if (days === 1) return 'Tomorrow';
  if (days === -1) return 'Yesterday';
  return (days > 0 ? '+' : '') + days + ' days';
}

function diffText(minutes) {
  if (minutes === 0) return 'same time';
  const sign = minutes > 0 ? '+' : '−';
  const m = Math.abs(minutes), h = Math.floor(m / 60), r = m % 60;
  return `${sign}${h}${r ? ':' + String(r).padStart(2, '0') : ''} h`;
}

export function onSettingsChanged() {
  requestRedraw();
  if (popup.isOpen) popup.redraw();
}

export function tick() {
  if (popup.isOpen) popup.redraw();
}

// --- the tile ----------------------------------------------------------------------------------------

function shown() {
  const n = Math.max(1, Math.min(2, settings.tileZones || 1));
  return places().slice(0, n);
}

function styles(dock, k) {
  const icon = dock.standalone ? 48 : dock.iconSize;
  const s = (settings.scale || 1) * k;
  const font = settings.font || undefined;
  return {
    time: { size: Math.max(9, icon * 0.27 * s), weight: 'semibold', font },
    label: { size: Math.max(8, icon * 0.19 * s), font },
  };
}

function layout(canvas, dock, k = 1) {
  const list = shown();
  const st = styles(dock, k);
  const rows = list.map((p) => {
    const time = timeAt(p.zone);
    return {
      p, time,
      tw: Math.ceil(canvas.measureText(time, st.time).width),
      lw: Math.ceil(canvas.measureText(p.label, st.label).width),
    };
  });
  const two = rows.length === 2;
  // One place: its name over its time. Two: a row each, the name left of the time.
  const w = rows.reduce((m, r) => Math.max(m, two ? r.lw + 6 + r.tw : Math.max(r.lw, r.tw)), 0);
  return { rows, st, two, w };
}

export function measure(canvas, dock) {
  const icon = Math.round(dock.iconSize);
  if (dock.axis === 'vertical' && !dock.standalone) return { main: Math.round(icon * (shown().length === 2 ? 1.2 : 0.9)), cross: icon };
  const l = layout(canvas, dock);
  return { main: Math.max(icon, l.w + 14), cross: icon };
}

export function draw(canvas, dock) {
  const W = canvas.width, H = canvas.height;
  const ink = custom(settings.textColor) || dock.theme.text;
  const dim = custom(settings.labelColor) || dock.theme.dateText;
  let l = layout(canvas, dock);
  if (!l.rows.length) {
    canvas.text('No place', W / 2, H / 2, { size: 11, align: 'center', baseline: 'middle', color: dim });
    return;
  }
  const vertical = dock.axis === 'vertical' && !dock.standalone;
  const fitW = vertical ? W - 4 : W - 8;
  const k = l.w > fitW ? fitW / l.w : dock.standalone ? Math.min(3, fitW / Math.max(1, l.w), H / 60) : 1;
  if (k !== 1) l = layout(canvas, dock, k);
  if (l.two && !vertical) {
    const lineH = Math.max(l.st.time.size, l.st.label.size) * 1.15;
    const top = H / 2 - lineH;
    l.rows.forEach((r, i) => {
      const y = top + lineH * (i + 0.5);
      const x0 = (W - l.w) / 2;
      canvas.text(r.p.label, x0, y, { ...l.st.label, baseline: 'middle', color: dim });
      canvas.text(r.time, x0 + l.w, y, { ...l.st.time, align: 'right', baseline: 'middle', color: ink });
    });
    return;
  }
  // One place (or a side dock): the name above the time, stacked for each.
  const blockH = l.st.label.size * 1.15 + l.st.time.size * 1.1;
  const total = blockH * l.rows.length;
  let y = (H - total) / 2;
  for (const r of l.rows) {
    canvas.text(r.p.label, W / 2, y, { ...l.st.label, align: 'center', baseline: 'top', color: dim, maxWidth: 0 });
    canvas.text(r.time, W / 2, y + l.st.label.size * 1.15, { ...l.st.time, align: 'center', baseline: 'top', color: ink });
    y += blockH;
  }
}

// --- the popup ---------------------------------------------------------------------------------------

const ROW_H = 44, POPUP_W = 300;

export function onClick(e) {
  if (e.closedPopup) return;
  popup.open(POPUP_W, 44 + Math.max(1, places().length + 1) * ROW_H);
}

export function drawPopup(canvas, dock) {
  hits = [];
  const W = canvas.width, P = 16;
  const ink = dock.theme.labelText, dim = dock.theme.dateText;
  canvas.text('World clock', P, 12, { size: 14, weight: 'semibold', color: ink });
  const rows = [{ zone: '', label: 'Here' }].concat(places());
  rows.forEach((p, i) => {
    const y = 40 + i * ROW_H;
    if (i) canvas.line(P, y - 0.5, W - P, y - 0.5, '#26808080', 1);
    const o = p.zone ? offsetOf(p.zone) : { days: 0, minutes: 0 };
    canvas.text(p.label, P, y + 8, { size: 13, color: ink, maxWidth: W - 2 * P - 110 });
    canvas.text(p.zone ? `${dayText(o.days)} · ${diffText(o.minutes)}` : dayText(0), P, y + 26, { size: 11.5, color: dim, maxWidth: W - 2 * P - 110 });
    canvas.text(timeAt(p.zone), W - P, y + ROW_H / 2, { size: 20, weight: 'light', align: 'right', baseline: 'middle', color: ink });
  });
}
