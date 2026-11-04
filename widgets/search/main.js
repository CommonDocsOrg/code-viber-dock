// Search: opens Windows Search with Win+S (shell.sendKeys('win+s'), SendInput). A magnifier glyph
// (or settings.image), optionally a "Search" label beside it on a horizontal dock. Idle: nothing runs.

let hovered = false;
let warned = false;

const labelShown = () => settings.showLabel && dock.axis !== 'vertical' && settings.label;
const labelStyle = { size: 12.5, weight: 'semibold' };

export function measure(canvas, dock) {
  const s = Math.round(dock.iconSize);
  const extra = labelShown() ? Math.ceil(canvas.measureText(settings.label, labelStyle).width) + 10 : 0;
  return { main: s + extra, cross: s };
}

function magnifier(canvas, cx, cy, g, color) {
  const r = g * 0.3, w = Math.max(1.5, g * 0.1);
  canvas.strokeCircle(cx - g * 0.08, cy - g * 0.08, r, color, w);
  const k = (r + w / 2) / Math.SQRT2;
  canvas.line(cx - g * 0.08 + k, cy - g * 0.08 + k, cx + g * 0.36, cy + g * 0.36, color, { width: w * 1.25, cap: 'round' });
}

function image(canvas, x, y, g) {
  const p = String(settings.image || '');
  if (!p) return false;
  try {
    if (/^[A-Za-z]:|^\\\\|^%/.test(p)) canvas.fileIcon(p, x, y, g);
    else canvas.image(p, x, y, g, g);
    return true;
  } catch (e) {
    if (!warned) console.warn('icon: ' + e.message);
    warned = true;
    return false;
  }
}

export function draw(canvas, dock) {
  const h = canvas.height, s = Math.min(h, canvas.width), g = s * 0.56;
  // Without a label the glyph is centred (a standalone window may be wider than tall).
  const ox = labelShown() ? 0 : (canvas.width - s) / 2;
  if (hovered) canvas.fillRoundRect(2, 2, canvas.width - 4, h - 4, s * 0.22, '#26FFFFFF');
  if (!image(canvas, ox + (s - g) / 2, (h - g) / 2, g)) magnifier(canvas, ox + s / 2, h / 2, g, dock.theme.text);
  if (labelShown()) canvas.text(settings.label, s + 2, h / 2, { ...labelStyle, baseline: 'middle', color: dock.theme.text });
}

export function onSettingsChanged() {
  warned = false;
}

export function onHover(e) {
  hovered = e.inside;
  requestRedraw();
}

export function onClick() {
  shell.sendKeys('win+s');
}
