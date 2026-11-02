#pragma once

#include <Unknwn.h>
#include <windows.h>
#include <d2d1_1.h>

#include <winrt/base.h>
#include <winrt/Windows.Data.Json.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Config.h"

// Theme presets, user theme files and the ThemeConfig field table.
//
// Layering: built-in preset -> user theme file (%LocalAppData%\WinDock\themes\<id>.json, with a
// built-in `base`) -> fields set inline in config.json (Config::themeOverrides).
namespace Theme {

namespace json = winrt::Windows::Data::Json;

enum class Field : uint32_t {
    Background, CornerRadius, Acrylic, AcrylicBackground, LabelBackground, LabelText, LabelBorder,
    LabelCornerRadius, Accent, BarBorder, Separator, DotRunning, DotActive, DotAttention, ClockText,
    ClockDateText, PlaceholderFill, PlaceholderText, PreviewHotBackground, PreviewHotBorder, CloseHover,
    Shelf, ShelfSlant, ShelfEdge,
    Count
};
static_assert(static_cast<uint32_t>(Field::Count) <= 32, "one override bit per field");

constexpr uint32_t Bit(Field f) { return 1u << static_cast<uint32_t>(f); }
constexpr uint32_t kAllFields = (1u << static_cast<uint32_t>(Field::Count)) - 1u;

enum class Kind { Color, Float, Bool };

struct FieldInfo {
    Field field;
    const wchar_t* key;    // JSON key
    const wchar_t* label;  // UI label
    Kind kind;
    size_t offset;         // offsetof(ThemeConfig, ...)
    float min, max;        // Kind::Float only
};

// The single source of truth for reading, writing and the settings UI, in Field order.
const FieldInfo* Fields();
size_t FieldCount();
const FieldInfo& Info(Field f);

// Field access by table entry.
void CopyField(const FieldInfo& f, const ThemeConfig& from, ThemeConfig& to);
bool FieldEquals(const FieldInfo& f, const ThemeConfig& a, const ThemeConfig& b);
// Typed access by table entry (the kind must match: Color / Float / Bool).
uint32_t GetColor(const ThemeConfig& t, const FieldInfo& f);
void SetColor(ThemeConfig& t, const FieldInfo& f, uint32_t argb);
float GetFloat(const ThemeConfig& t, const FieldInfo& f);
void SetFloat(ThemeConfig& t, const FieldInfo& f, float v);  // clamped to [min, max]
bool GetBool(const ThemeConfig& t, const FieldInfo& f);
void SetBool(ThemeConfig& t, const FieldInfo& f, bool v);

struct Preset {
    const wchar_t* id;    // "dark", "light", "glass", "solid", "contrast"
    const wchar_t* name;  // "Dark", "Light", ...
    ThemeConfig values;
};
const Preset* BuiltIns(size_t& count);
const Preset* FindBuiltIn(std::wstring_view id);  // case-insensitive; nullptr if none

// Config::Directory() + L"\themes"
std::wstring Directory();

struct UserTheme {
    std::wstring id;    // file stem
    std::wstring name;  // "name" in the file, else the stem
    std::wstring path;
    bool valid = false;  // the file parsed as a JSON object
};
// themes\*.json, skipping stems that equal a built-in id.
std::vector<UserTheme> ListUserThemes();
// Reads a theme file: its built-in `base` (dark if missing / unknown), then its fields.
bool LoadUserTheme(const std::wstring& path, ThemeConfig& out, std::wstring* name);
// Built-in first, then themes\<id>.json. "" = the dark defaults (ok). Not found / unreadable /
// malformed: ok = false and the dark preset is returned.
ThemeConfig Resolve(std::wstring_view id, bool& ok);

// Reads every field present (with a valid value) into `t`, setting its bit in *maskOut.
void ReadFields(const json::JsonObject& o, ThemeConfig& t, uint32_t* maskOut);
// `,\n<indent>"key": value` for every field in `mask`, in table order.
std::wstring WriteFields(const ThemeConfig& t, uint32_t mask, const wchar_t* indent);
// Writes { "name", every field } atomically (a portable theme file).
bool Export(const std::wstring& path, const ThemeConfig& t, const std::wstring& name);

// Draws the glass shelf (t.shelf) in `bar`: the surface trapezoid, the front lip and highlights,
// and the separator at `separatorX` (same coordinates as `bar`; < 0 = none). `unit` maps DIPs
// to pixels.
void DrawShelf(ID2D1DeviceContext* dc, const D2D1_RECT_F& bar, const ThemeConfig& t, float unit, float stroke,
               float separatorX);

inline D2D1_COLOR_F ToColor(uint32_t argb, float alphaScale = 1.0f) {
    return D2D1::ColorF(static_cast<float>((argb >> 16) & 0xFF) / 255.0f,
                        static_cast<float>((argb >> 8) & 0xFF) / 255.0f, static_cast<float>(argb & 0xFF) / 255.0f,
                        static_cast<float>((argb >> 24) & 0xFF) / 255.0f * alphaScale);
}

}  // namespace Theme
