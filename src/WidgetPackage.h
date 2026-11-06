#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "Config.h"

// Widget packages (docs/widgets.md). A widget is a folder %LocalAppData%\WinDock\widgets\<id>\
// with a manifest `widget.json`, its ES module `main.js` (plus any modules it imports) and its
// assets, or a built-in shipped inside the exe as RCDATA resources (res/app.rc, the sources live in
// the repo's widgets\ folder). A folder with the id of a built-in replaces it (the folder wins).
// Everything a script reads goes through Package::Read, which keeps it inside the package.
namespace Widgets {

// %LocalAppData%\WinDock\widgets (not created).
std::wstring Directory();
// 1 - 64 characters of a-z, 0-9, '-', '_', '.', not starting with '.'.
bool ValidId(std::wstring_view id);

struct SizeHint {
    float main = 0.0f;   // DIPs along the dock
    float cross = 0.0f;  // DIPs across it
};

// One setting of the manifest's `settings` schema.
struct SettingField {
    // Round 5 phase A: Place (a geocoded place; its value is the item's `latitude`, `longitude` and
    // `location` keys, not a key of its own) and TimeZone (a Windows time zone key name, "" = the
    // local time).
    enum class Type { Bool, Int, Number, String, Choice, Color, Font, Place, TimeZone };
    std::wstring key;
    std::wstring label;        // default: the key
    std::wstring description;  // optional note
    Type type = Type::String;
    std::wstring defaultJson;  // the default as JSON text (always present after parsing)
    double min = -1e9, max = 1e9, step = 1.0;  // Int / Number
    std::vector<std::pair<std::wstring, std::wstring>> choices;  // Choice: value, label
    // Choice: more choices from the package's files matching this glob ("faces/*.json"), added by
    // Load: the value is the file name without folder and extension, the label the file's JSON
    // "name" (else the value).
    std::wstring choiceFrom;
    // Round 6 phase 6D: `"choiceFrom": "perf"`: the host lists the perf provider's metrics on the
    // settings page (PerfService); the value is a metric key, kept even when it is not listed now.
    bool ChoiceFromPerf() const { return choiceFrom == L"perf"; }
    // The generated settings page (round 4 phase 14): a section header when `group` changes from
    // the field before, a unit after an int / number control, and the field shown only while every
    // `visibleIf` key (another field) has that value (JSON text of the value).
    std::wstring group;
    std::wstring unit;
    std::vector<std::pair<std::wstring, std::wstring>> visibleIf;
    // Round 10 phase 10B: `"common": true`: one of the few settings the Dock window's Contents tab
    // shows in its inspector for the selected item (CommonFields), editable in place.
    bool common = false;
};

// A problem found in a widget's manifest (Validate): `field` names the key ("main",
// "settings[2].choiceFrom" ...), `fix` says what to change. Errors stop the widget from loading
// (the error tile and the Console show them); warnings are logged.
struct Issue {
    bool error = true;
    std::wstring field;
    std::wstring message;
    std::wstring fix;
    // error: main: "app.js" was not found in the widget folder (fix: ...)
    std::wstring Text() const;
};

// The manifest's `standalone` object (round 6 phase 6B): how the widget behaves in a window of its
// own on the desktop (WidgetWindow). The window's client area is the widget's box.
struct StandaloneHint {
    enum class Aspect { Free, Keep, Ratio };
    float minWidth = 48.0f;   // DIPs, 16 - 4000
    float minHeight = 48.0f;
    // "free" (default), "keep" (w : h stays what it was when the window was made) or a number
    // (w / h is kept at it).
    Aspect aspect = Aspect::Free;
    float ratio = 0.0f;       // Ratio: w / h (> 0)
    // What "Add to the desktop" creates (0: from the horizontal size hint, DefaultSize).
    float defaultWidth = 0.0f;
    float defaultHeight = 0.0f;
};

struct Manifest {
    std::wstring id;
    std::wstring name;
    std::wstring version;
    std::wstring author;
    std::wstring description;
    std::wstring main = L"main.js";  // package-relative module path
    SizeHint horizontal{64.0f, 40.0f};
    SizeHint vertical{48.0f, 48.0f};
    StandaloneHint standalone;
    // The size "Add to the desktop" gives a new standalone widget (DIPs): standalone.defaultWidth /
    // defaultHeight, else 200 x 200 for a square-ish horizontal hint, else twice the horizontal
    // hint; never below the minimum.
    void DefaultStandaloneSize(int& w, int& h) const;
    // Round 6 phase 6E, the manifest's `preview` object (the widget drawer's live preview): the
    // settings of the preview instance (a JSON object merged over the defaults; "" none) and the
    // preview box in DIPs (0: the box the widget measures on a 48 DIP horizontal dock).
    std::wstring previewSettings;
    float previewWidth = 0.0f;
    float previewHeight = 0.0f;
    int refreshMs = 0;      // 0: no tick; else tick() every refreshMs, aligned to local time
    int timeLimitMs = 50;     // the watchdog: longest a single call into the script may run
    int memoryLimitMb = 32;   // the script heap
    bool watch = false;       // reload when a file of the folder changes (folder widgets)
    bool shell = false;       // permission "shell"
    bool http = false;        // permission "http" (with httpHosts)
    bool audio = false;       // permission "audio"
    bool media = false;       // permission "media"
    bool notifications = false;  // permission "notifications" (round 4 phase 9)
    bool calendar = false;       // permission "calendar" (round 4 phase 13)
    bool weather = false;        // permission "weather" (round 4 phase 14)
    bool perf = false;           // permission "perf" (round 6 phase 6D)
    std::vector<std::wstring> httpHosts;  // lowercase; "*.example.com" matches sub-domains
    std::vector<SettingField> settings;
    std::vector<std::wstring> warnings;  // what was ignored while reading it
    std::vector<Issue> issues;           // Validate's findings (Load fills them in)
    bool HasErrors() const;
};

class Package {
public:
    enum class Source { BuiltIn, Folder };

