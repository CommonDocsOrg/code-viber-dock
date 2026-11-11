// Stocks (round 16 phase 16C): quotes for a few ticker symbols from Finnhub's REST API
// (https://finnhub.io/docs/api/quote: the current price, the change and percent change since the
// previous close, the day's high / low / open and the previous close; a free key allows 60 requests
// a minute). One symbol at a time (settings.view "single": the wheel steps through the symbols, an
// optional timer cycles them) or every symbol (settings.view "all": a row of cells on a top /
// bottom dock, a stacked list on a left / right dock, a table in a standalone window). A click
// opens a popup with the day's figures of every symbol, "Updated N min ago", a refresh button and
// the Finnhub credit; a click on a row opens the symbol's page on Yahoo / Google Finance / Finnhub.
//
// Data sources are a table (PROVIDERS): each knows its host, how to fetch one symbol and how to
// turn an answer into the one quote shape the rest of the widget draws, so another API is one more
// entry plus a choice in widget.json's "provider" setting.
//
// Cost: one wake-up per refresh (timer.setRefresh, default 5 minutes), one request per symbol, at
// most 4 in flight (the host's limit). The last quotes and a day of price points per symbol live in
// the widget's storage ("quote:AAPL", "hist:AAPL"), shared by every Stocks item, so a restart draws
// at once (dimmed until fresh) and two items with the same symbol show the same numbers. Nothing
// ticks while the symbol does not cycle; the cycle timer stops while the dock is hidden.

const MAX_SYMBOLS = 20;
const HIST_MAX = 200;                     // price points kept per symbol
const HIST_SPAN = 24 * 3600 * 1000;       // ... and no older than a day
const STALE = 0.6;                        // alpha of quotes older than two refreshes (or cached)
const CREDIT = 'Data by Finnhub';
const CREDIT_URL = 'https://finnhub.io/';

// --- data sources --------------------------------------------------------------------------------

// A provider: { name, url(symbol) -> the request, headers(key), parse(json, symbol) -> quote | null,
// error(status) -> a message }. A quote: { price, change, pct, high, low, open, prev, t (ms since
// 1970 of the last trade, 0 unknown) }.
const PROVIDERS = {
  finnhub: {
    name: 'Finnhub',
    url: (symbol) => 'https://finnhub.io/api/v1/quote?symbol=' + encodeURIComponent(symbol),
    headers: (key) => ({ 'X-Finnhub-Token': key }),  // the key in a header, not in the URL (the Console logs URLs)
    parse: (j) => {
      if (!j || typeof j.c !== 'number') return null;
      if (j.c === 0 && !j.t) return null;      // Finnhub answers all zeros for a symbol it does not know
      return { price: j.c, change: num(j.d), pct: num(j.dp), high: num(j.h), low: num(j.l), open: num(j.o), prev: num(j.pc), t: (j.t || 0) * 1000 };
    },
    error: (status) => (status === 401 || status === 403 ? 'The API key was refused' : status === 429 ? 'Rate limited (60 requests a minute on the free plan)' : 'HTTP ' + status),
  },
};

const num = (v) => (typeof v === 'number' && isFinite(v) ? v : null);
const provider = () => PROVIDERS[settings.provider] || PROVIDERS.finnhub;

// --- state ---------------------------------------------------------------------------------------

let symbols = [];          // the parsed setting, upper-case, unique
let quotes = {};           // symbol -> quote + { at: ms when fetched by any item }
let hist = {};             // symbol -> [[t, price], ...] (oldest first)
let freshRun = {};         // symbol -> true once this run fetched it
let error = '';            // the last failure (the popup shows it; the slot dims)
let busy = false;
let current = 0;           // the symbol shown in the single view
let cycleTimer = 0;
let signature = '';        // provider + key + symbols: a change refetches
let popupShown = false, shownSize = null, minuteTimer = 0;
let hits = [];

function parseSymbols(text) {
  const out = [];
  for (const s of String(text || '').split(/[,\s]+/)) {
    const sym = s.trim().toUpperCase();
    if (sym && !out.includes(sym)) out.push(sym);
    if (out.length >= MAX_SYMBOLS) break;
  }
  return out;
}

function loadStored() {
  for (const s of symbols) {
    if (!quotes[s]) {
      const q = storage.get('quote:' + s, null);
      if (q && typeof q.price === 'number') quotes[s] = q;
    }
    if (!hist[s]) hist[s] = storage.get('hist:' + s, []) || [];
  }
}

