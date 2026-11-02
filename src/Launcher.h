#pragma once

#include "DockItems.h"

// Starts dock items with ShellExecuteEx (no UI on failure: an error dialog would block the dock).
//   pinned .lnk / .exe: the original file (keeps the shortcut's arguments), working directory =
//                       the shortcut's (else the target's folder);
//   AUMID keys:         shell:AppsFolder\<AUMID>;
//   other path keys:    the exe itself, in its own folder.
namespace Launcher {

bool Launch(const DockItem& item);

// Same as Launch: most apps open a new window when started again (middle click, "New window").
bool LaunchNewInstance(const DockItem& item);

}  // namespace Launcher
