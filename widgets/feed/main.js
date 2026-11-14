// @ts-check
/// <reference path="../../docs/windock.d.ts" />
// Headlines (round 29 phase 29C): the newest items of an RSS or Atom feed the user chooses.
//
// The feed's address is a `url` setting named in the manifest's `hostSettings`: the widget names no
// host of its own; the host the user types is allowed only after ViberDock's prompt, for this item
// (http.get to any other host throws). A click opens the list (a script popup); a click on a row
// opens the item's link with links.open, which works only right after a click and only for http /
// https addresses. A middle click opens the headline the tile shows.
//
// Cost: one request every refreshMinutes (timer.setRefresh, paused while the dock cannot be seen);
// the rotation is one setTimeout per headline, only while the dock is on screen. The response is
// at most 2 MB (the host's cap); only its first 512 KB are parsed, with indexOf scans (no regular
// expression over the whole text), so a large feed stays well inside the 250 ms time limit.

const MAX_PARSE = 512 * 1024;
const POPUP_W = 380, ROW_H = 40, HEAD_H = 34;

/** @type {{ title: string, link: string, date: number }[]} */
let items = [];
let feedTitle = '';
let status = 'idle';   // idle | loading | ok | error | nourl
let message = '';
let shown = 0;         // the headline the tile shows (rotation)
let rotateTimer = 0;
let fetching = false;
/** @type {{ y: number, h: number, i: number }[]} */
let rows = [];

const SAMPLE = [
  { title: 'Sample headline: a widget for the feeds you read', link: 'https://example.com/1', date: Date.now() - 600000 },
  { title: 'Second sample headline', link: 'https://example.com/2', date: Date.now() - 3600000 },
  { title: 'A third one, with a longer title that does not fit on the tile', link: 'https://example.com/3', date: Date.now() - 7200000 },
];

// --- parsing (tolerant: RSS 2.0, RSS 1.0 / RDF, Atom) ------------------------------------------------

const ENTITIES = { amp: '&', lt: '<', gt: '>', quot: '"', apos: "'", nbsp: ' ' };
function decode(s) {
  return s.replace(/&(#x[0-9a-fA-F]+|#[0-9]+|[a-zA-Z]+);/g, (m, e) => {
    if (e[0] === '#') {
      const n = e[1] === 'x' || e[1] === 'X' ? parseInt(e.slice(2), 16) : parseInt(e.slice(1), 10);
      return Number.isFinite(n) && n > 0 && n < 0x110000 ? String.fromCodePoint(n) : '';
    }
    return ENTITIES[e] ?? m;
  });
}
function textOf(s) {
  // CDATA sections as they are; tags dropped; entities decoded; white space folded.
  let out = '', at = 0;
  while (at < s.length) {
    const c = s.indexOf('<![CDATA[', at);
    if (c < 0) { out += decode(s.slice(at).replace(/<[^>]*>/g, ' ')); break; }
    out += decode(s.slice(at, c).replace(/<[^>]*>/g, ' '));
    const end = s.indexOf(']]>', c + 9);
    const inner = s.slice(c + 9, end < 0 ? s.length : end);
    out += inner.replace(/<[^>]*>/g, ' ');
    at = end < 0 ? s.length : end + 3;
  }
  // An escaped HTML title (Atom type="html", many RSS feeds) has its tags after decoding: drop those too.
  return out.replace(/<\/?[a-zA-Z][^<>]*>/g, ' ').replace(/\s+/g, ' ').trim();
}
// The first <name ...>...</name> (or a self-closed one) inside s: { attrs, body }, or null.
function tag(s, name) {
  let at = 0;
  for (;;) {
    const open = s.indexOf('<' + name, at);
    if (open < 0) return null;
    const next = s[open + name.length + 1];
    if (next !== '>' && next !== ' ' && next !== '/' && next !== '\t' && next !== '\n' && next !== '\r') { at = open + 1; continue; }
    const gt = s.indexOf('>', open);
    if (gt < 0) return null;
    const attrs = s.slice(open + name.length + 1, gt);
    if (attrs.endsWith('/')) return { attrs, body: '' };
    const close = s.indexOf('</' + name + '>', gt);
    return { attrs, body: s.slice(gt + 1, close < 0 ? s.length : close) };
  }
}
function attr(attrs, name) {
  const m = new RegExp('\\b' + name + '\\s*=\\s*("([^"]*)"|\'([^\']*)\')').exec(attrs);
  return m ? decode(m[2] ?? m[3] ?? '') : '';
}
const isWeb = (u) => /^https?:\/\/[^\s]+$/i.test(u);

