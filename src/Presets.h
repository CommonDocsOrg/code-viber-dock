#pragma once

// Round 9 phase 9C: predefined dock setups ("presets"). Each is a JSON file shipped as RCDATA
// (res/presets/<id>.json, IDR_PRESET_*): { "id", "name", "tagline" (one line), "description" (2 - 4
// sentences), "dock": { docks[] keys }, "items": [ docks[].items entries ] }. The items may hold the
// marker { "kind": "pins" }: where the user's app / folder / group / separator / Recycle Bin items
// go. The dock object and the items are read by the config reader (ReadDockJson) and sanitized, so
// a preset is exactly a saved docks[] entry. A preset that fails validation (malformed JSON, no id
// or name, an unknown item kind, a widget that is not installed, an unknown theme preset, more than
// one marker) is left out of the list with an OutputDebugString line.

#include <windows.h>  // RECT (round 18 phase 18A: SilhouetteBar)

#include <string>
#include <vector>

#include "Config.h"

namespace Presets {

// The marker item's kind.
inline constexpr wchar_t kPinsKind[] = L"pins";

struct Preset {
    std::wstring id;           // "basic"
    std::wstring name;         // "Basic dock"
    std::wstring tagline;      // one line
    std::wstring description;  // what it is for
    DockConfig dock;           // its keys over the DockConfig defaults; `items` holds the marker as written
    bool hasPins = false;      // the items have the { "kind": "pins" } marker
    // --- round 12 phase 12C ---
    std::wstring path;         // a saved layout's file (Mine); "" for a built-in preset
    // --- end round 12 phase 12C ---
};

// The presets in list order (basic, minimal, modern, windows, sidebar, performance, dashboard),
// read and validated once (the first call).
const std::vector<Preset>& All();

// Index of the preset with this id (case-insensitive) in All(); SIZE_MAX: none.
size_t IndexOf(const std::wstring& id);

// What a preset puts on the dock, in order, for a list: "Your pinned apps and folders", "Running
// apps", "Separator", "Clock", the widgets' names ...
std::vector<std::wstring> ItemNames(const Preset& p, bool keepPins);

// Applies preset `p` to `target` (a dock entry): every dock key is the preset's (its value, else the
// default) except `id`, `color` and `monitor`; the items are the preset's, with `pins` at the marker
// when `keepPins` (anchors and offsets cleared so they stay there), the marker dropped otherwise.
// Clock / weather items get their full settings (the preset's keys over the config's defaults).
// Returns the new entry (a folder window / standalone widget target is not a dock: unchanged).
DockConfig Apply(const Preset& p, const Config& config, const DockConfig& target, const std::vector<DockItemEntry>& pins,
                 bool keepPins);

// A new dock from preset `p`: its id made unique (the preset's id, then id2 ...), the primary
// monitor, no colour (the palette by index), `pins` at the marker (none: dropped).
// Round 18 phase 18A: on display `monitor` (DockConfig::monitor: an index, kMonitorPrimary or
// kMonitorEach); `placeFree`: a bar dock whose edge is taken on that display by another enabled
// bar dock moves to the next free edge (bottom, top, left, right), as Duplicate dock does; every
// edge taken: it stays on its edge, moved along it by its estimated length (never refused). The
// Add a dock card places its docks so; the chooser's Add dock keeps the preset's edge as before.
DockConfig MakeNew(const Preset& p, const Config& config, const std::vector<DockItemEntry>& pins,
                   int monitor = DockConfig::kMonitorPrimary, bool placeFree = false);

// --- round 18 phase 18A: shared by the chooser and the Add a dock gallery ---------------------------
// "Bottom edge, centred. Hides while a window covers it." (where a layout puts its dock and how it
// shows; was PresetDialog.cpp's).
std::wstring WhereText(const DockConfig& d);
// Where the bar of `d` sits in a small picture of a display whose inner area (the screen inside its
// bezel) is `in`: `thick` px across, a share of the length (all of it when full width), aligned as
// the dock is; a floating dock where its x / y put it on a nominal 1920 x 1080 work area. Pure
// geometry, so the chooser's GDI silhouette and the gallery's Direct2D tiles draw the same picture.
RECT SilhouetteBar(const DockConfig& d, const RECT& in, int thick);
// A rough length of a new dock's bar in DIPs ((icon size + 8) per item, as Duplicate dock's
// fallback): MakeNew's step along a full edge and the Docks and displays canvas's ghost.
int EstimatedLengthDip(const DockConfig& d);

// The pins (IsPin() items) of a dock, as Apply / MakeNew take them.
std::vector<DockItemEntry> PinsOf(const DockConfig& dock);

// --- round 12 phase 12C: saved layouts ("Mine") and looks --------------------------------------
// A saved layout is %LocalAppData%\WinDock\layouts\<id>.json in the preset schema above, plus
// "look": "app" (the dock used the app look) or { "preset": "<theme id>", "theme": { fields } }
// (the dock's own look). Save dock as layout… writes it from a dock (SaveLayout): every dock key
// (the title fields included) except its id, colour and enabled state, the items with the pinned
// run replaced by the marker, the look, an automatic tagline ("12 items · Glass · bottom edge").
// It is read and validated by the same parser as a built-in preset (a theme file's stem is a valid
// look too); a file that does not validate is skipped with the reason.
struct MineList {
    std::vector<Preset> layouts;        // by name (case-insensitive), then id: DockCommand 167's index
    std::vector<std::wstring> skipped;  // "<file>: <reason>" for each file left out
};
// Reads the layouts folder now (the chooser asks when it opens and after every change).
MineList Mine();

// A look for the chooser's Looks kind: every built-in theme preset (Theme::BuiltIns order), then
// every readable theme file (Theme::ListUserThemes order). DockCommand 168's index.
struct Look {
    std::wstring id;       // the preset id / file stem (what themePreset holds)
    std::wstring name;     // "Glass"
    std::wstring tagline;  // LookTagline
    ThemeConfig values;
    bool user = false;     // a theme file
};
std::vector<Look> Looks();
// "Glass · 60 % transparent · round corners · glow" (from the Quick look values: Theme::Derived).
std::wstring LookTagline(const std::wstring& name, const ThemeConfig& values);

// Writes docks[cfg] of `config` as layout `name` (a dock, not a folder window / widget). The file
// is layouts\<stem of the name>.json (created with the folder); an existing file of that stem is
// replaced only when `overwrite` (else ERROR_FILE_EXISTS). `id` gets the stem. 0 or a Win32 error.
unsigned long SaveLayout(const Config& config, size_t cfg, const std::wstring& name, bool overwrite, std::wstring& id);
// The stem SaveLayout uses for `name` (file-name characters replaced, at most 64; "layout" for none).
std::wstring LayoutStem(const std::wstring& name);
// Renames a saved layout (its "name" in the file; the file keeps its stem). 0 or a Win32 error.
unsigned long RenameLayout(const Preset& layout, const std::wstring& name);
// Deletes a saved layout's file. 0 or a Win32 error.
unsigned long DeleteLayout(const Preset& layout);

}  // namespace Presets
