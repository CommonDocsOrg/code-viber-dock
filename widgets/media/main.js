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
//
// Round 23 phase 23B: the desktop mini players. settings.layout "deck", "capsule", "cover" or
// "disc" draws a mini player instead of the tile while the widget is standalone (on a dock it stays
// the tile), in settings.skin "theme" (the dock theme's colours), "silver" or "night", scaled as a
// whole to the window (the largest fit, centred; the Deck's display column and the Capsule's pill
// take extra width). The transport buttons, the progress bar (the Disc: its rim ring) and the
// volume slider claim their presses (onPress returns true): the host then sends the held button's
// moves to onDrag and its end to onRelease, so they work as buttons and sliders and never move the
// window; a press anywhere else moves it as before. A button acts on release over it; the progress
// bar previews while dragged and seeks on release (media.seek, only when the session can seek); the
// slider sets the volume as it moves (unmuting). One 1 s interval redraws the progress while
// something plays, the progress is shown and the widget is visible; nothing ticks otherwise.
// Picking a layout asks for its natural size (widget.resize).

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

let fetchedAt = Date.now();  // when m.position was read (the mini players carry it forward)

media.onChange((s) => {
  m = s;
  fetchedAt = Date.now();
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

// Frames only while a line overflows (`over`), the mode wants them (hovered, or always) and the
// dock is on screen; never with reduce motion or in a preview. A changed speed restarts the frames.
function updateMarquee(over) {
  const mode = settings.marqueeMode || 'hover';
  const want = (mode === 'always' || (mode === 'hover' && hovered)) && dock.visible && !dock.reduceMotion &&
               !dock.preview && over;
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
  const mini = miniName();
  if (mini) {
    drawMini(canvas, dock, mini);
    return;
  }
  stopTick();
  const g = geom(canvas.width, canvas.height);
  const mode = settings.controls || 'hover';
  const over = m.session && (mode === 'always' || (mode === 'hover' && hovered));  // the three over the face
  const onArt = m.session && mode === 'sides' && hovered;  // play / pause over the art
  drawArt(canvas, g.ax, g.ay, g.s, dock, over || onArt);
  if (m.session && !over && !onArt) {
    if (mode === 'sides') badge(canvas, g, m.status === 'playing' ? pauseGlyph : playGlyph);
    else if (m.status !== 'playing' && !textShown()) badge(canvas, g, pauseGlyph);
  }
  updateMarquee(overflow(canvas, g));
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
  if (!e.visible) {
    stopMarquee();
    stopTick();
  } else {
    requestRedraw();  // back on screen: draw() starts an "always" marquee (and the progress) again
  }
}

let lastLayout = settings.layout;

export function onSettingsChanged() {
  stopMarquee();
  stopTick();
  press = null;
  // Round 23 phase 23B: another layout: the window takes its natural size (the tile: the
  // manifest's standalone default). Only a standalone widget's window honours it.
  if (settings.layout !== lastLayout) {
    lastLayout = settings.layout;
    if (dock.standalone) {
      const size = MINI[settings.layout] || TILE_SIZE;
      widget.resize(size.w, size.h);
    }
  }
  requestRedraw();
}

function send(kind) {
  if (kind === 'toggle') media.toggle();
  else if (kind === 'next') media.next();
  else media.previous();
}

export function onClick(e) {
  if (miniName()) return;  // a mini player's controls claim their presses; elsewhere a click does nothing
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

// --- the mini players (round 23 phase 23B) ----------------------------------------------------------
// Geometry in DIPs at the natural size (1 DIP = 1 px of the approved mock's CSS), drawn under one
// transform that fits it into the window. The Cover is 228 x 364 rather than the mock's caption
// (348): that is what its CSS adds up to (art 206, display, transport, volume, the gaps).

const MINI = {
  deck: { w: 356, h: 100, grow: true },
  capsule: { w: 360, h: 104, grow: true },
  cover: { w: 228, h: 364 },
  disc: { w: 204, h: 232 },
};
const TILE_SIZE = { w: 340, h: 96 };  // the manifest's standalone default
const WHITE = '#FFFFFFFF', BLACK = '#FF000000';

const miniName = () => (dock.standalone && MINI[settings.layout] ? settings.layout : null);

let view = null;     // { ox, oy, k }: the last mini draw's transform (DIPs of the window = o + k * natural)
let miniHits = [];   // the last mini draw's targets, natural DIPs
let press = null;    // the claimed press: { kind, hit, inside, on, frac, v }
let tickTimer = 0;   // the 1 s progress redraw

function stopTick() {
  if (tickTimer) clearInterval(tickTimer);
  tickTimer = 0;
}

// One redraw a second while the progress moves where it can be seen; none otherwise.
function syncTick(name) {
  const want = !!name && settings.showProgress !== false && m.session && m.status === 'playing' && m.duration > 0 &&
               dock.visible && !dock.preview;
  if (want && !tickTimer) tickTimer = setInterval(() => requestRedraw(), 1000);
  else if (!want) stopTick();
}

// The position now (carried forward while playing), seconds; null unknown.
function livePos() {
  if (m.position == null) return null;
  let p = m.position;
  if (m.status === 'playing') p += (Date.now() - fetchedAt) / 1000;
  return m.duration ? Math.min(p, m.duration) : p;
}

const canSeekNow = () => !!(m.session && m.canSeek && m.duration > 0);

// --- colours ---

// A colour setting that is set: "#AARRGGBB" / "#RRGGBB" with a non-zero alpha ("#AARRGGBB" out).
function colorOf(value, fallback) {
  if (typeof value !== 'string' || !/^#([0-9a-f]{6}|[0-9a-f]{8})$/i.test(value)) return fallback;
  if (value.length === 7) return '#FF' + value.slice(1);
  if (value.slice(1, 3) === '00') return fallback;
  return value;
}
const rgbOf = (c) => (typeof c === 'string' && c.length === 9 ? c.slice(3) : 'FFFFFF');
const withAlpha = (c, aa) => '#' + aa + rgbOf(c);
function mix(a, b, t) {
  const pa = parseInt(rgbOf(a), 16), pb = parseInt(rgbOf(b), 16);
  let out = '#FF';
  for (const sh of [16, 8, 0]) {
    const v = Math.round(((pa >> sh) & 255) * (1 - t) + ((pb >> sh) & 255) * t);
    out += v.toString(16).padStart(2, '0').toUpperCase();
  }
  return out;
}

// The skin's colours (the mock's .stage[data-skin] blocks). A paint is a colour, a list of
// vertical gradient stops, or (buttons) { linear: [top, bottom] } / { radial: [3 colours], mid }.
function skin() {
  const t = dock.theme;
  const font = 'Segoe UI Variable Text';
  let s;
  if (settings.skin === 'silver') {
    s = {
      bg: [[0, '#FFFBFCFD'], [0.46, '#FFD6DBE3'], [0.54, '#FFB3BAC6'], [1, '#FFE0E4EA']], edge: '#FF808898',
      ink: '#FF1A2130', dim: '#FF475062', acc: '#FF2F7FE0',
      lcd: [[0, '#FF0B1830'], [1, '#FF14305A']], lcdInk: '#FFC8EAFF', lcdDim: '#FF86B7DE', lcdAcc: '#FF6FD0FF',
      track: '#3DC8EAFF', pad: 7, font, shade: '#BF000000', light: '#D9FFFFFF', fade: '#FF10244A',
      btn: { radial: ['#FFFFFFFF', '#FFCBD2DC', '#FF98A1B0'], mid: 0.58 }, btnInk: '#FF22304A', btnEdge: '#FF7A8292',
      play: { radial: ['#FFA9D6FF', '#FF2F7FE0', '#FF15469A'], mid: 0.56 }, playInk: WHITE, hole: '#FFC9CFD9',
    };
  } else if (settings.skin === 'night') {
    s = {
      bg: [[0, '#FF3B4049'], [0.52, '#FF1C1F25'], [1, '#FF121418']], edge: '#FF06070A',
      ink: '#FFECE8DF', dim: '#FFA39E92', acc: '#FFFFB347',
      lcd: '#FF0B0906', lcdInk: '#FFFFB347', lcdDim: '#FFBD8230', lcdAcc: '#FFFFB347',
      track: '#38FFB347', pad: 7, font: 'Consolas', shade: '#E6000000', light: '#1AFFFFFF', fade: '#FF0B0906',
      btn: { linear: ['#FF4C515B', '#FF24272E'] }, btnInk: '#FFECE8DF', btnEdge: '#FF08090C',
      play: { linear: ['#FFFFC76E', '#FFDF8818'] }, playInk: '#FF1C1206', hole: '#FF1C1F25',
    };
  } else {
    // "theme": the dock theme's panel, text and accent (no blur: the theme's own translucency).
    s = {
      bg: t.background, edge: withAlpha(t.text, '2B'), ink: t.text, dim: t.dateText, acc: t.accent,
      lcd: null, lcdInk: t.text, lcdDim: t.dateText, lcdAcc: t.accent,
      track: withAlpha(t.text, '38'), pad: 0, font, fade: t.background,
      btn: withAlpha(t.text, '21'), btnInk: t.text, btnEdge: null,
      play: withAlpha(t.text, 'FF'), playInk: withAlpha(t.background, 'FF'), hole: withAlpha(t.background, 'FF'),
    };
  }
  const acc = colorOf(settings.accentColor, null);
  if (acc) {
    s.acc = s.lcdAcc = acc;
    s.track = withAlpha(acc, '3D');
    if (s.play.radial) s.play = { radial: [mix(acc, WHITE, 0.55), acc, mix(acc, BLACK, 0.45)], mid: 0.56 };
    else if (s.play.linear) s.play = { linear: [mix(acc, WHITE, 0.25), mix(acc, BLACK, 0.12)] };
  }
  return s;
}

const vpaint = (canvas, p, y0, y1) => (Array.isArray(p) ? canvas.linearGradient(0, y0, 0, y1, p) : p);

function btnPaint(canvas, p, cx, cy, d) {
  if (typeof p === 'string') return p;
  if (p.linear) return canvas.linearGradient(0, cy - d / 2, 0, cy + d / 2, [[0, p.linear[0]], [1, p.linear[1]]]);
  // "radial-gradient(circle at 50% 22%)": centred on the highlight, out to the box's far corner.
  const fy = cy - 0.28 * d, R = 0.93 * d;
  return canvas.radialGradient(cx, fy, R, R, [[0, p.radial[0]], [p.mid, p.radial[1]], [1, p.radial[2]]]);
}

// --- shapes ---

function rrPath(canvas, x, y, w, h, r) {
  return canvas.path().moveTo(x + r, y).lineTo(x + w - r, y).arc(x + w - r, y + r, r, -Math.PI / 2, 0)
    .lineTo(x + w, y + h - r).arc(x + w - r, y + h - r, r, 0, Math.PI / 2).lineTo(x + r, y + h)
    .arc(x + r, y + h - r, r, Math.PI / 2, Math.PI).lineTo(x, y + r).arc(x + r, y + r, r, Math.PI, Math.PI * 1.5).close();
}
const circlePath = (canvas, cx, cy, r) => canvas.path().arc(cx, cy, r, 0, Math.PI).arc(cx, cy, r, Math.PI, Math.PI * 2).close();

// The panel: the skin's fill and its hairline.
function panel(canvas, sk, x, y, w, h, r) {
  canvas.fillRoundRect(x + 0.5, y + 0.5, w - 1, h - 1, r, vpaint(canvas, sk.bg, y, y + h));
  canvas.strokeRoundRect(x + 0.5, y + 0.5, w - 1, h - 1, r, sk.edge, 1);
}

// The art (or the placeholder gradient, with the note when `noteSize` > 0) inside a clip path.
function artIn(canvas, clipPath, x, y, w, h, noteSize) {
  canvas.save();
  canvas.clipPath(clipPath);
  let drawn = false;
  if (art) {
    try {
      canvas.image(art, x, y, w, h);
      drawn = true;
    } catch (e) {
      art = null;  // gone since (a newer track): the next change brings the new one
    }
  }
  if (!drawn) {
    canvas.fillRect(x, y, w, h, canvas.linearGradient(x, y, x + w, y + h, [[0, '#FF3A4A6A'], [1, '#FF1F2638']]));
    if (noteSize > 0) note(canvas, x + w / 2, y + h / 2, noteSize, m.session ? '#E6FFFFFF' : '#80FFFFFF');
  }
  canvas.restore();
}

// The display (the mock's .lcd): its fill, the inset shade along its top and the light line under it.
function lcdBox(canvas, sk, x, y, w, h) {
  if (!sk.lcd) return;
  canvas.fillRoundRect(x, y + 1, w, h, 6, sk.light);
  canvas.fillRoundRect(x, y, w, h, 6, vpaint(canvas, sk.lcd, y, y + h));
  canvas.fillRoundRect(x, y, w, Math.min(h, 8), 6, canvas.linearGradient(0, y, 0, y + 4, [[0, sk.shade], [1, '#00000000']]));
}

// One line of the display at centre height `cy`: scrolled by the marquee, else faded at the edge
// (or centred when it fits and `center`). True if it does not fit.
function miniLine(canvas, sk, text, x, cy, w, style, color, center) {
  if (!text || w <= 0) return false;
  const tw = canvas.measureText(text, style).width;
  canvas.save();
  canvas.clip(x, cy - style.size, w, style.size * 2);
  const st = { ...style, baseline: 'middle', color };
  if (tw > w && marquee) {
    const off = (((Date.now() - marquee.start) / 1000) * speed()) % (tw + MARQUEE_GAP);
    canvas.text(text, x - off, cy, st);
    canvas.text(text, x - off + tw + MARQUEE_GAP, cy, st);
  } else if (tw <= w && center) {
    canvas.text(text, x + w / 2, cy, { ...st, align: 'center' });
  } else {
    canvas.text(text, x, cy, st);
    if (tw > w) {
      canvas.fillRect(x + w - 18, cy - style.size, 18, style.size * 2,
        canvas.linearGradient(x + w - 18, 0, x + w, 0, [[0, withAlpha(sk.fade, '00')], [1, sk.fade]]));
    }
  }
  canvas.restore();
  return tw > w;
}

// The progress bar; a seek target when the session can seek.
function progressBar(canvas, sk, x, cy, w, frac) {
  if (w <= 4) return;
  canvas.fillRoundRect(x, cy - 2, w, 4, 2, sk.track);
  if (frac > 0) canvas.fillRoundRect(x, cy - 2, Math.max(4, w * Math.min(1, frac)), 4, 2, sk.lcdAcc);
  if (canSeekNow()) miniHits.push({ kind: 'seek', x, y: cy - 8, w, h: 16 });
}

// The position shown: a seek drag's, else the live one.
function shownPos() {
  if (press && (press.kind === 'seek' || press.kind === 'ring')) return press.frac * (m.duration || 0);
  return livePos();
}

// The display's lines in the box (x, y, w, h), centred on its height: the title, the second line,
// and the progress row (opts.prow: "clock" bar + "pos / dur", "split" pos | bar | dur, "bar").
// True if a line does not fit.
function textBlock(canvas, sk, x, y, w, h, opts) {
  const titleSt = { font: sk.font, size: 13, weight: 'semibold' };
  const subSt = { font: sk.font, size: 11.5 };
  const timeSt = { font: sk.font, size: 10.5 };
  const gap = opts.gap ?? 3;
  const prow = settings.showProgress !== false && opts.prow ? opts.prow : null;
  const ph = prow === 'bar' ? 4 : prow ? 14 : 0;
  const total = 17 + gap + 15 + (prow ? gap + ph : 0);
  const top = y + (h - total) / 2;
  let title, sub;
  if (!m.session) {
    title = 'Nothing playing';
    sub = '';
  } else {
    title = titleText();
    sub = opts.by ? [m.artist, m.album].filter(Boolean).join(' · ') : artistText();
  }
  let over = miniLine(canvas, sk, title, x, top + 8.5, w, titleSt, m.session ? sk.lcdInk : sk.lcdDim, opts.center);
  over = miniLine(canvas, sk, sub, x, top + 17 + gap + 7.5, w, subSt, sk.lcdDim, opts.center) || over;
  if (prow) {
    const cy = top + 17 + gap + 15 + gap + ph / 2;
    const dur = m.session && m.duration > 0 ? m.duration : 0;
    const pos = dur ? Math.max(0, Math.min(dur, shownPos() || 0)) : 0;
    const frac = dur ? pos / dur : 0;
    if (prow === 'bar') {
      progressBar(canvas, sk, x, cy, w, frac);
    } else if (prow === 'clock') {
      const label = dur ? clock(pos) + ' / ' + clock(dur) : '';
      const lw = label ? canvas.measureText(label, timeSt).width : 0;
      if (label) canvas.text(label, x + w, cy, { ...timeSt, align: 'right', baseline: 'middle', color: sk.lcdDim });
      progressBar(canvas, sk, x, cy, w - (lw ? lw + 6 : 0), frac);
    } else {
      const a = dur ? clock(pos) : '', b = dur ? clock(dur) : '';
      const aw = a ? canvas.measureText(a, timeSt).width : 0, bw = b ? canvas.measureText(b, timeSt).width : 0;
      if (a) canvas.text(a, x, cy, { ...timeSt, baseline: 'middle', color: sk.lcdDim });
      if (b) canvas.text(b, x + w, cy, { ...timeSt, align: 'right', baseline: 'middle', color: sk.lcdDim });
      const bx = x + (aw ? aw + 6 : 0);
      progressBar(canvas, sk, bx, cy, x + w - (bw ? bw + 6 : 0) - bx, frac);
    }
  }
  return over;
}

// The SVG glyphs of the mock (24 x 24 view box) in an s x s box centred on (cx, cy).
function icon(canvas, kind, cx, cy, s, color) {
  const u = s / 24, x0 = cx - s / 2, y0 = cy - s / 2;
  const poly = (pts) => {
    const p = canvas.path();
    pts.forEach(([px, py], i) => (i ? p.lineTo(x0 + px * u, y0 + py * u) : p.moveTo(x0 + px * u, y0 + py * u)));
    canvas.fill(p.close(), color);
  };
  if (kind === 'previous') {
    canvas.fillRect(x0 + 6 * u, y0 + 5 * u, 3 * u, 14 * u, color);
    poly([[21, 5], [21, 19], [11, 12]]);
  } else if (kind === 'next') {
    canvas.fillRect(x0 + 15 * u, y0 + 5 * u, 3 * u, 14 * u, color);
    poly([[3, 5], [13, 12], [3, 19]]);
  } else if (kind === 'play') {
    poly([[8, 4], [20, 12], [8, 20]]);
  } else {
    canvas.fillRect(x0 + 6 * u, y0 + 4 * u, 4.5 * u, 16 * u, color);
    canvas.fillRect(x0 + 13.5 * u, y0 + 4 * u, 4.5 * u, 16 * u, color);
  }
}

// A round transport button (the mock's .tb / .tb.play): brighter while pressed, dimmed glyph when
// the session does not offer it.
function miniButton(canvas, sk, kind, cx, cy, d, on) {
  const play = kind === 'toggle';
  canvas.fillCircle(cx, cy, d / 2, btnPaint(canvas, play ? sk.play : sk.btn, cx, cy, d));
  if (sk.btnEdge) canvas.strokeCircle(cx, cy, d / 2 - 0.5, sk.btnEdge, 1);
  if (on && press && press.kind === kind && press.inside) canvas.fillCircle(cx, cy, d / 2, '#24FFFFFF');
  const ink = play ? sk.playInk : sk.btnInk;
  const glyph = play ? (m.status === 'playing' ? 'pause' : 'play') : kind;
  icon(canvas, glyph, cx, cy, d * 0.52, on ? ink : withAlpha(ink, '66'));
  miniHits.push({ kind, cx, cy, r: d / 2, on });
}

// Previous / play-pause / next centred on (cx, cy), 7 DIPs apart.
function transport(canvas, sk, cx, cy, small, big) {
  const dx = (small + big) / 2 + 7;
  const live = !!m.session;
  miniButton(canvas, sk, 'previous', cx - dx, cy, small, live && m.canPrevious);
  miniButton(canvas, sk, 'toggle', cx, cy, big, live);
  miniButton(canvas, sk, 'next', cx + dx, cy, small, live && m.canNext);
}

// The speaker and the volume slider (the mock's .volrow) from x, w wide, centred on cy.
function volumeRow(canvas, sk, x, cy, w) {
  let a = { available: false, volume: 0, muted: false };
  try {
    a = audio.get();
  } catch (e) {
    // no audio device: the slider shows empty
  }
  const v = press && press.kind === 'vol' ? press.v : a.available ? a.volume || 0 : 0;
  const off = !a.available || (a.muted && !(press && press.kind === 'vol'));
  const waves = off ? 0 : v <= 0.005 ? 0 : v < 0.34 ? 1 : v < 0.67 ? 2 : 3;
  speaker(canvas, x, cy - 7, 14, waves, off, sk.dim);
  const sx = x + 19, sw = w - 19;
  const kx = sx + 6 + (sw - 12) * v;
  canvas.fillRoundRect(sx, cy - 2, sw, 4, 2, '#40808080');
  canvas.fillRoundRect(sx, cy - 2, Math.max(4, kx - sx), 4, 2, off ? '#80808080' : sk.acc);
  canvas.fillCircle(kx, cy, 6, off ? '#FF9A9A9A' : sk.acc);
  canvas.strokeCircle(kx, cy, 6, '#30000000', 1);
  if (a.available) miniHits.push({ kind: 'vol', x: sx - 6, y: cy - 9, w: sw + 12, h: 18, sx, sw });
}

// --- the four layouts (natural DIPs; `E`: the extra width the Deck's display / the Capsule's pill takes) ---

function drawDeck(canvas, sk, E) {
  const W = 356 + E;
  panel(canvas, sk, 0, 0, W, 100, 12);
  artIn(canvas, rrPath(canvas, 10, 10, 80, 80, 8), 10, 10, 80, 80, 44);
  canvas.strokeRoundRect(10.5, 10.5, 79, 79, 7.5, '#40000000', 1);
  const lw = 136 + E;
  lcdBox(canvas, sk, 100, 10, lw, 80);
  const over = textBlock(canvas, sk, 100 + sk.pad, 10, lw - 2 * sk.pad, 80, { by: true, prow: 'clock' });
  if (settings.showVolume !== false) {
    transport(canvas, sk, 296 + E, 32, 28, 38);
    volumeRow(canvas, sk, 246 + E, 80, 100);
  } else {
    transport(canvas, sk, 296 + E, 50, 28, 38);
  }
  return over;
}

function drawCapsule(canvas, sk, E) {
  const W = 360 + E;
  // The pill.
  canvas.fillRoundRect(34.5, 14.5, W - 35, 75, 37.5, vpaint(canvas, sk.bg, 14, 90));
  canvas.strokeRoundRect(34.5, 14.5, W - 35, 75, 37.5, sk.edge, 1);
  // The platter's shadow on the pill (the mock's box-shadow 0 8px 22px), then the platter.
  canvas.save();
  canvas.clipPath(rrPath(canvas, 34, 14, W - 34, 76, 38));
  canvas.fillCircle(52, 60, 64, canvas.radialGradient(52, 60, 64, 64, [[0.72, '#70000000'], [1, '#00000000']]));
  canvas.restore();
  canvas.fillCircle(52, 52, 51.5, vpaint(canvas, sk.bg, 0, 104));
  canvas.strokeCircle(52, 52, 51.5, sk.edge, 1);
  artIn(canvas, circlePath(canvas, 52, 52, 45), 7, 7, 90, 90, m.session ? 0 : 46);
  canvas.strokeCircle(52, 52, 44.5, '#40000000', 1);
  if (m.session || art) {  // the centre hole with its light ring
    canvas.strokeCircle(52, 52, 8.5, '#59FFFFFF', 3);
    canvas.fillCircle(52, 52, 7, sk.hole);
  }
  // The display and the transport.
  const prog = settings.showProgress !== false;
  const lines = 17 + 3 + 15 + (prog ? 3 + 4 : 0);
  const lw = W - 248, lh = lines + 2 * sk.pad;
  lcdBox(canvas, sk, 117, 52 - lh / 2, lw, lh);
  const over = textBlock(canvas, sk, 117 + sk.pad, 52 - lines / 2, lw - 2 * sk.pad, lines, { prow: 'bar' });
  transport(canvas, sk, W - 67, 52, 28, 38);
  return over;
}

function drawCover(canvas, sk) {
  panel(canvas, sk, 0, 0, 228, 364, 14);
  artIn(canvas, rrPath(canvas, 11, 11, 206, 206, 8), 11, 11, 206, 206, 110);
  canvas.strokeRoundRect(11.5, 11.5, 205, 205, 7.5, '#40000000', 1);
  lcdBox(canvas, sk, 11, 225, 206, 58);
  const over = textBlock(canvas, sk, 11 + sk.pad, 225, 206 - 2 * sk.pad, 58, { by: true, prow: 'split' });
  if (settings.showVolume !== false) {
    transport(canvas, sk, 114, 312, 32, 42);
    volumeRow(canvas, sk, 11, 348, 206);
  } else {
    transport(canvas, sk, 114, 323, 32, 42);
  }
  return over;
}

function drawDisc(canvas, sk) {
  // The rim, the progress ring round it, the art as a disc with a dark scrim under the hub.
  canvas.fillCircle(102, 102, 101.5, vpaint(canvas, sk.bg, 0, 204));
  canvas.strokeCircle(102, 102, 101.5, sk.edge, 1);
  canvas.strokeCircle(102, 102, 93.5, '#59808080', 5);
  const dur = m.session && m.duration > 0 ? m.duration : 0;
  if (settings.showProgress !== false && dur) {
    const f = Math.max(0, Math.min(1, (shownPos() || 0) / dur));
    const a0 = -Math.PI / 2, a1 = a0 + Math.PI * 2 * f;
    if (f >= 0.999) {
      canvas.strokeCircle(102, 102, 93.5, sk.acc, 5);
    } else if (f > 0.001) {
      const p = canvas.path().arc(102, 102, 93.5, a0, Math.min(a1, a0 + Math.PI));
      if (a1 > a0 + Math.PI) p.arc(102, 102, 93.5, a0 + Math.PI, a1);
      canvas.stroke(p, sk.acc, 5);
    }
    if (canSeekNow()) miniHits.push({ kind: 'ring', cx: 102, cy: 102, r0: 84, r1: 102 });
  }
  artIn(canvas, circlePath(canvas, 102, 102, 91), 11, 11, 182, 182, m.session ? 0 : 90);
  if (m.session) {
    // radial-gradient(circle, rgba(0,0,0,.55) 0 34%, transparent 62%) of a 182 DIP box: its
    // farthest corner is 128.7 DIPs out.
    canvas.fillCircle(102, 102, 91, canvas.radialGradient(102, 102, 128.7, 128.7,
      [[0, '#8C000000'], [0.34, '#8C000000'], [0.62, '#00000000'], [1, '#00000000']]));
    transport(canvas, sk, 102, 102, 28, 50);
  }
  canvas.strokeCircle(102, 102, 90.5, '#40000000', 1);
  // The label plate overlapping the rim.
  panel(canvas, sk, 14, 184, 176, 48, 10);
  lcdBox(canvas, sk, 20, 190, 164, 36);
  return textBlock(canvas, sk, 28, 190, 148, 36, { center: true, gap: 2 });
}

function drawMini(canvas, dock, name) {
  const L = MINI[name], W = canvas.width, H = canvas.height;
  const k = Math.max(0.05, Math.min(W / L.w, H / L.h));
  const E = L.grow ? Math.max(0, W / k - L.w) : 0;
  view = { ox: (W - (L.w + E) * k) / 2, oy: (H - L.h * k) / 2, k };
  miniHits = [];
  const sk = skin();
  canvas.save();
  canvas.translate(view.ox, view.oy);
  canvas.scaleBy(k, k);
  let over = false;
  if (name === 'deck') over = drawDeck(canvas, sk, E);
  else if (name === 'capsule') over = drawCapsule(canvas, sk, E);
  else if (name === 'cover') over = drawCover(canvas, sk);
  else over = drawDisc(canvas, sk);
  canvas.restore();
  updateMarquee(over && !!m.session);
  syncTick(name);
}

// --- the mini players' input: claimed presses (onPress returns true) -----------------------------

function hitAt(x, y) {
  for (const h of miniHits) {
    if (h.r !== undefined) {
      if (Math.hypot(x - h.cx, y - h.cy) <= h.r) return h;
    } else if (h.kind === 'ring') {
      const d = Math.hypot(x - h.cx, y - h.cy);
      if (d >= h.r0 && d <= h.r1) return h;
    } else if (x >= h.x && x < h.x + h.w && y >= h.y && y < h.y + h.h) {
      return h;
    }
  }
  return null;
}

const clamp01 = (v) => Math.max(0, Math.min(1, v));
function local(e) {
  return view ? { x: (e.x - view.ox) / view.k, y: (e.y - view.oy) / view.k } : { x: e.x, y: e.y };
}
// The press's value at a point: the seek fraction, the ring's angle from the top, the volume.
function follow(p, x, y) {
  const h = p.hit;
  if (p.kind === 'seek') p.frac = clamp01((x - h.x) / h.w);
  else if (p.kind === 'ring') p.frac = (Math.atan2(x - h.cx, -(y - h.cy)) + Math.PI * 2) % (Math.PI * 2) / (Math.PI * 2);
  else if (p.kind === 'vol') setVolume(p, clamp01((x - h.sx - 6) / (h.sw - 12)));
  else p.inside = Math.hypot(x - h.cx, y - h.cy) <= h.r;
}

// The slider sets the volume as it moves (on the 1 % grid, once per change), unmuting.
function setVolume(p, v) {
  const step = Math.round(v * 100) / 100;
  p.v = step;
  if (p.sent === step) return;
  p.sent = step;
  audio.setVolume(step);
  if (p.muted && step > 0) {
    audio.setMuted(false);
    p.muted = false;
  }
}

export function onPress(e) {
  if (!miniName() || !view) return;  // the tile: as before (a drag moves the item / the window)
  const { x, y } = local(e);
  const h = hitAt(x, y);
  if (!h) return;  // the panel, the art: a drag moves the window
  press = { kind: h.kind, hit: h, inside: true, on: h.on !== false };
  if (h.kind === 'vol') {
    try {
      press.muted = !!audio.get().muted;
    } catch (err) {
      press.muted = false;
    }
  }
  follow(press, x, y);
  requestRedraw();
  return true;
}

export function onDrag(e) {
  if (!press) return;
  const { x, y } = local(e);
  const was = press.inside;
  const before = press.frac;
  follow(press, x, y);
  if (press.inside !== was || press.frac !== before || press.kind === 'vol') requestRedraw();
}

export function onRelease(e) {
  const p = press;
  press = null;
  if (!p) return;
  if (!e.cancelled) {
    if ((p.kind === 'previous' || p.kind === 'toggle' || p.kind === 'next') && p.inside && p.on) {
      send(p.kind);
    } else if ((p.kind === 'seek' || p.kind === 'ring') && canSeekNow()) {
      const to = p.frac * m.duration;
      media.seek(to);
      m = { ...m, position: to };  // shown at once; the session's report follows
      fetchedAt = Date.now();
    }
  }
  requestRedraw();
}
