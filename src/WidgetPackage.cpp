#include "WidgetPackage.h"

#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <iterator>

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
        if (f.type == SettingField::Type::Choice) {
            JsonUtil::ReadString(o, L"choiceFrom", f.choiceFrom);
            if (!f.choiceFrom.empty() && f.choiceFrom.find_first_of(L":\\") != std::wstring::npos) {
                m.warnings.push_back(L"settings." + f.key + L": choiceFrom \"" + f.choiceFrom + L"\" is not a package glob");
                f.choiceFrom.clear();
            }
        }
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
            case SettingField::Type::Font:
            case SettingField::Type::String:
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
    return pkg;
}

void ExpandChoices(Package& pkg) {
    std::vector<std::string> files;
    bool listed = false;
    for (SettingField& f : pkg.manifest.settings) {
        if (f.type != SettingField::Type::Choice || f.choiceFrom.empty()) continue;
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

std::string MergeSettings(const Manifest& manifest, const std::wstring& itemJson) {
    try {
        json::JsonObject item{nullptr};
        if (!itemJson.empty() && (!json::JsonObject::TryParse(itemJson, item) || !item)) item = nullptr;
        json::JsonObject out;
        for (const SettingField& f : manifest.settings) {
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
                    default:
                        if (t == json::JsonValueType::String) value = given;
                        break;
                }
            }
            out.Insert(f.key, value);
        }
        if (item) {
            for (const auto& kv : item) {
                if (!out.HasKey(kv.Key())) out.Insert(kv.Key(), kv.Value());
            }
        }
        return WideToUtf8(std::wstring(out.Stringify()));
    } catch (...) {
        return "{}";
    }
}

}  // namespace Widgets
