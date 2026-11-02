#include "TaskbarController.h"

#include <shellapi.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>

#include "Config.h"

// TODO(win11-fallback): some Windows 11 builds are reported to ignore ShowWindow(SW_HIDE) on
// Shell_TrayWnd (Explorer re-shows or re-composites the taskbar within seconds). On the build
// this was developed on (26300) SW_HIDE plus the EVENT_OBJECT_SHOW hook holds, so the fallback is
// not implemented. If it is ever needed: in ApplyHide(), after SW_HIDE, record each taskbar's
// window rect in SavedState (and the marker) and SetWindowPos it below its monitor's bottom edge
// (SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSIZE); hook EVENT_OBJECT_LOCATIONCHANGE for Explorer's
// PID to push it back; in Restore(), SetWindowPos it back to the saved rect before SW_SHOWNA.

namespace {

constexpr wchar_t kTrayClass[] = L"Shell_TrayWnd";
constexpr wchar_t kSecondaryTrayClass[] = L"Shell_SecondaryTrayWnd";
constexpr char kMarkerHeader[] = "WinDock taskbar-hidden 1";
constexpr int kMaxWorkAreaFixes = 6;
// One-shot re-checks after a hide: Explorer applies the auto-hide state change and its work
// area update asynchronously. This is a short, bounded sequence, not polling.
constexpr UINT kVerifyDelaysMs[] = {250, 1000, 3000};

void Trace(const wchar_t* fmt, ...) {
    wchar_t buf[256];
    va_list args;
    va_start(args, fmt);
    vswprintf_s(buf, fmt, args);
    va_end(args);
    OutputDebugStringW(buf);
}

bool SameRect(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

bool Contains(const RECT& outer, const RECT& inner) {
    return inner.left >= outer.left && inner.top >= outer.top && inner.right <= outer.right &&
           inner.bottom <= outer.bottom && inner.right > inner.left && inner.bottom > inner.top;
}

struct MonitorList {
    int count = 0;
    HMONITOR handle[16]{};
};

BOOL CALLBACK CollectMonitor(HMONITOR mon, HDC, LPRECT, LPARAM lp) {
    auto* list = reinterpret_cast<MonitorList*>(lp);
    if (list->count < static_cast<int>(std::size(list->handle))) list->handle[list->count++] = mon;
    return TRUE;
}

MonitorList EnumMonitors() {
    MonitorList list;
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&list));
    return list;
}

// Explorer persists the auto-hide setting in StuckRects3 "Settings" (byte 8, bit 0) lazily: on
// build 26300 the auto-hide we set while hiding got written, but ABM_SETSTATE back to normal was
// not, so the next Explorer start / logon would come back auto-hidden. Make the persisted bit
// match the state we restore. Undocumented layout, so only touch a blob that looks right.
// No heap allocation (crash-path safe).
void SyncPersistedAutoHide(bool autoHide) {
    constexpr wchar_t kKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StuckRects3";
    BYTE data[256];
    DWORD size = sizeof(data);
    DWORD type = 0;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (RegQueryValueExW(key, L"Settings", nullptr, &type, data, &size) == ERROR_SUCCESS && type == REG_BINARY &&
        size >= 12) {
        const BYTE wanted = autoHide ? static_cast<BYTE>(data[8] | 0x01) : static_cast<BYTE>(data[8] & ~0x01);
        if (wanted != data[8]) {
            data[8] = wanted;
            RegSetValueExW(key, L"Settings", 0, REG_BINARY, data, size);
        }
    }
    RegCloseKey(key);
}

}  // namespace

std::atomic<TaskbarController*> TaskbarController::instance_{nullptr};

TaskbarController::TaskbarController() {
    const std::wstring path = Config::Directory() + L"\\taskbar-hidden";
    wcsncpy_s(markerPath_, path.c_str(), _TRUNCATE);
    TaskbarController* expected = nullptr;
    instance_.compare_exchange_strong(expected, this);
}