// Another item fetched a symbol: pick its answer up instead of showing older numbers.
storage.onChange((e) => {
  const m = /^(quote|hist):(.+)$/.exec(e.key);
  if (!m || !symbols.includes(m[2])) return;
  if (m[1] === 'quote') quotes[m[2]] = storage.get(e.key, quotes[m[2]]);
  else hist[m[2]] = storage.get(e.key, hist[m[2]]) || [];
  requestRedraw();
  if (popupShown) popup.redraw();
});

// --- fetching ------------------------------------------------------------------------------------

const hasKey = () => !!(settings.apiKey && String(settings.apiKey).trim());
const refreshMs = () => Math.max(1, Math.min(120, settings.refreshMinutes || 5)) * 60000;

async function fetchOne(symbol) {
  const p = provider();
  const r = await http.get(p.url(symbol), { type: 'json', timeout: 10000, headers: p.headers(String(settings.apiKey).trim()) });
  if (!r.ok) throw new Error(p.error(r.status));
  const q = p.parse(r.json, symbol);
  if (!q) throw new Error(symbol + ': no quote (unknown symbol?)');
  q.at = Date.now();
  quotes[symbol] = q;
  freshRun[symbol] = true;
  storage.set('quote:' + symbol, q);
  // One price point per fetch, pruned to the last day: the sparkline's data.
  const h = (hist[symbol] || []).filter((pt) => q.at - pt[0] < HIST_SPAN);
  const last = h[h.length - 1];
  if (!last || last[1] !== q.price || q.at - last[0] > 60000) h.push([q.at, q.price]);
  while (h.length > HIST_MAX) h.shift();
  hist[symbol] = h;
  storage.set('hist:' + symbol, h);
}

async function fetchAll() {
  if (busy || dock.preview || !hasKey() || !symbols.length) return;
  busy = true;
  error = '';
  if (popupShown) popup.redraw();
  if (updatedMode() !== 'none') requestRedraw();  // "Fetching…"
  try {
    // Four at a time (the host allows 4 requests in flight per widget); a refused key or a rate
    // limit stops the round, so one bad key costs one request, not one per symbol.
    for (let i = 0; i < symbols.length; i += 4) {
      const results = await Promise.allSettled(symbols.slice(i, i + 4).map(fetchOne));
      let fatal = '';
      for (const res of results) {
        if (res.status !== 'rejected') continue;
        const msg = String(res.reason && res.reason.message || res.reason);
        if (!error) error = msg;
        if (/refused|Rate limited/.test(msg)) fatal = msg;
      }
      if (fatal) { error = fatal; break; }
    }
  } finally {
    busy = false;
  }
  if (error) console.warn('stocks:', error);
  requestRedraw();
  if (popupShown) showPopup();
}

function schedule() {
  timer.setRefresh(refreshMs());
}

function applySettings() {
  symbols = parseSymbols(settings.symbols);
  if (current >= symbols.length) current = 0;
  loadStored();
  schedule();
  cycle();
  const sig = [settings.provider, settings.apiKey, symbols.join(',')].join('|');
  if (sig !== signature) {
    signature = sig;
    freshRun = {};
    fetchAll();
  }
}

export function tick() {
  fetchAll();
  return false;  // the answers redraw
}

export function onSettingsChanged() {
  applySettings();
  requestRedraw();
  if (popupShown) showPopup();
}

// --- the single view's cycling -------------------------------------------------------------------

const single = () => settings.view === 'single';

function cycle() {
  if (cycleTimer) clearInterval(cycleTimer);
  cycleTimer = 0;
  const secs = Math.max(0, Math.min(300, settings.cycleSeconds || 0));
  if (!single() || secs <= 0 || symbols.length < 2 || !dock.visible || dock.preview) return;
  cycleTimer = setInterval(() => { step(1); }, secs * 1000);
}

function step(n) {
  if (!symbols.length) return;
  current = ((current + n) % symbols.length + symbols.length) % symbols.length;
  requestRedraw();
}

export function onVisibilityChange(e) {
  cycle();
  if (e.visible) requestRedraw();  // the "Updated" line catches up (draw schedules its timeout again)
  else scheduleUpdatedTick();      // hidden: no timeout
}

export function onWheel(e) {
  if (!single() || symbols.length < 2) return false;
  step(e.notches < 0 ? 1 : -1);
  cycle();  // a manual step restarts the interval
  return true;
}

// --- sample data (previews) ----------------------------------------------------------------------

