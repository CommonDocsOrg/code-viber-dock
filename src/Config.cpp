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
bool FromString(std::wstring_view s, AutoHideMode& v) { return EnumFromString(s, v, kAutoHideNames); }
bool FromString(std::wstring_view s, HideStyle& v) { return EnumFromString(s, v, kHideStyleNames); }
bool FromString(std::wstring_view s, LaunchStyle& v) { return EnumFromString(s, v, kLaunchStyleNames); }
bool FromString(std::wstring_view s, Falloff& v) { return EnumFromString(s, v, kFalloffNames); }
bool FromString(std::wstring_view s, Easing& v) { return EnumFromString(s, v, kEasingNames); }
bool FromString(std::wstring_view s, ReduceMotion& v) { return EnumFromString(s, v, kReduceMotionNames); }

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
    return ok;
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
    if (!(theme.labelCornerRadius >= 0.0f)) theme.labelCornerRadius = 0.0f;
    theme.labelCornerRadius = std::min(theme.labelCornerRadius, 32.0f);
    if (!(theme.shelfSlant >= 0.0f)) theme.shelfSlant = 0.0f;
    theme.shelfSlant = std::min(theme.shelfSlant, 64.0f);
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
            L"theme",         L"previews", L"previewDelayMs", L"animation",
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
        // Older versions wrote a bool (true = "always").
        if (auto a = Find(root, L"autoHide")) {
            if (a.ValueType() == json::JsonValueType::Boolean) {
                autoHide = a.GetBoolean() ? AutoHideMode::Always : AutoHideMode::Off;
            } else if (a.ValueType() == json::JsonValueType::String) {
                if (!FromString(std::wstring(a.GetString()), autoHide)) autoHide = AutoHideMode::Always;
            }
        }
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
        const uint32_t bit = Theme::Bit(f.field);
        if (ok && (themeOverrides & bit) && Theme::FieldEquals(f, theme, base)) themeOverrides &= ~bit;
        if (!(themeOverrides & bit)) Theme::CopyField(f, base, theme);
    }
    themeResolveFailed = !ok;
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
        o += L"  \"autoHide\": " + Quote(ToString(autoHide)) + L",\n";
        o += L"  \"hideDelayMs\": " + Num(hideDelayMs) + L",\n";
        o += L"  \"revealDelayMs\": " + Num(revealDelayMs) + L",\n";
        o += L"  \"previews\": " + Bool(previews) + L",\n";
        o += L"  \"previewDelayMs\": " + Num(previewDelayMs) + L",\n";
        o += L"  \"monitor\": " + Num(monitor) + L",\n";
        o += L"  \"hideTaskbar\": " + Bool(hideTaskbar) + L",\n";
        o += L"  \"startWithWindows\": " + Bool(startWithWindows) + L",\n";
        o += L"  \"renderer\": " + Quote(renderer) + L",\n";
        o += L"  \"clock\": { \"show\": " + Bool(clock.show) + L", \"format\": " + Quote(clock.format) +
             L", \"showDate\": " + Bool(clock.showDate) + L" },\n";
        // Only the preset and the fields set inline: editing the theme file keeps affecting the dock.
        o += L"  \"theme\": { \"preset\": " + Quote(themePreset) +
             Theme::WriteFields(theme, themeOverrides, L"             ") + L" },\n";
        o += L"  \"animation\": { \"speed\": " + Num(animation.speed) +
             L", \"hideStyle\": " + Quote(ToString(animation.hideStyle)) +
             L", \"launchStyle\": " + Quote(ToString(animation.launchStyle)) +
             L",\n                 \"magnifyFalloff\": " + Quote(ToString(animation.magnifyFalloff)) +
             L", \"easing\": " + Quote(ToString(animation.easing)) +
             L", \"labelFadeMs\": " + Num(animation.labelFadeMs) +
             L",\n                 \"reduceMotion\": " + Quote(ToString(animation.reduceMotion)) + L" }";
        for (const auto& [key, value] : extraFields) o += L",\n  " + Quote(key) + L": " + value;
        o += L"\n}\n";
        return WriteFileAtomic(FilePath(), WideToUtf8(o));
    } catch (...) {
        return false;
    }
}
