// Agenda (round 4 phase 13b): the next event of the dock's calendar feeds (the `calendar`
// provider) in the slot, the next events in a popup.
//
// Cost: nothing polls. One timer (setTimeout) waits for the next moment the slot's text changes:
// the shown event's start ("Now") or end (the next one moves up), each minute of the hour before
// it ("In 25 min"), the next event entering the look-ahead, local midnight ("Tomorrow" becomes
// "Today"); calendar.onChange redraws when the feeds change. Each draw replaces the timer.

const POPUP_W = 340;
const ROW_H = 44;
let timer = null;
let listening = null;
let warned = false;
let hovered = false;
let hits = [];
let lastShown = '';

function pad2(n) { return (n < 10 ? '0' : '') + n; }
// Local days as "YYYY-MM-DD" (Date.UTC arithmetic only: QuickJS's local Date applies today's DST
// offset to every date on Windows).
function todayYmd() { const s = system.now(); return s.year + '-' + pad2(s.month) + '-' + pad2(s.day); }
function addDays(ymd, n) {
  const [y, m, d] = ymd.split('-').map(Number);
  const t = new Date(Date.UTC(y, m - 1, d + n));
  return t.getUTCFullYear() + '-' + pad2(t.getUTCMonth() + 1) + '-' + pad2(t.getUTCDate());
}
function weekday(ymd) {
  const [y, m, d] = ymd.split('-').map(Number);
  return system.shortDayNames[new Date(Date.UTC(y, m - 1, d)).getUTCDay()];
}
function dayName(ymd) {
  const today = todayYmd();
  if (ymd === today) return 'Today';
  if (ymd === addDays(today, 1)) return 'Tomorrow';
  return weekday(ymd);
}

function feedFilter() {
  return String(settings.feeds || '').split(',').map((s) => s.trim().toLowerCase()).filter(Boolean);
}

// The next events (ongoing first), filtered by feed; `all`: ignore the look-ahead.
function upcoming(n, all) {
  let list;
  try {
    list = calendar.next(100);
  } catch (e) {
    if (!warned) console.warn('calendar: ' + e.message);
    warned = true;
    return [];
  }
  const feeds = feedFilter();
  const limit = Date.now() + (settings.lookaheadDays | 0) * 86400000;
  return list.filter((e) => (!feeds.length || feeds.includes(String(e.feed).toLowerCase())) && (all || e.start < limit)).slice(0, n);
}

// The slot's event: the next timed one (ongoing or ahead), else an all-day one.
function shownEvent(list) {
  return list.find((e) => !e.allDay) || list[0] || null;
}

function whenText(e, now) {
  if (e.allDay) return dayName(e.startDay);
  if (e.start <= now && now < e.end) return 'Now';
  const left = e.start - now;
  if (left <= 3600000) return 'In ' + Math.max(1, Math.ceil(left / 60000)) + ' min';
  return (e.startDay === todayYmd() ? '' : dayName(e.startDay) + ' ') + e.startTime;
}

// ms until the text of the slot changes.
function nextBoundary(e, now) {
  const s = system.now();
  let t = ((24 - s.hour) * 3600 - s.minute * 60 - s.second) * 1000 - s.millisecond;  // local midnight
  if (e && !e.allDay) {
    if (now < e.start) {
      const left = e.start - now;
      t = Math.min(t, left > 3600000 ? left - 3600000 : ((left - 1) % 60000) + 1);
    } else if (now < e.end) {
      t = Math.min(t, e.end - now);
    }
  }
  if (!e) {  // the first event beyond the look-ahead comes into it
    const far = upcoming(1, true)[0];
    if (far) t = Math.min(t, far.start - (settings.lookaheadDays | 0) * 86400000 - now);
  }
  return Math.max(50, t);
}

function schedule(ms) {
  if (timer !== null) clearTimeout(timer);
  timer = setTimeout(() => {
    timer = null;
    requestRedraw();
    if (popup.isOpen) popup.redraw();
  }, Math.min(ms + 30, 86400000));
}

function follow() {
  if (listening) return;
  try {
    listening = calendar.onChange(() => {
      requestRedraw();
      if (popup.isOpen) popup.open(POPUP_W, popupHeight());
    });
  } catch (e) {
    if (!warned) console.warn('calendar: ' + e.message);
    warned = true;
  }
}

// Text that fits `w` DIPs (an ellipsis cut).
function fit(canvas, text, w, style) {
  if (canvas.measureText(text, style).width <= w) return text;
  let lo = 0, hi = text.length;
  while (lo < hi) {
    const mid = (lo + hi + 1) >> 1;
    if (canvas.measureText(text.slice(0, mid) + '…', style).width <= w) lo = mid; else hi = mid - 1;
  }
  return text.slice(0, lo) + '…';
}

export function measure(canvas, dock) {
  const cross = Math.round(dock.iconSize);
  if (dock.axis === 'vertical') return { main: 56, cross };
  return { main: settings.width | 0 || 160, cross };
}