const SAMPLE = {
  AAPL: { price: 189.84, change: 2.31, pct: 1.23, high: 190.42, low: 187.1, open: 187.55, prev: 187.53 },
  MSFT: { price: 415.2, change: -1.85, pct: -0.44, high: 418.9, low: 413.6, open: 417.0, prev: 417.05 },
  NVDA: { price: 131.6, change: 3.9, pct: 3.05, high: 132.4, low: 127.2, open: 127.9, prev: 127.7 },
};

function sampleQuote(symbol, i) {
  const q = SAMPLE[symbol] || { price: 100 + i * 7.5, change: i % 2 ? -0.8 : 1.2, pct: i % 2 ? -0.8 : 1.2, high: 102, low: 98, open: 99, prev: 99.2 };
  return { ...q, t: Date.now(), at: Date.now() - 5 * 60000 };
}

function sampleHist(q) {
  const pts = [];
  for (let i = 0; i < 24; i++) pts.push([Date.now() - (24 - i) * 3600000, q.prev + (q.price - q.prev) * (i / 23) + Math.sin(i * 1.7) * Math.abs(q.price - q.prev) * 0.4]);
  return pts;
}

function quoteOf(symbol, i) {
  if (dock.preview) return sampleQuote(symbol, i);
  return quotes[symbol] || null;
}

function histOf(symbol, i) {
  if (dock.preview) return sampleHist(sampleQuote(symbol, i));
  return hist[symbol] || [];
}

// --- formatting ----------------------------------------------------------------------------------

function group(intText) {
  return intText.replace(/\B(?=(\d{3})+(?!\d))/g, ',');
}

// Prices: two decimals, four under 1 (forex, small crypto), none above 100000.
function price(v) {
  if (v === null || v === undefined) return '–';
  const a = Math.abs(v);
  const d = a < 1 ? 4 : a >= 100000 ? 0 : 2;
  const s = a.toFixed(d);
  const [int, frac] = s.split('.');
  return (v < 0 ? '-' : '') + group(int) + (frac ? '.' + frac : '');
}

const sign = (v) => (v > 0 ? '+' : v < 0 ? '-' : '');

function changeText(q, mode) {
  if (!q || mode === 'none') return '';
  const parts = [];
  if ((mode === 'amount' || mode === 'both') && q.change !== null) parts.push(sign(q.change) + price(Math.abs(q.change)));
  if ((mode === 'percent' || mode === 'both') && q.pct !== null) parts.push((mode === 'both' ? '(' : sign(q.pct)) + Math.abs(q.pct).toFixed(2) + '%' + (mode === 'both' ? ')' : ''));
  if (!parts.length) return '';
  const dir = q.change > 0 ? 1 : q.change < 0 ? -1 : 0;
  const arrow = settings.arrows && dir ? (dir > 0 ? '▲' : '▼') : '';
  return arrow + parts.join(' ');
}

// "#AARRGGBB" with its alpha multiplied.
function fade(color, k) {
  const m = /^#([0-9a-f]{2})([0-9a-f]{6})$/i.exec(color);
  if (!m) return color;
  const a = Math.min(255, Math.round(parseInt(m[1], 16) * k));
  return '#' + a.toString(16).padStart(2, '0').toUpperCase() + m[2];
}

const isTransparent = (c) => typeof c === 'string' && /^#00/i.test(c);

function colors(dock) {
  const text = isTransparent(settings.textColor) || !settings.textColor ? dock.theme.text : settings.textColor;
  return { text, dim: fade(text, 0.62), up: settings.upColor || '#FF16C60C', down: settings.downColor || '#FFE81123' };
}

function changeColor(q, c) {
  if (!q || q.change === null || q.change === 0) return c.dim;
  return q.change > 0 ? c.up : c.down;
}

// A quote is drawn dimmed while it is older than two refreshes, or came from the store and this
// run has not fetched it yet (and nothing is fetched without a key).
function dimOf(symbol, q) {
  if (dock.preview) return 1;
  if (!q) return STALE;
  if (!freshRun[symbol] && !busy && (Date.now() - (q.at || 0)) > 2 * refreshMs()) return STALE;
  return 1;
}

function ago(ms) {
  if (!ms) return '';
  const min = Math.floor((Date.now() - ms) / 60000);
  if (min < 1) return 'Updated just now';
  if (min < 60) return `Updated ${min} min ago`;
  const h = Math.floor(min / 60);
  return h < 24 ? `Updated ${h} h ${min % 60} min ago` : `Updated ${Math.floor(h / 24)} d ago`;
}

