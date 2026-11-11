// Round 9 phase 9C: the predefined dock setups (see Presets.h).

#include "Presets.h"

#include <algorithm>
#include <cwchar>
#include <iterator>

#include "JsonUtil.h"
#include "Theme.h"
#include "WidgetPackage.h"
#include "resource.h"

namespace Presets {

namespace {

using namespace JsonUtil;

// In list order (DockCommand 153's index). Round 11 phase 11C: "win7" after "windows" (sidebar,
// performance and dashboard moved from 4 - 6 to 5 - 7).
constexpr int kResources[] = {IDR_PRESET_BASIC,   IDR_PRESET_MINIMAL,     IDR_PRESET_MODERN,
                              IDR_PRESET_WINDOWS, IDR_PRESET_WIN7,        IDR_PRESET_SIDEBAR,
                              IDR_PRESET_PERFORMANCE, IDR_PRESET_DASHBOARD};

void Log(int resource, const std::wstring& text) {
    OutputDebugStringW((L"WinDock: preset resource " + std::to_wstring(resource) + L" skipped: " + text + L"\n").c_str());
}

bool ReadResource(int resource, std::string& out) {
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(resource), MAKEINTRESOURCEW(10));  // RT_RCDATA
    HGLOBAL mem = res ? LoadResource(module, res) : nullptr;
    const void* data = mem ? LockResource(mem) : nullptr;
    if (!data) return false;
    out.assign(static_cast<const char*>(data), SizeofResource(module, res));
    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF && static_cast<unsigned char>(out[1]) == 0xBB &&
        static_cast<unsigned char>(out[2]) == 0xBF) {
        out.erase(0, 3);
    }
    return true;
}

bool IsMarker(const DockItemEntry& e) { return CompareStringOrdinal(e.kind.c_str(), -1, kPinsKind, -1, TRUE) == CSTR_EQUAL; }

// One preset from its JSON text; false (with the reason) when it does not validate. Round 12 phase
// 12C: split out of LoadOne so a saved layout (`user`) goes through the same checks; for one, a
// theme file's stem is a valid look and its "look" key sets the dock's look.
bool ParsePreset(const std::wstring& text, Preset& p, std::wstring& reason, bool user) {
    try {
        json::JsonObject root{nullptr};
        if (!json::JsonObject::TryParse(text, root) || !root) {
            reason = L"malformed JSON";
            return false;
        }
        ReadString(root, L"id", p.id);
        ReadString(root, L"name", p.name);
        ReadString(root, L"tagline", p.tagline);
        ReadString(root, L"description", p.description);
        if (p.id.empty() || p.name.empty()) {
            reason = L"no id or name";
            return false;
        }
        // The dock object with the items in it: one docks[] entry for the config reader.
        json::JsonObject dock = json::JsonObject::Parse(L"{}");
        if (const json::JsonObject d = ReadObject(root, L"dock")) dock = json::JsonObject::Parse(d.Stringify());
        const json::JsonArray items = ReadArray(root, L"items");
        if (!items) {
            reason = L"\"" + p.id + L"\" has no items array";
            return false;
        }
        dock.SetNamedValue(L"items", items);
        DockConfig d;
        if (!ReadDockJson(std::wstring(dock.Stringify()), d)) {
            reason = L"\"" + p.id + L"\": the dock object could not be read";
            return false;
        }
        // Round 12 phase 12C: a saved layout's look ("app": the app look; else its own preset and
        // fields, read as a docks[] entry's "themePreset" and "theme" are).
        if (user) {
            if (const json::IJsonValue look = Find(root, L"look")) {
                if (look.ValueType() == json::JsonValueType::String) {
                    d.themePreset.clear();
                    d.themeOverrides = 0;
                } else if (look.ValueType() == json::JsonValueType::Object) {
                    const json::JsonObject lo = look.GetObject();
                    d.themePreset.clear();
                    ReadString(lo, L"preset", d.themePreset);
                    d.themeOverrides = 0;
                    if (const json::JsonObject t = ReadObject(lo, L"theme")) Theme::ReadFields(t, d.theme, &d.themeOverrides);
                }
            }
        }
        // Clamped like a loaded config (Sanitize also sorts the items into group order).
        Config tmp;
        tmp.docks = {std::move(d)};
        tmp.Sanitize();
        p.dock = std::move(tmp.docks.front());
        p.dock.id = p.id;
        if (p.dock.items.empty()) {
            reason = L"\"" + p.id + L"\" has no items";
            return false;
        }
        int markers = 0, running = 0;
        for (const DockItemEntry& e : p.dock.items) {
            if (IsMarker(e)) {
                ++markers;
                continue;
            }
            const ItemKind k = e.Kind();
            if (k == ItemKind::Unknown) {
                reason = L"\"" + p.id + L"\": unknown item kind \"" + e.kind + L"\"";
                return false;
            }
            if (k == ItemKind::Running) ++running;
            if (k == ItemKind::Widget) {
                std::wstring error;
                if (!Widgets::Load(e.id, error)) {
                    reason = L"\"" + p.id + L"\": widget \"" + e.id + L"\" is not installed (" + error + L")";
                    return false;
                }
            }
        }
        if (markers > 1 || running > 1) {
            reason = L"\"" + p.id + L"\": more than one pins marker or running item";
            return false;
        }
        if (!p.dock.themePreset.empty() && !Theme::FindBuiltIn(p.dock.themePreset)) {
            bool ok = false;
            if (user) Theme::Resolve(p.dock.themePreset, ok);  // a theme file (round 12 phase 12C)
            if (!ok) {
                reason = L"\"" + p.id + L"\": unknown theme preset \"" + p.dock.themePreset + L"\"";
                return false;
            }
        }
        p.hasPins = markers == 1;
        return true;
    } catch (...) {
        reason = L"read failed";
        return false;
    }
}

