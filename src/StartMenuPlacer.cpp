#include "StartMenuPlacer.h"

#include <dwmapi.h>
#include <ShellScalingApi.h>
#include <UIAutomation.h>

#include <algorithm>
#include <cmath>
#include <cwchar>

// Round 16 phase 16A. Measured on Windows 11 Pro 26300 (2026-11-09; the user's 5120 x 1440 display
// at 96 DPI, the taskbar hidden by the dock, TaskbarAl not set = centred):
//
// - The Start menu lives in ONE full-screen host window: class "Windows.UI.Core.CoreWindow", title
//   "Start", process StartMenuExperienceHost.exe, rect 0,0 - 5120,1392 (the screen less the hidden
//   taskbar's 48 px band: Windows still lays Start out as if the taskbar were there). It exists all
//   the time; closed it is cloaked (DWMWA_CLOAKED = 2, IsWindowVisible true). Other CoreWindows of
//   the shell share the class (Search, New notification, Windows Input Experience ...): the title
//   and the process tell them apart.
// - After the Windows key the foreground window is SearchHost's "Search" CoreWindow (2131,550 -
//   2989,1440), the Start host is uncloaked; Escape (while that window is the foreground) closes it.
// - SetWindowPos(host, x, y, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) from another process moves
//   the menu with it (the search box included) and it stays (no snap-back 1.5 s later). The shift
//   PERSISTS while closed: the next Windows key opens the menu shifted too, so every path that
//   shifts the host must put it back (Restore, Shutdown, EmergencyRestore).
// - UI Automation (probe, this phase): ElementFromHandle(host) works while cloaked, the tree is
//   complete (the search box, the apps list, the pinned and recommended lists...), but every
//   BoundingRectangle is EMPTY (infinite / negative size) while the host is cloaked. So nothing of
//   the panel's size is known before the first open: Place falls back to the last rect read (below)
//   and, before any, to the anchor model's guess.
// - There is no single element for the panel: the host's direct children are the parts (raw view):
//   "SearchBoxToggleButton" (2035,531 736x32), "ShowHideCompanion" (2775,531 32x32), "TopLevelRoot"
//   (SemanticZoom, 2003,579 832x736), "UserControl" (the account / power row, 2055,1327 728x40), a
//   NamedContainerAutomationPeer group (the Phone Link pane, 2841,543 276x772),
//   "ActionBarOverflowButton" (3061,1327 40x40) and "CloseStartAccessibleButton" (always empty).
//   Their union (2003,531 - 3117,1367) is the panel's content, centred on the host (x 2560); its
//   bottom is 25 px above the host's bottom edge. The panel's frame reaches a little beyond the
//   content: kFrameDip (an estimate, not measured: no picture was taken).
// - Walking those children took ~34 ms from PowerShell; it is done once per open beside a button,
//   from the watch, after the opening animation (kSettleTicks).
//
// Not measured: a second display, TaskbarAl 0 (left), a taskbar on another edge, another DPI.

namespace {

constexpr wchar_t kHostClass[] = L"Windows.UI.Core.CoreWindow";
constexpr wchar_t kHostTitle[] = L"Start";
constexpr wchar_t kHostImage[] = L"StartMenuExperienceHost.exe";
constexpr UINT kWatchMs = 250;
constexpr int kOpenCapTicks = 12;  // 3 s: the key never opened the menu -> restore
constexpr int kSettleTicks = 2;    // read the panel 500 ms after it was seen open (the animation is over)
constexpr int kGapPx = 8;          // between the button's box and the menu, and the monitor margin
// The panel's frame beyond the union of its parts (an estimate: about the inner padding of the
// Windows 11 menu; not measured).
constexpr float kFrameDip = 12.0f;
// The anchor model's guess before the panel was ever read: the measured content size here plus the
// frame (1114 x 836 px at 96 DPI with the Phone Link pane shown; without it the panel is narrower),
// its bottom kBottomDip above the host's bottom edge, centred. A guess: the first open beside a
// button may be off by the difference, later ones use the rect read from UI Automation.
constexpr float kGuessWDip = 1138.0f;
constexpr float kGuessHDip = 860.0f;
constexpr float kBottomDip = 13.0f;

// Written by Place / Restore for EmergencyRestore (no allocation, no COM on the crash path).
HWND volatile g_shiftedHost = nullptr;
LONG volatile g_naturalX = 0;
LONG volatile g_naturalY = 0;

bool IsHostProcess(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return false;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return false;
    wchar_t path[MAX_PATH]{};
    DWORD n = MAX_PATH;
    const bool ok = QueryFullProcessImageNameW(p, 0, path, &n) != FALSE;
    CloseHandle(p);
    if (!ok) return false;
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return _wcsicmp(name, kHostImage) == 0;
}

bool IsHostWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    wchar_t cls[64]{};
    wchar_t title[16]{};
    GetClassNameW(hwnd, cls, 64);
    GetWindowTextW(hwnd, title, 16);
    return wcscmp(cls, kHostClass) == 0 && wcscmp(title, kHostTitle) == 0;
}

