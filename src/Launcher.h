#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "DockItems.h"

// Starts dock items with ShellExecuteEx (no UI on failure: an error dialog would block the dock).
//   pinned .lnk / .exe: the original file (keeps the shortcut's arguments), working directory =
//                       the shortcut's (else the target's folder);
//   folders:            the directory (Explorer opens it);
//   the Recycle Bin:    shell:RecycleBinFolder; separators: nothing (false);
//   AUMID keys:         shell:AppsFolder\<AUMID>;
//   other path keys:    the exe itself, in its own folder.
namespace Launcher {

bool Launch(const DockItem& item);

// Same as Launch: most apps open a new window when started again (middle click, "New window").
bool LaunchNewInstance(const DockItem& item);

// Opens files with an app item (files dropped on its icon):
//   the app is a registered handler of the first file's type (SHAssocEnumHandlers; packaged
//               apps matched by AUMID, desktop apps by exe path): IAssocHandler::Invoke, as
//               Explorer's "Open with" does;
//   otherwise   AUMID keys: the app execution alias (an .exe pin) or ActivateApplication with
//               the quoted files as arguments; .lnk pins: the shortcut's target with its own
//               arguments, then the quoted files, in its working directory; .exe: the exe with
//               the quoted files as parameters, in its folder.
bool OpenWith(const DockItem& item, const std::vector<std::wstring>& files);

// Moves files to the Recycle Bin (IFileOperation::DeleteItems with FOF_ALLOWUNDO |
// FOFX_RECYCLEONDELETE; the shell's progress / confirmation UI owned by `owner`). Runs a modal
// loop while it works.
bool Recycle(const std::vector<std::wstring>& files, HWND owner);

// Opens a target the user typed (clock.clickTarget): an app / file path or a URL, environment
// variables expanded; a quoted path may be followed by arguments ("C:\x y\app.exe" -n). A file
// starts in its own folder.
bool OpenTarget(const std::wstring& target);
// A short name for a target: the file name without extension, or a URL's host.
std::wstring TargetName(const std::wstring& target);

}  // namespace Launcher