TaskbarController::~TaskbarController() {
    if (active_) Restore();
    RemoveHook();
    CancelVerify();
    TaskbarController* expected = this;
    instance_.compare_exchange_strong(expected, nullptr);
}

// ---------------------------------------------------------------------------------------------
// Discovery and system state

bool TaskbarController::IsTaskbarClass(HWND hwnd) {
    wchar_t cls[32];
    const int n = GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls)));
    if (n <= 0) return false;
    return wcscmp(cls, kTrayClass) == 0 || wcscmp(cls, kSecondaryTrayClass) == 0;
}

TaskbarController::TaskbarList TaskbarController::FindTaskbars() {
    TaskbarList list;
    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            auto* l = reinterpret_cast<TaskbarList*>(lp);
            if (l->count < kMaxTaskbars && IsTaskbarClass(hwnd)) l->hwnd[l->count++] = hwnd;
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&list));
    return list;
}

HWND TaskbarController::PrimaryTaskbar() { return FindWindowW(kTrayClass, nullptr); }

void TaskbarController::CaptureState(SavedState& s) {
    s = SavedState{};
    APPBARDATA abd{sizeof(abd)};
    abd.hWnd = PrimaryTaskbar();
    s.abmState = static_cast<UINT>(SHAppBarMessage(ABM_GETSTATE, &abd));

    const MonitorList mons = EnumMonitors();
    for (int i = 0; i < mons.count && s.monitorCount < kMaxMonitors; ++i) {
        MONITORINFO mi{sizeof(mi)};
        if (!GetMonitorInfoW(mons.handle[i], &mi)) continue;
        s.monitor[s.monitorCount] = mi.rcMonitor;
        s.work[s.monitorCount] = mi.rcWork;
        ++s.monitorCount;
    }
    s.valid = true;
}

void TaskbarController::SetAppBarState(UINT state) {
    APPBARDATA abd{sizeof(abd)};
    abd.hWnd = PrimaryTaskbar();
    abd.lParam = static_cast<LPARAM>(state);
    SHAppBarMessage(ABM_SETSTATE, &abd);
}

// ---------------------------------------------------------------------------------------------
// Public API

void TaskbarController::SetEnabled(bool enabled) {
    enabled_ = enabled;
    if (enabled) {
        Hide();
    } else {
        Restore();
    }
}

void TaskbarController::Hide() {
    if (!enabled_) return;

    if (!active_) {
        if (!PrimaryTaskbar()) {
            // Explorer is not running (yet): TaskbarCreated will bring us back here.
            pendingHide_ = true;
            Trace(L"WinDock: no Shell_TrayWnd yet; hide deferred until TaskbarCreated\n");
            return;
        }
        // Original state, in order of trust: recovered at startup, a stale crash marker, live.
        if (carried_.valid) {
            saved_ = carried_;
            carried_.valid = false;
        } else if (!ReadMarker(saved_)) {
            CaptureState(saved_);
        }
        active_ = true;
        pendingHide_ = false;
        if (!WriteMarker()) Trace(L"WinDock: could not write the taskbar crash marker\n");
        Trace(L"WinDock: taskbar hide (saved ABM state 0x%X, %d monitor(s))\n", saved_.abmState,
              saved_.monitorCount);
    }
    ApplyHide();
}

