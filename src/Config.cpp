#include "Config.h"

#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <iterator>

#include "Hotkey.h"
#include "JsonUtil.h"
#include "Theme.h"

using namespace JsonUtil;

namespace {

template <typename E, size_t N>
const wchar_t* EnumToString(E v, const wchar_t* const (&names)[N]) {
    const size_t i = static_cast<size_t>(v);
    return i < N ? names[i] : names[0];
}

template <typename E, size_t N>
bool EnumFromString(std::wstring_view s, E& v, const wchar_t* const (&names)[N]) {
    for (size_t i = 0; i < N; ++i) {
        const size_t len = std::wcslen(names[i]);
        if (s.size() == len && (len == 0 || CompareStringOrdinal(s.data(), static_cast<int>(len), names[i],
                                                                 static_cast<int>(len), TRUE) == CSTR_EQUAL)) {
            v = static_cast<E>(i);
            return true;
        }
    }
    return false;
}

// In enum order.
const wchar_t* const kAutoHideNames[] = {L"off", L"always", L"intelligent"};
const wchar_t* const kHideStyleNames[] = {L"slide", L"slideOnly", L"fade", L"none"};
const wchar_t* const kLaunchStyleNames[] = {L"bounce", L"hop", L"pulse", L"none"};
const wchar_t* const kFalloffNames[] = {L"cosine", L"linear"};
const wchar_t* const kEasingNames[] = {L"easeOut", L"linear", L"spring"};
const wchar_t* const kReduceMotionNames[] = {L"system", L"on", L"off"};
const wchar_t* const kUnitsNames[] = {L"metric", L"imperial"};
const wchar_t* const kMiddleClickNames[] = {L"newInstance", L"minimize", L"close", L"none"};
const wchar_t* const kWheelNames[] = {L"none", L"cycle"};
const wchar_t* const kClockClickNames[] = {L"calendar", L"launch", L"none"};
const wchar_t* const kEdgeNames[] = {L"bottom", L"top", L"left", L"right"};
const wchar_t* const kAlignNames[] = {L"center", L"start", L"end"};
const wchar_t* const kRunningFilterNames[] = {L"all", L"monitor"};
const wchar_t* const kLayerNames[] = {L"normal", L"desktop", L"behind"};
const wchar_t* const kWindowsModeNames[] = {L"stacked", L"unstacked"};
const wchar_t* const kTitlesNames[] = {L"off", L"one", L"two"};
const wchar_t* const kTitlesVerticalNames[] = {L"off", L"beside", L"under"};
const wchar_t* const kThumbnailNames[] = {L"off", L"live", L"snapshots"};
const wchar_t* const kRunningOrderNames[] = {L"firstSeen", L"name"};  // round 13 phase 13D
const wchar_t* const kOverflowNames[] = {L"scroll", L"shrink", L"clip"};
const wchar_t* const kMagnifyBarNames[] = {L"steady", L"follow", L"always"};  // round 9 phase 9A
const wchar_t* const kShapeNames[] = {L"bar", L"arc", L"circle"};
const wchar_t* const kItemViewNames[] = {L"auto", L"grid", L"list"};
const wchar_t* const kItemAnchorNames[] = {L"none", L"start", L"center", L"end"};
const wchar_t* const kFolderSortNames[] = {L"name", L"date", L"type", L"size"};
const wchar_t* const kWidgetBackgroundNames[] = {L"none", L"panel"};
const wchar_t* const kItemKindNames[] = {L"",        L"app",   L"folder", L"separator",
                                         L"recycleBin", L"running", L"clock", L"weather", L"widget", L"group",
                                         L"tray",    L"trayApp", L"notifications"};

// The keys of a dock object; before `docks` existed (legacy files) they were top-level keys.
void ReadDockFields(const json::JsonObject& o, DockConfig& d) {
    ReadInt(o, L"iconSize", d.iconSize);
    ReadNumber(o, L"magnification", d.magnification);
    ReadInt(o, L"magnifyRange", d.magnifyRange);
    // Older versions wrote a bool (true = "always").
    if (auto a = Find(o, L"autoHide")) {
        if (a.ValueType() == json::JsonValueType::Boolean) {
            d.autoHide = a.GetBoolean() ? AutoHideMode::Always : AutoHideMode::Off;
        } else if (a.ValueType() == json::JsonValueType::String) {
            if (!FromString(std::wstring(a.GetString()), d.autoHide)) d.autoHide = AutoHideMode::Always;
        }
    }
    ReadInt(o, L"hideDelayMs", d.hideDelayMs);
    ReadInt(o, L"revealDelayMs", d.revealDelayMs);
    ReadBool(o, L"previews", d.previews);
    ReadInt(o, L"previewDelayMs", d.previewDelayMs);
    ReadInt(o, L"monitor", d.monitor);
}

// The keys that exist only in dock objects (never in legacy files).
void ReadDockOnlyFields(const json::JsonObject& o, DockConfig& d) {
    ReadBool(o, L"previewsSingleWindow", d.previewSingle);
    std::wstring s;
    ReadString(o, L"middleClick", s);
    if (!s.empty()) FromString(s, d.middleClick);  // unknown text: keep the default
    s.clear();
    ReadString(o, L"wheel", s);
    if (!s.empty()) FromString(s, d.wheel);
    s.clear();
    ReadString(o, L"edge", s);
    if (!s.empty() && !FromString(s, d.edge)) d.edge = DockEdge::Bottom;
    s.clear();
    ReadString(o, L"align", s);
    if (!s.empty()) FromString(s, d.align);  // unknown text: center
    ReadInt(o, L"offset", d.offset);
    ReadInt(o, L"edgeGap", d.edgeGap);  // round 12 phase 12E
    ReadBool(o, L"fullWidth", d.fullWidth);
    ReadBool(o, L"reserveSpace", d.reserveSpace);
    ReadInt(o, L"peekPx", d.peekPx);
    ReadNumber(o, L"peekOpacity", d.peekOpacity);
    ReadInt(o, L"maxLength", d.maxLength);  // round 7 phase 7A
    ReadInt(o, L"length", d.length);        // round 14 phase 14A
    ReadInt(o, L"thickness", d.thickness);  // round 14 phase 14A
    ReadInt(o, L"popupGap", d.popupGap);
    s.clear();
    ReadString(o, L"magnifyBar", s);  // round 9 phase 9A
    if (!s.empty()) FromString(s, d.magnifyBar);  // unknown text: steady
    ReadString(o, L"title", d.title);
    // "monitor": a number (ReadDockFields), or "primary" / "each".
    if (auto m = Find(o, L"monitor"); m && m.ValueType() == json::JsonValueType::String) {
        const std::wstring v(m.GetString());
        if (CompareStringOrdinal(v.c_str(), -1, L"each", -1, TRUE) == CSTR_EQUAL) {
            d.monitor = DockConfig::kMonitorEach;
        } else if (CompareStringOrdinal(v.c_str(), -1, L"primary", -1, TRUE) == CSTR_EQUAL) {
            d.monitor = DockConfig::kMonitorPrimary;
        }
    }
    // Round 13 phase 13D: runningFilter / windows / titles / titlesVertical / thumbnails* / badges
    // are the running item's now (MoveOldRunningKeys, after the items).
    ReadString(o, L"themePreset", d.themePreset);
    s.clear();
    ReadString(o, L"layer", s);
    if (!s.empty()) FromString(s, d.layer);  // unknown text: normal
    s.clear();
    ReadString(o, L"overflow", s);
    if (!s.empty()) FromString(s, d.overflow);  // unknown text: scroll
    s.clear();
    ReadString(o, L"shape", s);
    if (!s.empty()) FromString(s, d.shape);  // unknown text: bar
    ReadInt(o, L"radius", d.radius);
    ReadBool(o, L"rotateIcons", d.rotateIcons);
    ReadInt(o, L"x", d.x);
    ReadInt(o, L"y", d.y);
    ReadBool(o, L"floating", d.floating);
    // --- round 12 phase 12C: the name and the title options (unknown text keeps the default) ---
    ReadString(o, L"name", d.name);
    s.clear();
    ReadString(o, L"titleShow", s);
    if (CompareStringOrdinal(s.c_str(), -1, L"off", -1, TRUE) == CSTR_EQUAL) d.titleShow = TitleShow::Off;
    if (CompareStringOrdinal(s.c_str(), -1, L"hover", -1, TRUE) == CSTR_EQUAL) d.titleShow = TitleShow::Hover;
    if (CompareStringOrdinal(s.c_str(), -1, L"always", -1, TRUE) == CSTR_EQUAL) d.titleShow = TitleShow::Always;
    s.clear();
    ReadString(o, L"titlePlace", s);
    if (CompareStringOrdinal(s.c_str(), -1, L"inside", -1, TRUE) == CSTR_EQUAL) d.titlePlace = TitlePlace::Inside;
    if (CompareStringOrdinal(s.c_str(), -1, L"outside", -1, TRUE) == CSTR_EQUAL) d.titlePlace = TitlePlace::Outside;
    ReadBool(o, L"titleUseName", d.titleUseName);
    // --- end round 12 phase 12C ---
}

// docks[].monitor as JSON: the index, or "primary" / "each".
std::wstring MonitorJson(int monitor) {
    if (monitor == DockConfig::kMonitorEach) return L"\"each\"";
    if (monitor == DockConfig::kMonitorPrimary) return L"\"primary\"";
    return std::to_wstring(monitor);
}

// The path names a directory (environment variables expanded). A .lnk to a folder is not one:
// it stays an "app" item (the resolver still shows it as a folder).
bool IsDirectoryPath(const std::wstring& path) {
    if (path.empty()) return false;
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = ExpandEnvironmentStringsW(path.c_str(), buf, static_cast<DWORD>(std::size(buf)));
    const std::wstring expanded = n > 0 && n <= std::size(buf) ? std::wstring(buf) : path;
    const DWORD attr = GetFileAttributesW(expanded.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

// Latitude / longitude: 4 decimals (~11 m), no exponent, trailing zeros dropped.
std::wstring Coord(double v) {
    wchar_t buf[32];
    swprintf_s(buf, L"%.4f", v);
    std::wstring s = buf;
    while (!s.empty() && s.back() == L'0') s.pop_back();
    if (!s.empty() && s.back() == L'.') s.pop_back();
    if (s == L"-0") s = L"0";
    return s;
}

template <typename E>
void ReadEnum(const json::JsonObject& o, const wchar_t* key, E& v) {
    std::wstring s;
    ReadString(o, key, s);
    if (!s.empty()) FromString(s, v);  // unknown text: keep the default
}

// The keys of a clock object: the top-level defaults and a clock item's settings (round 5).
void ReadClockFields(const json::JsonObject& o, ClockConfig& c) {
    ReadString(o, L"format", c.format);
    ReadBool(o, L"showSeconds", c.showSeconds);
    ReadBool(o, L"showDate", c.showDate);
    std::wstring click;
    ReadString(o, L"click", click);
    if (!click.empty()) {
        FromString(click, c.click);  // unknown text: keep the default
    } else if (auto p = Find(o, L"calendarPopup"); p && p.ValueType() == json::JsonValueType::Boolean) {
        if (!p.GetBoolean()) c.click = ClockClick::None;  // before "click": a bool for the calendar popup
    }
    ReadString(o, L"clickTarget", c.clickTarget);
    ReadString(o, L"face", c.face);
    ReadString(o, L"timeZone", c.timeZone);
    ReadString(o, L"label", c.label);
}

void ReadWeatherFields(const json::JsonObject& o, WeatherConfig& w) {
    ReadNumber(o, L"latitude", w.latitude);
    ReadNumber(o, L"longitude", w.longitude);
    ReadString(o, L"location", w.location);
    ReadEnum(o, L"units", w.units);
    ReadInt(o, L"refreshMinutes", w.refreshMinutes);
    ReadString(o, L"label", w.label);
}

// Control characters (a pasted line break) out of a single-line text, then at most `max` chars.
void SingleLine(std::wstring& s, size_t max) {
    std::erase_if(s, [](wchar_t ch) { return ch < 0x20 || ch == 0x7F; });
    if (s.size() > max) s.resize(max);
}

// The theme's values in range (Theme::ReadFields clamps what it reads; this also covers values set in
// code). Round 12 phase 12B: split out of Sanitize for the docks' own looks.
void SanitizeTheme(ThemeConfig& theme) {
    if (!(theme.cornerRadius >= 0.0f)) theme.cornerRadius = 0.0f;
    theme.cornerRadius = std::min(theme.cornerRadius, 64.0f);
    if (!(theme.labelCornerRadius >= 0.0f)) theme.labelCornerRadius = 0.0f;
    theme.labelCornerRadius = std::min(theme.labelCornerRadius, 32.0f);
    if (!(theme.shelfSlant >= 0.0f)) theme.shelfSlant = 0.0f;
    theme.shelfSlant = std::min(theme.shelfSlant, 64.0f);
    auto clampf = [](float& v, float lo, float hi, float fallback) {
        v = std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
    };
    if (theme.glow < 0 || theme.glow > 3) theme.glow = 0;
    clampf(theme.glowRadius, 4.0f, 32.0f, 14.0f);
    clampf(theme.glowStrength, 0.0f, 1.0f, 0.6f);
    clampf(theme.reflectionHeight, 0.2f, 1.0f, 0.5f);
    clampf(theme.reflectionOpacity, 0.0f, 1.0f, 0.35f);
    clampf(theme.perspectiveAngle, 0.0f, 35.0f, 18.0f);
    // Round 11 phase 11A.
    if (theme.slotStyle < 0 || theme.slotStyle > 1) theme.slotStyle = 0;
    clampf(theme.slotCornerRadius, 0.0f, 16.0f, 3.0f);
    clampf(theme.sheen, 0.0f, 1.0f, 0.0f);
    clampf(theme.iconGap, 0.0f, 48.0f, 8.0f);
    clampf(theme.barPadding, 0.0f, 24.0f, 8.0f);
}

void SanitizeClock(ClockConfig& c) {
    if (c.format.size() > 128) c.format.resize(128);
    if (c.format.empty()) c.format = L"%H:%M";  // an empty format would hide the clock
    if (c.clickTarget.size() > 2048) c.clickTarget.resize(2048);
    if (c.face.empty() || c.face.size() > 64) c.face = L"digital";
    if (static_cast<unsigned>(c.click) > static_cast<unsigned>(ClockClick::None)) c.click = ClockClick::Calendar;
    SingleLine(c.timeZone, 128);
    SingleLine(c.label, 64);
}

void SanitizeWeather(WeatherConfig& w) {
    if (!(w.latitude >= -90.0 && w.latitude <= 90.0)) w.latitude = 0.0;  // also NaN
    if (!(w.longitude >= -180.0 && w.longitude <= 180.0)) w.longitude = 0.0;
    w.latitude = std::round(w.latitude * 1e4) / 1e4;  // what Save writes (4 decimals)
    w.longitude = std::round(w.longitude * 1e4) / 1e4;
    if (w.latitude == 0.0) w.latitude = 0.0;  // no -0
    if (w.longitude == 0.0) w.longitude = 0.0;
    w.refreshMinutes = std::clamp(w.refreshMinutes, 5, 360);
    if (static_cast<unsigned>(w.units) > static_cast<unsigned>(Units::Imperial)) w.units = Units::Metric;
    SingleLine(w.location, 128);
    SingleLine(w.label, 64);
}

// An item's settings text as an object (null: empty or not an object).
json::JsonObject SettingsObject(const std::wstring& text) {
    if (text.empty()) return nullptr;
    try {
        json::JsonObject o{nullptr};
        if (json::JsonObject::TryParse(text, o) && o) return o;
    } catch (...) {
    }
    return nullptr;
}

// --- round 13 phase 13D: a running item's settings ---
void ReadRunningFields(const json::JsonObject& o, RunningConfig& r) {
    ReadEnum(o, L"filter", r.filter);  // unknown text keeps the default
    ReadEnum(o, L"windows", r.windows);
    ReadEnum(o, L"titles", r.titles);
    ReadEnum(o, L"titlesVertical", r.titlesVertical);
    ReadEnum(o, L"thumbnails", r.thumbnails);
    ReadInt(o, L"thumbnailIntervalS", r.thumbnailIntervalS);
    ReadInt(o, L"thumbnailSize", r.thumbnailSize);
    ReadInt(o, L"thumbnailAcross", r.thumbnailAcross);  // round 14 phase 14D
    ReadInt(o, L"thumbnailFps", r.thumbnailFps);
    ReadBool(o, L"badges", r.badges);
    ReadInt(o, L"gap", r.gap);
    ReadInt(o, L"padding", r.padding);
    ReadEnum(o, L"order", r.order);
}

void SanitizeRunning(RunningConfig& r) {
    if (static_cast<unsigned>(r.filter) > static_cast<unsigned>(RunningFilter::Monitor)) r.filter = RunningFilter::All;
    if (static_cast<unsigned>(r.windows) > static_cast<unsigned>(WindowsMode::Unstacked)) r.windows = WindowsMode::Stacked;
    if (static_cast<unsigned>(r.titles) > static_cast<unsigned>(TitlesMode::Two)) r.titles = TitlesMode::Off;
    if (static_cast<unsigned>(r.titlesVertical) > static_cast<unsigned>(TitlesVertical::Under)) {
        r.titlesVertical = TitlesVertical::Under;
    }
    if (static_cast<unsigned>(r.thumbnails) > static_cast<unsigned>(ThumbnailMode::Snapshots)) r.thumbnails = ThumbnailMode::Off;
    r.thumbnailIntervalS = std::clamp(r.thumbnailIntervalS, 1, 3600);
    r.thumbnailSize = r.thumbnailSize <= 0 ? 0 : std::clamp(r.thumbnailSize, 16, 512);
    r.thumbnailAcross = r.thumbnailAcross <= 0 ? 0 : std::clamp(r.thumbnailAcross, 16, 512);  // round 14 phase 14D
    r.thumbnailFps = std::clamp(r.thumbnailFps, 1, 60);
    r.gap = r.gap < 0 ? RunningConfig::kThemeGap : std::min(r.gap, 48);
    r.padding = std::clamp(r.padding, 0, 64);
    if (static_cast<unsigned>(r.order) > static_cast<unsigned>(RunningOrder::Name)) r.order = RunningOrder::FirstSeen;
}

// Before round 13 phase 13D these were docks[] keys (the dock key -> the running item's key).
// A dock object that carries any of them: they go into the first running item's settings, each
// only when that object does not have it already (its own value wins); a dock without a running
// item drops them (they describe running windows; recorded in STATUS.md). Save never writes them
// at the dock level again. True when the object had one (the caller saves the moved form).
bool MoveOldRunningKeys(const json::JsonObject& obj, DockConfig& d) {
    static constexpr std::pair<const wchar_t*, const wchar_t*> kKeys[] = {
        {L"runningFilter", L"filter"},
        {L"windows", L"windows"},
        {L"titles", L"titles"},
        {L"titlesVertical", L"titlesVertical"},
        {L"thumbnails", L"thumbnails"},
        {L"thumbnailIntervalS", L"thumbnailIntervalS"},
        {L"thumbnailSize", L"thumbnailSize"},
        {L"thumbnailFps", L"thumbnailFps"},
        {L"badges", L"badges"},
    };
    DockItemEntry* running = nullptr;
    for (DockItemEntry& e : d.items) {
        if (e.Kind() == ItemKind::Running) {
            running = &e;
            break;
        }
    }
    bool any = false;
    json::JsonObject settings{nullptr};
    for (const auto& [from, to] : kKeys) {
        const json::IJsonValue v = Find(obj, from);
        if (!v) continue;
        any = true;
        if (!running) continue;
        if (!settings) {
            settings = SettingsObject(running->settings);
            if (!settings) settings = json::JsonObject();
        }
        // A copy: a value belongs to one object.
        if (!settings.HasKey(to)) settings.SetNamedValue(to, json::JsonValue::Parse(v.Stringify()));
    }
    if (running && settings) running->settings = std::wstring(settings.Stringify());
    return any;
}

// A "folderWindow" docks[] entry (round 4 phase 12): its own keys; the dock keys are not read.
void ReadFolderWindowFields(const json::JsonObject& o, DockConfig& d) {
    FolderWindowConfig& f = d.folder;
    ReadString(o, L"path", f.path);
    ReadString(o, L"title", d.title);
    ReadInt(o, L"x", d.x);
    ReadInt(o, L"y", d.y);
    ReadInt(o, L"w", f.w);
    ReadInt(o, L"h", f.h);
    ReadEnum(o, L"view", f.view);
    ReadInt(o, L"iconSize", f.iconSize);
    ReadEnum(o, L"sort", f.sort);
    ReadBool(o, L"showHidden", f.showHidden);
    ReadBool(o, L"collapsed", f.collapsed);
    ReadBool(o, L"singleClick", f.singleClick);
    ReadString(o, L"rules", f.rules);
    ReadBool(o, L"autoSort", f.autoSort);
    ReadNumber(o, L"opacity", f.opacity);
    std::wstring color;
    ReadString(o, L"titleColor", color);
    if (!color.empty() && !ParseArgbColor(color, f.titleColor)) f.titleColor = 0;
    ReadBool(o, L"hideTitle", f.hideTitle);
    ReadInt(o, L"labelLines", f.labelLines);
    ReadBool(o, L"transparentBackground", f.transparentBackground);
    ReadBool(o, L"rollUpOnHover", f.rollUpOnHover);
    d.monitor = 0;
    if (auto m = Find(o, L"monitor")) {
        if (m.ValueType() == json::JsonValueType::String) {
            d.monitor = DockConfig::kMonitorPrimary;  // "primary" (a folder window is never "each")
        } else {
            ReadInt(o, L"monitor", d.monitor);
            if (d.monitor < 0) d.monitor = DockConfig::kMonitorPrimary;
        }
    }
}

// A "widget" docks[] entry (round 6 phase 6B): the widget (as items[0]), its window's keys and the
// DockConfig keys it shares with folder windows; the dock keys are not read.
void ReadWidgetWindowFields(const json::JsonObject& o, DockConfig& d) {
    WidgetWindowConfig& w = d.widgetWindow;
    DockItemEntry e;
    e.kind = ToString(ItemKind::Widget);
    ReadString(o, L"widget", e.id);
    if (auto so = ReadObject(o, L"settings")) e.settings = std::wstring(so.Stringify());
    d.items = {std::move(e)};
    ReadInt(o, L"x", d.x);
    ReadInt(o, L"y", d.y);
    ReadInt(o, L"w", w.w);
    ReadInt(o, L"h", w.h);
    ReadEnum(o, L"background", w.background);
    ReadBool(o, L"locked", d.locked);
    std::wstring color;
    ReadString(o, L"color", color);
    if (!color.empty() && !ParseArgbColor(color, d.color)) d.color = 0;
    d.layer = DockLayer::Behind;  // this kind's default
    ReadEnum(o, L"layer", d.layer);
    d.monitor = 0;
    if (auto m = Find(o, L"monitor")) {
        if (m.ValueType() == json::JsonValueType::String) {
            d.monitor = DockConfig::kMonitorPrimary;  // "primary" (never "each")
        } else {
            ReadInt(o, L"monitor", d.monitor);
            if (d.monitor < 0) d.monitor = DockConfig::kMonitorPrimary;
        }
    }
}

// A docks[] entry of kind "dock" (its keys over `d`; the items when "items" is an array). Also a
// preset's dock (round 9 phase 9C: ReadDockJson).
// Round 13 phase 13D: `oldKeys` (when given) is set when the object carried a pre-13D running key
// (MoveOldRunningKeys).
void ReadDockItems(const json::JsonObject& obj, DockConfig& d);
void ReadDockObject(const json::JsonObject& obj, DockConfig& d, bool* oldKeys = nullptr) {
    ReadDockItems(obj, d);
    if (MoveOldRunningKeys(obj, d) && oldKeys) *oldKeys = true;
}

void ReadDockItems(const json::JsonObject& obj, DockConfig& d) {
    ReadBool(obj, L"locked", d.locked);
    std::wstring color;
    ReadString(obj, L"color", color);
    if (!color.empty() && !ParseArgbColor(color, d.color)) d.color = 0;
    ReadDockFields(obj, d);
    ReadDockOnlyFields(obj, d);
    // Round 12 phase 12B: the dock's own inline theme fields (only with a themePreset; Sanitize drops
    // them otherwise). Not in ReadDockFields: that also reads a legacy file's root, whose "theme"
    // is the app's.
    if (auto t = ReadObject(obj, L"theme")) {
        d.themeOverrides = 0;
        Theme::ReadFields(t, d.theme, &d.themeOverrides);
    }
    auto items = ReadArray(obj, L"items");
    if (!items) return;
    d.items.clear();
    for (const auto& iv : items) {
        if (iv.ValueType() != json::JsonValueType::Object) continue;
        const json::JsonObject io = iv.GetObject();
        DockItemEntry e;
        e.kind.clear();
        ReadString(io, L"kind", e.kind);
        ReadString(io, L"path", e.path);
        ReadString(io, L"aumid", e.aumid);
        ReadString(io, L"icon", e.icon);
        ReadString(io, L"id", e.id);
        if (auto so = ReadObject(io, L"settings")) e.settings = std::wstring(so.Stringify());
        ReadEnum(io, L"view", e.view);
        ReadEnum(io, L"anchor", e.anchor);  // round 7 phase 7B (unknown text: none)
        ReadInt(io, L"offset", e.offset);
        if (e.Kind() == ItemKind::Tray) {
            ReadInt(io, L"rows", e.rows);
            ReadInt(io, L"columns", e.columns);
            ReadInt(io, L"iconSize", e.trayIconSize);
            ReadStrings(io, L"hidden", e.hidden);
            ReadStrings(io, L"order", e.order);
        }
        if (e.Kind() == ItemKind::Group) {
            ReadString(io, L"name", e.name);
            if (auto members = ReadArray(io, L"items")) {
                for (const auto& mv : members) {
                    if (mv.ValueType() != json::JsonValueType::Object) continue;
                    const json::JsonObject mo = mv.GetObject();
                    DockItemEntry m;
                    m.kind.clear();
                    ReadString(mo, L"kind", m.kind);
                    ReadString(mo, L"path", m.path);
                    ReadString(mo, L"aumid", m.aumid);
                    ReadString(mo, L"icon", m.icon);
                    if (m.kind.empty()) m.kind = L"app";
                    const ItemKind mk = m.Kind();
                    // Members are shortcuts: apps and folders with a target.
                    if ((mk != ItemKind::App && mk != ItemKind::Folder) || (m.path.empty() && m.aumid.empty())) {
                        continue;
                    }
                    e.members.push_back(std::move(m));
                }
            }
        }
        const bool target = !e.path.empty() || !e.aumid.empty();
        if (e.kind.empty()) {
            if (!target) continue;
            e.kind = L"app";  // a hand-written entry without a kind
        }
        const ItemKind k = e.Kind();
        if ((k == ItemKind::App || k == ItemKind::Folder) && !target) continue;  // nothing to show
        if ((k == ItemKind::Widget || k == ItemKind::TrayApp) && e.id.empty()) continue;  // which one?
        d.items.push_back(std::move(e));
    }
}

}  // namespace

bool ReadDockJson(const std::wstring& text, DockConfig& d) {
    try {
        json::JsonObject o{nullptr};
        if (!json::JsonObject::TryParse(text, o) || !o) return false;
        ReadDockObject(o, d);
        return true;
    } catch (...) {
        return false;
    }
}

const wchar_t* ToString(AutoHideMode v) { return EnumToString(v, kAutoHideNames); }
const wchar_t* ToString(HideStyle v) { return EnumToString(v, kHideStyleNames); }
const wchar_t* ToString(LaunchStyle v) { return EnumToString(v, kLaunchStyleNames); }
const wchar_t* ToString(Falloff v) { return EnumToString(v, kFalloffNames); }
const wchar_t* ToString(Easing v) { return EnumToString(v, kEasingNames); }
const wchar_t* ToString(ReduceMotion v) { return EnumToString(v, kReduceMotionNames); }
const wchar_t* ToString(Units v) { return EnumToString(v, kUnitsNames); }
const wchar_t* ToString(MiddleClickAction v) { return EnumToString(v, kMiddleClickNames); }
const wchar_t* ToString(WheelAction v) { return EnumToString(v, kWheelNames); }
const wchar_t* ToString(ClockClick v) { return EnumToString(v, kClockClickNames); }
const wchar_t* ToString(DockEdge v) { return EnumToString(v, kEdgeNames); }
const wchar_t* ToString(DockAlign v) { return EnumToString(v, kAlignNames); }
const wchar_t* ToString(RunningFilter v) { return EnumToString(v, kRunningFilterNames); }
const wchar_t* ToString(DockLayer v) { return EnumToString(v, kLayerNames); }
bool FromString(std::wstring_view s, RunningFilter& v) { return EnumFromString(s, v, kRunningFilterNames); }
bool FromString(std::wstring_view s, DockLayer& v) { return EnumFromString(s, v, kLayerNames); }
const wchar_t* ToString(WindowsMode v) { return EnumToString(v, kWindowsModeNames); }
const wchar_t* ToString(TitlesMode v) { return EnumToString(v, kTitlesNames); }
const wchar_t* ToString(TitlesVertical v) { return EnumToString(v, kTitlesVerticalNames); }
bool FromString(std::wstring_view s, WindowsMode& v) { return EnumFromString(s, v, kWindowsModeNames); }
bool FromString(std::wstring_view s, TitlesMode& v) { return EnumFromString(s, v, kTitlesNames); }
bool FromString(std::wstring_view s, TitlesVertical& v) { return EnumFromString(s, v, kTitlesVerticalNames); }
const wchar_t* ToString(ThumbnailMode v) { return EnumToString(v, kThumbnailNames); }
bool FromString(std::wstring_view s, ThumbnailMode& v) { return EnumFromString(s, v, kThumbnailNames); }
const wchar_t* ToString(OverflowMode v) { return EnumToString(v, kOverflowNames); }
bool FromString(std::wstring_view s, OverflowMode& v) { return EnumFromString(s, v, kOverflowNames); }
const wchar_t* ToString(MagnifyBar v) { return EnumToString(v, kMagnifyBarNames); }
bool FromString(std::wstring_view s, MagnifyBar& v) { return EnumFromString(s, v, kMagnifyBarNames); }
const wchar_t* ToString(DockShape v) { return EnumToString(v, kShapeNames); }
bool FromString(std::wstring_view s, DockShape& v) { return EnumFromString(s, v, kShapeNames); }
const wchar_t* ToString(ItemView v) { return EnumToString(v, kItemViewNames); }
bool FromString(std::wstring_view s, ItemView& v) { return EnumFromString(s, v, kItemViewNames); }
const wchar_t* ToString(ItemAnchor v) { return EnumToString(v, kItemAnchorNames); }
bool FromString(std::wstring_view s, ItemAnchor& v) { return EnumFromString(s, v, kItemAnchorNames); }
const wchar_t* ToString(FolderSort v) { return EnumToString(v, kFolderSortNames); }
const wchar_t* ToString(WidgetBackground v) { return EnumToString(v, kWidgetBackgroundNames); }
bool FromString(std::wstring_view s, WidgetBackground& v) { return EnumFromString(s, v, kWidgetBackgroundNames); }
const wchar_t* ToString(RunningOrder v) { return EnumToString(v, kRunningOrderNames); }
bool FromString(std::wstring_view s, RunningOrder& v) { return EnumFromString(s, v, kRunningOrderNames); }

size_t FolderWindowConfig::RuleCount() const {
    size_t n = 0;
    size_t start = 0;
    while (start <= rules.size()) {
        const size_t end = std::min(rules.find(L';', start), rules.size());
        const std::wstring part = rules.substr(start, end - start);
        if (part.find_first_not_of(L" \t") != std::wstring::npos) ++n;
        start = end + 1;
    }
    return n;
}
bool FromString(std::wstring_view s, FolderSort& v) { return EnumFromString(s, v, kFolderSortNames); }
bool FromString(std::wstring_view s, DockEdge& v) { return EnumFromString(s, v, kEdgeNames); }
bool FromString(std::wstring_view s, DockAlign& v) { return EnumFromString(s, v, kAlignNames); }
bool FromString(std::wstring_view s, MiddleClickAction& v) { return EnumFromString(s, v, kMiddleClickNames); }
bool FromString(std::wstring_view s, WheelAction& v) { return EnumFromString(s, v, kWheelNames); }
bool FromString(std::wstring_view s, ClockClick& v) { return EnumFromString(s, v, kClockClickNames); }
bool FromString(std::wstring_view s, AutoHideMode& v) { return EnumFromString(s, v, kAutoHideNames); }
bool FromString(std::wstring_view s, HideStyle& v) { return EnumFromString(s, v, kHideStyleNames); }
bool FromString(std::wstring_view s, LaunchStyle& v) { return EnumFromString(s, v, kLaunchStyleNames); }
bool FromString(std::wstring_view s, Falloff& v) { return EnumFromString(s, v, kFalloffNames); }
bool FromString(std::wstring_view s, Easing& v) { return EnumFromString(s, v, kEasingNames); }
bool FromString(std::wstring_view s, ReduceMotion& v) { return EnumFromString(s, v, kReduceMotionNames); }
bool FromString(std::wstring_view s, Units& v) { return EnumFromString(s, v, kUnitsNames); }

Units LocaleUnits() {
    DWORD measure = 0;  // 0 metric, 1 U.S.
    if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IMEASURE | LOCALE_RETURN_NUMBER,
                        reinterpret_cast<LPWSTR>(&measure), sizeof(measure) / sizeof(wchar_t)) &&
        measure == 1) {
        return Units::Imperial;
    }
    return Units::Metric;
}

