#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// docks[].items[].kind values this version knows (case-insensitive); Unknown for anything else.
enum class ItemKind { Unknown, App, Folder, Separator, RecycleBin, Running, Clock, Weather };
ItemKind ItemKindOf(std::wstring_view kind);
const wchar_t* ToString(ItemKind v);  // the canonical JSON name ("" for Unknown)
// The kinds that are "pins" (the pinned section of the dock): app, folder, separator, recycleBin.
inline bool IsPinKind(ItemKind k) {
    return k == ItemKind::App || k == ItemKind::Folder || k == ItemKind::Separator || k == ItemKind::RecycleBin;
}

// One element of a dock, in dock order. App / folder entries set path (or aumid); the other
// kinds set neither.
struct DockItemEntry {
    // "app", "folder", "separator", "recycleBin", "running" (the unpinned running apps), "clock",
    // "weather". Kept verbatim, so a kind this version does not know survives a save (such an
    // item is not shown).
    std::wstring kind = L"app";
    std::wstring path;   // .lnk, .exe or a folder (a folder is also detected from a .lnk's target)
    std::wstring aumid;  // AppUserModelID for packaged apps
    // Custom icon: an image (.png .jpg .jpeg .bmp .gif), an .ico, or "file.exe,3" / "file.dll,-120"
    // (icon index, negative = resource id). Environment variables are expanded. Empty: the
    // default icon; a file that does not exist also keeps the default (the entry is kept as is).
    std::wstring icon;

    ItemKind Kind() const { return ItemKindOf(kind); }
    bool IsPin() const { return IsPinKind(Kind()); }
};

// "off": always shown. "always": hides when the mouse leaves. "intelligent": hides only while
// the foreground window overlaps the dock.
enum class AutoHideMode { Off, Always, Intelligent };
// A middle click on an app: "newInstance" (start another copy), "minimize" / "close" its windows,
// "none". Folders and the Recycle Bin always open in Explorer.
enum class MiddleClickAction { NewInstance, Minimize, Close, None };
// The mouse wheel over an app: "none", "cycle" (bring its next / previous window to the front).
enum class WheelAction { None, Cycle };

// One dock: its contents and the settings that are per dock. Config::docks always holds at least
// one (docks[0], Config::Main()).
struct DockConfig {
    std::wstring id = L"main";
    // Defaults (first run, a malformed file, a dock object without "items"): the running apps and
    // the clock. The first run also seeds pins in front of them.
    std::vector<DockItemEntry> items = {DockItemEntry{L"running"}, DockItemEntry{L"clock"}};
    std::wstring edge = L"bottom";  // read and written; only "bottom" is implemented (Phase 2)
    // No drag reorder, no pinning by file drop, no Add separator / Add widget from the dock menu.
    // The settings window still edits everything.
    bool locked = false;
    int iconSize = 48;
    double magnification = 1.5;  // hovered icon scale; 1.0 disables magnification (1.0 - 2.5)
    int magnifyRange = 2;        // falloff radius in icon slots (1 - 4)
    AutoHideMode autoHide = AutoHideMode::Always;
    int hideDelayMs = 400;
    int revealDelayMs = 150;
    // Hover popup with live window thumbnails for apps with two or more windows (with
    // previewSingle: one or more).
    bool previews = true;
    bool previewSingle = false;  // "previewsSingleWindow"
    int previewDelayMs = 600;
    int monitor = 0;
    MiddleClickAction middleClick = MiddleClickAction::NewInstance;
    WheelAction wheel = WheelAction::None;

    bool AutoHideOn() const { return autoHide != AutoHideMode::Off; }
    // An item of this kind is in `items` (the clock / weather widget is shown).
    bool Has(ItemKind kind) const;
    // Adds an item of `kind` at the end when there is none (on), or removes every item of that
    // kind (off). Returns true if `items` changed.
    bool Show(ItemKind kind, bool on);
    // Adds a clock, weather or running item (Add widget): the clock at the end, the weather
    // before a clock that ends the list (else at the end), the running apps after the last pin
    // (no pins: first). A second `running` item is refused (only the first one expands). Returns
    // the new item's index, or SIZE_MAX if nothing was added.
    size_t AddWidget(ItemKind kind);
    // Moves item `from` so it comes right before the item now at `to` (to >= items.size(): to the
    // end). Returns true if the order changed.
    bool MoveItem(size_t from, size_t to);

    // The pins (IsPin() items) in order, and their indexes into `items`. A "pin index" k below
    // is the position among the pins, which is what the dock's pinned section and its resolved
    // pins (DockWindow::Pins()) are parallel to.
    std::vector<size_t> PinIndexes() const;
    size_t PinCount() const;
    std::vector<DockItemEntry> Pins() const;
    DockItemEntry& PinAt(size_t k);  // k < PinCount()
    // Inserts `e` so it becomes pin k (k >= PinCount(): after the last pin; without any pin,
    // before the first `running` item, else at the end).
    void InsertPin(size_t k, DockItemEntry e);
    void ErasePin(size_t k);
    // Replaces the pins with `pins`: pin slots are refilled in order, missing ones are removed,
    // extra pins go after the last pin (as InsertPin does). The other items keep their places.
    void SetPins(std::vector<DockItemEntry> pins);
};

