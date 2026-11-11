#include "WidgetPackage.h"

#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <iterator>

#include "ClockFaces.h"
#include "Config.h"
#include "JsonUtil.h"
#include "resource.h"

namespace Widgets {

namespace {

namespace json = winrt::Windows::Data::Json;
using JsonUtil::Utf8ToWide;
using JsonUtil::WideToUtf8;

constexpr size_t kMaxFileBytes = 8u << 20;
constexpr size_t kMaxListedFiles = 512;

// The built-in widgets: every file of each, as an RCDATA resource (res/app.rc).
struct BuiltInFile {
    const wchar_t* id;
    const char* path;
    int resource;
};
constexpr BuiltInFile kBuiltIns[] = {
    {L"hello", "widget.json", IDR_WIDGET_HELLO_JSON},
    {L"hello", "main.js", IDR_WIDGET_HELLO_MAIN},
    {L"digital-clock", "widget.json", IDR_WIDGET_CLOCK_JSON},
    {L"digital-clock", "main.js", IDR_WIDGET_CLOCK_MAIN},
    {L"analog-clock", "widget.json", IDR_WIDGET_ANALOG_JSON},
    {L"analog-clock", "main.js", IDR_WIDGET_ANALOG_MAIN},
    {L"analog-clock", "faces/classic.json", IDR_WIDGET_ANALOG_FACE_CLASSIC},
    {L"analog-clock", "faces/minimal.json", IDR_WIDGET_ANALOG_FACE_MINIMAL},
    {L"analog-clock", "faces/modern-dark.json", IDR_WIDGET_ANALOG_FACE_MODERN_DARK},
    {L"analog-clock", "faces/roman.json", IDR_WIDGET_ANALOG_FACE_ROMAN},
    {L"analog-clock", "faces/station.json", IDR_WIDGET_ANALOG_FACE_STATION},
    {L"analog-date", "widget.json", IDR_WIDGET_DATE_JSON},
    {L"analog-date", "main.js", IDR_WIDGET_DATE_MAIN},
    {L"volume", "widget.json", IDR_WIDGET_VOLUME_JSON},
    {L"volume", "main.js", IDR_WIDGET_VOLUME_MAIN},
    {L"media", "widget.json", IDR_WIDGET_MEDIA_JSON},
    {L"media", "main.js", IDR_WIDGET_MEDIA_MAIN},
    {L"start", "widget.json", IDR_WIDGET_START_JSON},
    {L"start", "main.js", IDR_WIDGET_START_MAIN},
    {L"search", "widget.json", IDR_WIDGET_SEARCH_JSON},
    {L"search", "main.js", IDR_WIDGET_SEARCH_MAIN},
    {L"notification-bell", "widget.json", IDR_WIDGET_BELL_JSON},
    {L"notification-bell", "main.js", IDR_WIDGET_BELL_MAIN},
    {L"agenda", "widget.json", IDR_WIDGET_AGENDA_JSON},
    {L"agenda", "main.js", IDR_WIDGET_AGENDA_MAIN},
    {L"weather", "widget.json", IDR_WIDGET_WEATHER_JSON},
    {L"weather", "main.js", IDR_WIDGET_WEATHER_MAIN},
    {L"weather", "meteocons.js", IDR_WIDGET_WEATHER_METEOCONS},  // round 16 phase 16F
    {L"notes", "widget.json", IDR_WIDGET_NOTES_JSON},
    {L"notes", "main.js", IDR_WIDGET_NOTES_MAIN},
    {L"system-monitor", "widget.json", IDR_WIDGET_SYSMON_JSON},
    {L"system-monitor", "main.js", IDR_WIDGET_SYSMON_MAIN},
    {L"system-overview", "widget.json", IDR_WIDGET_SYSVIEW_JSON},
    {L"system-overview", "main.js", IDR_WIDGET_SYSVIEW_MAIN},
    {L"battery", "widget.json", IDR_WIDGET_BATTERY_JSON},
    {L"battery", "main.js", IDR_WIDGET_BATTERY_MAIN},
    {L"show-desktop", "widget.json", IDR_WIDGET_SHOWDESKTOP_JSON},  // round 11 phase 11B
    {L"show-desktop", "main.js", IDR_WIDGET_SHOWDESKTOP_MAIN},
    {L"stocks", "widget.json", IDR_WIDGET_STOCKS_JSON},  // round 16 phase 16C
    {L"stocks", "main.js", IDR_WIDGET_STOCKS_MAIN},
    {L"photo-frame", "widget.json", IDR_WIDGET_PHOTOFRAME_JSON},  // round 17 phase 17A
    {L"photo-frame", "main.js", IDR_WIDGET_PHOTOFRAME_MAIN},
};

bool IsBuiltIn(const std::wstring& id) {
    return std::any_of(std::begin(kBuiltIns), std::end(kBuiltIns), [&](const BuiltInFile& f) { return id == f.id; });
}

bool ReadResource(int resource, std::string& out) {
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(resource), MAKEINTRESOURCEW(10));  // RT_RCDATA
    if (!res) return false;
    HGLOBAL mem = LoadResource(module, res);
    const DWORD size = SizeofResource(module, res);
    const void* data = mem ? LockResource(mem) : nullptr;
    if (!data) return false;
    out.assign(static_cast<const char*>(data), size);
    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF && static_cast<unsigned char>(out[1]) == 0xBB &&
        static_cast<unsigned char>(out[2]) == 0xBF) {
        out.erase(0, 3);
    }
    return true;
}

// The canonical path of an open handle ("C:\..." without the \\?\ prefix), or "".
std::wstring FinalPath(HANDLE h) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetFinalPathNameByHandleW(h, buf, static_cast<DWORD>(std::size(buf)), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (n == 0 || n >= std::size(buf)) return {};
    std::wstring p(buf, n);
    if (p.rfind(L"\\\\?\\UNC\\", 0) == 0) {
        p = L"\\\\" + p.substr(8);
    } else if (p.rfind(L"\\\\?\\", 0) == 0) {
        p = p.substr(4);
    }
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    return p;
}

