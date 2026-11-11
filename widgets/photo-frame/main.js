// Photo frame (round 17 phase 17A): one picture, or a folder of pictures as a slideshow.
//
// Pictures come from anywhere on disk: canvas.image with an absolute path (the "shell"
// permission; the host decodes a photo at most 2048 px on its long side, turned the way the
// camera held it, and keeps the last few decoded ones). The folder is read with shell.list
// (Explorer's name order, so "2.jpg" sorts before "10.jpg"), filtered to image extensions, and
// sorted by the `order` setting; a random order is a shuffle per round. One setTimeout waits for
// the next picture; it stops while the dock is hidden (onVisibilityChange) and, with
// pauseOnHover, while the pointer rests on the frame. Nothing runs for a single picture.
//
// Transitions (fade, slides) are drawn by this script, 60 frames a second for transitionMs only
// (a setInterval that stops when it is done), never with Reduce motion, never in a preview. The
// frame is: the shadow (a stack of translucent rounded rectangles, a cheap blur), the border (a
// line, a mat or a Polaroid band), then the picture clipped to the inside, covering it (cropped,
// centred) or fitting inside it (bands in `background`).
//
// On a dock measure() sizes the frame from `size` / `width` and `aspect` (`image`: the current
// picture's own shape, read with canvas.imageSize). Standalone the window is the frame: a fixed
// aspect gives the largest picture of that shape that fits, centred.

const EXTENSIONS = ['png', 'jpg', 'jpeg', 'jfif', 'webp', 'bmp', 'gif', 'tif', 'tiff', 'ico', 'heic', 'heif', 'avif'];
const SIZES = { small: 64, medium: 96, large: 160 };
const RATIOS = { '1:1': 1, '4:3': 4 / 3, '3:2': 3 / 2, '16:9': 16 / 9, '3:4': 3 / 4, '2:3': 2 / 3, '9:16': 9 / 16 };
const SHADOWS = { soft: { size: 6, alpha: 0.35 }, medium: { size: 12, alpha: 0.5 }, strong: { size: 20, alpha: 0.7 } };
const SHADOW_STEPS = 8;
const FRAME_MS = 16;
const DEFAULT_RATIO = 4 / 3;
const MAX_SHOWN = 50;  // debugStore: the last paths shown

let files = [];        // the folder's pictures in order: [{ name, path, modified }]
let listedFolder = ''; // what `files` was read for (and with which order)
let listedOrder = '';
let index = 0;         // the picture shown, in `files`
let current = '';      // its path (or the single image's)
let previous = '';     // the picture a transition slides / fades away from
let ratio = 0;         // the current picture's width / height, once known (aspect "image")
let failed = new Map();  // path -> error text (skipped by the slideshow, shown in the frame)
let slideTimer = 0;
let anim = null;       // { start, ms, kind }
let animTimer = 0;
let hot = false;       // the pointer is over the frame
let cycle = 0;         // rounds through the folder (a re-list at each wrap)

const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
const ease = (t) => (t < 0.5 ? 4 * t * t * t : 1 - Math.pow(-2 * t + 2, 3) / 2);

function transparent(c) {
  return typeof c !== 'string' || c === '' || /^#00[0-9a-f]{6}$/i.test(c);
}

function isImage(name) {
  const dot = name.lastIndexOf('.');
  return dot >= 0 && EXTENSIONS.includes(name.slice(dot + 1).toLowerCase());
}

function shuffle(a) {
  for (let i = a.length - 1; i > 0; i--) {
    const j = Math.floor(Math.random() * (i + 1));
    [a[i], a[j]] = [a[j], a[i]];
  }
  return a;
}

// --- the slideshow's list ---------------------------------------------------------------------

function slideshow() {
  return settings.source === 'folder';
}

// Reads the folder (when it, or the order, changed, or `force`) and sorts it. Explorer's name
// order is what shell.list returns; the date orders use the files' modified time.
function listFolder(force) {
  const folder = String(settings.folder ?? '').trim();
  const order = String(settings.order ?? 'name-asc');
  if (!force && folder === listedFolder && order === listedOrder) return;
  listedFolder = folder;
  listedOrder = order;
  files = [];
  if (!folder || dock.preview) return;
  let entries = [];
  try {
    entries = shell.list(folder);
  } catch (e) {
    console.warn('photo-frame:', e.message);
    failed.set('folder:' + folder, 'Cannot read the folder');
    return;
  }
  failed.delete('folder:' + folder);
  files = entries.filter((e) => !e.folder && isImage(e.name)).map((e) => ({ name: e.name, path: e.path, modified: e.modified || 0 }));
  if (order === 'name-desc') files.reverse();
  else if (order === 'oldest') files.sort((a, b) => a.modified - b.modified);
  else if (order === 'newest') files.sort((a, b) => b.modified - a.modified);
  else if (order === 'random') shuffle(files);
}

