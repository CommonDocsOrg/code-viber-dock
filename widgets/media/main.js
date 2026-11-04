// Now playing: the system's current media session (the `media` provider): album art, title and
// artist. The controls (settings.controls, round 7 phase 7C): "hover" (previous / play-pause / next
// over the whole face, art and text alike, while the pointer is over it), "always" (the same, always,
// over a lighter shade) or "sides" (previous left of the art, next right of the text, or above /
// below the art on a left / right dock; play / pause over the art while hovered, else a small
// badge in its corner; the slot is two buttons longer). A click on a control sends it; a middle click
// opens a popup with a larger art, the album and the controls. A title or artist wider than the
// text column scrolls (settings.marqueeMode: "hover" while the pointer is over the widget, "always"
// while the dock is on screen, "off": it fades out at the edge) at settings.marqueeSpeed DIPs a
// second, in frames of 1000 / speed ms (33 - 100 ms: 10 - 30 frames a second), never with reduce
// motion or in a preview. The wheel changes the volume (a setting) and shows the Volume widget's bar
// beside the dock (a passive popup) until 1.2 s after the last notch.
// Event-driven: media.onChange redraws when the track, the playback state or the art change;
// nothing ticks except the marquee's frames while a line scrolls.

const POPUP_W = 320, POPUP_H = 140;
const BAR_W = 44, BAR_H = 150, BAR_MS = 1200;
const MARQUEE_GAP = 36;

let m = media.get();
let art = media.art();
let hovered = false;
let marquee = null;  // { start, timer } while a line scrolls
let hits = [];
let bar = false;  // the volume bar (the passive popup) is up
let barTimer = 0;

media.onChange((s) => {
  m = s;
  art = media.art();
  requestRedraw();
  if (popup.isOpen) popup.redraw();
});

const vertical = () => dock.axis === 'vertical' && !dock.standalone;
const textShown = () => settings.showText && !vertical();
const sides = () => settings.controls === 'sides';
const speed = () => Math.max(5, Math.min(120, settings.marqueeSpeed || 28));
const frameMs = () => Math.max(33, Math.min(100, Math.round(1000 / speed())));

// The tile's parts in a W x H box: the art square, the text column, the side buttons ("sides").
function geom(W, H) {
  if (vertical()) {
    const s = W, bw = sides() ? Math.round(s * 0.6) : 0;
    const ay = sides() ? bw : (H - s) / 2;
    return { s, bw, ax: 0, ay, tx: 0, tw: 0,
      prev: { x: 0, y: 0, w: W, h: bw }, next: { x: 0, y: ay + s, w: W, h: bw } };
  }
  const s = Math.min(W, H), bw = sides() ? Math.round(s * 0.6) : 0;
  const ax = bw, ay = (H - s) / 2;
  const tx = ax + s + 8;
  const tw = !textShown() ? 0 : dock.standalone ? Math.max(20, W - tx - bw - (bw ? 0 : 6)) : settings.textWidth;
  return { s, bw, ax, ay, tx, tw, prev: { x: 0, y: 0, w: bw, h: H }, next: { x: W - bw, y: 0, w: bw, h: H } };
}

export function measure(canvas, dock) {
  const s = Math.round(dock.iconSize);
  const extra = sides() ? 2 * Math.round(s * 0.6) : 0;
  return { main: (textShown() ? s + 8 + settings.textWidth : s) + extra, cross: s };
}

// --- glyphs -----------------------------------------------------------------------------------------

function note(canvas, cx, cy, g, color) {
  canvas.fillEllipse(cx - 0.2 * g, cy + 0.24 * g, 0.15 * g, 0.12 * g, color);
  canvas.fillEllipse(cx + 0.26 * g, cy + 0.14 * g, 0.15 * g, 0.12 * g, color);
  canvas.line(cx - 0.07 * g, cy + 0.24 * g, cx - 0.07 * g, cy - 0.32 * g, color, Math.max(1, 0.06 * g));
  canvas.line(cx + 0.39 * g, cy + 0.14 * g, cx + 0.39 * g, cy - 0.42 * g, color, Math.max(1, 0.06 * g));
  canvas.fill(canvas.path().moveTo(cx - 0.1 * g, cy - 0.34 * g).lineTo(cx + 0.42 * g, cy - 0.45 * g)
    .lineTo(cx + 0.42 * g, cy - 0.3 * g).lineTo(cx - 0.1 * g, cy - 0.19 * g).close(), color);
}