void TaskbarController::Restore() {
    // Re-entrancy guard: the exception filter may call in while a Restore is already running.
    if (restoring_.exchange(true)) return;

    // No heap allocation below this point (crash-path safe).
    CancelVerify();
    RemoveHook();
    pendingHide_ = false;

    if (active_ || saved_.valid) {
        active_ = false;

        const TaskbarList bars = FindTaskbars();
        for (int i = 0; i < bars.count; ++i) {
            // SW_SHOWNA rather than SW_SHOW: do not activate the taskbar / steal focus.
            ShowWindow(bars.hwnd[i], SW_SHOWNA);
        }

        if (saved_.valid) {
            SetAppBarState(saved_.abmState);
            SyncPersistedAutoHide((saved_.abmState & ABS_AUTOHIDE) != 0);

            // Put back each saved work area whose monitor still exists with the same bounds.
            const MonitorList mons = EnumMonitors();
            for (int m = 0; m < mons.count; ++m) {
                MONITORINFO mi{sizeof(mi)};
                if (!GetMonitorInfoW(mons.handle[m], &mi)) continue;
                for (int s = 0; s < saved_.monitorCount; ++s) {
                    if (!SameRect(saved_.monitor[s], mi.rcMonitor)) continue;
                    RECT work = saved_.work[s];
                    if (Contains(mi.rcMonitor, work) && !SameRect(work, mi.rcWork)) {
                        SystemParametersInfoW(SPI_SETWORKAREA, 0, &work, SPIF_SENDCHANGE);
                    }
                    break;
                }
            }
        }
        saved_.valid = false;
        DeleteMarker();
        OutputDebugStringW(L"WinDock: taskbar restored\n");
    }

    restoring_.store(false);
}

void TaskbarController::OnTaskbarCreated() {
    if (!enabled_ || !(active_ || pendingHide_)) return;
    Trace(L"WinDock: TaskbarCreated, re-hiding\n");
    Hide();  // re-finds the windows and re-hooks the new Explorer process
}

void TaskbarController::OnWorkAreaChanged() {
    if (active_) VerifyWorkArea();
}

void TaskbarController::SetReservedMonitors(const RECT* monitors, int count) {
    reservedCount_ = 0;
    for (int i = 0; i < count && reservedCount_ < kMaxMonitors; ++i) reservedMonitors_[reservedCount_++] = monitors[i];
}

bool TaskbarController::RecoverFromMarker() {
    SavedState s;
    if (!ReadMarker(s)) return false;
    Trace(L"WinDock: crash marker found (ABM state 0x%X); restoring the taskbar first\n", s.abmState);
    saved_ = s;
    active_ = true;
    Restore();
    carried_ = s;  // the next Hide() saves this, not the half-restored live state
    return true;
}

void TaskbarController::RestoreStandalone() {
    TaskbarController c;
    if (c.ReadMarker(c.saved_)) {
        c.active_ = true;
        c.Restore();
        return;
    }
    // No marker: we cannot know the original state. If a taskbar is hidden, show it and turn
    // auto-hide off (the default Windows behaviour); otherwise leave everything alone.
    const TaskbarList bars = FindTaskbars();
    bool anyHidden = false;
    for (int i = 0; i < bars.count; ++i) {
        if (!IsWindowVisible(bars.hwnd[i])) {
            anyHidden = true;
            ShowWindow(bars.hwnd[i], SW_SHOWNA);
        }
    }
    if (anyHidden) {
        APPBARDATA abd{sizeof(abd)};
        abd.hWnd = PrimaryTaskbar();
        const UINT state = static_cast<UINT>(SHAppBarMessage(ABM_GETSTATE, &abd));
        if (state & ABS_AUTOHIDE) SetAppBarState(state & ~static_cast<UINT>(ABS_AUTOHIDE));
        SyncPersistedAutoHide(false);
    }
}

void TaskbarController::EmergencyRestore() {
    if (TaskbarController* self = instance_.load()) self->Restore();
}

// ---------------------------------------------------------------------------------------------
// Hide internals

void TaskbarController::ApplyHide() {
    if (!active_) return;
    const TaskbarList bars = FindTaskbars();
    if (bars.count == 0) return;  // Explorer went away; TaskbarCreated will call Hide() again

    // Auto-hide makes Explorer itself give the work area back; only set it if needed, since
    // every ABM_SETSTATE makes Explorer re-run its layout.
    APPBARDATA abd{sizeof(abd)};
    abd.hWnd = PrimaryTaskbar();
    const UINT state = static_cast<UINT>(SHAppBarMessage(ABM_GETSTATE, &abd));
    if (!(state & ABS_AUTOHIDE)) SetAppBarState(state | ABS_AUTOHIDE);

    for (int i = 0; i < bars.count; ++i) {
        if (IsWindowVisible(bars.hwnd[i])) ShowWindow(bars.hwnd[i], SW_HIDE);
    }

    InstallHook();
    workAreaFixes_ = 0;
    VerifyWorkArea();
    verifyStep_ = 0;
    ScheduleVerify(kVerifyDelaysMs[0]);
}