ItemKind ItemKindOf(std::wstring_view kind) {
    ItemKind v = ItemKind::Unknown;
    if (!kind.empty()) EnumFromString(kind, v, kItemKindNames);  // unknown text: Unknown
    return v;
}

const wchar_t* ToString(ItemKind v) { return EnumToString(v, kItemKindNames); }

bool DockConfig::Has(ItemKind kind) const {
    return std::any_of(items.begin(), items.end(), [&](const DockItemEntry& e) { return e.Kind() == kind; });
}

bool DockConfig::Show(ItemKind kind, bool on) {
    if (!on) return std::erase_if(items, [&](const DockItemEntry& e) { return e.Kind() == kind; }) > 0;
    if (Has(kind)) return false;
    DockItemEntry e;
    e.kind = ToString(kind);
    // The weather widget goes before an existing clock (where the dock has always drawn it).
    auto at = items.end();
    if (kind == ItemKind::Weather) {
        at = std::find_if(items.begin(), items.end(), [](const DockItemEntry& i) { return i.Kind() == ItemKind::Clock; });
    }
    items.insert(at, std::move(e));
    return true;
}

size_t DockConfig::AddWidget(ItemKind kind) {
    // Retain the legacy Add Weather command, but only create the script widget.
    if (kind == ItemKind::Weather) return AddScriptWidget(L"weather");
    if (kind != ItemKind::Clock && kind != ItemKind::Weather && kind != ItemKind::Running && kind != ItemKind::Tray &&
        kind != ItemKind::Notifications) {
        return SIZE_MAX;
    }
    if (kind == ItemKind::Running && Has(ItemKind::Running)) return SIZE_MAX;  // only the first one expands
    size_t at = items.size();
    if ((kind == ItemKind::Weather || kind == ItemKind::Tray || kind == ItemKind::Notifications) && !items.empty() &&
        items.back().Kind() == ItemKind::Clock) {
        at = items.size() - 1;
    } else if (kind == ItemKind::Running) {
        at = 0;
        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i].IsPin()) at = i + 1;
        }
    }
    DockItemEntry e;
    e.kind = ToString(kind);
    items.insert(items.begin() + static_cast<ptrdiff_t>(at), std::move(e));
    return at;
}