bool Cloaked(HWND hwnd) {
    DWORD cloaked = 0;
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) return true;
    return cloaked != 0;
}

float ScaleOf(HWND hwnd) {
    const UINT dpi = GetDpiForWindow(hwnd);
    return dpi ? static_cast<float>(dpi) / 96.0f : 1.0f;
}

int Round(float v) { return static_cast<int>(std::lround(v)); }

}  // namespace

void StartMenuPlacer::Shutdown() {
    Restore();
    if (uia_) {
        uia_->Release();
        uia_ = nullptr;
    }
}

HWND StartMenuPlacer::Host() {
    if (host_ && IsHostWindow(host_)) return host_;
    host_ = nullptr;
    // Several shell CoreWindows share the class; the title "Start" alone is not proof enough (a
    // future build, another app's window): the owner process must be StartMenuExperienceHost.
    HWND h = nullptr;
    while ((h = FindWindowExW(nullptr, h, kHostClass, kHostTitle)) != nullptr) {
        if (IsHostProcess(h)) {
            host_ = h;
            break;
        }
    }
    return host_;
}

bool StartMenuPlacer::IsOpen() {
    HWND h = Host();
    return h && !Cloaked(h);
}

bool StartMenuPlacer::ReadPanel(HWND host, RECT& panel) {
    if (Cloaked(host)) return false;  // the cloaked tree reports no rectangles (measured)
    if (!uia_ && !uiaFailed_) {
        if (FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation),
                                    reinterpret_cast<void**>(&uia_)))) {
            uia_ = nullptr;
            uiaFailed_ = true;
        }
    }
    if (!uia_) return false;
    RECT hostRc{};
    if (!GetWindowRect(host, &hostRc)) return false;
    IUIAutomationElement* root = nullptr;
    if (FAILED(uia_->ElementFromHandle(host, &root)) || !root) return false;
    IUIAutomationTreeWalker* walker = nullptr;
    bool any = false;
    RECT u{};
    if (SUCCEEDED(uia_->get_RawViewWalker(&walker)) && walker) {
        IUIAutomationElement* child = nullptr;
        walker->GetFirstChildElement(root, &child);
        int guard = 0;
        while (child && guard++ < 64) {
            RECT r{};
            // An empty rect (the cloaked tree, "CloseStartAccessibleButton") or one outside the host
            // is not part of the panel.
            if (SUCCEEDED(child->get_CurrentBoundingRectangle(&r)) && r.right > r.left && r.bottom > r.top &&
                r.left >= hostRc.left && r.top >= hostRc.top && r.right <= hostRc.right && r.bottom <= hostRc.bottom) {
                if (!any) {
                    u = r;
                    any = true;
                } else {
                    UnionRect(&u, &u, &r);
                }
            }
            IUIAutomationElement* next = nullptr;
            walker->GetNextSiblingElement(child, &next);
            child->Release();
            child = next;
        }
        if (child) child->Release();
        walker->Release();
    }
    root->Release();
    if (!any) return false;
    // Sanity: a panel is smaller than the host and not a sliver.
    const LONG w = u.right - u.left, h = u.bottom - u.top;
    if (w < 100 || h < 100 || w >= hostRc.right - hostRc.left || h >= hostRc.bottom - hostRc.top) return false;
    const int frame = Round(kFrameDip * ScaleOf(host));
    InflateRect(&u, frame, frame);
    IntersectRect(&u, &u, &hostRc);
    panel = u;
    return true;
}

