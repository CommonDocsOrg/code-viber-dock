#include "Theme.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <iterator>

#include "JsonUtil.h"

namespace Theme {

using namespace JsonUtil;

namespace {

#define WD_FIELD(f, member, label, kind, mn, mx) \
    FieldInfo { Field::f, L"" #member, label, Kind::kind, offsetof(ThemeConfig, member), mn, mx }
// A Float shown as round(value * scale) `unit` in the settings UI.
#define WD_FLOAT(f, member, label, mn, mx, unitText, unitScale)                                                 \
    FieldInfo {                                                                                                  \
        Field::f, L"" #member, label, Kind::Float, offsetof(ThemeConfig, member), mn, mx, nullptr, nullptr, 0,   \
            unitText, unitScale                                                                                  \
    }
// An Enum over the arrays `names` (JSON) and `choices` (UI); the index range is 0 - count - 1.
#define WD_ENUM(f, member, label, names, choices)                                             \
    FieldInfo {                                                                                \
        Field::f, L"" #member, label, Kind::Enum, offsetof(ThemeConfig, member), 0.0f,         \
            static_cast<float>(std::size(names) - 1), names, choices, std::size(names)         \
    }

// GlowMode order.
const wchar_t* const kGlowNames[] = {L"off", L"hover", L"active", L"always"};
const wchar_t* const kGlowChoices[] = {L"Off", L"While hovered", L"Active app", L"Always"};
static_assert(std::size(kGlowNames) == std::size(kGlowChoices), "one UI choice per name");
// SlotStyle order (round 11 phase 11A).
const wchar_t* const kSlotStyleNames[] = {L"none", L"buttons"};
const wchar_t* const kSlotStyleChoices[] = {L"None (indicator dots)", L"Buttons (taskbar style)"};
static_assert(std::size(kSlotStyleNames) == std::size(kSlotStyleChoices), "one UI choice per name");

const FieldInfo kFields[] = {
    WD_FIELD(Background, background, L"Background", Color, 0.0f, 0.0f),
    WD_FIELD(CornerRadius, cornerRadius, L"Corner radius", Float, 0.0f, 64.0f),
    WD_FIELD(Acrylic, acrylic, L"Acrylic blur", Bool, 0.0f, 0.0f),
    WD_FIELD(AcrylicBackground, acrylicBackground, L"Acrylic tint", Color, 0.0f, 0.0f),
    WD_FIELD(LabelBackground, labelBackground, L"Label background", Color, 0.0f, 0.0f),
    WD_FIELD(LabelText, labelText, L"Label text", Color, 0.0f, 0.0f),
    WD_FIELD(LabelBorder, labelBorder, L"Label border", Color, 0.0f, 0.0f),
    WD_FIELD(LabelCornerRadius, labelCornerRadius, L"Label corner radius", Float, 0.0f, 32.0f),
    WD_FIELD(Accent, accent, L"Accent", Color, 0.0f, 0.0f),
    WD_FIELD(BarBorder, barBorder, L"Bar border", Color, 0.0f, 0.0f),
    WD_FIELD(Separator, separator, L"Separator", Color, 0.0f, 0.0f),
    WD_FIELD(DotRunning, dotRunning, L"Running dot", Color, 0.0f, 0.0f),
    WD_FIELD(DotActive, dotActive, L"Active dot", Color, 0.0f, 0.0f),
    WD_FIELD(DotAttention, dotAttention, L"Attention dot", Color, 0.0f, 0.0f),
    WD_FIELD(ClockText, clockText, L"Clock text", Color, 0.0f, 0.0f),
    WD_FIELD(ClockDateText, clockDateText, L"Clock date text", Color, 0.0f, 0.0f),
    WD_FIELD(PlaceholderFill, placeholderFill, L"Placeholder icon", Color, 0.0f, 0.0f),
    WD_FIELD(PlaceholderText, placeholderText, L"Placeholder letter", Color, 0.0f, 0.0f),
    WD_FIELD(PreviewHotBackground, previewHotBackground, L"Preview hover background", Color, 0.0f, 0.0f),
    WD_FIELD(PreviewHotBorder, previewHotBorder, L"Preview hover border", Color, 0.0f, 0.0f),
    WD_FIELD(CloseHover, closeHover, L"Close button hover", Color, 0.0f, 0.0f),
    WD_FIELD(Shelf, shelf, L"Glass shelf", Bool, 0.0f, 0.0f),
    WD_FIELD(ShelfSlant, shelfSlant, L"Shelf slant", Float, 0.0f, 64.0f),
    WD_FIELD(ShelfEdge, shelfEdge, L"Shelf front edge", Color, 0.0f, 0.0f),
    WD_ENUM(Glow, glow, L"Glow", kGlowNames, kGlowChoices),
    WD_FIELD(GlowColor, glowColor, L"Glow colour", Color, 0.0f, 0.0f),
    WD_FLOAT(GlowRadius, glowRadius, L"Glow radius", 4.0f, 32.0f, L"DIPs", 1.0f),
    WD_FLOAT(GlowStrength, glowStrength, L"Glow strength", 0.0f, 1.0f, L"%", 100.0f),
    WD_FIELD(Reflection, reflection, L"Reflection", Bool, 0.0f, 0.0f),
    WD_FLOAT(ReflectionHeight, reflectionHeight, L"Reflection height", 0.2f, 1.0f, L"% of icon", 100.0f),
    WD_FLOAT(ReflectionOpacity, reflectionOpacity, L"Reflection opacity", 0.0f, 1.0f, L"%", 100.0f),
    WD_FIELD(Perspective, perspective, L"3D perspective", Bool, 0.0f, 0.0f),
    WD_FLOAT(PerspectiveAngle, perspectiveAngle, L"Perspective angle", 0.0f, 35.0f, L"degrees", 1.0f),
    WD_FIELD(ReflectionClip, reflectionClip, L"Clip the reflection to the bar", Bool, 0.0f, 0.0f),
    WD_FIELD(BadgeFill, badgeFill, L"Badge", Color, 0.0f, 0.0f),
    WD_FIELD(BadgeText, badgeText, L"Badge count", Color, 0.0f, 0.0f),
    WD_FLOAT(BadgeSize, badgeSize, L"Badge size", 0.2f, 0.6f, L"% of icon", 100.0f),
    // Round 11 phase 11A.
    WD_ENUM(SlotStyle, slotStyle, L"Slot style", kSlotStyleNames, kSlotStyleChoices),
    WD_FIELD(SlotRunning, slotRunning, L"Running app", Color, 0.0f, 0.0f),
    WD_FIELD(SlotActive, slotActive, L"Active app", Color, 0.0f, 0.0f),
    WD_FIELD(SlotAttention, slotAttention, L"Attention", Color, 0.0f, 0.0f),
    WD_FIELD(SlotHover, slotHover, L"Hover", Color, 0.0f, 0.0f),
    WD_FIELD(SlotBorder, slotBorder, L"Button border", Color, 0.0f, 0.0f),
    WD_FLOAT(SlotCornerRadius, slotCornerRadius, L"Button corner radius", 0.0f, 16.0f, L"DIPs", 1.0f),
    WD_FIELD(SlotHotTrack, slotHotTrack, L"Light under the pointer (hot tracking)", Bool, 0.0f, 0.0f),
    WD_FLOAT(Sheen, sheen, L"Sheen", 0.0f, 1.0f, L"%", 100.0f),
    WD_FLOAT(IconGap, iconGap, L"Gap between icons", 0.0f, 48.0f, L"DIPs", 1.0f),
    WD_FLOAT(BarPadding, barPadding, L"Bar padding", 0.0f, 24.0f, L"DIPs", 1.0f),
};

#undef WD_FIELD
#undef WD_FLOAT
#undef WD_ENUM

static_assert(std::size(kFields) == static_cast<size_t>(Field::Count), "one table entry per field");

template <typename T>
T& At(ThemeConfig& t, const FieldInfo& f) {
    return *reinterpret_cast<T*>(reinterpret_cast<unsigned char*>(&t) + f.offset);
}

template <typename T>
const T& At(const ThemeConfig& t, const FieldInfo& f) {
    return *reinterpret_cast<const T*>(reinterpret_cast<const unsigned char*>(&t) + f.offset);
}

bool EqualsNoCase(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    if (a.empty()) return true;
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}

// A theme id must be usable as a file stem inside the themes folder.
bool ValidId(std::wstring_view id) {
    if (id.empty() || id.size() > 64 || id == L"." || id == L"..") return false;
    return std::none_of(id.begin(), id.end(),
                        [](wchar_t c) { return c < 32 || std::wcschr(L"\\/:*?\"<>|", c) != nullptr; });
}

ThemeConfig MakeLight() {
    ThemeConfig t;
    t.background = 0xD9F3F3F3;
    t.acrylic = true;
    t.acrylicBackground = 0x80F7F7F7;
    t.cornerRadius = 16.0f;
    t.labelBackground = 0xF0FFFFFF;
    t.labelText = 0xFF1B1B1B;
    t.labelBorder = 0x1F000000;
    t.accent = 0xFF0067C0;
    t.barBorder = 0x1F000000;
    t.separator = 0x33000000;
    t.dotRunning = 0x99000000;
    t.dotActive = 0xFF1B1B1B;
    t.dotAttention = 0xFFD9780A;
    t.badgeFill = 0xFFE5342A;
    t.badgeText = 0xFFFFFFFF;
    t.clockText = 0xF21B1B1B;
    t.clockDateText = 0xBF1B1B1B;
    t.placeholderFill = 0xFF8A93A6;
    t.placeholderText = 0xFFFFFFFF;
    t.previewHotBackground = 0x14000000;
    t.previewHotBorder = 0x29000000;
    t.closeHover = 0xFFC42B1C;
    return t;
}

ThemeConfig MakeGlass() {  // fields not set here are dark's
    ThemeConfig t;
    t.background = 0x59202020;
    t.acrylic = true;
    t.acrylicBackground = 0x1A202020;
    t.cornerRadius = 16.0f;
    t.labelBackground = 0xCC202020;
    t.labelText = 0xFFFFFFFF;
    t.labelBorder = 0x33FFFFFF;
    t.accent = 0xFF4CC2FF;
    t.barBorder = 0x33FFFFFF;
    t.separator = 0x40FFFFFF;
    return t;
}

ThemeConfig MakeSolid() {  // fields not set here are dark's
    ThemeConfig t;
    t.background = 0xFF202020;
    t.acrylic = false;
    t.acrylicBackground = 0x66202020;
    t.cornerRadius = 12.0f;
    t.labelBackground = 0xFF2B2B2B;
    t.labelText = 0xFFFFFFFF;
    t.labelBorder = 0x1FFFFFFF;
    t.accent = 0xFF4CC2FF;
    t.barBorder = 0x2EFFFFFF;
    t.separator = 0x38FFFFFF;
    return t;
}

ThemeConfig MakeShelf() {  // fields not set here are dark's
    ThemeConfig t;
    t.shelf = true;
    t.shelfSlant = 14.0f;
    t.background = 0x99BEE1D7;  // the shelf's top surface: pale sea-green glass
    t.shelfEdge = 0xE6A0CDBE;
    t.barBorder = 0xB3FFFFFF;   // light catching the back edge and the lip
    t.acrylic = false;
    t.cornerRadius = 3.0f;
    t.separator = 0x66FFFFFF;
    t.dotRunning = 0xB3FFFFFF;
    t.accent = 0xFF4CC2FF;
    return t;
}

ThemeConfig MakeLeopard() {  // "3D shelf": the glass shelf with reflections, a hover glow and a tilt
    ThemeConfig t = MakeShelf();
    t.background = 0x8CC8D2DC;  // cool grey glass: the coloured reflections read on it
    t.shelfEdge = 0xE6AAB4C3;
    t.barBorder = 0xCCFFFFFF;
    t.separator = 0x73FFFFFF;
    t.dotRunning = 0xCCFFFFFF;
    t.dotActive = 0xFFFFFFFF;
    t.shelfSlant = 18.0f;
    t.reflection = true;
    t.reflectionHeight = 0.5f;
    t.reflectionOpacity = 0.35f;
    t.glow = static_cast<int>(GlowMode::Hover);
    t.glowColor = 0x00000000;  // each icon's own colour
    t.glowRadius = 14.0f;
    t.glowStrength = 0.6f;
    t.perspective = true;
    t.perspectiveAngle = 18.0f;
    return t;
}

ThemeConfig MakeContrast() {
    ThemeConfig t;
    t.background = 0xFF000000;
    t.acrylic = false;
    t.acrylicBackground = 0x66202020;
    t.cornerRadius = 0.0f;
    t.labelBackground = 0xFF000000;
    t.labelText = 0xFFFFFFFF;
    t.labelBorder = 0xFFFFFFFF;
    t.accent = 0xFF1AEBFF;
    t.barBorder = 0xFFFFFFFF;
    t.separator = 0xFFFFFFFF;
    t.dotRunning = 0xFFB0B0B0;
    t.dotActive = 0xFFFFFFFF;
    t.dotAttention = 0xFFFFD400;
    t.badgeFill = 0xFFFFD400;
    t.badgeText = 0xFF000000;
    t.clockText = 0xFFFFFFFF;
    t.clockDateText = 0xFFFFFFFF;
    t.placeholderFill = 0xFF000000;
    t.placeholderText = 0xFFFFFFFF;
    t.previewHotBackground = 0x40FFFFFF;
    t.previewHotBorder = 0xFFFFFFFF;
    t.closeHover = 0xFFC42B1C;
    return t;
}

// Round 11 phase 11A: "Aero glass", the Windows 7 taskbar: a blue-grey ("Sky") tint over the blur,
// square corners, a sheen, taskbar buttons instead of dots, wide slots on a 40 DIP bar (32 DIP
// icons + 2 x 4), light labels. Fields not set here are dark's (dots, badges, the accent).
ThemeConfig MakeAero() {
    ThemeConfig t;
    t.acrylic = true;
    t.acrylicBackground = 0xB8587BA8;  // the "Sky" tint over the blur
    t.background = 0xF0496A94;         // without blur (transparency effects off)
    t.cornerRadius = 0.0f;
    t.barBorder = 0x50FFFFFF;
    t.separator = 0x40FFFFFF;
    t.sheen = 0.6f;
    t.slotStyle = static_cast<int>(SlotStyle::Buttons);
    t.iconGap = 28.0f;
    t.barPadding = 4.0f;
    t.labelBackground = 0xF2FFFFFF;
    t.labelText = 0xFF000000;
    t.labelBorder = 0xFF767676;
    t.labelCornerRadius = 3.0f;
    t.clockText = 0xFFFFFFFF;
    t.clockDateText = 0xFFFFFFFF;
    t.previewHotBackground = 0x2E5B8DD0;
    t.previewHotBorder = 0x80FFFFFF;
    t.glow = static_cast<int>(GlowMode::Off);
    t.reflection = false;
    t.perspective = false;
    return t;
}

void Log(const std::wstring& text) { OutputDebugStringW((L"WinDock: " + text + L"\n").c_str()); }

}  // namespace

const FieldInfo* Fields() { return kFields; }
size_t FieldCount() { return std::size(kFields); }
const FieldInfo& Info(Field f) { return kFields[static_cast<size_t>(f)]; }

void CopyField(const FieldInfo& f, const ThemeConfig& from, ThemeConfig& to) {
    switch (f.kind) {
        case Kind::Color: At<uint32_t>(to, f) = At<uint32_t>(from, f); break;
        case Kind::Float: At<float>(to, f) = At<float>(from, f); break;
        case Kind::Bool: At<bool>(to, f) = At<bool>(from, f); break;
        case Kind::Enum: At<int>(to, f) = At<int>(from, f); break;
    }
}

bool FieldEquals(const FieldInfo& f, const ThemeConfig& a, const ThemeConfig& b) {
    switch (f.kind) {
        case Kind::Color: return At<uint32_t>(a, f) == At<uint32_t>(b, f);
        case Kind::Float: return At<float>(a, f) == At<float>(b, f);
        case Kind::Bool: return At<bool>(a, f) == At<bool>(b, f);
        case Kind::Enum: return At<int>(a, f) == At<int>(b, f);
    }
    return false;
}

uint32_t GetColor(const ThemeConfig& t, const FieldInfo& f) { return f.kind == Kind::Color ? At<uint32_t>(t, f) : 0; }
void SetColor(ThemeConfig& t, const FieldInfo& f, uint32_t argb) {
    if (f.kind == Kind::Color) At<uint32_t>(t, f) = argb;
}
float GetFloat(const ThemeConfig& t, const FieldInfo& f) { return f.kind == Kind::Float ? At<float>(t, f) : 0.0f; }
void SetFloat(ThemeConfig& t, const FieldInfo& f, float v) {
    if (f.kind == Kind::Float && std::isfinite(v)) At<float>(t, f) = std::clamp(v, f.min, f.max);
}
bool GetBool(const ThemeConfig& t, const FieldInfo& f) { return f.kind == Kind::Bool && At<bool>(t, f); }
void SetBool(ThemeConfig& t, const FieldInfo& f, bool v) {
    if (f.kind == Kind::Bool) At<bool>(t, f) = v;
}
int GetInt(const ThemeConfig& t, const FieldInfo& f) { return f.kind == Kind::Enum ? At<int>(t, f) : 0; }
void SetInt(ThemeConfig& t, const FieldInfo& f, int v) {
    if (f.kind == Kind::Enum) At<int>(t, f) = std::clamp(v, static_cast<int>(f.min), static_cast<int>(f.max));
}

const Preset* BuiltIns(size_t& count) {
    static const Preset kPresets[] = {
        {L"dark", L"Dark", ThemeConfig{}},
        {L"light", L"Light", MakeLight()},
        {L"glass", L"Glass", MakeGlass()},
        {L"solid", L"Solid", MakeSolid()},
        {L"shelf", L"Glass shelf", MakeShelf()},
        {L"leopard", L"3D shelf", MakeLeopard()},
        {L"contrast", L"High contrast", MakeContrast()},
        {L"aero", L"Aero glass", MakeAero()},  // round 11 phase 11A
    };
    count = std::size(kPresets);
    return kPresets;
}

const Preset* FindBuiltIn(std::wstring_view id) {
    size_t count = 0;
    const Preset* presets = BuiltIns(count);
    for (size_t i = 0; i < count; ++i) {
        if (EqualsNoCase(id, presets[i].id)) return &presets[i];
    }
    return nullptr;
}

std::wstring Directory() { return Config::Directory() + L"\\themes"; }

std::vector<UserTheme> ListUserThemes() {
    std::vector<UserTheme> out;
    const std::wstring dir = Directory();
    WIN32_FIND_DATAW fd{};
    HANDLE find =
        FindFirstFileExW((dir + L"\\*.json").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (find == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring file = fd.cFileName;
        const size_t dot = file.rfind(L'.');
        // "*.json" also matches 8.3 short names: check the real extension.
        if (dot == std::wstring::npos || !EqualsNoCase(std::wstring_view(file).substr(dot), L".json")) continue;
        UserTheme t;
        t.id = file.substr(0, dot);
        if (!ValidId(t.id)) continue;
        if (FindBuiltIn(t.id)) {
            Log(L"theme file " + file + L" ignored: its name is a built-in preset id");
            continue;
        }
        t.path = dir + L"\\" + file;
        t.name = t.id;
        json::JsonObject root{nullptr};
        t.valid = ReadObjectFile(t.path, root);
        if (t.valid) {
            try {
                ReadString(root, L"name", t.name);
            } catch (...) {
            }
            if (t.name.empty()) t.name = t.id;
        }
        out.push_back(std::move(t));
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    std::sort(out.begin(), out.end(), [](const UserTheme& a, const UserTheme& b) {
        return CompareStringOrdinal(a.name.c_str(), -1, b.name.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    return out;
}

bool LoadUserTheme(const std::wstring& path, ThemeConfig& out, std::wstring* name) {
    json::JsonObject root{nullptr};
    if (!ReadObjectFile(path, root)) {
        Log(L"theme file " + path + L" is missing, unreadable or malformed");
        return false;
    }
    try {
        std::wstring base;
        ReadString(root, L"base", base);
        const Preset* preset = FindBuiltIn(base.empty() ? std::wstring_view(L"dark") : std::wstring_view(base));
        if (!preset) {
            Log(L"theme file " + path + L": unknown base \"" + base + L"\", using dark");
            preset = FindBuiltIn(L"dark");
        }
        ThemeConfig t = preset->values;
        ReadFields(root, t, nullptr);
        if (name) {
            std::wstring n;
            ReadString(root, L"name", n);
            *name = std::move(n);
        }
        out = t;
        return true;
    } catch (...) {
        Log(L"theme file " + path + L" could not be read");
        return false;
    }
}

ThemeConfig Resolve(std::wstring_view id, bool& ok) {
    ok = true;
    if (id.empty()) return ThemeConfig{};
    if (const Preset* preset = FindBuiltIn(id)) return preset->values;
    ThemeConfig t;
    if (ValidId(id) && LoadUserTheme(Directory() + L"\\" + std::wstring(id) + L".json", t, nullptr)) return t;
    Log(L"theme \"" + std::wstring(id) + L"\" not found or invalid; using dark");
    ok = false;
    return FindBuiltIn(L"dark")->values;
}

void ReadFields(const json::JsonObject& o, ThemeConfig& t, uint64_t* maskOut) {
    for (const FieldInfo& f : kFields) {
        auto j = Find(o, f.key);
        if (!j) continue;
        bool read = false;
        switch (f.kind) {
            case Kind::Color: {
                uint32_t argb = 0;
                if (j.ValueType() == json::JsonValueType::String &&
                    ParseArgbColor(std::wstring(j.GetString()), argb)) {
                    At<uint32_t>(t, f) = argb;
                    read = true;
                }
                break;
            }
            case Kind::Float:
                if (j.ValueType() == json::JsonValueType::Number) {
                    const double d = j.GetNumber();
                    if (std::isfinite(d)) {
                        At<float>(t, f) = std::clamp(static_cast<float>(d), f.min, f.max);
                        read = true;
                    }
                }
                break;
            case Kind::Bool:
                if (j.ValueType() == json::JsonValueType::Boolean) {
                    At<bool>(t, f) = j.GetBoolean();
                    read = true;
                }
                break;
            case Kind::Enum:
                if (j.ValueType() == json::JsonValueType::String) {
                    const std::wstring text(j.GetString());
                    for (size_t i = 0; i < f.count && !read; ++i) {
                        if (EqualsNoCase(text, f.names[i])) {
                            At<int>(t, f) = static_cast<int>(i);
                            read = true;
                        }
                    }
                    if (!read) Log(L"theme key \"" + std::wstring(f.key) + L"\": unknown value \"" + text + L"\"");
                }
                break;
        }
        if (read && maskOut) *maskOut |= Bit(f.field);
    }
}

std::wstring WriteFields(const ThemeConfig& t, uint64_t mask, const wchar_t* indent) {
    std::wstring o;
    for (const FieldInfo& f : kFields) {
        if (!(mask & Bit(f.field))) continue;
        o += L",\n";
        o += indent;
        o += L"\"";
        o += f.key;
        o += L"\": ";
        switch (f.kind) {
            case Kind::Color: o += Quote(FormatArgbColor(At<uint32_t>(t, f))); break;
            case Kind::Float: o += Num(At<float>(t, f)); break;
            case Kind::Bool: o += JsonUtil::Bool(At<bool>(t, f)); break;
            case Kind::Enum: {
                const int v = At<int>(t, f);
                o += Quote(f.names[v >= 0 && static_cast<size_t>(v) < f.count ? static_cast<size_t>(v) : 0]);
                break;
            }
        }
    }
    return o;
}

void DrawShelf(ID2D1DeviceContext* dc, const D2D1_RECT_F& r, const ThemeConfig& t, float unit, float stroke,
               float separatorX, float reflectionPx, float separatorX2) {
    const float w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 2.0f || h <= 2.0f) return;
    // The shelf fills the lower part of the bar; the icons stand on its top surface. With
    // reflections it is at least as deep as they are plus its lip, so they lie on the glass.
    const float baseH = std::round(h * 0.45f);
    const float lip = std::max(2.0f, std::round(baseH * 0.28f));
    const float shelfH = std::min(h, std::max(baseH, std::round(reflectionPx) + lip));
    const float top = r.bottom - shelfH;
    const float front = r.bottom - lip;
    const float slant = std::clamp(std::round(t.shelfSlant * unit), 0.0f, std::floor(w / 2.0f) - 1.0f);
    const float radius = std::clamp(std::round(t.cornerRadius * unit), 0.0f, std::floor(lip / 2.0f));

    winrt::com_ptr<ID2D1Factory> factory;
    dc->GetFactory(factory.put());
    // Top surface: a trapezoid, narrower at the back (perspective).
    winrt::com_ptr<ID2D1PathGeometry> surface;
    winrt::com_ptr<ID2D1GeometrySink> sink;
    if (FAILED(factory->CreatePathGeometry(surface.put())) || FAILED(surface->Open(sink.put()))) return;
    sink->BeginFigure(D2D1::Point2F(r.left + slant, top), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(r.right - slant, top));
    sink->AddLine(D2D1::Point2F(r.right, front));
    sink->AddLine(D2D1::Point2F(r.left, front));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close())) return;
    // Front lip: square where it meets the surface, rounded at the bottom.
    winrt::com_ptr<ID2D1PathGeometry> edge;
    sink = nullptr;
    if (FAILED(factory->CreatePathGeometry(edge.put())) || FAILED(edge->Open(sink.put()))) return;
    sink->BeginFigure(D2D1::Point2F(r.left, front), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(r.right, front));
    sink->AddLine(D2D1::Point2F(r.right, r.bottom - radius));
    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(r.right - radius, r.bottom), D2D1::SizeF(radius, radius), 0.0f,
                                  D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    sink->AddLine(D2D1::Point2F(r.left + radius, r.bottom));
    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(r.left, r.bottom - radius), D2D1::SizeF(radius, radius), 0.0f,
                                  D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close())) return;

    // The surface fades towards the back, like glass seen at a low angle.
    const D2D1_GRADIENT_STOP stops[] = {{0.0f, ToColor(t.background, 0.55f)}, {1.0f, ToColor(t.background)}};
    winrt::com_ptr<ID2D1GradientStopCollection> stopCollection;
    winrt::com_ptr<ID2D1LinearGradientBrush> gradient;
    if (FAILED(dc->CreateGradientStopCollection(stops, 2, stopCollection.put())) ||
        FAILED(dc->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.left, top), D2D1::Point2F(r.left, front)),
            stopCollection.get(), gradient.put()))) {
        return;
    }
    dc->FillGeometry(surface.get(), gradient.get());
    winrt::com_ptr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(ToColor(t.shelfEdge), brush.put()))) return;
    dc->FillGeometry(edge.get(), brush.get());

    // Light catches the angled outline faintly, the back edge and the top of the lip fully.
    const float half = stroke / 2.0f;
    brush->SetColor(ToColor(t.barBorder, 0.5f));
    dc->DrawGeometry(surface.get(), brush.get(), stroke);
    brush->SetColor(ToColor(t.barBorder));
    dc->DrawLine(D2D1::Point2F(r.left + slant, top + half), D2D1::Point2F(r.right - slant, top + half), brush.get(),
                 stroke);
    dc->DrawLine(D2D1::Point2F(r.left, front + half), D2D1::Point2F(r.right, front + half), brush.get(), stroke);

    brush->SetColor(ToColor(t.separator));
    for (const float sx : {separatorX, separatorX2}) {
        if (sx < 0.0f) continue;
        const float x = sx + half;
        dc->DrawLine(D2D1::Point2F(x, top + 2.0f * stroke), D2D1::Point2F(x, front - stroke), brush.get(), stroke);
    }
}