size_t DockConfig::AddScriptWidget(const std::wstring& widgetId) {
    size_t at = items.size();
    if (!items.empty() && items.back().Kind() == ItemKind::Clock) at = items.size() - 1;
    DockItemEntry e;
    e.kind = ToString(ItemKind::Widget);
    e.id = widgetId;
    items.insert(items.begin() + static_cast<ptrdiff_t>(at), std::move(e));
    return at;
}

bool DockConfig::MoveItem(size_t from, size_t to) {
    if (from >= items.size()) return false;
    to = std::min(to, items.size());
    if (to == from || to == from + 1) return false;  // already right before `to`
    DockItemEntry e = std::move(items[from]);
    items.erase(items.begin() + static_cast<ptrdiff_t>(from));
    if (to > from) --to;
    items.insert(items.begin() + static_cast<ptrdiff_t>(to), std::move(e));
    return true;
}

bool DockConfig::Anchored() const {
    return std::any_of(items.begin(), items.end(),
                       [](const DockItemEntry& e) { return e.anchor != ItemAnchor::None || e.offset != 0; });
}

bool DockConfig::SortByGroup(size_t* track) {
    std::vector<size_t> order(items.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return GroupOf(items[a]) < GroupOf(items[b]); });
    bool changed = false;
    for (size_t i = 0; i < order.size() && !changed; ++i) changed = order[i] != i;
    if (!changed) return false;
    std::vector<DockItemEntry> sorted;
    sorted.reserve(items.size());
    for (size_t i = 0; i < order.size(); ++i) {
        if (track && *track == order[i]) {
            *track = i;
            track = nullptr;  // followed once
        }
        sorted.push_back(std::move(items[order[i]]));
    }
    items = std::move(sorted);
    return true;
}