std::wstring CanonicalFolder(const std::wstring& folder) {
    HANDLE h = CreateFileW(folder.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::wstring p = FinalPath(h);
    CloseHandle(h);
    return p;
}

bool StartsWithFolder(const std::wstring& path, const std::wstring& folder) {
    if (path.size() <= folder.size() + 1) return false;
    if (CompareStringOrdinal(path.c_str(), static_cast<int>(folder.size()), folder.c_str(),
                             static_cast<int>(folder.size()), TRUE) != CSTR_EQUAL) {
        return false;
    }
    return path[folder.size()] == L'\\';
}

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

double Clamp(double v, double lo, double hi) { return std::min(std::max(v, lo), hi); }

std::wstring NumberJson(double v) { return JsonUtil::Num(v); }

SettingField::Type TypeOf(const std::wstring& t, bool& ok) {
    ok = true;
    const std::wstring s = Lower(t);
    if (s == L"bool" || s == L"boolean") return SettingField::Type::Bool;
    if (s == L"int" || s == L"integer") return SettingField::Type::Int;
    if (s == L"number") return SettingField::Type::Number;
    if (s == L"string" || s == L"text") return SettingField::Type::String;
    if (s == L"choice" || s == L"enum") return SettingField::Type::Choice;
    if (s == L"colour" || s == L"color") return SettingField::Type::Color;
    if (s == L"font") return SettingField::Type::Font;
    if (s == L"place") return SettingField::Type::Place;
    if (s == L"timezone") return SettingField::Type::TimeZone;
    if (s == L"file") return SettingField::Type::File;      // round 17 phase 17A
    if (s == L"folder") return SettingField::Type::Folder;  // round 17 phase 17A
    ok = false;
    return SettingField::Type::String;
}

void ReadSize(const json::JsonObject& size, const wchar_t* key, SizeHint& hint) {
    if (!size) return;
    if (auto o = JsonUtil::ReadObject(size, key)) {
        double m = hint.main, c = hint.cross;
        JsonUtil::ReadNumber(o, L"main", m);
        JsonUtil::ReadNumber(o, L"cross", c);
        hint.main = static_cast<float>(Clamp(m, 4.0, 1024.0));
        hint.cross = static_cast<float>(Clamp(c, 4.0, 512.0));
    }
}

// The `standalone` object (round 6 phase 6B): minimum size, aspect, the default size.
void ReadStandalone(const json::JsonObject& root, Manifest& m) {
    const json::JsonObject o = JsonUtil::ReadObject(root, L"standalone");
    if (!o) return;
    StandaloneHint& s = m.standalone;
    double v = s.minWidth;
    JsonUtil::ReadNumber(o, L"minWidth", v);
    s.minWidth = static_cast<float>(Clamp(v, 16.0, 4000.0));
    v = s.minHeight;
    JsonUtil::ReadNumber(o, L"minHeight", v);
    s.minHeight = static_cast<float>(Clamp(v, 16.0, 4000.0));
    v = 0.0;
    JsonUtil::ReadNumber(o, L"defaultWidth", v);
    s.defaultWidth = v > 0.0 ? static_cast<float>(Clamp(v, 16.0, 4000.0)) : 0.0f;
    v = 0.0;
    JsonUtil::ReadNumber(o, L"defaultHeight", v);
    s.defaultHeight = v > 0.0 ? static_cast<float>(Clamp(v, 16.0, 4000.0)) : 0.0f;
    JsonUtil::ReadBool(o, L"supported", s.supported);  // round 16 phase 16E: false = needs a dock
    if (const json::IJsonValue a = JsonUtil::Find(o, L"aspect")) {
        if (a.ValueType() == json::JsonValueType::Number && std::isfinite(a.GetNumber()) && a.GetNumber() > 0.0) {
            s.aspect = StandaloneHint::Aspect::Ratio;
            s.ratio = static_cast<float>(Clamp(a.GetNumber(), 0.01, 100.0));
        } else if (a.ValueType() == json::JsonValueType::String && Lower(std::wstring(a.GetString())) == L"keep") {
            s.aspect = StandaloneHint::Aspect::Keep;
        } else if (!(a.ValueType() == json::JsonValueType::String && Lower(std::wstring(a.GetString())) == L"free")) {
            m.warnings.push_back(L"standalone.aspect: not \"free\", \"keep\" or a number > 0 (free used)");
        }
    }
}

// The `preview` object (round 6 phase 6E): settings for the drawer's preview, its box.
void ReadPreview(const json::JsonObject& root, Manifest& m) {
    const json::JsonObject o = JsonUtil::ReadObject(root, L"preview");
    if (!o) return;
    if (const json::JsonObject s = JsonUtil::ReadObject(o, L"settings")) m.previewSettings = std::wstring(s.Stringify());
    double v = 0.0;
    JsonUtil::ReadNumber(o, L"width", v);
    m.previewWidth = v > 0.0 ? static_cast<float>(Clamp(v, 8.0, 1024.0)) : 0.0f;
    v = 0.0;
    JsonUtil::ReadNumber(o, L"height", v);
    m.previewHeight = v > 0.0 ? static_cast<float>(Clamp(v, 8.0, 512.0)) : 0.0f;
}

void ReadPermissions(const json::JsonObject& root, Manifest& m) {
    const json::JsonArray perms = JsonUtil::ReadArray(root, L"permissions");
    if (!perms) return;
    auto addHost = [&](std::wstring h) {
        h = Lower(h);
        if (!h.empty() && h.find_first_of(L"/:\\ ") == std::wstring::npos) {
            m.httpHosts.push_back(std::move(h));
        } else {
            m.warnings.push_back(L"permissions: ignored http host \"" + h + L"\"");
        }
    };
    for (const auto& v : perms) {
        if (v.ValueType() == json::JsonValueType::String) {
            const std::wstring s = Lower(std::wstring(v.GetString()));
            if (s == L"shell") {
                m.shell = true;
            } else if (s == L"audio") {
                m.audio = true;
            } else if (s == L"media") {
                m.media = true;
            } else if (s == L"notifications") {
                m.notifications = true;
            } else if (s == L"calendar") {
                m.calendar = true;
            } else if (s == L"weather") {
                m.weather = true;
            } else if (s == L"perf") {
                m.perf = true;
            } else if (s == L"http") {
                m.http = true;  // no hosts: every request is refused (name the hosts)
            } else if (s.rfind(L"http:", 0) == 0) {
                m.http = true;
                addHost(s.substr(5));
            } else {
                m.warnings.push_back(L"permissions: unknown \"" + s + L"\"");
            }
        } else if (v.ValueType() == json::JsonValueType::Object) {
            const json::JsonObject o = v.GetObject();
            if (const json::JsonArray hosts = JsonUtil::ReadArray(o, L"http")) {
                m.http = true;
                for (const auto& h : hosts) {
                    if (h.ValueType() == json::JsonValueType::String) addHost(std::wstring(h.GetString()));
                }
            }
            bool shell = false;
            JsonUtil::ReadBool(o, L"shell", shell);
            if (shell) m.shell = true;
        }
    }
}

void ReadSettingsSchema(const json::JsonObject& root, Manifest& m) {
    const json::JsonArray arr = JsonUtil::ReadArray(root, L"settings");
    if (!arr) return;
    for (const auto& v : arr) {
        if (v.ValueType() != json::JsonValueType::Object) continue;
        const json::JsonObject o = v.GetObject();
        SettingField f;
        JsonUtil::ReadString(o, L"key", f.key);
        if (f.key.empty() || f.key.size() > 64) {
            m.warnings.push_back(L"settings: an entry without a key");
            continue;
        }
        if (std::any_of(m.settings.begin(), m.settings.end(), [&](const SettingField& s) { return s.key == f.key; })) {
            m.warnings.push_back(L"settings: duplicate key \"" + f.key + L"\"");
            continue;
        }
        std::wstring type = L"string";
        JsonUtil::ReadString(o, L"type", type);
        bool ok = true;
        f.type = TypeOf(type, ok);
        if (!ok) m.warnings.push_back(L"settings." + f.key + L": unknown type \"" + type + L"\" (read as string)");
        JsonUtil::ReadString(o, L"label", f.label);
        if (f.label.empty()) f.label = f.key;
        JsonUtil::ReadString(o, L"description", f.description);
        JsonUtil::ReadString(o, L"group", f.group);
        JsonUtil::ReadString(o, L"unit", f.unit);
        JsonUtil::ReadBool(o, L"common", f.common);  // round 10 phase 10B
        // Round 13 phase 13C: a quick setting (the widget's menu): bool / choice only, at most 8.
        JsonUtil::ReadBool(o, L"quick", f.quick);
        if (f.quick && f.type != SettingField::Type::Bool && f.type != SettingField::Type::Choice) {
            m.warnings.push_back(L"settings." + f.key + L": quick is for bool and choice fields only (ignored)");
            f.quick = false;
        }
        if (f.quick && std::count_if(m.settings.begin(), m.settings.end(), [](const SettingField& s) { return s.quick; }) >=
                           kMaxQuickFields) {
            m.warnings.push_back(L"settings." + f.key + L": more than 8 quick settings (ignored)");
            f.quick = false;
        }
        // Round 10 phase 10C: the tab the field is on (an unknown value: the default, with a warning;
        // Validate makes it an error).
        std::wstring section;
        JsonUtil::ReadString(o, L"section", section);
        if (section == L"content") {
            f.section = SettingField::Section::Content;
        } else if (section == L"appearance") {
            f.section = SettingField::Section::Appearance;
        } else {
            if (!section.empty()) m.warnings.push_back(L"settings." + f.key + L": unknown section \"" + section + L"\" (the default used)");
            f.section = DefaultSection(f);
        }
        if (const json::JsonObject vis = JsonUtil::ReadObject(o, L"visibleIf")) {
            for (const auto& kv : vis) f.visibleIf.emplace_back(std::wstring(kv.Key()), std::wstring(kv.Value().Stringify()));
        }
        if (f.type == SettingField::Type::Choice) {
            JsonUtil::ReadString(o, L"choiceFrom", f.choiceFrom);
            if (!f.choiceFrom.empty() && f.choiceFrom.find_first_of(L":\\") != std::wstring::npos) {
                m.warnings.push_back(L"settings." + f.key + L": choiceFrom \"" + f.choiceFrom + L"\" is not a package glob");
                f.choiceFrom.clear();
            }
        }
        if (f.type == SettingField::Type::File) JsonUtil::ReadString(o, L"filter", f.filter);  // round 17 phase 17A
        JsonUtil::ReadNumber(o, L"min", f.min);
        JsonUtil::ReadNumber(o, L"max", f.max);
        JsonUtil::ReadNumber(o, L"step", f.step);
        if (f.max < f.min) std::swap(f.min, f.max);
        if (const json::JsonArray choices = JsonUtil::ReadArray(o, L"choices")) {
            for (const auto& c : choices) {
                if (c.ValueType() == json::JsonValueType::String) {
                    f.choices.emplace_back(std::wstring(c.GetString()), std::wstring(c.GetString()));
                } else if (c.ValueType() == json::JsonValueType::Object) {
                    std::wstring value, label;
                    JsonUtil::ReadString(c.GetObject(), L"value", value);
                    JsonUtil::ReadString(c.GetObject(), L"label", label);
                    if (!value.empty()) f.choices.emplace_back(value, label.empty() ? value : label);
                }
            }
        }
        // The default, checked against the type (a wrong one falls back to the type's own).
        const json::IJsonValue d = JsonUtil::Find(o, L"default");
        const json::JsonValueType dt = d ? d.ValueType() : json::JsonValueType::Null;
        switch (f.type) {
            case SettingField::Type::Bool:
                f.defaultJson = dt == json::JsonValueType::Boolean && d.GetBoolean() ? L"true" : L"false";
                break;
            case SettingField::Type::Int:
            case SettingField::Type::Number: {
                double n = dt == json::JsonValueType::Number ? d.GetNumber() : (f.min > 0 ? f.min : 0.0);
                if (f.type == SettingField::Type::Int) n = std::round(n);
                f.defaultJson = NumberJson(Clamp(n, f.min, f.max));
                break;
            }
            case SettingField::Type::Choice: {
                std::wstring s = dt == json::JsonValueType::String ? std::wstring(d.GetString()) : std::wstring();
                if (!f.choices.empty() &&
                    std::none_of(f.choices.begin(), f.choices.end(), [&](const auto& c) { return c.first == s; })) {
                    s = f.choices.front().first;
                }
                f.defaultJson = JsonUtil::Quote(s);
                break;
            }
            case SettingField::Type::Color: {
                std::wstring s = dt == json::JsonValueType::String ? std::wstring(d.GetString()) : L"#FFFFFFFF";
                uint32_t argb = 0;
                if (!ParseArgbColor(s, argb)) s = L"#FFFFFFFF";
                f.defaultJson = JsonUtil::Quote(s);
                break;
            }
            case SettingField::Type::Place:
                f.defaultJson = L"null";  // its value is the item's latitude / longitude / location
                break;
            case SettingField::Type::Font:
            case SettingField::Type::String:
            case SettingField::Type::TimeZone:  // "" = the local time
            case SettingField::Type::File:      // round 17 phase 17A: "" = none chosen
            case SettingField::Type::Folder:
            default:
                f.defaultJson = JsonUtil::Quote(dt == json::JsonValueType::String
                                                    ? std::wstring(d.GetString())
                                                    : (f.type == SettingField::Type::Font ? L"Segoe UI" : L""));
                break;
        }
        m.settings.push_back(std::move(f));
    }
}

bool ParseManifest(const std::string& bytes, const std::wstring& id, Manifest& m, std::wstring& error) {
    json::JsonObject root{nullptr};
    try {
        if (!json::JsonObject::TryParse(Utf8ToWide(bytes), root) || !root) {
            error = L"widget.json is not a JSON object";
            return false;
        }
        m = Manifest{};
        m.id = id;
        JsonUtil::ReadString(root, L"name", m.name);
        if (m.name.empty()) m.name = id;
        JsonUtil::ReadString(root, L"version", m.version);
        JsonUtil::ReadString(root, L"author", m.author);
        JsonUtil::ReadString(root, L"description", m.description);
        std::wstring main;
        JsonUtil::ReadString(root, L"main", main);
        if (!main.empty()) {
            std::string resolved;
            if (ResolvePath("", WideToUtf8(main), resolved)) {
                m.main = Utf8ToWide(resolved);
            } else {
                m.warnings.push_back(L"main: \"" + main + L"\" is not a path inside the widget (main.js used)");
            }
        }
        const json::JsonObject size = JsonUtil::ReadObject(root, L"size");
        ReadSize(size, L"horizontal", m.horizontal);
        ReadSize(size, L"vertical", m.vertical);
        ReadStandalone(root, m);
        ReadPreview(root, m);
        double refresh = 0, limit = 50, memory = 32;
        JsonUtil::ReadNumber(root, L"refreshMs", refresh);
        JsonUtil::ReadNumber(root, L"timeLimitMs", limit);
        JsonUtil::ReadNumber(root, L"memoryLimitMB", memory);
        m.refreshMs = refresh <= 0 ? 0 : static_cast<int>(Clamp(refresh, 100.0, 86400000.0));
        m.timeLimitMs = static_cast<int>(Clamp(limit, 10.0, 1000.0));
        m.memoryLimitMb = static_cast<int>(Clamp(memory, 4.0, 256.0));
        JsonUtil::ReadBool(root, L"watch", m.watch);
        ReadPermissions(root, m);
        ReadSettingsSchema(root, m);
        return true;
    } catch (...) {
        error = L"widget.json could not be read";
        return false;
    }
}

}  // namespace

