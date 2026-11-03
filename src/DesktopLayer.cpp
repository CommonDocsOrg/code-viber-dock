#include "DesktopLayer.h"

#include <dwmapi.h>

#include <algorithm>
#include <cwchar>
#include <vector>

namespace {

bool HasClass(HWND hwnd, const wchar_t* cls) {
    wchar_t buf[32]{};
    return hwnd && GetClassNameW(hwnd, buf, 32) > 0 && wcscmp(buf, cls) == 0;
}

bool Own(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid == GetCurrentProcessId();
}

bool Cloaked(HWND hwnd) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
}

struct Entry {
    HWND hwnd = nullptr;
    HWND under = nullptr;
};

// One UI thread uses the layer (WinEvent callbacks carry no context).
std::vector<Entry>& Entries() {
    static std::vector<Entry> entries;
    return entries;
}
bool g_placing = false;          // our own SetWindowPos calls (FilterPosChanging lets them through)
HWINEVENTHOOK g_hook = nullptr;  // EVENT_SYSTEM_FOREGROUND, while a desktop window exists
HWND g_desktop = nullptr;        // the Progman the windows were last placed above

Entry* Find(HWND hwnd) {
    for (Entry& e : Entries()) {
        if (e.hwnd == hwnd) return &e;
    }
    return nullptr;
}

void SetPos(HWND hwnd, HWND after) {
    g_placing = true;
    SetWindowPos(hwnd, after, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    g_placing = false;
}

void CALLBACK OnForeground(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG, DWORD, DWORD) {
    if (event != EVENT_SYSTEM_FOREGROUND || idObject != OBJID_WINDOW) return;
    // Another process's window came forward (WINEVENT_SKIPOWNPROCESS): the desktop itself (Win+D,
    // a click on the wallpaper may raise it) or a new desktop window (Explorer restarted).
    HWND root = hwnd ? GetAncestor(hwnd, GA_ROOT) : nullptr;
    const bool desktop = HasClass(root, L"Progman") || HasClass(root, L"WorkerW");
    if (desktop || DesktopLayer::Desktop() != g_desktop) DesktopLayer::Reassert();
}

void SyncHook() {
    if (!Entries().empty() && !g_hook) {
        g_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, &OnForeground, 0, 0,
                                 WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    } else if (Entries().empty() && g_hook) {
        UnhookWinEvent(g_hook);
        g_hook = nullptr;
    }
}

}  // namespace

HWND DesktopLayer::Desktop() { return FindWindowW(L"Progman", nullptr); }

bool DesktopLayer::DesktopForeground() {
    HWND fg = GetForegroundWindow();
    if (!fg) return true;
    if (Own(fg)) return true;
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

// --- desktop windows ---

void DesktopLayer::Register(HWND hwnd, HWND under) {
    if (!hwnd) return;
    if (Entry* e = Find(hwnd)) {
        e->under = under;
    } else {
        Entries().push_back(Entry{hwnd, under});
    }
    SyncHook();
    Place(hwnd);
}

void DesktopLayer::Unregister(HWND hwnd) {
    std::erase_if(Entries(), [&](const Entry& e) { return e.hwnd == hwnd; });
    SyncHook();
}

void DesktopLayer::SetUnder(HWND hwnd, HWND under) {
    if (Entry* e = Find(hwnd)) {
        e->under = under;
        Place(hwnd);
    }
}

void DesktopLayer::FilterPosChanging(HWND hwnd, WINDOWPOS* pos) {
    if (g_placing || !pos || (pos->flags & SWP_NOZORDER) || !Find(hwnd)) return;
    // Activation ("bring to top"), an owned window's group moving up, a scripted HWND_TOP: the
    // window keeps its place at the bottom; the move / size / show parts still happen.
    pos->flags |= SWP_NOZORDER;
}

bool DesktopLayer::AboveDesktop(HWND hwnd) {
    HWND desktop = Desktop();
    if (!hwnd || !desktop || (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST)) return false;
    for (HWND w = GetWindow(hwnd, GW_HWNDNEXT); w; w = GetWindow(w, GW_HWNDNEXT)) {
        if (w == desktop) return true;
        if (IsWindowVisible(w) && !Own(w) && !Cloaked(w)) return false;  // something of another app between
    }
    return false;  // under the desktop (or no desktop below)
}

bool DesktopLayer::Place(HWND hwnd) {
    Entry* e = Find(hwnd);
    if (!e) return false;
    HWND desktop = Desktop();
    g_desktop = desktop;
    if (!desktop) {
        SetPos(hwnd, HWND_BOTTOM);
        if (e->under) SetPos(e->under, hwnd);
        return true;
    }
    const bool underOk = !e->under || GetWindow(hwnd, GW_HWNDNEXT) == e->under;
    if (AboveDesktop(hwnd) && underOk) return false;
    // Above the run of our own windows directly above the desktop (the other folder windows, their
    // backdrops, desktop docks): after the first window of another process above it.
    HWND after = GetWindow(desktop, GW_HWNDPREV);
    while (after && Own(after)) after = GetWindow(after, GW_HWNDPREV);
    SetPos(hwnd, after ? after : HWND_TOP);
    if (e->under) SetPos(e->under, hwnd);
    return true;
}

void DesktopLayer::Reassert() {
    const std::vector<Entry> entries = Entries();  // Place may run window messages
    for (const Entry& e : entries) {
        if (IsWindow(e.hwnd)) Place(e.hwnd);
    }
}

int DesktopLayer::SimulateExplorerRestart() {
    // Progman stays at the very bottom of the z-order (a window sent to HWND_BOTTOM lands directly
    // above it: measured on 26300), so a new desktop window cannot end up above ours; what a
    // restart can do is leave them somewhere else in the z-order. The hook lifts them to the top of
    // the non-topmost band (other apps' windows between them and the desktop), then runs the
    // recovery that TaskbarCreated runs.
    const std::vector<Entry> entries = Entries();
    bool under = !entries.empty();
    // Right above the first visible window of another app (not topmost) from the top.
    HWND foreign = GetTopWindow(nullptr);
    while (foreign && (!IsWindowVisible(foreign) || Own(foreign) || Cloaked(foreign) ||
                       (GetWindowLongPtrW(foreign, GWL_EXSTYLE) & WS_EX_TOPMOST))) {
        foreign = GetWindow(foreign, GW_HWNDNEXT);
    }
    HWND after = foreign ? GetWindow(foreign, GW_HWNDPREV) : nullptr;
    for (const Entry& e : entries) {
        SetPos(e.hwnd, after ? after : HWND_TOP);
        if (e.under) SetPos(e.under, e.hwnd);
    }
    for (const Entry& e : entries) under = under && !AboveDesktop(e.hwnd);
    g_desktop = nullptr;  // as if Progman were a new window
    Reassert();
    bool above = !entries.empty();
    for (const Entry& e : entries) above = above && AboveDesktop(e.hwnd);
    return (under ? 1 : 0) | (above ? 2 : 0);
}

size_t DesktopLayer::Count() { return Entries().size(); }
