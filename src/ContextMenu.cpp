#include "ContextMenu.h"

#include "MenuIcons.h"

#include <algorithm>

namespace ContextMenu {

namespace {

// --- round 13 phase 13B: menu icons ---
// The one table of Segoe Fluent Icons codepoints (Segoe MDL2 Assets has the same ones), so a
// command has the same picture in the dock menu, an item menu's Dock submenu and the tray-icon
// menu. Radio entries (Position, Auto-hide, Contents layout) and most check entries have none:
// the gutter shows their check mark.
namespace G {
constexpr wchar_t kCustomize = 0xE70F;     // Edit (pencil)
constexpr wchar_t kSettings = 0xE713;      // Settings (gear)
constexpr wchar_t kAdd = 0xE710;           // Add
constexpr wchar_t kPosition = 0xE707;      // MapPin
constexpr wchar_t kAutoHide = 0xE8A0;      // OpenPane
constexpr wchar_t kLock = 0xE72E;          // Lock
constexpr wchar_t kUnlock = 0xE785;        // Unlock
constexpr wchar_t kRename = 0xE8AC;        // Rename
constexpr wchar_t kDuplicate = 0xE8C8;     // Copy
constexpr wchar_t kSave = 0xE74E;          // Save
constexpr wchar_t kAllDocks = 0xE8A1;      // PreviewLink
constexpr wchar_t kTools = 0xE90F;         // Repair
constexpr wchar_t kHelp = 0xE897;          // Help
constexpr wchar_t kQuit = 0xE8BB;          // ChromeClose
constexpr wchar_t kUndo = 0xE7A7;          // Undo
constexpr wchar_t kOpen = 0xE8A7;          // OpenInNewWindow
constexpr wchar_t kNewWindow = 0xE8A5;     // the plan's pick for New window
constexpr wchar_t kClose = 0xE8BB;         // ChromeClose
constexpr wchar_t kFileLocation = 0xE8DA;  // OpenLocal
constexpr wchar_t kFolderOpen = 0xE838;    // FolderOpen
constexpr wchar_t kPin = 0xE718;           // Pin
constexpr wchar_t kUnpin = 0xE77A;         // Unpin
constexpr wchar_t kRemove = 0xE74D;        // Delete
constexpr wchar_t kReload = 0xE72C;        // Refresh
constexpr wchar_t kShow = 0xE7B3;          // RedEye
constexpr wchar_t kCalendar = 0xE787;      // Calendar
constexpr wchar_t kForecast = 0xE9C3;      // the plan's pick for Show forecast
constexpr wchar_t kTray = 0xE8B8;          // the plan's pick for the tray
constexpr wchar_t kNotifications = 0xE7E7; // Ringer (bell)
constexpr wchar_t kWidget = 0xE80F;        // the plan's pick for a script widget
constexpr wchar_t kClock = 0xE823;         // Recent (clock)
constexpr wchar_t kWeather = 0xE706;       // Brightness (sun)
constexpr wchar_t kRunning = 0xE7C4;       // TaskView
constexpr wchar_t kDock = 0xE7F4;          // the item menus' Dock submenu
constexpr wchar_t kFolder = 0xE8B7;        // Folder
constexpr wchar_t kGroup = 0xE8A9;         // ViewAll
constexpr wchar_t kApp = 0xE7AC;           // OpenWith (an app without an icon of its own)
constexpr wchar_t kSeparator = 0xE76F;     // GripperBarHorizontal
constexpr wchar_t kIcon = 0xE91B;          // Photo (Customize icon)
constexpr wchar_t kTaskManager = 0xE9D9;   // Diagnostic
}  // namespace G

// The menu being built: the owner's DPI and the glyphs' colour (set by Begin, one menu at a time
// on the UI thread).
struct Building {
    UINT dpi = 96;
    COLORREF text = 0;
};
Building g_menu;

void Begin(HWND owner) {
    g_menu.dpi = owner ? GetDpiForWindow(owner) : 96;
    if (!g_menu.dpi) g_menu.dpi = 96;
    g_menu.text = MenuIcons::TextColour(owner);
}

// The picture of the entry just appended (by position: kNone is shared by several entries).
void SetBitmap(HMENU menu, HBITMAP bitmap) {
    if (!bitmap) return;
    const int count = GetMenuItemCount(menu);
    if (count <= 0) return;
    MENUITEMINFOW mii{sizeof(mii)};
    mii.fMask = MIIM_BITMAP;
    mii.hbmpItem = bitmap;
    SetMenuItemInfoW(menu, static_cast<UINT>(count - 1), TRUE, &mii);
}

void SetGlyph(HMENU menu, wchar_t glyph) {
    if (glyph) SetBitmap(menu, MenuIcons::Glyph(glyph, g_menu.dpi, g_menu.text));
}
// --- end round 13 phase 13B ---

void Append(HMENU menu, UINT id, const wchar_t* text, bool enabled, bool checked = false, wchar_t glyph = 0) {
    UINT flags = MF_STRING;
    if (!enabled) flags |= MF_GRAYED;
    if (checked) flags |= MF_CHECKED;
    if (AppendMenuW(menu, flags, id, text)) SetGlyph(menu, glyph);
}

void Separator(HMENU menu) { AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); }

// A submenu `sub` (destroyed with its parent) under `text`.
void AppendSub(HMENU menu, HMENU sub, const wchar_t* text, bool enabled = true, wchar_t glyph = 0) {
    if (AppendMenuW(menu, MF_POPUP | MF_STRING | (enabled ? 0 : MF_GRAYED), reinterpret_cast<UINT_PTR>(sub), text)) {
        SetGlyph(menu, glyph);
    }
}

// Tools ▸ (round 10 phase 10A): the dock menu's and the tray-icon menu's. Reload widgets and the
// Console stay in place greyed without a script widget, so the positions do not move.
void AppendTools(HMENU menu, bool hasWidgets) {
    HMENU sub = CreatePopupMenu();
    if (!sub) return;
    Append(sub, kOpenConfig, L"Open config file", true);
    Append(sub, kReloadConfig, L"Reload config", true, false, G::kReload);
    Append(sub, kReloadWidgets, L"Reload widgets", hasWidgets, false, G::kReload);
    Append(sub, kWidgetConsole, L"Widget console…", hasWidgets);
    Separator(sub);
    Append(sub, kTaskManager, L"Task Manager", true, false, G::kTaskManager);  // round 7 phase 7A
    AppendSub(menu, sub, L"Tools", true, G::kTools);
}

// Round 10 phase 10A: the dock menu grouped by task (was one long list; every command kept). The
// first group is this dock's, after the separator what goes beyond it.
void AppendDockEntries(HMENU menu, const TrayMenuState& state) {
    // Round 12 phase 12D: customize mode first (under Undo "<label>" when there is one).
    if (state.canEdit) Append(menu, kCustomize, state.editing ? L"Done customizing" : L"Customize this dock", true, false,
                                G::kCustomize);
    Append(menu, kDockSettings, L"Dock settings…", true, false, G::kSettings);  // round 5 phase B: this dock's window
    if (HMENU sub = CreatePopupMenu()) {
        // The dock's own items (greyed while it is locked, as Add widget was), then the drawer and
        // a folder window (neither touches this dock's items: never greyed).
        const bool unlocked = !state.locked;
        Append(sub, kAddClock, L"Clock", unlocked, false, G::kClock);
        Append(sub, kAddWeather, L"Weather (script)", unlocked, false, G::kWeather);
        Append(sub, kAddRunning, L"Running apps", unlocked && !state.hasRunning, false, G::kRunning);
        Append(sub, kAddTray, L"Tray (notify icons)", unlocked && !state.hasTray, false, G::kTray);
        Append(sub, kAddNotifications, L"Notification centre", unlocked && !state.hasNotifications, false,
               G::kNotifications);
        Append(sub, kAddSeparator, L"Separator", unlocked, false, G::kSeparator);
        Separator(sub);
        Append(sub, kAddFromDrawer, L"Apps, folders and more widgets…", true, false, G::kApp);
        Separator(sub);
        Append(sub, kNewFolderWindow, L"Folder window on the desktop…", true, false, G::kFolder);
        AppendSub(menu, sub, L"Add", true, G::kAdd);
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
        AppendSub(menu, sub, L"Position on screen", true, G::kPosition);
    }
    if (HMENU sub = CreatePopupMenu()) {
        Append(sub, kAutoHideOff, L"Off", true);
        Append(sub, kAutoHideAlways, L"Always", true);
        Append(sub, kAutoHideIntelligent, L"When a window overlaps", true);
        const UINT checked = state.autoHide == AutoHideMode::Intelligent ? kAutoHideIntelligent
                             : state.autoHide == AutoHideMode::Always    ? kAutoHideAlways
                                                                         : kAutoHideOff;
        CheckMenuRadioItem(sub, kAutoHideOff, kAutoHideIntelligent, checked, MF_BYCOMMAND);
        AppendSub(menu, sub, L"Auto-hide", true, G::kAutoHide);
    }
    Append(menu, kLockDock, L"Lock dock", true, state.locked, state.locked ? G::kLock : G::kUnlock);
    // --- round 12 phase 12C: docks you keep ---
    Append(menu, kRenameDock, L"Rename dock\x2026", true, false, G::kRename);
    Append(menu, kDuplicateDock, L"Duplicate dock", true, false, G::kDuplicate);
    Append(menu, kSaveDockLayout, L"Save dock as layout\x2026", true, false, G::kSave);
    // --- end round 12 phase 12C ---
    Separator(menu);
    if (HMENU sub = CreatePopupMenu()) {
        // Round 6 phase 6A: the desktop docks to the front (the dashboard hotkey does the same).
        Append(sub, kToggleDashboard, L"Show dashboard", true, state.dashboard);
        if (state.dockCount > 1) {
            Append(sub, kLockAllDocks, L"Lock all docks", state.anyUnlocked, false, G::kLock);
            Append(sub, kUnlockAllDocks, L"Unlock all docks", state.anyLocked, false, G::kUnlock);
        }
        AppendSub(menu, sub, L"All docks", true, G::kAllDocks);
    }
    if (HMENU sub = CreatePopupMenu()) {
        Append(sub, kOpenSettings, L"App settings…", true, false, G::kSettings);
        Append(sub, kHideTaskbar, L"Hide taskbar", true, state.hideTaskbar);
        Append(sub, kStartWithWindows, L"Start with Windows", true, state.startWithWindows);
        // Session-only: unchecks "Hide taskbar" until it is checked again (or the dock restarts).
        Append(sub, kRestoreTaskbar, L"Show Windows taskbar temporarily", state.hideTaskbar);
        Append(sub, kQuickSetup, L"Quick setup\x2026", true, false, G::kCustomize);  // round 12 phase 12G
        AppendSub(menu, sub, L"App");
        SetBitmap(menu, MenuIcons::AppIcon(g_menu.dpi));  // round 13 phase 13B: the exe's own icon
    }
    AppendTools(menu, state.hasWidgets);
    Append(menu, kHelpMenu, L"Help…", true, false, G::kHelp);  // round 9 phase 9D: the Help window (quick start)
    Append(menu, kQuit, L"Quit", true, false, G::kQuit);
}

UINT Track(HWND owner, HMENU menu, POINT pt) {
    // Required so the menu closes when the user clicks elsewhere (KB135788).
    SetForegroundWindow(owner);
    UINT flags = TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN;
    flags |= GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    // Round 13 phase 13A: TPM_NONOTIFY also suppresses WM_ENTERMENULOOP / WM_EXITMENULOOP to the
    // owner, so they are sent here: a dock owner then knows the menu is open and does not raise
    // itself above it (DockWindow::menuOpen_). Other owners pass them to DefWindowProc (no effect).
    SendMessageW(owner, WM_ENTERMENULOOP, TRUE, 0);
    UINT cmd = static_cast<UINT>(TrackPopupMenuEx(menu, flags, pt.x, pt.y, owner, nullptr));
    if (IsWindow(owner)) SendMessageW(owner, WM_EXITMENULOOP, TRUE, 0);
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
    Append(sub, kChooseImage, L"Choose image…", true, false, G::kIcon);
    Append(sub, kResetIcon, L"Reset icon", item.hasIcon, false, G::kUndo);
    AppendSub(menu, sub, L"Customize icon", true, G::kCustomize);
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

// Round 13 phase 13B: the disabled header entry with the item's picture (its own icon, else the
// kind's glyph). Still MF_GRAYED: the system dims the bitmap slightly (the plan accepts that).
void AppendHeader(HMENU menu, const ItemMenuState& item) {
    if (item.headerText.empty()) return;
    Append(menu, kNone, MenuText(item.headerText).c_str(), false);
    HBITMAP bitmap = item.headerIcon ? MenuIcons::FromIcon(item.headerIcon, g_menu.dpi) : nullptr;
    if (bitmap) {
        SetBitmap(menu, bitmap);
    } else {
        SetGlyph(menu, item.headerGlyph);
    }
    Separator(menu);
}

// Round 13 phase 13B: the widget kinds' shared tail: Widget settings… (was "<name> settings…"),
// Reload widget (script widgets), ─, Remove this widget (was "Remove from dock").
// Round 13 phase 13C: the last entry appended is a separator (or there is none).
bool EndsWithSeparator(HMENU menu) {
    const int count = GetMenuItemCount(menu);
    if (count <= 0) return true;
    MENUITEMINFOW mii{sizeof(mii)};
    mii.fMask = MIIM_FTYPE;
    return GetMenuItemInfoW(menu, static_cast<UINT>(count - 1), TRUE, &mii) && (mii.fType & MFT_SEPARATOR);
}

// Round 13 phase 13C: the widget's quick settings between separators: a bool is a checkable entry
// (the gutter's check mark, no glyph), a choice a submenu titled with its label holding a radio
// entry per choice. The entries' ids are kQuickBase + their index in item.quick.
void AppendQuick(HMENU menu, const ItemMenuState& item) {
    const std::vector<QuickEntry>& q = item.quick;
    const size_t n = std::min<size_t>(q.size(), kQuickMax);
    if (n == 0) return;
    if (!EndsWithSeparator(menu)) Separator(menu);
    for (size_t i = 0; i < n;) {
        if (q[i].group.empty()) {
            Append(menu, kQuickBase + static_cast<UINT>(i), MenuText(q[i].label).c_str(), true, q[i].checked);
            ++i;
            continue;
        }
        size_t j = i;
        HMENU sub = CreatePopupMenu();  // destroyed with its parent
        UINT checked = 0;
        for (; j < n && !q[j].group.empty() && q[j].key == q[i].key; ++j) {
            if (sub) Append(sub, kQuickBase + static_cast<UINT>(j), MenuText(q[j].label).c_str(), true);
            if (q[j].checked) checked = kQuickBase + static_cast<UINT>(j);
        }
        if (sub) {
            if (checked) CheckMenuRadioItem(sub, kQuickBase + static_cast<UINT>(i), kQuickBase + static_cast<UINT>(j - 1), checked, MF_BYCOMMAND);
            AppendSub(menu, sub, MenuText(q[i].group).c_str());
        }
        i = j;
    }
    Separator(menu);
}

void AppendWidgetTail(HMENU menu, const ItemMenuState& item) {
    AppendQuick(menu, item);  // round 13 phase 13C (13B left the place for them)
    if (item.hasSettings) Append(menu, kOpenItemSettings, L"Widget settings…", true, false, G::kSettings);
    if (item.kind == DockItem::Kind::Widget) Append(menu, kReloadWidget, L"Reload widget", true, false, G::kReload);
    Separator(menu);
    Append(menu, kRemoveFromDock, L"Remove this widget", !item.locked, false, G::kRemove);
}

}  // namespace

wchar_t KindGlyph(DockItem::Kind kind) {
    switch (kind) {
        case DockItem::Kind::Folder: return G::kFolder;
        case DockItem::Kind::Separator: return G::kSeparator;
        case DockItem::Kind::RecycleBin: return G::kRemove;
        case DockItem::Kind::Clock: return G::kClock;
        case DockItem::Kind::Weather: return G::kWeather;
        case DockItem::Kind::Widget: return G::kWidget;
        case DockItem::Kind::Group: return G::kGroup;
        case DockItem::Kind::Tray:
        case DockItem::Kind::TrayApp: return G::kTray;
        case DockItem::Kind::Notifications: return G::kNotifications;
        case DockItem::Kind::App:
        default: return G::kApp;
    }
}

namespace {
// Round 12 phase 12A: Undo “<label>” and a separator at the top while there is something to undo.
void AppendUndo(HMENU menu, const std::wstring& label) {
    if (label.empty()) return;
    const std::wstring text = L"Undo “" + MenuText(label) + L"”";
    Append(menu, kUndo, text.c_str(), true, false, G::kUndo);
    Separator(menu);
}

// Round 13 phase 13B: the menu is destroyed, then the item icons' bitmaps it used.
UINT TrackAndDestroy(HWND owner, HMENU menu, POINT pt) {
    const UINT cmd = Track(owner, menu, pt);
    DestroyMenu(menu);
    MenuIcons::ReleaseTransient();
    return cmd;
}
}  // namespace

UINT ShowDockMenu(HWND owner, POINT pt, const TrayMenuState& state) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return kNone;
    Begin(owner);
    AppendUndo(menu, state.undoLabel);
    AppendDockEntries(menu, state);
    return TrackAndDestroy(owner, menu, pt);
}