std::wstring Directory() { return Config::Directory() + L"\\widgets"; }

bool ValidId(std::wstring_view id) {
    if (id.empty() || id.size() > 64 || id.front() == L'.') return false;
    return std::all_of(id.begin(), id.end(), [](wchar_t c) {
        return (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'-' || c == L'_' || c == L'.';
    });
}

bool ResolvePath(std::string_view base, std::string_view spec, std::string& out) {
    out.clear();
    if (spec.empty() || spec.size() > 260) return false;
    if (spec.find(':') != std::string_view::npos || spec.find('\\') != std::string_view::npos) return false;
    if (spec.front() == '/') return false;
    if (std::any_of(spec.begin(), spec.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20; })) return false;
    std::vector<std::string> parts;
    const bool relative = spec.rfind("./", 0) == 0 || spec.rfind("../", 0) == 0;
    if (relative) {
        // The importing module's folder.
        size_t start = 0;
        while (start < base.size()) {
            const size_t slash = base.find('/', start);
            if (slash == std::string_view::npos) break;  // the last part is the file name
            parts.emplace_back(base.substr(start, slash - start));
            start = slash + 1;
        }
    }
    size_t start = 0;
    while (start <= spec.size()) {
        size_t slash = spec.find('/', start);
        if (slash == std::string_view::npos) slash = spec.size();
        const std::string_view part = spec.substr(start, slash - start);
        if (part.empty()) return false;  // "a//b", a trailing '/'
        if (part == ".") {
            if (!relative || start != 0) return false;
        } else if (part == "..") {
            if (!relative || parts.empty()) return false;  // would leave the package
            parts.pop_back();
        } else {
            if (part.find("..") != std::string_view::npos && part.find_first_not_of('.') == std::string_view::npos) {
                return false;
            }
            parts.emplace_back(part);
        }
        start = slash + 1;
    }
    if (parts.empty()) return false;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += '/';
        out += parts[i];
    }
    return true;
}

bool GlobMatch(std::string_view pattern, std::string_view path) {
    // Iterative matcher with one backtrack point per '*' (which never crosses a '/').
    size_t p = 0, s = 0, starP = std::string_view::npos, starS = 0;
    auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    while (s < path.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            starP = p++;
            starS = s;
        } else if (p < pattern.size() && (pattern[p] == '?' ? path[s] != '/' : lower(pattern[p]) == lower(path[s]))) {
            ++p;
            ++s;
        } else if (starP != std::string_view::npos && path[starS] != '/') {
            p = starP + 1;
            s = ++starS;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

bool Package::Read(const std::string& path, std::string& out, std::wstring& error) const {
    out.clear();
    std::string normalized;
    if (!ResolvePath("", path, normalized) || normalized != path) {
        error = L"\"" + Utf8ToWide(path) + L"\" is not a path inside the widget";
        return false;
    }
    if (source == Source::BuiltIn) {
        for (const BuiltInFile& f : kBuiltIns) {
            if (id == f.id && path == f.path) {
                if (ReadResource(f.resource, out)) return true;
                break;
            }
        }
        error = L"\"" + Utf8ToWide(path) + L"\" is not part of the built-in widget";
        return false;
    }
    std::wstring rel = Utf8ToWide(path);
    std::replace(rel.begin(), rel.end(), L'/', L'\\');
    const std::wstring full = folder + L"\\" + rel;
    HANDLE h = CreateFileW(full.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = L"\"" + Utf8ToWide(path) + L"\" was not found in the widget folder";
        return false;
    }
    // Junctions / links inside the folder may point elsewhere: the file actually opened must be
    // inside the (canonical) widget folder.
    const std::wstring real = FinalPath(h);
    bool ok = !real.empty() && StartsWithFolder(real, folder);
    if (!ok) {
        error = L"\"" + Utf8ToWide(path) + L"\" is outside the widget folder";
    }
    LARGE_INTEGER size{};
    if (ok && (!GetFileSizeEx(h, &size) || static_cast<unsigned long long>(size.QuadPart) > kMaxFileBytes)) {
        error = L"\"" + Utf8ToWide(path) + L"\" is larger than 8 MB";
        ok = false;
    }
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = out.empty() || (ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) && read == out.size());
        if (!ok) error = L"\"" + Utf8ToWide(path) + L"\" could not be read";
    }
    CloseHandle(h);
    if (ok && out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF &&
        static_cast<unsigned char>(out[1]) == 0xBB && static_cast<unsigned char>(out[2]) == 0xBF) {
        out.erase(0, 3);
    }
    if (!ok) out.clear();
    return ok;
}

std::vector<std::string> Package::Files() const {
    std::vector<std::string> files;
    if (source == Source::BuiltIn) {
        for (const BuiltInFile& f : kBuiltIns) {
            if (id == f.id) files.emplace_back(f.path);
        }
        return files;
    }
    // Breadth-first below the folder (no reparse points followed).
    std::vector<std::wstring> dirs{L""};
    while (!dirs.empty() && files.size() < kMaxListedFiles) {
        const std::wstring rel = dirs.back();
        dirs.pop_back();
        WIN32_FIND_DATAW fd{};
        HANDLE f = FindFirstFileExW((folder + L"\\" + rel + L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                    nullptr, 0);
        if (f == INVALID_HANDLE_VALUE) continue;
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                dirs.push_back(rel + name + L"\\");
            } else {
                std::wstring p = rel + name;
                std::replace(p.begin(), p.end(), L'\\', L'/');
                files.push_back(WideToUtf8(p));
            }
        } while (files.size() < kMaxListedFiles && FindNextFileW(f, &fd));
        FindClose(f);
    }
    std::sort(files.begin(), files.end());
    return files;
}

// --- validation (round 4 phase 14) -----------------------------------------------------------------

std::wstring Issue::Text() const {
    std::wstring s = (error ? L"error: " : L"warning: ") + field + L": " + message;
    if (!fix.empty()) s += L" (fix: " + fix + L")";
    return s;
}

bool Manifest::HasErrors() const {
    return std::any_of(issues.begin(), issues.end(), [](const Issue& i) { return i.error; });
}

void Manifest::DefaultStandaloneSize(int& w, int& h) const {
    float fw = standalone.defaultWidth, fh = standalone.defaultHeight;
    if (fw <= 0.0f || fh <= 0.0f) {
        const float hm = std::max(1.0f, horizontal.main), hc = std::max(1.0f, horizontal.cross);
        const float ratio = hm / hc;
        float dw = 200.0f, dh = 200.0f;
        if (ratio < 0.75f || ratio > 1.34f) {
            dw = 2.0f * hm;
            dh = 2.0f * hc;
        }
        if (fw <= 0.0f) fw = dw;
        if (fh <= 0.0f) fh = dh;
    }
    if (standalone.aspect == StandaloneHint::Aspect::Ratio && standalone.ratio > 0.0f) fh = fw / standalone.ratio;
    fw = std::max(fw, standalone.minWidth);
    fh = std::max(fh, standalone.minHeight);
    w = std::clamp(static_cast<int>(std::lround(fw)), 48, 4000);
    h = std::clamp(static_cast<int>(std::lround(fh)), 48, 4000);
}