function playGlyph(canvas, cx, cy, g, color) {
  canvas.fill(canvas.path().moveTo(cx - 0.3 * g, cy - 0.4 * g).lineTo(cx + 0.42 * g, cy).lineTo(cx - 0.3 * g, cy + 0.4 * g).close(), color);
}
function pauseGlyph(canvas, cx, cy, g, color) {
  canvas.fillRoundRect(cx - 0.34 * g, cy - 0.38 * g, 0.24 * g, 0.76 * g, 0.05 * g, color);
  canvas.fillRoundRect(cx + 0.1 * g, cy - 0.38 * g, 0.24 * g, 0.76 * g, 0.05 * g, color);
}
function skipGlyph(canvas, cx, cy, g, color, forward) {
  const d = forward ? 1 : -1;
  canvas.fill(canvas.path().moveTo(cx - d * 0.36 * g, cy - 0.32 * g).lineTo(cx + d * 0.12 * g, cy)
    .lineTo(cx - d * 0.36 * g, cy + 0.32 * g).close(), color);
  canvas.fillRect(forward ? cx + 0.14 * g : cx - 0.26 * g, cy - 0.32 * g, 0.12 * g, 0.64 * g, color);
}
function control(canvas, kind, cx, cy, g, color) {
  if (kind === 'toggle') (m.status === 'playing' ? pauseGlyph : playGlyph)(canvas, cx, cy, g, color);
  else skipGlyph(canvas, cx, cy, g, color, kind === 'next');
}

// The art (or the note on a tile) in a rounded square.
function drawArt(canvas, x, y, s, dock, bare) {
  const r = s * 0.16;
  canvas.save();
  canvas.clipPath(canvas.path().moveTo(x + r, y).lineTo(x + s - r, y).arc(x + s - r, y + r, r, -Math.PI / 2, 0)
    .lineTo(x + s, y + s - r).arc(x + s - r, y + s - r, r, 0, Math.PI / 2).lineTo(x + r, y + s)
    .arc(x + r, y + s - r, r, Math.PI / 2, Math.PI).lineTo(x, y + r).arc(x + r, y + r, r, Math.PI, Math.PI * 1.5).close());
  let drawn = false;
  if (art) {
    try {
      canvas.image(art, x, y, s, s);
      drawn = true;
    } catch (e) {
      art = null;  // gone since (a newer track): the next change brings the new one
    }
  }
  if (!drawn) {
    canvas.fillRect(x, y, s, s, canvas.linearGradient(x, y, x + s, y + s, [[0, '#FF3A4A6A'], [1, '#FF1F2638']]));
    if (!bare) note(canvas, x + s / 2, y + s / 2, s * 0.55, m.session ? "#E6FFFFFF" : "#80FFFFFF");
  }
  canvas.restore();
  canvas.strokeRoundRect(x + 0.5, y + 0.5, s - 1, s - 1, r, '#30000000', 1);
}

// --- the tile --------------------------------------------------------------------------------------

const titleStyle = { size: 12.5, weight: 'semibold' };
const artistStyle = { size: 11 };

const titleText = () => m.title || '(no title)';
const artistText = () => m.artist || m.album || '';

// A line of the text column is wider than the column (the title, the artist).
function overflow(canvas, g) {
  if (!textShown() || !m.session || g.tw <= 0) return false;
  return canvas.measureText(titleText(), titleStyle).width > g.tw || canvas.measureText(artistText(), artistStyle).width > g.tw;
}

function stopMarquee() {
  if (!marquee) return;
  clearInterval(marquee.timer);
  marquee = null;
}