export function draw(canvas, dock) {
  follow();
  const now = Date.now();
  const list = upcoming(20, false);
  const e = shownEvent(list);
  const w = canvas.width, h = canvas.height;
  if (hovered) canvas.fillRoundRect(1, 1, w - 2, h - 2, Math.min(w, h) * 0.2, '#22FFFFFF');
  const when = e ? whenText(e, now) : 'No events';
  const title = e ? (e.title || '(No title)') : '';
  if (dock.axis === 'vertical') {
    if (e) canvas.fillRoundRect(w / 2 - 10, 6, 20, 3, 1.5, e.color);
    canvas.text(when, w / 2, h / 2 + 2, { size: 11, align: 'center', baseline: 'middle', maxWidth: w - 4, color: e && when === 'Now' ? dock.theme.accent : dock.theme.text });
  } else {
    const x = e ? 12 : 6;
    if (e) canvas.fillRoundRect(4, 7, 3, h - 14, 1.5, e.color);
    const small = { size: 11, color: when === 'Now' ? dock.theme.accent : dock.theme.dateText };
    const big = { size: 13, weight: 'semibold', color: dock.theme.text };
    if (e) {
      canvas.text(fit(canvas, when, w - x - 4, small), x, h / 2 - 1, { ...small, baseline: 'bottom' });
      canvas.text(fit(canvas, title, w - x - 4, big), x, h / 2 + 1, { ...big, baseline: 'top' });
    } else {
      canvas.text(when, x, h / 2, { ...small, baseline: 'middle' });
    }
  }
  schedule(nextBoundary(e, now));
  // Test hook (an item setting that the schema does not name): what the slot shows, in storage.
  if (settings.debugStore) {
    const shown = when + ' | ' + title;
    if (shown !== lastShown) {
      lastShown = shown;
      const log = storage.get('shown', []);
      log.push(shown);
      storage.set('shown', log.slice(-20));
    }
  }
}

function popupHeight() {
  const n = Math.max(1, upcoming(settings.count | 0 || 6, false).length);
  return 12 + 28 + n * ROW_H + 8 + 30 + 12;
}

export function drawPopup(canvas, dock) {
  const W = canvas.width;
  const now = Date.now();
  const list = upcoming(settings.count | 0 || 6, false);
  hits = [];
  canvas.text('Agenda', 16, 12 + 14, { size: 14, weight: 'semibold', baseline: 'middle', color: dock.theme.text });
  let y = 12 + 28;
  if (!list.length) {
    canvas.text('No events in the next ' + (settings.lookaheadDays | 0) + ' days', W / 2, y + ROW_H / 2, { size: 12, align: 'center', baseline: 'middle', color: dock.theme.dateText });
    y += ROW_H;
  }
  for (const e of list) {
    canvas.fillRoundRect(14, y + 7, 4, ROW_H - 14, 2, e.color);
    const title = { size: 13, weight: 'semibold', color: dock.theme.text };
    const sub = { size: 11, color: dock.theme.dateText };
    let when = e.allDay ? dayName(e.startDay) + ', all day'
      : (e.start <= now && now < e.end ? 'Now, until ' + e.endTime : dayName(e.startDay) + ' ' + e.startTime + ' – ' + e.endTime);
    if (settings.showLocation && e.location) when += ' · ' + e.location;
    canvas.text(fit(canvas, e.title || '(No title)', W - 44, title), 28, y + 20, { ...title, baseline: 'bottom' });
    canvas.text(fit(canvas, when, W - 44, sub), 28, y + 23, { ...sub, baseline: 'top' });
    hits.push({ x: 8, y, w: W - 16, h: ROW_H, ev: e });
    y += ROW_H;
  }
  y += 8;
  canvas.fillRoundRect(16, y, W - 32, 30, 6, '#1AFFFFFF');
  canvas.text('Open the month calendar', W / 2, y + 15, { size: 12, align: 'center', baseline: 'middle', color: dock.theme.text });
  hits.push({ x: 16, y, w: W - 32, h: 30, ev: null });
}

export function onPopupClick(e) {
  if (e.button !== 'left') return;
  const hit = hits.find((r) => e.x >= r.x && e.x < r.x + r.w && e.y >= r.y && e.y < r.y + r.h);
  if (!hit) return;
  if (hit.ev && /^https?:\/\//i.test(hit.ev.url || '')) {
    popup.close();
    shell.open(hit.ev.url);
    return;
  }
  calendar.openPopup(hit.ev ? hit.ev.startDay : undefined);  // the month calendar replaces this popup
}

export function onClick(e) {
  switch (settings.clickOpens) {
    case 'calendar':
      calendar.openPopup();
      break;
    case 'target':
      if (settings.target) shell.open(String(settings.target));
      break;
    default:
      if (!e.closedPopup) popup.open(POPUP_W, popupHeight());
  }
}

export function onHover(e) {
  hovered = e.inside;
  requestRedraw();
}

export function onSettingsChanged() {
  warned = false;
  lastShown = '';
  requestRedraw();
}