    Source source = Source::BuiltIn;
    std::wstring id;
    std::wstring folder;  // Folder: the canonical folder path (no trailing backslash)
    Manifest manifest;

    // Reads a package file by its normalized package-relative path ("main.js", "lib/a.js",
    // "assets/x.png"). False (with `error`) if it does not exist, is larger than 8 MB, or (folder
    // packages) resolves outside the folder (a junction or symbolic link pointing elsewhere).
    bool Read(const std::string& path, std::string& out, std::wstring& error) const;
    // The package's files (built-in: its resources; folder: every file below it, up to 512).
    std::vector<std::string> Files() const;
};

// Resolves an import specifier (or an asset path) against the module that imports it (`base`, a
// normalized path; "" for the package root): "./x.js" and "../x.js" are relative to base's folder,
// any other name is relative to the package root. Rejected (false): absolute paths ("/x", "\\x",
// "C:..."), URLs or anything with ':', backslashes, empty segments, and a ".." that would leave
// the package. `out`: "dir/file.js".
bool ResolvePath(std::string_view base, std::string_view spec, std::string& out);
// A package path against a glob: '*' is any run of characters and '?' any one character, neither
// crossing a '/'; case-insensitive (ASCII). "faces/*.json" matches "faces/classic.json".
bool GlobMatch(std::string_view pattern, std::string_view path);

// Loads the package with this id (folder first, then the built-ins). Null with `error` if there is
// none or its manifest is unreadable.
std::shared_ptr<const Package> Load(const std::wstring& id, std::wstring& error);

// Checks widget.json (`bytes`) of `pkg` (its files: main, choiceFrom globs) against the manifest
// format: unknown / malformed keys, a missing main module, sizes, permissions, the settings
// schema. Called by Load (into manifest.issues) and by `WinDock.exe --validate-widget`.
std::vector<Issue> Validate(const std::string& bytes, const Package& pkg);
// The CLI's check of a widget folder (not installed): reads and validates it. False when it cannot
// be read at all (no folder, no widget.json; `issues` say why). `name`: its manifest name.
bool ValidateFolder(const std::wstring& folder, std::vector<Issue>& issues, std::wstring& name);

// The installed widgets for menus and the settings window: every built-in and every folder under
// Directory() with a widget.json, one entry per id (folder wins), sorted by name.
struct CatalogEntry {
    std::wstring id;
    std::wstring name;
    Package::Source source = Package::Source::BuiltIn;
    bool builtIn = false;  // a built-in with this id exists (also when the folder replaces it)
};
std::vector<CatalogEntry> Catalog();
// The display name of widget `id` (its manifest name, else the id).
std::wstring DisplayName(const std::wstring& id);

// Writes built-in widget `id`'s files to Directory()\<id>\ (refused if that folder exists), its
// faces / assets sub-folders included, and a README.md stub pointing at docs/widgets.md.
// `folder`: where it went. The folder copy then replaces the built-in.
bool ExportBuiltIn(const std::wstring& id, std::wstring& folder, std::wstring& error);
// Copies a widget folder (it must hold a widget.json; up to 512 files, 8 MB each, no links
// followed) to Directory()\<its folder name> (lowercased; must be a valid id, refused if taken).
// `id`: the installed id.
bool InstallFolder(const std::wstring& source, std::wstring& id, std::wstring& error);

// The settings a script sees: the schema's defaults, overridden by the item's `settings` object
// (`itemJson`, may be empty) where the value has the right type (ints / numbers clamped to the
// range, choices checked); keys the schema does not know are passed through. A place field adds
// no key of its own: the item's `latitude` / `longitude` (numbers in range, 4 decimals) and
// `location` (a string) when it has them. UTF-8 JSON object.
std::string MergeSettings(const Manifest& manifest, const std::wstring& itemJson);

// The schemas of the built-in kinds that have item settings (round 5 phase A), for the generated
// settings page: ItemKind::Clock (format, showSeconds, timeZone, showDate, label, face, click,
// clickTarget) and ItemKind::Weather (place, label, units, refreshMinutes); empty for the others.
// The clock's face choices are read now (Digital, then ClockFaces::List()). The keys are those of
// ClockConfig / WeatherConfig (Config.h: ClockOf / WeatherOf read them).
std::vector<SettingField> BuiltInSchema(ItemKind kind);

// Round 10 phase 10B: the fields the Contents tab's inspector offers (indexes into
// manifest.settings, in schema order): the ones marked `"common": true`; a schema without any: its
// first three fields of a simple type (bool, int, number, string, a choice not from "perf").
std::vector<size_t> CommonFields(const Manifest& manifest);

}  // namespace Widgets