RECT StartMenuPlacer::PanelRect() {
    HWND h = Host();
    if (!h) return RECT{};
    RECT hostRc{};
    GetWindowRect(h, &hostRc);
    // The host's natural top-left: where it is, less the shift in force.
    const bool shifted = shifted_ && shiftedHost_ == h;
    const POINT origin = shifted ? natural_ : POINT{hostRc.left, hostRc.top};
    RECT live{};
    if (ReadPanel(h, live)) {
        panelInHost_ = RECT{live.left - hostRc.left, live.top - hostRc.top, live.right - hostRc.left, live.bottom - hostRc.top};
        panelKnown_ = true;
    }
    if (panelKnown_) {
        return RECT{origin.x + panelInHost_.left, origin.y + panelInHost_.top, origin.x + panelInHost_.right,
                    origin.y + panelInHost_.bottom};
    }
    // The anchor model: the host's bottom edge, centred (TaskbarAl's centred default; a left-aligned
    // taskbar's menu is at the left and gets the same guess: not measured).
    const float s = ScaleOf(h);
    const int w = Round(kGuessWDip * s), ht = Round(kGuessHDip * s);
    const int hostW = hostRc.right - hostRc.left, hostH = hostRc.bottom - hostRc.top;
    const int left = origin.x + (hostW - w) / 2;
    const int bottom = origin.y + hostH - Round(kBottomDip * s);
    return RECT{left, bottom - ht, left + w, bottom};
}

bool StartMenuPlacer::Place(const RECT& box, int side, HMONITOR monitor, bool dryRun) {
    HWND h = Host();
    if (!h) return false;
    // The key will close an open menu: no shift then (and the open menu stays where it is).
    if (!dryRun && IsOpen()) return false;
    // A shift of another host window (Explorer restarted Start) is void: that window is at its own place.
    if (shifted_ && shiftedHost_ != h) {
        shifted_ = false;
        g_shiftedHost = nullptr;
    }
    const RECT panel = PanelRect();
    const int w = panel.right - panel.left, ht = panel.bottom - panel.top;
    if (w <= 0 || ht <= 0) return false;
    if (!monitor) monitor = MonitorFromRect(&box, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(monitor, &mi)) return false;
    // A button on another display of another DPI: moving the full-screen host there would make it
    // cross a DPI boundary (Windows may rescale it; not measured): Windows' own place instead.
    if (monitor != MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST)) {
        UINT mx = 0, my = 0;
        if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &mx, &my)) || mx != GetDpiForWindow(h)) {
            lastDx_ = lastDy_ = 0;
            return false;
        }
    }
    // Beside the box like a dock popup: centred on the box, kGapPx from it.
    const int cx = (box.left + box.right) / 2, cy = (box.top + box.bottom) / 2;
    int left = cx - w / 2, top = 0;
    switch (side) {
        case 1: top = box.bottom + kGapPx; break;                                  // below (a top dock)
        case 2: left = box.right + kGapPx; top = cy - ht / 2; break;               // right (a left dock)
        case 3: left = box.left - kGapPx - w; top = cy - ht / 2; break;            // left (a right dock)
        case 0:
        default: top = box.top - kGapPx - ht; break;                               // above (a bottom dock)
    }
    // Into the monitor with a margin; a panel larger than the monitor: its top / left edge.
    const int ml = static_cast<int>(mi.rcMonitor.left), mt = static_cast<int>(mi.rcMonitor.top);
    const int mr = static_cast<int>(mi.rcMonitor.right), mb = static_cast<int>(mi.rcMonitor.bottom);
    left = (w > mr - ml - 2 * kGapPx) ? ml + kGapPx : std::clamp(left, ml + kGapPx, mr - kGapPx - w);
    top = (ht > mb - mt - 2 * kGapPx) ? mt + kGapPx : std::clamp(top, mt + kGapPx, mb - kGapPx - ht);
    const int dx = left - panel.left, dy = top - panel.top;
    lastDx_ = dx;
    lastDy_ = dy;
    if (dryRun) return false;  // compute only: nothing moves under a test
    RECT hostRc{};
    GetWindowRect(h, &hostRc);
    const POINT natural = (shifted_ && shiftedHost_ == h) ? natural_ : POINT{hostRc.left, hostRc.top};
    if (dx == 0 && dy == 0) {
        Restore();  // at its natural place already (or put back there)
        return false;
    }
    // Before the move, so that a crash between the move and here still restores.
    natural_ = natural;
    g_naturalX = natural.x;
    g_naturalY = natural.y;
    g_shiftedHost = h;
    if (!SetWindowPos(h, nullptr, natural.x + dx, natural.y + dy, 0, 0,
                      SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER)) {
        Restore();
        return false;
    }
    shifted_ = true;
    shiftedHost_ = h;
    dx_ = dx;
    dy_ = dy;
    StartWatch();
    return true;
}

