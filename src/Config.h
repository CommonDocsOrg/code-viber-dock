#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A pinned dock entry. Exactly one of path / aumid is normally set; the special kinds
// ("separator", "recycleBin") set neither.
struct PinnedEntry {
    std::wstring path;   // .lnk, .exe or a folder (folders are detected from the path, no `kind`)
    std::wstring aumid;  // AppUserModelID for packaged apps
    // Custom icon: an image (.png .jpg .jpeg .bmp .gif), an .ico, or "file.exe,3" / "file.dll,-120"
    // (icon index, negative = resource id). Environment variables are expanded. Empty: the
    // default icon; a file that does not exist also keeps the default (the entry is kept as is).
    std::wstring icon;
    // "" (an app or folder, from the path), "separator" or "recycleBin" (case-insensitive). Kept
    // verbatim, so a kind this version does not know survives a save.
    std::wstring kind;
};

// pinned[].kind values with a meaning of their own (case-insensitive); None for "" and for
// text this version does not know.
enum class SpecialPin { None, Separator, RecycleBin };
SpecialPin SpecialPinOf(std::wstring_view kind);

struct ClockConfig {
    bool show = true;
    std::wstring format = L"%H:%M";
    bool showDate = false;
    bool calendarPopup = true;  // a click on the clock opens the month calendar
};

// Temperatures in °C and wind in km/h ("metric") or °F and mph ("imperial").
enum class Units { Metric, Imperial };

// The weather widget (Open-Meteo, no API key). Unconfigured (latitude and longitude both 0) it
// shows a "set location" glyph instead of the conditions.
struct WeatherConfig {
    bool show = false;
    double latitude = 0.0;   // -90 - 90
    double longitude = 0.0;  // -180 - 180
    std::wstring location;   // display name (header of the popup); not used to look anything up
    Units units = Units::Metric;  // without a "units" key: from the locale (LOCALE_IMEASURE)
    int refreshMinutes = 30;  // 5 - 360
    bool Configured() const { return latitude != 0.0 || longitude != 0.0; }
};
// The user's locale measurement system (LOCALE_IMEASURE: 1 = U.S.) as Units.
Units LocaleUnits();

// "off": always shown. "always": hides when the mouse leaves. "intelligent": hides only while
// the foreground window overlaps the dock.
enum class AutoHideMode { Off, Always, Intelligent };
// How the dock hides: "slide" (slide + fade), "slideOnly", "fade", "none" (instant).
enum class HideStyle { Slide, SlideOnly, Fade, None };
// Feedback while an app launches: "bounce" (repeating), "hop" (one bounce), "pulse" (opacity), "none".
enum class LaunchStyle { Bounce, Hop, Pulse, None };
// Magnification falloff over `magnifyRange` slots.
enum class Falloff { Cosine, Linear };
// Settle animations (rest, slide, label). Also used by the Renderer.
enum class Easing { EaseOut, Linear, Spring };
// "system": follow Settings > Accessibility > Visual effects > Animation effects.
enum class ReduceMotion { System, On, Off };

// JSON strings for the enums above (shared by config.json, README and the settings UI).
// FromString is case-insensitive and leaves `v` unchanged (returning false) for unknown text.
const wchar_t* ToString(AutoHideMode v);
const wchar_t* ToString(HideStyle v);
const wchar_t* ToString(LaunchStyle v);
const wchar_t* ToString(Falloff v);
const wchar_t* ToString(Easing v);
const wchar_t* ToString(ReduceMotion v);
const wchar_t* ToString(Units v);
bool FromString(std::wstring_view s, AutoHideMode& v);
bool FromString(std::wstring_view s, HideStyle& v);
bool FromString(std::wstring_view s, LaunchStyle& v);
bool FromString(std::wstring_view s, Falloff& v);
bool FromString(std::wstring_view s, Easing& v);
bool FromString(std::wstring_view s, ReduceMotion& v);
bool FromString(std::wstring_view s, Units& v);

struct AnimationConfig {
    double speed = 1.0;  // 0 = animations off; otherwise 0.25 - 4.0 (durations are divided by it)
    HideStyle hideStyle = HideStyle::Slide;
    LaunchStyle launchStyle = LaunchStyle::Bounce;
    Falloff magnifyFalloff = Falloff::Cosine;
    Easing easing = Easing::EaseOut;
    int labelFadeMs = 120;  // 0 = instant; 0 - 1000
    ReduceMotion reduceMotion = ReduceMotion::System;
};

