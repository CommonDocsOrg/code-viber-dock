#include "Config.h"

#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <iterator>

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
const wchar_t* const kLayerNames[] = {L"normal", L"desktop"};
const wchar_t* const kItemKindNames[] = {L"",        L"app",   L"folder", L"separator",
                                         L"recycleBin", L"running", L"clock", L"weather"};

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
    ReadBool(o, L"fullWidth", d.fullWidth);
    ReadBool(o, L"reserveSpace", d.reserveSpace);
    ReadInt(o, L"peekPx", d.peekPx);
    ReadNumber(o, L"peekOpacity", d.peekOpacity);
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
    s.clear();
    ReadString(o, L"runningFilter", s);
    if (!s.empty()) FromString(s, d.runningFilter);  // unknown text: all
    ReadString(o, L"themePreset", d.themePreset);
    s.clear();
    ReadString(o, L"layer", s);
    if (!s.empty()) FromString(s, d.layer);  // unknown text: normal
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

}  // namespace

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
    if (kind != ItemKind::Clock && kind != ItemKind::Weather && kind != ItemKind::Running) return SIZE_MAX;
    if (kind == ItemKind::Running && Has(ItemKind::Running)) return SIZE_MAX;  // only the first one expands
    size_t at = items.size();
    if (kind == ItemKind::Weather && !items.empty() && items.back().Kind() == ItemKind::Clock) {
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

std::wstring FormatArgbColor(uint32_t argb) {
    wchar_t buf[16];
    swprintf_s(buf, L"#%08X", argb);
    return buf;
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
    for (DockConfig& d : docks) {
        d.ownTheme = false;
        if (d.themePreset.empty() || d.themePreset == themePreset) continue;
        bool ok = true;
        ThemeConfig t = Theme::Resolve(d.themePreset, ok);
        if (!ok) continue;  // unknown preset / broken file: the global theme
        for (size_t i = 0; i < Theme::FieldCount(); ++i) {
            const Theme::FieldInfo& f = Theme::Fields()[i];
            if (themeOverrides & Theme::Bit(f.field)) Theme::CopyField(f, theme, t);
        }
        d.theme = t;
        d.ownTheme = true;
    }
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
    if (docks.empty()) docks.emplace_back();
    for (DockConfig& d : docks) {
        if (d.id.empty()) d.id = L"main";
        if (d.id.size() > 64) d.id.resize(64);
        if (static_cast<unsigned>(d.edge) > static_cast<unsigned>(DockEdge::Right)) d.edge = DockEdge::Bottom;
        if (static_cast<unsigned>(d.align) > static_cast<unsigned>(DockAlign::End)) d.align = DockAlign::Center;
        d.offset = std::clamp(d.offset, -16000, 16000);
        d.peekPx = std::clamp(d.peekPx, 0, 64);
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
        if (static_cast<unsigned>(d.runningFilter) > static_cast<unsigned>(RunningFilter::Monitor)) {
            d.runningFilter = RunningFilter::All;
        }
        if (static_cast<unsigned>(d.layer) > static_cast<unsigned>(DockLayer::Desktop)) d.layer = DockLayer::Normal;
        // A file stem inside the themes folder, like theme.preset: no paths.
        if (d.themePreset.size() > 64 || d.themePreset.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos ||
            d.themePreset == L"." || d.themePreset == L"..") {
            d.themePreset.clear();
        }
    }
    // Unique ids (the docks are matched by id when the config is re-applied): a repeated id gets a
    // number appended.
    for (size_t i = 1; i < docks.size(); ++i) {
        const bool repeated = std::any_of(docks.begin(), docks.begin() + static_cast<ptrdiff_t>(i),
                                          [&](const DockConfig& d) { return d.id == docks[i].id; });
        if (repeated) docks[i].id = UniqueDockId(docks[i].id);
    }
    if (editDock >= docks.size()) editDock = 0;
    if (!(theme.cornerRadius >= 0.0f)) theme.cornerRadius = 0.0f;
    theme.cornerRadius = std::min(theme.cornerRadius, 64.0f);
    if (!(theme.labelCornerRadius >= 0.0f)) theme.labelCornerRadius = 0.0f;
    theme.labelCornerRadius = std::min(theme.labelCornerRadius, 32.0f);
    if (!(theme.shelfSlant >= 0.0f)) theme.shelfSlant = 0.0f;
    theme.shelfSlant = std::min(theme.shelfSlant, 64.0f);
    // The icon effects (Theme::ReadFields clamps what it reads; this also covers values set in code).
    auto clampf = [](float& v, float lo, float hi, float fallback) {
        v = std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
    };
    if (theme.glow < 0 || theme.glow > 3) theme.glow = 0;
    clampf(theme.glowRadius, 4.0f, 32.0f, 14.0f);
    clampf(theme.glowStrength, 0.0f, 1.0f, 0.6f);
    clampf(theme.reflectionHeight, 0.2f, 1.0f, 0.5f);
    clampf(theme.reflectionOpacity, 0.0f, 1.0f, 0.35f);
    clampf(theme.perspectiveAngle, 0.0f, 35.0f, 18.0f);
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
    if (clock.format.size() > 128) clock.format.resize(128);
    if (clock.clickTarget.size() > 2048) clock.clickTarget.resize(2048);
    if (renderer != L"auto" && renderer != L"software") renderer = L"auto";
    if (!(weather.latitude >= -90.0 && weather.latitude <= 90.0)) weather.latitude = 0.0;  // also NaN
    if (!(weather.longitude >= -180.0 && weather.longitude <= 180.0)) weather.longitude = 0.0;
    weather.latitude = std::round(weather.latitude * 1e4) / 1e4;  // what Save writes (4 decimals)
    weather.longitude = std::round(weather.longitude * 1e4) / 1e4;
    weather.refreshMinutes = std::clamp(weather.refreshMinutes, 5, 360);
    if (weather.location.size() > 128) weather.location.resize(128);
}

Config::LoadResult Config::LoadFile() {
    *this = Config{};
    weather.units = LocaleUnits();  // the default when the file (or its "units" key) is absent
    const std::wstring path = FilePath();

    std::string bytes;
    if (!ReadFileUtf8(path, bytes)) {
        // Missing (first run), or present but unreadable (treated like a malformed file).
        return GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES ? LoadResult::Missing
                                                                           : LoadResult::Malformed;
    }

    try {
        json::JsonObject root{nullptr};
        if (!json::JsonObject::TryParse(Utf8ToWide(bytes), root) || !root) {
            OutputDebugStringW(L"WinDock: config.json is malformed; using defaults\n");
            return LoadResult::Malformed;
        }

        // The legacy (pre-`docks`) keys are known too: in a current file they are ignored and
        // dropped on the next save; in a legacy file they are migrated below.
        static const wchar_t* const kKnown[] = {
            L"docks",         L"hideTaskbar", L"startWithWindows", L"renderer",       L"clock",
            L"theme",         L"animation",   L"weather",          L"pinned",         L"iconSize",
            L"magnification", L"magnifyRange", L"autoHide",        L"hideDelayMs",    L"revealDelayMs",
            L"previews",      L"previewDelayMs", L"monitor",
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
        if (clockObj) {
            ReadString(clockObj, L"format", clock.format);
            ReadBool(clockObj, L"showSeconds", clock.showSeconds);
            ReadBool(clockObj, L"showDate", clock.showDate);
            std::wstring click;
            ReadString(clockObj, L"click", click);
            if (!click.empty()) {
                FromString(click, clock.click);  // unknown text: keep the default
            } else {
                bool calendarPopup = true;  // before "click": a bool for the calendar popup
                ReadBool(clockObj, L"calendarPopup", calendarPopup);
                if (!calendarPopup) clock.click = ClockClick::None;
            }
            ReadString(clockObj, L"clickTarget", clock.clickTarget);
        }
        const json::JsonObject weatherObj = ReadObject(root, L"weather");

        if (root.HasKey(L"docks")) {
            docks.clear();
            if (auto arr = ReadArray(root, L"docks")) {
                for (const auto& dv : arr) {
                    if (dv.ValueType() != json::JsonValueType::Object) continue;
                    const json::JsonObject obj = dv.GetObject();
                    DockConfig d;
                    ReadString(obj, L"id", d.id);
                    ReadBool(obj, L"locked", d.locked);
                    ReadDockFields(obj, d);
                    ReadDockOnlyFields(obj, d);
                    if (auto items = ReadArray(obj, L"items")) {
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
                            const bool target = !e.path.empty() || !e.aumid.empty();
                            if (e.kind.empty()) {
                                if (!target) continue;
                                e.kind = L"app";  // a hand-written entry without a kind
                            }
                            const ItemKind k = e.Kind();
                            if ((k == ItemKind::App || k == ItemKind::Folder) && !target) continue;  // nothing to show
                            d.items.push_back(std::move(e));
                        }
                    }
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

        if (auto w = weatherObj) {
            ReadNumber(w, L"latitude", weather.latitude);
            ReadNumber(w, L"longitude", weather.longitude);
            ReadString(w, L"location", weather.location);
            ReadEnum(w, L"units", weather.units);
            ReadInt(w, L"refreshMinutes", weather.refreshMinutes);
        }
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
    } catch (...) {
        OutputDebugStringW(L"WinDock: failed to read config.json; using defaults\n");
        *this = Config{};
        weather.units = LocaleUnits();
        return LoadResult::Malformed;
    }
    Sanitize();  // also validates themePreset before it is used as a file name

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
    ResolveDockThemes();
    return LoadResult::Ok;
}

bool Config::Save() const {
    try {
        std::wstring dir = Directory();
        CreateDirectoryW(dir.c_str(), nullptr);

        std::wstring o;
        o += L"{\n  \"docks\": [";
        for (size_t di = 0; di < docks.size(); ++di) {
            const DockConfig& d = docks[di];
            o += di ? L",\n    {\n" : L"\n    {\n";
            o += L"      \"id\": " + Quote(d.id) + L",\n      \"items\": [";
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
                o += L"{ " + fields + L" }";
            }
            o += d.items.empty() ? L"],\n" : L"\n      ],\n";
            o += L"      \"edge\": " + Quote(ToString(d.edge)) + L", \"align\": " + Quote(ToString(d.align)) +
                 L", \"offset\": " + Num(d.offset) + L", \"locked\": " + Bool(d.locked) + L",\n";
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
            o += L"      \"monitor\": " + MonitorJson(d.monitor) + L", \"runningFilter\": " +
                 Quote(ToString(d.runningFilter)) + L", \"themePreset\": " + Quote(d.themePreset) + L", \"layer\": " +
                 Quote(ToString(d.layer)) + L"\n    }";
        }
        o += docks.empty() ? L"],\n" : L"\n  ],\n";
        o += L"  \"hideTaskbar\": " + Bool(hideTaskbar) + L",\n";
        o += L"  \"startWithWindows\": " + Bool(startWithWindows) + L",\n";
        o += L"  \"renderer\": " + Quote(renderer) + L",\n";
        o += L"  \"clock\": { \"format\": " + Quote(clock.format) + L", \"showSeconds\": " + Bool(clock.showSeconds) +
             L", \"showDate\": " + Bool(clock.showDate) + L",\n             \"click\": " + Quote(ToString(clock.click)) +
             L", \"clickTarget\": " + Quote(clock.clickTarget) + L" },\n";
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
             L", \"refreshMinutes\": " + Num(weather.refreshMinutes) + L" }";
        for (const auto& [key, value] : extraFields) o += L",\n  " + Quote(key) + L": " + value;
        o += L"\n}\n";
        return WriteFileAtomic(FilePath(), WideToUtf8(o));
    } catch (...) {
        return false;
    }
}
