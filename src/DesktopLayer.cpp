#include "DesktopLayer.h"

#include <cwchar>

namespace {

bool HasClass(HWND hwnd, const wchar_t* cls) {
    wchar_t buf[32]{};
    return hwnd && GetClassNameW(hwnd, buf, 32) > 0 && wcscmp(buf, cls) == 0;
}

}  // namespace

HWND DesktopLayer::Desktop() { return FindWindowW(L"Progman", nullptr); }

bool DesktopLayer::DesktopForeground() {
    HWND fg = GetForegroundWindow();
    if (!fg) return true;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) return true;
    HWND root = GetAncestor(fg, GA_ROOT);
    return HasClass(root, L"Progman") || HasClass(root, L"WorkerW");
}

void DesktopLayer::PlaceAboveDesktop(HWND hwnd) {
    if (!hwnd) return;
    constexpr UINT kFlags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    HWND desktop = Desktop();
    if (!desktop) {
        SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, kFlags);
        return;
    }
    HWND above = GetWindow(desktop, GW_HWNDPREV);
    if (above == hwnd && !(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST)) return;  // already there
    // Placed after (below) the window that is above the desktop: between it and the desktop. A
    // topmost window placed after a non-topmost one loses its topmost state.
    if (above == hwnd) above = GetWindow(hwnd, GW_HWNDPREV);
    if (!above) {
        SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, kFlags);
        return;
    }
    SetWindowPos(hwnd, above, 0, 0, 0, 0, kFlags);
}