// Frames only while a line overflows, the mode wants them (hovered, or always) and the dock is on
// screen; never with reduce motion or in a preview. A changed speed restarts the frames.
function updateMarquee(canvas, g) {
  const mode = settings.marqueeMode || 'hover';
  const want = (mode === 'always' || (mode === 'hover' && hovered)) && dock.visible && !dock.reduceMotion &&
               !dock.preview && overflow(canvas, g);
  if (want && marquee && marquee.ms !== frameMs()) stopMarquee();
  if (want && !marquee) {
    marquee = { start: Date.now(), ms: frameMs(), timer: setInterval(() => requestRedraw(), frameMs()) };
  } else if (!want) {
    stopMarquee();
  }
}

function lineText(canvas, text, x, y, w, style, color, scroll) {
  canvas.save();
  canvas.clip(x, y - 1, w, style.size * 1.5);
  const tw = canvas.measureText(text, style).width;
  if (scroll && tw > w) {
    const off = (((Date.now() - scroll.start) / 1000) * speed()) % (tw + MARQUEE_GAP);
    canvas.text(text, x - off, y, { ...style, color });
    canvas.text(text, x - off + tw + MARQUEE_GAP, y, { ...style, color });
  } else {
    canvas.text(text, x, y, { ...style, color });
    if (tw > w) {  // fade out at the edge
      canvas.fillRect(x + w - 18, y - 1, 18, style.size * 1.5,
        canvas.linearGradient(x + w - 18, 0, x + w, 0, [[0, '#00000000'], [1, dock.theme.background]]));
    }
  }
  canvas.restore();
}

export function draw(canvas, dock) {
  const g = geom(canvas.width, canvas.height);
  const mode = settings.controls || 'hover';
  const over = m.session && (mode === 'always' || (mode === 'hover' && hovered));  // the three over the face
  const onArt = m.session && mode === 'sides' && hovered;  // play / pause over the art
  drawArt(canvas, g.ax, g.ay, g.s, dock, over || onArt);
  if (m.session && !over && !onArt) {
    if (mode === 'sides') badge(canvas, g, m.status === 'playing' ? pauseGlyph : playGlyph);
    else if (m.status !== 'playing' && !textShown()) badge(canvas, g, pauseGlyph);
  }
  updateMarquee(canvas, g);
  if (textShown()) drawText(canvas, dock, g);
  if (mode === 'sides') drawSides(canvas, dock, g, onArt);
  else if (over) drawControls(canvas, g, hovered || mode === 'hover' ? '#B3000000' : '#80000000');
}

// A small round badge in the art's bottom-right corner.
function badge(canvas, g, glyph) {
  const r = Math.max(6, g.s * 0.15), cx = g.ax + g.s - r - 2, cy = g.ay + g.s - r - 2;
  canvas.fillCircle(cx, cy, r, '#C0000000');
  glyph(canvas, cx, cy, r, '#FFFFFFFF');
}

function drawText(canvas, dock, g) {
  const ink = dock.theme.text, dim = dock.theme.dateText;
  if (!m.session) {
    canvas.save();
    canvas.clip(g.tx, 0, g.tw, canvas.height);
    canvas.text('Nothing playing', g.tx, canvas.height / 2, { size: 12, baseline: 'middle', color: dim });
    canvas.restore();
    return;
  }
  const alpha = m.status === 'playing' ? 1 : 0.7;
  canvas.save();
  canvas.opacity(alpha);
  const top = canvas.height / 2 - 16;
  lineText(canvas, titleText(), g.tx, top, g.tw, titleStyle, ink, marquee);
  lineText(canvas, artistText(), g.tx, top + 18, g.tw, artistStyle, dim, marquee);
  canvas.restore();
}

// The three controls over the whole widget (art and text alike): a dark rounded panel across the
// face, the buttons spread over its width, each the height of the tile.
function drawControls(canvas, g, shade) {
  const full = textShown();
  const x = full ? 0 : g.ax, y = full ? 0 : g.ay;
  const w = full ? canvas.width : g.s, h = full ? canvas.height : g.s;
  canvas.fillRoundRect(x, y, w, h, g.s * 0.16, shade);
  const gs = Math.min(g.s * 0.5, w / 5);
  const cy = y + h / 2;
  control(canvas, 'previous', x + w / 6, cy, gs * 0.8, m.canPrevious ? '#FFFFFFFF' : '#60FFFFFF');
  control(canvas, 'toggle', x + w / 2, cy, gs, '#FFFFFFFF');
  control(canvas, 'next', x + (w * 5) / 6, cy, gs * 0.8, m.canNext ? '#FFFFFFFF' : '#60FFFFFF');
}