UINT ShowAppMenu(HWND owner, POINT pt, const AppMenuState& state) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return kNone;
    Begin(owner);
    AppendUndo(menu, state.undoLabel);
    Append(menu, kOpenSettings, L"App settings…", true, false, G::kSettings);
    if (HMENU sub = CreatePopupMenu()) {
        for (size_t i = 0; i < state.docks.size() && i < 200; ++i) {
            Append(sub, kDockSettingsBase + static_cast<UINT>(i), MenuText(state.docks[i]).c_str(), true);
        }
        AppendSub(menu, sub, L"Docks", !state.docks.empty(), G::kAllDocks);
    }
    Append(menu, kToggleDashboard, L"Show dashboard", true, state.dashboard);
    Separator(menu);
    // The user's wish: directly here, not in a submenu (session-only; greyed while the taskbar shows).
    Append(menu, kRestoreTaskbar, L"Show Windows taskbar temporarily", state.hideTaskbar);
    Append(menu, kHideTaskbar, L"Hide taskbar", true, state.hideTaskbar);
    Append(menu, kStartWithWindows, L"Start with Windows", true, state.startWithWindows);
    Separator(menu);
    AppendTools(menu, state.hasWidgets);
    Append(menu, kHelpMenu, L"Help…", true, false, G::kHelp);
    Append(menu, kQuit, L"Quit", true, false, G::kQuit);
    return TrackAndDestroy(owner, menu, pt);
}

