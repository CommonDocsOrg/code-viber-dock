#include "ClockFaces.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <initializer_list>
#include <iterator>

#include "Config.h"
#include "JsonUtil.h"
#include "resource.h"

namespace ClockFaces {

namespace {

namespace json = winrt::Windows::Data::Json;
using JsonUtil::Utf8ToWide;

struct Shipped {
    const wchar_t* id;
    int resource;
};
constexpr Shipped kShipped[] = {
    {L"default", IDR_CLOCKFACE_DEFAULT},
    {L"large-time-small-date", IDR_CLOCKFACE_LARGE},
    {L"mono-pill", IDR_CLOCKFACE_MONO_PILL},
};
constexpr size_t kMaxFileBytes = 256u * 1024u;

bool ReadResource(int resource, std::string& out) {
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(resource), MAKEINTRESOURCEW(10));  // RT_RCDATA
    HGLOBAL mem = res ? LoadResource(module, res) : nullptr;
    const void* data = mem ? LockResource(mem) : nullptr;
    if (!data) return false;
    out.assign(static_cast<const char*>(data), SizeofResource(module, res));
    if (out.size() >= 3 && out.compare(0, 3, "\xEF\xBB\xBF") == 0) out.erase(0, 3);
    return true;
}

bool ValidId(const std::wstring& id) {
    if (id.empty() || id.size() > 64 || id.front() == L'.') return false;
    return std::all_of(id.begin(), id.end(), [](wchar_t c) {
        return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'-' ||
               c == L'_' || c == L'.';
    });
}

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

bool ReadUserFile(const std::wstring& id, std::string& bytes) {
    const std::wstring path = Directory() + L"\\" + id + L".json";
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        a.nFileSizeHigh || a.nFileSizeLow > kMaxFileBytes) {
        return false;
    }
    return JsonUtil::ReadFileUtf8(path, bytes);
}

// One of `names` (case-insensitive) -> its index; `def` when missing; -1 when not one of them.
int Pick(const json::JsonObject& o, const wchar_t* key, std::initializer_list<const wchar_t*> names, int def) {
    const json::IJsonValue v = JsonUtil::Find(o, key);
    if (!v) return def;
    if (v.ValueType() != json::JsonValueType::String) return -1;
    const std::wstring s = Lower(std::wstring(v.GetString()));
    int i = 0;
    for (const wchar_t* n : names) {
        if (s == n) return i;
        ++i;
    }
    return -1;
}

bool ColorOk(const std::wstring& s) {
    if (s.empty() || s.rfind(L"theme:", 0) == 0) return true;
    uint32_t argb = 0;
    return ParseArgbColor(s, argb);
}

bool ReadText(const json::JsonObject& root, const wchar_t* key, ClockFace::Text& t, std::wstring& error) {
    const json::IJsonValue v = JsonUtil::Find(root, key);
    if (!v) return true;
    if (v.ValueType() != json::JsonValueType::Object) {
        error = std::wstring(key) + L": must be an object ({ \"font\", \"weight\", \"size\", \"color\" })";
        return false;
    }
    const json::JsonObject o = v.GetObject();
    JsonUtil::ReadString(o, L"font", t.font);
    double weight = t.weight, size = t.size;
    JsonUtil::ReadNumber(o, L"weight", weight);
    JsonUtil::ReadNumber(o, L"size", size);
    if (!(size > 0.1 && size <= 4.0)) {
        error = std::wstring(key) + L".size: a ratio from 0.1 to 4 (1 = the built-in clock's size)";
        return false;
    }
    if (weight != 0 && !(weight >= 100 && weight <= 950)) {
        error = std::wstring(key) + L".weight: 100 - 950 (400 normal, 600 semibold, 700 bold)";
        return false;
    }
    t.weight = static_cast<int>(weight);
    t.size = static_cast<float>(size);
    JsonUtil::ReadString(o, L"color", t.color);
    if (!ColorOk(t.color)) {
        error = std::wstring(key) + L".color: \"#AARRGGBB\", \"#RRGGBB\" or \"theme:text\" (dateText, accent, ...)";
        return false;
    }
    const int c = Pick(o, L"case", {L"as-is", L"upper", L"lower"}, 0);
    if (c < 0) {
        error = std::wstring(key) + L".case: \"as-is\", \"upper\" or \"lower\"";
        return false;
    }
    t.textCase = static_cast<ClockFace::Case>(c);
    return true;
}

}  // namespace

std::wstring Directory() { return Config::Directory() + L"\\clock-faces"; }