namespace {

const wchar_t* TypeName(json::JsonValueType t) {
    switch (t) {
        case json::JsonValueType::Null: return L"null";
        case json::JsonValueType::Boolean: return L"a boolean";
        case json::JsonValueType::Number: return L"a number";
        case json::JsonValueType::String: return L"a string";
        case json::JsonValueType::Array: return L"an array";
        case json::JsonValueType::Object:
        default: return L"an object";
    }
}

class Checker {
public:
    explicit Checker(std::vector<Issue>& out) : out_(out) {}
    void Error(std::wstring field, std::wstring message, std::wstring fix) {
        out_.push_back(Issue{true, std::move(field), std::move(message), std::move(fix)});
    }
    void Warn(std::wstring field, std::wstring message, std::wstring fix = {}) {
        out_.push_back(Issue{false, std::move(field), std::move(message), std::move(fix)});
    }
    // `key` of `o` must be of type `t` when present. False if present with another type.
    bool Type(const json::JsonObject& o, const wchar_t* key, json::JsonValueType t, const std::wstring& field,
              const wchar_t* fix) {
        const json::IJsonValue v = JsonUtil::Find(o, key);
        if (!v || v.ValueType() == t) return true;
        Error(field, std::wstring(L"is ") + TypeName(v.ValueType()) + L", not " + TypeName(t), fix);
        return false;
    }
    void Range(const json::JsonObject& o, const wchar_t* key, double lo, double hi, const std::wstring& field,
               const wchar_t* what) {
        const json::IJsonValue v = JsonUtil::Find(o, key);
        if (!v) return;
        if (v.ValueType() != json::JsonValueType::Number) {
            Error(field, std::wstring(L"is ") + TypeName(v.ValueType()) + L", not a number", what);
            return;
        }
        const double n = v.GetNumber();
        if (n < lo || n > hi) Warn(field, JsonUtil::Num(n) + L" is outside " + JsonUtil::Num(lo) + L" - " + JsonUtil::Num(hi) + L" (clamped)", what);
    }

private:
    std::vector<Issue>& out_;
};

constexpr const wchar_t* kTopKeys[] = {L"name",         L"version",     L"author",      L"description", L"main",
                                       L"size",         L"refreshMs",   L"timeLimitMs", L"memoryLimitMB",
                                       L"permissions",  L"watch",       L"settings",    L"api",
                                       L"standalone",   L"preview"};
constexpr const wchar_t* kFieldKeys[] = {L"key",     L"type",       L"label", L"description", L"default", L"min",
                                         L"max",     L"step",       L"choices", L"choiceFrom", L"group", L"unit",
                                         L"visibleIf", L"common",  // common: round 10 phase 10B
                                         L"section",               // round 10 phase 10C
                                         L"quick",                 // round 13 phase 13C
                                         L"filter"};               // round 17 phase 17A (file fields)
constexpr wchar_t kPermissionFix[] =
    L"use \"shell\", \"audio\", \"media\", \"notifications\", \"calendar\", \"weather\", \"perf\" or { \"http\": [\"host\"] }";

bool Known(const wchar_t* const* begin, const wchar_t* const* end, const std::wstring& key) {
    return std::any_of(begin, end, [&](const wchar_t* k) { return key == k; });
}

void CheckSize(Checker& c, const json::JsonObject& root) {
    const json::IJsonValue size = JsonUtil::Find(root, L"size");
    if (!size) return;
    if (size.ValueType() != json::JsonValueType::Object) {
        c.Error(L"size", std::wstring(L"is ") + TypeName(size.ValueType()) + L", not an object",
                L"{ \"horizontal\": { \"main\": 64, \"cross\": 40 }, \"vertical\": { \"main\": 48, \"cross\": 48 } }");
        return;
    }
    const json::JsonObject o = size.GetObject();
    for (const auto& kv : o) {
        const std::wstring axis(kv.Key());
        if (axis != L"horizontal" && axis != L"vertical") {
            c.Warn(L"size." + axis, L"is not a dock axis (ignored)", L"use \"horizontal\" and \"vertical\"");
            continue;
        }
        if (kv.Value().ValueType() != json::JsonValueType::Object) {
            c.Error(L"size." + axis, std::wstring(L"is ") + TypeName(kv.Value().ValueType()) + L", not an object",
                    L"{ \"main\": 64, \"cross\": 40 } (DIPs along / across the dock)");
            continue;
        }
        const json::JsonObject a = kv.Value().GetObject();
        for (const wchar_t* k : {L"main", L"cross"}) {
            const json::IJsonValue v = JsonUtil::Find(a, k);
            const std::wstring field = L"size." + axis + L"." + k;
            if (!v) {
                c.Error(field, L"is missing", L"a number of DIPs (main along the dock, cross across it)");
            } else if (v.ValueType() != json::JsonValueType::Number) {
                c.Error(field, std::wstring(L"is ") + TypeName(v.ValueType()) + L", not a number", L"a number of DIPs");
            } else {
                const double n = v.GetNumber(), hi = std::wstring(k) == L"main" ? 1024.0 : 512.0;
                if (!(n >= 4.0 && n <= hi)) {
                    c.Error(field, JsonUtil::Num(n) + L" is outside 4 - " + JsonUtil::Num(hi), L"a size from 4 to " + JsonUtil::Num(hi) + L" DIPs");
                }
            }
        }
    }
}

// Round 6 phase 6B: the `standalone` object.
void CheckStandalone(Checker& c, const json::JsonObject& root) {
    const json::IJsonValue v = JsonUtil::Find(root, L"standalone");
    if (!v) return;
    constexpr wchar_t kFix[] = L"{ \"minWidth\": 96, \"minHeight\": 96, \"aspect\": \"keep\", \"defaultWidth\": 200, \"defaultHeight\": 200 }";
    if (v.ValueType() != json::JsonValueType::Object) {
        c.Error(L"standalone", std::wstring(L"is ") + TypeName(v.ValueType()) + L", not an object", kFix);
        return;
    }
    const json::JsonObject o = v.GetObject();
    for (const auto& kv : o) {
        const std::wstring k(kv.Key());
        if (k != L"minWidth" && k != L"minHeight" && k != L"aspect" && k != L"defaultWidth" && k != L"defaultHeight" &&
            k != L"supported") {
            c.Warn(L"standalone." + k, L"unknown key (ignored)", L"minWidth, minHeight, aspect, defaultWidth, defaultHeight, supported");
        }
    }
    // Round 16 phase 16E: `"supported": false` (the widget needs a dock).
    c.Type(o, L"supported", json::JsonValueType::Boolean, L"standalone.supported", L"\"supported\": false (the widget needs a dock)");
    for (const wchar_t* k : {L"minWidth", L"minHeight", L"defaultWidth", L"defaultHeight"}) {
        c.Range(o, k, 16, 4000, L"standalone." + std::wstring(k), L"DIPs, 16 - 4000");
    }
    if (const json::IJsonValue a = JsonUtil::Find(o, L"aspect")) {
        const bool number = a.ValueType() == json::JsonValueType::Number && std::isfinite(a.GetNumber()) && a.GetNumber() > 0.0;
        const bool word = a.ValueType() == json::JsonValueType::String &&
                          (Lower(std::wstring(a.GetString())) == L"free" || Lower(std::wstring(a.GetString())) == L"keep");
        if (!number && !word) {
            c.Error(L"standalone.aspect", L"is not \"free\", \"keep\" or a number > 0",
                    L"\"free\" (any shape), \"keep\" (the shape it was made with) or width / height, e.g. 1 or 1.5");
        }
    }
    double minW = 48, minH = 48, defW = 0, defH = 0;
    JsonUtil::ReadNumber(o, L"minWidth", minW);
    JsonUtil::ReadNumber(o, L"minHeight", minH);
    JsonUtil::ReadNumber(o, L"defaultWidth", defW);
    JsonUtil::ReadNumber(o, L"defaultHeight", defH);
    if ((defW > 0 && defW < minW) || (defH > 0 && defH < minH)) {
        c.Warn(L"standalone", L"the default size is smaller than the minimum (the minimum is used)", L"defaultWidth >= minWidth, defaultHeight >= minHeight");
    }
}

// Round 6 phase 6E: the `preview` object (the widget drawer's live preview).
void CheckPreview(Checker& c, const json::JsonObject& root) {
    const json::IJsonValue v = JsonUtil::Find(root, L"preview");
    if (!v) return;
    constexpr wchar_t kFix[] = L"{ \"settings\": { \"face\": \"classic\" }, \"width\": 64, \"height\": 48 }";
    if (v.ValueType() != json::JsonValueType::Object) {
        c.Error(L"preview", std::wstring(L"is ") + TypeName(v.ValueType()) + L", not an object", kFix);
        return;
    }
    const json::JsonObject o = v.GetObject();
    for (const auto& kv : o) {
        const std::wstring k(kv.Key());
        if (k != L"settings" && k != L"width" && k != L"height") c.Warn(L"preview." + k, L"unknown key (ignored)", L"settings, width, height");
    }
    c.Type(o, L"settings", json::JsonValueType::Object, L"preview.settings",
           L"an object of setting values: { \"label\": \"Sample\" } (merged over the defaults for the preview only)");
    if (const json::IJsonValue s = JsonUtil::Find(o, L"settings"); s && s.ValueType() == json::JsonValueType::Object) {
        std::vector<std::wstring> keys;
        if (const json::JsonArray schema = JsonUtil::ReadArray(root, L"settings")) {
            for (const auto& f : schema) {
                std::wstring k;
                if (f.ValueType() == json::JsonValueType::Object) JsonUtil::ReadString(f.GetObject(), L"key", k);
                if (!k.empty()) keys.push_back(k);
            }
        }
        for (const auto& kv : s.GetObject()) {
            const std::wstring k(kv.Key());
            if (std::find(keys.begin(), keys.end(), k) == keys.end()) {
                c.Warn(L"preview.settings." + k, L"names no setting of the schema (passed through as is)");
            }
        }
    }
    c.Range(o, L"width", 8, 1024, L"preview.width", L"DIPs, 8 - 1024 (leave it out for the measured box)");
    c.Range(o, L"height", 8, 512, L"preview.height", L"DIPs, 8 - 512 (leave it out for the measured box)");
}

void CheckPermissions(Checker& c, const json::JsonObject& root) {
    const json::IJsonValue perms = JsonUtil::Find(root, L"permissions");
    if (!perms) return;
    if (perms.ValueType() != json::JsonValueType::Array) {
        c.Error(L"permissions", std::wstring(L"is ") + TypeName(perms.ValueType()) + L", not an array", kPermissionFix);
        return;
    }
    uint32_t i = 0;
    auto hostOk = [](const std::wstring& h) { return !h.empty() && h.find_first_of(L"/:\\ ") == std::wstring::npos; };
    for (const auto& v : perms.GetArray()) {
        const std::wstring field = L"permissions[" + std::to_wstring(i++) + L"]";
        if (v.ValueType() == json::JsonValueType::String) {
            const std::wstring s = Lower(std::wstring(v.GetString()));
            if (s == L"shell" || s == L"audio" || s == L"media" || s == L"notifications" || s == L"calendar" ||
                s == L"weather" || s == L"perf") {
                continue;
            }
            if (s == L"http") {
                c.Error(field, L"\"http\" without hosts allows no request", L"name the hosts: { \"http\": [\"api.example.com\"] }");
            } else if (s.rfind(L"http:", 0) == 0) {
                if (!hostOk(s.substr(5))) c.Error(field, L"\"" + s + L"\" is not a host name", L"\"http:api.example.com\" (no scheme, path or port)");
            } else {
                c.Error(field, L"unknown permission \"" + s + L"\"", kPermissionFix);
            }
        } else if (v.ValueType() == json::JsonValueType::Object) {
            const json::JsonObject o = v.GetObject();
            for (const auto& kv : o) {
                const std::wstring k(kv.Key());
                if (k == L"shell") continue;
                if (k != L"http") {
                    c.Error(field + L"." + k, L"unknown permission \"" + k + L"\"", kPermissionFix);
                    continue;
                }
                if (kv.Value().ValueType() != json::JsonValueType::Array) {
                    c.Error(field + L".http", L"is not an array of host names", L"{ \"http\": [\"api.example.com\", \"*.example.org\"] }");
                    continue;
                }
                uint32_t j = 0;
                for (const auto& h : kv.Value().GetArray()) {
                    const std::wstring hf = field + L".http[" + std::to_wstring(j++) + L"]";
                    if (h.ValueType() != json::JsonValueType::String || !hostOk(std::wstring(h.GetString()))) {
                        c.Error(hf, L"is not a host name", L"\"api.example.com\" or \"*.example.org\" (no scheme, path or port)");
                    }
                }
            }
        } else {
            c.Error(field, std::wstring(L"is ") + TypeName(v.ValueType()), kPermissionFix);
        }
    }
}

constexpr wchar_t kTypeNames[] =
    L"\"bool\", \"int\", \"number\", \"string\", \"choice\", \"colour\", \"font\", \"place\", \"timezone\", \"file\" or \"folder\"";

// A Windows time zone key name of this machine (EnumDynamicTimeZoneInformation).
bool KnownTimeZone(const std::wstring& key) {
    DYNAMIC_TIME_ZONE_INFORMATION z{};
    for (DWORD n = 0; EnumDynamicTimeZoneInformation(n, &z) == ERROR_SUCCESS && n < 1000; ++n) {
        if (_wcsicmp(z.TimeZoneKeyName, key.c_str()) == 0) return true;
    }
    return false;
}

void CheckSettings(Checker& c, const json::JsonObject& root, const Package& pkg) {
    const json::IJsonValue settings = JsonUtil::Find(root, L"settings");
    if (!settings) return;
    if (settings.ValueType() != json::JsonValueType::Array) {
        c.Error(L"settings", std::wstring(L"is ") + TypeName(settings.ValueType()) + L", not an array",
                L"[ { \"key\": \"label\", \"type\": \"string\", \"label\": \"Label\", \"default\": \"Hello\" } ]");
        return;
    }
    std::vector<std::wstring> keys;
    for (const auto& v : settings.GetArray()) {
        if (v.ValueType() == json::JsonValueType::Object) {
            std::wstring k;
            JsonUtil::ReadString(v.GetObject(), L"key", k);
            keys.push_back(k);
        }
    }
    std::vector<std::string> files;
    bool listed = false;
    uint32_t i = 0;
    int places = 0;
    int quicks = 0;  // round 13 phase 13C
    std::vector<std::wstring> seen;
    for (const auto& v : settings.GetArray()) {
        std::wstring field = L"settings[" + std::to_wstring(i++) + L"]";
        if (v.ValueType() != json::JsonValueType::Object) {
            c.Error(field, std::wstring(L"is ") + TypeName(v.ValueType()) + L", not an object", L"{ \"key\": ..., \"type\": ... }");
            continue;
        }
        const json::JsonObject o = v.GetObject();
        std::wstring key;
        JsonUtil::ReadString(o, L"key", key);
        if (key.empty() || key.size() > 64) {
            c.Error(field + L".key", key.empty() ? L"is missing" : L"is longer than 64 characters", L"a short name: \"key\": \"label\"");
            continue;
        }
        field += L" (" + key + L")";
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
            c.Error(field + L".key", L"\"" + key + L"\" is used twice", L"give every setting its own key");
            continue;
        }
        seen.push_back(key);
        for (const auto& kv : o) {
            const std::wstring k(kv.Key());
            if (!Known(std::begin(kFieldKeys), std::end(kFieldKeys), k)) c.Warn(field + L"." + k, L"unknown key (ignored)");
        }
        std::wstring type = L"string";
        if (!c.Type(o, L"type", json::JsonValueType::String, field + L".type", kTypeNames)) continue;
        JsonUtil::ReadString(o, L"type", type);
        bool ok = true;
        const SettingField::Type t = TypeOf(type, ok);
        if (!ok) {
            c.Error(field + L".type", L"unknown type \"" + type + L"\"", kTypeNames);
            continue;
        }
        for (const wchar_t* k : {L"label", L"description", L"group", L"unit", L"choiceFrom", L"filter"}) {
            c.Type(o, k, json::JsonValueType::String, field + L"." + k, L"a string");
        }
        // Round 17 phase 17A: `filter` is the file picker's patterns; only a file field has one.
        if (o.HasKey(L"filter") && t != SettingField::Type::File) c.Warn(field + L".filter", L"is not used by a " + type + L" field (ignored)");
        c.Type(o, L"common", json::JsonValueType::Boolean, field + L".common", L"\"common\": true");  // round 10 phase 10B
        // Round 13 phase 13C: a quick setting (the widget's menu) is a bool or a choice, 8 at most.
        if (c.Type(o, L"quick", json::JsonValueType::Boolean, field + L".quick", L"\"quick\": true")) {
            bool quick = false;
            JsonUtil::ReadBool(o, L"quick", quick);
            if (quick && t != SettingField::Type::Bool && t != SettingField::Type::Choice) {
                c.Warn(field + L".quick", L"only bool and choice fields can be quick settings (ignored)", L"remove \"quick\"");
            } else if (quick && ++quicks > kMaxQuickFields) {
                c.Warn(field + L".quick", L"more than 8 quick settings: this one is not in the menu", L"mark 8 at most");
            }
        }
        // Round 10 phase 10C: the field's tab; an unknown name is an error like an unknown type.
        if (c.Type(o, L"section", json::JsonValueType::String, field + L".section", L"\"content\" or \"appearance\"")) {
            std::wstring section;
            JsonUtil::ReadString(o, L"section", section);
            if (o.HasKey(L"section") && section != L"content" && section != L"appearance") {
                c.Error(field + L".section", L"unknown section \"" + section + L"\"", L"\"content\" or \"appearance\"");
            }
        }
        const json::IJsonValue d = JsonUtil::Find(o, L"default");
        const json::JsonValueType dt = d ? d.ValueType() : json::JsonValueType::Null;
        if (t == SettingField::Type::Int || t == SettingField::Type::Number) {
            double lo = -1e9, hi = 1e9, step = 1.0;
            for (const wchar_t* k : {L"min", L"max", L"step"}) c.Type(o, k, json::JsonValueType::Number, field + L"." + k, L"a number");
            JsonUtil::ReadNumber(o, L"min", lo);
            JsonUtil::ReadNumber(o, L"max", hi);
            JsonUtil::ReadNumber(o, L"step", step);
            if (lo > hi) c.Error(field, L"min " + JsonUtil::Num(lo) + L" is above max " + JsonUtil::Num(hi), L"swap them");
            if (!(step > 0)) c.Error(field + L".step", L"must be above 0", L"\"step\": 1");
            if (d && dt != json::JsonValueType::Number) c.Warn(field + L".default", std::wstring(L"is ") + TypeName(dt) + L", not a number (min used)", L"a number");
            if (dt == json::JsonValueType::Number && (d.GetNumber() < lo || d.GetNumber() > hi)) {
                c.Warn(field + L".default", JsonUtil::Num(d.GetNumber()) + L" is outside min - max (clamped)");
            }
        } else if (t == SettingField::Type::Bool) {
            if (d && dt != json::JsonValueType::Boolean) c.Warn(field + L".default", std::wstring(L"is ") + TypeName(dt) + L", not a boolean (false used)", L"true or false");
        } else if (t == SettingField::Type::Color) {
            uint32_t argb = 0;
            if (d && (dt != json::JsonValueType::String || !ParseArgbColor(std::wstring(d.GetString()), argb))) {
                c.Warn(field + L".default", L"is not a colour (\"#FFFFFFFF\" used)", L"\"#AARRGGBB\" or \"#RRGGBB\"");
            }
        } else if (t == SettingField::Type::Place || t == SettingField::Type::TimeZone) {
            // Round 5: no extra keys; a place has no default (its value is the item's latitude /
            // longitude / location), a time zone's is a Windows zone key name ("" = local).
            for (const wchar_t* k : {L"min", L"max", L"step", L"choices", L"choiceFrom", L"unit"}) {
                if (o.HasKey(k)) c.Warn(field + L"." + k, L"is not used by a " + type + L" field (ignored)");
            }
            if (t == SettingField::Type::Place) {
                if (d) c.Warn(field + L".default", L"a place has no default (the item's latitude / longitude are its value)", L"remove it");
                if (++places > 1) c.Warn(field, L"only the first place field is shown on the settings page", L"one place per widget");
            } else if (d && dt != json::JsonValueType::String) {
                c.Warn(field + L".default", std::wstring(L"is ") + TypeName(dt) + L", not a time zone name",
                       L"\"\" (local) or \"Tokyo Standard Time\"");
            } else if (dt == json::JsonValueType::String && !d.GetString().empty() && !KnownTimeZone(std::wstring(d.GetString()))) {
                c.Warn(field + L".default", L"\"" + std::wstring(d.GetString()) + L"\" is not a time zone of this Windows (local used)",
                       L"a key name such as \"Tokyo Standard Time\"");
            }
        } else if (d && dt != json::JsonValueType::String) {
            c.Warn(field + L".default", std::wstring(L"is ") + TypeName(dt) + L", not a string", L"a string");
        }
        if (t == SettingField::Type::Choice) {
            std::vector<std::wstring> values;
            const json::IJsonValue ch = JsonUtil::Find(o, L"choices");
            if (ch && ch.ValueType() != json::JsonValueType::Array) {
                c.Error(field + L".choices", std::wstring(L"is ") + TypeName(ch.ValueType()) + L", not an array", L"[\"a\", \"b\"] or [{ \"value\": \"a\", \"label\": \"A\" }]");
            } else if (ch) {
                uint32_t j = 0;
                for (const auto& e : ch.GetArray()) {
                    const std::wstring cf = field + L".choices[" + std::to_wstring(j++) + L"]";
                    if (e.ValueType() == json::JsonValueType::String) {
                        values.emplace_back(e.GetString());
                    } else if (e.ValueType() == json::JsonValueType::Object) {
                        std::wstring value;
                        JsonUtil::ReadString(e.GetObject(), L"value", value);
                        if (value.empty()) c.Error(cf + L".value", L"is missing", L"{ \"value\": \"a\", \"label\": \"A\" }");
                        values.push_back(value);
                    } else {
                        c.Error(cf, std::wstring(L"is ") + TypeName(e.ValueType()), L"a string or { \"value\", \"label\" }");
                    }
                }
            }
            std::wstring from;
            JsonUtil::ReadString(o, L"choiceFrom", from);
            if (from == L"perf") {
                // Round 6 phase 6D: the host lists perf.metrics() (the settings page); any key is kept.
                const json::IJsonValue def = JsonUtil::Find(o, L"default");
                if (!def || def.ValueType() != json::JsonValueType::String || def.GetString().empty()) {
                    c.Warn(field + L".default", L"a perf metric choice has no default key", L"\"default\": \"cpu.total\"");
                }
            } else if (!from.empty()) {
                if (from.find_first_of(L":\\") != std::wstring::npos || from.front() == L'/') {
                    c.Error(field + L".choiceFrom", L"\"" + from + L"\" is not a glob of package files", L"a path inside the widget such as \"faces/*.json\"");
                } else {
                    if (!listed) {
                        files = pkg.Files();
                        listed = true;
                    }
                    const std::string pattern = WideToUtf8(from);
                    size_t matches = 0;
                    for (const std::string& file : files) {
                        if (!GlobMatch(pattern, file)) continue;
                        ++matches;
                        std::string text;
                        std::wstring readError;
                        json::JsonObject fo{nullptr};
                        if (file.size() > 5 && file.compare(file.size() - 5, 5, ".json") == 0 && pkg.Read(file, text, readError) &&
                            !(json::JsonObject::TryParse(Utf8ToWide(text), fo) && fo)) {
                            c.Warn(field + L".choiceFrom", Utf8ToWide(file) + L" is not a JSON object (its label is its file name)");
                        }
                    }
                    if (!matches) {
                        c.Error(field + L".choiceFrom", L"\"" + from + L"\" matches no file of the widget",
                                L"add files matching it (e.g. faces/classic.json) or fix the glob");
                    }
                }
            } else if (values.empty()) {
                c.Error(field, L"a choice without \"choices\" or \"choiceFrom\"", L"\"choices\": [\"a\", \"b\"]");
            }
            if (dt == json::JsonValueType::String && !values.empty() && from.empty() &&
                std::find(values.begin(), values.end(), std::wstring(d.GetString())) == values.end()) {
                c.Warn(field + L".default", L"\"" + std::wstring(d.GetString()) + L"\" is not one of the choices (the first is used)");
            }
        }
        if (const json::IJsonValue vis = JsonUtil::Find(o, L"visibleIf")) {
            if (vis.ValueType() != json::JsonValueType::Object || vis.GetObject().Size() == 0) {
                c.Error(field + L".visibleIf", L"is not an object of { \"otherKey\": value }", L"\"visibleIf\": { \"mode\": \"custom\" }");
            } else {
                for (const auto& kv : vis.GetObject()) {
                    const std::wstring other(kv.Key());
                    if (other == key || std::find(keys.begin(), keys.end(), other) == keys.end()) {
                        c.Error(field + L".visibleIf." + other, L"names no other setting", L"the key of another entry of \"settings\"");
                    }
                }
            }
        }
    }
}

}  // namespace