// The next picture in the list that did not fail to load (in `step`'s direction), or ''.
function pick(from, step) {
  if (!files.length) return -1;
  let i = from;
  for (let n = 0; n < files.length; n++) {
    i = (i + step + files.length) % files.length;
    if (!failed.has(files[i].path)) return i;
  }
  return -1;
}

// Shows the picture at `i` (a transition from the one before, when asked).
function show(i, transition) {
  if (i < 0 || i >= files.length) return;
  const path = files[i].path;
  if (path === current) return;
  index = i;
  startTransition(path, transition);
  requestRedraw();
  schedule();
}

function startTransition(path, transition) {
  const kind = transition && !dock.reduceMotion && !dock.preview && current ? String(settings.transition ?? 'fade') : 'none';
  previous = kind === 'none' ? '' : current;
  current = path;
  ratio = 0;
  stopAnim();
  if (kind !== 'none') {
    anim = { start: Date.now(), ms: clamp(Number(settings.transitionMs) || 600, 100, 3000), kind };
    animTimer = setInterval(frame, FRAME_MS);
  }
  if (settings.debugStore) {
    const shown = storage.get('shown:' + dock.itemKey, []);
    shown.push(path);
    while (shown.length > MAX_SHOWN) shown.shift();
    storage.set('shown:' + dock.itemKey, shown);
  }
}

function frame() {
  if (!anim) return stopAnim();
  if (Date.now() - anim.start >= anim.ms) stopAnim();
  requestRedraw();
}

function stopAnim() {
  if (animTimer) clearInterval(animTimer);
  animTimer = 0;
  anim = null;
  previous = '';
}

function advance(step, transition) {
  listFolder(false);
  if (!files.length) return;
  let i = pick(index, step);
  if (i < 0) return;
  // A wrap ends the round: read the folder again (new files, a fresh shuffle), then start over.
  if (step > 0 && i <= index) {
    cycle++;
    listFolder(true);
    i = pick(files.length - 1, 1);
    if (i < 0) return;
  }
  show(i, transition);
}

// The slideshow's one timer: the next picture after `interval` seconds, while there is something
// to show and the frame is on screen (and not paused under the pointer).
function schedule() {
  if (slideTimer) clearTimeout(slideTimer);
  slideTimer = 0;
  if (!slideshow() || dock.preview || !dock.visible || files.length < 2) return;
  if (settings.pauseOnHover && hot) return;
  const ms = clamp(Number(settings.interval) || 10, 1, 3600) * 1000;
  slideTimer = setTimeout(() => {
    slideTimer = 0;
    advance(1, true);
    schedule();  // advance() scheduled already when the picture changed; this covers a stuck list
  }, ms);
}

// Picks what to show for the settings as they are now (a load, a settings change).
function sync() {
  if (slideshow()) {
    listFolder(false);
    if (!files.length) {
      current = '';
      stopAnim();
    } else if (!files.some((f) => f.path === current)) {
      const i = pick(-1, 1);
      if (i >= 0) {
        index = i;
        startTransition(files[i].path, false);
      } else {
        current = '';
      }
    } else {
      index = files.findIndex((f) => f.path === current);
    }
  } else {
    const path = String(settings.image ?? '').trim();
    if (path !== current) {
      stopAnim();
      current = path;
      ratio = 0;
      if (path && settings.debugStore) storage.set('shown:' + dock.itemKey, [path]);
    }
  }
  schedule();
  requestRedraw();
}

// --- geometry --------------------------------------------------------------------------------

function borderWidths() {
  const b = String(settings.border ?? 'mat');
  const w = clamp(Number(settings.borderWidth) || 6, 1, 48);
  if (b === 'none') return { left: 0, top: 0, right: 0, bottom: 0 };
  if (b === 'line') return { left: 1, top: 1, right: 1, bottom: 1 };
  if (b === 'polaroid') return { left: w, top: w, right: w, bottom: w * 3 };
  return { left: w, top: w, right: w, bottom: w };
}