void StartMenuPlacer::Restore() {
    StopWatch();
    if (shifted_) {
        // Only the window that was moved: a new host (Explorer / Start restarted) is at its own place.
        if (shiftedHost_ && IsWindow(shiftedHost_)) {
            SetWindowPos(shiftedHost_, nullptr, natural_.x, natural_.y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
    }
    shifted_ = false;
    shiftedHost_ = nullptr;
    dx_ = dy_ = 0;
    g_shiftedHost = nullptr;
}

void StartMenuPlacer::StartWatch() {
    watching_ = true;
    seenOpen_ = false;
    ticks_ = 0;
    openTicks_ = 0;
    readThisOpen_ = false;
    if (owner_) SetTimer(owner_, kWatchTimerId, kWatchMs, nullptr);
}

void StartMenuPlacer::StopWatch() {
    if (watching_ && owner_) KillTimer(owner_, kWatchTimerId);
    watching_ = false;
}

bool StartMenuPlacer::OnTimer(UINT_PTR id) {
    if (id != kWatchTimerId) return false;
    if (owner_) KillTimer(owner_, kWatchTimerId);  // one-shot, re-armed below
    if (!watching_ || !shifted_) {
        watching_ = false;
        return true;
    }
    ++ticks_;
    // The host gone (Start restarted) or cloaked again: the menu closed, however it closed (Escape,
    // a click elsewhere, the key again). Before it was seen open the key may not have opened it
    // yet: wait, up to kOpenCapTicks (the key never opened it: put the host back).
    const bool alive = shiftedHost_ && IsWindow(shiftedHost_);
    const bool open = alive && !Cloaked(shiftedHost_);
    if (!alive || (seenOpen_ && !open) || (!seenOpen_ && ticks_ >= kOpenCapTicks)) {
        Restore();
        return true;
    }
    if (open) {
        seenOpen_ = true;
        // Once per open, after the opening animation: the panel's real rect for the next Place.
        if (!readThisOpen_ && ++openTicks_ >= kSettleTicks) {
            readThisOpen_ = true;
            RECT r{};
            RECT hostRc{};
            if (GetWindowRect(shiftedHost_, &hostRc) && ReadPanel(shiftedHost_, r)) {
                panelInHost_ = RECT{r.left - hostRc.left, r.top - hostRc.top, r.right - hostRc.left, r.bottom - hostRc.top};
                panelKnown_ = true;
            }
        }
    }
    if (owner_) SetTimer(owner_, kWatchTimerId, kWatchMs, nullptr);
    return true;
}

int StartMenuPlacer::State() {
    if (shifted_ && shiftedHost_ && IsWindow(shiftedHost_)) return 2;
    return Host() ? 1 : 0;
}

void StartMenuPlacer::EmergencyRestore() {
    HWND h = g_shiftedHost;
    if (!h) return;
    g_shiftedHost = nullptr;
    SetWindowPos(h, nullptr, g_naturalX, g_naturalY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
}