std::vector<Issue> Validate(const std::string& bytes, const Package& pkg) {
    std::vector<Issue> issues;
    Checker c(issues);
    json::JsonObject root{nullptr};
    try {
        if (!json::JsonObject::TryParse(Utf8ToWide(bytes), root) || !root) {
            c.Error(L"widget.json", L"is not a JSON object (a syntax error, or not an object)",
                    L"{ \"name\": \"My widget\", \"main\": \"main.js\" } (check commas and quotes)");
            return issues;
        }
        for (const auto& kv : root) {
            const std::wstring k(kv.Key());
            if (!Known(std::begin(kTopKeys), std::end(kTopKeys), k)) c.Warn(k, L"unknown key (ignored)");
        }
        for (const wchar_t* k : {L"name", L"version", L"author", L"description"}) {
            c.Type(root, k, json::JsonValueType::String, k, L"a string");
        }
        c.Type(root, L"watch", json::JsonValueType::Boolean, L"watch", L"true or false");
        // main: a package path to a module that exists.
        std::wstring main = L"main.js";
        if (c.Type(root, L"main", json::JsonValueType::String, L"main", L"\"main\": \"main.js\"")) {
            JsonUtil::ReadString(root, L"main", main);
            std::string resolved;
            if (!ResolvePath("", WideToUtf8(main), resolved)) {
                c.Error(L"main", L"\"" + main + L"\" is not a path inside the widget folder", L"a relative path such as \"main.js\" or \"src/main.js\"");
            } else {
                const std::vector<std::string> files = pkg.Files();
                if (std::find(files.begin(), files.end(), resolved) == files.end()) {
                    c.Error(L"main", L"\"" + Utf8ToWide(resolved) + L"\" was not found in the widget",
                            root.HasKey(L"main") ? L"add the module or set \"main\" to its path"
                                                            : L"add main.js, or name the module: \"main\": \"app.js\"");
                }
            }
        }
        CheckSize(c, root);
        CheckStandalone(c, root);
        CheckPreview(c, root);
        c.Range(root, L"refreshMs", 0, 86400000, L"refreshMs", L"0 (no tick) or 100 - 86400000 ms");
        c.Range(root, L"timeLimitMs", 10, 1000, L"timeLimitMs", L"10 - 1000 ms");
        c.Range(root, L"memoryLimitMB", 4, 256, L"memoryLimitMB", L"4 - 256 MB");
        CheckPermissions(c, root);
        CheckSettings(c, root, pkg);
    } catch (...) {
        c.Error(L"widget.json", L"could not be read", L"save it as UTF-8 JSON");
    }
    return issues;
}

