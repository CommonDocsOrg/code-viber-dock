#include "ContextMenu.h"

namespace ContextMenu {

namespace {

void Append(HMENU menu, UINT id, const wchar_t* text, bool enabled, bool checked = false) {
    UINT flags = MF_STRING;
    if (!enabled) flags |= MF_GRAYED;
    if (checked) flags |= MF_CHECKED;
    AppendMenuW(menu, flags, id, text);
}

void Separator(HMENU menu) { AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); }

// A submenu `sub` (destroyed with its parent) under `text`.
void AppendSub(HMENU menu, HMENU sub, const wchar_t* text, bool enabled = true) {
    AppendMenuW(menu, MF_POPUP | MF_STRING | (enabled ? 0 : MF_GRAYED), reinterpret_cast<UINT_PTR>(sub), text);
}

// Tools ▸ (round 10 phase 10A): the dock menu's and the tray-icon menu's. Reload widgets and the
// Console stay in place greyed without a script widget, so the positions do not move.
void AppendTools(HMENU menu, bool hasWidgets) {
    HMENU sub = CreatePopupMenu();
    if (!sub) return;
    Append(sub, kOpenConfig, L"Open config file", true);
    Append(sub, kReloadConfig, L"Reload config", true);
    Append(sub, kReloadWidgets, L"Reload widgets", hasWidgets);
    Append(sub, kWidgetConsole, L"Widget console…", hasWidgets);
    Separator(sub);
    Append(sub, kTaskManager, L"Task Manager", true);  // round 7 phase 7A
    AppendSub(menu, sub, L"Tools");
}

// Round 10 phase 10A: the dock menu grouped by task (was one long list; every command kept). The
// first group is this dock's, after the separator what goes beyond it.
void AppendDockEntries(HMENU menu, const TrayMenuState& state) {
    // Round 12 phase 12D: customize mode first (under Undo "<label>" when there is one).
    if (state.canEdit) Append(menu, kCustomize, state.editing ? L"Done customizing" : L"Customize this dock", true);
    Append(menu, kDockSettings, L"Dock settings…", true);  // round 5 phase B: this dock's window
    if (HMENU sub = CreatePopupMenu()) {
        // The dock's own items (greyed while it is locked, as Add widget was), then the drawer and
        // a folder window (neither touches this dock's items: never greyed).
        const bool unlocked = !state.locked;
        Append(sub, kAddClock, L"Clock", unlocked);
        Append(sub, kAddWeather, L"Weather (script)", unlocked);
        Append(sub, kAddRunning, L"Running apps", unlocked && !state.hasRunning);
        Append(sub, kAddTray, L"Tray (notify icons)", unlocked && !state.hasTray);
        Append(sub, kAddNotifications, L"Notification centre", unlocked && !state.hasNotifications);
        Append(sub, kAddSeparator, L"Separator", unlocked);
        Separator(sub);
        Append(sub, kAddFromDrawer, L"Apps, folders and more widgets…", true);
        Separator(sub);
        Append(sub, kNewFolderWindow, L"Folder window on the desktop…", true);
        AppendSub(menu, sub, L"Add");
    }
    if (HMENU sub = CreatePopupMenu()) {
        Append(sub, kEdgeBottom, L"Bottom", true);
        Append(sub, kEdgeTop, L"Top", true);
        Append(sub, kEdgeLeft, L"Left", true);
        Append(sub, kEdgeRight, L"Right", true);
        CheckMenuRadioItem(sub, kEdgeBottom, kEdgeRight, kEdgeBottom + static_cast<UINT>(state.edge), MF_BYCOMMAND);
        Separator(sub);
        // How to move it along the edge (or to another one): a hint, not a command.
        Append(sub, kNone, state.locked ? L"Unlock the dock to move it by dragging" : L"Ctrl+drag the dock to move it",
               false);
        AppendSub(menu, sub, L"Position on screen");
    }
    if (HMENU sub = CreatePopupMenu()) {
        Append(sub, kAutoHideOff, L"Off", true);
        Append(sub, kAutoHideAlways, L"Always", true);
        Append(sub, kAutoHideIntelligent, L"When a window overlaps", true);
        const UINT checked = state.autoHide == AutoHideMode::Intelligent ? kAutoHideIntelligent
                             : state.autoHide == AutoHideMode::Always    ? kAutoHideAlways
                                                                         : kAutoHideOff;
        CheckMenuRadioItem(sub, kAutoHideOff, kAutoHideIntelligent, checked, MF_BYCOMMAND);
        AppendSub(menu, sub, L"Auto-hide");
    }
    Append(menu, kLockDock, L"Lock dock", true, state.locked);
    // --- round 12 phase 12C: docks you keep ---
    Append(menu, kRenameDock, L"Rename dock\x2026", true);
    Append(menu, kDuplicateDock, L"Duplicate dock", true);
    Append(menu, kSaveDockLayout, L"Save dock as layout\x2026", true);
    // --- end round 12 phase 12C ---
    Separator(menu);
    if (HMENU sub = CreatePopupMenu()) {
        // Round 6 phase 6A: the desktop docks to the front (the dashboard hotkey does the same).
        Append(sub, kToggleDashboard, L"Show dashboard", true, state.dashboard);
        if (state.dockCount > 1) {
            Append(sub, kLockAllDocks, L"Lock all docks", state.anyUnlocked);
            Append(sub, kUnlockAllDocks, L"Unlock all docks", state.anyLocked);
        }
        AppendSub(menu, sub, L"All docks");
    }
    if (HMENU sub = CreatePopupMenu()) {
        Append(sub, kOpenSettings, L"App settings…", true);
        Append(sub, kHideTaskbar, L"Hide taskbar", true, state.hideTaskbar);
        Append(sub, kStartWithWindows, L"Start with Windows", true, state.startWithWindows);
        // Session-only: unchecks "Hide taskbar" until it is checked again (or the dock restarts).
        Append(sub, kRestoreTaskbar, L"Show Windows taskbar temporarily", state.hideTaskbar);
        Append(sub, kQuickSetup, L"Quick setup\x2026", true);  // round 12 phase 12G
        AppendSub(menu, sub, L"App");
    }
    AppendTools(menu, state.hasWidgets);
    Append(menu, kHelpMenu, L"Help…", true);  // round 9 phase 9D: the Help window (quick start)
    Append(menu, kQuit, L"Quit", true);
}

UINT Track(HWND owner, HMENU menu, POINT pt) {
    // Required so the menu closes when the user clicks elsewhere (KB135788).
    SetForegroundWindow(owner);
    UINT flags = TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN;
    flags |= GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    UINT cmd = static_cast<UINT>(TrackPopupMenuEx(menu, flags, pt.x, pt.y, owner, nullptr));
    PostMessageW(owner, WM_NULL, 0, 0);
    return cmd;
}

// Menu text: escape '&' and keep titles to a sane length.
std::wstring MenuText(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        if (c == L'&') out += L'&';
        out += c;
        if (out.size() > 80) {
            out += L"…";
            break;
        }
    }
    return out;
}

