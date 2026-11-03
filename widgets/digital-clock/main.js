// A scripted copy of WinDock's own clock (src/Clock.cpp): the time in a strftime format (the
// host formats it: system.format), ":%S" added after the last minutes specifier with showSeconds,
// and the date ("%a %d %b") under the time in a smaller font. On a vertical dock every space
// breaks the line, as the built-in clock does. It wakes once a minute, or once a second when the
// format shows seconds (timer.setRefresh aligns the wake-ups to the boundary).
//
// Time zone (round 5): settings.timeZone is a Windows zone key ("Tokyo Standard Time"; "" local);
// every system.format / setRefresh call passes it, so the host converts the time with the zone's
// own rules. Every zone is a whole number of minutes from UTC, so the minute / second wake-ups are
// the same instants in every zone. settings.label goes on the date's line ("Tokyo · Tue 03 Nov"),
// or alone under the time without the date, as the built-in clock draws its label.

const DATE_FORMAT = '%a %d %b';

// Any specifier whose output includes seconds (as Clock::FormatHasSeconds).
function hasSeconds(format) {
  for (let i = 0; i + 1 < format.length; i++) {
    if (format[i] !== '%') continue;
    let j = i + 1;
    if (format[j] === '#' && j + 1 < format.length) j++;
    if ('STXcr'.includes(format[j])) return true;
    i = j; // also skips "%%"
  }
  return false;
}

// clock.format with showSeconds applied (as Clock::EffectiveFormat).
function effectiveFormat(format, seconds) {
  if (!seconds || !format || hasSeconds(format)) return format;
  let after = -1; // just past the last minutes specifier
  for (let i = 0; i + 1 < format.length; i++) {
    if (format[i] !== '%') continue;
    let j = i + 1;
    if (format[j] === '#' && j + 1 < format.length) j++;
    if (format[j] === 'M' || format[j] === 'R') after = j + 1;
    i = j;
  }
  return after < 0 ? format + ':%S' : format.slice(0, after) + ':%S' + format.slice(after);
}

let format = '%H:%M';
let zone = '';

function configure() {
  format = effectiveFormat(settings.format || '%H:%M', settings.showSeconds);
  zone = settings.timeZone || '';
  timer.setRefresh(hasSeconds(format) ? 1000 : 60000, { timeZone: zone });
  if (zone) {
    // One line per (re)configuration: which zone and what it shows (tests read it in the Console).
    const t = system.localTime(zone);
    if (!t.timeZone) {
      console.warn('unknown time zone "' + zone + '": showing the local time');
    } else {
      const off = t.utcOffset, a = Math.abs(off);
      const utc = 'UTC' + (off < 0 ? '-' : '+') + String(Math.floor(a / 60)).padStart(2, '0') + ':' + String(a % 60).padStart(2, '0');
      console.log('time zone ' + t.timeZone + ' (' + utc + '): ' + system.format('%Y-%m-%d %H:%M', zone));
    }
  }
}
configure();

export function onSettingsChanged() {
  configure();
}

function lines(dock) {
  let time = system.format(format, zone);
  let date = settings.showDate ? system.format(DATE_FORMAT, zone) : '';
  if (settings.label) date = date ? settings.label + ' · ' + date : settings.label;
  if (dock.axis === 'vertical') {
    time = time.replaceAll(' ', '\n');
    date = date.replaceAll(' ', '\n');
  }
  return { time, date };
}

const timeStyle = () => ({ size: settings.showDate || settings.label ? 14 : 16, weight: 'semibold', align: 'center' });
const dateStyle = { size: 11, align: 'center' };

export function measure(canvas, dock) {
  const t = lines(dock);
  const a = canvas.measureText(t.time, timeStyle());
  const b = t.date ? canvas.measureText(t.date, dateStyle) : { width: 0, height: 0 };
  const width = Math.ceil(Math.max(a.width, b.width));
  const height = Math.ceil(a.height + b.height);
  return dock.axis === 'vertical' ? { main: height, cross: width } : { main: width, cross: height };
}

export function draw(canvas, dock) {
  const t = lines(dock);
  const style = timeStyle();
  const a = canvas.measureText(t.time, style);
  canvas.text(t.time, canvas.width / 2, 0, { ...style, color: dock.theme.text });
  if (t.date) canvas.text(t.date, canvas.width / 2, a.height, { ...dateStyle, color: dock.theme.dateText });
}