// Every field is listed in Theme::Fields() (Theme.h), which drives reading, writing and the UI.
// The defaults are the built-in "dark" preset.
struct ThemeConfig {
    uint32_t background = 0xCC202020;       // ARGB; the bar fill without acrylic
    float cornerRadius = 16.0f;             // DIPs (with acrylic: snapped to a system radius, 0/4/8)
    // Real blur behind the bar (a DWM system backdrop on a second, bar-sized window). Falls back
    // to `background` when unsupported or when Windows has transparency effects turned off.
    bool acrylic = true;
    uint32_t acrylicBackground = 0x66202020;  // tint drawn over the blur while acrylic is active
    uint32_t labelBackground = 0xE6303030;  // hover name label
    uint32_t labelText = 0xFFFFFFFF;
    uint32_t labelBorder = 0x1FFFFFFF;
    float labelCornerRadius = 7.0f;         // DIPs, 0 - 32
    uint32_t accent = 0xFF4CC2FF;           // insertion marker while a file is dragged onto the dock
    uint32_t barBorder = 0x24FFFFFF;        // hairline around the bar and the preview popup
    uint32_t separator = 0x38FFFFFF;        // between the app icons and the clock
    uint32_t dotRunning = 0x80FFFFFF;       // indicator dots
    uint32_t dotActive = 0xFFFFFFFF;        // the app owns the foreground window
    uint32_t dotAttention = 0xFFFF9E26;     // the app flashes for attention
    uint32_t clockText = 0xF2FFFFFF;
    uint32_t clockDateText = 0xB6FFFFFF;    // = clockText alpha 0.95 x 0.75, as drawn before
    uint32_t placeholderFill = 0xFF6B7385;  // icon placeholder (missing file / no icon)
    uint32_t placeholderText = 0xF2FFFFFF;
    uint32_t previewHotBackground = 0x1AFFFFFF;  // hovered preview card (half alpha: icon-only box)
    uint32_t previewHotBorder = 0x33FFFFFF;
    uint32_t closeHover = 0xFFC42B1C;       // preview card close button while hovered
    // Glass shelf: instead of a panel, the bar is drawn as a shelf in perspective under the icons
    // (top surface in `background`, sides angled in by `shelfSlant`, a front lip in `shelfEdge`,
    // highlights in `barBorder`). Acrylic is not used with it: the blur window is a rectangle.
    bool shelf = false;
    float shelfSlant = 14.0f;               // DIPs, 0 - 64
    uint32_t shelfEdge = 0xE6A0CDBE;
};

struct Config {
    std::vector<PinnedEntry> pinned;
    int iconSize = 48;
    double magnification = 1.5;  // hovered icon scale; 1.0 disables magnification (1.0 - 2.5)
    int magnifyRange = 2;        // falloff radius in icon slots (1 - 4)
    AutoHideMode autoHide = AutoHideMode::Always;
    int hideDelayMs = 400;
    int revealDelayMs = 150;
    // Hover popup with live window thumbnails for apps with two or more windows.
    bool previews = true;
    int previewDelayMs = 600;
    int monitor = 0;
    bool hideTaskbar = true;
    bool startWithWindows = false;
    // "auto": hardware D3D11 device, WARP if that fails. "software": WARP only, which avoids the
    // GPU user-mode driver's per-process memory (the dock redraws rarely, so WARP is cheap).
    std::wstring renderer = L"auto";
    ClockConfig clock;
    WeatherConfig weather;
    // Theme layering: built-in preset -> user theme file (themes\<id>.json, which names its own
    // built-in `base`) -> fields set inline in config.json. `themePreset` is a built-in id or a
    // user theme file stem; "" means none (inline fields over the dark defaults).
    std::wstring themePreset = L"dark";
    uint32_t themeOverrides = 0;  // one bit per Theme::Field set inline (only those are saved)
    ThemeConfig theme;            // RESOLVED values: what the dock draws with
    bool themeResolveFailed = false;  // the last ResolveTheme() fell back to dark (not saved)
    AnimationConfig animation;
    // Top-level keys this version does not know, kept verbatim (compact JSON) so a save does not
    // drop them. Unknown keys inside known objects (pinned entries, clock, theme, animation, weather) are
    // not kept.
    std::vector<std::pair<std::wstring, std::wstring>> extraFields;

    bool AutoHideOn() const { return autoHide != AutoHideMode::Off; }

    // Re-bases `theme` on `themePreset` for every field whose override bit is clear. Returns
    // false (and uses dark for those fields, keeping `themePreset` so the user can fix it) when
    // the preset is unknown or its theme file is unreadable / malformed.
    bool ResolveTheme();

    // %LocalAppData%\WinDock
    static std::wstring Directory();
    // %LocalAppData%\WinDock\config.json
    static std::wstring FilePath();

    enum class LoadResult { Ok, Missing, Malformed };
    // Loads the config file into *this (defaults for anything missing). Missing file: defaults.
    // Malformed file: defaults (the file is left untouched so the user can fix it). Never throws.
    LoadResult LoadFile();
    // LoadFile() == Missing, i.e. first run: the caller seeds defaults and saves.
    bool Load() { return LoadFile() == LoadResult::Missing; }
    // Writes the config file atomically (temp file + rename). Never throws.
    bool Save() const;

    // Clamp values to sane ranges.
    void Sanitize();
};

// "#AARRGGBB" or "#RRGGBB" -> ARGB. Returns false on parse failure.
bool ParseArgbColor(const std::wstring& text, uint32_t& argb);
std::wstring FormatArgbColor(uint32_t argb);