void DockConfig::JoinNeighbourGroup(size_t at) {
    if (at >= items.size() || !Anchored()) return;
    const int own = GroupOf(at);
    const int before = at > 0 ? GroupOf(at - 1) : -1;
    const int after = at + 1 < items.size() ? GroupOf(at + 1) : -1;
    if (own == before || own == after) return;
    const int g = before >= 0 ? before : after;
    if (g >= 0) items[at].anchor = AnchorFor(g);
}

std::vector<size_t> DockConfig::PinIndexes() const {
    std::vector<size_t> idx;
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].IsPin()) idx.push_back(i);
    }
    return idx;
}

size_t DockConfig::PinCount() const {
    return static_cast<size_t>(std::count_if(items.begin(), items.end(), [](const DockItemEntry& e) { return e.IsPin(); }));
}

std::vector<DockItemEntry> DockConfig::Pins() const {
    std::vector<DockItemEntry> pins;
    for (const DockItemEntry& e : items) {
        if (e.IsPin()) pins.push_back(e);
    }
    return pins;
}

DockItemEntry& DockConfig::PinAt(size_t k) { return items[PinIndexes().at(k)]; }

void DockConfig::InsertPin(size_t k, DockItemEntry e) {
    const std::vector<size_t> idx = PinIndexes();
    size_t pos = items.size();
    if (k < idx.size()) {
        pos = idx[k];
    } else if (!idx.empty()) {
        pos = idx.back() + 1;
    } else {
        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i].Kind() == ItemKind::Running) {
                pos = i;
                break;
            }
        }
    }
    items.insert(items.begin() + static_cast<ptrdiff_t>(pos), std::move(e));
}

void DockConfig::ErasePin(size_t k) {
    const std::vector<size_t> idx = PinIndexes();
    if (k < idx.size()) items.erase(items.begin() + static_cast<ptrdiff_t>(idx[k]));
}

void DockConfig::SetPins(std::vector<DockItemEntry> pins) {
    const std::vector<size_t> idx = PinIndexes();
    const size_t n = std::min(idx.size(), pins.size());
    for (size_t i = 0; i < n; ++i) items[idx[i]] = std::move(pins[i]);
    for (size_t i = idx.size(); i-- > n;) items.erase(items.begin() + static_cast<ptrdiff_t>(idx[i]));
    for (size_t i = n; i < pins.size(); ++i) InsertPin(SIZE_MAX, std::move(pins[i]));
}

std::wstring Config::Directory() {
    PWSTR base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &base))) {
        dir = base;
    }
    CoTaskMemFree(base);
    if (dir.empty()) {
        wchar_t tmp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tmp);
        dir = tmp;
    }
    if (!dir.empty() && dir.back() != L'\\') dir += L'\\';
    dir += L"WinDock";
    return dir;
}

std::wstring Config::FilePath() { return Directory() + L"\\config.json"; }

bool ParseArgbColor(const std::wstring& text, uint32_t& argb) {
    std::wstring s = text;
    if (!s.empty() && s[0] == L'#') s.erase(0, 1);
    if (s.size() != 6 && s.size() != 8) return false;
    uint32_t v = 0;
    for (wchar_t c : s) {
        v <<= 4;
        if (c >= L'0' && c <= L'9') v |= static_cast<uint32_t>(c - L'0');
        else if (c >= L'a' && c <= L'f') v |= static_cast<uint32_t>(c - L'a' + 10);
        else if (c >= L'A' && c <= L'F') v |= static_cast<uint32_t>(c - L'A' + 10);
        else return false;
    }
    if (s.size() == 6) v |= 0xFF000000u;
    argb = v;
    return true;
}

uint32_t DefaultFeedColor(size_t index) {
    static constexpr uint32_t kPalette[] = {0xFF4CC2FF, 0xFFFF8C42, 0xFF6CCB5F, 0xFFE0453A,
                                            0xFFB283F0, 0xFFF2C744, 0xFF2EC4B6, 0xFFEC6FB0};
    return kPalette[index % (sizeof(kPalette) / sizeof(kPalette[0]))];
}

std::wstring FormatArgbColor(uint32_t argb) {
    wchar_t buf[16];
    swprintf_s(buf, L"#%08X", argb);
    return buf;
}

uint32_t DockPaletteColor(size_t index) {
    static constexpr uint32_t kPalette[] = {0xFF4CC2FF, 0xFFFFB356, 0xFF8FD3A8, 0xFFC4A3FF, 0xFFF0A0C0, 0xFF7FD8D0};
    return kPalette[index % std::size(kPalette)];
}

uint32_t Config::DockColor(size_t index) const {
    if (index < docks.size() && docks[index].color != 0) return docks[index].color;
    return DockPaletteColor(index);
}

ClockConfig ClockOf(const Config& config, const DockItemEntry& item) {
    ClockConfig c = config.clock;
    if (item.Kind() != ItemKind::Clock) return c;
    try {
        if (const json::JsonObject o = SettingsObject(item.settings)) ReadClockFields(o, c);
    } catch (...) {
        c = config.clock;
    }
    SanitizeClock(c);
    return c;
}

WeatherConfig WeatherOf(const Config& config, const DockItemEntry& item) {
    WeatherConfig w = config.weather;
    try {
        // A script widget item's settings use the same keys (the weather widget's place).
        if (const json::JsonObject o = SettingsObject(item.settings)) ReadWeatherFields(o, w);
    } catch (...) {
        w = config.weather;
    }
    SanitizeWeather(w);
    return w;
}

bool SettingsHavePlace(const std::wstring& settingsJson) {
    try {
        const json::JsonObject o = SettingsObject(settingsJson);
        if (!o) return false;
        const json::IJsonValue lat = Find(o, L"latitude"), lon = Find(o, L"longitude");
        if (!lat || !lon || lat.ValueType() != json::JsonValueType::Number || lon.ValueType() != json::JsonValueType::Number) {
            return false;
        }
        WeatherConfig w;
        w.latitude = lat.GetNumber();
        w.longitude = lon.GetNumber();
        SanitizeWeather(w);
        return w.Configured();
    } catch (...) {
        return false;
    }
}

WeatherConfig ScriptWeatherOf(const Config& config, const DockConfig& dock, const DockItemEntry& item) {
    if (SettingsHavePlace(item.settings)) return WeatherOf(config, item);
    for (const DockItemEntry& e : dock.items) {
        if (e.Kind() == ItemKind::Weather) return WeatherOf(config, e);
    }
    return config.weather;
}

std::wstring ClockSettingsJson(const ClockConfig& c) {
    return L"{\"format\":" + Quote(c.format) + L",\"showSeconds\":" + Bool(c.showSeconds) + L",\"showDate\":" +
           Bool(c.showDate) + L",\"click\":" + Quote(ToString(c.click)) + L",\"clickTarget\":" + Quote(c.clickTarget) +
           L",\"face\":" + Quote(c.face) + L",\"timeZone\":" + Quote(c.timeZone) + L",\"label\":" + Quote(c.label) + L"}";
}

std::wstring WeatherSettingsJson(const WeatherConfig& w) {
    return L"{\"latitude\":" + Coord(w.latitude) + L",\"longitude\":" + Coord(w.longitude) + L",\"location\":" +
           Quote(w.location) + L",\"units\":" + Quote(ToString(w.units)) + L",\"refreshMinutes\":" +
           Num(w.refreshMinutes) + L",\"label\":" + Quote(w.label) + L"}";
}

// --- round 13 phase 13D ---
RunningConfig RunningOf(const DockItemEntry& item) {
    RunningConfig r;
    if (item.Kind() != ItemKind::Running) return r;
    try {
        if (const json::JsonObject o = SettingsObject(item.settings)) ReadRunningFields(o, r);
    } catch (...) {
        r = RunningConfig{};
    }
    SanitizeRunning(r);
    return r;
}

RunningConfig RunningOf(const Config&, const DockItemEntry& item) { return RunningOf(item); }

std::wstring RunningSettingsJson(const RunningConfig& r) {
    return L"{\"filter\":" + Quote(ToString(r.filter)) + L",\"windows\":" + Quote(ToString(r.windows)) + L",\"titles\":" +
           Quote(ToString(r.titles)) + L",\"titlesVertical\":" + Quote(ToString(r.titlesVertical)) + L",\"thumbnails\":" +
           Quote(ToString(r.thumbnails)) + L",\"thumbnailIntervalS\":" + Num(r.thumbnailIntervalS) +
           L",\"thumbnailSize\":" + Num(r.thumbnailSize) +
           // Round 14 phase 14D: only when set (0, the icon size, is the default).
           (r.thumbnailAcross > 0 ? L",\"thumbnailAcross\":" + Num(r.thumbnailAcross) : std::wstring()) +
           L",\"thumbnailFps\":" + Num(r.thumbnailFps) + L",\"badges\":" +
           Bool(r.badges) + L",\"gap\":" + Num(r.gap) + L",\"padding\":" + Num(r.padding) + L",\"order\":" +
           Quote(ToString(r.order)) + L"}";
}

const RunningConfig& DockConfig::Running() const {
    const DockItemEntry* running = nullptr;
    for (const DockItemEntry& e : items) {
        if (e.Kind() == ItemKind::Running) {
            running = &e;
            break;
        }
    }
    if (!running) {
        if (!runningCacheValid_ || runningCacheHas_) {
            runningCache_ = RunningConfig{};
            runningCacheKey_.clear();
            runningCacheHas_ = false;
            runningCacheValid_ = true;
        }
        return runningCache_;
    }
    if (!runningCacheValid_ || !runningCacheHas_ || runningCacheKey_ != running->settings) {
        runningCache_ = RunningOf(*running);
        runningCacheKey_ = running->settings;
        runningCacheHas_ = true;
        runningCacheValid_ = true;
    }
    return runningCache_;
}

