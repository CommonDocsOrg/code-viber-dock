#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "Config.h"
#include "DockItems.h"

namespace ContextMenu {

// Dock commands 100 - kLastDockCommand are stable automation ids (WinDock.DockCommand).
enum Command : UINT {
    kNone = 0,
    kAutoHide = 100,  // toggles Off <-> the last on-mode (Always / Intelligent)
    kHideTaskbar,
    kStartWithWindows,
    kOpenConfig,
    kRestoreTaskbar,  // show the taskbar for this session (unchecks "Hide taskbar"; config unchanged)
    kQuit,
    kReloadConfig,
    kOpenSettings = 107,  // opens (or activates) the Global settings window ("Global settings…")
    kAutoHideOff = 108,
    kAutoHideAlways = 109,
    kAutoHideIntelligent = 110,
    kRefreshWeather = 111,  // fetches the weather now
    kAddSeparator = 112,  // appends a separator item to the pinned list
    kWeatherSettings = 113,     // the dock's first weather item's settings window (none: the dock's window)
    kToggleCalendar = 114,      // shows / hides the month calendar over the clock
    kToggleWeatherPopup = 115,  // shows / hides the forecast popup over the weather widget
    kLockDock = 116,            // toggles docks[].locked of the dock the menu / message is for
    kAddClock = 117,            // Add widget > Clock (DockConfig::AddWidget; not while locked)
    kAddWeather = 118,          // Add widget > Weather
    kAddRunning = 119,          // Add widget > Running apps (only when the dock has none)
    kEdgeBottom = 120,          // Position > Bottom / Top / Left / Right (docks[0].edge)
    kEdgeTop = 121,
    kEdgeLeft = 122,
    kEdgeRight = 123,
    kLockAllDocks = 124,        // sets docks[].locked on every dock
    kUnlockAllDocks = 125,      // clears it on every dock
    kReloadWidgets = 126,       // every script widget (every dock) reloads its package and script
    kWidgetConsole = 127,       // opens (or activates) the widget Console
    kAddTray = 128,             // Add widget > Tray (a "tray" item; also turns tray.enabled on)
    kTrayEnable = 129,          // tray.enabled on: the dock takes the notify icons over (with a tray item)
    kTrayDisable = 130,         // tray.enabled off: the icons go back to Explorer ("Reset to Explorer")
    kPollNotifications = 131,   // the notification service polls now (no-op while it is off)
    kToggleBadges = 132,        // docks[].badges of the dock the message is for (on / off)
    kToggleCentre = 133,        // shows / hides the notification centre over the first notifications item
    kAddNotifications = 134,    // Add widget > Notification centre (a "notifications" item; one per dock)
    kRefreshCalendars = 135,    // every calendar feed fetches / is read now (no-op while none is enabled)
    // Desktop folder windows (round 4 phase 12):
    kCreateFolderWindow = 136,  // automation: a folder window for the path in the global atom LOWORD(lParam), at the pointer
    kRemoveFolderWindow = 137,  // automation: removes the folder window HIWORD(lParam)
    kSimulateExplorerRestart = 138,  // test hook: every folder window pushed under the desktop, then the restart recovery
    kNewFolderWindow = 139,     // "New folder window…": a folder picker, then a folder window at the pointer
    kSortDesktopNow = 140,      // 12b: the Desktop's files matching folder windows' rules move into their folders (once)
    // Widgets (round 4 phase 14):
    kInstallWidgetFolder = 141,  // automation: Settings > Widgets > Add from folder… for the path in the global atom LOWORD(lParam)
    // Round 5 phase A:
    kItemSettings = 142,        // automation: the settings window of item LOWORD(lParam) of dock HIWORD(lParam) (App::OpenItemSettings)
    // Round 5 phase B:
    kDockSettings = 143,        // "Dock settings…": the settings window of dock HIWORD(lParam) (the dock whose menu it is)
    kFlashDock = 144,           // "Flash this dock": dock HIWORD(lParam) reveals and pulses its border in its colour (~1.5 s)
    // Round 6 phase 6A: dashboard mode (every "desktop" / "behind" dock shown and topmost; DockManager::SetDashboard).
    kToggleDashboard = 145,     // "Show dashboard" (the tray / dock menu, = the hotkey): on <-> off
    kEnterDashboard = 146,      // on (Settings > General "Show the dashboard now")
    kLeaveDashboard = 147,      // off
    // Round 6 phase 6E: every open settings window drops its widget previews and renders them
    // again as they are painted (automation; Reload widgets does the same).
    kRerenderPreviews = 148,
    // Round 7 phase 7A: "Task Manager" (runs taskmgr.exe; it elevates by itself).
    kTaskManager = 149,
    kLastDockCommand = kTaskManager,
    // Round 9 phase 9A (automation only, App::OnSharedMessage): the full-screen guard forced on
    // (LOWORD(lParam) 1) / off (0) for every monitor, or back to the real evaluation (2).
    kFullscreenGuard = 150,
    // Round 9 phase 9B (automation only, App::OnSharedMessage): the settings search dialog of the
    // Dock window of dock HIWORD(lParam) (LOWORD 1: of the Global window), the window opened first.
    kFindSetting = 151,
    // Round 9 phase 9C (automation only, App::OnSharedMessage): the preset chooser for dock
    // HIWORD(lParam) (LOWORD 1: the welcome variant, 2: the "new dock" variant, else apply to the
    // dock), and preset LOWORD & 0xFF applied to dock HIWORD without the dialog (bit 8: keep pins).
    kPresetDialog = 152,
    kApplyPreset = 153,
    kTogglePin = 800,      // per-item: Pin to dock / Unpin from dock (separators: Remove)
    kNewWindow,            // per-item: launch (a new instance); the Recycle Bin's Open
    kOpenFileLocation,     // per-item: explorer /select,<target>
    kOpenInExplorer = 803,      // folders: open the folder in Explorer
    kChooseIcon = 804,          // pins: the Change Icon dialog (an icon from an .exe / .dll / .ico)
    kResetIcon = 805,           // pins: drop the custom icon (the entry's `icon`)
    kToggleFolderPopup = 806,   // folders: show / hide the contents popup
    kEmptyRecycleBin = 807,     // the Recycle Bin: empty it (the shell's confirmation)
    kChooseImage = 808,         // pins: an open-file dialog for an image / icon file
    kReloadWidget = 809,        // a script widget: reload its package and script (automation)
    kClickSlot = 810,           // a left click on the slot (automation; e.g. activate a window slot)
    kViewAuto = 811,            // folders: the popup's layout (the item's `view`): auto / grid / list
    kViewGrid = 812,
    kViewList = 813,
    // Automation on the dock's open folder popup (lParam's low word is the argument, not an item):
    kPopupSelect = 814,         // select entry n (scrolled into view)
    kPopupScroll = 815,         // scroll to n px (animated unless reduce motion)
    kPopupOpen = 816,           // open entry n as a left click does
    kRenameGroup = 817,         // groups: Rename… (a small dialog; sent: posted back, like 804)
    kUngroup = 818,             // groups: the members become pins at the group's place
    // Groups, with an argument in the HIGH word of wParam (the low word is the command):
    kGroupWith = 819,           // slot lParam onto slot HIWORD(wParam): a group of the two (or added to that group)
    kMemberOut = 820,           // group slot lParam: member HIWORD(wParam) becomes a pin right after the group
    kRemoveMember = 821,        // group slot lParam: member HIWORD(wParam) is removed
    kTrayClick = 822,           // tray slot lParam: left click on its drawer's icon HIWORD(wParam) (forwarded to the app)
    kTrayRightClick = 823,      // ... a right click (the app's menu)
    kTrayPin = 824,             // tray slot lParam: its drawer's icon HIWORD(wParam) pinned as a trayApp item
    kTrayUnpin = 825,           // trayApp slot lParam: unpinned (back in the grid)
    kTrayHide = 826,            // tray slot lParam: drawer icon HIWORD(wParam) hidden from / shown in the grid (toggle)
    kTrayMove = 827,            // tray slot lParam: drawer icon (wParam bits 16-23) moved before drawer icon (bits 24-31; 255 = to the end)
    // The dock's open notification centre (lParam: the dock in its high word; the row in HIWORD(wParam)):
    kCentreDismiss = 828,       // dismiss row HIWORD(wParam) (as its close glyph does)
    kCentreClearAll = 829,      // "Clear all"
    kCentreActivate = 830,      // row HIWORD(wParam) as a click: its app to the front, the notification dismissed
    // A scrolling dock's run (round 4 phase 11a; lParam: the dock in its high word):
    kScrollTo = 831,            // scroll to offset HIWORD(wParam) px at once
    kScrollAnimated = 832,      // ... with the wheel's glide
    kScrollIntoView = 833,      // scroll just far enough to show slot LOWORD(lParam)
    kMagnifyAt = 834,           // magnify as if the pointer were at stage main coordinate HIWORD(wParam) (0xFFFF: left)
    // Calendars (round 4 phase 13; lParam: the dock in its high word):
    kCalendarSelect = 835,      // the open calendar popup selects today + (int16) HIWORD(wParam) days
    kReminderFireNext = 836,    // the earliest pending reminder fires now (any dock: the service's)
    kReminderSnooze = 837,      // the dock's reminder banner: Snooze 5 min
    kReminderDismiss = 838,     // ... Dismiss
    // A folder window (round 4 phase 12; lParam: the folder window's dock index in its high word):
    kFolderSelect = 839,        // select entry LOWORD(lParam) (0xFFFF: none); HIWORD(wParam) 1 = Ctrl, 2 = Shift
    kFolderOpen = 840,          // open entry LOWORD(lParam) as a double click does
    kFolderScroll = 841,        // scroll to HIWORD(wParam) px
    kFolderCollapse = 842,      // collapse / expand (as a double click on the title)
    kFolderBeginRename = 843,   // F2 on entry LOWORD(lParam): the inline rename box opens
    kFolderCommitRename = 844,  // commits it with the name in the global atom HIWORD(wParam) (0: the box's text)
    kFolderRecycle = 845,       // Delete: the selection goes to the Recycle Bin
    kFolderMenu = 846,          // the context menu of entry LOWORD(lParam) (0xFFFF: the background); posted
    kFolderSelectAll = 847,     // Ctrl+A
    kFolderDropOn = 848,        // test hook: the selection dropped onto folder window HIWORD(wParam) (no OLE loop)
    kFolderDropPath = 849,      // test hook: the path in the atom HIWORD(wParam) dropped on entry LOWORD(lParam) (0xFFFF: background)
    kFolderDropPathCopy = 850,  // ... with Ctrl held (a copy)
    kFolderCopy = 851,          // 12b: Ctrl+C on the selection (the shell data object, "Preferred DropEffect" copy)
    kFolderCut = 852,           // Ctrl+X (a move when pasted)
    kFolderPaste = 853,         // Ctrl+V: the clipboard's files into the folder
    // Round 5 phase B: "Settings…" of a widget / clock / weather / folder / group slot (its Item window).
    kOpenItemSettings = 854,
    // Round 6 phase 6B: a standalone widget entry (lParam: its dock index in the high word; also its
    // right-click menu's ids, with 854 Widget settings…, 809 Reload widget, 143 Window settings… and
    // 107 Global settings…).
    kWidgetToggleLock = 855,    // Lock / Unlock (docks[].locked: no move, no resize)
    kWidgetRemove = 856,        // Remove from desktop (the entry goes; posted)
    // Round 6 phase 6C: a widget's open text editor (popup.editText). lParam: the dock in its high
    // word, the widget's slot in its low word (0xFFFF: the dock's open editor); a standalone
    // widget entry: its dock index.
    kEditorOk = 857,            // closes it as OK does (the promise gets the text)
    kEditorCancel = 858,        // closes it as Cancel does (the promise gets null)
    // Round 7 phase 7B: item anchors (automation; slot LOWORD(lParam) of dock HIWORD(lParam), the
    // argument in HIWORD(wParam)); the slot's config item changes, the items are sorted into group order.
    kSetItemAnchor = 859,       // anchor HIWORD(wParam): 0 none (the dock's align), 1 start, 2 centre, 3 end
    kSetItemOffset = 860,       // offset (int16) HIWORD(wParam) DIPs, clamped to -4000 - 4000
    kCloseAllWindows = 900,
    kCloseWindowBase = 1000,  // + window index (per-item menu)
};

struct TrayMenuState {
    AutoHideMode autoHide = AutoHideMode::Off;
    bool hideTaskbar = false;       // the taskbar controller is enabled (taskbar hidden by the dock)
    bool startWithWindows = false;  // the HKCU Run value exists
    bool weatherShown = false;      // the dock has a weather item: Refresh weather / Weather settings… entries
    bool weatherConfigured = false; // ... and a location is set (Refresh weather enabled)
    bool locked = false;            // the dock is locked: Lock dock checked, Add separator / widget greyed
    bool hasRunning = false;        // the dock has a running item (Add widget > Running apps greyed)
    DockEdge edge = DockEdge::Bottom;  // Position > (radio)
    int dockCount = 1;              // config docks: Lock all / Unlock all docks with more than one
    bool anyLocked = false;         // some dock is locked (Unlock all docks enabled)
    bool anyUnlocked = false;       // some dock is not (Lock all docks enabled)
    bool hasWidgets = false;        // some dock shows a script widget: Reload widgets
    bool hasTray = false;           // the dock has a tray item (Add widget > Tray greyed)
    bool hasNotifications = false;  // the dock has a notifications item (Add widget > Notification centre greyed)
    bool dashboard = false;         // dashboard mode is on ("Show dashboard" checked)
    // The dock menu opened over a widget / clock / weather slot: its "<name> settings…" entry
    // (kOpenItemSettings) at the top; empty: none.
    std::wstring itemSettings;
};

// Shows the dock/tray menu at a screen point and returns the chosen Command (kNone if dismissed).
UINT ShowTrayMenu(HWND owner, POINT screenPt, const TrayMenuState& state);

struct ItemMenuState {
    DockItem::Kind kind = DockItem::Kind::App;
    bool pinned = false;
    bool canPin = true;            // false for apps without a launchable identity
    bool canLaunch = true;
    bool running = false;          // "New window" vs "Open"
    bool canOpenLocation = false;  // path-based items
    bool binEmpty = false;         // the Recycle Bin has nothing in it (Empty is greyed)
    bool hasIcon = false;          // the pin's config entry names an `icon` (Reset icon is greyed otherwise)
    ItemView view = ItemView::Auto;  // folders / groups: the popup's layout (View submenu)
    size_t members = 0;              // groups: how many members (Unpin group's text)
};

// Per-item menu, then the dock menu. Apps: the app name (disabled header), Pin/Unpin, the icon
// entries (pinned apps only), New window (Open when not running), Open file location, one
// "Close <title>" entry per window (kCloseWindowBase + index), "Close all windows" when there
// are several. Folders: the name, Unpin, the icon entries, Open in Explorer, View (Automatic /
// Grid / List), Settings… (its Item window). Groups: the name, Show contents, Rename…, Ungroup,
// Unpin group (with its members), the icon entries, View, Settings…. Separators: Remove.
// The Recycle Bin: Open, Empty Recycle Bin, Unpin, the icon entries. The icon entries: Choose
// icon…, Choose image…, Reset icon.
UINT ShowItemMenu(HWND owner, POINT screenPt, const std::wstring& appName, const std::vector<std::wstring>& windowTitles,
                  const ItemMenuState& item, const TrayMenuState& state);

}  // namespace ContextMenu
