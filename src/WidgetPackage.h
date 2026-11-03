#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

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
    enum class Type { Bool, Int, Number, String, Choice, Color, Font };
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
    int refreshMs = 0;        // 0: no tick; else tick() every refreshMs, aligned to local time
    int timeLimitMs = 50;     // the watchdog: longest a single call into the script may run
    int memoryLimitMb = 32;   // the script heap
    bool watch = false;       // reload when a file of the folder changes (folder widgets)
    bool shell = false;       // permission "shell"
    bool http = false;        // permission "http" (with httpHosts)
    bool audio = false;       // permission "audio"
    bool media = false;       // permission "media"
    bool notifications = false;  // permission "notifications" (round 4 phase 9)
    std::vector<std::wstring> httpHosts;  // lowercase; "*.example.com" matches sub-domains
    std::vector<SettingField> settings;
    std::vector<std::wstring> warnings;  // what was ignored while reading it
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

// Writes built-in widget `id`'s files to Directory()\<id>\ (refused if that folder exists).
// `folder`: where it went. The folder copy then replaces the built-in.
bool ExportBuiltIn(const std::wstring& id, std::wstring& folder, std::wstring& error);
// Copies a widget folder (it must hold a widget.json; up to 512 files, 8 MB each, no links
// followed) to Directory()\<its folder name> (lowercased; must be a valid id, refused if taken).
// `id`: the installed id.
bool InstallFolder(const std::wstring& source, std::wstring& id, std::wstring& error);

// The settings a script sees: the schema's defaults, overridden by the item's `settings` object
// (`itemJson`, may be empty) where the value has the right type (ints / numbers clamped to the
// range, choices checked); keys the schema does not know are passed through. UTF-8 JSON object.
std::string MergeSettings(const Manifest& manifest, const std::wstring& itemJson);

}  // namespace Widgets