// "sides": previous and next beside the face (drawn without a session too, dimmed: the slot keeps
// its shape), play / pause over the art while hovered.
function drawSides(canvas, dock, g, onArt) {
  const ink = dock.theme.text;
  const dimmed = '#59' + ink.slice(3);  // the theme's text at 35 %
  const gs = Math.min(g.bw, g.s) * 0.5;
  for (const [kind, r, on] of [['previous', g.prev, m.session && m.canPrevious], ['next', g.next, m.session && m.canNext]]) {
    if (hovered && on) canvas.fillRoundRect(r.x + 2, r.y + 2, r.w - 4, r.h - 4, 6, '#18FFFFFF');
    control(canvas, kind, r.x + r.w / 2, r.y + r.h / 2, gs, on ? ink : dimmed);
  }
  if (onArt) {
    canvas.fillRoundRect(g.ax, g.ay, g.s, g.s, g.s * 0.16, '#B3000000');
    control(canvas, 'toggle', g.ax + g.s / 2, g.ay + g.s / 2, g.s * 0.42, '#FFFFFFFF');
  }
}

const inside = (e, r) => r.w > 0 && r.h > 0 && e.x >= r.x && e.x < r.x + r.w && e.y >= r.y && e.y < r.y + r.h;

export function onHover(e) {
  hovered = e.inside;
  requestRedraw();  // draw() starts / stops the marquee for the new state
}

export function onVisibilityChange(e) {
  if (!e.visible) stopMarquee();
  else requestRedraw();  // back on screen: draw() starts an "always" marquee again
}

export function onSettingsChanged() {
  stopMarquee();
  requestRedraw();
}

function send(kind) {
  if (kind === 'toggle') media.toggle();
  else if (kind === 'next') media.next();
  else media.previous();
}

export function onClick(e) {
  if (!m.session || e.closedPopup) return;  // the press closed the popup: that is the click's job
  const g = geom(dock.width, dock.height);
  if (sides()) {
    send(inside(e, g.prev) ? 'previous' : inside(e, g.next) ? 'next' : 'toggle');
    return;
  }
  const w = dock.standalone ? dock.width : textShown() ? g.s + 8 + settings.textWidth : g.s;
  send(e.x < w / 3 ? 'previous' : e.x > (w * 2) / 3 ? 'next' : 'toggle');
}

export function onMiddleClick() {
  if (!m.session) return;
  endBar();  // the ordinary popup replaces the bar (the host closes it)
  popup.open(POPUP_W, POPUP_H);  // the press already closed an open popup
}

// 2 % a notch on the 2 % grid, unmuting on the way up (as the Volume widget does).
export function onWheel(e) {
  if (!settings.wheelVolume) return false;
  const a = audio.get();
  if (!a.available) return false;
  const step = 0.02;
  const base = Math.round(a.volume / step) * step;
  audio.setVolume(Math.min(1, Math.max(0, base + Math.sign(e.notches) * Math.max(1, Math.round(Math.abs(e.notches))) * step)));
  if (a.muted && e.notches > 0) audio.setMuted(false);
  if (popup.isOpen) popup.redraw();  // the now-playing popup is open: no bar over it
  else showBar();
  return true;
}

// --- the volume bar (a passive popup) -------------------------------------------------------------

function endBar() {
  if (barTimer) clearTimeout(barTimer);
  barTimer = 0;
  bar = false;
}

// Opens the bar (or redraws it in place) and starts its 1.2 s again.
function showBar() {
  bar = true;
  popup.open(BAR_W, BAR_H, { passive: true });
  if (barTimer) clearTimeout(barTimer);
  barTimer = setTimeout(() => {
    barTimer = 0;
    if (bar) popup.close();
  }, BAR_MS);
}

export function onPopupClose(e) {
  if (e.passive) endBar();
}