// The newest fetch among the symbols shown (0: none yet).
function newestAt() {
  let newest = 0;
  for (const s of symbols) { const q = quotes[s]; if (q && q.at > newest) newest = q.at; }
  return newest;
}

// --- the "Updated N min ago" line on the slot (settings.updatedLabel) ------------------------------

const UPD_SIZE = 9, UPD_GAP = 2;
let updatedTimer = 0;

const updatedMode = () => (settings.updatedLabel === 'below' || settings.updatedLabel === 'beside' ? settings.updatedLabel : 'none');

function updatedText() {
  if (dock.preview) return 'Updated 5 min ago';
  if (!hasKey() || !symbols.length) return '';
  if (busy) return 'Fetching…';
  const at = newestAt();
  if (!at) return error ? 'Not updated' : 'Not updated yet';
  return ago(at);
}

// One timeout to the next whole minute of the newest fetch's age, only while the line is shown
// and the dock is on screen: a widget without the line never wakes for it.
function scheduleUpdatedTick() {
  if (updatedTimer) clearTimeout(updatedTimer);
  updatedTimer = 0;
  if (updatedMode() === 'none' || dock.preview || !dock.visible) return;
  const at = newestAt();
  if (!at) return;
  const age = Math.max(0, Date.now() - at);
  updatedTimer = setTimeout(() => { updatedTimer = 0; requestRedraw(); }, 60000 - (age % 60000) + 50);
}

// --- the slot ------------------------------------------------------------------------------------

const SYM_SIZE = 10, PRICE_SIZE = 14, CHANGE_SIZE = 11;
const SPARK_W = 28, SPARK_H = 12;
const GAP = 4;           // inside a cell
const CELL_GAP = 12;     // between cells of a row
const ROW_H = 20;        // a table row (standalone)
const PAD = 4;           // around everything with a background

const symStyle = (k) => ({ size: SYM_SIZE * k });
const priceStyle = (k) => ({ size: PRICE_SIZE * k, weight: 'semibold' });
const changeStyle = (k) => ({ size: CHANGE_SIZE * k, weight: 'semibold' });

const mt = (canvas, s, st) => (s ? canvas.measureText(s, st) : { width: 0, height: 0 });

// One symbol's cell: its texts and their sizes at scale k.
function cell(canvas, symbol, i, k) {
  const q = quoteOf(symbol, i);
  const sym = settings.showSymbol ? symbol : '';
  const pr = q ? price(q.price) : '–';
  const ch = changeText(q, settings.change);
  const a = mt(canvas, sym, symStyle(k)), b = mt(canvas, pr, priceStyle(k)), c = mt(canvas, ch, changeStyle(k));
  const spark = settings.sparkline ? { w: SPARK_W * k, h: SPARK_H * k } : null;
  return { symbol, q, i, sym, pr, ch, a, b, c, spark, k };
}

// The cell as drawn on a top / bottom dock: [symbol over price] [sparkline] [change].
function cellBoxH(cl) {
  const colW = Math.ceil(Math.max(cl.a.width, cl.b.width));
  const colH = Math.ceil(cl.a.height) + Math.ceil(cl.b.height);
  let w = colW;
  if (cl.spark) w += GAP * cl.k + cl.spark.w;
  if (cl.ch) w += GAP * cl.k + Math.ceil(cl.c.width);
  return { w, h: Math.max(colH, cl.spark ? cl.spark.h : 0, Math.ceil(cl.c.height)), colW, colH };
}

// The cell stacked (a left / right dock): symbol / price / change / sparkline, centred.
function cellBoxV(cl) {
  const w = Math.ceil(Math.max(cl.a.width, cl.b.width, cl.c.width, cl.spark ? cl.spark.w : 0));
  let h = Math.ceil(cl.a.height) + Math.ceil(cl.b.height) + (cl.ch ? Math.ceil(cl.c.height) : 0);
  if (cl.spark) h += GAP * cl.k + cl.spark.h;
  return { w, h };
}

function drawSpark(canvas, cl, x, y, c, dim) {
  const pts = histOf(cl.symbol, cl.i);
  if (pts.length < 2) return;
  let lo = Infinity, hi = -Infinity;
  for (const p of pts) { if (p[1] < lo) lo = p[1]; if (p[1] > hi) hi = p[1]; }
  const t0 = pts[0][0], t1 = pts[pts.length - 1][0];
  const path = canvas.path();
  pts.forEach((p, i) => {
    const px = x + (t1 > t0 ? ((p[0] - t0) / (t1 - t0)) * cl.spark.w : (i / (pts.length - 1)) * cl.spark.w);
    const py = y + (hi > lo ? (1 - (p[1] - lo) / (hi - lo)) * cl.spark.h : cl.spark.h / 2);
    if (i === 0) path.moveTo(px, py); else path.lineTo(px, py);
  });
  const col = cl.q && cl.q.prev !== null ? (pts[pts.length - 1][1] >= cl.q.prev ? c.up : c.down) : c.dim;
  canvas.stroke(path, fade(col, dim), { width: 1.25 * cl.k, join: 'round', cap: 'round' });
}