bool ValidateFolder(const std::wstring& folder, std::vector<Issue>& issues, std::wstring& name) {
    issues.clear();
    std::wstring src = folder;
    while (src.size() > 3 && (src.back() == L'\\' || src.back() == L'/')) src.pop_back();
    Package pkg;
    pkg.source = Package::Source::Folder;
    pkg.folder = CanonicalFolder(src);
    if (pkg.folder.empty()) {
        issues.push_back(Issue{true, L"folder", src + L" cannot be opened", L"the path of a widget folder"});
        return false;
    }
    const size_t cut = pkg.folder.find_last_of(L'\\');
    pkg.id = Lower(cut == std::wstring::npos ? pkg.folder : pkg.folder.substr(cut + 1));
    std::string bytes;
    std::wstring error;
    if (!pkg.Read("widget.json", bytes, error)) {
        issues.push_back(Issue{true, L"widget.json", L"was not found in " + pkg.folder, L"a widget folder holds a widget.json"});
        return false;
    }
    if (!ValidId(pkg.id)) {
        issues.push_back(Issue{true, L"folder name", L"\"" + pkg.id + L"\" is not a valid widget id",
                               L"rename the folder: 1 - 64 of a-z 0-9 - _ . (it becomes the id when installed)"});
    }
    std::vector<Issue> found = Validate(bytes, pkg);
    issues.insert(issues.end(), found.begin(), found.end());
    if (ParseManifest(bytes, pkg.id, pkg.manifest, error)) name = pkg.manifest.name;
    return true;
}

// choiceFrom: the choices from the package's files (below).
void ExpandChoices(Package& pkg);

