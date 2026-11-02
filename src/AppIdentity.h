#pragma once

#include <windows.h>

#include <optional>
#include <string>

#include "Config.h"
#include "DockItems.h"

namespace AppIdentity {

// Resolves which application a top-level window belongs to:
//   1. the window's PKEY_AppUserModel_ID property (packaged apps, apps that set an explicit id),
//   2. for ApplicationFrameWindow (UWP host) without the property: the hosted CoreWindow's process,
//   3. the owning process's package AUMID (GetApplicationUserModelId) or its exe path,
//   4. "class:<window class>" when the process can't be opened (e.g. some elevated processes).
// Returns nullopt when the window can't be identified yet (e.g. a UWP frame whose content is
// not attached yet); callers should retry later rather than cache a wrong key.
std::optional<AppKey> IdentifyWindow(HWND hwnd);

// Human-readable app name: the Start menu name for AUMIDs, the exe's FileDescription
// (falling back to the file stem) for paths.
std::wstring DisplayNameFor(const AppKey& key);

// Lowercases with the invariant locale.
std::wstring Normalize(const std::wstring& s);

AppKey MakeAumidKey(const std::wstring& aumid);
AppKey MakePathKey(const std::wstring& path);

// Maps keys that name the same app to one canonical key, using a small alias table (e.g.
// Explorer windows report the AUMID "Microsoft.Windows.Explorer" while the pin is
// %WINDIR%\explorer.exe; both become the explorer.exe path key). Keys without an alias are
// returned unchanged. IdentifyWindow and ResolvePin already return canonical keys.
AppKey Canonicalize(AppKey key);

// True if a package of the AUMID's family ("Family_hash!App") is installed. Cheap (no shell).
bool AumidInstalled(const std::wstring& aumid);

using PinKind = DockItem::Kind;

// A config `pinned` entry resolved for the dock.
struct ResolvedPin {
    AppKey key;               // canonical identity, matched against running windows
    PinKind kind = PinKind::App;
    std::wstring displayName;
    std::wstring launchPath;  // the original .lnk / .exe, or the folder (empty for AUMID entries)
    std::wstring workingDir;  // the link's working dir, else the target's folder
    std::wstring targetPath;  // resolved target file (for "Open file location"), may be empty
    std::wstring iconFile;    // the entry's custom icon, else the link's explicit icon location
    int iconIndex = 0;
    bool customIcon = false;  // iconFile / iconIndex come from the entry's `icon`
    bool missing = false;     // the .lnk / .exe (or the link's target) does not exist
};

// Resolves a pinned entry. `ordinal` numbers separators (0 for the first separator entry, 1 for
// the next, ...) so each gets its own key.
//   kind "separator": key "kind:separator#<ordinal>", name "Separator", nothing to launch.
//   kind "recycleBin": key "kind:recyclebin", name "Recycle Bin", nothing to launch (the dock
//         opens shell:RecycleBinFolder).
//   folder (the path, or a .lnk's target, is a directory): kind Folder, key = the directory's
//         full path (so a shortcut to a folder and the folder itself are one item), name = its
//         last component ("C:" for a drive root), launchPath = targetPath = the directory.
//   .lnk: IShellLink + IPersistFile::Load + Resolve(SLR_NO_UI); key precedence: the link's
//         PKEY_AppUserModel_ID, else the target's package AUMID when the target is an app
//         execution alias (e.g. %LOCALAPPDATA%\Microsoft\WindowsApps\mspaint.exe), else the
//         target exe path. Name: the link's file stem.
//   .exe: the app execution alias AUMID if it is one, else the path key.
//   aumid (no path): the AUMID key.
// `icon` ("file", "file,index"; environment variables expanded) replaces the icon when the file
// exists (customIcon); otherwise the default icon is kept.
ResolvedPin ResolvePin(const PinnedEntry& entry, size_t ordinal = 0);

}  // namespace AppIdentity