function drawCellH(canvas, cl, x, y, box, c, dim) {
  const k = cl.k;
  canvas.save();
  canvas.opacity(dim);
  const colTop = y + (box.h - box.colH) / 2;
  if (cl.sym) canvas.text(cl.sym, x, colTop, { ...symStyle(k), color: c.dim });
  canvas.text(cl.pr, x, colTop + Math.ceil(cl.a.height), { ...priceStyle(k), color: c.text });
  let cx = x + box.colW;
  if (cl.spark) {
    cx += GAP * k;
    drawSpark(canvas, cl, cx, y + (box.h - cl.spark.h) / 2, c, 1);
    cx += cl.spark.w;
  }
  if (cl.ch) {
    cx += GAP * k;
    canvas.text(cl.ch, cx, y + box.h / 2, { ...changeStyle(k), baseline: 'middle', color: changeColor(cl.q, c) });
  }
  canvas.restore();
}

function drawCellV(canvas, cl, cx, y, box, c, dim) {
  const k = cl.k;
  canvas.save();
  canvas.opacity(dim);
  if (cl.sym) { canvas.text(cl.sym, cx, y, { ...symStyle(k), align: 'center', color: c.dim }); }
  y += Math.ceil(cl.a.height);
  canvas.text(cl.pr, cx, y, { ...priceStyle(k), align: 'center', color: c.text });
  y += Math.ceil(cl.b.height);
  if (cl.ch) { canvas.text(cl.ch, cx, y, { ...changeStyle(k), align: 'center', color: changeColor(cl.q, c) }); y += Math.ceil(cl.c.height); }
  if (cl.spark) drawSpark(canvas, cl, cx - cl.spark.w / 2, y + GAP * k, c, 1);
  canvas.restore();
}

// Which symbols the slot shows, and how.
function plan(canvas, dock, k = 1) {
  const vertical = dock.axis === 'vertical' && !dock.standalone;
  const list = symbols.length ? (single() ? [symbols[Math.min(current, symbols.length - 1)]] : symbols) : [];
  const cells = list.map((s, i) => cell(canvas, s, single() ? current : i, k));
  const pad = settings.background && settings.background !== 'none' ? PAD * k : 0;
  const mode = single() ? 'single' : settings.layout === 'row' ? 'row' : settings.layout === 'list' ? 'list' : vertical || dock.standalone ? 'list' : 'row';
  // A table (one line per symbol) when the list is wide enough for it: standalone windows and
  // explicit lists on a top / bottom dock; a left / right dock stacks the cells instead.
  const table = mode === 'list' && !vertical;
  let w = 0, h = 0, boxes = [];
  if (!cells.length) {
    const msg = message();
    const m = canvas.measureText(msg, symStyle(k));
    w = Math.ceil(m.width); h = Math.ceil(m.height);
  } else if (table) {
    const symW = Math.ceil(Math.max(...cells.map((cl) => mt(canvas, cl.symbol, changeStyle(k)).width)));
    const prW = Math.ceil(Math.max(...cells.map((cl) => cl.b.width)));
    const chW = Math.ceil(Math.max(...cells.map((cl) => cl.c.width)));
    const sparkW = settings.sparkline ? SPARK_W * k + GAP * 2 * k : 0;
    w = symW + GAP * 2 * k + sparkW + prW + (chW ? GAP * 2 * k + chW : 0);
    h = cells.length * ROW_H * k;
    boxes = { symW, prW, chW, sparkW };
  } else if (vertical || (mode === 'list')) {
    boxes = cells.map(cellBoxV);
    w = Math.max(...boxes.map((b) => b.w));
    h = boxes.reduce((s, b) => s + b.h, 0) + (boxes.length - 1) * CELL_GAP * k * 0.75;
  } else {
    boxes = cells.map(cellBoxH);
    w = boxes.reduce((s, b) => s + b.w, 0) + (boxes.length - 1) * CELL_GAP * k;
    h = Math.max(...boxes.map((b) => b.h));
  }
  // The "Updated N min ago" line: under everything, or after it on a row (a left / right dock
  // and a list: under it).
  let upd = null;
  const updText = cells.length ? updatedText() : '';
  if (updText && updatedMode() !== 'none') {
    const m = canvas.measureText(updText, { size: UPD_SIZE * k });
    const beside = updatedMode() === 'beside' && !vertical && !table && mode !== 'list';
    upd = { text: updText, w: Math.ceil(m.width), h: Math.ceil(m.height), beside };
    if (beside) {
      w += CELL_GAP * k + upd.w;
      h = Math.max(h, upd.h);
    } else {
      w = Math.max(w, upd.w);
      h += UPD_GAP * k + upd.h;
    }
  }
  return { cells, boxes, mode, table, vertical, pad, k, upd, w: Math.ceil(w + 2 * pad), h: Math.ceil(h + 2 * pad) };
}

