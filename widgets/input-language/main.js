// Input language (round 27 phase 27C): the taskbar's language indicator, from the `input` provider
// (input.get(): { available, id, tag, code, region, language, layout, current, layouts: [{ id, tag,
// code, region, language, layout, current }] }). The short code ("ENG") in the dock's text colour,
// the region under it when two layouts share a code (or always), optionally the full name. A click
// switches to the next layout (input.next(): Windows + Space) or opens the list; the list's rows
// select a layout (input.set(id)).
//
// Cost: nothing runs while the language does not change: the provider follows the shell's language
// and activation notices (quiet while this widget cannot be seen). After a click the widget reads the
// state again once, 400 ms later, in case Windows does not report the switch.

const ROW_H = 40, POPUP_W = 300, PAD = 8;

let lang = input.get();
let hits = [];
let recheck = 0;

input.onChange((s) => update(s));

function update(s) {
  if (s.id === lang.id && s.layouts.length === lang.layouts.length && s.layouts.every((l, i) => l.id === lang.layouts[i].id)) return;
  lang = s;
  requestRedraw();
  if (popup.isOpen) popup.redraw();
}

// One read shortly after our own switch (a one-shot after a click, never while idle).
function checkSoon() {
  if (recheck) clearTimeout(recheck);
  recheck = setTimeout(() => { recheck = 0; update(input.get()); }, 400);
}

export function onSettingsChanged() {
  requestRedraw();
}

