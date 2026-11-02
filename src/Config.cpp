#include <Unknwn.h>
#include <windows.h>
#include <shlobj.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>

#include <algorithm>
#include <cwchar>
#include <iterator>

#include "Config.h"

namespace json = winrt::Windows::Data::Json;

namespace {

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool ReadFileUtf8(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (16 << 20);
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = out.empty() ||
             (ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) && read == out.size());
    }
    CloseHandle(h);
    if (ok && out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF &&
        static_cast<unsigned char>(out[1]) == 0xBB && static_cast<unsigned char>(out[2]) == 0xBF) {
        out.erase(0, 3);
    }
    return ok;
}

bool WriteFileAtomic(const std::wstring& path, const std::string& data) {
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size();
    ok = FlushFileBuffers(h) && ok;
    CloseHandle(h);
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

// --- typed lookups that tolerate missing keys and wrong types ---
json::IJsonValue Find(const json::JsonObject& o, const wchar_t* key) {
    if (!o.HasKey(key)) return nullptr;
    return o.GetNamedValue(key);
}

void ReadNumber(const json::JsonObject& o, const wchar_t* key, double& v) {
    auto j = Find(o, key);
    if (j && j.ValueType() == json::JsonValueType::Number) v = j.GetNumber();
}

void ReadInt(const json::JsonObject& o, const wchar_t* key, int& v) {
    double d = v;
    ReadNumber(o, key, d);
    if (d > -1e9 && d < 1e9) v = static_cast<int>(d);
}

void ReadBool(const json::JsonObject& o, const wchar_t* key, bool& v) {
    auto j = Find(o, key);
    if (j && j.ValueType() == json::JsonValueType::Boolean) v = j.GetBoolean();
}

void ReadString(const json::JsonObject& o, const wchar_t* key, std::wstring& v) {
    auto j = Find(o, key);
    if (j && j.ValueType() == json::JsonValueType::String) v = std::wstring(j.GetString());
}

json::JsonObject ReadObject(const json::JsonObject& o, const wchar_t* key) {
    auto j = Find(o, key);
    if (j && j.ValueType() == json::JsonValueType::Object) return j.GetObject();
    return nullptr;
}

// --- writer (deterministic key order, human-friendly indentation) ---
std::wstring Quote(const std::wstring& s) {
    return std::wstring(json::JsonValue::CreateStringValue(s).Stringify());
}

std::wstring Num(double d) {
    wchar_t buf[64];
    swprintf_s(buf, L"%.6g", d);
    return buf;
}

std::wstring Bool(bool b) { return b ? L"true" : L"false"; }

}  // namespace

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

void Config::Sanitize() {
    iconSize = std::clamp(iconSize, 16, 256);
    if (!(magnification >= 1.0)) magnification = 1.0;  // also catches NaN
    magnification = std::min(magnification, 2.5);
    magnifyRange = std::clamp(magnifyRange, 1, 4);
    hideDelayMs = std::clamp(hideDelayMs, 0, 10000);
    revealDelayMs = std::clamp(revealDelayMs, 0, 10000);
    previewDelayMs = std::clamp(previewDelayMs, 0, 10000);
    monitor = std::max(monitor, 0);
    if (!(theme.cornerRadius >= 0.0f)) theme.cornerRadius = 0.0f;
    theme.cornerRadius = std::min(theme.cornerRadius, 64.0f);
    if (clock.format.size() > 128) clock.format.resize(128);
    if (renderer != L"auto" && renderer != L"software") renderer = L"auto";
}