function drawUpdated(canvas, p, x0, y0, c) {
  const u = p.upd;
  if (!u) return;
  const style = { size: UPD_SIZE * p.k, color: c.dim };
  if (u.beside) {
    canvas.text(u.text, x0 + p.w - p.pad - u.w, y0 + p.h / 2, { ...style, baseline: 'middle' });
  } else {
    canvas.text(u.text, x0 + p.w / 2, y0 + p.h - p.pad - u.h, { ...style, align: 'center' });
  }
}

function message() {
  if (!hasKey() && !dock.preview) return 'Stocks: no API key';
  if (!symbols.length) return 'Stocks: no symbols';
  return 'Stocks';
}

export function measure(canvas, dock) {
  const p = plan(canvas, dock);
  return p.vertical ? { main: p.h, cross: p.w } : { main: p.w, cross: p.h };
}

function background(canvas, dock, x, y, w, h, k) {
  const bg = settings.background;
  if (!bg || bg === 'none') return;
  canvas.fillRoundRect(x, y, w, h, 6 * k, bg === 'theme' ? dock.theme.labelBackground : '#99000000');
}

export function draw(canvas, dock) {
  let p = plan(canvas, dock);
  if (dock.standalone) {
    // Grow the slot's layout to the window (a wide window gets bigger text, not more room).
    const k = Math.max(0.6, Math.min(3, (canvas.width - 8) / Math.max(1, p.w), (canvas.height - 8) / Math.max(1, p.h)));
    p = plan(canvas, dock, k);
  }
  const c = colors(dock);
  const x0 = dock.standalone ? Math.round((canvas.width - p.w) / 2) : 0;
  const y0 = dock.standalone ? Math.round((canvas.height - p.h) / 2) : 0;
  background(canvas, dock, x0, y0, p.w, p.h, p.k);
  if (settings.debugStore) {
    storage.set('slot', { mode: p.mode, table: p.table, cells: p.cells.map((cl) => ({ s: cl.symbol, pr: cl.pr, ch: cl.ch })), w: p.w, h: p.h, error, busy,
      updated: p.upd ? p.upd.text : '' });
  }
  if (!p.cells.length) {
    canvas.text(message(), x0 + p.pad, y0 + p.h / 2, { ...symStyle(p.k), baseline: 'middle', color: c.dim });
    return;
  }
  scheduleUpdatedTick();
  drawUpdated(canvas, p, x0, y0, c);
  // The room the cells have (the "Updated" line under them takes its own).
  const contentH = p.h - 2 * p.pad - (p.upd && !p.upd.beside ? UPD_GAP * p.k + p.upd.h : 0);
  let x = x0 + p.pad, y = y0 + p.pad;
  if (p.table) {
    const b = p.boxes, k = p.k, rh = ROW_H * k;
    p.cells.forEach((cl) => {
      const dim = dimOf(cl.symbol, cl.q), cy = y + rh / 2;
      canvas.save();
      canvas.opacity(dim);
      canvas.text(cl.symbol, x, cy, { ...changeStyle(k), baseline: 'middle', color: c.text });
      let cx = x + b.symW + GAP * 2 * k;
      if (cl.spark) { drawSpark(canvas, cl, cx + GAP * k, cy - cl.spark.h / 2, c, 1); cx += b.sparkW; }
      canvas.text(cl.pr, cx + b.prW, cy, { ...priceStyle(k), size: (PRICE_SIZE - 2) * k, align: 'right', baseline: 'middle', color: c.text });
      if (cl.ch) canvas.text(cl.ch, cx + b.prW + GAP * 2 * k + b.chW, cy, { ...changeStyle(k), align: 'right', baseline: 'middle', color: changeColor(cl.q, c) });
      canvas.restore();
      y += rh;
    });
    return;
  }
  if (p.vertical || p.mode === 'list') {
    const cx = x0 + p.w / 2;
    p.cells.forEach((cl, i) => {
      drawCellV(canvas, cl, cx, y, p.boxes[i], c, dimOf(cl.symbol, cl.q));
      y += p.boxes[i].h + CELL_GAP * p.k * 0.75;
    });
    return;
  }
  p.cells.forEach((cl, i) => {
    drawCellH(canvas, cl, x, y + (contentH - p.boxes[i].h) / 2, p.boxes[i], c, dimOf(cl.symbol, cl.q));
    x += p.boxes[i].w + CELL_GAP * p.k;
  });
}