// Customize icon ▸ Choose icon… / Choose image… / Reset icon, for pinned items (not separators).
// Round 10 phase 10A: one submenu (they were three top-level entries).
void AppendIconEntries(HMENU menu, const ItemMenuState& item) {
    if (!item.pinned || item.kind == DockItem::Kind::Separator) return;
    HMENU sub = CreatePopupMenu();  // destroyed with its parent
    if (!sub) return;
    Append(sub, kChooseIcon, L"Choose icon…", true);
    Append(sub, kChooseImage, L"Choose image…", true);
    Append(sub, kResetIcon, L"Reset icon", item.hasIcon);
    AppendSub(menu, sub, L"Customize icon");
}

// Contents layout ▸ Automatic / Grid / List (the popup's layout of a folder / group item; round
// 10 phase 10A: was "View", which did not say what it changes).
void AppendViewMenu(HMENU menu, const ItemMenuState& item) {
    if (!item.pinned) return;
    HMENU sub = CreatePopupMenu();  // destroyed with its parent
    if (!sub) return;
    Append(sub, kViewAuto, L"Automatic", true);
    Append(sub, kViewGrid, L"Grid", true);
    Append(sub, kViewList, L"List", true);
    CheckMenuRadioItem(sub, kViewAuto, kViewList, kViewAuto + static_cast<UINT>(item.view), MF_BYCOMMAND);
    AppendSub(menu, sub, L"Contents layout");
}