// What a click on the clock does: "calendar" (the month calendar popup), "launch" (open
// clock.clickTarget), "none".
enum class ClockClick { Calendar, Launch, None };

struct ClockConfig {
    // Shown when the dock has a "clock" item (DockConfig::Has(ItemKind::Clock)).
    std::wstring format = L"%H:%M";
    // Adds ":%S" after the minutes of `format` (Clock::EffectiveFormat); a format that shows
    // seconds already is used as it is.
    bool showSeconds = false;
    bool showDate = false;
    // Without a "click" key, the older "calendarPopup": false reads as "none".
    ClockClick click = ClockClick::Calendar;
    // An app / file path or a URL, opened by ShellExecute ("%VAR%" expanded; a quoted path may be
    // followed by arguments). Used by click "launch" and by the calendar popup's footer button.
    std::wstring clickTarget;
};

// Temperatures in °C and wind in km/h ("metric") or °F and mph ("imperial").
enum class Units { Metric, Imperial };

// The weather widget (Open-Meteo, no API key). Unconfigured (latitude and longitude both 0) it
// shows a "set location" glyph instead of the conditions.
// Shown when the dock has a "weather" item (DockConfig::Has(ItemKind::Weather)).
struct WeatherConfig {
    double latitude = 0.0;   // -90 - 90
    double longitude = 0.0;  // -180 - 180
    std::wstring location;   // display name (header of the popup); not used to look anything up
    Units units = Units::Metric;  // without a "units" key: from the locale (LOCALE_IMEASURE)
    int refreshMinutes = 30;  // 5 - 360
    bool Configured() const { return latitude != 0.0 || longitude != 0.0; }
};
// The user's locale measurement system (LOCALE_IMEASURE: 1 = U.S.) as Units.
Units LocaleUnits();

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
const wchar_t* ToString(MiddleClickAction v);
const wchar_t* ToString(WheelAction v);
const wchar_t* ToString(ClockClick v);
bool FromString(std::wstring_view s, MiddleClickAction& v);
bool FromString(std::wstring_view s, WheelAction& v);
bool FromString(std::wstring_view s, ClockClick& v);
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
    // Icon effects, baked into each icon's surfaces when it is drawn (magnification, bounce and
    // drags only move them). Glow: a soft halo behind the icon (GlowMode as int: off / hover /
    // active app / always).
    int glow = 0;
    uint32_t glowColor = 0x00000000;        // alpha 0: the icon's dominant colour
    float glowRadius = 14.0f;               // DIPs, 4 - 32 (blur radius)
    float glowStrength = 0.6f;              // 0 - 1, halo opacity
    // A mirrored, fading copy of each icon under it (the bar grows by its height).
    bool reflection = false;
    float reflectionHeight = 0.5f;          // 0.2 - 1, fraction of the icon height
    float reflectionOpacity = 0.35f;        // 0 - 1, at the icon's foot (fading to 0)
    // The icon row and the bar tilted back about the bar's bottom edge (cosmetic: hit-testing uses
    // the untilted layout). Acrylic is not used with it.
    bool perspective = false;
    float perspectiveAngle = 18.0f;         // degrees, 0 - 35
};

// ThemeConfig::glow (the "glow" key: "off", "hover", "active", "always").
enum class GlowMode { Off = 0, Hover = 1, Active = 2, Always = 3 };
inline GlowMode GlowOf(const ThemeConfig& t) {
    return t.glow >= 0 && t.glow <= 3 ? static_cast<GlowMode>(t.glow) : GlowMode::Off;
}

struct Config {
    // At least one dock after LoadFile / Sanitize. Only docks[0] is shown (multiple docks: Phase 3).
    std::vector<DockConfig> docks = std::vector<DockConfig>(1);
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
    uint64_t themeOverrides = 0;  // one bit per Theme::Field set inline (only those are saved)
    ThemeConfig theme;            // RESOLVED values: what the dock draws with
    bool themeResolveFailed = false;  // the last ResolveTheme() fell back to dark (not saved)
    AnimationConfig animation;
    // Top-level keys this version does not know, kept verbatim (compact JSON) so a save does not
    // drop them. Unknown keys inside known objects (docks, items, clock, theme, animation, weather)
    // are not kept, nor are the legacy pre-`docks` keys.
    std::vector<std::pair<std::wstring, std::wstring>> extraFields;
    // The last LoadFile() built `docks` from the legacy keys (pinned, iconSize, clock.show ...):
    // the caller saves, so the file is rewritten in the current format. Not saved.
    bool migrated = false;

    DockConfig& Main() { return docks.front(); }
    const DockConfig& Main() const { return docks.front(); }

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