bool Parse(const std::string& utf8, ClockFace& f, std::wstring& error) {
    json::JsonObject root{nullptr};
    try {
        if (!json::JsonObject::TryParse(Utf8ToWide(utf8), root) || !root) {
            error = L"not a JSON object";
            return false;
        }
        std::wstring type;
        JsonUtil::ReadString(root, L"type", type);
        if (Lower(type) != L"digital") {
            error = type.empty() ? L"type: missing (a digital clock face says \"type\": \"digital\"; analog faces are for "
                                   L"the Analog clock widget)"
                                 : L"type: \"" + type + L"\" is not \"digital\" (analog faces are for the Analog clock widget)";
            return false;
        }
        double format = 2;
        JsonUtil::ReadNumber(root, L"format", format);
        if (format != 2) {
            error = L"format: must be 2 (the digital face format)";
            return false;
        }
        JsonUtil::ReadString(root, L"name", f.name);
        JsonUtil::ReadString(root, L"description", f.description);
        const int layout = Pick(root, L"layout", {L"stacked", L"inline", L"date-first"}, 0);
        if (layout < 0) {
            error = L"layout: \"stacked\", \"inline\" or \"date-first\"";
            return false;
        }
        f.layout = static_cast<ClockFace::Layout>(layout);
        const int align = Pick(root, L"align", {L"center", L"start", L"end"}, 0);
        if (align < 0) {
            error = L"align: \"center\", \"start\" or \"end\"";
            return false;
        }
        f.align = static_cast<ClockFace::Align>(align);
        if (!ReadText(root, L"time", f.time, error) || !ReadText(root, L"date", f.date, error)) return false;
        if (const json::JsonObject t = JsonUtil::ReadObject(root, L"time")) JsonUtil::ReadString(t, L"separator", f.timeSeparator);
        if (const json::JsonObject d = JsonUtil::ReadObject(root, L"date")) {
            JsonUtil::ReadString(d, L"format", f.dateFormat);
            if (f.dateFormat.empty() || f.dateFormat.size() > 64) {
                error = L"date.format: a strftime format of 1 - 64 characters (\"%a %d %b\")";
                return false;
            }
        }
        JsonUtil::ReadString(root, L"separator", f.separator);
        if (f.separator.size() > 16) f.separator.resize(16);
        if (const json::JsonObject s = JsonUtil::ReadObject(root, L"seconds")) {
            const int st = Pick(s, L"style", {L"inline", L"small", L"line", L"hidden"}, 0);
            if (st < 0) {
                error = L"seconds.style: \"inline\", \"small\", \"line\" or \"hidden\"";
                return false;
            }
            f.seconds = static_cast<ClockFace::Seconds>(st);
        }
        if (!ReadText(root, L"seconds", f.secondsText, error)) return false;
        if (const json::JsonObject s = JsonUtil::ReadObject(root, L"suffix")) {
            const int st = Pick(s, L"style", {L"as-is", L"small", L"lower", L"none"}, 0);
            if (st < 0) {
                error = L"suffix.style: \"as-is\", \"small\", \"lower\" or \"none\"";
                return false;
            }
            f.suffix = static_cast<ClockFace::Suffix>(st);
        }
        if (!ReadText(root, L"suffix", f.suffixText, error)) return false;
        double gap = f.lineGap;
        JsonUtil::ReadNumber(root, L"lineGap", gap);
        f.lineGap = static_cast<float>(std::clamp(gap, -20.0, 40.0));
        if (const json::JsonObject v = JsonUtil::ReadObject(root, L"vertical")) {
            JsonUtil::ReadBool(v, L"stack", f.verticalStack);
            const int va = Pick(v, L"align", {L"center", L"start", L"end"}, 0);
            if (va < 0) {
                error = L"vertical.align: \"center\", \"start\" or \"end\"";
                return false;
            }
            f.verticalAlign = static_cast<ClockFace::Align>(va);
        }
        if (const json::JsonObject b = JsonUtil::ReadObject(root, L"background")) {
            f.background = true;
            JsonUtil::ReadString(b, L"fill", f.fill);
            JsonUtil::ReadString(b, L"border", f.border);
            if (!ColorOk(f.fill) || !ColorOk(f.border)) {
                error = L"background.fill / border: \"#AARRGGBB\" or \"theme:<name>\"";
                return false;
            }
            double w = f.borderWidth, r = f.radius, px = f.padX, py = f.padY;
            JsonUtil::ReadNumber(b, L"borderWidth", w);
            JsonUtil::ReadNumber(b, L"radius", r);
            JsonUtil::ReadNumber(b, L"padX", px);
            JsonUtil::ReadNumber(b, L"padY", py);
            f.borderWidth = static_cast<float>(std::clamp(w, 0.0, 8.0));
            f.radius = static_cast<float>(std::clamp(r, 0.0, 64.0));
            f.padX = static_cast<float>(std::clamp(px, 0.0, 32.0));
            f.padY = static_cast<float>(std::clamp(py, 0.0, 32.0));
        }
        return true;
    } catch (...) {
        error = L"could not be read";
        return false;
    }
}