void AppendHeader(HMENU menu, const std::wstring& name) {
    if (name.empty()) return;
    Append(menu, kNone, MenuText(name).c_str(), false);
    Separator(menu);
}

void AppendSettings(HMENU menu, const ItemMenuState& item) {
    if (!item.settingsText.empty()) Append(menu, kOpenItemSettings, MenuText(item.settingsText).c_str(), true);
}

void AppendRemove(HMENU menu, const ItemMenuState& item) {
    Separator(menu);
    Append(menu, kRemoveFromDock, L"Remove from dock", !item.locked);
}

}  // namespace

namespace {
// Round 12 phase 12A: Undo “<label>” and a separator at the top while there is something to undo.
void AppendUndo(HMENU menu, const std::wstring& label) {
    if (label.empty()) return;
    const std::wstring text = L"Undo “" + MenuText(label) + L"”";
    Append(menu, kUndo, text.c_str(), true);
    Separator(menu);
}
}  // namespace

UINT ShowDockMenu(HWND owner, POINT pt, const TrayMenuState& state) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return kNone;
    AppendUndo(menu, state.undoLabel);
    AppendDockEntries(menu, state);
    const UINT cmd = Track(owner, menu, pt);
    DestroyMenu(menu);
    return cmd;
}

UINT ShowAppMenu(HWND owner, POINT pt, const AppMenuState& state) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return kNone;
    AppendUndo(menu, state.undoLabel);
    Append(menu, kOpenSettings, L"App settings…", true);
    if (HMENU sub = CreatePopupMenu()) {
        for (size_t i = 0; i < state.docks.size() && i < 200; ++i) {
            Append(sub, kDockSettingsBase + static_cast<UINT>(i), MenuText(state.docks[i]).c_str(), true);
        }
        AppendSub(menu, sub, L"Docks", !state.docks.empty());
    }
    Append(menu, kToggleDashboard, L"Show dashboard", true, state.dashboard);
    Separator(menu);
    // The user's wish: directly here, not in a submenu (session-only; greyed while the taskbar shows).
    Append(menu, kRestoreTaskbar, L"Show Windows taskbar temporarily", state.hideTaskbar);
    Append(menu, kHideTaskbar, L"Hide taskbar", true, state.hideTaskbar);
    Append(menu, kStartWithWindows, L"Start with Windows", true, state.startWithWindows);
    Separator(menu);
    AppendTools(menu, state.hasWidgets);
    Append(menu, kHelpMenu, L"Help…", true);
    Append(menu, kQuit, L"Quit", true);
    const UINT cmd = Track(owner, menu, pt);
    DestroyMenu(menu);
    return cmd;
}