function parse(xml) {
  const s = xml.length > MAX_PARSE ? xml.slice(0, MAX_PARSE) : xml;
  const atom = s.indexOf('<entry') >= 0 && s.indexOf('<item') < 0;
  const unit = atom ? 'entry' : 'item';
  // The feed's own title: the first <title> before the first item.
  const first = s.indexOf('<' + unit);
  const head = first < 0 ? s : s.slice(0, first);
  const t = tag(head, 'title');
  const title = t ? textOf(t.body) : '';
  const out = [];
  let at = 0;
  const limit = Math.max(3, Math.min(30, settings.items || 10));
  while (out.length < limit) {
    const open = s.indexOf('<' + unit, at);
    if (open < 0) break;
    const close = s.indexOf('</' + unit + '>', open);
    const body = s.slice(open, close < 0 ? s.length : close);
    at = close < 0 ? s.length : close + unit.length + 3;
    const it = tag(body, 'title');
    let link = '';
    if (atom) {
      // <link href="..."/>: the rel="alternate" one (or the one without rel).
      let la = 0;
      for (;;) {
        const l = body.indexOf('<link', la);
        if (l < 0) break;
        const gt = body.indexOf('>', l);
        const attrs = body.slice(l + 5, gt < 0 ? body.length : gt);
        const rel = attr(attrs, 'rel');
        if (!rel || rel === 'alternate') { link = attr(attrs, 'href'); break; }
        la = l + 5;
      }
    } else {
      const l = tag(body, 'link');
      link = l ? textOf(l.body) : '';
      if (!link) { const g = tag(body, 'guid'); if (g && attr(g.attrs, 'isPermaLink') !== 'false') link = textOf(g.body); }
    }
    const d = tag(body, atom ? 'updated' : 'pubDate') || tag(body, 'published') || tag(body, 'dc:date');
    const date = d ? Date.parse(textOf(d.body)) : NaN;
    const headline = it ? textOf(it.body) : '';
    if (headline) out.push({ title: headline.slice(0, 300), link: isWeb(link) ? link : '', date: Number.isFinite(date) ? date : 0 });
  }
  return { title, items: out };
}

// --- fetching ------------------------------------------------------------------------------------------

async function fetchFeed() {
  if (dock.preview) {
    items = SAMPLE;
    feedTitle = 'Sample feed';
    status = 'ok';
    requestRedraw();
    return;
  }
  const url = String(settings.feedUrl || '').trim();
  if (!url) {
    status = 'nourl';
    items = [];
    requestRedraw();
    return;
  }
  if (fetching) return;
  fetching = true;
  if (!items.length) status = 'loading';
  requestRedraw();
  try {
    const r = await http.get(url, { type: 'text', timeout: 15000, headers: { Accept: 'application/rss+xml, application/atom+xml, application/xml, text/xml' } });
    if (!r.ok) throw new Error('the feed answered ' + r.status);
    const p = parse(r.text);
    if (!p.items.length) throw new Error('no headlines found (is it an RSS or Atom feed?)');
    items = p.items;
    feedTitle = p.title;
    status = 'ok';
    message = '';
    if (shown >= items.length) shown = 0;
  } catch (e) {
    status = 'error';
    message = String(e && e.message ? e.message : e);
    console.warn('feed: ' + message);
  } finally {
    fetching = false;
  }
  requestRedraw();
  if (popup.isOpen) popup.open(POPUP_W, popupHeight());
  scheduleRotation();
}

function scheduleRotation() {
  if (rotateTimer) { clearTimeout(rotateTimer); rotateTimer = 0; }
  if (!settings.rotate || items.length < 2 || !dock.visible || dock.preview || dock.reduceMotion) return;
  rotateTimer = setTimeout(() => {
    rotateTimer = 0;
    shown = (shown + 1) % Math.min(items.length, Math.max(3, settings.items || 10));
    requestRedraw();
    scheduleRotation();
  }, Math.max(5, settings.rotateSeconds || 12) * 1000);
}

function start() {
  timer.setRefresh(Math.max(5, settings.refreshMinutes || 30) * 60000);
  fetchFeed();
}
start();

export function tick() {
  fetchFeed();
  return false;  // fetchFeed redraws when the answer comes
}
export function onSettingsChanged() {
  shown = 0;
  start();
}
export function onVisibilityChange(e) {
  if (e.visible) scheduleRotation();
  else if (rotateTimer) { clearTimeout(rotateTimer); rotateTimer = 0; }
}

// --- the tile ----------------------------------------------------------------------------------------

// `text` cut with "…" to `w` DIPs.
function fit(canvas, text, w, style) {
  if (canvas.measureText(text, style).width <= w) return text;
  let lo = 0, hi = text.length;
  while (lo < hi) {
    const mid = (lo + hi + 1) >> 1;
    if (canvas.measureText(text.slice(0, mid) + '…', style).width <= w) lo = mid; else hi = mid - 1;
  }
  return text.slice(0, lo).trimEnd() + '…';
}

function tileLines() {
  if (status === 'nourl') return ['Headlines', 'Set a feed in its settings'];
  if (status === 'loading' && !items.length) return [feedTitle || 'Headlines', 'Loading…'];
  if (status === 'error' && !items.length) {
    const notAllowed = /not allowed/.test(message);
    return [feedTitle || 'Headlines', notAllowed ? 'Not allowed: enter the address again' : 'Could not load the feed'];
  }
  const it = items[Math.min(shown, items.length - 1)];
  return [feedTitle || 'Headlines', it ? it.title : ''];
}

