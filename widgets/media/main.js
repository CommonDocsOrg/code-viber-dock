// Now playing: the system's current media session (the `media` provider): album art, title and
// artist. Hovering the art shows previous / play-pause / next over it (a click on one sends it);
// a click on the text opens a popup with a larger art, the album and the controls. A long title
// scrolls (marquee) while the pointer is over the widget: 30 frames a second only then, never with
// reduce motion; otherwise it fades out at the edge. Event-driven: media.onChange redraws when the
// track, the playback state or the art change; nothing ticks.

const POPUP_W = 320, POPUP_H = 140;
const MARQUEE_PX_S = 28, MARQUEE_GAP = 36, FRAME_MS = 33;

let m = media.get();
let art = media.art();
let hovered = false;
let marquee = null;  // { start, timer } while the title scrolls
let hits = [];

media.onChange((s) => {
  m = s;
  art = media.art();
  requestRedraw();
  if (popup.isOpen) popup.redraw();
});

const vertical = () => dock.axis === 'vertical';
const textShown = () => settings.showText && !vertical();

export function measure(canvas, dock) {
  const s = Math.round(dock.iconSize);
  return { main: textShown() ? s + 8 + settings.textWidth : s, cross: s };
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

function overflow(canvas) {
  return textShown() && m.session && canvas.measureText(m.title || '', titleStyle).width > settings.textWidth;
}

function updateMarquee(canvas) {
  const want = hovered && settings.marquee && dock.visible && !dock.reduceMotion && overflow(canvas);
  if (want && !marquee) {
    marquee = { start: Date.now(), timer: setInterval(() => requestRedraw(), FRAME_MS) };
  } else if (!want && marquee) {
    clearInterval(marquee.timer);
    marquee = null;
  }
}

function lineText(canvas, text, x, y, w, style, color, scroll) {
  canvas.save();
  canvas.clip(x, y - 1, w, style.size * 1.5);
  const tw = canvas.measureText(text, style).width;
  if (scroll && tw > w) {
    const off = (((Date.now() - scroll.start) / 1000) * MARQUEE_PX_S) % (tw + MARQUEE_GAP);
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
  const s = Math.min(canvas.height, vertical() ? canvas.width : canvas.height);
  const ax = vertical() ? (canvas.width - s) / 2 : 0, ay = (canvas.height - s) / 2;
  drawArt(canvas, ax, ay, s, dock, hovered && m.session);
  if (hovered && m.session) {
    canvas.fillRoundRect(ax, ay, s, s, s * 0.16, '#99000000');
    const g = s * 0.22;
    control(canvas, 'previous', ax + s * 0.2, ay + s / 2, g * 0.8, m.canPrevious ? '#FFFFFFFF' : '#60FFFFFF');
    control(canvas, 'toggle', ax + s / 2, ay + s / 2, g, '#FFFFFFFF');
    control(canvas, 'next', ax + s * 0.8, ay + s / 2, g * 0.8, m.canNext ? '#FFFFFFFF' : '#60FFFFFF');
  } else if (m.session && m.status !== 'playing' && !textShown()) {
    canvas.fillCircle(ax + s - 9, ay + s - 9, 7, '#C0000000');
    pauseGlyph(canvas, ax + s - 9, ay + s - 9, 7, '#FFFFFFFF');
  }
  updateMarquee(canvas);
  if (!textShown()) return;
  const tx = s + 8, w = settings.textWidth;
  const ink = dock.theme.text, dim = dock.theme.dateText;
  if (!m.session) {
    canvas.text('Nothing playing', tx, canvas.height / 2, { size: 12, baseline: 'middle', color: dim });
    return;
  }
  const alpha = m.status === 'playing' ? 1 : 0.7;
  canvas.save();
  canvas.opacity(alpha);
  const top = canvas.height / 2 - 16;
  lineText(canvas, m.title || '(no title)', tx, top, w, titleStyle, ink, marquee);
  lineText(canvas, m.artist || m.album || '', tx, top + 18, w, artistStyle, dim, null);
  canvas.restore();
}

export function onHover(e) {
  hovered = e.inside;
  requestRedraw();
  if (!e.inside && marquee) {
    clearInterval(marquee.timer);
    marquee = null;
  }
}

export function onVisibilityChange(e) {
  if (!e.visible && marquee) {
    clearInterval(marquee.timer);
    marquee = null;
  }
}

function send(kind) {
  if (kind === 'toggle') media.toggle();
  else if (kind === 'next') media.next();
  else media.previous();
}

export function onClick(e) {
  const s = Math.min(dock.height, vertical() ? dock.width : dock.height);
  const onArt = !textShown() || e.x < s;
  if (onArt && m.session) {
    send(e.x < s / 3 ? 'previous' : e.x > (s * 2) / 3 ? 'next' : 'toggle');
    return;
  }
  if (!e.closedPopup && m.session) popup.open(POPUP_W, POPUP_H);
}

export function onWheel(e) {
  if (!settings.wheelVolume) return false;
  const a = audio.get();
  if (!a.available) return false;
  audio.setVolume(Math.min(1, Math.max(0, Math.round(a.volume * 50) / 50 + Math.sign(e.notches) * 0.02)));
  return true;
}

// --- the popup ------------------------------------------------------------------------------------

const clock = (t) => (t == null ? '' : Math.floor(t / 60) + ':' + String(Math.floor(t % 60)).padStart(2, '0'));

export function drawPopup(canvas, dock) {
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