// One preset from its resource; false (with a log line) when it does not validate.
bool LoadOne(int resource, Preset& p) {
    std::string bytes;
    if (!ReadResource(resource, bytes)) {
        Log(resource, L"not found");
        return false;
    }
    std::wstring reason;
    if (!ParsePreset(Utf8ToWide(bytes), p, reason, false)) {
        Log(resource, reason);
        return false;
    }
    return true;
}

std::vector<DockItemEntry> BuildItems(const Preset& p, const Config& config, const std::vector<DockItemEntry>& pins, bool keepPins) {
    std::vector<DockItemEntry> out;
    for (const DockItemEntry& e : p.dock.items) {
        if (IsMarker(e)) {
            if (!keepPins) continue;
            for (DockItemEntry pin : pins) {
                pin.anchor = ItemAnchor::None;  // they stay at the marker
                pin.offset = 0;
                out.push_back(std::move(pin));
            }
            continue;
        }
        DockItemEntry item = e;
        if (item.Kind() == ItemKind::Clock) item.settings = ClockSettingsJson(ClockOf(config, e));
        if (item.Kind() == ItemKind::Weather) item.settings = WeatherSettingsJson(WeatherOf(config, e));
        out.push_back(std::move(item));
    }
    return out;
}

}  // namespace

const std::vector<Preset>& All() {
    static const std::vector<Preset> presets = [] {
        std::vector<Preset> list;
        for (int r : kResources) {
            Preset p;
            if (!LoadOne(r, p)) continue;
            if (std::any_of(list.begin(), list.end(), [&](const Preset& q) { return _wcsicmp(q.id.c_str(), p.id.c_str()) == 0; })) {
                Log(r, L"\"" + p.id + L"\": the id is taken");
                continue;
            }
            list.push_back(std::move(p));
        }
        return list;
    }();
    return presets;
}

size_t IndexOf(const std::wstring& id) {
    const std::vector<Preset>& all = All();
    for (size_t i = 0; i < all.size(); ++i) {
        if (_wcsicmp(all[i].id.c_str(), id.c_str()) == 0) return i;
    }
    return SIZE_MAX;
}

std::vector<std::wstring> ItemNames(const Preset& p, bool keepPins) {
    std::vector<std::wstring> names;
    for (const DockItemEntry& e : p.dock.items) {
        if (IsMarker(e)) {
            if (keepPins) names.push_back(L"Your pinned apps and folders");
            continue;
        }
        switch (e.Kind()) {
            case ItemKind::Running: names.push_back(L"Running apps"); break;
            case ItemKind::Separator: names.push_back(L"Separator"); break;
            case ItemKind::Clock: names.push_back(L"Clock"); break;
            case ItemKind::Weather: names.push_back(L"Weather"); break;
            case ItemKind::Notifications: names.push_back(L"Notifications"); break;
            case ItemKind::Tray: names.push_back(L"System tray"); break;
            case ItemKind::RecycleBin: names.push_back(L"Recycle Bin"); break;
            case ItemKind::Widget: names.push_back(Widgets::DisplayName(e.id)); break;
            default: names.push_back(e.kind); break;
        }
    }
    return names;
}

