#pragma once

// Round 9 phase 9C: predefined dock setups ("presets"). Each is a JSON file shipped as RCDATA
// (res/presets/<id>.json, IDR_PRESET_*): { "id", "name", "tagline" (one line), "description" (2 - 4
// sentences), "dock": { docks[] keys }, "items": [ docks[].items entries ] }. The items may hold the
// marker { "kind": "pins" }: where the user's app / folder / group / separator / Recycle Bin items
// go. The dock object and the items are read by the config reader (ReadDockJson) and sanitized, so
// a preset is exactly a saved docks[] entry. A preset that fails validation (malformed JSON, no id
// or name, an unknown item kind, a widget that is not installed, an unknown theme preset, more than
// one marker) is left out of the list with an OutputDebugString line.

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
DockConfig MakeNew(const Preset& p, const Config& config, const std::vector<DockItemEntry>& pins);

// The pins (IsPin() items) of a dock, as Apply / MakeNew take them.
std::vector<DockItemEntry> PinsOf(const DockConfig& dock);

}  // namespace Presets