void TaskbarController::VerifyWorkArea() {
    if (!active_) return;

    // Monitors to check: the primary, plus every monitor that hosts a taskbar window.
    HMONITOR targets[kMaxTaskbars + 1]{};
    int n = 0;
    targets[n++] = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    const TaskbarList bars = FindTaskbars();
    for (int i = 0; i < bars.count; ++i) {
        HMONITOR m = MonitorFromWindow(bars.hwnd[i], MONITOR_DEFAULTTONULL);
        bool seen = !m;
        for (int j = 0; j < n && !seen; ++j) seen = targets[j] == m;
        if (!seen) targets[n++] = m;
    }

    for (int i = 0; i < n; ++i) {
        MONITORINFO mi{sizeof(mi)};
        if (!GetMonitorInfoW(targets[i], &mi)) continue;
        // The docks' own app bars (reserveSpace) leave a smaller work area on their monitors; the
        // shell keeps those up to date itself.
        bool reserved = false;
        for (int r = 0; r < reservedCount_ && !reserved; ++r) reserved = SameRect(mi.rcMonitor, reservedMonitors_[r]);
        if (reserved) continue;
        if (SameRect(mi.rcWork, mi.rcMonitor)) continue;
        if (workAreaFixes_ >= kMaxWorkAreaFixes) {
            Trace(L"WinDock: work area still reserved after %d fixes; giving up\n", workAreaFixes_);
            return;
        }
        ++workAreaFixes_;
        Trace(L"WinDock: work area (%ld,%ld,%ld,%ld) != monitor; forcing full monitor\n", mi.rcWork.left,
              mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom);
        RECT full = mi.rcMonitor;
        // Broadcasts WM_SETTINGCHANGE, which re-enters OnWorkAreaChanged(); by then it matches.
        SystemParametersInfoW(SPI_SETWORKAREA, 0, &full, SPIF_SENDCHANGE);
    }
}

void TaskbarController::InstallHook() {
    DWORD pid = 0;
    if (HWND tray = PrimaryTaskbar()) GetWindowThreadProcessId(tray, &pid);
    if (!pid) return;
    if (hook_ && hookPid_ == pid) return;
    RemoveHook();
    hook_ = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, &TaskbarController::WinEventProc, pid,
                            0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    hookPid_ = hook_ ? pid : 0;
    if (!hook_) Trace(L"WinDock: SetWinEventHook failed (%lu)\n", GetLastError());
}

void TaskbarController::RemoveHook() {
    if (hook_) UnhookWinEvent(hook_);
    hook_ = nullptr;
    hookPid_ = 0;
}

void CALLBACK TaskbarController::WinEventProc(HWINEVENTHOOK, DWORD, HWND hwnd, LONG idObject, LONG idChild, DWORD,
                                              DWORD) {
    // Fires for every window Explorer shows (menus, tooltips, flyouts): keep it trivial.
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd) return;
    TaskbarController* self = instance_.load(std::memory_order_relaxed);
    if (!self || !self->active_ || !IsTaskbarClass(hwnd)) return;
    ShowWindowAsync(hwnd, SW_HIDE);  // async: never block on Explorer from inside the callback
    // A newly shown secondary taskbar (monitor attached) may have reserved its work area.
    if (!self->verifyTimer_) {
        self->verifyStep_ = static_cast<int>(std::size(kVerifyDelaysMs)) - 1;
        self->ScheduleVerify(kVerifyDelaysMs[0]);
    }
}

void TaskbarController::ScheduleVerify(UINT delayMs) {
    CancelVerify();
    verifyTimer_ = SetTimer(nullptr, 0, delayMs, &TaskbarController::VerifyTimerProc);
}