DockConfig Apply(const Preset& p, const Config& config, const DockConfig& target, const std::vector<DockItemEntry>& pins,
                 bool keepPins) {
    if (!target.IsDock()) return target;
    DockConfig d = p.dock;
    d.id = target.id;
    d.color = target.color;
    d.monitor = target.monitor;
    d.name = target.name;  // round 12 phase 12C: the dock keeps its name (a saved layout carries one)
    d.items = BuildItems(p, config, pins, keepPins);
    return d;
}

DockConfig MakeNew(const Preset& p, const Config& config, const std::vector<DockItemEntry>& pins, int monitor, bool placeFree) {
    DockConfig d = p.dock;
    d.id = config.UniqueDockId(p.id);
    d.color = 0;
    d.monitor = monitor;
    d.items = BuildItems(p, config, pins, !pins.empty());
    // Round 18 phase 18A: the next free edge of that display (the rule of App::DuplicateDock: only
    // enabled bar docks that are not floating or rings count; "each" is on every display).
    if (placeFree && d.shape == DockShape::Bar && !d.Floating()) {
        auto display = [](int m) { return m == DockConfig::kMonitorPrimary ? 0 : m; };
        bool used[4] = {};
        for (const DockConfig& o : config.docks) {
            if (!o.IsDock() || !o.enabled || o.Floating() || o.shape == DockShape::Circle) continue;
            if (display(o.monitor) != display(d.monitor) && !o.Each() && !d.Each()) continue;
            used[static_cast<int>(o.edge) & 3] = true;
        }
        if (used[static_cast<int>(d.edge) & 3]) {
            const DockEdge order[] = {DockEdge::Bottom, DockEdge::Top, DockEdge::Left, DockEdge::Right};
            const auto freeEdge = std::find_if(std::begin(order), std::end(order), [&](DockEdge e) { return !used[static_cast<int>(e)]; });
            if (freeEdge != std::end(order)) {
                d.edge = *freeEdge;
                d.offset = 0;
            } else if (!d.fullWidth) {
                // Every edge taken: along its own edge, one bar length further (a full-width bar has
                // no room to move: it overlaps, as two full-width docks on one edge always did).
                const int step = EstimatedLengthDip(d) + 8;
                d.offset = std::clamp(d.offset + (d.align == DockAlign::End ? -step : step), -16000, 16000);
            }
        }
    }
    return d;
}

std::wstring WhereText(const DockConfig& d) {
    // Round 18 phase 18A: moved here from PresetDialog.cpp (the gallery's hint shows it too).
    std::wstring s;
    if (d.Floating()) {
        const bool cx = d.x == DockConfig::kCentred, cy = d.y == DockConfig::kCentred;
        if (cx && cy) {
            s = L"Floating in the middle of the screen.";
        } else {
            const std::wstring v = cy ? L"middle" : d.y < 400 ? L"top" : L"bottom";
            const std::wstring h = cx ? L"" : d.x > 1000 ? L" right" : L" left";
            s = L"Floating at the " + v + h + L" of the screen.";
        }
    } else {
        s = std::wstring(d.edge == DockEdge::Top    ? L"Top"
                         : d.edge == DockEdge::Left ? L"Left"
                         : d.edge == DockEdge::Right ? L"Right"
                                                     : L"Bottom") +
            L" edge";
        const bool vertical = IsVertical(d.edge);
        if (d.fullWidth) {
            s += L", full length.";
        } else if (d.align == DockAlign::Start) {
            s += vertical ? L", at the top." : L", at the left.";
        } else if (d.align == DockAlign::End) {
            s += vertical ? L", at the bottom." : L", at the right.";
        } else {
            s += L", centred.";
        }
    }
    switch (d.layer) {
        case DockLayer::Behind: s += L" On the desktop, under your windows."; break;
        case DockLayer::Desktop: s += L" Shown while the desktop is showing."; break;
        default:
            s += !d.AutoHideOn()                           ? L" Always shown, above your windows."
                 : d.autoHide == AutoHideMode::Intelligent ? L" Hides while a window covers it."
                                                           : L" Hides when the pointer leaves it.";
            break;
    }
    if (d.locked) s += L" Locked.";
    return s;
}

