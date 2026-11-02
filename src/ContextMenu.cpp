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
    Append(menu, kAutoHide, L"Auto-hide", true, state.autoHide);
    Append(menu, kHideTaskbar, L"Hide taskbar", true, state.hideTaskbar);
    Append(menu, kStartWithWindows, L"Start with Windows", true, state.startWithWindows);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    Append(menu, kOpenConfig, L"Open config file", true);
    Append(menu, kReloadConfig, L"Reload config", true);
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
    if (!appName.empty()) {
        Append(menu, kNone, MenuText(appName).c_str(), false);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    Append(menu, kTogglePin, item.pinned ? L"Unpin from dock" : L"Pin to dock", item.pinned || item.canPin);
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