function shadowSpec() {
  const s = SHADOWS[String(settings.shadow ?? 'soft')];
  const offset = clamp(Number(settings.shadowOffset) || 0, 0, 32);
  return s ? { size: s.size, alpha: s.alpha, offset } : { size: 0, alpha: 0, offset: 0 };
}

// The ratio the frame should have: the picture's own, a preset, or the custom width / height; 0
// when the picture's own is not known yet (DEFAULT_RATIO is used then).
function wantedRatio(canvas) {
  const a = String(settings.aspect ?? '4:3');
  if (a === 'custom') {
    return clamp(Number(settings.width) || 96, 24, 1024) / clamp(Number(settings.height) || 72, 24, 1024);
  }
  if (RATIOS[a]) return RATIOS[a];
  // The picture's own shape.
  if (dock.preview) return 3 / 2;
  if (ratio > 0) return ratio;
  if (current && !failed.has(current)) {
    try {
      const s = canvas.imageSize(current);
      if (s.width > 0 && s.height > 0) ratio = s.width / s.height;
    } catch (e) {
      failed.set(current, e.message);
    }
  }
  return ratio > 0 ? ratio : DEFAULT_RATIO;
}

// The frame on a dock: the picture's width from `size`, its height from the ratio.
function dockPicture(canvas) {
  const s = String(settings.size ?? 'medium');
  const w = s === 'icon' ? dock.iconSize : s === 'custom' ? clamp(Number(settings.width) || 96, 24, 1024) : (SIZES[s] ?? 96);
  return { w, h: Math.max(8, Math.round(w / wantedRatio(canvas))) };
}

// The layout: the whole box, the frame (border included), the picture inside it, all in DIPs.
function layout(canvas) {
  const b = borderWidths();
  const sh = shadowSpec();
  const r = clamp(Number(settings.cornerRadius) || 0, 0, 64);
  const mx = sh.size, myTop = sh.size, myBottom = sh.size + sh.offset;
  let pic;
  if (dock.standalone) {
    const availW = Math.max(8, canvas.width - 2 * mx - b.left - b.right);
    const availH = Math.max(8, canvas.height - myTop - myBottom - b.top - b.bottom);
    const want = wantedRatio(canvas);
    let w = availW, h = availW / want;
    if (h > availH) {
      h = availH;
      w = availH * want;
    }
    pic = { w, h };
  } else {
    pic = dockPicture(canvas);
  }
  const frameW = pic.w + b.left + b.right, frameH = pic.h + b.top + b.bottom;
  const totalW = frameW + 2 * mx, totalH = frameH + myTop + myBottom;
  // Centred in the box (standalone: the window; a dock: exactly the box measure() asked for).
  const fx = Math.round((canvas.width - totalW) / 2 + mx), fy = Math.round((canvas.height - totalH) / 2 + myTop);
  return {
    frame: { x: fx, y: fy, w: frameW, h: frameH, r },
    pic: { x: fx + b.left, y: fy + b.top, w: pic.w, h: pic.h, r: Math.max(0, r - Math.min(b.left, b.top)) },
    total: { w: totalW, h: totalH },
    shadow: sh,
  };
}

export function measure(canvas) {
  if (dock.standalone) return { main: canvas.width, cross: canvas.height };
  const b = borderWidths();
  const sh = shadowSpec();
  const pic = dockPicture(canvas);
  const w = pic.w + b.left + b.right + 2 * sh.size;
  const h = pic.h + b.top + b.bottom + 2 * sh.size + sh.offset;
  return dock.axis === 'vertical' ? { main: h, cross: w } : { main: w, cross: h };
}

// --- drawing ---------------------------------------------------------------------------------

function roundRectPath(canvas, x, y, w, h, r) {
  const p = canvas.path();
  r = Math.min(r, w / 2, h / 2);
  if (r <= 0) return p.rect(x, y, w, h);
  p.moveTo(x + r, y).lineTo(x + w - r, y).arc(x + w - r, y + r, r, -Math.PI / 2, 0, false)
    .lineTo(x + w, y + h - r).arc(x + w - r, y + h - r, r, 0, Math.PI / 2, false)
    .lineTo(x + r, y + h).arc(x + r, y + h - r, r, Math.PI / 2, Math.PI, false)
    .lineTo(x, y + r).arc(x + r, y + r, r, Math.PI, Math.PI * 1.5, false).close();
  return p;
}

