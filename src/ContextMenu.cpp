#include "ContextMenu.h"

namespace ContextMenu {

namespace {

void Append(HMENU menu, UINT id, const wchar_t* text, bool enabled, bool checked = false) {
    UINT flags = MF_STRING;
    if (!enabled) flags |= MF_GRAYED;
    if (checked) flags |= MF_CHECKED;
    AppendMenuW(menu, flags, id, text);
}

}  // namespace

UINT ShowTrayMenu(HWND owner, POINT pt, const TrayMenuState& state) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return kNone;

    // Toggles are wired in later phases; shown disabled so the menu shape is final.
    Append(menu, kAutoHide, L"Auto-hide", false, state.autoHide);
    Append(menu, kHideTaskbar, L"Hide taskbar", false, state.hideTaskbar);
    Append(menu, kStartWithWindows, L"Start with Windows", false, state.startWithWindows);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    Append(menu, kOpenConfig, L"Open config file", true);
    Append(menu, kRestoreTaskbar, L"Restore taskbar", false);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    Append(menu, kQuit, L"Quit", true);

    // Required so the menu closes when the user clicks elsewhere (KB135788).
    SetForegroundWindow(owner);
    UINT flags = TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN;
    flags |= GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    UINT cmd = static_cast<UINT>(TrackPopupMenuEx(menu, flags, pt.x, pt.y, owner, nullptr));
    PostMessageW(owner, WM_NULL, 0, 0);

    DestroyMenu(menu);
    return cmd;
}

}  // namespace ContextMenu
