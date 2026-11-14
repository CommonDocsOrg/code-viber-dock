// To-do (round 28 phase 28B): a short list of things to do. The tile shows a check-box glyph with
// the number still open; a click opens the list: tick a row off (its box), delete it (its ×), add
// one (+ Add, typed in the dock's text editor: popup.editText) and clear the done ones.
//
// The list lives in the widget's storage under "todo:<item key>" (or the List id setting, so two
// items can share one list; another item's change arrives through storage.onChange). Nothing runs
// while nobody touches it: no timer, no refresh.

const MAX_ITEMS = 200, MAX_TEXT = 200;
const POPUP_W = 320, ROW_H = 30, PAGE = 10;

let list = [];      // [{ id, text, done }]
let page = 0;       // the popup's page of PAGE rows
let hits = [];
let editing = false;

const custom = (c) => (c && !/^#00[0-9a-f]{6}$/i.test(c) ? c : null);
const key = () => 'todo:' + (settings.listId || dock.itemKey || 'default');
const open = () => list.filter((t) => !t.done).length;

const SAMPLE = [
  { id: 1, text: 'Book the dentist', done: false },
  { id: 2, text: 'Send the report', done: false },
  { id: 3, text: 'Buy coffee', done: true },
  { id: 4, text: 'Call Sam back', done: false },
];

function load() {
  if (dock.preview) {
    list = SAMPLE.map((t) => ({ ...t }));
    return;
  }
  const v = storage.get(key(), null);
  list = Array.isArray(v && v.items)
    ? v.items.filter((t) => t && typeof t.text === 'string').slice(0, MAX_ITEMS)
        .map((t, i) => ({ id: Number.isFinite(t.id) ? t.id : i + 1, text: t.text.slice(0, MAX_TEXT), done: !!t.done }))
    : [];
}

function save() {
  if (dock.preview) return;
  storage.set(key(), { items: list });
}

function changed() {
  save();
  requestRedraw();
  if (popup.isOpen) {
    page = Math.min(page, pages() - 1);
    popup.open(POPUP_W, popupHeight());  // resizes and redraws
  }
}

load();

storage.onChange((e) => {
  if (e.key !== key()) return;
  load();
  requestRedraw();
  if (popup.isOpen) popup.open(POPUP_W, popupHeight());
});

export function onSettingsChanged() {
  load();
  requestRedraw();
  if (popup.isOpen) popup.open(POPUP_W, popupHeight());
}

// --- the tile ----------------------------------------------------------------------------------------

function layout(canvas, dock, k = 1) {
  const icon = dock.standalone ? 48 : dock.iconSize;
  const g = Math.max(12, Math.round(icon * 0.5 * (settings.scale || 1) * k));
  const vertical = dock.axis === 'vertical' && !dock.standalone;
  const size = Math.max(9, icon * 0.3 * (settings.scale || 1) * k);
  const style = { size, weight: 'semibold', font: settings.font || undefined };
  const count = String(open());
  const label = settings.showLabel && !vertical ? (settings.label || 'To-do') : '';
  const text = label ? `${count} ${label}` : count;
  const tw = Math.ceil(canvas.measureText(text, style).width);
  const gap = Math.round(5 * k);
  return { icon, g, vertical, style, text, tw, gap, w: vertical ? Math.max(g, tw) : g + gap + tw, h: vertical ? g + 2 + size : Math.max(g, size) };
}

export function measure(canvas, dock) {
  const l = layout(canvas, dock);
  const icon = Math.round(dock.iconSize);
  return l.vertical ? { main: Math.ceil(l.h + 10), cross: icon } : { main: Math.ceil(l.w + 12), cross: icon };
}

function box(canvas, x, y, g, ink, checked, accent) {
  const sw = Math.max(1.2, g * 0.09), r = g * 0.2;
  if (checked) {
    canvas.fillRoundRect(x, y, g, g, r, accent);
    canvas.stroke(canvas.path().moveTo(x + g * 0.25, y + g * 0.52).lineTo(x + g * 0.43, y + g * 0.7).lineTo(x + g * 0.76, y + g * 0.32),
      '#FFFFFFFF', { width: sw * 1.2, cap: 'round', join: 'round' });
    return;
  }
  canvas.strokeRoundRect(x + sw / 2, y + sw / 2, g - sw, g - sw, r, ink, sw);
}

export function draw(canvas, dock) {
  const W = canvas.width, H = canvas.height;
  let l = layout(canvas, dock);
  if (dock.standalone) {
    const k = Math.max(0.5, Math.min((W * 0.85) / Math.max(1, l.w), (H * 0.85) / Math.max(1, l.h)));
    l = layout(canvas, dock, k);
  }
  const ink = custom(settings.iconColor) || dock.theme.text;
  const textInk = custom(settings.textColor) || dock.theme.text;
  const done = open() === 0 && list.length > 0;
  if (l.vertical) {
    const top = (H - l.h) / 2;
    box(canvas, (W - l.g) / 2, top, l.g, ink, done, dock.theme.accent);
    canvas.text(l.text, W / 2, top + l.g + 2, { ...l.style, align: 'center', baseline: 'top', color: textInk });
    return;
  }
  const x = (W - l.w) / 2;
  box(canvas, x, (H - l.g) / 2, l.g, ink, done, dock.theme.accent);
  canvas.text(l.text, x + l.g + l.gap, H / 2, { ...l.style, baseline: 'middle', color: textInk });
}

// --- the popup ---------------------------------------------------------------------------------------

const pages = () => Math.max(1, Math.ceil(list.length / PAGE));
function popupHeight() {
  const rows = Math.max(1, Math.min(PAGE, list.length));
  return 44 + rows * ROW_H + 50;
}

export function onClick(e) {
  if (e.closedPopup) return;
  page = 0;
  popup.open(POPUP_W, popupHeight());
}

function button(canvas, dock, label, x, y, what, enabled = true) {
  const ink = dock.theme.labelText;
  const w = Math.ceil(canvas.measureText(label, { size: 12.5 }).width) + 22;
  canvas.save();
  if (!enabled) canvas.opacity(0.45);
  canvas.fillRoundRect(x, y, w, 26, 6, '#20FFFFFF');
  canvas.strokeRoundRect(x + 0.5, y + 0.5, w - 1, 25, 6, '#30808080', 1);
  canvas.text(label, x + w / 2, y + 13, { size: 12.5, align: 'center', baseline: 'middle', color: ink });
  canvas.restore();
  if (enabled) hits.push({ x, y, w, h: 26, what });
  return w;
}

export function drawPopup(canvas, dock) {
  hits = [];
  const W = canvas.width, H = canvas.height, P = 14;
  const ink = dock.theme.labelText, dim = dock.theme.dateText;
  const title = settings.label || 'To-do';
  canvas.text(title, P, 12, { size: 14, weight: 'semibold', color: ink, maxWidth: W - 2 * P - 90 });
  canvas.text(`${open()} open`, W - P, 14, { size: 12, align: 'right', color: dim });
  const start = page * PAGE;
  const rows = list.slice(start, start + PAGE);
  if (!rows.length) canvas.text('Nothing to do. + Add puts a first item here.', P, 48, { size: 12.5, color: dim, maxWidth: W - 2 * P });
  rows.forEach((t, i) => {
    const y = 40 + i * ROW_H;
    box(canvas, P, y + 7, 16, ink, t.done, dock.theme.accent);
    hits.push({ x: P - 4, y, w: 26, h: ROW_H, what: 'toggle', id: t.id });
    canvas.save();
    if (t.done) canvas.opacity(0.55);
    canvas.text(t.text, P + 26, y + ROW_H / 2, { size: 13, baseline: 'middle', color: ink, maxWidth: W - 2 * P - 26 - 26 });
    canvas.restore();
    if (t.done) {
      const tw = Math.min(W - 2 * P - 52, canvas.measureText(t.text, { size: 13 }).width);
      canvas.line(P + 26, y + ROW_H / 2 + 0.5, P + 26 + tw, y + ROW_H / 2 + 0.5, dim, 1);
    }
    const xx = W - P - 9, xy = y + ROW_H / 2, s = 4;
    canvas.line(xx - s, xy - s, xx + s, xy + s, dim, { width: 1.3, cap: 'round' });
    canvas.line(xx + s, xy - s, xx - s, xy + s, dim, { width: 1.3, cap: 'round' });
    hits.push({ x: W - P - 22, y, w: 26, h: ROW_H, what: 'delete', id: t.id });
    hits.push({ x: P + 26, y, w: W - 2 * P - 52, h: ROW_H, what: 'toggle', id: t.id });  // the text toggles too
  });
  const by = H - 38;
  let x = P;
  x += button(canvas, dock, '+ Add', x, by, 'add', list.length < MAX_ITEMS) + 8;
  button(canvas, dock, 'Clear done', x, by, 'clear', list.some((t) => t.done));
  if (pages() > 1) {
    const label = `${page + 1} / ${pages()}`;
    canvas.text(label, W - P - 60, by + 13, { size: 12, align: 'center', baseline: 'middle', color: dim });
    canvas.text('‹', W - P - 92, by + 13, { size: 16, align: 'center', baseline: 'middle', color: page > 0 ? ink : dim });
    canvas.text('›', W - P - 28, by + 13, { size: 16, align: 'center', baseline: 'middle', color: page < pages() - 1 ? ink : dim });
    hits.push({ x: W - P - 106, y: by, w: 28, h: 26, what: 'prev' }, { x: W - P - 42, y: by, w: 28, h: 26, what: 'next' });
  }
}

async function add() {
  if (editing || dock.preview) return;
  editing = true;
  try {
    const text = await popup.editText({ title: 'New to-do', multiline: false, maxLength: MAX_TEXT, placeholder: 'What to do' });
    const t = text === null ? '' : text.trim();
    if (t && list.length < MAX_ITEMS) {
      const id = list.reduce((m, x) => Math.max(m, x.id), 0) + 1;
      list.push({ id, text: t, done: false });
      page = pages() - 1;
      changed();
    }
  } catch (e) {
    console.warn(e.message);
  } finally {
    editing = false;
  }
}

export function onPopupClick(e) {
  if (e.button !== 'left') return;
  const h = hits.find((t) => e.x >= t.x && e.x < t.x + t.w && e.y >= t.y && e.y < t.y + t.h);
  if (!h) return;
  if (h.what === 'prev' || h.what === 'next') {
    page = Math.max(0, Math.min(pages() - 1, page + (h.what === 'next' ? 1 : -1)));
    popup.open(POPUP_W, popupHeight());
    return;
  }
  if (h.what === 'add') { add(); return; }
  if (dock.preview) return;
  if (h.what === 'toggle') {
    const t = list.find((x) => x.id === h.id);
    if (t) t.done = !t.done;
  } else if (h.what === 'delete') {
    list = list.filter((x) => x.id !== h.id);
  } else if (h.what === 'clear') {
    list = list.filter((x) => !x.done);
  }
  changed();
}
