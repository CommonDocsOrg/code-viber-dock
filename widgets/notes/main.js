// Notes (round 6 phase 6C): one note per item, edited in the dock's text editor (popup.editText).
//
// The text lives in this widget's storage (widget-data\notes.json) under "note:" + the item's key
// (dock.itemKey: "<dock id>/notes#<n>" for the n-th Notes item of a dock, the entry id for a
// standalone one), or under "note:id:" + the "Note id" setting when that is set (items with the
// same id share one note). Values: { text, modified } (ms since 1970).
//
// On a dock: the first line is the title (bold), then `lines` lines under it as far as they fit
// the tile's height; the tile grows with the text up to `maxWidth`; the wheel scrolls the lines; a
// left / right dock shows the title only in a square tile. Standalone: the whole note, wrapped, in
// the window (10 DIPs of padding), the wheel scrolls it. A click opens the editor. Nothing runs
// while idle: no timers; another item's edit of a shared note arrives through storage.onChange.

const SAMPLE = 'Shopping list\nMilk, eggs, bread';
const MAX_LENGTH = 20000;  // characters per note (storage holds 1 MB for every note together)
const PAPER = { yellow: '#FFFFF1A6', pink: '#FFFFD2E1', blue: '#FFD3E7FF' };
const INK = '#FF262626';
const INK_DIM = '#99262626';
const LINE = 1.3;  // line height / font size

let scroll = 0;     // a dock: the first line under the title that is shown; standalone: DIPs
let maxScroll = 0;  // standalone: set by draw()
let wheelRest = 0;  // a dock: wheel delta not used yet (fine-grained wheels)
let editing = false;

const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));

// The storage key of this item's note.
function key() {
  const id = String(settings.noteId ?? '').trim();
  return 'note:' + (id ? 'id:' + id.slice(0, 200) : dock.itemKey);
}

function noteText() {
  if (dock.preview) return SAMPLE;  // the widget drawer's preview (round 6 phase 6E)
  const n = storage.get(key(), null);
  return n && typeof n.text === 'string' ? n.text : '';
}

// { title, body: [lines] } (trailing empty lines dropped).
function split(text) {
  const lines = text.replace(/\r\n?/g, '\n').split('\n');
  while (lines.length > 1 && lines[lines.length - 1].trim() === '') lines.pop();
  return { title: lines[0], body: lines.slice(1) };
}

function automatic(c) {
  return typeof c !== 'string' || c === '' || /^#00[0-9a-f]{6}$/i.test(c);
}

// Paper, ink and the placeholder's colour.
function colours(d) {
  const custom = automatic(settings.textColor) ? null : settings.textColor;
  const p = settings.paper;
  if (PAPER[p]) return { paper: PAPER[p], ink: custom ?? INK, dim: INK_DIM };
  if (p === 'theme') return { paper: d.theme.labelBackground, ink: custom ?? d.theme.labelText, dim: d.theme.dateText };
  return { paper: null, ink: custom ?? d.theme.text, dim: d.theme.dateText };
}

// A rounded sheet with a soft shadow line under it.
function sheet(canvas, x, y, w, h, r, fill) {
  canvas.fillRoundRect(x, y + 1, w, h, r, '#33000000');
  canvas.fillRoundRect(x, y, w, h, r, fill);
}

// `s` shortened with an ellipsis to fit `width`.
function fit(canvas, s, width, style) {
  s = s.length > 300 ? s.slice(0, 300) : s;
  if (canvas.measureText(s, style).width <= width) return s;
  let lo = 0, hi = s.length;
  while (lo < hi) {
    const mid = (lo + hi + 1) >> 1;
    if (canvas.measureText(s.slice(0, mid) + '…', style).width <= width) lo = mid; else hi = mid - 1;
  }
  return lo > 0 ? s.slice(0, lo).trimEnd() + '…' : '';
}

// --- on a dock -----------------------------------------------------------------------------------

// The tile's text metrics: the font shrinks (to 7 DIPs at least) until the title and `lines` lines
// fit the dock's height; `shown` is how many lines under the title fit at that size.
function tile(d) {
  const cross = Math.max(24, Math.round(d.iconSize));
  const padY = 4, padX = 8;
  const lines = clamp(Math.round(settings.lines) || 2, 1, 6);
  const size = clamp((cross - 2 * padY) / ((1 + lines) * LINE), 7, settings.fontSize || 12);
  const lineH = size * LINE;
  const shown = clamp(Math.floor((cross - 2 * padY - lineH) / lineH + 0.001), 0, lines);
  return { cross, padY, padX, size, lineH, shown };
}

function styles(size, ink) {
  return {
    title: { size, weight: settings.boldTitle ? 'bold' : 'normal', color: ink },
    body: { size, color: ink },
  };
}

export function measure(canvas, d) {
  const t = tile(d);
  if (d.axis === 'vertical') return { main: t.cross, cross: t.cross };
  const text = noteText();
  const st = styles(t.size);
  let w = 0;
  if (!text.trim()) {
    w = canvas.measureText('New note', st.body).width;
  } else {
    // The widest of the title and the lines (not only the ones shown: scrolling keeps the width).
    const { title, body } = split(text);
    w = canvas.measureText(title.slice(0, 300), st.title).width;
    if (t.shown > 0) for (const l of body.slice(0, 100)) w = Math.max(w, canvas.measureText(l.slice(0, 300), st.body).width);
  }
  const maxW = clamp(Math.round(settings.maxWidth) || 180, 60, 400);
  return { main: Math.round(clamp(Math.ceil(w) + 2 * t.padX, 48, maxW)), cross: t.cross };
}

