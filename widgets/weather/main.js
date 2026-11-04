// Weather (script): the dock's weather element and its forecast popup rewritten on the widget
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
//
// Cost: nothing polls. The widget redraws when the provider reports a change (a fetch started,
// finished or failed); while the popup is open one timeout per minute updates "N min ago".

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

// --- the slot ------------------------------------------------------------------------------------

// As C++ std::lround (halves away from zero; JS Math.round rounds -0.5 up).
const lround = (x) => (x < 0 ? -Math.round(-x) : Math.round(x));
const tempText = (t) => (t === null || t === undefined ? '--°' : `${lround(t) || 0}°`);
const fresh = () => wx.state === 'fresh';

// settings.dockForecast (round 7 phase 7C): "none", "today" (today's high / low under the
// temperature, as showHigh + showLow) or "days" (dockDays compact day columns after the current
// conditions; rows under them on a left / right dock).
const forecastMode = () => settings.dockForecast || 'none';

function slotLines(dock) {
  // The temperature, then the optional second line (its label, today's high / low, the place).
  if (!wx.configured || settings.compact) return { main: '', extra: '' };
  const main = wx.current ? tempText(wx.current.temp) : '--°';
  const today = wx.daily && wx.daily[0];
  const todayMode = forecastMode() === 'today';
  const parts = [];
  if (settings.label) parts.push(settings.label);
  if ((settings.showHigh || todayMode) && today) parts.push('↑' + tempText(today.max));
  if ((settings.showLow || todayMode) && today) parts.push('↓' + tempText(today.min));
  if (settings.showPlace && wx.place) parts.push(wx.place);
  return { main, extra: parts.join(dock.axis === 'vertical' ? '\n' : '  ') };
}

const mainStyle = (dock, k = 1) => ({ size: (dock.clockDate ? FONT - 2 : FONT) * k, weight: 'semibold' });
const extraStyle = { size: 10 };

// The day columns ("days"): a column per day (the weekday on top, the glyph, the high over the
// low); on a left / right dock a row per day (the weekday's initial, the glyph, "high/low").
const DAY_GLYPH = 16, ROW_GLYPH = 14;  // DIPs
const DAY_FONT = 9, DAY_LINE = 10;      // the columns' text size and line pitch
const DAY_GAP = 6;                      // between columns
const FORECAST_GAP = 8;                 // the current conditions -> the columns (half before rows)
const DAY_ROW_H = 16;

const dayDeg = (t) => (t === null || t === undefined ? '–' : (lround(t) || 0) + '°');
const dayNum = (t) => (t === null || t === undefined ? '–' : String(lround(t) || 0));

function forecast(canvas, dock, k, vertical) {
  if (forecastMode() !== 'days' || !wx.configured || !wx.daily || !wx.daily.length) return null;
  const days = wx.daily.slice(0, Math.max(2, Math.min(5, settings.dockDays || 3)));
  const label = { size: DAY_FONT * k, weight: 'semibold' };
  const small = { size: DAY_FONT * k };
  const width = (s, st) => canvas.measureText(s, st).width;
  if (!vertical) {
    const cols = days.map((d, i) => {
      const name = i === 0 ? 'Today' : system.shortDayNames[d.weekday] || '';
      const hi = dayDeg(d.max), lo = dayDeg(d.min);
      const w = Math.ceil(Math.max(width(name, label), width(hi, small), width(lo, small), DAY_GLYPH * k));
      return { d, name, hi, lo, w };
    });
    const w = cols.reduce((s, c) => s + c.w, 0) + (cols.length - 1) * Math.round(DAY_GAP * k);
    return { vertical, cols, w, h: Math.ceil((3 * DAY_LINE + DAY_GLYPH) * k), label, small };
  }
  const rows = days.map((d) => ({ d, name: (system.shortDayNames[d.weekday] || '?').charAt(0), hi: dayNum(d.max), lo: '/' + dayNum(d.min) }));
  const nameW = Math.ceil(Math.max(...rows.map((r) => width(r.name, label))));
  const tempW = Math.ceil(Math.max(...rows.map((r) => width(r.hi, small) + width(r.lo, small))));
  const gap = Math.round(2 * k);
  return { vertical, rows, nameW, tempW, gap, w: nameW + gap + ROW_GLYPH * k + gap + tempW, h: Math.ceil(rows.length * DAY_ROW_H * k), label, small };
}

// The slot's parts at scale k (1 on a dock).
function layout(canvas, dock, k = 1) {
  const glyph = (dock.clockDate ? GLYPH - 2 : GLYPH) * k;
  const t = slotLines(dock);
  const a = t.main ? canvas.measureText(t.main, mainStyle(dock, k)) : { width: 0, height: 0 };
  const b = t.extra ? canvas.measureText(t.extra, { size: extraStyle.size * k }) : { width: 0, height: 0 };
  const textW = Math.ceil(Math.max(a.width, b.width));
  const textH = Math.ceil(a.height) + Math.ceil(b.height);
  const gap = textW > 0 ? GAP * k : 0;
  const vertical = dock.axis === 'vertical';
  const cw = vertical ? Math.max(glyph, textW) : glyph + gap + textW;  // the current conditions
  const ch = vertical ? glyph + gap + textH : Math.max(glyph, textH);
  const f = forecast(canvas, dock, k, vertical);
  let w = cw, h = ch;
  if (f && vertical) {
    w = Math.max(cw, f.w);
    h = ch + Math.round((FORECAST_GAP / 2) * k) + f.h;
  } else if (f) {
    w = cw + Math.round(FORECAST_GAP * k) + f.w;
    h = Math.max(ch, f.h);
  }
  return { t, a, b, glyph, gap, cw, ch, f, w, h, vertical, k };
}