std::shared_ptr<const Package> Load(const std::wstring& id, std::wstring& error) {
    error.clear();
    if (!ValidId(id)) {
        error = L"\"" + id + L"\" is not a valid widget id";
        return nullptr;
    }
    auto pkg = std::make_shared<Package>();
    pkg->id = id;
    const std::wstring folder = Directory() + L"\\" + id;
    const DWORD attrs = GetFileAttributesW((folder + L"\\widget.json").c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        pkg->source = Package::Source::Folder;
        pkg->folder = CanonicalFolder(folder);
        if (pkg->folder.empty()) {
            error = L"the widget folder " + folder + L" cannot be opened";
            return nullptr;
        }
    } else if (IsBuiltIn(id)) {
        pkg->source = Package::Source::BuiltIn;
    } else {
        error = L"widget \"" + id + L"\" is not installed (no " + folder + L"\\widget.json)";
        return nullptr;
    }
    std::string bytes;
    std::wstring readError;
    if (!pkg->Read("widget.json", bytes, readError)) {
        error = readError;
        return nullptr;
    }
    if (!ParseManifest(bytes, id, pkg->manifest, error)) return nullptr;
    ExpandChoices(*pkg);
    pkg->manifest.issues = Validate(bytes, *pkg);
    return pkg;
}

void ExpandChoices(Package& pkg) {
    std::vector<std::string> files;
    bool listed = false;
    for (SettingField& f : pkg.manifest.settings) {
        if (f.type != SettingField::Type::Choice || f.choiceFrom.empty() || f.ChoiceFromPerf()) continue;
        if (!listed) {
            files = pkg.Files();
            listed = true;
        }
        const std::string pattern = WideToUtf8(f.choiceFrom);
        for (const std::string& file : files) {
            if (!GlobMatch(pattern, file)) continue;
            std::string stem = file.substr(file.find_last_of('/') == std::string::npos ? 0 : file.find_last_of('/') + 1);
            if (const size_t dot = stem.find_last_of('.'); dot != std::string::npos && dot > 0) stem.resize(dot);
            const std::wstring value = Utf8ToWide(stem);
            if (value.empty() ||
                std::any_of(f.choices.begin(), f.choices.end(), [&](const auto& c) { return c.first == value; })) {
                continue;
            }
            std::wstring label = value;
            std::string text;
            std::wstring readError;
            if (pkg.Read(file, text, readError)) {
                try {
                    json::JsonObject o{nullptr};
                    if (json::JsonObject::TryParse(Utf8ToWide(text), o) && o) {
                        std::wstring name;
                        JsonUtil::ReadString(o, L"name", name);
                        if (!name.empty()) label = name;
                    } else {
                        pkg.manifest.warnings.push_back(Utf8ToWide(file) + L" is not a JSON object");
                    }
                } catch (...) {
                }
            }
            f.choices.emplace_back(value, label);
        }
        // The default must be one of them (a face file that went away: the first one).
        std::wstring def;
        try {
            const json::IJsonValue d = json::JsonValue::Parse(f.defaultJson);
            if (d.ValueType() == json::JsonValueType::String) def = d.GetString();
        } catch (...) {
        }
        if (!f.choices.empty() &&
            std::none_of(f.choices.begin(), f.choices.end(), [&](const auto& c) { return c.first == def; })) {
            f.defaultJson = JsonUtil::Quote(f.choices.front().first);
        }
    }
}

std::vector<CatalogEntry> Catalog() {
    std::vector<CatalogEntry> out;
    for (const BuiltInFile& f : kBuiltIns) {
        if (std::any_of(out.begin(), out.end(), [&](const CatalogEntry& e) { return e.id == f.id; })) continue;
        CatalogEntry e;
        e.id = f.id;
        e.source = Package::Source::BuiltIn;
        e.builtIn = true;
        out.push_back(std::move(e));
    }
    WIN32_FIND_DATAW fd{};
    const std::wstring dir = Directory();
    HANDLE find = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            const std::wstring id = fd.cFileName;
            if (!ValidId(id)) continue;
            const DWORD a = GetFileAttributesW((dir + L"\\" + id + L"\\widget.json").c_str());
            if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) continue;
            auto it = std::find_if(out.begin(), out.end(), [&](const CatalogEntry& e) { return e.id == id; });
            if (it == out.end()) {
                CatalogEntry e;
                e.id = id;
                out.push_back(std::move(e));
                it = out.end() - 1;
            }
            it->source = Package::Source::Folder;
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    for (CatalogEntry& e : out) e.name = DisplayName(e.id);
    std::sort(out.begin(), out.end(), [](const CatalogEntry& a, const CatalogEntry& b) {
        const int c = CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE, a.name.c_str(), -1, b.name.c_str(), -1,
                                      nullptr, nullptr, 0);
        return c == CSTR_LESS_THAN || (c == CSTR_EQUAL && a.id < b.id);
    });
    return out;
}

