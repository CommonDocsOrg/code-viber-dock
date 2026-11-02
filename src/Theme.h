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
    Glow, GlowColor, GlowRadius, GlowStrength, Reflection, ReflectionHeight, ReflectionOpacity, Perspective,
    PerspectiveAngle,
    Count
};
static_assert(static_cast<uint32_t>(Field::Count) <= 64, "one override bit per field");

constexpr uint64_t Bit(Field f) { return uint64_t{1} << static_cast<uint32_t>(f); }
constexpr uint64_t kAllFields = static_cast<uint32_t>(Field::Count) >= 64
                                    ? ~uint64_t{0}
                                    : (uint64_t{1} << static_cast<uint32_t>(Field::Count)) - 1u;

// Color: uint32_t ARGB; Float: float; Bool: bool; Enum: int (an index into `names`).
enum class Kind { Color, Float, Bool, Enum };

struct FieldInfo {
    Field field;
    const wchar_t* key;    // JSON key
    const wchar_t* label;  // UI label
    Kind kind;
    size_t offset;         // offsetof(ThemeConfig, ...)
    float min, max;        // Float: the range; Enum: the index range (0, count - 1)
    // Enum only: the JSON strings (read case-insensitively) and the UI choices, `count` of each.
    const wchar_t* const* names = nullptr;
    const wchar_t* const* choices = nullptr;
    size_t count = 0;
    // Float only: the settings UI shows round(value * scale) followed by `unit`.
    const wchar_t* unit = L"DIPs";
    float scale = 1.0f;
};

// The single source of truth for reading, writing and the settings UI, in Field order.
const FieldInfo* Fields();
size_t FieldCount();
const FieldInfo& Info(Field f);

// Field access by table entry.
void CopyField(const FieldInfo& f, const ThemeConfig& from, ThemeConfig& to);
bool FieldEquals(const FieldInfo& f, const ThemeConfig& a, const ThemeConfig& b);
// Typed access by table entry (the kind must match: Color / Float / Bool / Enum).
uint32_t GetColor(const ThemeConfig& t, const FieldInfo& f);
void SetColor(ThemeConfig& t, const FieldInfo& f, uint32_t argb);
float GetFloat(const ThemeConfig& t, const FieldInfo& f);
void SetFloat(ThemeConfig& t, const FieldInfo& f, float v);  // clamped to [min, max]
bool GetBool(const ThemeConfig& t, const FieldInfo& f);
void SetBool(ThemeConfig& t, const FieldInfo& f, bool v);
int GetInt(const ThemeConfig& t, const FieldInfo& f);
void SetInt(ThemeConfig& t, const FieldInfo& f, int v);  // clamped to [min, max]

struct Preset {
    const wchar_t* id;    // "dark", "light", "glass", "solid", "shelf", "leopard", "contrast"
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
// An Enum field with text that is not one of its names is skipped (the value stays as it was).
void ReadFields(const json::JsonObject& o, ThemeConfig& t, uint64_t* maskOut);
// `,\n<indent>"key": value` for every field in `mask`, in table order.
std::wstring WriteFields(const ThemeConfig& t, uint64_t mask, const wchar_t* indent);
// Writes { "name", every field } atomically (a portable theme file).
bool Export(const std::wstring& path, const ThemeConfig& t, const std::wstring& name);

// Draws the glass shelf (t.shelf) in `bar`: the surface trapezoid, the front lip and highlights,
// and the separator at `separatorX` (same coordinates as `bar`; < 0 = none). `unit` maps DIPs
// to pixels. `reflectionPx`: the height of the icon reflections at the bottom of the bar (the
// shelf is made at least that tall plus its lip, so the reflections lie on the glass).
// `separatorX2`: a second separator (< 0 = none), e.g. after widgets at the start of the dock.
void DrawShelf(ID2D1DeviceContext* dc, const D2D1_RECT_F& bar, const ThemeConfig& t, float unit, float stroke,
               float separatorX, float reflectionPx = 0.0f, float separatorX2 = -1.0f);

inline D2D1_COLOR_F ToColor(uint32_t argb, float alphaScale = 1.0f) {
    return D2D1::ColorF(static_cast<float>((argb >> 16) & 0xFF) / 255.0f,
                        static_cast<float>((argb >> 8) & 0xFF) / 255.0f, static_cast<float>(argb & 0xFF) / 255.0f,
                        static_cast<float>((argb >> 24) & 0xFF) / 255.0f * alphaScale);
}

}  // namespace Theme