function drawForecast(canvas, dock, l, x, y, dim) {
  const f = l.f, k = l.k;
  const text = dock.theme.text, low = dock.theme.dateText;
  canvas.save();
  canvas.opacity(dim);
  if (!f.vertical) {
    const g = DAY_GLYPH * k, line = DAY_LINE * k;
    for (const c of f.cols) {
      const cx = x + c.w / 2;
      canvas.text(c.name, cx, y + line / 2, { ...f.label, align: 'center', baseline: 'middle', color: text });
      if (c.d.code !== null && c.d.code >= 0) canvas.image(weather.icon(c.d.code, true), Math.round(cx - g / 2), Math.round(y + line), g, g);
      canvas.text(c.hi, cx, y + line + g + line / 2, { ...f.small, align: 'center', baseline: 'middle', color: text });
      canvas.text(c.lo, cx, y + 2 * line + g + line / 2, { ...f.small, align: 'center', baseline: 'middle', color: low });
      x += c.w + Math.round(DAY_GAP * k);
    }
  } else {
    const g = ROW_GLYPH * k, rh = DAY_ROW_H * k;
    f.rows.forEach((r, i) => {
      const cy = y + i * rh + rh / 2;
      canvas.text(r.name, x + f.nameW / 2, cy, { ...f.label, align: 'center', baseline: 'middle', color: text });
      const gx = x + f.nameW + f.gap;
      if (r.d.code !== null && r.d.code >= 0) canvas.image(weather.icon(r.d.code, true), Math.round(gx), Math.round(cy - g / 2), g, g);
      const tx = gx + g + f.gap;
      canvas.text(r.hi, tx, cy, { ...f.small, baseline: 'middle', color: text });
      canvas.text(r.lo, tx + canvas.measureText(r.hi, f.small).width, cy, { ...f.small, baseline: 'middle', color: low });
    });
  }
  canvas.restore();
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
  const code = !wx.configured ? 'setLocation' : wx.current ? wx.current.code : 3;  // no data yet: overcast
  const isDay = !wx.current || wx.current.isDay;
  const icon = weather.icon(code, isDay);
  const color = dock.theme.text;
  // Test hook (an item setting, not in the schema): what the slot shows, into the storage.
  if (settings.debugStore) {
    storage.set('slot', { state: wx.state, temp: wx.current ? wx.current.temp : null, code, dim, text: l.t.main, w: l.w, h: l.h,
      days: l.f ? (l.f.cols || l.f.rows).length : 0 });
  }
  if (l.vertical) {
    const left = Math.round((canvas.width - l.glyph) / 2);
    canvas.image(icon, left, 0, l.glyph, l.glyph, dim);
    let y = l.glyph + GAP;
    if (l.t.main) {
      canvas.save();
      canvas.opacity(dim);
      canvas.text(l.t.main, Math.round((canvas.width - l.a.width) / 2), y, { ...mainStyle(dock), color });
      canvas.restore();
      y += Math.ceil(l.a.height);
    }
    if (l.t.extra) canvas.text(l.t.extra, canvas.width / 2, y, { ...extraStyle, align: 'center', color: dock.theme.dateText });
    if (l.f) drawForecast(canvas, dock, l, Math.round((canvas.width - l.f.w) / 2), l.ch + Math.round(FORECAST_GAP / 2), dim);
    return;
  }
  const left = dock.standalone ? Math.round((canvas.width - l.w) / 2) : 0;  // standalone: centred
  const top = Math.round((canvas.height - l.glyph) / 2);
  canvas.image(icon, left, top, l.glyph, l.glyph, dim);
  if (l.f) drawForecast(canvas, dock, l, left + l.cw + Math.round(FORECAST_GAP * l.k), Math.round((canvas.height - l.f.h) / 2), dim);
  if (!l.t.main) return;
  const x = left + l.glyph + GAP * l.k;
  const blockH = l.a.height + (l.t.extra ? l.b.height : 0);
  const y = Math.round((canvas.height - blockH) / 2);
  canvas.save();
  canvas.opacity(dim);
  canvas.text(l.t.main, x, y, { ...mainStyle(dock, l.k), color });
  canvas.restore();
  if (l.t.extra) canvas.text(l.t.extra, x, y + l.a.height, { size: extraStyle.size * l.k, color: dock.theme.dateText });
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
  canvas.text('', gear.x + gear.w / 2, gear.y + gear.h / 2, { font: 'Segoe Fluent Icons', size: 14, align: 'center', baseline: 'middle', color: secondary });
  hits.push(gear);
  if (wx.configured) {
    canvas.fillRoundRect(refresh.x, refresh.y, refresh.w, refresh.h, 6, fade(text, 0.08));
    const g = 16;
    canvas.image(weather.icon('refresh', true, { ink: wx.busy ? fade(text, 0.4) : secondary }),
      refresh.x + (BUTTON - g) / 2, refresh.y + (BUTTON - g) / 2, g, g);
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
  canvas.image(weather.icon(c.code, c.isDay), PAD, MAIN_TOP + 2, BIG_GLYPH, BIG_GLYPH, stale ? STALE : 1);
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
    if (h.code >= 0) canvas.image(weather.icon(h.code, h.isDay), cx - 8, top + 17, 16, 16);
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
    if (d.code >= 0) canvas.image(weather.icon(d.code, true), PAD + 74, cy - 9, 18, 18);
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