void SetRunningSettings(DockItemEntry& item, const RunningConfig& r) {
    json::JsonObject settings = SettingsObject(item.settings);
    if (!settings) settings = json::JsonObject();
    const json::JsonObject values = SettingsObject(RunningSettingsJson(r));
    for (const auto& field : values) settings.SetNamedValue(field.Key(), field.Value());
    // Round 14 phase 14D: thumbnailAcross is written only when set; back at 0 the key goes (the merge
    // above would keep the old value).
    if (r.thumbnailAcross <= 0 && settings.HasKey(L"thumbnailAcross")) settings.Remove(L"thumbnailAcross");
    item.settings = std::wstring(settings.Stringify());
}

bool DockConfig::SetRunning(const RunningConfig& r) {
    for (DockItemEntry& e : items) {
        if (e.Kind() != ItemKind::Running) continue;
        SetRunningSettings(e, r);
        return true;
    }
    return false;
}

bool Config::EnsureItemSettings() {
    bool changed = false;
    auto snapshotWeather = [](DockItemEntry& item, const WeatherConfig& effective) {
        json::JsonObject settings = SettingsObject(item.settings);
        if (!settings) settings = json::JsonObject();
        const json::JsonObject place = SettingsObject(WeatherSettingsJson(effective));
        for (const auto& field : place) settings.SetNamedValue(field.Key(), field.Value());
        item.settings = std::wstring(settings.Stringify());
    };
    for (DockConfig& d : docks) {
        // Existing script widgets may inherit a native item's place. Snapshot that inheritance
        // before retiring the native item, keeping their own appearance and popup settings.
        if (d.Has(ItemKind::Weather)) {
            for (DockItemEntry& e : d.items) {
                if (e.Kind() == ItemKind::Widget && e.id == L"weather" && !SettingsHavePlace(e.settings)) {
                    snapshotWeather(e, ScriptWeatherOf(*this, d, e));
                    changed = true;
                }
            }
        }
        for (DockItemEntry& e : d.items) {
            if (e.Kind() == ItemKind::Weather) {
                // Snapshot the effective defaults before changing kind. Keep the item's other
                // fields and all supplied settings, including fields unknown to this version.
                snapshotWeather(e, WeatherOf(*this, e));
                e.kind = L"widget";
                e.id = L"weather";
                changed = true;
            }
            if (!e.settings.empty()) continue;
            if (e.Kind() == ItemKind::Clock) {
                e.settings = ClockSettingsJson(clock);
                changed = true;
            }
            if (e.Kind() == ItemKind::Running) {  // round 13 phase 13D: the RunningConfig defaults
                e.settings = RunningSettingsJson(RunningConfig{});
                changed = true;
            }
        }
    }
    return changed;
}

bool Config::ResolveTheme() {
    bool ok = true;
    const ThemeConfig base = Theme::Resolve(themePreset, ok);
    for (size_t i = 0; i < Theme::FieldCount(); ++i) {
        const Theme::FieldInfo& f = Theme::Fields()[i];
        if (!(themeOverrides & Theme::Bit(f.field))) Theme::CopyField(f, base, theme);
    }
    themeResolveFailed = !ok;
    ResolveDockThemes();
    return ok;
}

void Config::ResolveDockThemes() {
    // Round 12 phase 12B: a dock's own look is its preset plus its own fields; before 12B it was its
    // preset plus the app's fields, and a preset equal to the app's meant "the app's theme".
    for (DockConfig& d : docks) {
        d.ownTheme = false;
        if (!d.OwnLook()) continue;
        const ThemeConfig base = PresetOf(&d);
        for (size_t i = 0; i < Theme::FieldCount(); ++i) {
            const Theme::FieldInfo& f = Theme::Fields()[i];
            if (!(d.themeOverrides & Theme::Bit(f.field))) Theme::CopyField(f, base, d.theme);
        }
        d.ownTheme = true;
    }
}

ThemeConfig Config::PresetOf(const DockConfig* dock) const {
    bool ok = true;
    if (!dock || !dock->OwnLook()) return Theme::Resolve(themePreset, ok);
    ThemeConfig t = Theme::Resolve(dock->themePreset, ok);
    return ok ? t : theme;  // unknown preset / broken file: the app's look underneath
}

void Config::GiveOwnLook(DockConfig& d) const {
    d.themePreset = themePreset.empty() ? std::wstring(L"dark") : themePreset;
    d.themeOverrides = themeOverrides;
    d.theme = theme;
    d.ownTheme = true;
}

void Config::UseAppLook(DockConfig& d) {
    d.themePreset.clear();
    d.themeOverrides = 0;
    d.ownTheme = false;
}

void Config::CopyLook(const DockConfig* source, DockConfig& target) const {
    if (!source || !source->OwnLook()) {
        GiveOwnLook(target);
        return;
    }
    if (source == &target) return;
    target.themePreset = source->themePreset;
    target.themeOverrides = source->themeOverrides;
    target.theme = source->theme;
    target.ownTheme = true;
}

std::wstring Config::UniqueDockId(const std::wstring& base) const {
    auto taken = [&](const std::wstring& id) {
        return std::any_of(docks.begin(), docks.end(), [&](const DockConfig& d) { return d.id == id; });
    };
    if (!taken(base)) return base;
    for (int n = 2;; ++n) {
        std::wstring id = base + std::to_wstring(n);
        if (!taken(id)) return id;
    }
}

void Config::Sanitize() {
    // At least one dock (folder windows alone are not enough: the menus, tray icon and automation
    // need a dock).
    if (std::none_of(docks.begin(), docks.end(), [](const DockConfig& d) { return d.IsDock(); })) {
        docks.emplace(docks.begin());
    }
    // Round 11 phase 11D: and at least one of them enabled (a hand-edited file with every dock
    // "enabled": false would hide the taskbar and show nothing): the first dock comes back.
    if (std::none_of(docks.begin(), docks.end(), [](const DockConfig& d) { return d.IsDock() && d.enabled; })) {
        std::find_if(docks.begin(), docks.end(), [](const DockConfig& d) { return d.IsDock(); })->enabled = true;
    }
    for (DockConfig& d : docks) {
        if (d.id.empty()) d.id = d.IsFolderWindow() ? L"folder" : d.IsWidgetWindow() ? L"widget" : L"main";
        if (d.IsWidgetWindow()) {
            // Round 6 phase 6B: exactly one widget item (a missing one: an empty id, which shows the
            // error tile until the entry names a widget), the window's size and place in range.
            if (d.id.size() > 64) d.id.resize(64);
            DockItemEntry widget;
            widget.kind = ToString(ItemKind::Widget);
            if (const DockItemEntry* e = d.WidgetItem()) {
                widget.id = e->id;
                widget.settings = e->settings;
            }
            SingleLine(widget.id, 64);
            d.items = {std::move(widget)};
            WidgetWindowConfig& w = d.widgetWindow;
            w.w = std::clamp(w.w, WidgetWindowConfig::kMinSize, WidgetWindowConfig::kMaxSize);
            w.h = std::clamp(w.h, WidgetWindowConfig::kMinSize, WidgetWindowConfig::kMaxSize);
            if (static_cast<unsigned>(w.background) > static_cast<unsigned>(WidgetBackground::Panel)) w.background = WidgetBackground::None;
            if (d.x != DockConfig::kCentred) d.x = std::clamp(d.x, -16000, 16000);
            if (d.y != DockConfig::kCentred) d.y = std::clamp(d.y, -16000, 16000);
            if (d.monitor < DockConfig::kMonitorPrimary) d.monitor = DockConfig::kMonitorPrimary;
            if (static_cast<unsigned>(d.layer) > static_cast<unsigned>(DockLayer::Behind)) d.layer = DockLayer::Behind;
            d.floating = false;
            continue;
        }
        if (d.IsFolderWindow()) {
            FolderWindowConfig& f = d.folder;
            if (f.path.size() > 4096) f.path.resize(4096);
            if (d.title.size() > 128) d.title.resize(128);
            f.w = std::clamp(f.w, 160, 8000);
            f.h = std::clamp(f.h, 80, 8000);
            f.iconSize = std::clamp(f.iconSize, 32, 96);
            if (static_cast<unsigned>(f.view) > static_cast<unsigned>(ItemView::List)) f.view = ItemView::Auto;
            if (static_cast<unsigned>(f.sort) > static_cast<unsigned>(FolderSort::Size)) f.sort = FolderSort::Name;
            if (d.x != DockConfig::kCentred) d.x = std::clamp(d.x, -16000, 16000);
            if (d.y != DockConfig::kCentred) d.y = std::clamp(d.y, -16000, 16000);
            if (d.monitor < DockConfig::kMonitorPrimary) d.monitor = DockConfig::kMonitorPrimary;
            if (f.rules.size() > 1024) f.rules.resize(1024);
            f.opacity = std::isfinite(f.opacity) ? std::clamp(f.opacity, 0.2, 1.0) : 1.0;
            f.labelLines = std::clamp(f.labelLines, 1, 2);
            d.items.clear();
            continue;
        }
        if (d.id.size() > 64) d.id.resize(64);
        if (static_cast<unsigned>(d.edge) > static_cast<unsigned>(DockEdge::Right)) d.edge = DockEdge::Bottom;
        if (static_cast<unsigned>(d.align) > static_cast<unsigned>(DockAlign::End)) d.align = DockAlign::Center;
        d.offset = std::clamp(d.offset, -16000, 16000);
        d.edgeGap = std::clamp(d.edgeGap, 0, DockConfig::kMaxEdgeGap);  // round 12 phase 12E
        d.peekPx = std::clamp(d.peekPx, 0, 64);
        d.maxLength = d.maxLength <= 0 ? 0 : std::clamp(d.maxLength, 96, 8000);  // round 7 phase 7A
        d.length = d.length <= 0 ? 0 : std::clamp(d.length, 96, 8000);           // round 14 phase 14A
        d.thickness = d.thickness <= 0 ? 0 : std::clamp(d.thickness, 24, 1000);  // round 14 phase 14A
        d.popupGap = std::clamp(d.popupGap, 0, 64);
        if (!(d.peekOpacity >= 0.05)) d.peekOpacity = d.peekOpacity >= 0.0 ? 0.05 : 0.5;  // also NaN
        d.peekOpacity = std::min(d.peekOpacity, 1.0);
        if (d.title.size() > 128) d.title.resize(128);
        d.iconSize = std::clamp(d.iconSize, 16, 256);
        if (!(d.magnification >= 1.0)) d.magnification = 1.0;  // also catches NaN
        d.magnification = std::min(d.magnification, 2.5);
        d.magnifyRange = std::clamp(d.magnifyRange, 1, 4);
        d.hideDelayMs = std::clamp(d.hideDelayMs, 0, 10000);
        d.revealDelayMs = std::clamp(d.revealDelayMs, 0, 10000);
        d.previewDelayMs = std::clamp(d.previewDelayMs, 0, 10000);
        if (d.monitor < DockConfig::kMonitorEach) d.monitor = 0;
        if (static_cast<unsigned>(d.layer) > static_cast<unsigned>(DockLayer::Behind)) d.layer = DockLayer::Normal;
        // Round 13 phase 13D: the running keys are sanitized by RunningOf (the item's settings).
        if (static_cast<unsigned>(d.overflow) > static_cast<unsigned>(OverflowMode::Clip)) d.overflow = OverflowMode::Scroll;
        if (static_cast<unsigned>(d.magnifyBar) > static_cast<unsigned>(MagnifyBar::Always)) d.magnifyBar = MagnifyBar::Steady;
        if (static_cast<unsigned>(d.shape) > static_cast<unsigned>(DockShape::Circle)) d.shape = DockShape::Bar;
        d.radius = d.radius <= 0 ? 0 : std::clamp(d.radius, 60, 2000);
        if (d.x != DockConfig::kCentred) d.x = std::clamp(d.x, -16000, 16000);
        if (d.y != DockConfig::kCentred) d.y = std::clamp(d.y, -16000, 16000);
        // Round 6 phase 6A: a floating dock is a bar (another shape reads as not floating) and
        // never auto-hides (its layer and the dashboard hotkey decide when it shows).
        if (d.shape != DockShape::Bar) d.floating = false;
        if (d.floating) d.autoHide = AutoHideMode::Off;
        notifications.pollS = std::clamp(notifications.pollS, 1, 60);
        // Round 7 phase 7B: item anchors in range, the items in group order (start, centre, end).
        for (DockItemEntry& e : d.items) {
            if (static_cast<unsigned>(e.anchor) > static_cast<unsigned>(ItemAnchor::End)) e.anchor = ItemAnchor::None;
            e.offset = std::clamp(e.offset, -DockConfig::kMaxItemOffset, DockConfig::kMaxItemOffset);
        }
        d.SortByGroup();
        for (DockItemEntry& e : d.items) {
            if (e.Kind() != ItemKind::Tray) continue;
            e.rows = std::clamp(e.rows, 1, 2);
            e.columns = std::clamp(e.columns, 1, 20);
            e.trayIconSize = e.trayIconSize <= 18 ? 16 : e.trayIconSize <= 22 ? 20 : 24;
        }
        // A file stem inside the themes folder, like theme.preset: no paths.
        if (d.themePreset.size() > 64 || d.themePreset.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos ||
            d.themePreset == L"." || d.themePreset == L"..") {
            d.themePreset.clear();
        }
        // --- round 12 phase 12C: the name (one line, at most 64) and the title options in range ---
        SingleLine(d.name, 64);
        if (static_cast<unsigned>(d.titleShow) > static_cast<unsigned>(TitleShow::Always)) d.titleShow = TitleShow::Always;
        if (static_cast<unsigned>(d.titlePlace) > static_cast<unsigned>(TitlePlace::Inside)) d.titlePlace = TitlePlace::Outside;
        // --- end round 12 phase 12C ---
    }
    // Unique ids (the docks are matched by id when the config is re-applied): a repeated id gets a
    // number appended.
    for (size_t i = 1; i < docks.size(); ++i) {
        const bool repeated = std::any_of(docks.begin(), docks.begin() + static_cast<ptrdiff_t>(i),
                                          [&](const DockConfig& d) { return d.id == docks[i].id; });
        if (repeated) docks[i].id = UniqueDockId(docks[i].id);
    }
    // Round 12 phase 12B: the same clamps for the app's theme and every dock's own look; a dock
    // without a preset has no fields of its own (a "theme" object without "themePreset" is ignored).
    SanitizeTheme(theme);
    for (DockConfig& d : docks) {
        if (d.OwnLook()) {
            SanitizeTheme(d.theme);
        } else {
            d.themeOverrides = 0;
        }
    }
    // The preset names a file stem inside the themes folder: no paths.
    if (themePreset.size() > 64 ||
        themePreset.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos || themePreset == L"." ||
        themePreset == L"..") {
        themePreset = L"dark";
    }
    if (std::isnan(animation.speed)) animation.speed = 1.0;
    if (animation.speed < 0.0) animation.speed = 0.0;
    if (animation.speed != 0.0) animation.speed = std::clamp(animation.speed, 0.25, 4.0);
    animation.labelFadeMs = std::clamp(animation.labelFadeMs, 0, 1000);
    SanitizeClock(clock);
    if (renderer != L"auto" && renderer != L"software") renderer = L"auto";
    SanitizeWeather(weather);
    if (calendars.feeds.size() > 32) calendars.feeds.resize(32);
    for (CalendarFeed& f : calendars.feeds) {
        if (f.name.size() > 128) f.name.resize(128);
        if (f.url.size() > 4096) f.url.resize(4096);
        f.refreshMin = std::clamp(f.refreshMin, 5, 1440);
        f.reminderMin = std::clamp(f.reminderMin, 0, 1440);
    }
    calendars.defaultReminderMin = std::clamp(calendars.defaultReminderMin, 0, 1440);
    // The dashboard hotkey in its canonical spelling ("" = none); text that is not a hotkey keeps
    // the default.
    if (!dashboard.hotkey.empty()) {
        Hotkey::Combo combo;
        dashboard.hotkey = Hotkey::Parse(dashboard.hotkey, combo) ? Hotkey::Format(combo) : DashboardConfig{}.hotkey;
    }
    // Round 8 phase 8A: the settings windows' sizes ({0, 0}: the default).
    for (int k = 0; k < SettingsUiConfig::kKinds; ++k) {
        SettingsWindowSize& s = settings.windows[k];
        if (s.w == 0 && s.h == 0) continue;
        // Round 12 phase 12F: kSavedMinimum (the Dock slot holds a panel's size too).
        s.w = std::clamp(s.w, SettingsUiConfig::kSavedMinimum[k].w, SettingsUiConfig::kMaximum);
        s.h = std::clamp(s.h, SettingsUiConfig::kSavedMinimum[k].h, SettingsUiConfig::kMaximum);
        if (s == SettingsUiConfig::kDefault[k]) s = SettingsWindowSize{};
    }
    settings.tipsShown &= (1 << SettingsUiConfig::kTipCount) - 1;  // round 12 phase 12G: the three tips' bits
}