void DrawSheen(ID2D1DeviceContext* dc, const D2D1_RECT_F& r, float radius, const ThemeConfig& t, float stroke) {
    const float sheen = std::clamp(t.sheen, 0.0f, 1.0f);
    const float w = r.right - r.left, h = r.bottom - r.top;
    if (!(sheen > 0.0f) || w <= 2.0f || h <= 2.0f) return;
    // The highlight: a vertical gradient over the whole panel shape (transparent below 55 %), so
    // the rounded corners clip it as they clip the fill.
    const D2D1_GRADIENT_STOP stops[] = {
        {0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, sheen * 0.35f)},
        {0.55f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f)},
    };
    winrt::com_ptr<ID2D1GradientStopCollection> stopCollection;
    winrt::com_ptr<ID2D1LinearGradientBrush> gradient;
    if (FAILED(dc->CreateGradientStopCollection(stops, 2, stopCollection.put())) ||
        FAILED(dc->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.left, r.top), D2D1::Point2F(r.left, r.bottom)),
            stopCollection.get(), gradient.put()))) {
        return;
    }
    const float rr = std::clamp(radius, 0.0f, std::min(w, h) / 2.0f);
    dc->FillRoundedRectangle(D2D1::RoundedRect(r, rr, rr), gradient.get());
    // The bright hairline at the top edge, between the corners (inside the bar's own border).
    winrt::com_ptr<ID2D1SolidColorBrush> line;
    if (FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, sheen * 0.6f), line.put()))) return;
    const float y = r.top + stroke + stroke / 2.0f;
    const float x0 = r.left + std::max(rr, stroke), x1 = r.right - std::max(rr, stroke);
    if (x1 > x0) dc->DrawLine(D2D1::Point2F(x0, y), D2D1::Point2F(x1, y), line.get(), stroke);
}

