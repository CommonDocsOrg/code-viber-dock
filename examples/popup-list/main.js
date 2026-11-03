// A list in a script popup (the pattern of the dock's ItemGridView list mode, drawn with the
// canvas): a click on the widget toggles a popup, drawPopup() draws one row per link with the
// shell's icon (canvas.fileIcon) and its name, onPopupClick() hit-tests the rows it drew and opens
// the row's target (shell.open). The "shell" permission covers fileIcon and open.

const PAD = 8;
const W = 280;

// "Name = target; Name = target" -> [{ name, target }]
function links() {
  return String(settings.links || '')
    .split(';')
    .map((part) => {
      const at = part.indexOf('=');
      const name = (at < 0 ? part : part.slice(0, at)).trim();
      const target = (at < 0 ? part : part.slice(at + 1)).trim();
      return { name, target };
    })
    .filter((l) => l.name && l.target)
    .slice(0, 20);
}

const rowH = () => Math.max(settings.iconSize || 20, 16) + 10;
let rows = [];  // the rows of the last drawPopup: { y, h, link }

export function draw(canvas, dock) {
  // A simple "list" glyph: three lines with dots.
  const cx = canvas.width / 2, cy = canvas.height / 2;
  for (let i = -1; i <= 1; i++) {
    canvas.fillCircle(cx - 9, cy + i * 7, 1.8, dock.theme.accent);
    canvas.line(cx - 4, cy + i * 7, cx + 10, cy + i * 7, dock.theme.text, { width: 2, cap: 'round' });
  }
}

export function onClick(e) {
  if (e.closedPopup) return;  // this press closed the popup: a click toggles it
  const n = links().length;
  popup.open(W, PAD * 2 + Math.max(1, n) * rowH() + 22);
}

export function drawPopup(canvas, dock) {
  const list = links();
  const h = rowH(), icon = settings.iconSize || 20;
  canvas.text('Quick links', PAD + 4, PAD, { size: 12, weight: 'semibold', color: dock.theme.dateText });
  rows = [];
  let y = PAD + 22;
  if (!list.length) {
    canvas.text('No links: set them on Settings > Widget settings.', PAD + 4, y + h / 2, { size: 12, baseline: 'middle', color: dock.theme.text });
  }
  list.forEach((link, i) => {
    if (i % 2 === 1) canvas.fillRoundRect(PAD, y, W - 2 * PAD, h, 4, '#0CFFFFFF');  // zebra rows
    try {
      canvas.fileIcon(link.target, PAD + 6, y + (h - icon) / 2, icon);
    } catch (err) {
      // Not a file or folder (a URL, a typo): a dot instead of the icon.
      canvas.fillCircle(PAD + 6 + icon / 2, y + h / 2, 3, dock.theme.accent);
    }
    canvas.save();
    canvas.clip(PAD + icon + 14, y, W - 2 * PAD - icon - 18, h);  // long names are cut at the row's end
    canvas.text(link.name, PAD + icon + 14, y + h / 2, { size: 13, baseline: 'middle', color: dock.theme.text });
    canvas.restore();
    rows.push({ y, h, link });
    y += h;
  });
}

export function onPopupClick(e) {
  if (e.button !== 'left') return;
  const row = rows.find((r) => e.y >= r.y && e.y < r.y + r.h && e.x >= PAD && e.x < W - PAD);
  if (!row) return;
  try {
    shell.open(row.link.target);
  } catch (err) {
    console.error('cannot open', row.link.target, err.message);
  }
  if (settings.close) popup.close();
}