export function onClick(e) {
  if (e.closedPopup) return;
  if (settings.clickOpens === 'refresh') fetchAll();
  else if (settings.clickOpens === 'popup' || !hasKey()) showPopup();
}

export function onMiddleClick() {
  fetchAll();
}

// --- the popup -----------------------------------------------------------------------------------

const P_PAD = 14, P_TITLE_TOP = 12, P_TITLE_H = 22, P_ROWS_TOP = 44, P_ROW_H = 44, P_BOTTOM = 34, P_BUTTON = 28;
const P_W = 340, P_MSG_W = 340;

function popupSize() {
  const n = symbols.length;
  if (!n || !hasKey()) return { w: P_MSG_W, h: P_ROWS_TOP + 64 + P_BOTTOM };  // three lines of instructions
  return { w: P_W, h: P_ROWS_TOP + n * P_ROW_H + P_BOTTOM + (error ? 18 : 0) };
}

function showPopup() {
  const s = popupSize();
  if (popupShown && shownSize && shownSize.w === s.w && shownSize.h === s.h) popup.redraw();
  else popup.open(s.w, s.h);
  shownSize = s;
  popupShown = true;
}

export function onPopupClose() {
  popupShown = false;
  shownSize = null;
  if (minuteTimer) clearTimeout(minuteTimer);
  minuteTimer = 0;
}

function tradeTime(q) {
  if (!q || !q.t) return '';
  const d = new Date(q.t);
  const h = d.getHours(), m = d.getMinutes();
  const hh = system.hour12 ? (h % 12 === 0 ? 12 : h % 12) : h;
  return (system.hour12 ? hh : String(hh).padStart(2, '0')) + ':' + String(m).padStart(2, '0') + (system.hour12 ? (h < 12 ? ' AM' : ' PM') : '');
}

function line(canvas, text, x, top, height, style) {
  if (text) canvas.text(text, x, top + height / 2, { ...style, baseline: 'middle' });
}

