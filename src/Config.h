#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// A pinned dock entry. Exactly one of path / aumid is normally set.
struct PinnedEntry {
    std::wstring path;   // .lnk or .exe
    std::wstring aumid;  // AppUserModelID for packaged apps
};

struct ClockConfig {
    bool show = true;
    std::wstring format = L"%H:%M";
    bool showDate = false;
};

struct ThemeConfig {
    uint32_t background = 0xCC202020;       // ARGB; the bar fill without acrylic
    float cornerRadius = 16.0f;             // DIPs (with acrylic: snapped to a system radius, 0/4/8)
    // Real blur behind the bar (a DWM system backdrop on a second, bar-sized window). Falls back
    // to `background` when unsupported or when Windows has transparency effects turned off.
    bool acrylic = true;
    uint32_t acrylicBackground = 0x66202020;  // tint drawn over the blur while acrylic is active
    uint32_t labelBackground = 0xE6303030;  // hover name label
    uint32_t labelText = 0xFFFFFFFF;
    uint32_t accent = 0xFF4CC2FF;           // insertion marker while a file is dragged onto the dock
};

struct Config {
    std::vector<PinnedEntry> pinned;
    int iconSize = 48;
    double magnification = 1.5;  // hovered icon scale; 1.0 disables magnification (1.0 - 2.5)
    int magnifyRange = 2;        // falloff radius in icon slots (1 - 4)
    bool autoHide = true;
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
    ThemeConfig theme;
    // Top-level keys this version does not know, kept verbatim (compact JSON) so a save does not
    // drop them. Unknown keys inside known objects (pinned entries, clock, theme) are not kept.
    std::vector<std::pair<std::wstring, std::wstring>> extraFields;

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