// A stack of translucent rounded rectangles growing outward: a soft edge without a blur.
function drawShadow(canvas, f, sh) {
  if (sh.size <= 0 || sh.alpha <= 0) return;
  const step = sh.size / SHADOW_STEPS;
  const a = 1 - Math.pow(1 - sh.alpha, 1 / SHADOW_STEPS);  // each layer, so the centre reaches sh.alpha
  for (let i = SHADOW_STEPS; i >= 1; i--) {
    const d = i * step;
    canvas.fillRoundRect(f.x - d, f.y - d + sh.offset, f.w + 2 * d, f.h + 2 * d, f.r + d, `rgba(0, 0, 0, ${a})`);
  }
}

function drawBorder(canvas, f) {
  const b = String(settings.border ?? 'mat');
  if (b === 'none') return;
  const colour = transparent(settings.borderColor) ? '#FFFFFFFF' : settings.borderColor;
  if (b === 'line') {
    canvas.strokeRoundRect(f.x + 0.5, f.y + 0.5, f.w - 1, f.h - 1, Math.max(0, f.r - 0.5), colour, 1);
    return;
  }
  canvas.fillRoundRect(f.x, f.y, f.w, f.h, f.r, colour);
}

// The picture at `path` into the rect, covering it or fitting inside it, shifted by (dx, dy)
// (a slide) at `opacity`. False when it cannot be loaded (recorded in `failed`).
function drawPicture(canvas, path, p, dx, dy, opacity) {
  let size;
  try {
    size = canvas.imageSize(path);
  } catch (e) {
    failed.set(path, e.message);
    return false;
  }
  if (!(size.width > 0 && size.height > 0)) return false;
  if (path === current && ratio === 0) ratio = size.width / size.height;
  const cover = String(settings.fit ?? 'cover') !== 'contain';
  const k = cover ? Math.max(p.w / size.width, p.h / size.height) : Math.min(p.w / size.width, p.h / size.height);
  const w = size.width * k, h = size.height * k;
  const x = p.x + (p.w - w) / 2 + dx, y = p.y + (p.h - h) / 2 + dy;
  try {
    canvas.image(path, x, y, w, h, opacity);
  } catch (e) {
    failed.set(path, e.message);
    return false;
  }
  return true;
}

// The widget drawer's preview: a sample landscape (no file is read in a preview).
function drawSample(canvas, p) {
  canvas.fillRect(p.x, p.y, p.w, p.h, canvas.linearGradient(p.x, p.y, p.x, p.y + p.h, [[0, '#FF3A7BD5'], [0.6, '#FFB3D8F5'], [1, '#FFF2E2B8']]));
  canvas.fillCircle(p.x + p.w * 0.72, p.y + p.h * 0.3, Math.min(p.w, p.h) * 0.11, '#FFFFE07A');
  const hills = canvas.path();
  hills.moveTo(p.x, p.y + p.h).lineTo(p.x, p.y + p.h * 0.7).quadTo(p.x + p.w * 0.25, p.y + p.h * 0.35, p.x + p.w * 0.45, p.y + p.h * 0.72)
    .quadTo(p.x + p.w * 0.6, p.y + p.h * 0.5, p.x + p.w * 0.8, p.y + p.h * 0.68).lineTo(p.x + p.w, p.y + p.h * 0.6).lineTo(p.x + p.w, p.y + p.h).close();
  canvas.fill(hills, '#FF3C8D4A');
  const front = canvas.path();
  front.moveTo(p.x, p.y + p.h).lineTo(p.x, p.y + p.h * 0.85).quadTo(p.x + p.w * 0.4, p.y + p.h * 0.62, p.x + p.w, p.y + p.h * 0.82).lineTo(p.x + p.w, p.y + p.h).close();
  canvas.fill(front, '#FF2E6E3A');
}

// Nothing to show: a muted picture glyph and one line saying why.
function drawEmpty(canvas, p, text) {
  const ink = dock.theme.dateText;
  const s = Math.min(p.w, p.h);
  if (s >= 20) {
    const g = s * 0.4, gx = p.x + (p.w - g) / 2, gy = p.y + (p.h - g) / 2 - (p.h >= 40 ? s * 0.08 : 0);
    canvas.strokeRoundRect(gx + 0.5, gy + 0.5, g - 1, g - 1, 2, ink, 1);
    canvas.fillCircle(gx + g * 0.3, gy + g * 0.3, g * 0.09, ink);
    const m = canvas.path();
    m.moveTo(gx + 1, gy + g - 1).lineTo(gx + g * 0.4, gy + g * 0.5).lineTo(gx + g * 0.6, gy + g * 0.7).lineTo(gx + g * 0.75, gy + g * 0.58).lineTo(gx + g - 1, gy + g - 1).close();
    canvas.fill(m, ink);
  }
  if (p.h >= 40 && p.w >= 60) {
    canvas.text(text, p.x + p.w / 2, p.y + p.h * 0.5 + Math.min(p.w, p.h) * 0.2 + 2, { size: 10, align: 'center', baseline: 'top', color: ink });
  }
}

