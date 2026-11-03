#include "ContextMenu.h"

namespace ContextMenu {

namespace {

void Append(HMENU menu, UINT id, const wchar_t* text, bool enabled, bool checked = false) {
    UINT flags = MF_STRING;
    if (!enabled) flags |= MF_GRAYED;
    if (checked) flags |= MF_CHECKED;
    AppendMenuW(menu, flags, id, text);
}

void AppendDockEntries(HMENU menu, const TrayMenuState& state) {
    Append(menu, kOpenSettings, L"Settings…", true);
    Append(menu, kLockDock, L"Lock dock", true, state.locked);
    if (state.dockCount > 1) {
        Append(menu, kLockAllDocks, L"Lock all docks", state.anyUnlocked);
        Append(menu, kUnlockAllDocks, L"Unlock all docks", state.anyLocked);
    }
    Append(menu, kAddSeparator, L"Add separator", !state.locked);
    if (HMENU sub = CreatePopupMenu()) {  // destroyed with its parent
        Append(sub, kAddClock, L"Clock", true);
        Append(sub, kAddWeather, L"Weather", true);
        Append(sub, kAddRunning, L"Running apps", !state.hasRunning);
        Append(sub, kAddTray, L"Tray (notify icons)", !state.hasTray);
        UINT flags = MF_POPUP | MF_STRING;
        if (state.locked) flags |= MF_GRAYED;
        AppendMenuW(menu, flags, reinterpret_cast<UINT_PTR>(sub), L"Add widget");
    }
    if (HMENU sub = CreatePopupMenu()) {  // destroyed with its parent
        Append(sub, kEdgeBottom, L"Bottom", true);
        Append(sub, kEdgeTop, L"Top", true);
        Append(sub, kEdgeLeft, L"Left", true);
        Append(sub, kEdgeRight, L"Right", true);
        CheckMenuRadioItem(sub, kEdgeBottom, kEdgeRight, kEdgeBottom + static_cast<UINT>(state.edge), MF_BYCOMMAND);
        AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
        // How to move it along the edge (or to another one): a hint, not a command.
        Append(sub, kNone, state.locked ? L"Unlock the dock to move it by dragging" : L"Ctrl+drag the dock to move it",
               false);
        AppendMenuW(menu, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(sub), L"Position on screen");
    }
    if (state.weatherShown) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        Append(menu, kRefreshWeather, L"Refresh weather", state.weatherConfigured);
        Append(menu, kWeatherSettings, L"Weather settings…", true);
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    if (HMENU sub = CreatePopupMenu()) {  // destroyed with its parent
        Append(sub, kAutoHideOff, L"Off", true);
        Append(sub, kAutoHideAlways, L"Always", true);
        Append(sub, kAutoHideIntelligent, L"When a window overlaps", true);
        const UINT checked = state.autoHide == AutoHideMode::Intelligent ? kAutoHideIntelligent
                             : state.autoHide == AutoHideMode::Always    ? kAutoHideAlways
                                                                         : kAutoHideOff;
        CheckMenuRadioItem(sub, kAutoHideOff, kAutoHideIntelligent, checked, MF_BYCOMMAND);
        AppendMenuW(menu, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(sub), L"Auto-hide");
    }
    Append(menu, kHideTaskbar, L"Hide taskbar", true, state.hideTaskbar);
    Append(menu, kStartWithWindows, L"Start with Windows", true, state.startWithWindows);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    Append(menu, kOpenConfig, L"Open config file", true);
    Append(menu, kReloadConfig, L"Reload config", true);
    if (state.hasWidgets) {
        Append(menu, kReloadWidgets, L"Reload widgets", true);
        Append(menu, kWidgetConsole, L"Widget console…", true);
    }
    // Session-only: unchecks "Hide taskbar" until it is checked again (or the dock restarts).
    Append(menu, kRestoreTaskbar, L"Show taskbar until re-enabled", state.hideTaskbar);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
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

// Choose icon… / Choose image… / Reset icon, for pinned items (not separators).
void AppendIconEntries(HMENU menu, const ItemMenuState& item) {
    if (!item.pinned || item.kind == DockItem::Kind::Separator) return;
    Append(menu, kChooseIcon, L"Choose icon…", true);
    Append(menu, kChooseImage, L"Choose image…", true);
    Append(menu, kResetIcon, L"Reset icon", item.hasIcon);
}

// View ▸ Automatic / Grid / List (the popup's layout of a folder item).
void AppendViewMenu(HMENU menu, const ItemMenuState& item) {
    if (!item.pinned) return;
    HMENU sub = CreatePopupMenu();  // destroyed with its parent
    if (!sub) return;
    Append(sub, kViewAuto, L"Automatic", true);
    Append(sub, kViewGrid, L"Grid", true);
    Append(sub, kViewList, L"List", true);
    CheckMenuRadioItem(sub, kViewAuto, kViewList, kViewAuto + static_cast<UINT>(item.view), MF_BYCOMMAND);
    AppendMenuW(menu, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(sub), L"View");
}

}  // namespace

UINT ShowTrayMenu(HWND owner, POINT pt, const TrayMenuState& state) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return kNone;
    AppendDockEntries(menu, state);
    const UINT cmd = Track(owner, menu, pt);
    DestroyMenu(menu);
    return cmd;
}

UINT ShowItemMenu(HWND owner, POINT pt, const std::wstring& appName, const std::vector<std::wstring>& titles,
                  const ItemMenuState& item, const TrayMenuState& state) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return kNone;
    // The special kinds: their own few entries, then the dock menu.
    if (item.kind != DockItem::Kind::App) {
        switch (item.kind) {
            case DockItem::Kind::Folder:
                if (!appName.empty()) {
                    Append(menu, kNone, MenuText(appName).c_str(), false);
                    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                }
                Append(menu, kTogglePin, L"Unpin from dock", item.pinned);
                AppendIconEntries(menu, item);
                Append(menu, kOpenInExplorer, L"Open in Explorer", true);
                AppendViewMenu(menu, item);
                break;
            case DockItem::Kind::Separator:
                Append(menu, kTogglePin, L"Remove", item.pinned);
                break;
            case DockItem::Kind::Group: {
                if (!appName.empty()) {
                    Append(menu, kNone, MenuText(appName).c_str(), false);
                    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                }
                Append(menu, kToggleFolderPopup, L"Show contents", true);
                Append(menu, kRenameGroup, L"Rename…", true);
                Append(menu, kUngroup, L"Ungroup (its shortcuts become pins)", item.members > 0);
                const std::wstring unpin = item.members == 0 ? std::wstring(L"Unpin group")
                                           : item.members == 1
                                               ? std::wstring(L"Unpin group and its shortcut")
                                               : L"Unpin group and its " + std::to_wstring(item.members) + L" shortcuts";
                Append(menu, kTogglePin, unpin.c_str(), item.pinned);
                AppendIconEntries(menu, item);
                AppendViewMenu(menu, item);
                break;
            }
            case DockItem::Kind::RecycleBin:
                Append(menu, kNewWindow, L"Open", true);
                Append(menu, kEmptyRecycleBin, L"Empty Recycle Bin", !item.binEmpty);
                Append(menu, kTogglePin, L"Unpin from dock", item.pinned);
                AppendIconEntries(menu, item);
                break;
            default:
                break;
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendDockEntries(menu, state);
        const UINT cmd = Track(owner, menu, pt);
        DestroyMenu(menu);
        return cmd;
    }
    if (!appName.empty()) {
        Append(menu, kNone, MenuText(appName).c_str(), false);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    Append(menu, kTogglePin, item.pinned ? L"Unpin from dock" : L"Pin to dock", item.pinned || item.canPin);
    AppendIconEntries(menu, item);
    Append(menu, kNewWindow, item.running ? L"New window" : L"Open", item.canLaunch);
    if (item.canOpenLocation) Append(menu, kOpenFileLocation, L"Open file location", true);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    for (size_t i = 0; i < titles.size() && i < 200; ++i) {
        const std::wstring text = L"Close “" + MenuText(titles[i].empty() ? appName : titles[i]) + L"”";
        Append(menu, kCloseWindowBase + static_cast<UINT>(i), text.c_str(), true);
    }
    if (titles.size() > 1) Append(menu, kCloseAllWindows, L"Close all windows", true);
    if (!titles.empty()) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendDockEntries(menu, state);
    const UINT cmd = Track(owner, menu, pt);
    DestroyMenu(menu);
    return cmd;
}

}  // namespace ContextMenu
