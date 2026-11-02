#include "Activation.h"

#include <algorithm>

namespace Activation {

namespace {

void SendAlt(bool down) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = VK_MENU;
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(in));
}

bool IsForeground(HWND hwnd) {
    HWND fg = GetForegroundWindow();
    return fg && (fg == hwnd || GetAncestor(fg, GA_ROOTOWNER) == hwnd);
}

}  // namespace

bool BringToFront(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;

    if (IsIconic(hwnd)) ShowWindowAsync(hwnd, SW_RESTORE);
    if (SetForegroundWindow(hwnd)) return true;

    // Foreground lock: a synthetic Alt press makes us the last-input process. Release it only
    // after the switch so the previously active app doesn't see an Alt tap (menu-bar focus).
    SendAlt(true);
    const bool ok = SetForegroundWindow(hwnd) != FALSE;
    SendAlt(false);
    if (ok) return true;

    // Last resort: share input state with the current foreground thread.
    const DWORD self = GetCurrentThreadId();
    const DWORD fgThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    bool attached = false;
    if (fgThread && fgThread != self) attached = AttachThreadInput(self, fgThread, TRUE) != FALSE;
    BringWindowToTop(hwnd);
    const bool ok2 = SetForegroundWindow(hwnd) != FALSE;
    if (attached) AttachThreadInput(self, fgThread, FALSE);
    if (!ok2) OutputDebugStringW(L"WinDock: SetForegroundWindow refused\n");
    return ok2;
}

void Minimize(HWND hwnd) {
    if (hwnd && IsWindow(hwnd)) ShowWindowAsync(hwnd, SW_MINIMIZE);
}

void ActivateItem(const std::vector<HWND>& windows) {
    if (windows.empty()) return;

    if (windows.size() == 1) {
        HWND w = windows.front();
        if (IsForeground(w) && !IsIconic(w)) {
            Minimize(w);
        } else {
            BringToFront(w);
        }
        return;
    }

    size_t next = 0;
    for (size_t i = 0; i < windows.size(); ++i) {
        if (IsForeground(windows[i])) {
            next = (i + 1) % windows.size();
            break;
        }
    }
    BringToFront(windows[next]);
}

}  // namespace Activation