Config::LoadResult Config::LoadFile() {
    *this = Config{};
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

        static const wchar_t* const kKnown[] = {
            L"pinned",        L"iconSize", L"magnification", L"magnifyRange",     L"autoHide", L"hideDelayMs",
            L"revealDelayMs", L"monitor",  L"hideTaskbar",   L"startWithWindows", L"renderer", L"clock",
            L"theme",         L"previews", L"previewDelayMs",
        };
        for (const auto& kv : root) {
            const std::wstring key(kv.Key());
            if (std::any_of(std::begin(kKnown), std::end(kKnown), [&](const wchar_t* k) { return key == k; })) {
                continue;
            }
            extraFields.emplace_back(key, std::wstring(kv.Value().Stringify()));
        }

        if (auto p = Find(root, L"pinned"); p && p.ValueType() == json::JsonValueType::Array) {
            for (const auto& item : p.GetArray()) {
                if (item.ValueType() != json::JsonValueType::Object) continue;
                PinnedEntry e;
                auto obj = item.GetObject();
                ReadString(obj, L"path", e.path);
                ReadString(obj, L"aumid", e.aumid);
                if (!e.path.empty() || !e.aumid.empty()) pinned.push_back(std::move(e));
            }
        }

        ReadInt(root, L"iconSize", iconSize);
        ReadNumber(root, L"magnification", magnification);
        ReadInt(root, L"magnifyRange", magnifyRange);
        ReadBool(root, L"autoHide", autoHide);
        ReadInt(root, L"hideDelayMs", hideDelayMs);
        ReadInt(root, L"revealDelayMs", revealDelayMs);
        ReadBool(root, L"previews", previews);
        ReadInt(root, L"previewDelayMs", previewDelayMs);
        ReadInt(root, L"monitor", monitor);
        ReadBool(root, L"hideTaskbar", hideTaskbar);
        ReadBool(root, L"startWithWindows", startWithWindows);
        ReadString(root, L"renderer", renderer);

        if (auto c = ReadObject(root, L"clock")) {
            ReadBool(c, L"show", clock.show);
            ReadString(c, L"format", clock.format);
            ReadBool(c, L"showDate", clock.showDate);
        }
        if (auto t = ReadObject(root, L"theme")) {
            std::wstring bg;
            ReadString(t, L"background", bg);
            uint32_t argb = 0;
            if (!bg.empty() && ParseArgbColor(bg, argb)) theme.background = argb;
            ReadBool(t, L"acrylic", theme.acrylic);
            bg.clear();
            ReadString(t, L"acrylicBackground", bg);
            if (!bg.empty() && ParseArgbColor(bg, argb)) theme.acrylicBackground = argb;
            double radius = theme.cornerRadius;
            ReadNumber(t, L"cornerRadius", radius);
            theme.cornerRadius = static_cast<float>(radius);
            std::wstring color;
            ReadString(t, L"labelBackground", color);
            if (!color.empty() && ParseArgbColor(color, argb)) theme.labelBackground = argb;
            color.clear();
            ReadString(t, L"labelText", color);
            if (!color.empty() && ParseArgbColor(color, argb)) theme.labelText = argb;
            color.clear();
            ReadString(t, L"accent", color);
            if (!color.empty() && ParseArgbColor(color, argb)) theme.accent = argb;
        }
    } catch (...) {
        OutputDebugStringW(L"WinDock: failed to read config.json; using defaults\n");
        *this = Config{};
        return LoadResult::Malformed;
    }
    Sanitize();
    return LoadResult::Ok;
}

bool Config::Save() const {
    try {
        std::wstring dir = Directory();
        CreateDirectoryW(dir.c_str(), nullptr);

        std::wstring o;
        o += L"{\n  \"pinned\": [";
        for (size_t i = 0; i < pinned.size(); ++i) {
            const auto& e = pinned[i];
            o += i ? L",\n    " : L"\n    ";
            if (!e.aumid.empty()) o += L"{ \"aumid\": " + Quote(e.aumid) + L" }";
            else o += L"{ \"path\": " + Quote(e.path) + L" }";
        }
        o += pinned.empty() ? L"],\n" : L"\n  ],\n";
        o += L"  \"iconSize\": " + Num(iconSize) + L",\n";
        o += L"  \"magnification\": " + Num(magnification) + L",\n";
        o += L"  \"magnifyRange\": " + Num(magnifyRange) + L",\n";
        o += L"  \"autoHide\": " + Bool(autoHide) + L",\n";
        o += L"  \"hideDelayMs\": " + Num(hideDelayMs) + L",\n";
        o += L"  \"revealDelayMs\": " + Num(revealDelayMs) + L",\n";
        o += L"  \"monitor\": " + Num(monitor) + L",\n";
        o += L"  \"hideTaskbar\": " + Bool(hideTaskbar) + L",\n";
        o += L"  \"startWithWindows\": " + Bool(startWithWindows) + L",\n";
        o += L"  \"renderer\": " + Quote(renderer) + L",\n";
        o += L"  \"clock\": { \"show\": " + Bool(clock.show) + L", \"format\": " + Quote(clock.format) +
             L", \"showDate\": " + Bool(clock.showDate) + L" },\n";
        o += L"  \"theme\": { \"background\": " + Quote(FormatArgbColor(theme.background)) +
             L", \"cornerRadius\": " + Num(theme.cornerRadius) +
             L",\n             \"acrylic\": " + Bool(theme.acrylic) +
             L", \"acrylicBackground\": " + Quote(FormatArgbColor(theme.acrylicBackground)) +
             L",\n             \"labelBackground\": " + Quote(FormatArgbColor(theme.labelBackground)) +
             L", \"labelText\": " + Quote(FormatArgbColor(theme.labelText)) +
             L", \"accent\": " + Quote(FormatArgbColor(theme.accent)) + L" }";
        for (const auto& [key, value] : extraFields) o += L",\n  " + Quote(key) + L": " + value;
        o += L"\n}\n";
        return WriteFileAtomic(FilePath(), WideToUtf8(o));
    } catch (...) {
        return false;
    }
}
