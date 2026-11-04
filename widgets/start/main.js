// Start: a button that opens the Start menu. The click sends the Windows key
// (shell.sendKeys('win'), SendInput: the same as pressing it, so Start opens and closes as the key
// does); if Windows refuses the injected input (another program's window of a higher integrity
// level has the focus), the All apps folder (shell:AppsFolder) opens instead. Idle: nothing runs.

let hovered = false;
let warned = false;

export function measure(canvas, dock) {
  const s = Math.round(dock.iconSize);
  return { main: s, cross: s };
}

// The four tiles snapped to device pixels (canvas.scale px per DIP): the tile size and the gap are
// whole pixels and the logo's corner lands on a pixel, centred on (cx, cy) to the nearest pixel, so
// the tiles' straight edges are crisp (at fractional DIPs they were anti-aliased: a blurred look).
function logo(canvas, cx, cy, g, mono, dock) {
  const k = canvas.scale > 0 ? canvas.scale : 1;
  const gapPx = Math.max(1, Math.round(g * 0.06 * k));
  const qPx = Math.max(1, Math.round((g * k - gapPx) / 2));
  const sizePx = 2 * qPx + gapPx;
  const x = Math.round(cx * k - sizePx / 2) / k, y = Math.round(cy * k - sizePx / 2) / k;
  const q = qPx / k, gap = gapPx / k, size = sizePx / k;
  const paint = mono ? dock.theme.text
    : canvas.linearGradient(x, y, x + size, y + size, [[0, '#FF3CB4FF'], [1, '#FF0063D1']]);
  canvas.fillRoundRect(x, y, q, q, q * 0.08, paint);
  canvas.fillRoundRect(x + q + gap, y, q, q, q * 0.08, paint);
  canvas.fillRoundRect(x, y + q + gap, q, q, q * 0.08, paint);
  canvas.fillRoundRect(x + q + gap, y + q + gap, q, q, q * 0.08, paint);
}

// settings.image: a package file or an absolute path (its shell icon); false if it cannot be drawn.
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
  const w = canvas.width, h = canvas.height, g = Math.min(w, h) * (settings.size || 62) / 100;
  if (hovered) canvas.fillRoundRect(2, 2, w - 4, h - 4, Math.min(w, h) * 0.22, '#26FFFFFF');
  if (settings.icon === 'image' && image(canvas, (w - g) / 2, (h - g) / 2, g)) return;
  logo(canvas, w / 2, h / 2, g * 0.86, settings.icon === 'mono', dock);
}

export function onSettingsChanged() {
  warned = false;
}

export function onHover(e) {
  hovered = e.inside;
  requestRedraw();
}

export function onClick() {
  try {
    shell.sendKeys('win');
  } catch (e) {
    console.warn(e.message + ': opening All apps instead');
    shell.open('shell:AppsFolder');
  }
}