const custom = (c) => (c && !/^#00[0-9a-f]{6}$/i.test(c) ? c : null);
const codeColor = (dock) => custom(settings.textColor) || dock.theme.text;
const nameColor = (dock) => custom(settings.nameColor) || dock.theme.dateText;
const hidden = () => settings.hideSingle && lang.layouts.length <= 1;

function showRegion() {
  if (!lang.region) return false;
  if (settings.region === 'always') return true;
  if (settings.region === 'never') return false;
  return lang.layouts.filter((l) => l.code === lang.code).length > 1;
}

// --- layout ----------------------------------------------------------------------------------------

function layout(canvas, dock, k = 1) {
  const vertical = dock.axis === 'vertical' && !dock.standalone;
  const icon = dock.standalone ? 48 : dock.iconSize;
  const s = (settings.scale || 1) * k;
  const font = settings.font || undefined;
  const code = { str: lang.code || '?', style: { size: Math.max(9, icon * 0.27) * s, weight: 'semibold', font } };
  const lines = [code];
  if (showRegion()) lines.push({ str: lang.region, style: { size: Math.max(8, icon * 0.2) * s, font }, dim: true });
  let blockW = 0, blockH = 0;
  for (const l of lines) {
    const m = canvas.measureText(l.str, l.style);
    l.w = Math.ceil(m.width);
    l.h = Math.ceil(m.height);
    blockW = Math.max(blockW, l.w);
    blockH += l.h;
  }
  let name = null;
  if (settings.showName && lang.language) {
    const style = { size: Math.max(8, icon * 0.21) * s, font };
    const m = canvas.measureText(lang.language, style);
    name = { str: lang.language, style, w: Math.ceil(m.width), h: Math.ceil(m.height) };
    if (vertical && name.w > icon) name = null;  // no room across a side dock
  }
  const gap = Math.round((vertical ? 2 : 6) * k), pad = Math.round(4 * k);
  const w = vertical ? Math.max(blockW, name ? name.w : 0) : blockW + (name ? gap + name.w : 0);
  const h = vertical ? blockH + (name ? gap + name.h : 0) : Math.max(blockH, name ? name.h : 0);
  return { vertical, icon, lines, blockW, blockH, name, gap, pad, w, h };
}

export function measure(canvas, dock) {
  const icon = Math.round(dock.iconSize);
  if (hidden() && !dock.standalone) return { main: 0, cross: icon };  // takes no room
  const l = layout(canvas, dock);
  return l.vertical ? { main: Math.ceil(l.h + 2 * l.pad + 4), cross: icon } : { main: Math.ceil(l.w + 2 * l.pad), cross: icon };
}

// --- drawing ---------------------------------------------------------------------------------------

function block(canvas, dock, cx, top, l) {
  let y = top;
  for (const line of l.lines) {
    canvas.text(line.str, cx, y, { ...line.style, align: 'center', color: line.dim ? nameColor(dock) : codeColor(dock) });
    y += line.h;
  }
}

export function draw(canvas, dock) {
  if (hidden() && !dock.standalone) return;
  let l = layout(canvas, dock);
  if (dock.standalone) {
    const k = Math.max(0.5, Math.min((canvas.width * 0.85) / Math.max(1, l.w), (canvas.height * 0.85) / Math.max(1, l.h)));
    l = layout(canvas, dock, k);
  }
  const W = canvas.width, H = canvas.height;
  if (l.vertical) {
    const top = Math.round((H - l.h) / 2);
    block(canvas, dock, W / 2, top, l);
    if (l.name) canvas.text(l.name.str, W / 2, top + l.blockH + l.gap, { ...l.name.style, align: 'center', color: nameColor(dock) });
    return;
  }
  const left = dock.standalone || !l.name ? Math.round((W - l.w) / 2) : l.pad;
  block(canvas, dock, left + l.blockW / 2, Math.round((H - l.blockH) / 2), l);
  if (l.name) canvas.text(l.name.str, left + l.blockW + l.gap, H / 2, { ...l.name.style, baseline: 'middle', color: nameColor(dock) });
}

// --- clicks and the popup --------------------------------------------------------------------------

function act(what) {
  if (what === 'next') {
    input.next();
    checkSoon();
  } else if (what === 'popup') {
    lang = input.get();
    requestRedraw();
    popup.open(POPUP_W, PAD * 2 + 22 + Math.max(1, lang.layouts.length) * ROW_H);
  }
}

export function onClick(e) {
  if (e.closedPopup) return;  // the press closed the popup: a click toggles it
  act(settings.click === 'popup' ? 'popup' : 'next');
}

export function onMiddleClick() {
  if (settings.middleClick !== 'none') act(settings.middleClick === 'next' ? 'next' : 'popup');
}

export function drawPopup(canvas, dock) {
  hits = [];
  const W = canvas.width;
  const ink = dock.theme.labelText, dim = dock.theme.dateText;
  canvas.text('Keyboard layouts', 14, PAD + 2, { size: 13, weight: 'semibold', color: ink });
  let y = PAD + 22;
  if (!lang.layouts.length) {
    canvas.text('No layouts were found.', 14, y + 10, { size: 12, color: dim });
    return;
  }
  for (const l of lang.layouts) {
    if (l.current) {
      canvas.fillRoundRect(6, y + 2, W - 12, ROW_H - 4, 6, '#20808080');
      canvas.fillRoundRect(8, y + 12, 3, ROW_H - 24, 1.5, dock.theme.accent);
    }
    canvas.fillRoundRect(18, y + 8, 44, ROW_H - 16, 4, '#18808080');
    canvas.text(l.code || '?', 40, y + ROW_H / 2, { size: 12, weight: 'semibold', align: 'center', baseline: 'middle', color: ink });
    canvas.text(l.language || l.tag, 72, y + 6, { size: 12.5, color: ink, maxWidth: W - 86 });
    canvas.text(l.layout ? l.layout + ' keyboard' : l.tag, 72, y + 22, { size: 11, color: dim, maxWidth: W - 86 });
    const row = { x: 0, y, w: W, h: ROW_H, id: l.id };
    hits.push(row);
    y += ROW_H;
  }
}

export function onPopupClick(e) {
  if (e.button !== 'left') return;
  const h = hits.find((t) => e.y >= t.y && e.y < t.y + t.h);
  if (!h) return;
  popup.close();
  try {
    input.set(h.id);
  } catch (err) {
    console.warn(String(err));  // e.g. an app running as administrator refused it
  }
  checkSoon();
}
