// Battery (round 7 phase 7C): the battery's level from the `perf` provider (battery.percent,
// battery.charging, battery.acLine, battery.timeLeft: GetSystemPowerStatus). A battery outline filled
// to the level (the accent colour, or settings.fillColor; red at or below settings.lowPercent), a
// bolt over it while charging, the percentage beside it (a top / bottom dock) or under it (a left /
// right dock) and, optionally, the time left. A PC without a battery (the keys are absent) shows a
// crossed battery and "No battery", or takes no room at all (hideWithoutBattery: measure() returns
// main 0). A click opens a popup with the level, the state, the time left and a button for Windows'
// Power & battery settings.
//
// Cost: the sampler runs every 10 s for this widget (perf.setInterval(10000); a faster widget of
// another kind speeds it up for every widget), and the widget redraws only when what it shows
// changed: a steady battery costs one comparison per sample and no draw.

const POPUP_W = 260, POPUP_H = 120;
const LOW = '#FFE81123';

const has = (v) => v !== undefined && v !== null;

// What the widget shows, from a perf snapshot.
function read(snap) {
  const m = snap.metrics || {};
  if (!has(m['battery.percent'])) return { known: snap.serial > 0, present: false };
  return {
    known: true,
    present: true,
    percent: Math.max(0, Math.min(100, Math.round(m['battery.percent']))),
    charging: m['battery.charging'] === 1,
    ac: has(m['battery.acLine']) ? m['battery.acLine'] : null,
    left: has(m['battery.timeLeft']) ? m['battery.timeLeft'] : null,
  };
}

let bat = read(perf.get());
let hits = [];

perf.setInterval(10000);
perf.onChange((snap) => {
  const b = read(snap);
  if (same(b, bat)) return;
  bat = b;
  requestRedraw();  // re-measures too (the text's width may change)
  if (popup.isOpen) popup.redraw();
});

// The time left matters only while it is shown (the setting, or the popup).
function same(a, b) {
  if (a.known !== b.known || a.present !== b.present) return false;
  if (!a.present) return true;
  if (a.percent !== b.percent || a.charging !== b.charging || a.ac !== b.ac) return false;
  return !(settings.showTimeLeft || popup.isOpen) || a.left === b.left;
}

export function onSettingsChanged() {
  bat = read(perf.get());
  requestRedraw();
}

// --- text ------------------------------------------------------------------------------------------

function leftText(sec) {
  const h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60);
  return h > 0 ? `${h} h ${m} m` : `${m} m`;
}
const leftKnown = (b) => b.present && !b.charging && b.ac !== 1 && has(b.left);

function stateText(b) {
  if (b.charging) return 'Charging';
  if (b.ac === 1) return b.percent >= 100 ? 'Full' : 'Plugged in, not charging';
  return 'On battery';
}

const fillColor = (dock) => {
  const c = settings.fillColor || '';
  return /^#00[0-9a-f]{6}$/i.test(c) || !c ? dock.theme.accent : c;
};
const levelColor = (b, dock) => (b.percent <= settings.lowPercent ? LOW : fillColor(dock));

// --- layout ----------------------------------------------------------------------------------------

// The parts at scale k: the glyph (body bw x bh plus the nub), the text lines, the block's size.
function layout(canvas, dock, k = 1) {
  const vertical = dock.axis === 'vertical' && !dock.standalone;
  const icon = dock.standalone ? 48 : dock.iconSize;
  const bw = Math.round(icon * (vertical ? 0.62 : 0.56) * k);
  const bh = Math.round(bw * 0.52);
  const nub = Math.max(2, Math.round(bw * 0.09));
  const gw = bw + nub;
  const lines = [];
  if (bat.present) {
    if (settings.showPercent) lines.push({ text: bat.percent + '%', style: { size: Math.max(9, icon * 0.29) * k, weight: 'semibold' }, dim: false });
    if (settings.showTimeLeft && leftKnown(bat)) lines.push({ text: leftText(bat.left), style: { size: Math.max(8, icon * 0.21) * k }, dim: true });
  } else if (bat.known) {
    lines.push({ text: vertical ? 'No\nbattery' : 'No battery', style: { size: Math.max(8, icon * 0.22) * k, align: vertical ? 'center' : 'left' }, dim: true });
  }
  let textW = 0, textH = 0;
  for (const l of lines) {
    const m = canvas.measureText(l.text, l.style);
    l.w = Math.ceil(m.width);
    l.h = Math.ceil(m.height);
    textW = Math.max(textW, l.w);
    textH += l.h;
  }
  const pad = Math.round(3 * k), gap = Math.round((vertical ? 2 : 5) * k);
  const w = vertical ? Math.max(gw, textW) : gw + (lines.length ? gap + textW : 0);
  const h = vertical ? bh + (lines.length ? gap + textH : 0) : Math.max(bh, textH);
  return { vertical, icon, bw, bh, nub, gw, lines, textW, textH, pad, gap, w, h, k };
}

export function measure(canvas, dock) {
  const icon = Math.round(dock.iconSize);
  if (settings.hideWithoutBattery && !bat.present) return { main: 0, cross: icon };  // takes no room
  const l = layout(canvas, dock);
  return l.vertical ? { main: Math.ceil(l.h + 2 * l.pad + 4), cross: icon } : { main: Math.ceil(l.w + 2 * l.pad), cross: icon };
}

// --- drawing ---------------------------------------------------------------------------------------