namespace {

bool CreateDirs(const std::wstring& path) {
    // Creates every missing level ("C:\a\b\c").
    for (size_t p = path.find(L'\\', 3); ; p = path.find(L'\\', p + 1)) {
        const std::wstring part = p == std::wstring::npos ? path : path.substr(0, p);
        if (!CreateDirectoryW(part.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
        if (p == std::wstring::npos) return true;
    }
}

}  // namespace

bool ExportBuiltIn(const std::wstring& id, std::wstring& folder, std::wstring& error) {
    if (!IsBuiltIn(id)) {
        error = L"\"" + id + L"\" is not a built-in widget";
        return false;
    }
    folder = Directory() + L"\\" + id;
    if (GetFileAttributesW(folder.c_str()) != INVALID_FILE_ATTRIBUTES) {
        error = folder + L" exists already";
        return false;
    }
    if (!CreateDirs(folder)) {
        error = L"cannot create " + folder;
        return false;
    }
    for (const BuiltInFile& f : kBuiltIns) {
        if (id != f.id) continue;
        std::string bytes;
        if (!ReadResource(f.resource, bytes)) {
            error = L"cannot read the built-in " + Utf8ToWide(f.path);
            return false;
        }
        std::wstring rel = Utf8ToWide(f.path);
        std::replace(rel.begin(), rel.end(), L'/', L'\\');
        const std::wstring target = folder + L"\\" + rel;
        if (const size_t cut = target.find_last_of(L'\\'); cut != std::wstring::npos) CreateDirs(target.substr(0, cut));
        if (!JsonUtil::WriteFileAtomic(target, bytes)) {
            error = L"cannot write " + target;
            return false;
        }
    }
    // A stub README: where the API is documented and how the copy behaves.
    std::wstring name = id;
    if (const auto pkg = Load(id, error)) name = pkg->manifest.name;
    error.clear();
    const std::string readme =
        "# " + WideToUtf8(name) + " (exported from WinDock)\n\n"
        "This folder is a copy of WinDock's built-in `" + WideToUtf8(id) + "` widget. While it exists it replaces the\n"
        "built-in: edit `widget.json` and `main.js` (and any `faces/` or `assets/` files), then press **Reload widgets**\n"
        "(the dock menu's Tools, App settings > Widget library or the Console). Set `\"watch\": true` in widget.json to reload on every save.\n"
        "Delete the folder to get the built-in back.\n\n"
        "- The API (canvas, dock, timer, settings, storage, providers, popups, the manifest and its settings schema):\n"
        "  docs/widgets.md in the WinDock repository.\n"
        "- Check the manifest: `WinDock.exe --validate-widget \"<this folder>\"`.\n"
        "- Errors and `console.log` output: the dock menu's Tools > Widget console (or the Console button of the widget's settings).\n";
    if (!JsonUtil::WriteFileAtomic(folder + L"\\README.md", readme)) {
        error = L"cannot write " + folder + L"\\README.md";
        return false;
    }
    return true;
}

bool InstallFolder(const std::wstring& source, std::wstring& id, std::wstring& error) {
    std::wstring src = source;
    while (src.size() > 3 && src.back() == L'\\') src.pop_back();
    const DWORD a = GetFileAttributesW((src + L"\\widget.json").c_str());
    if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) {
        error = src + L" has no widget.json";
        return false;
    }
    const size_t cut = src.find_last_of(L'\\');
    id = Lower(cut == std::wstring::npos ? src : src.substr(cut + 1));
    if (!ValidId(id)) {
        error = L"the folder name \"" + id + L"\" is not a valid widget id (a-z, 0-9, - _ .)";
        return false;
    }
    const std::wstring dest = Directory() + L"\\" + id;
    if (GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES) {
        error = L"a widget \"" + id + L"\" is installed already (" + dest + L")";
        return false;
    }
    Package from;
    from.source = Package::Source::Folder;
    from.folder = CanonicalFolder(src);
    if (from.folder.empty()) {
        error = L"cannot open " + src;
        return false;
    }
    const std::vector<std::string> files = from.Files();
    if (!CreateDirs(dest)) {
        error = L"cannot create " + dest;
        return false;
    }
    for (const std::string& f : files) {
        std::string bytes;
        std::wstring readError;
        if (!from.Read(f, bytes, readError)) continue;  // outside the folder (a link) or too large: skipped
        std::wstring rel = Utf8ToWide(f);
        std::replace(rel.begin(), rel.end(), L'/', L'\\');
        const std::wstring target = dest + L"\\" + rel;
        if (const size_t c = target.find_last_of(L'\\'); c != std::wstring::npos) CreateDirs(target.substr(0, c));
        if (!JsonUtil::WriteFileAtomic(target, bytes)) {
            error = L"cannot write " + target;
            return false;
        }
    }
    return true;
}

std::wstring DisplayName(const std::wstring& id) {
    std::wstring error;
    const auto pkg = Load(id, error);
    return pkg && !pkg->manifest.name.empty() ? pkg->manifest.name : id;
}

bool CanBeStandalone(const std::wstring& id) {
    std::wstring error;
    const auto pkg = Load(id, error);
    return pkg && pkg->manifest.standalone.supported;
}

std::string MergeSettings(const Manifest& manifest, const std::wstring& itemJson) {
    try {
        json::JsonObject item{nullptr};
        if (!itemJson.empty() && (!json::JsonObject::TryParse(itemJson, item) || !item)) item = nullptr;
        json::JsonObject out;
        for (const SettingField& f : manifest.settings) {
            if (f.type == SettingField::Type::Place) {
                // No key of its own: the item's place keys, checked (round 5).
                if (!item) continue;
                const json::IJsonValue lat = JsonUtil::Find(item, L"latitude"), lon = JsonUtil::Find(item, L"longitude");
                if (lat && lon && lat.ValueType() == json::JsonValueType::Number && lon.ValueType() == json::JsonValueType::Number &&
                    std::fabs(lat.GetNumber()) <= 90.0 && std::fabs(lon.GetNumber()) <= 180.0) {
                    out.Insert(L"latitude", json::JsonValue::CreateNumberValue(std::round(lat.GetNumber() * 1e4) / 1e4));
                    out.Insert(L"longitude", json::JsonValue::CreateNumberValue(std::round(lon.GetNumber() * 1e4) / 1e4));
                }
                const json::IJsonValue loc = JsonUtil::Find(item, L"location");
                if (loc && loc.ValueType() == json::JsonValueType::String) out.Insert(L"location", loc);
                continue;
            }
            json::IJsonValue value = json::JsonValue::Parse(f.defaultJson);
            const json::IJsonValue given = item ? JsonUtil::Find(item, f.key.c_str()) : nullptr;
            if (given) {
                const json::JsonValueType t = given.ValueType();
                switch (f.type) {
                    case SettingField::Type::Bool:
                        if (t == json::JsonValueType::Boolean) value = given;
                        break;
                    case SettingField::Type::Int:
                    case SettingField::Type::Number:
                        if (t == json::JsonValueType::Number && std::isfinite(given.GetNumber())) {
                            double n = given.GetNumber();
                            if (f.type == SettingField::Type::Int) n = std::round(n);
                            value = json::JsonValue::CreateNumberValue(Clamp(n, f.min, f.max));
                        }
                        break;
                    case SettingField::Type::Choice:
                        if (t == json::JsonValueType::String &&
                            (f.choices.empty() || std::any_of(f.choices.begin(), f.choices.end(), [&](const auto& c) {
                                 return c.first == given.GetString();
                             }))) {
                            value = given;
                        }
                        break;
                    case SettingField::Type::Color: {
                        uint32_t argb = 0;
                        if (t == json::JsonValueType::String && ParseArgbColor(std::wstring(given.GetString()), argb)) {
                            value = given;
                        }
                        break;
                    }
                    case SettingField::Type::Font:
                    case SettingField::Type::String:
                    case SettingField::Type::File:  // round 17 phase 17A: any string (the widget checks the path)
                    case SettingField::Type::Folder:
                    default:
                        if (t == json::JsonValueType::String) value = given;
                        break;
                }
            }
            out.Insert(f.key, value);
        }
        if (item) {
            for (const auto& kv : item) {
                if (out.HasKey(kv.Key())) continue;
                // A place's own keys were checked above: a bad latitude / longitude is not passed on.
                const std::wstring k(kv.Key());
                if ((k == L"latitude" || k == L"longitude") &&
                    std::any_of(manifest.settings.begin(), manifest.settings.end(),
                                [](const SettingField& f) { return f.type == SettingField::Type::Place; })) {
                    continue;
                }
                out.Insert(kv.Key(), kv.Value());
            }
        }
        return WideToUtf8(std::wstring(out.Stringify()));
    } catch (...) {
        return "{}";
    }
}

std::vector<SettingField> BuiltInSchema(ItemKind kind) {
    using T = SettingField::Type;
    auto field = [](const wchar_t* key, T type, const wchar_t* label, std::wstring defaultJson, const wchar_t* group,
                    const wchar_t* description = L"") {
        SettingField f;
        f.key = key;
        f.type = type;
        f.label = label;
        f.defaultJson = std::move(defaultJson);
        f.group = group;
        f.description = description;
        return f;
    };
    std::vector<SettingField> s;
    if (kind == ItemKind::Clock) {
        const ClockConfig d;
        // Round 10 phase 10B: format, time zone, the date and the label are "common" (the Contents
        // tab's inspector, in schema order).
        s.push_back(field(L"format", T::String, L"Time format", JsonUtil::Quote(d.format), L"Time",
                          L"strftime: %H:%M (14:05), %I:%M %p (02:05 PM), %#I:%M %p (2:05 PM), %H:%M:%S"));
        s.back().common = true;
        s.push_back(field(L"showSeconds", T::Bool, L"Show seconds", L"false", L"Time",
                          L"Adds the seconds to the time (the clock then redraws every second). Off by default."));
        s.push_back(field(L"timeZone", T::TimeZone, L"Time zone", L"\"\"", L"Time",
                          L"Local time follows Windows; another zone shows its time (with its daylight saving)."));
        s.back().common = true;
        s.push_back(field(L"showDate", T::Bool, L"Show the date under the time", L"false", L"Text",
                          L"A short date line (e.g. Wed 4 Nov) under the time. Off by default."));
        s.back().common = true;
        s.push_back(field(L"label", T::String, L"Label", L"\"\"", L"Text",
                          L"Shown on the date's line (alone under the time without the date), e.g. Tokyo."));
        s.back().common = true;
        SettingField face = field(L"face", T::Choice, L"Face", JsonUtil::Quote(ClockFaces::kDigital), L"Look",
                                  L"Digital is the clock's own look; the others are face files (docs/widgets.md \"Clock faces\").");
        face.choices.emplace_back(ClockFaces::kDigital, L"Digital (built in)");
        for (const ClockFaces::Entry& e : ClockFaces::List()) {
            face.choices.emplace_back(e.id, e.name + (e.builtIn ? L"" : L" (file)"));
        }
        s.push_back(std::move(face));
        SettingField click = field(L"click", T::Choice, L"Clicking the clock opens", L"\"calendar\"", L"Click",
                                   L"What a click on the clock does: the month calendar (the default), an app or URL, or nothing.");
        click.choices = {{L"calendar", L"The month calendar"}, {L"launch", L"The app or URL below"}, {L"none", L"Nothing"}};
        s.push_back(std::move(click));
        SettingField target = field(L"clickTarget", T::String, L"App or URL", L"\"\"", L"Click",
                                    L"An app, a document or a URL; the month calendar shows a button that opens it too.");
        target.visibleIf.emplace_back(L"click", L"\"launch\"");
        s.push_back(std::move(target));
        // Round 10 phase 10C: the face is Appearance (the clock has no colour / font / size of its
        // own: the theme draws it); the time, the zone, the date, the label and the click Content.
        for (SettingField& f : s) f.section = f.key == L"face" ? SettingField::Section::Appearance : SettingField::Section::Content;
    } else if (kind == ItemKind::Weather) {
        // Round 10 phase 10B: the place (read only in the inspector), the name and the units are "common".
        s.push_back(field(L"place", T::Place, L"Place", L"null", L"Place"));
        s.back().common = true;
        s.push_back(field(L"label", T::String, L"Name on the dock", L"\"\"", L"Place",
                          L"Shown under the temperature (empty: none)."));
        s.back().common = true;
        SettingField units = field(L"units", T::Choice, L"Units", JsonUtil::Quote(ToString(LocaleUnits())), L"Updates",
                                   L"Temperature and wind units. The default follows your Windows region.");
        units.choices = {{L"metric", L"Metric (°C, km/h)"}, {L"imperial", L"Imperial (°F, mph)"}};
        units.common = true;
        s.push_back(std::move(units));
        SettingField refresh = field(L"refreshMinutes", T::Int, L"Refresh every", L"30", L"Updates",
                                     L"How often the forecast is fetched again. Default: 30 minutes.");
        refresh.min = 5;
        refresh.max = 360;
        refresh.step = 5;
        refresh.unit = L"minutes";
        s.push_back(std::move(refresh));
        // Round 10 phase 10C: every field is Content (the place, the name, the units, the refresh):
        // the Item window then shows no tab strip.
        for (SettingField& f : s) f.section = SettingField::Section::Content;
    }
    return s;
}

SettingField::Section DefaultSection(const SettingField& field) {
    if (field.type == SettingField::Type::Color) return SettingField::Section::Appearance;
    for (const wchar_t* g : {L"Colours", L"Colors", L"Look", L"Face", L"Size", L"Slot", L"Display", L"Motion", L"Motion and size"}) {
        if (_wcsicmp(field.group.c_str(), g) == 0) return SettingField::Section::Appearance;
    }
    return SettingField::Section::Content;
}

unsigned SectionMask(const std::vector<SettingField>& fields, size_t limit) {
    unsigned mask = 0;
    for (size_t k = 0; k < fields.size() && k < limit; ++k) {
        mask |= fields[k].section == SettingField::Section::Appearance ? 2u : 1u;
    }
    return mask;
}

const wchar_t* SectionName(SettingField::Section section) {
    return section == SettingField::Section::Appearance ? L"Appearance" : L"Content";
}

std::vector<size_t> CommonFields(const Manifest& manifest) {
    std::vector<size_t> out;
    const auto& fields = manifest.settings;
    for (size_t k = 0; k < fields.size(); ++k) {
        if (fields[k].common) out.push_back(k);
    }
    if (!out.empty()) return out;
    // None marked (another author's widget): the first three of a simple type.
    using T = SettingField::Type;
    for (size_t k = 0; k < fields.size() && out.size() < 3; ++k) {
        const SettingField& f = fields[k];
        const bool simple = f.type == T::Bool || f.type == T::Int || f.type == T::Number || f.type == T::String ||
                            (f.type == T::Choice && !f.ChoiceFromPerf());
        if (simple) out.push_back(k);
    }
    return out;
}

}  // namespace Widgets