export function drawPopup(canvas, dock) {
  const w = canvas.width;
  const c = colors(dock);
  const text = dock.theme.text, secondary = fade(text, 0.62), border = dock.theme.border;
  hits = [];

  // Title row: "Stocks", the refresh button.
  line(canvas, 'Stocks', P_PAD, P_TITLE_TOP, P_TITLE_H, { size: 14, weight: 'semibold', color: text });
  if (hasKey() && symbols.length) {
    const b = { x: w - P_PAD - P_BUTTON, y: 9, w: P_BUTTON, h: P_BUTTON, what: 'refresh' };
    canvas.fillRoundRect(b.x, b.y, b.w, b.h, 6, fade(text, 0.08));
    canvas.text('', b.x + b.w / 2, b.y + b.h / 2, { font: 'Segoe Fluent Icons', size: 14, align: 'center', baseline: 'middle', color: busy ? fade(text, 0.4) : secondary });
    hits.push(b);
    line(canvas, busy ? 'Fetching…' : ago(newestAt()), b.x - 8, P_TITLE_TOP, P_TITLE_H, { size: 12, align: 'right', color: secondary });
  }

  if (!hasKey() || !symbols.length) {
    const msg = !hasKey()
      ? 'No API key. Get a free key at finnhub.io, then enter it under Widget settings… (right-click the widget) > Data > API key.'
      : 'No symbols. Enter ticker symbols under Widget settings… > Symbols.';
    canvas.text(msg, P_PAD, P_ROWS_TOP, { size: 12.5, color: secondary, maxWidth: w - 2 * P_PAD });
    if (!hasKey()) {
      const l = { x: P_PAD, y: canvas.height - P_BOTTOM + 6, w: 120, h: 20, what: 'register' };
      line(canvas, 'finnhub.io/register', l.x, l.y, l.h, { size: 12, color: dock.theme.accent });
      hits.push(l);
    }
    if (settings.debugStore) storage.set('popup', { message: msg, width: w, height: canvas.height });
    return;
  }

  // One row per symbol: the symbol and the trade time; the price and the change on the right;
  // the day's open, high, low and previous close under them.
  const link = settings.rowLink && settings.rowLink !== 'none';
  symbols.forEach((s, i) => {
    const q = quotes[s];
    const y = P_ROWS_TOP + i * P_ROW_H;
    const dim = dimOf(s, q);
    if (i) canvas.line(P_PAD, y - 0.5, w - P_PAD, y - 0.5, border);
    canvas.save();
    canvas.opacity(dim);
    line(canvas, s, P_PAD, y + 4, 20, { size: 13, weight: 'semibold', color: text });
    const t = tradeTime(q);
    if (t) line(canvas, 'Last trade ' + t, P_PAD, y + 22, 18, { size: 11, color: secondary });
    if (q) {
      const ch = changeText(q, 'both');  // the popup always shows the amount and the percent
      line(canvas, price(q.price), w - P_PAD, y + 4, 20, { size: 15, weight: 'semibold', align: 'right', color: text });
      line(canvas, ch, w - P_PAD, y + 22, 18, { size: 11.5, weight: 'semibold', align: 'right', color: changeColor(q, c) });
      const parts = [];
      if (q.open !== null) parts.push('O ' + price(q.open));
      if (q.high !== null) parts.push('H ' + price(q.high));
      if (q.low !== null) parts.push('L ' + price(q.low));
      if (q.prev !== null) parts.push('PC ' + price(q.prev));
      const detailsW = Math.ceil(canvas.measureText(ch, { size: 11.5, weight: 'semibold' }).width) + 12;
      line(canvas, parts.join('  '), P_PAD + 110, y + 22, 18, { size: 10.5, color: secondary, maxWidth: Math.max(40, w - P_PAD - 110 - detailsW - P_PAD) });
    } else {
      line(canvas, busy ? 'Fetching…' : 'No quote yet', w - P_PAD, y + 4, 20, { size: 12, align: 'right', color: secondary });
    }
    canvas.restore();
    if (link) hits.push({ x: 0, y, w, h: P_ROW_H, what: 'row', symbol: s });
  });

  // The footer: the error, if any, and the credit.
  let fy = P_ROWS_TOP + symbols.length * P_ROW_H;
  if (error) {
    line(canvas, error, P_PAD, fy, 18, { size: 11, color: c.down, maxWidth: w - 2 * P_PAD });
    fy += 18;
  }
  const creditW = Math.ceil(canvas.measureText(CREDIT, { size: 11.5 }).width);
  const credit = { x: w - P_PAD - creditW, y: fy + 8, w: creditW, h: 18, what: 'credit' };
  line(canvas, CREDIT, w - P_PAD, credit.y, credit.h, { size: 11.5, align: 'right', color: secondary });
  hits.push(credit);
  if (link) line(canvas, 'Click a symbol to open it', P_PAD, credit.y, credit.h, { size: 11, color: secondary });

  if (settings.debugStore) storage.set('popup', { rows: symbols.length, error, busy, width: w, height: canvas.height });

  // "N min ago" stays current while the popup is open.
  if (minuteTimer) clearTimeout(minuteTimer);
  minuteTimer = setTimeout(() => { minuteTimer = 0; if (popupShown) popup.redraw(); }, 60000);
}

function symbolUrl(symbol) {
  const s = encodeURIComponent(symbol);
  switch (settings.rowLink) {
    case 'google': return 'https://www.google.com/finance/quote/' + s;
    case 'finnhub': return 'https://finnhub.io/quote?symbol=' + s;
    default: return 'https://finance.yahoo.com/quote/' + s;
  }
}

export function onPopupClick(e) {
  if (e.button !== 'left') return;
  const hit = hits.find((h) => e.x >= h.x && e.x < h.x + h.w && e.y >= h.y && e.y < h.y + h.h);
  if (!hit) return;
  if (hit.what === 'refresh') fetchAll();
  else if (hit.what === 'credit') shell.open(CREDIT_URL);
  else if (hit.what === 'register') shell.open('https://finnhub.io/register');
  else if (hit.what === 'row') shell.open(symbolUrl(hit.symbol));
}

// --- start ---------------------------------------------------------------------------------------

applySettings();