RECT SilhouetteBar(const DockConfig& d, const RECT& in, int thick) {
    // Round 18 phase 18A: PresetDialog::PaintPreview's geometry, shared.
    const int iw = in.right - in.left, ih = in.bottom - in.top;
    const bool vertical = IsVertical(d.edge);
    if (d.Floating()) {
        const int bw = vertical ? thick : iw * 40 / 100, bh = vertical ? ih * 40 / 100 : thick;
        // x / y in DIPs from the work area (a nominal 1920 x 1080 one): left / centre / right.
        auto at = [](int v, int room, int size, int nominal) {
            if (v == DockConfig::kCentred) return (room - size) / 2;
            return std::clamp(static_cast<int>(static_cast<long long>(v) * room / nominal), 0, std::max(0, room - size));
        };
        const int fx = at(d.x, iw, bw, 1920), fy = at(d.y, ih, bh, 1080);
        return RECT{in.left + fx, in.top + fy, in.left + fx + bw, in.top + fy + bh};
    }
    const int length = vertical ? ih : iw;
    const int len = d.fullWidth ? length : length * 46 / 100;
    const int start = vertical ? in.top : in.left;
    const int pos = d.fullWidth || d.align == DockAlign::Center ? start + (length - len) / 2
                    : d.align == DockAlign::Start               ? start
                                                                : start + length - len;
    switch (d.edge) {
        case DockEdge::Top: return RECT{pos, in.top, pos + len, in.top + thick};
        case DockEdge::Left: return RECT{in.left, pos, in.left + thick, pos + len};
        case DockEdge::Right: return RECT{in.right - thick, pos, in.right, pos + len};
        case DockEdge::Bottom:
        default: return RECT{pos, in.bottom - thick, pos + len, in.bottom};
    }
}

int EstimatedLengthDip(const DockConfig& d) {
    return (d.iconSize + 8) * static_cast<int>(std::max<size_t>(1, d.items.size()));
}

std::vector<DockItemEntry> PinsOf(const DockConfig& dock) { return dock.IsDock() ? dock.Pins() : std::vector<DockItemEntry>{}; }

// --- round 12 phase 12C: saved layouts and looks ---------------------------------------------------

namespace {

std::wstring Trimmed(const std::wstring& s) {
    const size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return {};
    return s.substr(a, s.find_last_not_of(L" \t\r\n") - a + 1);
}

// The look a dock shows, by name: its own preset's (a built-in's name, a theme file's name, else
// the stem), or "app look".
std::wstring LookNameOf(const DockConfig& d) {
    if (!d.OwnLook()) return L"app look";
    if (const Theme::Preset* t = Theme::FindBuiltIn(d.themePreset)) return t->name;
    for (const Theme::UserTheme& u : Theme::ListUserThemes()) {
        if (_wcsicmp(u.id.c_str(), d.themePreset.c_str()) == 0) return u.name;
    }
    return d.themePreset;
}

std::wstring WhereWords(const DockConfig& d) {
    const wchar_t* edge = d.edge == DockEdge::Top ? L"top" : d.edge == DockEdge::Left ? L"left" : d.edge == DockEdge::Right ? L"right" : L"bottom";
    if (d.shape == DockShape::Circle) return L"circle";
    if (d.shape == DockShape::Arc) return std::wstring(L"half circle on the ") + edge + L" edge";
    if (d.Floating()) return L"floating";
    return std::wstring(edge) + L" edge" + (d.fullWidth ? L", full width" : L"");
}

// A layout file's text: the keys in a fixed order, one per line (the dock and each item compact).
std::wstring LayoutText(const json::JsonObject& root) {
    std::wstring o = L"{";
    bool first = true;
    auto key = [&](const wchar_t* k, const std::wstring& value) {
        o += (first ? L"\n  " : L",\n  ") + Quote(k) + L": " + value;
        first = false;
    };
    for (const wchar_t* k : {L"id", L"name", L"tagline", L"description", L"dock"}) {
        if (const json::IJsonValue v = Find(root, k)) key(k, std::wstring(v.Stringify()));
    }
    if (const json::JsonArray items = ReadArray(root, L"items")) {
        std::wstring a = L"[";
        for (uint32_t i = 0; i < items.Size(); ++i) a += (i ? L",\n    " : L"\n    ") + std::wstring(items.GetAt(i).Stringify());
        key(L"items", a + (items.Size() ? L"\n  ]" : L"]"));
    }
    if (const json::IJsonValue v = Find(root, L"look")) key(L"look", std::wstring(v.Stringify()));
    // Anything else a hand-edited file holds, kept.
    for (const auto& kv : root) {
        const std::wstring k(kv.Key());
        if (k == L"id" || k == L"name" || k == L"tagline" || k == L"description" || k == L"dock" || k == L"items" || k == L"look") continue;
        key(k.c_str(), std::wstring(kv.Value().Stringify()));
    }
    return o + L"\n}\n";
}

unsigned long WriteLayout(const std::wstring& path, const json::JsonObject& root) {
    DWORD error = 0;
    if (!WriteFileAtomic(path, WideToUtf8(LayoutText(root)), &error)) return error ? error : ERROR_WRITE_FAULT;
    return 0;
}

}  // namespace

