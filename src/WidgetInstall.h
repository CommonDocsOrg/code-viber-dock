#pragma once

#include <windows.h>

#include <atomic>
#include <string>
#include <vector>

// Round 29 phase 29B: installing a widget from a .zip (or .viberwidget, the same format) and
// removing one, for App settings > Widget library.
//
// A zip install has two halves. PrepareZipInstall (a worker thread, inside Crash::Job) reads the
// archive (at most 20 MB), checks every entry with Zip::ReadDirectory (names, links, sizes, ratio),
// finds the widget's layout (widget.json at the root, or inside the only top-level folder; a
// __MACOSX/ folder is ignored), extracts into a fresh staging folder of its own under
// %LocalAppData%\WinDock (install-<unique>\<id>\, every file created new: nothing existing is ever
// written through), and validates the result as --validate-widget does (ValidateFolder: the
// manifest and every script's syntax). Nothing of the archive runs: the check compiles the scripts
// in a throwaway engine without any host API and calls nothing. CommitZipInstall (the UI thread,
// after the user said whether to replace an installed widget of that id) moves the staged folder
// into the widgets folder (one rename on the same volume). The installed widget then takes the
// normal consent path: the caller drops any consent of that id first, so an install never inherits
// what the user allowed an earlier folder.
namespace Widgets {

struct ZipInstall {
    std::wstring zipPath;
    // --- PrepareZipInstall's results ---
    std::atomic<bool> done{false};
    bool ok = false;
    std::wstring error;       // why it was refused (plain words, the first problem)
    std::wstring id;          // the widget id it installs as
    std::wstring name, version, author;
    std::vector<std::wstring> permissionLines;  // what the consent dialog will list
    size_t files = 0;
    size_t warnings = 0;      // ValidateFolder's warnings (the Console shows them after install)
    std::wstring stagingRoot;  // %LocalAppData%\WinDock\install-<unique> ("" once committed / discarded)
    std::wstring staged;       // stagingRoot\<id>
};

// The worker half (see above). Never throws; sets `done` last.
void PrepareZipInstall(ZipInstall& job);
// Directory()\<id> exists (an installed widget of that id; the caller asks before replacing it).
bool InstalledFolderExists(const std::wstring& id);
// The UI half: the staged folder becomes Directory()\<id>. `replace`: an existing folder of that id
// goes to the Recycle Bin first (else its existence is an error). Discards the staging folder.
bool CommitZipInstall(ZipInstall& job, bool replace, HWND owner, std::wstring& error);
// Removes what PrepareZipInstall staged (a refused or cancelled install).
void DiscardZipInstall(ZipInstall& job);
// Staging folders left behind (the dock quit or the settings window closed during an install).
void RemoveStaleStaging();

// The files / folders to the Recycle Bin (IFileOperation with undo; the shell warns before deleting
// anything it cannot recycle). False with `error` when it failed or was cancelled.
bool RecyclePaths(const std::vector<std::wstring>& paths, HWND owner, std::wstring& error);

}  // namespace Widgets
