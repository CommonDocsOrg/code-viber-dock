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
    PerspectiveAngle, ReflectionClip,
    BadgeFill, BadgeText, BadgeSize,
    // Round 11 phase 11A (appended: the override mask is one bit per field, ids never renumbered).
    SlotStyle, SlotRunning, SlotActive, SlotAttention, SlotHover, SlotBorder, SlotCornerRadius, SlotHotTrack,
    Sheen, IconGap, BarPadding,
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
    const wchar_t* id;    // "dark", "light", "glass", "solid", "shelf", "leopard", "contrast", "aero"
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

// --- Round 12 phase 12B: Quick look, the "derived fields" table ---
// Five controls of App settings > Themes and of a dock's Look tab that write through to the raw
// fields. This table is the one place that knows the mapping (guide-settings "Quick look" tells it
// in words):
//  Tint (0xRRGGBB): `background`'s RGB (its alpha kept) and `acrylicBackground`'s (the bar's
//      colour while the blur is on, else Tint would do nothing there); `labelBackground`,
//      `slotRunning`, `slotActive` and `slotHover` get the tint's hue at the brightness the
//      preset gives each (their alphas kept; the attention colours stay signal colours).
//      Reads: background's RGB.
//  Transparency (0 - 100 %): `background`'s alpha = (100 - T) % of 255, and `acrylicBackground`'s
//      alpha in the preset's proportion to it (the blur's tint fades with the fill). Reads: 100 -
//      background's alpha in percent. There is no separate blur opacity field: Acrylic blur is on
//      or off (`acrylic`), and its tint's alpha is how much of the blur shows.
//  Corners (0 square, 1 soft, 2 round; -1 Mixed): `cornerRadius` 0 / 8 / 16 and `slotCornerRadius`
//      0 / 3 / 6. Reads: cornerRadius alone (any other radius is Mixed; slot corners may differ).
//  Icon effects (bit 0 glow, bit 1 reflection: 0 off, 1 glow, 2 reflection, 3 both): `glow` (on:
//      the preset's mode, else While hovered) and `reflection`.
//  Indicator dots (1 on / 0 off): the three dot colours' alpha 0, or back to the preset's alpha
//      (255 where the preset's is 0). Slot buttons draw no dots either way.
enum class Quick { Tint, Transparency, Corners, Effects, Dots, Count };
struct DerivedField {
    Quick quick;
    const wchar_t* label;
    uint64_t fields;  // the raw fields it writes (Bit(Field) ...)
    int (*get)(const ThemeConfig& t);
    // Writes `value` into `t`; `preset` is the look's preset (what proportions and "on again" use).
    void (*set)(ThemeConfig& t, const ThemeConfig& preset, int value);
};
const DerivedField& Derived(Quick q);
// Applies `value` through `q` to a look `t` (override mask `mask`, preset values `preset`): every
// field it writes becomes an override where it now differs from the preset and stops being one
// where it equals it. True if `t` changed.
bool SetDerived(Quick q, ThemeConfig& t, uint64_t& mask, const ThemeConfig& preset, int value);
// The fields `q` writes that differ from the preset (its ↺ shows while this is not 0: the control's
// own value differs, whatever the other fields it shares a raw field with did).
bool DerivedDiffers(Quick q, const ThemeConfig& t, const ThemeConfig& preset);
// "Back to the preset's value" for `q`: the preset's value through the same setter (a Mixed
// preset's fields are copied as they are). True if `t` changed.
bool ResetDerived(Quick q, ThemeConfig& t, uint64_t& mask, const ThemeConfig& preset);

// --- Round 12 phase 12B: theme files ---
// The file stem for a theme called `name`: characters a file name cannot hold become '-', trimmed,
// at most 64 characters ("" for an empty name); a built-in id gets "my-" in front (a file named
// like a built-in is ignored by the list).
std::wstring StemFor(const std::wstring& name);
// Writes themes\<StemFor(name)>.json (as Export: the name and every field). `overwrite` false:
// ERROR_FILE_EXISTS when there is one. `stem` gets the stem. 0 on success, else a Win32 error.
DWORD SaveAs(const std::wstring& name, const ThemeConfig& t, bool overwrite, std::wstring& stem);
// Copies the theme file `path` into the themes folder once it reads as a theme (a JSON object:
// LoadUserTheme): under its own stem (StemFor), else "<stem> 2", "<stem> 3" ... when that is
// taken. A file that is in the folder already stays as it is. `stem` gets its stem. 0 on success,
// ERROR_INVALID_DATA when it is not a theme file, else a Win32 error.
DWORD Import(const std::wstring& path, std::wstring& stem);

// Draws the glass shelf (t.shelf) in `bar`: the surface trapezoid, the front lip and highlights,
// and the separator at `separatorX` (same coordinates as `bar`; < 0 = none). `unit` maps DIPs
// to pixels. `reflectionPx`: the height of the icon reflections at the bottom of the bar (the
// shelf is made at least that tall plus its lip, so the reflections lie on the glass).
// `separatorX2`: a second separator (< 0 = none), e.g. after widgets at the start of the dock.
void DrawShelf(ID2D1DeviceContext* dc, const D2D1_RECT_F& bar, const ThemeConfig& t, float unit, float stroke,
               float separatorX, float reflectionPx = 0.0f, float separatorX2 = -1.0f);

// Round 11 phase 11A: the glossy sheen (t.sheen) over a panel `bar` with corner `radius` (pixels),
// in canonical coordinates (the top is the side away from the screen edge): white at sheen x 35 %
// fading to 0 over the upper 55 %, and a `stroke` wide white line at sheen x 60 % along the top
// between the corners. No-op for sheen 0. The dock and the settings preview draw it the same way.
void DrawSheen(ID2D1DeviceContext* dc, const D2D1_RECT_F& bar, float radius, const ThemeConfig& t, float stroke);

// Round 11 phase 11A: one slot button (t.slotStyle "buttons"), drawn into `r` (canonical pixels:
// the slot inset on the main axis, the bar's cross extent inset; the top is away from the edge).
struct SlotButton {
    int state = 0;           // 0 none, 1 running, 2 active (owns the foreground), 3 attention
    bool hovered = false;    // + the hover fill (a pinned app that does not run gets only this)
    bool pressed = false;    // + black at 25 %
    int stacked = 0;         // extra outlines (0 - 2) 3 DIPs apart behind the frame, toward r.right
    bool hot = false;        // hot tracking: a radial light at hotAt in hotColor
    D2D1_POINT_2F hotAt{};
    uint32_t hotColor = 0;   // ARGB (its alpha is ignored)
};
// True when the button draws anything (a state, hovered or pressed).
inline bool SlotButtonVisible(const SlotButton& b) { return b.state != 0 || b.hovered || b.pressed; }
void DrawSlotButton(ID2D1DeviceContext* dc, const D2D1_RECT_F& r, const ThemeConfig& t, float unit, const SlotButton& b);

inline D2D1_COLOR_F ToColor(uint32_t argb, float alphaScale = 1.0f) {
    return D2D1::ColorF(static_cast<float>((argb >> 16) & 0xFF) / 255.0f,
                        static_cast<float>((argb >> 8) & 0xFF) / 255.0f, static_cast<float>(argb & 0xFF) / 255.0f,
                        static_cast<float>((argb >> 24) & 0xFF) / 255.0f * alphaScale);
}

}  // namespace Theme