export function measure(canvas, dock) {
  if (dock.axis === 'vertical' && !dock.standalone) return { main: Math.round(dock.iconSize), cross: Math.round(dock.iconSize) };
  return { main: Math.max(100, Math.min(600, settings.width || 220)), cross: Math.round(dock.iconSize) };
}

function feedGlyph(canvas, cx, cy, r, color) {
  // The usual feed mark: a dot and two arcs.
  canvas.fillCircle(cx - r * 0.55, cy + r * 0.55, r * 0.2, color);
  const w = Math.max(1.5, r * 0.18);
  canvas.stroke(canvas.path().arc(cx - r * 0.55, cy + r * 0.55, r * 0.75, -Math.PI / 2, 0), color, { width: w, cap: 'round' });
  canvas.stroke(canvas.path().arc(cx - r * 0.55, cy + r * 0.55, r * 1.25, -Math.PI / 2, 0), color, { width: w, cap: 'round' });
}

export function draw(canvas, dock) {
  const W = canvas.width, H = canvas.height;
  const ink = dock.theme.text, dim = dock.theme.dateText, accent = dock.theme.accent;
  if (dock.axis === 'vertical' && !dock.standalone) {
    feedGlyph(canvas, W / 2, H / 2, Math.min(W, H) * 0.3, status === 'error' ? dim : accent);
    return;
  }
  const k = dock.standalone ? Math.max(0.8, Math.min(2, H / 48)) : Math.max(0.75, dock.iconSize / 48);
  const g = Math.round(14 * k);
  feedGlyph(canvas, 4 + g / 2, H / 2, g / 2, status === 'error' ? dim : accent);
  const x = 8 + g + 6, w = W - x - 4;
  const [source, headline] = tileLines();
  const small = { size: 10 * k, color: dim };
  const big = { size: 12 * k, weight: 'semibold', color: ink };
  if (settings.showSource !== false) {
    canvas.text(fit(canvas, source, w, small), x, H / 2 - 1, { ...small, baseline: 'bottom' });
    canvas.text(fit(canvas, headline, w, big), x, H / 2 + 1, { ...big, baseline: 'top' });
  } else {
    canvas.text(fit(canvas, headline, w, big), x, H / 2, { ...big, baseline: 'middle' });
  }
}

export function onClick(e) {
  if (e.closedPopup) return;
  if (status !== 'ok' && !items.length) {
    if (status === 'error' || status === 'idle') fetchFeed();  // try again
    return;
  }
  popup.open(POPUP_W, popupHeight());
}

export function onMiddleClick() {
  const it = items[Math.min(shown, items.length - 1)];
  if (it && it.link) links.open(it.link);
}

// --- the popup -----------------------------------------------------------------------------------------

function popupHeight() {
  return HEAD_H + Math.max(1, Math.min(items.length, settings.items || 10)) * ROW_H + 8;
}

function age(t) {
  if (!t) return '';
  const m = Math.max(0, Math.round((Date.now() - t) / 60000));
  if (m < 60) return m + ' min ago';
  const h = Math.round(m / 60);
  if (h < 48) return h + ' h ago';
  return Math.round(h / 24) + ' days ago';
}

export function drawPopup(canvas, dock) {
  const W = canvas.width;
  const ink = dock.theme.text, dim = dock.theme.dateText;
  canvas.text(fit(canvas, feedTitle || 'Headlines', W - 24, { size: 13, weight: 'semibold' }), 12, HEAD_H / 2,
    { size: 13, weight: 'semibold', baseline: 'middle', color: ink });
  if (status === 'error') {
    canvas.text(fit(canvas, 'Not updated: ' + message, W - 24, { size: 10 }), W - 12, HEAD_H / 2, { size: 10, align: 'right', baseline: 'middle', color: dim });
  }
  canvas.line(12, HEAD_H - 1, W - 12, HEAD_H - 1, dock.theme.separator, 1);
  rows = [];
  const n = Math.min(items.length, settings.items || 10);
  for (let i = 0; i < n; i++) {
    const it = items[i], y = HEAD_H + i * ROW_H;
    if (i === shown) canvas.fillRoundRect(6, y + 2, W - 12, ROW_H - 4, 6, '#18808080');
    canvas.text(fit(canvas, it.title, W - 24, { size: 12 }), 12, y + 7, { size: 12, baseline: 'top', color: it.link ? ink : dim });
    canvas.text(age(it.date) + (it.link ? '' : ' · no link'), 12, y + ROW_H - 6, { size: 10, baseline: 'bottom', color: dim });
    rows.push({ y, h: ROW_H, i });
  }
}

export function onPopupClick(e) {
  const r = rows.find((row) => e.y >= row.y && e.y < row.y + row.h);
  if (!r) return;
  const it = items[r.i];
  if (it && it.link && links.open(it.link)) popup.close();
}