UINT ShowItemMenu(HWND owner, POINT pt, const std::wstring& appName, const std::vector<std::wstring>& titles,
                  const ItemMenuState& item, const TrayMenuState& state) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return kNone;
    switch (item.kind) {
        case DockItem::Kind::Folder:
            AppendHeader(menu, appName);
            Append(menu, kOpenInExplorer, L"Open in Explorer", true);
            Append(menu, kToggleFolderPopup, L"Show contents", true);
            AppendViewMenu(menu, item);
            AppendIconEntries(menu, item);
            Append(menu, kOpenItemSettings, L"Folder settings…", item.pinned);  // its own window
            Separator(menu);
            Append(menu, kTogglePin, L"Unpin from dock", item.pinned);
            break;
        case DockItem::Kind::Separator:
            Append(menu, kTogglePin, L"Remove separator", item.pinned);
            break;
        case DockItem::Kind::Group: {
            AppendHeader(menu, appName);
            Append(menu, kToggleFolderPopup, L"Show contents", true);
            Append(menu, kRenameGroup, L"Rename…", true);
            AppendViewMenu(menu, item);
            AppendIconEntries(menu, item);
            Append(menu, kOpenItemSettings, L"Group settings…", item.pinned);  // its own window
            Separator(menu);
            // The explanation stays in the label (the user's wish): ungrouping keeps the shortcuts.
            Append(menu, kUngroup, L"Ungroup (shortcuts stay pinned)", item.members > 0);
            const std::wstring unpin = item.members == 0 ? std::wstring(L"Unpin group")
                                       : item.members == 1
                                           ? std::wstring(L"Unpin group and its shortcut")
                                           : L"Unpin group and its " + std::to_wstring(item.members) + L" shortcuts";
            Append(menu, kTogglePin, unpin.c_str(), item.pinned);
            break;
        }
        case DockItem::Kind::RecycleBin:
            Append(menu, kNewWindow, L"Open", true);
            Append(menu, kEmptyRecycleBin, L"Empty Recycle Bin", !item.binEmpty);
            AppendIconEntries(menu, item);
            Separator(menu);
            Append(menu, kTogglePin, L"Unpin from dock", item.pinned);
            break;
        // Round 10 phase 10A: the widget kinds get a menu of their own (they showed the whole dock
        // menu with one "<name> settings…" entry on top).
        case DockItem::Kind::Weather:
            Append(menu, kRefreshWeather, L"Refresh weather", item.weatherConfigured);
            Append(menu, kToggleWeatherPopup, L"Show forecast", true);
            AppendSettings(menu, item);
            AppendRemove(menu, item);
            break;
        case DockItem::Kind::Clock:
            Append(menu, kToggleCalendar, L"Show calendar", true);
            AppendSettings(menu, item);
            AppendRemove(menu, item);
            break;
        case DockItem::Kind::Widget:
            AppendSettings(menu, item);
            Append(menu, kReloadWidget, L"Reload widget", true);
            AppendRemove(menu, item);
            break;
        case DockItem::Kind::Tray:
            AppendSettings(menu, item);
            AppendRemove(menu, item);
            break;
        case DockItem::Kind::Notifications:
            Append(menu, kToggleCentre, L"Show notification centre", true);
            AppendSettings(menu, item);
            AppendRemove(menu, item);
            break;
        case DockItem::Kind::App:
        default: {
            AppendHeader(menu, appName);
            Append(menu, kNewWindow, item.running ? L"New window" : L"Open", item.canLaunch);
            if (item.canOpenLocation) Append(menu, kOpenFileLocation, L"Open file location", true);
            if (!titles.empty()) Separator(menu);
            for (size_t i = 0; i < titles.size() && i < 200; ++i) {
                const std::wstring text = L"Close “" + MenuText(titles[i].empty() ? appName : titles[i]) + L"”";
                Append(menu, kCloseWindowBase + static_cast<UINT>(i), text.c_str(), true);
            }
            if (titles.size() > 1) Append(menu, kCloseAllWindows, L"Close all windows", true);
            AppendIconEntries(menu, item);
            Separator(menu);
            Append(menu, kTogglePin, item.pinned ? L"Unpin from dock" : L"Pin to dock", item.pinned || item.canPin);
            break;
        }
    }
    // The shared dock commands in one submenu (round 10 phase 10A: the whole dock menu was appended).
    Separator(menu);
    if (HMENU sub = CreatePopupMenu()) {
        AppendDockEntries(sub, state);
        AppendSub(menu, sub, L"Dock");
    }
    const UINT cmd = Track(owner, menu, pt);
    DestroyMenu(menu);
    return cmd;
}

}  // namespace ContextMenu