// A speaker in a g x g box at (x, y): waves 0 - 3, or a cross (the Volume widget's glyph).
function speaker(canvas, x, y, g, waves, muted, color) {
  canvas.fill(canvas.path().moveTo(x + 0.06 * g, y + 0.36 * g).lineTo(x + 0.24 * g, y + 0.36 * g)
    .lineTo(x + 0.48 * g, y + 0.14 * g).lineTo(x + 0.48 * g, y + 0.86 * g).lineTo(x + 0.24 * g, y + 0.64 * g)
    .lineTo(x + 0.06 * g, y + 0.64 * g).close(), color);
  const st = { width: Math.max(1.2, g * 0.075), cap: 'round' };
  if (muted) {
    canvas.line(x + 0.62 * g, y + 0.38 * g, x + 0.88 * g, y + 0.62 * g, color, st);
    canvas.line(x + 0.88 * g, y + 0.38 * g, x + 0.62 * g, y + 0.62 * g, color, st);
    return;
  }
  const cx = x + 0.48 * g, cy = y + 0.5 * g;
  for (let i = 1; i <= 3; i++) {
    const r = 0.13 * g * i + 0.04 * g;
    canvas.stroke(canvas.path().arc(cx, cy, r, -0.85, 0.85), i <= waves ? color : '#30808080', st);
  }
}

// The level on top, a vertical track filled from the bottom with a knob, the speaker under it.
function drawBar(canvas, dock) {
  const W = canvas.width, H = canvas.height;
  const ink = dock.theme.labelText;
  const a = audio.get();
  const v = a.volume || 0, off = a.muted || !a.available;
  canvas.text(off ? 'Muted' : Math.round(v * 100) + '%', W / 2, 9,
    { size: 11, weight: 'semibold', align: 'center', color: ink });
  const tw = 8, tx = (W - tw) / 2, t0 = 32, t1 = H - 38;
  canvas.fillRoundRect(tx, t0, tw, t1 - t0, tw / 2, '#40808080');
  const ky = t1 - (t1 - t0) * v;
  canvas.fillRoundRect(tx, ky, tw, Math.max(tw, t1 - ky), tw / 2, off ? '#80808080' : dock.theme.accent);
  canvas.fillCircle(W / 2, ky, 6, '#FFFFFFFF');
  canvas.strokeCircle(W / 2, ky, 6, '#30000000', 1);
  const waves = v <= 0.005 ? 0 : v < 0.34 ? 1 : v < 0.67 ? 2 : 3;
  speaker(canvas, (W - 20) / 2, H - 28, 20, waves, off, ink);
}

// --- the popup ------------------------------------------------------------------------------------

const clock = (t) => (t == null ? '' : Math.floor(t / 60) + ':' + String(Math.floor(t % 60)).padStart(2, '0'));

export function drawPopup(canvas, dock) {
  if (bar && canvas.width === BAR_W && canvas.height === BAR_H) {
    drawBar(canvas, dock);
    return;
  }
  hits = [];
  const ink = dock.theme.labelText, dim = dock.theme.dateText;
  drawArt(canvas, 16, 16, 100, dock);
  const x = 132, w = POPUP_W - x - 16;
  canvas.text(m.title || '(no title)', x, 18, { size: 14, weight: 'semibold', maxWidth: w, color: ink });
  canvas.text(m.artist || '', x, 40, { size: 12, maxWidth: w, color: dim });
  canvas.text(m.album || '', x, 58, { size: 11, maxWidth: w, color: dim });
  const buttons = [['previous', m.canPrevious], ['toggle', true], ['next', m.canNext]];
  buttons.forEach(([kind, on], i) => {
    const bx = x + i * 46, by = 96;
    canvas.fillRoundRect(bx, by, 38, 32, 6, '#20FFFFFF');
    control(canvas, kind, bx + 19, by + 16, kind === 'toggle' ? 15 : 12, on ? ink : '#60808080');
    hits.push({ x: bx, y: by, w: 38, h: 32, kind });
  });
  if (m.duration) {
    canvas.text(clock(m.position) + " / " + clock(m.duration), x, 80,
      { size: 11, baseline: 'middle', color: dim });
  }
}

export function onPopupClick(e) {
  for (const h of hits) {
    if (e.x >= h.x && e.x < h.x + h.w && e.y >= h.y && e.y < h.y + h.h) send(h.kind);
  }
}
