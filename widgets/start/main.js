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

function logo(canvas, cx, cy, g, mono, dock) {
  const gap = g * 0.06, q = (g - gap) / 2, x = cx - g / 2, y = cy - g / 2;
  const paint = mono ? dock.theme.text
    : canvas.linearGradient(x, y, x + g, y + g, [[0, '#FF3CB4FF'], [1, '#FF0063D1']]);
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