void DrawSlotButton(ID2D1DeviceContext* dc, const D2D1_RECT_F& r, const ThemeConfig& t, float unit, const SlotButton& b) {
    if (!SlotButtonVisible(b)) return;
    const float stroke = std::max(1.0f, std::round(unit));
    const int stacked = std::clamp(b.stacked, 0, 2);
    const float step = std::max(2.0f, std::round(3.0f * unit));  // the stacked outlines' offset
    // The frame leaves room for the stacked outlines at its far end (the slot does not change).
    const D2D1_RECT_F frame = D2D1::RectF(r.left, r.top, r.right - static_cast<float>(stacked) * step, r.bottom);
    const float w = frame.right - frame.left, h = frame.bottom - frame.top;
    if (w <= 2.0f * stroke || h <= 2.0f * stroke) return;
    const float radius = std::clamp(std::round(t.slotCornerRadius * unit), 0.0f, std::min(w, h) / 2.0f);
    const float half = stroke / 2.0f;
    auto rounded = [&](const D2D1_RECT_F& rc, float inset) {
        const float rad = std::max(0.0f, radius - inset);
        return D2D1::RoundedRect(D2D1::RectF(rc.left + inset, rc.top + inset, rc.right - inset, rc.bottom - inset), rad, rad);
    };
    winrt::com_ptr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(ToColor(t.slotBorder), brush.put()))) return;

    // Stacked windows: the frame's outline repeated behind it, farthest first, each clipped to
    // the part beyond the one in front (a translucent fill must not show it through).
    if (b.state != 0) {
        for (int k = stacked; k >= 1; --k) {
            const float dx = static_cast<float>(k) * step;
            const D2D1_RECT_F o = D2D1::RectF(frame.left + dx, frame.top, frame.right + dx, frame.bottom);
            const float frontRight = o.right - step;  // the outline (or frame) in front of this one
            dc->PushAxisAlignedClip(D2D1::RectF(frontRight - radius, r.top, r.right, r.bottom), D2D1_ANTIALIAS_MODE_ALIASED);
            brush->SetColor(ToColor(t.slotBorder, k == 1 ? 0.8f : 0.55f));
            dc->DrawRoundedRectangle(rounded(o, half), brush.get(), stroke);
            dc->PopAxisAlignedClip();
        }
    }

    // The state's fill: top to bottom, the colour to 60 % of its alpha.
    const uint32_t fill = b.state == 3 ? t.slotAttention : b.state == 2 ? t.slotActive : b.state == 1 ? t.slotRunning : 0;
    if (b.state != 0) {
        const D2D1_GRADIENT_STOP stops[] = {{0.0f, ToColor(fill)}, {1.0f, ToColor(fill, 0.6f)}};
        winrt::com_ptr<ID2D1GradientStopCollection> sc;
        winrt::com_ptr<ID2D1LinearGradientBrush> g;
        if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 2, sc.put())) &&
            SUCCEEDED(dc->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(frame.left, frame.top), D2D1::Point2F(frame.left, frame.bottom)),
                sc.get(), g.put()))) {
            dc->FillRoundedRectangle(rounded(frame, 0.0f), g.get());
        }
    }
    if (b.hovered) {
        brush->SetColor(ToColor(t.slotHover));
        dc->FillRoundedRectangle(rounded(frame, 0.0f), brush.get());
    }
    // Hot tracking: the icon's colour, 55 % at the pointer to 0 at 0.9 x the frame's length,
    // clipped to the frame by its shape.
    if (b.hot && b.hovered) {
        const float reach = std::max(1.0f, 0.9f * w);
        const D2D1_GRADIENT_STOP stops[] = {{0.0f, ToColor(b.hotColor | 0xFF000000u, 0.55f)},
                                            {1.0f, ToColor(b.hotColor | 0xFF000000u, 0.0f)}};
        winrt::com_ptr<ID2D1GradientStopCollection> sc;
        winrt::com_ptr<ID2D1RadialGradientBrush> g;
        if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 2, sc.put())) &&
            SUCCEEDED(dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(b.hotAt, D2D1::Point2F(), reach, reach),
                                                    sc.get(), g.put()))) {
            dc->FillRoundedRectangle(rounded(frame, 0.0f), g.get());
        }
    }
    if (b.pressed) {
        brush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.25f));
        dc->FillRoundedRectangle(rounded(frame, 0.0f), brush.get());
    }
    // The hairline, with an inner highlight under it (40 % of its alpha); a hover-only frame (a
    // pinned app that does not run) at half strength.
    const float strength = b.state != 0 ? 1.0f : 0.5f;
    brush->SetColor(ToColor(t.slotBorder, 0.4f * strength));
    dc->DrawRoundedRectangle(rounded(frame, stroke + half), brush.get(), stroke);
    brush->SetColor(ToColor(t.slotBorder, strength));
    dc->DrawRoundedRectangle(rounded(frame, half), brush.get(), stroke);
}

bool Export(const std::wstring& path, const ThemeConfig& t, const std::wstring& name) {
    try {
        const std::wstring o = L"{\n  \"name\": " + Quote(name) + WriteFields(t, kAllFields, L"  ") + L"\n}\n";
        return WriteFileAtomic(path, WideToUtf8(o));
    } catch (...) {
        return false;
    }
}

}  // namespace Theme