export function draw(canvas) {
  const L = layout(canvas);
  drawShadow(canvas, L.frame, L.shadow);
  drawBorder(canvas, L.frame);
  const p = L.pic;
  canvas.save();
  if (p.r > 0) canvas.clipPath(roundRectPath(canvas, p.x, p.y, p.w, p.h, p.r));
  else canvas.clip(p.x, p.y, p.w, p.h);
  if (!transparent(settings.background)) canvas.fillRect(p.x, p.y, p.w, p.h, settings.background);
  if (dock.preview) {
    drawSample(canvas, p);
  } else if (!current) {
    const folder = String(settings.folder ?? '').trim();
    const why = slideshow()
      ? (!folder ? 'Choose a folder' : failed.has('folder:' + folder) ? 'Cannot read the folder' : 'No pictures in the folder')
      : 'Choose a picture';
    drawEmpty(canvas, p, why);
  } else if (anim && previous) {
    // A transition: the old picture under or beside the new one.
    const t = ease(clamp((Date.now() - anim.start) / anim.ms, 0, 1));
    if (anim.kind === 'fade') {
      drawPicture(canvas, previous, p, 0, 0, 1);
      drawPicture(canvas, current, p, 0, 0, t);
    } else {
      const dir = anim.kind === 'slide-left' ? [-1, 0] : anim.kind === 'slide-right' ? [1, 0] : anim.kind === 'slide-top' ? [0, -1] : [0, 1];
      const ox = dir[0] * p.w, oy = dir[1] * p.h;
      drawPicture(canvas, previous, p, -ox * t, -oy * t, 1);
      drawPicture(canvas, current, p, ox * (1 - t), oy * (1 - t), 1);
    }
  } else if (!drawPicture(canvas, current, p, 0, 0, 1)) {
    drawEmpty(canvas, p, 'Cannot open the picture');
  }
  canvas.restore();
  // Decode the next picture now, so the change to it is instant (the host keeps a few decoded).
  if (slideshow() && !anim && files.length > 1 && !dock.preview) {
    const n = pick(index, 1);
    if (n >= 0 && files[n].path !== current) {
      try {
        canvas.imageSize(files[n].path);
      } catch (e) {
        failed.set(files[n].path, e.message);
      }
    }
  }
  if (settings.debugStore) {
    storage.set('state:' + dock.itemKey, { current, previous, index, count: files.length, cycle, anim: !!anim, ratio, frame: L.frame, pic: L.pic, total: L.total, box: { w: canvas.width, h: canvas.height } });
  }
}

// --- input -------------------------------------------------------------------------------------

function openCurrent() {
  if (!current) return;
  try {
    shell.open(current);
  } catch (e) {
    console.warn('photo-frame:', e.message);
  }
}

function openFolder() {
  const target = slideshow() ? String(settings.folder ?? '').trim() : current.replace(/[\\/][^\\/]*$/, '');
  if (!target) return;
  try {
    shell.open(target);
  } catch (e) {
    console.warn('photo-frame:', e.message);
  }
}

export function onClick() {
  const c = String(settings.click ?? 'next');
  if (c === 'none') return;
  if (c === 'open') return openCurrent();
  if (c === 'folder') return openFolder();
  if (slideshow() && files.length > 1) advance(1, true);
  else openCurrent();
}

export function onMiddleClick() {
  openCurrent();
}

export function onWheel(e) {
  if (!slideshow() || !settings.wheel || files.length < 2) return false;
  advance(e.delta < 0 ? 1 : -1, true);
  return true;
}

export function onHover(e) {
  hot = e.inside;
  if (slideshow() && settings.pauseOnHover) schedule();
}

export function onVisibilityChange(e) {
  if (!e.visible) {
    if (slideTimer) clearTimeout(slideTimer);
    slideTimer = 0;
    stopAnim();
    requestRedraw();
  } else {
    schedule();
  }
}

export function onSettingsChanged() {
  failed = new Map();
  sync();
}

sync();