Config::LoadResult Config::LoadFile() {
    const std::wstring path = FilePath();
    std::string bytes;
    if (!ReadFileUtf8(path, bytes)) {
        *this = Config{};
        weather.units = LocaleUnits();  // the default when the file (or its "units" key) is absent
        // Missing (first run), or present but unreadable (treated like a malformed file).
        return GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES ? LoadResult::Missing
                                                                           : LoadResult::Malformed;
    }
    return LoadJson(Utf8ToWide(bytes));
}

Config::LoadResult Config::LoadJson(const std::wstring& text) {
    *this = Config{};
    weather.units = LocaleUnits();  // the default when the file (or its "units" key) is absent
    try {
        json::JsonObject root{nullptr};
        if (!json::JsonObject::TryParse(text, root) || !root) {
            OutputDebugStringW(L"WinDock: config.json is malformed; using defaults\n");
            return LoadResult::Malformed;
        }

        // The legacy (pre-`docks`) keys are known too: in a current file they are ignored and
        // dropped on the next save; in a legacy file they are migrated below.
        static const wchar_t* const kKnown[] = {
            L"docks",         L"hideTaskbar", L"startWithWindows", L"renderer",       L"clock",
            L"theme",         L"animation",   L"weather",          L"pinned",         L"iconSize",
            L"magnification", L"magnifyRange", L"autoHide",        L"hideDelayMs",    L"revealDelayMs",
            L"previews",      L"previewDelayMs", L"monitor",       L"tray",           L"notifications",
            L"calendars",     L"dashboard",   L"settings",
        };
        for (const auto& kv : root) {
            const std::wstring key(kv.Key());
            if (std::any_of(std::begin(kKnown), std::end(kKnown), [&](const wchar_t* k) { return key == k; })) {
                continue;
            }
            extraFields.emplace_back(key, std::wstring(kv.Value().Stringify()));
        }

        ReadBool(root, L"hideTaskbar", hideTaskbar);
        ReadBool(root, L"startWithWindows", startWithWindows);
        ReadString(root, L"renderer", renderer);

        const json::JsonObject clockObj = ReadObject(root, L"clock");
        if (clockObj) ReadClockFields(clockObj, clock);
        const json::JsonObject weatherObj = ReadObject(root, L"weather");

        if (root.HasKey(L"docks")) {
            docks.clear();
            if (auto arr = ReadArray(root, L"docks")) {
                for (const auto& dv : arr) {
                    if (dv.ValueType() != json::JsonValueType::Object) continue;
                    const json::JsonObject obj = dv.GetObject();
                    DockConfig d;
                    ReadString(obj, L"id", d.id);
                    ReadBool(obj, L"enabled", d.enabled);  // round 11 phase 11D: every kind
                    std::wstring kind;
                    ReadString(obj, L"kind", kind);
                    if (!kind.empty() && CompareStringOrdinal(kind.c_str(), -1, L"dock", -1, TRUE) != CSTR_EQUAL) {
                        if (CompareStringOrdinal(kind.c_str(), -1, L"widget", -1, TRUE) == CSTR_EQUAL) {
                            d.dockKind = DockKind::Widget;  // round 6 phase 6B: a standalone widget
                            ReadWidgetWindowFields(obj, d);
                            docks.push_back(std::move(d));
                            continue;
                        }
                        if (CompareStringOrdinal(kind.c_str(), -1, L"folderWindow", -1, TRUE) != CSTR_EQUAL) {
                            extraDocks.emplace_back(obj.Stringify());  // a kind this version does not know: kept, not shown
                            continue;
                        }
                        d.dockKind = DockKind::FolderWindow;
                        d.items.clear();
                        ReadFolderWindowFields(obj, d);
                        docks.push_back(std::move(d));
                        continue;
                    }
                    bool oldKeys = false;
                    ReadDockObject(obj, d, &oldKeys);
                    if (oldKeys) migrated = true;  // round 13 phase 13D: the moved keys are saved at once
                    docks.push_back(std::move(d));
                }
            }
        } else {
            // Legacy file (before docks[] / items[]): build docks[0] from the top-level keys, once.
            // The caller saves, which writes the current format and drops the legacy keys.
            migrated = true;
            DockConfig& d = Main();
            d.items.clear();
            ReadDockFields(root, d);
            if (auto p = ReadArray(root, L"pinned")) {
                for (const auto& item : p) {
                    if (item.ValueType() != json::JsonValueType::Object) continue;
                    DockItemEntry e;
                    std::wstring kind;
                    auto obj = item.GetObject();
                    ReadString(obj, L"path", e.path);
                    ReadString(obj, L"aumid", e.aumid);
                    ReadString(obj, L"icon", e.icon);
                    ReadString(obj, L"kind", kind);
                    const ItemKind k = ItemKindOf(kind);
                    if (k == ItemKind::Separator || k == ItemKind::RecycleBin) {
                        e.kind = kind;  // these need neither a path nor an AUMID
                    } else if (e.path.empty() && e.aumid.empty()) {
                        continue;  // nothing to show (skipped by older versions too)
                    } else {
                        e.kind = e.aumid.empty() && IsDirectoryPath(e.path) ? L"folder" : L"app";
                    }
                    d.items.push_back(std::move(e));
                }
            }
            bool clockShown = true, weatherShown = false;  // the legacy defaults
            if (clockObj) ReadBool(clockObj, L"show", clockShown);
            if (weatherObj) ReadBool(weatherObj, L"show", weatherShown);
            d.items.push_back(DockItemEntry{L"running"});
            if (weatherShown) d.items.push_back(DockItemEntry{L"weather"});
            if (clockShown) d.items.push_back(DockItemEntry{L"clock"});
        }

        if (auto w = weatherObj) ReadWeatherFields(w, weather);
        // A config without "preset" (older versions) is "dark" with every key present as an override.
        if (auto t = ReadObject(root, L"theme")) {
            ReadString(t, L"preset", themePreset);
            Theme::ReadFields(t, theme, &themeOverrides);
        }
        if (auto a = ReadObject(root, L"animation")) {
            ReadNumber(a, L"speed", animation.speed);
            ReadEnum(a, L"hideStyle", animation.hideStyle);
            ReadEnum(a, L"launchStyle", animation.launchStyle);
            ReadEnum(a, L"magnifyFalloff", animation.magnifyFalloff);
            ReadEnum(a, L"easing", animation.easing);
            ReadInt(a, L"labelFadeMs", animation.labelFadeMs);
            ReadEnum(a, L"reduceMotion", animation.reduceMotion);
        }
        if (auto t = ReadObject(root, L"tray")) ReadBool(t, L"enabled", tray.enabled);
        if (auto n = ReadObject(root, L"notifications")) {
            ReadInt(n, L"pollS", notifications.pollS);
            ReadBool(n, L"clearOnActivate", notifications.clearOnActivate);
            ReadBool(n, L"balloons", notifications.balloons);
            ReadStrings(n, L"excluded", notifications.excluded);
            for (std::wstring& k : notifications.excluded) {
                if (!k.empty()) CharLowerBuffW(k.data(), static_cast<DWORD>(k.size()));
            }
        }
        if (auto c = ReadObject(root, L"calendars")) {
            ReadBool(c, L"showDots", calendars.showDots);
            ReadInt(c, L"defaultReminderMin", calendars.defaultReminderMin);
            if (const json::JsonArray feeds = ReadArray(c, L"feeds")) {
                for (const auto& v : feeds) {
                    if (v.ValueType() != json::JsonValueType::Object) continue;
                    const json::JsonObject f = v.GetObject();
                    CalendarFeed feed;
                    ReadString(f, L"name", feed.name);
                    ReadString(f, L"url", feed.url);
                    std::wstring color;
                    ReadString(f, L"color", color);
                    if (!ParseArgbColor(color, feed.color)) feed.color = DefaultFeedColor(calendars.feeds.size());
                    ReadBool(f, L"enabled", feed.enabled);
                    ReadInt(f, L"refreshMin", feed.refreshMin);
                    ReadInt(f, L"reminderMin", feed.reminderMin);
                    calendars.feeds.push_back(std::move(feed));
                }
            }
        }
        if (auto dash = ReadObject(root, L"dashboard")) ReadString(dash, L"hotkey", dashboard.hotkey);
        if (auto s = ReadObject(root, L"settings")) {
            if (auto w = ReadObject(s, L"windows")) {
                for (int k = 0; k < SettingsUiConfig::kKinds; ++k) {
                    std::vector<double> v;
                    if (!ReadNumbers(w, SettingsUiConfig::kNames[k], v) || v.size() != 2) continue;
                    if (!std::isfinite(v[0]) || !std::isfinite(v[1])) continue;
                    settings.windows[k].w = static_cast<int>(std::clamp(v[0], 0.0, 100000.0));
                    settings.windows[k].h = static_cast<int>(std::clamp(v[1], 0.0, 100000.0));
                }
            }
            // Round 9 phase 9A: "hide" (default) / "ignore"; unknown text keeps the default.
            std::wstring fs;
            ReadString(s, L"fullscreen", fs);
            if (CompareStringOrdinal(fs.c_str(), -1, L"ignore", -1, TRUE) == CSTR_EQUAL) settings.fullscreenHide = false;
            ReadBool(s, L"confirmPlacement", settings.confirmPlacement);  // round 12 phase 12A
            ReadBool(s, L"snapToScreen", settings.snapToScreen);          // round 14 phase 14B
            ReadInt(s, L"tipsShown", settings.tipsShown);                  // round 12 phase 12G
        }
    } catch (...) {
        OutputDebugStringW(L"WinDock: failed to read config.json; using defaults\n");
        *this = Config{};
        weather.units = LocaleUnits();
        return LoadResult::Malformed;
    }
    Sanitize();  // also validates themePreset before it is used as a file name
    // Round 5 phase A: clock / weather items without settings of their own (every file written
    // before it) get the defaults' values; the caller saves, so the file has them from now on.
    if (EnsureItemSettings()) migrated = true;

    // Resolve the theme. Inline values equal to the preset's are not overrides: an untouched
    // older config becomes a plain `"preset": "dark"` on the next save. Not done when the preset
    // failed to resolve (the user's values would be compared against the dark fallback).
    bool ok = true;
    const ThemeConfig base = Theme::Resolve(themePreset, ok);
    for (size_t i = 0; i < Theme::FieldCount(); ++i) {
        const Theme::FieldInfo& f = Theme::Fields()[i];
        const uint64_t bit = Theme::Bit(f.field);
        if (ok && (themeOverrides & bit) && Theme::FieldEquals(f, theme, base)) themeOverrides &= ~bit;
        if (!(themeOverrides & bit)) Theme::CopyField(f, base, theme);
    }
    themeResolveFailed = !ok;
    // Round 12 phase 12B: the same for every dock's own fields (over its own preset). A file from
    // before 12B has no docks[].theme: a dock with a themePreset loads as its own look = that
    // preset with no fields of its own (the app's inline fields no longer apply to it).
    for (DockConfig& d : docks) {
        if (!d.OwnLook() || d.themeOverrides == 0) continue;
        bool dockOk = true;
        const ThemeConfig dockBase = Theme::Resolve(d.themePreset, dockOk);
        if (!dockOk) continue;
        for (size_t i = 0; i < Theme::FieldCount(); ++i) {
            const Theme::FieldInfo& f = Theme::Fields()[i];
            const uint64_t bit = Theme::Bit(f.field);
            if ((d.themeOverrides & bit) && Theme::FieldEquals(f, d.theme, dockBase)) d.themeOverrides &= ~bit;
        }
    }
    ResolveDockThemes();
    return LoadResult::Ok;
}

