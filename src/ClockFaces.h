#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct ThemeConfig;

// Custom faces for the dock's built-in digital clock (round 4 phase 14; docs/widgets.md "Clock
// faces"): a JSON file of `"type": "digital"`, `"format": 2` describing the lines (time / date /
// seconds), fonts, size ratios, colours (or theme colour references), an optional background pill,
// separators, the 12-hour suffix and the alignment, also on vertical docks. Shipped faces are RCDATA
// (res/clock-faces); user faces are %LocalAppData%\WinDock\clock-faces\<id>.json (a user file wins
// over a shipped face with the same id). `clock.face` "digital" (the default) is no file at all:
// the clock's own rendering, unchanged.
struct ClockFace {
    enum class Layout { Stacked, Inline, DateFirst };
    enum class Align { Center, Start, End };
    enum class Seconds { Inline, Small, Line, Hidden };
    enum class Suffix { AsIs, Small, Lower, None };
    enum class Case { AsIs, Upper, Lower };
    struct Text {
        std::wstring font = L"Segoe UI";
        int weight = 400;
        float size = 1.0f;        // a ratio of the built-in clock's size for the line (time 16 / 14 DIPs
                                  // without / with the date, date 11 DIPs; seconds / suffix: of the time)
        std::wstring color;       // "#AARRGGBB", "theme:<name>" ("" the line's default)
        Case textCase = Case::AsIs;
    };

    std::wstring id, name, description;
    bool builtIn = false;
    Layout layout = Layout::Stacked;
    Align align = Align::Center;
    bool verticalStack = true;          // vertical docks: every space breaks the line
    Align verticalAlign = Align::Center;
    Text time{L"Segoe UI", 600, 1.0f, L"theme:text"};
    Text date{L"Segoe UI", 400, 1.0f, L"theme:dateText"};
    std::wstring dateFormat = L"%a %d %b";
    std::wstring timeSeparator;         // replaces ':' in the time ("" keeps it)
    std::wstring separator = L"  ";     // between the time and the date on one line (layout inline)
    Seconds seconds = Seconds::Inline;
    Text secondsText{L"", 0, 0.6f, L""};  // font / weight "" / 0: the time's
    Suffix suffix = Suffix::AsIs;
    Text suffixText{L"", 0, 0.6f, L""};
    float lineGap = 0.0f;               // DIPs between lines (may be negative)
    bool background = false;
    std::wstring fill = L"#2EFFFFFF", border;
    float borderWidth = 1.0f, radius = 6.0f, padX = 6.0f, padY = 2.0f;  // DIPs
};

// A face's colours resolved for a theme (ARGB).
struct ClockFaceColors {
    uint32_t time = 0, date = 0, seconds = 0, suffix = 0, fill = 0, border = 0;
};

namespace ClockFaces {

// "digital": the built-in rendering (no face file).
inline constexpr wchar_t kDigital[] = L"digital";

struct Entry {
    std::wstring id;
    std::wstring name;
    bool builtIn = false;  // shipped in the exe (a user file with its id replaces it)
};

// %LocalAppData%\WinDock\clock-faces (not created).
std::wstring Directory();
// The faces to choose from: the shipped ones, then the user files (by name; a user file replaces a
// shipped face with the same id). "digital" is not listed (callers add it first).
std::vector<Entry> List();
// The face `id` (a user file first, then a shipped face). Null for "digital", or with `error` set
// when there is no such face or it cannot be read.
std::shared_ptr<const ClockFace> Load(const std::wstring& id, std::wstring& error);
// Parses a face file. False with `error` (the field and what is wrong) when it is not a digital face.
bool Parse(const std::string& utf8, ClockFace& face, std::wstring& error);
// "#AARRGGBB" / "#RRGGBB" or "theme:text" (dateText, accent, background, labelText,
// labelBackground, separator, border) -> ARGB; `fallback` for "" or anything else.
uint32_t ResolveColor(const std::wstring& spec, const ThemeConfig& theme, uint32_t fallback);
ClockFaceColors Resolve(const ClockFace& face, const ThemeConfig& theme);

}  // namespace ClockFaces