UINT ShowItemMenu(HWND owner, POINT pt, const std::wstring& appName, const std::vector<std::wstring>& titles,
                  const ItemMenuState& item, const TrayMenuState& state) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return kNone;
    Begin(owner);
    switch (item.kind) {
        case DockItem::Kind::Folder:
            AppendHeader(menu, item);
            Append(menu, kOpenInExplorer, L"Open in Explorer", true, false, G::kFolderOpen);
            Append(menu, kToggleFolderPopup, L"Show contents", true, false, G::kShow);
            AppendViewMenu(menu, item);
            AppendIconEntries(menu, item);
            Append(menu, kOpenItemSettings, L"Folder settings…", item.pinned, false, G::kSettings);  // its own window
            Separator(menu);
            Append(menu, kTogglePin, L"Unpin from dock", item.pinned, false, G::kUnpin);
            break;
        case DockItem::Kind::Separator:
            Append(menu, kTogglePin, L"Remove separator", item.pinned, false, G::kRemove);
            break;
        case DockItem::Kind::Group: {
            AppendHeader(menu, item);
            Append(menu, kToggleFolderPopup, L"Show contents", true, false, G::kShow);
            Append(menu, kRenameGroup, L"Rename…", true, false, G::kRename);
            AppendViewMenu(menu, item);
            AppendIconEntries(menu, item);
            Append(menu, kOpenItemSettings, L"Group settings…", item.pinned, false, G::kSettings);  // its own window
            Separator(menu);
            // The explanation stays in the label (the user's wish): ungrouping keeps the shortcuts.
            Append(menu, kUngroup, L"Ungroup (shortcuts stay pinned)", item.members > 0);
            const std::wstring unpin = item.members == 0 ? std::wstring(L"Unpin group")
                                       : item.members == 1
                                           ? std::wstring(L"Unpin group and its shortcut")
                                           : L"Unpin group and its " + std::to_wstring(item.members) + L" shortcuts";
            Append(menu, kTogglePin, unpin.c_str(), item.pinned, false, G::kUnpin);
            break;
        }
        case DockItem::Kind::RecycleBin:
            AppendHeader(menu, item);  // round 13 phase 13B: it has a header now, as the other icons
            Append(menu, kNewWindow, L"Open", true, false, G::kOpen);
            Append(menu, kEmptyRecycleBin, L"Empty Recycle Bin", !item.binEmpty, false, G::kRemove);
            AppendIconEntries(menu, item);
            Separator(menu);
            Append(menu, kTogglePin, L"Unpin from dock", item.pinned, false, G::kUnpin);
            break;
        // Round 10 phase 10A: the widget kinds get a menu of their own (they showed the whole dock
        // menu with one "<name> settings…" entry on top). Round 13 phase 13B: "Widget: <name>"
        // header · the kind's entries · AppendWidgetTail.
        case DockItem::Kind::Weather:
            AppendHeader(menu, item);
            Append(menu, kRefreshWeather, L"Refresh weather", item.weatherConfigured, false, G::kReload);
            Append(menu, kToggleWeatherPopup, L"Show forecast", true, false, G::kForecast);
            AppendWidgetTail(menu, item);
            break;
        case DockItem::Kind::Clock:
            AppendHeader(menu, item);
            Append(menu, kToggleCalendar, L"Show calendar", true, false, G::kCalendar);
            AppendWidgetTail(menu, item);
            break;
        case DockItem::Kind::Widget:
        case DockItem::Kind::Tray:
            AppendHeader(menu, item);
            AppendWidgetTail(menu, item);
            break;
        case DockItem::Kind::Notifications:
            AppendHeader(menu, item);
            Append(menu, kToggleCentre, L"Show notification centre", true, false, G::kNotifications);
            AppendWidgetTail(menu, item);
            break;
        case DockItem::Kind::App:
        default: {
            AppendHeader(menu, item);
            Append(menu, kNewWindow, item.running ? L"New window" : L"Open", item.canLaunch, false,
                   item.running ? G::kNewWindow : G::kOpen);
            if (item.canOpenLocation) Append(menu, kOpenFileLocation, L"Open file location", true, false, G::kFileLocation);
            if (!titles.empty()) Separator(menu);
            for (size_t i = 0; i < titles.size() && i < 200; ++i) {
                const std::wstring text = L"Close “" + MenuText(titles[i].empty() ? appName : titles[i]) + L"”";
                Append(menu, kCloseWindowBase + static_cast<UINT>(i), text.c_str(), true);
            }
            if (titles.size() > 1) Append(menu, kCloseAllWindows, L"Close all windows", true, false, G::kClose);
            AppendIconEntries(menu, item);
            Separator(menu);
            Append(menu, kTogglePin, item.pinned ? L"Unpin from dock" : L"Pin to dock", item.pinned || item.canPin, false,
                   item.pinned ? G::kUnpin : G::kPin);
            break;
        }
    }
    // The shared dock commands in one submenu (round 10 phase 10A: the whole dock menu was appended).
    Separator(menu);
    if (HMENU sub = CreatePopupMenu()) {
        AppendDockEntries(sub, state);
        AppendSub(menu, sub, L"Dock", true, G::kDock);
    }
    return TrackAndDestroy(owner, menu, pt);
}

}  // namespace ContextMenu