bool Config::Save(DWORD* error) const {
    if (error) *error = 0;
    try {
        std::wstring dir = Directory();
        CreateDirectoryW(dir.c_str(), nullptr);
        return WriteFileAtomic(FilePath(), WideToUtf8(ToJson()), error);
    } catch (...) {
        if (error) *error = ERROR_GEN_FAILURE;
        return false;
    }
}

std::wstring Config::ToJson(bool windowSizes) const {
    {
        std::wstring o;
        o += L"{\n  \"docks\": [";
        for (size_t di = 0; di < docks.size(); ++di) {
            const DockConfig& d = docks[di];
            o += di ? L",\n    {\n" : L"\n    {\n";
            if (d.IsWidgetWindow()) {
                // Round 6 phase 6B: a standalone widget. Older versions keep the entry verbatim (an
                // unknown kind) and show nothing. Keys written only when not the default (the widget
                // id, w and h always).
                const DockItemEntry* e = d.WidgetItem();
                const WidgetWindowConfig& w = d.widgetWindow;
                o += L"      \"id\": " + Quote(d.id) + L", \"kind\": \"widget\", \"widget\": " + Quote(e ? e->id : std::wstring()) +
                     (e && !e->settings.empty() ? L", \"settings\": " + e->settings : std::wstring()) + L",\n";
                o += L"      " + (d.x != DockConfig::kCentred ? L"\"x\": " + Num(d.x) + L", " : std::wstring()) +
                     (d.y != DockConfig::kCentred ? L"\"y\": " + Num(d.y) + L", " : std::wstring()) + L"\"w\": " + Num(w.w) +
                     L", \"h\": " + Num(w.h);
                if (d.monitor != 0) o += L", \"monitor\": " + MonitorJson(d.monitor);
                if (d.layer != DockLayer::Behind) o += L", \"layer\": " + Quote(ToString(d.layer));
                if (d.locked) o += L", \"locked\": true";
                if (!d.enabled) o += L", \"enabled\": false";
                if (w.background != WidgetBackground::None) o += L", \"background\": " + Quote(ToString(w.background));
                if (d.color) o += L", \"color\": " + Quote(FormatArgbColor(d.color));
                o += L"\n    }";
                continue;
            }
            if (d.IsFolderWindow()) {
                // The folder window's keys, then what makes an older version (which reads this as a
                // dock) show nothing: no items, the desktop layer, auto-hidden, reserving nothing.
                const FolderWindowConfig& f = d.folder;
                o += L"      \"id\": " + Quote(d.id) + L", \"kind\": \"folderWindow\", \"path\": " + Quote(f.path) +
                     L", \"title\": " + Quote(d.title) + L",\n";
                // x / y only when placed (no key: centred on the work area).
                o += L"      " + (d.x != DockConfig::kCentred ? L"\"x\": " + Num(d.x) + L", " : std::wstring()) +
                     (d.y != DockConfig::kCentred ? L"\"y\": " + Num(d.y) + L", " : std::wstring()) + L"\"w\": " + Num(f.w) +
                     L", \"h\": " + Num(f.h) + L", \"monitor\": " + MonitorJson(d.monitor) + L",\n";
                o += L"      \"view\": " + Quote(ToString(f.view)) + L", \"iconSize\": " + Num(f.iconSize) +
                     L", \"sort\": " + Quote(ToString(f.sort)) + L", \"showHidden\": " + Bool(f.showHidden) +
                     L", \"collapsed\": " + Bool(f.collapsed) + (f.singleClick ? L", \"singleClick\": true" : L"") + L",\n";
                // 12b keys, each only when not the default.
                std::wstring more;
                if (!f.rules.empty()) more += L", \"rules\": " + Quote(f.rules);
                if (f.autoSort) more += L", \"autoSort\": true";
                if (f.opacity != 1.0) more += L", \"opacity\": " + Num(f.opacity);
                if (f.titleColor != 0) more += L", \"titleColor\": " + Quote(FormatArgbColor(f.titleColor));
                if (f.hideTitle) more += L", \"hideTitle\": true";
                if (f.labelLines != 2) more += L", \"labelLines\": " + Num(f.labelLines);
                if (f.transparentBackground) more += L", \"transparentBackground\": true";
                if (f.rollUpOnHover) more += L", \"rollUpOnHover\": true";
                if (!d.enabled) more += L", \"enabled\": false";
                if (!more.empty()) o += L"      " + more.substr(2) + L",\n";
                o += L"      \"items\": [], \"layer\": \"desktop\", \"autoHide\": \"always\", \"reserveSpace\": false\n    }";
                continue;
            }
            o += L"      \"id\": " + Quote(d.id) + (d.color ? L", \"color\": " + Quote(FormatArgbColor(d.color)) : std::wstring()) +
                 L",\n      \"items\": [";
            for (size_t i = 0; i < d.items.size(); ++i) {
                const DockItemEntry& e = d.items[i];
                o += i ? L",\n        " : L"\n        ";
                // kind first; then the target and the icon only when set.
                std::wstring fields = Quote(L"kind") + L": " + Quote(e.kind);
                auto add = [&](const wchar_t* key, const std::wstring& value) {
                    fields += L", " + Quote(key) + L": " + Quote(value);
                };
                if (!e.aumid.empty()) add(L"aumid", e.aumid);
                else if (!e.path.empty()) add(L"path", e.path);
                if (!e.icon.empty()) add(L"icon", e.icon);
                if (!e.id.empty()) add(L"id", e.id);
                if (!e.settings.empty()) fields += L", " + Quote(L"settings") + L": " + e.settings;
                if (e.view != ItemView::Auto) add(L"view", ToString(e.view));
                if (e.anchor != ItemAnchor::None) add(L"anchor", ToString(e.anchor));  // round 7 phase 7B
                if (e.offset != 0) fields += L", " + Quote(L"offset") + L": " + Num(e.offset);
                if (e.Kind() == ItemKind::Tray) {
                    const DockItemEntry defaults{L"tray"};
                    if (e.rows != defaults.rows) fields += L", " + Quote(L"rows") + L": " + Num(e.rows);
                    if (e.columns != defaults.columns) fields += L", " + Quote(L"columns") + L": " + Num(e.columns);
                    if (e.trayIconSize != defaults.trayIconSize) fields += L", " + Quote(L"iconSize") + L": " + Num(e.trayIconSize);
                    auto list = [&](const wchar_t* key, const std::vector<std::wstring>& v) {
                        if (v.empty()) return;
                        std::wstring a;
                        for (size_t k = 0; k < v.size(); ++k) a += (k ? L", " : L"") + Quote(v[k]);
                        fields += L", " + Quote(key) + L": [" + a + L"]";
                    };
                    list(L"hidden", e.hidden);
                    list(L"order", e.order);
                }
                if (e.Kind() == ItemKind::Group) {
                    add(L"name", e.name);
                    std::wstring members;
                    for (size_t m = 0; m < e.members.size(); ++m) {
                        const DockItemEntry& me = e.members[m];
                        std::wstring mf = Quote(L"kind") + L": " + Quote(me.kind);
                        if (!me.aumid.empty()) mf += L", " + Quote(L"aumid") + L": " + Quote(me.aumid);
                        else if (!me.path.empty()) mf += L", " + Quote(L"path") + L": " + Quote(me.path);
                        if (!me.icon.empty()) mf += L", " + Quote(L"icon") + L": " + Quote(me.icon);
                        members += (m ? L",\n            { " : L"\n            { ") + mf + L" }";
                    }
                    fields += L", " + Quote(L"items") + L": [" + members + (e.members.empty() ? L"]" : L"\n          ]");
                }
                o += L"{ " + fields + L" }";
            }
            o += d.items.empty() ? L"],\n" : L"\n      ],\n";
            o += L"      \"edge\": " + Quote(ToString(d.edge)) + L", \"align\": " + Quote(ToString(d.align)) +
                 L", \"offset\": " + Num(d.offset) + (d.edgeGap != 0 ? L", \"edgeGap\": " + Num(d.edgeGap) : L"") +  // round 12 phase 12E
                 L", \"locked\": " + Bool(d.locked) + (d.enabled ? L"" : L", \"enabled\": false") + L",\n";
            o += L"      \"fullWidth\": " + Bool(d.fullWidth) + L", \"reserveSpace\": " + Bool(d.reserveSpace) +
                 L", \"peekPx\": " + Num(d.peekPx) + L", \"peekOpacity\": " + Num(d.peekOpacity) +
                 L", \"title\": " + Quote(d.title) + L",\n";
            o += L"      \"iconSize\": " + Num(d.iconSize) + L", \"magnification\": " + Num(d.magnification) +
                 L", \"magnifyRange\": " + Num(d.magnifyRange) + L",\n";
            o += L"      \"autoHide\": " + Quote(ToString(d.autoHide)) + L", \"hideDelayMs\": " + Num(d.hideDelayMs) +
                 L", \"revealDelayMs\": " + Num(d.revealDelayMs) + L",\n";
            o += L"      \"previews\": " + Bool(d.previews) + L", \"previewsSingleWindow\": " + Bool(d.previewSingle) +
                 L", \"previewDelayMs\": " + Num(d.previewDelayMs) + L",\n";
            o += L"      \"middleClick\": " + Quote(ToString(d.middleClick)) + L", \"wheel\": " + Quote(ToString(d.wheel)) +
                 L",\n";
            // Round 13 phase 13D: runningFilter, windows, titles, titlesVertical, thumbnails*, badges
            // are the running item's settings now; never written here.
            o += L"      \"monitor\": " + MonitorJson(d.monitor) + L", \"themePreset\": " + Quote(d.themePreset) +
                 L", \"layer\": " + Quote(ToString(d.layer)) +
                 (d.overflow != OverflowMode::Scroll ? L",\n      \"overflow\": " + Quote(ToString(d.overflow)) : L"") +
                 (d.shape != DockShape::Bar ? L",\n      \"shape\": " + Quote(ToString(d.shape)) : L"") +
                 (d.radius > 0 ? L", \"radius\": " + Num(d.radius) : L"") +
                 (d.rotateIcons ? L", \"rotateIcons\": true" : L"") +
                 (d.Floating() ? L",\n      \"floating\": true" : L"") +
                 (d.x != DockConfig::kCentred ? L", \"x\": " + Num(d.x) : L"") +
                 (d.y != DockConfig::kCentred ? L", \"y\": " + Num(d.y) : L"") +
                 // Round 7 phase 7A: only when not the defaults.
                 (d.maxLength > 0 ? L",\n      \"maxLength\": " + Num(d.maxLength) : L"") +
                 // Round 14 phase 14A: only when set (0 = automatic).
                 (d.length > 0 ? L",\n      \"length\": " + Num(d.length) : L"") +
                 (d.thickness > 0 ? L", \"thickness\": " + Num(d.thickness) : L"") +
                 (d.popupGap != DockConfig::kDefaultPopupGap ? L",\n      \"popupGap\": " + Num(d.popupGap) : L"") +
                 // Round 9 phase 9A: only when not the default.
                 (d.magnifyBar != MagnifyBar::Steady ? L",\n      \"magnifyBar\": " + Quote(ToString(d.magnifyBar)) : L"") +
                 // Round 12 phase 12B: the dock's own inline theme fields (its own look with changes).
                 (d.OwnLook() && d.themeOverrides != 0
                      ? L",\n      \"theme\": {" + Theme::WriteFields(d.theme, d.themeOverrides, L"        ").substr(1) + L" }"
                      : L"") +
                 // --- round 12 phase 12C: the name and the title options, only when not the defaults ---
                 (!d.name.empty() ? L",\n      \"name\": " + Quote(d.name) : L"") +
                 (d.titleShow != TitleShow::Always
                      ? L",\n      \"titleShow\": " + Quote(d.titleShow == TitleShow::Off ? L"off" : L"hover")
                      : L"") +
                 (d.titlePlace != TitlePlace::Outside ? L",\n      \"titlePlace\": \"inside\"" : L"") +
                 (d.titleUseName ? L",\n      \"titleUseName\": true" : L"") +
                 // --- end round 12 phase 12C ---
                 L"\n    }";
        }
        for (const std::wstring& extra : extraDocks) o += L",\n    " + extra;
        o += docks.empty() && extraDocks.empty() ? L"],\n" : L"\n  ],\n";
        o += L"  \"hideTaskbar\": " + Bool(hideTaskbar) + L",\n";
        o += L"  \"startWithWindows\": " + Bool(startWithWindows) + L",\n";
        o += L"  \"renderer\": " + Quote(renderer) + L",\n";
        o += L"  \"clock\": { \"format\": " + Quote(clock.format) + L", \"showSeconds\": " + Bool(clock.showSeconds) +
             L", \"showDate\": " + Bool(clock.showDate) + L",\n             \"click\": " + Quote(ToString(clock.click)) +
             L", \"clickTarget\": " + Quote(clock.clickTarget) +
             (clock.face != L"digital" ? L", \"face\": " + Quote(clock.face) : std::wstring()) +
             (!clock.timeZone.empty() ? L", \"timeZone\": " + Quote(clock.timeZone) : std::wstring()) +
             (!clock.label.empty() ? L", \"label\": " + Quote(clock.label) : std::wstring()) + L" },\n";
        // Only the preset and the fields set inline: editing the theme file keeps affecting the dock.
        o += L"  \"theme\": { \"preset\": " + Quote(themePreset) +
             Theme::WriteFields(theme, themeOverrides, L"             ") + L" },\n";
        o += L"  \"animation\": { \"speed\": " + Num(animation.speed) +
             L", \"hideStyle\": " + Quote(ToString(animation.hideStyle)) +
             L", \"launchStyle\": " + Quote(ToString(animation.launchStyle)) +
             L",\n                 \"magnifyFalloff\": " + Quote(ToString(animation.magnifyFalloff)) +
             L", \"easing\": " + Quote(ToString(animation.easing)) +
             L", \"labelFadeMs\": " + Num(animation.labelFadeMs) +
             L",\n                 \"reduceMotion\": " + Quote(ToString(animation.reduceMotion)) + L" },\n";
        o += L"  \"weather\": { \"latitude\": " + Coord(weather.latitude) +
             L", \"longitude\": " + Coord(weather.longitude) + L",\n               \"location\": " +
             Quote(weather.location) + L", \"units\": " + Quote(ToString(weather.units)) +
             L", \"refreshMinutes\": " + Num(weather.refreshMinutes) +
             (!weather.label.empty() ? L", \"label\": " + Quote(weather.label) : std::wstring()) + L" }";
        if (tray.enabled) o += L",\n  \"tray\": { \"enabled\": true }";
        if (notifications != NotificationsConfig{}) {
            std::wstring ex;
            for (size_t k = 0; k < notifications.excluded.size(); ++k) ex += (k ? L", " : L"") + Quote(notifications.excluded[k]);
            o += L",\n  \"notifications\": { \"pollS\": " + Num(notifications.pollS) + L", \"clearOnActivate\": " +
                 Bool(notifications.clearOnActivate) + L", \"balloons\": " + Bool(notifications.balloons) +
                 L", \"excluded\": [" + ex + L"] }";
        }
        if (calendars != CalendarsConfig{}) {
            o += L",\n  \"calendars\": { \"feeds\": [";
            for (size_t k = 0; k < calendars.feeds.size(); ++k) {
                const CalendarFeed& f = calendars.feeds[k];
                o += (k ? L",\n    { \"name\": " : L"\n    { \"name\": ") + Quote(f.name) + L", \"url\": " + Quote(f.url) +
                     L",\n      \"color\": " + Quote(FormatArgbColor(f.color)) + L", \"enabled\": " + Bool(f.enabled) +
                     L", \"refreshMin\": " + Num(f.refreshMin) +
                     (f.reminderMin > 0 ? L", \"reminderMin\": " + Num(f.reminderMin) : std::wstring()) + L" }";
            }
            o += calendars.feeds.empty() ? L"]" : L"\n  ]";
            o += L", \"showDots\": " + Bool(calendars.showDots) + L", \"defaultReminderMin\": " +
                 Num(calendars.defaultReminderMin) + L" }";
        }
        if (dashboard != DashboardConfig{}) o += L",\n  \"dashboard\": { \"hotkey\": " + Quote(dashboard.hotkey) + L" }";
        if (settings != SettingsUiConfig{}) {
            std::wstring w;
            for (int k = 0; k < SettingsUiConfig::kKinds && windowSizes; ++k) {
                const SettingsWindowSize& s = settings.windows[k];
                if (s == SettingsWindowSize{}) continue;
                w += (w.empty() ? L"" : L", ") + Quote(SettingsUiConfig::kNames[k]) + L": [" + Num(s.w) + L", " + Num(s.h) + L"]";
            }
            // Each part only when not the default (round 9 phase 9A: `fullscreen`; round 12 phase
            // 12A: `confirmPlacement`); the object only when a part is written.
            std::wstring parts;
            if (!w.empty()) parts = L"\"windows\": { " + w + L" }";
            if (!settings.fullscreenHide) parts += (parts.empty() ? L"" : L", ") + std::wstring(L"\"fullscreen\": \"ignore\"");
            if (!settings.confirmPlacement) parts += (parts.empty() ? L"" : L", ") + std::wstring(L"\"confirmPlacement\": false");
            // Round 14 phase 14B: `snapToScreen`, only when off.
            if (!settings.snapToScreen) parts += (parts.empty() ? L"" : L", ") + std::wstring(L"\"snapToScreen\": false");
            // Round 12 phase 12G: the tips' bits, with the windows' sizes (UI state: not in a snapshot).
            if (settings.tipsShown != 0 && windowSizes) parts += (parts.empty() ? L"" : L", ") + std::wstring(L"\"tipsShown\": ") + Num(settings.tipsShown);
            if (!parts.empty()) o += L",\n  \"settings\": { " + parts + L" }";
        }
        for (const auto& [key, value] : extraFields) o += L",\n  " + Quote(key) + L": " + value;
        o += L"\n}\n";
        return o;
    }
}

// --- round 12 phase 12C ---------------------------------------------------------------------------

std::wstring Config::LayoutsDirectory() { return Directory() + L"\\layouts"; }

std::wstring Config::DockJson(size_t index) const {
    if (index >= docks.size() || !docks[index].IsDock()) return {};
    // The writer is the config file's own (one dock in a copy), so a saved layout holds exactly the
    // keys Save writes for that dock, in the same format.
    try {
        Config one;
        one.docks = {docks[index]};
        one.extraDocks.clear();
        one.extraFields.clear();
        json::JsonObject root{nullptr};
        if (!json::JsonObject::TryParse(one.ToJson(false), root) || !root) return {};
        const json::JsonArray list = ReadArray(root, L"docks");
        if (!list || list.Size() == 0 || list.GetAt(0).ValueType() != json::JsonValueType::Object) return {};
        return std::wstring(list.GetAt(0).GetObject().Stringify());
    } catch (...) {
        return {};
    }
}