std::vector<Entry> List() {
    std::vector<Entry> out;
    for (const Shipped& s : kShipped) {
        std::string bytes;
        ClockFace f;
        std::wstring error;
        Entry e{s.id, s.id, true};
        if (ReadResource(s.resource, bytes) && Parse(bytes, f, error) && !f.name.empty()) e.name = f.name;
        out.push_back(std::move(e));
    }
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileExW((Directory() + L"\\*.json").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (find != INVALID_HANDLE_VALUE) {
        std::vector<Entry> user;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring id = fd.cFileName;
            id.resize(id.size() - 5);  // ".json"
            if (!ValidId(id) || Lower(id) == kDigital) continue;
            std::string bytes;
            ClockFace f;
            std::wstring error;
            if (!ReadUserFile(id, bytes) || !Parse(bytes, f, error)) continue;  // not a digital face: not offered
            Entry e{id, f.name.empty() ? id : f.name, false};
            const auto it = std::find_if(out.begin(), out.end(), [&](const Entry& x) { return Lower(x.id) == Lower(id); });
            if (it != out.end()) {
                *it = e;  // the user's file replaces the shipped face
            } else {
                user.push_back(std::move(e));
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
        std::sort(user.begin(), user.end(), [](const Entry& a, const Entry& b) {
            return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE, a.name.c_str(), -1, b.name.c_str(), -1, nullptr,
                                   nullptr, 0) == CSTR_LESS_THAN;
        });
        out.insert(out.end(), user.begin(), user.end());
    }
    return out;
}

std::shared_ptr<const ClockFace> Load(const std::wstring& id, std::wstring& error) {
    error.clear();
    if (id.empty() || Lower(id) == kDigital) return nullptr;
    if (!ValidId(id)) {
        error = L"\"" + id + L"\" is not a face id";
        return nullptr;
    }
    std::string bytes;
    bool builtIn = false;
    if (!ReadUserFile(id, bytes)) {
        const auto* s = std::find_if(std::begin(kShipped), std::end(kShipped), [&](const Shipped& x) { return Lower(id) == x.id; });
        if (s == std::end(kShipped) || !ReadResource(s->resource, bytes)) {
            error = L"no clock face \"" + id + L"\" (" + Directory() + L"\\" + id + L".json)";
            return nullptr;
        }
        builtIn = true;
    }
    auto f = std::make_shared<ClockFace>();
    if (!Parse(bytes, *f, error)) {
        error = id + L".json: " + error;
        return nullptr;
    }
    f->id = id;
    f->builtIn = builtIn;
    if (f->name.empty()) f->name = id;
    return f;
}

uint32_t ResolveColor(const std::wstring& spec, const ThemeConfig& t, uint32_t fallback) {
    if (spec.rfind(L"theme:", 0) == 0) {
        const std::wstring n = Lower(spec.substr(6));
        if (n == L"text") return t.clockText;
        if (n == L"datetext") return t.clockDateText;
        if (n == L"accent") return t.accent;
        if (n == L"background") return t.background;
        if (n == L"labeltext") return t.labelText;
        if (n == L"labelbackground") return t.labelBackground;
        if (n == L"separator") return t.separator;
        if (n == L"border") return t.barBorder;
        return fallback;
    }
    uint32_t argb = 0;
    return !spec.empty() && ParseArgbColor(spec, argb) ? argb : fallback;
}

ClockFaceColors Resolve(const ClockFace& f, const ThemeConfig& t) {
    ClockFaceColors c;
    c.time = ResolveColor(f.time.color, t, t.clockText);
    c.date = ResolveColor(f.date.color, t, t.clockDateText);
    c.seconds = ResolveColor(f.secondsText.color, t, c.time);
    c.suffix = ResolveColor(f.suffixText.color, t, c.time);
    c.fill = ResolveColor(f.fill, t, 0x2EFFFFFF);
    c.border = ResolveColor(f.border, t, 0);
    return c;
}

}  // namespace ClockFaces
