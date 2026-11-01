#pragma once

#include <cstdint>
#include <string>
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
    uint32_t background = 0xCC202020;  // ARGB
    float cornerRadius = 16.0f;        // DIPs
};

struct Config {
    std::vector<PinnedEntry> pinned;
    int iconSize = 48;
    double magnification = 1.5;
    int magnifyRange = 2;
    bool autoHide = true;
    int hideDelayMs = 400;
    int revealDelayMs = 150;
    int monitor = 0;
    bool hideTaskbar = true;
    bool startWithWindows = false;
    // "auto": hardware D3D11 device, WARP if that fails. "software": WARP only, which avoids the
    // GPU user-mode driver's per-process memory (the dock redraws rarely, so WARP is cheap).
    std::wstring renderer = L"auto";
    ClockConfig clock;
    ThemeConfig theme;

    // %LocalAppData%\WinDock
    static std::wstring Directory();
    // %LocalAppData%\WinDock\config.json
    static std::wstring FilePath();

    // Loads the config file. Missing file: writes defaults. Malformed file: keeps defaults
    // (the file is left untouched so the user can fix it). Never throws.
    void Load();
    // Writes the config file atomically (temp file + rename). Never throws.
    bool Save() const;

    // Clamp values to sane ranges.
    void Sanitize();
};

// "#AARRGGBB" or "#RRGGBB" -> ARGB. Returns false on parse failure.
bool ParseArgbColor(const std::wstring& text, uint32_t& argb);
std::wstring FormatArgbColor(uint32_t argb);