void TaskbarController::CancelVerify() {
    if (verifyTimer_) KillTimer(nullptr, verifyTimer_);
    verifyTimer_ = 0;
}

void CALLBACK TaskbarController::VerifyTimerProc(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    TaskbarController* self = instance_.load();
    if (!self || self->verifyTimer_ != id) return;
    self->verifyTimer_ = 0;
    if (!self->active_) return;

    const TaskbarList bars = FindTaskbars();
    for (int i = 0; i < bars.count; ++i) {
        if (IsWindowVisible(bars.hwnd[i])) {
            Trace(L"WinDock: taskbar %p visible again at verify step %d; re-hiding\n", bars.hwnd[i],
                  self->verifyStep_);
            ShowWindow(bars.hwnd[i], SW_HIDE);
        }
    }
    self->VerifyWorkArea();

    if (++self->verifyStep_ < static_cast<int>(std::size(kVerifyDelaysMs))) {
        self->ScheduleVerify(kVerifyDelaysMs[self->verifyStep_]);
    }
}

// ---------------------------------------------------------------------------------------------
// Crash marker: a tiny text file holding SavedState.
//
//   WinDock taskbar-hidden 1
//   abm <state>
//   mon <l> <t> <r> <b> <wl> <wt> <wr> <wb>      (one line per monitor)

bool TaskbarController::WriteMarker() const {
    if (!saved_.valid || !markerPath_[0]) return false;

    wchar_t dir[MAX_PATH];
    wcscpy_s(dir, markerPath_);
    if (wchar_t* slash = wcsrchr(dir, L'\\')) {
        *slash = L'\0';
        CreateDirectoryW(dir, nullptr);
    }

    char buf[2048];
    int len = sprintf_s(buf, "%s\r\nabm %u\r\n", kMarkerHeader, saved_.abmState);
    for (int i = 0; i < saved_.monitorCount && len > 0; ++i) {
        const RECT& m = saved_.monitor[i];
        const RECT& w = saved_.work[i];
        const int n = sprintf_s(buf + len, sizeof(buf) - static_cast<size_t>(len),
                                "mon %ld %ld %ld %ld %ld %ld %ld %ld\r\n", m.left, m.top, m.right, m.bottom,
                                w.left, w.top, w.right, w.bottom);
        if (n <= 0) break;
        len += n;
    }
    if (len <= 0) return false;

    HANDLE h = CreateFileW(markerPath_, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(h, buf, static_cast<DWORD>(len), &written, nullptr) && written == static_cast<DWORD>(len);
    FlushFileBuffers(h);
    CloseHandle(h);
    return ok;
}

bool TaskbarController::ReadMarker(SavedState& out) const {
    out = SavedState{};
    if (!markerPath_[0]) return false;
    HANDLE h = CreateFileW(markerPath_, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[2048];
    DWORD read = 0;
    const BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &read, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    buf[read] = '\0';

    if (strncmp(buf, kMarkerHeader, sizeof(kMarkerHeader) - 1) != 0) return false;

    SavedState s;
    bool haveAbm = false;
    char* context = nullptr;
    for (char* line = strtok_s(buf, "\r\n", &context); line; line = strtok_s(nullptr, "\r\n", &context)) {
        unsigned abm = 0;
        RECT m{}, w{};
        if (sscanf_s(line, "abm %u", &abm) == 1) {
            s.abmState = abm;
            haveAbm = true;
        } else if (sscanf_s(line, "mon %ld %ld %ld %ld %ld %ld %ld %ld", &m.left, &m.top, &m.right, &m.bottom,
                            &w.left, &w.top, &w.right, &w.bottom) == 8 &&
                   s.monitorCount < kMaxMonitors) {
            s.monitor[s.monitorCount] = m;
            s.work[s.monitorCount] = w;
            ++s.monitorCount;
        }
    }
    if (!haveAbm) return false;
    s.valid = true;
    out = s;
    return true;
}

void TaskbarController::DeleteMarker() const {
    if (markerPath_[0]) DeleteFileW(markerPath_);
}
