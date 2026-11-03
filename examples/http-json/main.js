// Fetching a public JSON API: Open-Meteo's air quality (https://open-meteo.com, no key) for one
// place, shown as "AQI 23" with a coloured dot. What it shows:
// - The manifest's permission { "http": ["air-quality-api.open-meteo.com"] }: requests to any other
//   host throw a TypeError at the call (try changing URL below).
// - http.get(url, { type: 'json' }) returns a promise; the answer arrives on the dock's thread
//   later, so the widget keeps what it got in module variables and asks for a redraw.
// - timer.setRefresh(ms) calls tick() on a fixed cadence (aligned to the clock, one wake-up per
//   refresh, nothing in between); tick() returns false: the fetch's answer redraws, not the tick.
// - storage keeps the last answer, so a dock restart shows it at once (dimmed until it is fresh).

const HOST = 'https://air-quality-api.open-meteo.com';

let value = storage.get('aqi', null);   // the last index, kept across restarts
let fresh = false;                      // fetched by this run
let error = '';

function url() {
  const lat = Number(settings.latitude).toFixed(3), lon = Number(settings.longitude).toFixed(3);
  return `${HOST}/v1/air-quality?latitude=${lat}&longitude=${lon}&current=european_aqi`;
}

async function fetchNow() {
  try {
    const r = await http.get(url(), { type: 'json', timeout: 10000 });
    if (!r.ok) throw new Error('HTTP ' + r.status);
    const v = r.json && r.json.current ? r.json.current.european_aqi : null;
    if (typeof v !== 'number') throw new Error('no european_aqi in the answer');
    value = v;
    fresh = true;
    error = '';
    storage.set('aqi', v);
  } catch (e) {
    error = String(e.message || e);
    console.warn('air quality:', error);  // the Console shows it
  }
  requestRedraw();
}

function schedule() {
  timer.setRefresh(Math.max(10, settings.minutes || 60) * 60000);
}
schedule();
fetchNow();

export function tick() {
  fetchNow();
  return false;  // nothing changed yet: the answer redraws
}

export function onSettingsChanged() {
  schedule();
  fresh = false;
  fetchNow();
}

// The European AQI bands' colours (good ... extremely poor).
function band(v) {
  if (v <= 20) return '#FF50F0E6';
  if (v <= 40) return '#FF50CCAA';
  if (v <= 60) return '#FFF0E641';
  if (v <= 80) return '#FFFF5050';
  if (v <= 100) return '#FF960032';
  return '#FF7D2181';
}

const label = () => (value === null ? 'AQI –' : `AQI ${Math.round(value)}`);
const style = { size: 13, weight: 'semibold', baseline: 'middle' };

export function measure(canvas, dock) {
  const w = Math.ceil(canvas.measureText(label(), style).width) + 18;
  return dock.axis === 'vertical' ? { main: 40, cross: Math.max(dock.iconSize, w) } : { main: w, cross: 32 };
}

export function draw(canvas, dock) {
  const y = canvas.height / 2;
  canvas.opacity(fresh ? 1 : 0.6);  // the stored value until the first answer, dimmed
  if (value !== null) canvas.fillCircle(6, y, 4, band(value));
  canvas.text(label(), 14, y, { ...style, color: error && !fresh ? dock.theme.dateText : dock.theme.text });
}

export function onClick() {
  fetchNow();  // a click fetches now
}