function bolt(canvas, cx, cy, h) {
  const w = h * 0.72, x0 = cx - w / 2, y0 = cy - h / 2;
  const p = (x, y) => [x0 + x * w, y0 + y * h];
  const pts = [p(0.62, 0), p(0.12, 0.58), p(0.46, 0.58), p(0.36, 1), p(0.88, 0.4), p(0.54, 0.4), p(0.7, 0)];
  const path = canvas.path().moveTo(pts[0][0], pts[0][1]);
  for (let i = 1; i < pts.length; i++) path.lineTo(pts[i][0], pts[i][1]);
  path.close();
  canvas.fill(path, '#FFFFFFFF');
  canvas.stroke(path, '#C0000000', { width: Math.max(0.7, h * 0.06), join: 'round' });
}

function glyph(canvas, dock, x, y, l) {
  const { bw, bh, nub } = l;
  const ink = dock.theme.text;
  const sw = Math.max(1, Math.round(bh * 0.1 * 2) / 2);
  const r = Math.max(1.5, bh * 0.22);
  canvas.save();
  if (!bat.present) canvas.opacity(0.55);
  canvas.strokeRoundRect(x + sw / 2, y + sw / 2, bw - sw, bh - sw, r, ink, sw);
  const nh = Math.round(bh * 0.42);
  canvas.fillRoundRect(x + bw - sw / 2, y + (bh - nh) / 2, nub + sw / 2, nh, Math.min(nub, nh) * 0.4, ink);
  canvas.restore();
  if (bat.present) {
    const inset = sw + Math.max(1, bh * 0.08);
    const fw = (bw - 2 * inset) * (bat.percent / 100);
    if (fw > 0.3) canvas.fillRoundRect(x + inset, y + inset, Math.max(fw, 1), bh - 2 * inset, Math.max(0.5, r - inset), levelColor(bat, dock));
    if (bat.charging) bolt(canvas, x + bw / 2, y + bh / 2, bh * 0.92);
  } else if (bat.known) {
    // No battery: crossed out.
    canvas.line(x - 1, y + bh + 2, x + bw + nub + 1, y - 2, ink, { width: sw * 1.2, cap: 'round' });
  }
}

function texts(canvas, dock, x, y, l, centre) {
  let ty = y;
  for (const line of l.lines) {
    const color = line.dim ? dock.theme.dateText : dock.theme.text;
    canvas.text(line.text, x, ty, { ...line.style, align: centre ? 'center' : 'left', color });
    ty += line.h;
  }
}

// Standalone (a window of its own): the dock layout grown to fill the window.
function fitLayout(canvas, dock) {
  const l = layout(canvas, dock);
  if (!dock.standalone) return l;
  const k = Math.max(0.5, Math.min((canvas.width * 0.85) / Math.max(1, l.w), (canvas.height * 0.85) / Math.max(1, l.h)));
  return layout(canvas, dock, k);
}

export function draw(canvas, dock) {
  if (settings.hideWithoutBattery && !bat.present && !dock.standalone) return;
  const l = fitLayout(canvas, dock);
  const W = canvas.width, H = canvas.height;
  if (l.vertical) {
    const top = Math.round((H - l.h) / 2);
    glyph(canvas, dock, Math.round((W - l.gw) / 2), top, l);
    texts(canvas, dock, W / 2, top + l.bh + l.gap, l, true);
    return;
  }
  const left = dock.standalone ? Math.round((W - l.w) / 2) : l.pad;
  glyph(canvas, dock, left, Math.round((H - l.bh) / 2), l);
  if (l.lines.length) texts(canvas, dock, left + l.gw + l.gap, Math.round((H - l.textH) / 2), l, false);
}

// --- the popup -------------------------------------------------------------------------------------

export function onClick(e) {
  if (e.closedPopup) return;  // the press closed the popup: a click toggles it
  bat = read(perf.get());
  requestRedraw();
  popup.open(POPUP_W, POPUP_H);
}

export function drawPopup(canvas, dock) {
  hits = [];
  const W = canvas.width;
  const ink = dock.theme.labelText, dim = dock.theme.dateText;
  canvas.text('Battery', 16, 14, { size: 14, weight: 'semibold', color: ink });
  if (bat.present) {
    canvas.text(bat.percent + '%', W - 16, 14, { size: 14, weight: 'semibold', align: 'right', color: ink });
    const x0 = 16, x1 = W - 16, by = 42, bh = 10;
    canvas.fillRoundRect(x0, by, x1 - x0, bh, bh / 2, '#40808080');
    const fw = (x1 - x0) * (bat.percent / 100);
    if (fw > 0) canvas.fillRoundRect(x0, by, Math.max(bh, fw), bh, bh / 2, levelColor(bat, dock));
    canvas.text(stateText(bat), 16, 60, { size: 12, color: ink });
    if (leftKnown(bat)) canvas.text(leftText(bat.left) + ' left', W - 16, 60, { size: 12, align: 'right', color: dim });
  } else {
    canvas.text(bat.known ? 'No battery: this PC runs on mains power.' : 'Reading the battery…', 16, 44,
      { size: 12, maxWidth: W - 32, color: dim });
  }
  const label = 'Power & battery settings';
  const bw = Math.ceil(canvas.measureText(label, { size: 12.5 }).width) + 28;
  const b = { x: W - 16 - bw, y: 84, w: bw, h: 26 };
  canvas.fillRoundRect(b.x, b.y, b.w, b.h, 6, '#20FFFFFF');
  canvas.strokeRoundRect(b.x + 0.5, b.y + 0.5, b.w - 1, b.h - 1, 6, '#30808080', 1);
  canvas.text(label, b.x + b.w / 2, b.y + b.h / 2, { size: 12.5, align: 'center', baseline: 'middle', color: ink });
  hits.push({ ...b, act: () => { shell.open('ms-settings:batterysaver'); popup.close(); } });
}

export function onPopupClick(e) {
  if (e.button !== 'left') return;
  const h = hits.find((t) => e.x >= t.x && e.x < t.x + t.w && e.y >= t.y && e.y < t.y + t.h);
  if (h) h.act();
}