std::wstring LayoutStem(const std::wstring& name) {
    std::wstring s;
    for (wchar_t c : Trimmed(name)) {
        if (c < 0x20 || wcschr(L"\\/:*?\"<>|", c)) c = L'-';
        s += c;
    }
    while (!s.empty() && (s.back() == L'.' || s.back() == L' ')) s.pop_back();  // Windows drops them
    if (s.size() > 64) s.resize(64);
    return s.empty() ? std::wstring(L"layout") : s;
}

MineList Mine() {
    MineList out;
    const std::wstring dir = Config::LayoutsDirectory();
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((dir + L"\\*.json").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring file = fd.cFileName;
        const std::wstring path = dir + L"\\" + file;
        std::string bytes;
        std::wstring reason;
        Preset p;
        if (!ReadFileUtf8(path, bytes)) {
            reason = L"could not be read";
        } else if (ParsePreset(Utf8ToWide(bytes), p, reason, true)) {
            // The file is the layout's identity: its stem is the id (Rename keeps it).
            p.id = file.size() > 5 ? file.substr(0, file.size() - 5) : file;
            p.dock.id = p.id;
            p.path = path;
            out.layouts.push_back(std::move(p));
            continue;
        }
        out.skipped.push_back(file + L": " + reason);
        OutputDebugStringW((L"WinDock: layout " + file + L" skipped: " + reason + L"\n").c_str());
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    std::sort(out.layouts.begin(), out.layouts.end(), [](const Preset& a, const Preset& b) {
        const int c = _wcsicmp(a.name.c_str(), b.name.c_str());
        return c != 0 ? c < 0 : _wcsicmp(a.id.c_str(), b.id.c_str()) < 0;
    });
    return out;
}

std::wstring LookTagline(const std::wstring& name, const ThemeConfig& values) {
    const int transparency = Theme::Derived(Theme::Quick::Transparency).get(values);
    const int corners = Theme::Derived(Theme::Quick::Corners).get(values);
    const int effects = Theme::Derived(Theme::Quick::Effects).get(values);
    const wchar_t* cornerText = corners == 0 ? L"square corners" : corners == 1 ? L"soft corners" : corners == 2 ? L"round corners" : L"custom corners";
    const wchar_t* effectText = effects == 1 ? L"glow" : effects == 2 ? L"reflection" : effects == 3 ? L"glow and reflection" : L"no icon effects";
    return name + L" \x00B7 " + std::to_wstring(transparency) + L" % transparent \x00B7 " + cornerText + L" \x00B7 " + effectText;
}

std::vector<Look> Looks() {
    std::vector<Look> out;
    size_t count = 0;
    const Theme::Preset* presets = Theme::BuiltIns(count);
    for (size_t i = 0; i < count; ++i) out.push_back(Look{presets[i].id, presets[i].name, LookTagline(presets[i].name, presets[i].values), presets[i].values, false});
    for (const Theme::UserTheme& t : Theme::ListUserThemes()) {
        ThemeConfig v;
        if (!t.valid || !Theme::LoadUserTheme(t.path, v, nullptr)) continue;  // an unreadable file is not a look to apply
        out.push_back(Look{t.id, t.name, LookTagline(t.name, v), v, true});
    }
    return out;
}

unsigned long SaveLayout(const Config& config, size_t cfg, const std::wstring& name, bool overwrite, std::wstring& id) {
    if (cfg >= config.docks.size() || !config.docks[cfg].IsDock()) return ERROR_INVALID_PARAMETER;
    const std::wstring title = Trimmed(name);
    if (title.empty()) return ERROR_INVALID_NAME;
    id = LayoutStem(title);
    const std::wstring dir = Config::LayoutsDirectory();
    if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return GetLastError();
    const std::wstring path = dir + L"\\" + id + L".json";
    if (!overwrite && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return ERROR_FILE_EXISTS;
    const DockConfig& d = config.docks[cfg];
    const std::wstring dockText = config.DockJson(cfg);
    if (dockText.empty()) return ERROR_INVALID_DATA;
    try {
        json::JsonObject dock = json::JsonObject::Parse(dockText);
        const json::JsonArray written = ReadArray(dock, L"items");
        // Not the dock's identity (the target keeps its own), not its look (the "look" key says it).
        for (const wchar_t* k : {L"id", L"color", L"enabled", L"items", L"themePreset", L"theme"}) {
            if (dock.HasKey(k)) dock.Remove(k);
        }
        // The items, with the pinned run replaced by the marker (where the first pin was; a dock
        // without pins: where InsertPin puts a pin, so "Keep my pinned apps" has a place to go).
        json::JsonObject marker;
        marker.SetNamedValue(L"kind", json::JsonValue::CreateStringValue(kPinsKind));
        json::JsonArray items;
        const bool anyPin = std::any_of(d.items.begin(), d.items.end(), [](const DockItemEntry& e) { return e.IsPin(); });
        size_t markAt = 0;
        if (!anyPin) {
            const auto running = std::find_if(d.items.begin(), d.items.end(), [](const DockItemEntry& e) { return e.Kind() == ItemKind::Running; });
            markAt = running == d.items.end() ? 0 : static_cast<size_t>(running - d.items.begin());
        }
        bool marked = false;
        const uint32_t n = written ? written.Size() : 0;
        for (uint32_t i = 0; i < n; ++i) {
            if (!anyPin && i == markAt) {
                items.Append(marker);
                marked = true;
            }
            if (i < d.items.size() && d.items[i].IsPin()) {
                if (!marked) items.Append(marker);
                marked = true;
                continue;
            }
            items.Append(written.GetAt(i));
        }
        if (!marked) items.Append(marker);
        // The look: its own preset and fields, else the app look.
        json::IJsonValue look = json::JsonValue::CreateStringValue(L"app");
        if (d.OwnLook()) {
            json::JsonObject lo;
            lo.SetNamedValue(L"preset", json::JsonValue::CreateStringValue(d.themePreset));
            const std::wstring fields = Theme::WriteFields(d.theme, d.themeOverrides, L"");
            lo.SetNamedValue(L"theme", json::JsonObject::Parse(L"{" + (fields.empty() ? std::wstring() : fields.substr(1)) + L"}"));
            look = lo;
        }
        wchar_t date[64]{};
        if (!GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, nullptr, nullptr, date, static_cast<int>(std::size(date)), nullptr)) date[0] = 0;
        json::JsonObject root;
        root.SetNamedValue(L"id", json::JsonValue::CreateStringValue(id));
        root.SetNamedValue(L"name", json::JsonValue::CreateStringValue(title));
        root.SetNamedValue(L"tagline", json::JsonValue::CreateStringValue(std::to_wstring(d.items.size()) +
                                                                          (d.items.size() == 1 ? L" item \x00B7 " : L" items \x00B7 ") +
                                                                          LookNameOf(d) + L" \x00B7 " + WhereWords(d)));
        root.SetNamedValue(L"description",
                           json::JsonValue::CreateStringValue(L"Saved from the dock " + d.DisplayName() + (date[0] ? L" on " + std::wstring(date) : L"") + L"."));
        root.SetNamedValue(L"dock", dock);
        root.SetNamedValue(L"items", items);
        root.SetNamedValue(L"look", look);
        return WriteLayout(path, root);
    } catch (...) {
        return ERROR_INVALID_DATA;
    }
}

unsigned long RenameLayout(const Preset& layout, const std::wstring& name) {
    const std::wstring title = Trimmed(name);
    if (layout.path.empty() || title.empty()) return ERROR_INVALID_PARAMETER;
    try {
        json::JsonObject root{nullptr};
        if (!ReadObjectFile(layout.path, root) || !root) return ERROR_INVALID_DATA;
        root.SetNamedValue(L"name", json::JsonValue::CreateStringValue(title));
        return WriteLayout(layout.path, root);
    } catch (...) {
        return ERROR_INVALID_DATA;
    }
}

unsigned long DeleteLayout(const Preset& layout) {
    if (layout.path.empty()) return ERROR_INVALID_PARAMETER;
    return DeleteFileW(layout.path.c_str()) ? 0 : GetLastError();
}

}  // namespace Presets