function drawTile(canvas, d) {
  const w = canvas.width, h = canvas.height;
  const t = tile(d);
  const c = colours(d);
  if (c.paper) sheet(canvas, 1, 2, w - 2, h - 5, 6, c.paper);
  const text = noteText();
  const st = styles(t.size, c.ink);
  canvas.clip(2, 2, w - 4, h - 4);
  if (!text.trim()) {
    const style = { size: t.size, color: c.dim, align: 'center', baseline: 'middle' };
    if (d.axis === 'vertical') style.maxWidth = w - 6;
    canvas.text(d.axis === 'vertical' ? 'New note' : fit(canvas, 'New note', w - 2 * t.padX, style), w / 2, h / 2, style);
    return;
  }
  const { title, body } = split(text);
  if (d.axis === 'vertical') {
    // A square tile: the title only, wrapped; smaller when its longest word would not fit a line.
    const avail = w - 6;
    let size = t.size, widest = 0;
    for (const word of title.split(/\s+/).filter(Boolean).slice(0, 20)) {
      widest = Math.max(widest, canvas.measureText(word.slice(0, 40), st.title).width);
    }
    if (widest > avail) size = Math.max(7, size * avail / widest);
    canvas.text(title.slice(0, 200), w / 2, h / 2, { ...st.title, size, align: 'center', baseline: 'middle', maxWidth: avail });
    return;
  }
  scroll = clamp(scroll, 0, Math.max(0, body.length - t.shown));
  const lines = body.slice(scroll, scroll + t.shown);
  const top = (h - t.lineH * (1 + lines.length)) / 2;
  const tw = w - 2 * t.padX;
  canvas.text(fit(canvas, title, tw, st.title), t.padX, top + t.lineH / 2, { ...st.title, baseline: 'middle' });
  lines.forEach((l, i) => {
    canvas.text(fit(canvas, l, tw, st.body), t.padX, top + t.lineH * (i + 1.5), { ...st.body, baseline: 'middle' });
  });
  // More lines than shown: a small mark at the right edge, its place following the scroll.
  if (body.length > t.shown && t.shown > 0) {
    const track = h - 12, frac = scroll / Math.max(1, body.length - t.shown);
    canvas.fillRoundRect(w - 4, 6 + frac * (track - 8), 2, 8, 1, c.dim);
  }
}

// --- standalone ----------------------------------------------------------------------------------

function drawSheet(canvas, d) {
  const w = canvas.width, h = canvas.height, pad = 10;
  const c = colours(d);
  if (c.paper) sheet(canvas, 1, 1, w - 2, h - 3, 8, c.paper);
  const size = settings.fontSize || 12;
  const text = noteText();
  const tw = Math.max(10, w - 2 * pad), th = Math.max(10, h - 2 * pad);
  if (!text.trim()) {
    canvas.text('New note', w / 2, h / 2, { size, color: c.dim, align: 'center', baseline: 'middle', maxWidth: tw });
    maxScroll = 0;
    return;
  }
  const { title, body } = split(text);
  const st = styles(size, c.ink);
  const tm = canvas.measureText(title || ' ', { ...st.title, maxWidth: tw });
  const rest = body.join('\n');
  const gap = rest ? size * 0.35 : 0;
  const bm = rest ? canvas.measureText(rest, { ...st.body, maxWidth: tw }) : { height: 0 };
  maxScroll = Math.max(0, tm.height + gap + bm.height - th);
  scroll = clamp(scroll, 0, maxScroll);
  canvas.save();
  canvas.clip(pad, pad, tw, th);
  canvas.text(title || ' ', pad, pad - scroll, { ...st.title, maxWidth: tw, baseline: 'top' });
  if (rest) canvas.text(rest, pad, pad - scroll + tm.height + gap, { ...st.body, maxWidth: tw, baseline: 'top' });
  canvas.restore();
  if (maxScroll > 0) {
    // Where the view is in the note: a thin bar at the right edge.
    const barH = Math.max(16, th * th / (th + maxScroll));
    canvas.fillRoundRect(w - 5, pad + (th - barH) * (scroll / maxScroll), 3, barH, 1.5, c.dim);
  }
}

// --- the host's calls ----------------------------------------------------------------------------

export function draw(canvas, d) {
  if (d.standalone) drawSheet(canvas, d); else drawTile(canvas, d);
}

export async function onClick() {
  if (editing || dock.preview) return;
  editing = true;
  const k = key();  // the note being edited, whatever happens to the settings meanwhile
  try {
    const text = await popup.editText({
      title: 'Note', text: noteText(), multiline: true, maxLength: MAX_LENGTH,
      placeholder: 'Write your note here. Its first line is its title.',
    });
    if (text !== null) {
      storage.set(k, { text, modified: Date.now() });
      scroll = 0;
      requestRedraw();
    }
  } catch (e) {
    console.warn('Notes: ' + (e && e.message));
  } finally {
    editing = false;
  }
}

export function onWheel(e) {
  if (dock.standalone) {
    const before = scroll;
    scroll = clamp(scroll - e.notches * (settings.fontSize || 12) * LINE * 3, 0, maxScroll);
    if (scroll !== before) requestRedraw();
    return true;
  }
  if (dock.axis === 'vertical') return false;
  const t = tile(dock);
  const max = Math.max(0, split(noteText()).body.length - t.shown);
  if (max === 0) return false;
  wheelRest += e.delta;
  const before = scroll;
  while (wheelRest >= 120) { scroll--; wheelRest -= 120; }
  while (wheelRest <= -120) { scroll++; wheelRest += 120; }
  scroll = clamp(scroll, 0, max);
  if (scroll !== before) requestRedraw();
  return true;
}

export function onSettingsChanged() {
  scroll = 0;
  requestRedraw();
}

// Another item that shows the same note (the same Note id) saved it.
storage.onChange((e) => {
  if (e.key === key()) requestRedraw();
});
