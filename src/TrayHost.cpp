#include "TrayHost.h"

#include <cguid.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlwapi.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <iterator>

#include "Config.h"

namespace {

constexpr wchar_t kTrayClass[] = L"Shell_TrayWnd";
constexpr wchar_t kNotifyClass[] = L"TrayNotifyWnd";
constexpr DWORD kSignature = 0x34753423;
constexpr UINT kMirrorTimeoutMs = 2000;
constexpr size_t kMaxBalloons = 32;
constexpr size_t kMaxIcons = 512;  // a runaway app cannot grow the registry without bound

// The 32-bit-compatible layouts shell32 sends (spike: build 26300), packed.
#pragma pack(push, 1)
struct NotifyIconData32 {
    DWORD cbSize;
    DWORD hWnd;
    UINT uID;
    UINT uFlags;
    UINT uCallbackMessage;
    DWORD hIcon;
    WCHAR szTip[128];
    DWORD dwState;
    DWORD dwStateMask;
    WCHAR szInfo[256];
    UINT uTimeoutOrVersion;
    WCHAR szInfoTitle[64];
    DWORD dwInfoFlags;
    GUID guidItem;
    DWORD hBalloonIcon;
};
struct TrayNotifyData {
    DWORD dwSignature;
    DWORD dwMessage;  // NIM_*
    NotifyIconData32 nid;
};
struct IconIdentifier32 {
    DWORD dwMagic;
    DWORD dwMessage;  // 1: MAKELONG(left, top); 2: MAKELONG(width, height)
    DWORD cbSize;
    DWORD dwPadding;
    DWORD hWnd;
    UINT uID;
    GUID guidItem;
};
// SHAppBarMessage's payload (64-bit callers; spike + 8a trace, build 26300): the 40-byte
// APPBARDATA with 32-bit handles, the ABM_* message, the shared block and the process it lives in.
struct AppBarData32 {
    DWORD cbSize;  // 40
    DWORD hWnd;
    UINT uCallbackMessage;
    UINT uEdge;
    RECT rc;
    LONGLONG lParam;
};
struct AppBarMessage64 {
    AppBarData32 abd;
    DWORD dwMessage;
    DWORD padding1;
    ULONGLONG hSharedMemory;  // a handle in dwSourceProcessId's process (or, in-process, a pointer)
    DWORD dwSourceProcessId;  // the process the block was made for (the Shell_TrayWnd's)
    DWORD padding2;
};
#pragma pack(pop)
static_assert(sizeof(AppBarData32) == 40);
static_assert(sizeof(AppBarMessage64) == 64);
static_assert(sizeof(NotifyIconData32) == 956);
static_assert(sizeof(IconIdentifier32) == 40);

template <class T>
HANDLE_PTR SignExtend(T v) {
    return static_cast<HANDLE_PTR>(static_cast<LONG_PTR>(static_cast<LONG>(v)));
}
HWND ToHwnd(DWORD v) { return reinterpret_cast<HWND>(SignExtend(v)); }
HICON ToHicon(DWORD v) { return reinterpret_cast<HICON>(SignExtend(v)); }

std::wstring Bounded(const WCHAR* text, size_t cap) {
    size_t n = 0;
    while (n < cap && text[n]) ++n;
    return std::wstring(text, n);
}

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

std::wstring FileName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// FNV-1a over the icon's colour and mask pixels (0: could not be read).
uint64_t IconHash(HICON icon) {
    ICONINFO ii{};
    if (!icon || !GetIconInfo(icon, &ii)) return 0;
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](HBITMAP bmp) {
        if (!bmp) return;
        BITMAP bm{};
        if (!GetObjectW(bmp, sizeof(bm), &bm) || bm.bmWidthBytes <= 0 || bm.bmHeight <= 0) return;
        const size_t bytes = static_cast<size_t>(bm.bmWidthBytes) * static_cast<size_t>(bm.bmHeight);
        if (bytes > 4u * 1024u * 1024u) return;
        std::vector<BYTE> buf(bytes);
        const LONG got = GetBitmapBits(bmp, static_cast<LONG>(bytes), buf.data());
        for (LONG i = 0; i < got; ++i) h = (h ^ buf[static_cast<size_t>(i)]) * 1099511628211ull;
        h ^= static_cast<uint64_t>(bm.bmWidth) << 32 | static_cast<uint64_t>(bm.bmHeight);
    };
    mix(ii.hbmColor);
    mix(ii.hbmMask);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return h ? h : 1;
}

std::wstring MarkerPath() { return Config::Directory() + L"\\tray-hosted"; }

}  // namespace

std::atomic<HWND> TrayHost::live_{nullptr};
TrayHost* TrayHost::self_ = nullptr;

std::wstring TrayIcon::Title() const {
    std::wstring t = tip.substr(0, tip.find_first_of(L"\r\n"));
    while (!t.empty() && iswspace(t.back())) t.pop_back();
    if (!t.empty()) return t;
    std::wstring name = FileName(exe);
    return name.empty() ? key : name;
}

// ---------------------------------------------------------------------------------------------
// Start / stop

bool TrayHost::Start(HINSTANCE instance) {
    if (hwnd_) return true;
    instance_ = instance;
    msgTaskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &TrayHost::WndProcThunk;
    wc.hInstance = instance;
    wc.lpszClassName = kTrayClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    WNDCLASSEXW nc{sizeof(nc)};
    nc.lpfnWndProc = DefWindowProcW;
    nc.hInstance = instance;
    nc.lpszClassName = kNotifyClass;
    if (!RegisterClassExW(&nc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    // Where Explorer's taskbar is (apps that read the "taskbar" rect get a sensible answer). Never shown.
    RECT r{};
    if (HWND ex = ExplorerTray()) GetWindowRect(ex, &r);
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kTrayClass, nullptr, WS_POPUP, r.left, r.top,
                            r.right - r.left, r.bottom - r.top, nullptr, nullptr, instance, this);
    if (!hwnd_) return false;
    CreateWindowExW(0, kNotifyClass, nullptr, WS_CHILD, 0, 0, 0, 0, hwnd_, nullptr, instance, nullptr);
    // Elevated apps may register too (a higher integrity level may always send down; allow the copy).
    ChangeWindowMessageFilterEx(hwnd_, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    self_ = this;
    live_.store(hwnd_);
    Raise();
    HookExplorer();
    WriteMarker(true);
    Broadcast(true);
    OutputDebugStringW(L"WinDock: tray host started\n");
    return true;
}

void TrayHost::Stop() {
    if (!hwnd_) return;
    if (rebroadcastPending_) KillTimer(hwnd_, kRebroadcastTimer);
    rebroadcastPending_ = false;
    FlushMirrors();  // Explorer gets everything before the apps are sent back to it
    Unhook();
    for (auto& [pid, w] : waits_) {
        if (w.wait) UnregisterWaitEx(w.wait, INVALID_HANDLE_VALUE);  // waits for a running callback
        if (w.process) CloseHandle(w.process);
    }
    waits_.clear();
    HWND hwnd = hwnd_;
    hwnd_ = nullptr;
    live_.store(nullptr);
    DestroyWindow(hwnd);
    if (self_ == this) self_ = nullptr;
    for (TrayIcon& icon : icons_) {
        if (icon.icon) DestroyIcon(icon.icon);
    }
    icons_.clear();
    changedPosted_ = false;
    // Every app asks FindWindow again: Explorer's tray (which has every icon: the mirror).
    Broadcast(false);
    WriteMarker(false);
    if (changed_) changed_();  // the grids empty
    OutputDebugStringW(L"WinDock: tray host stopped, icons handed back to Explorer\n");
}

void TrayHost::OnExplorerRestarted() {
    if (!hwnd_) return;
    Raise();
    HookExplorer();
    Broadcast(true);
}

void TrayHost::Broadcast(bool takeOver) {
    if (!msgTaskbarCreated_) msgTaskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    lastBroadcast_ = GetTickCount64();
    broadcasting_ = true;
    if (!takeOver) {
        // Same-thread windows (ours) get it synchronously: App sees Broadcasting() and ignores it.
        SendNotifyMessageW(HWND_BROADCAST, msgTaskbarCreated_, 0, 0);
    } else {
        // Every top-level window but ours and Explorer's taskbars: Explorer's tray answers
        // TaskbarCreated by raising itself above ours, and the apps answering at the same moment
        // then re-registered with Explorer (8a: a third of the icons missed). Explorer's other
        // windows (they own its notify icons) get it.
        struct Ctx {
            UINT msg;
            DWORD self;
        } ctx{msgTaskbarCreated_, GetCurrentProcessId()};
        EnumWindows(
            [](HWND h, LPARAM lp) -> BOOL {
                const auto* c = reinterpret_cast<const Ctx*>(lp);
                DWORD pid = 0;
                GetWindowThreadProcessId(h, &pid);
                if (pid == c->self) return TRUE;
                wchar_t cls[32];
                if (GetClassNameW(h, cls, static_cast<int>(std::size(cls))) &&
                    (wcscmp(cls, kTrayClass) == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0)) {
                    return TRUE;
                }
                SendNotifyMessageW(h, c->msg, 0, 0);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&ctx));
    }
    broadcasting_ = false;
}

void TrayHost::Raise() {
    if (hwnd_) SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

HWND TrayHost::ExplorerTray() {
    const DWORD self = GetCurrentProcessId();
    for (HWND h = FindWindowExW(nullptr, nullptr, kTrayClass, nullptr); h;
         h = FindWindowExW(nullptr, h, kTrayClass, nullptr)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid != self) return h;
    }
    return nullptr;
}

void TrayHost::HookExplorer() {
    DWORD pid = 0;
    if (HWND ex = ExplorerTray()) GetWindowThreadProcessId(ex, &pid);
    if (reorderHook_ && pid == explorerPid_) return;
    Unhook();
    if (!pid) return;
    // Every top-level z-order change raises EVENT_OBJECT_REORDER on the desktop window in the
    // process that made it: only Explorer's can put its tray above ours.
    reorderHook_ = SetWinEventHook(EVENT_OBJECT_REORDER, EVENT_OBJECT_REORDER, nullptr, &TrayHost::OnReorder, pid, 0,
                                   WINEVENT_OUTOFCONTEXT);
    explorerPid_ = reorderHook_ ? pid : 0;
}

void TrayHost::Unhook() {
    if (reorderHook_) UnhookWinEvent(reorderHook_);
    reorderHook_ = nullptr;
    explorerPid_ = 0;
}

void CALLBACK TrayHost::OnReorder(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
    TrayHost* self = self_;
    if (!self || !self->hwnd_) return;
    if (FindWindowW(kTrayClass, nullptr) == self->hwnd_) return;
    ++self->raises_;
    self->Raise();
    // While Explorer's tray was above ours, apps' messages went to Explorer only (it raises its
    // hidden tray around taskbar state changes, e.g. right after the dock hid it, when the apps
    // were answering our broadcast): ask them again, at most every kRebroadcastMs.
    self->ScheduleRebroadcast();
}

void TrayHost::ScheduleRebroadcast() {
    if (!hwnd_ || rebroadcastPending_) return;
    const ULONGLONG now = GetTickCount64();
    if (now - rebroadcastWindow_ >= 60000) {
        rebroadcastWindow_ = now;
        rebroadcastsInWindow_ = 0;
    }
    if (rebroadcastsInWindow_ >= kRebroadcastsPerMinute) return;  // never a broadcast loop with Explorer
    ++rebroadcastsInWindow_;
    const ULONGLONG since = now - lastBroadcast_;
    const UINT delay = since >= kRebroadcastMs ? 0 : static_cast<UINT>(kRebroadcastMs - since);
    rebroadcastPending_ = SetTimer(hwnd_, kRebroadcastTimer, std::max<UINT>(delay, USER_TIMER_MINIMUM), nullptr) != 0;
}

void TrayHost::WriteMarker(bool on) {
    const std::wstring path = MarkerPath();
    if (!on) {
        DeleteFileW(path.c_str());
        return;
    }
    CreateDirectoryW(Config::Directory().c_str(), nullptr);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    static const char kText[] = "WinDock tray host 1\r\n";
    DWORD written = 0;
    WriteFile(h, kText, sizeof(kText) - 1, &written, nullptr);
    CloseHandle(h);
}

void TrayHost::RecoverFromMarker() {
    const std::wstring path = MarkerPath();
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    OutputDebugStringW(L"WinDock: tray marker found (no clean hand-back last time): broadcasting TaskbarCreated\n");
    SendNotifyMessageW(HWND_BROADCAST, RegisterWindowMessageW(L"TaskbarCreated"), 0, 0);
    DeleteFileW(path.c_str());
}

void TrayHost::EmergencyRelease() {
    HWND hwnd = live_.exchange(nullptr);
    if (!hwnd) return;
    // DestroyWindow works only on the window's own thread; elsewhere the process exit removes it.
    if (GetWindowThreadProcessId(hwnd, nullptr) == GetCurrentThreadId()) DestroyWindow(hwnd);
    // Posted: never wait on other processes from a crashing one (and never run our own handlers).
    static UINT msg = 0;
    if (!msg) msg = RegisterWindowMessageW(L"TaskbarCreated");
    PostMessageW(HWND_BROADCAST, msg, 0, 0);
}

// ---------------------------------------------------------------------------------------------
// Window

LRESULT CALLBACK TrayHost::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    TrayHost* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<TrayHost*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<TrayHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return self->WndProc(hwnd, msg, wp, lp);
}

LRESULT TrayHost::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COPYDATA: {
            const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(lp);
            if (!cds) return FALSE;
            if (cds->dwData == 1) return OnNotifyIcon(reinterpret_cast<HWND>(wp), *cds);
            if (cds->dwData == 3) return OnIconRect(*cds);
            // App bar messages (0) and anything else: Explorer's to answer.
            return cds->dwData == 0 ? OnAppBar(wp, *cds) : Relay(wp, lp);
        }
        case kMirrorMessage:
            FlushMirrors();
            return 0;
        case WM_TIMER:
            if (wp != kRebroadcastTimer) break;
            KillTimer(hwnd, kRebroadcastTimer);  // one-shot
            rebroadcastPending_ = false;
            if (hwnd_) {
                ++rebroadcasts_;
                Raise();
                Broadcast(true);
            }
            return 0;
        case kChangedMessage:
            changedPosted_ = false;
            if (changed_) changed_();
            return 0;
        case kOwnerGoneMessage:
            RemoveOwner(static_cast<DWORD>(wp));
            return 0;
        default:
            break;
    }
    // Private messages for "the taskbar" (WM_COMMAND 419 = minimize all, WM_USER + n ...): Explorer's.
    if (hwnd_ && (msg == WM_COMMAND || (msg >= WM_USER && msg < 0x8000 && msg != kChangedMessage && msg != kOwnerGoneMessage))) {
        if (HWND ex = ExplorerTray()) {
            if (InSendMessage()) {
                DWORD_PTR result = 0;
                SendMessageTimeoutW(ex, msg, wp, lp, SMTO_ABORTIFHUNG, kMirrorTimeoutMs, &result);
                return static_cast<LRESULT>(result);
            }
            PostMessageW(ex, msg, wp, lp);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT TrayHost::Relay(WPARAM wp, LPARAM lp) {
    // Not SMTO_BLOCK: while Explorer answers, apps' notify icon messages to us keep being handled
    // (Explorer is slow right after a crash recovery restored its taskbar; a blocked thread here
    // made apps' Shell_NotifyIcon calls fail).
    DWORD_PTR result = 0;
    HWND ex = ExplorerTray();
    if (!ex || !SendMessageTimeoutW(ex, WM_COPYDATA, wp, lp, SMTO_ABORTIFHUNG, kMirrorTimeoutMs, &result)) return 0;
    return static_cast<LRESULT>(result);
}

LRESULT TrayHost::OnAppBar(WPARAM wp, const COPYDATASTRUCT& cds) {
    // SHAppBarMessage puts what Explorer answers through (QUERYPOS / SETPOS / GETTASKBARPOS) into
    // a block made by SHAllocShared for the process of the Shell_TrayWnd it found, i.e. ours, and
    // names that process in dwSourceProcessId. From another process the block comes as a handle in
    // our process, which Explorer maps itself (relayed as it is). From our own process (the docks'
    // reserve-space app bars) shell32 passes the block's address in this process instead (8a
    // trace, build 26300: the 16-byte SHAllocShared header {size, 0xBAFF1AFF, pid, pid} and the
    // 40-byte APPBARDATA after it), which Explorer cannot map: the data is copied into a block
    // made for Explorer's process, sent, and the answer copied back.
    if (cds.cbData < sizeof(AppBarMessage64) || !cds.lpData) return Relay(wp, reinterpret_cast<LPARAM>(&cds));
    AppBarMessage64 m;
    std::memcpy(&m, cds.lpData, sizeof(m));
    if (!m.hSharedMemory || m.dwSourceProcessId != GetCurrentProcessId()) return Relay(wp, reinterpret_cast<LPARAM>(&cds));
    auto* header = reinterpret_cast<BYTE*>(static_cast<ULONG_PTR>(m.hSharedMemory));
    MEMORY_BASIC_INFORMATION mbi{};
    // A handle value is never a mapped address (the first 64 KB are never mapped).
    if (m.hSharedMemory < 0x10000 || !VirtualQuery(header, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        !(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) ||
        header + 16 > static_cast<BYTE*>(mbi.BaseAddress) + mbi.RegionSize) {
        return Relay(wp, reinterpret_cast<LPARAM>(&cds));
    }
    DWORD head[4];
    std::memcpy(head, header, sizeof(head));
    BYTE* data = header + sizeof(head);
    const DWORD size = head[0];
    if (head[1] != 0xBAFF1AFF || size == 0 || size > 4096 || data + size > static_cast<BYTE*>(mbi.BaseAddress) + mbi.RegionSize) {
        return Relay(wp, reinterpret_cast<LPARAM>(&cds));
    }
    HWND ex = ExplorerTray();
    DWORD explorer = 0;
    if (ex) GetWindowThreadProcessId(ex, &explorer);
    if (!explorer) return 0;
    HANDLE shared = SHAllocShared(data, size, explorer);
    if (!shared) return 0;
    m.hSharedMemory = reinterpret_cast<ULONG_PTR>(shared);
    m.dwSourceProcessId = explorer;
    std::vector<BYTE> payload(static_cast<const BYTE*>(cds.lpData), static_cast<const BYTE*>(cds.lpData) + cds.cbData);
    std::memcpy(payload.data(), &m, sizeof(m));
    COPYDATASTRUCT copy{cds.dwData, cds.cbData, payload.data()};
    const LRESULT result = Relay(wp, reinterpret_cast<LPARAM>(&copy));
    if (void* answer = SHLockShared(shared, explorer)) {
        std::memcpy(data, answer, size);
        SHUnlockShared(answer);
    }
    SHFreeShared(shared, explorer);
    return result;
}

int TrayHost::Find(HWND owner, UINT uid, const GUID* guid) const {
    for (size_t i = 0; i < icons_.size(); ++i) {
        const TrayIcon& t = icons_[i];
        if (guid) {
            if (t.hasGuid && IsEqualGUID(t.guid, *guid)) return static_cast<int>(i);
        } else if (!t.hasGuid && t.owner == owner && t.uid == uid) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::wstring TrayHost::ProcessImage(DWORD pid) {
    std::wstring image;
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[MAX_PATH * 2];
        DWORD n = static_cast<DWORD>(std::size(buf));
        if (QueryFullProcessImageNameW(p, 0, buf, &n)) image.assign(buf, n);
        CloseHandle(p);
    }
    return image;
}

LRESULT TrayHost::OnNotifyIcon(HWND sender, const COPYDATASTRUCT& cds) {
    // Shorter than the header + the fields up to the GUID: not a notify message we understand.
    if (cds.cbData < offsetof(TrayNotifyData, nid) + offsetof(NotifyIconData32, hBalloonIcon) || !cds.lpData) return FALSE;
    TrayNotifyData d;
    std::memcpy(&d, cds.lpData, std::min<size_t>(cds.cbData, sizeof(d)));
    if (cds.cbData < sizeof(d)) d.nid.hBalloonIcon = 0;
    if (d.dwSignature != kSignature) return FALSE;
    const NotifyIconData32& n = d.nid;
    const HWND owner = ToHwnd(n.hWnd);
    const bool hasGuid = (n.uFlags & NIF_GUID) != 0;

    DWORD pid = 0;
    if (owner) GetWindowThreadProcessId(owner, &pid);
    BOOL result = TRUE;
    bool changed = false;
    // Our own process (the dock's tray icon): Explorer keeps it, the grid does not show it.
    const bool own = owner && pid == GetCurrentProcessId();
    int i = own ? -1 : Find(owner, n.uID, hasGuid ? &n.guidItem : nullptr);
    if (!own) {
        switch (d.dwMessage) {
            case NIM_ADD:
            case NIM_MODIFY: {
                if (i < 0) {
                    if (d.dwMessage == NIM_MODIFY || !owner || icons_.size() >= kMaxIcons) {
                        result = FALSE;  // like Explorer: modifying an unknown icon fails
                        break;
                    }
                    TrayIcon t;
                    t.id = nextId_++;
                    t.owner = owner;
                    t.pid = pid;
                    t.uid = n.uID;
                    t.hasGuid = hasGuid;
                    if (hasGuid) t.guid = n.guidItem;
                    auto img = images_.find(pid);
                    if (img == images_.end()) img = images_.emplace(pid, ProcessImage(pid)).first;
                    t.exe = img->second;
                    if (hasGuid) {
                        wchar_t g[64];
                        StringFromGUID2(n.guidItem, g, static_cast<int>(std::size(g)));
                        t.key = Lower(g);
                    } else {
                        const std::wstring exe = FileName(t.exe);
                        t.key = Lower(exe.empty() ? L"pid" + std::to_wstring(pid) : exe) + L":" + std::to_wstring(n.uID);
                    }
                    icons_.push_back(std::move(t));
                    i = static_cast<int>(icons_.size()) - 1;
                    WatchOwner(pid);
                    changed = true;
                }
                // A duplicate NIM_ADD (an app re-registering after TaskbarCreated) updates it.
                TrayIcon& t = icons_[static_cast<size_t>(i)];
                if (owner && owner != t.owner) {
                    t.owner = owner;
                    t.pid = pid;
                    WatchOwner(pid);
                }
                if (n.uFlags & NIF_MESSAGE) {
                    if (t.callback != n.uCallbackMessage) changed = true;
                    t.callback = n.uCallbackMessage;
                }
                if (n.uFlags & NIF_TIP) {
                    std::wstring tip = Bounded(n.szTip, std::size(n.szTip));
                    if (tip != t.tip) {
                        t.tip = std::move(tip);
                        changed = true;
                    }
                }
                if (n.uFlags & NIF_STATE) {
                    const DWORD state = (t.state & ~n.dwStateMask) | (n.dwState & n.dwStateMask);
                    if (state != t.state) changed = true;
                    t.state = state;
                }
                const UINT flags = (t.flags & ~static_cast<UINT>(NIF_SHOWTIP)) | n.uFlags;
                if (flags != t.flags) changed = true;
                t.flags = flags;
                if ((n.uFlags & NIF_ICON) && n.hIcon) {
                    // The app may destroy its handle as soon as this returns: copy now.
                    if (HICON copy = CopyIcon(ToHicon(n.hIcon))) {
                        const uint64_t hash = IconHash(copy);
                        if (t.icon && hash && hash == t.imageHash) {
                            DestroyIcon(copy);  // the same picture again
                        } else {
                            if (t.icon) DestroyIcon(t.icon);
                            t.icon = copy;
                            t.imageHash = hash;
                            t.imageSerial = nextSerial_++;
                            changed = true;
                        }
                    }
                }
                if ((n.uFlags & NIF_INFO) && n.szInfo[0]) {
                    TrayBalloon b;
                    b.key = t.key;
                    b.exe = t.exe;
                    b.title = Bounded(n.szInfoTitle, std::size(n.szInfoTitle));
                    b.text = Bounded(n.szInfo, std::size(n.szInfo));
                    b.infoFlags = n.dwInfoFlags;
                    b.timeoutMs = n.uTimeoutOrVersion;
                    b.at = GetTickCount64();
                    balloons_.push_back(b);
                    while (balloons_.size() > kMaxBalloons) balloons_.pop_front();
                    ++balloonCount_;
                    if (balloonSink_) balloonSink_(b);
                }
                break;
            }
            case NIM_DELETE:
                if (i >= 0) {
                    Remove(static_cast<size_t>(i));
                    changed = true;
                } else {
                    result = FALSE;
                }
                break;
            case NIM_SETVERSION:
                if (i >= 0) {
                    if (icons_[static_cast<size_t>(i)].version != n.uTimeoutOrVersion) changed = true;
                    icons_[static_cast<size_t>(i)].version = n.uTimeoutOrVersion;
                } else {
                    result = FALSE;
                }
                break;
            case NIM_SETFOCUS:
                break;
            default:
                result = FALSE;
                break;
        }
    }
    if (changed) PostChanged();
    // Our own icons: Explorer's answer is the answer.
    if (own) {
        DWORD_PTR mirrored = 0;
        HWND ex = ExplorerTray();
        const bool sent = ex && SendMessageTimeoutW(ex, WM_COPYDATA, reinterpret_cast<WPARAM>(sender),
                                                    reinterpret_cast<LPARAM>(&cds), SMTO_ABORTIFHUNG, kMirrorTimeoutMs, &mirrored);
        return sent ? static_cast<LRESULT>(mirrored) : FALSE;
    }
    // Everyone else gets our answer now; Explorer gets the message from the message loop, in order
    // (8a: mirrored inside the call, an app's Shell_NotifyIcon waited for Explorer, which is slow
    // right after it hid the taskbar, and timed out: many apps' icons never arrived).
    QueueMirror(sender, cds);
    return result;
}

void TrayHost::QueueMirror(HWND sender, const COPYDATASTRUCT& cds) {
    if (!cds.lpData || !cds.cbData || mirrors_.size() >= 4096) return;
    Mirror m;
    m.sender = sender;
    m.data.assign(static_cast<const BYTE*>(cds.lpData), static_cast<const BYTE*>(cds.lpData) + cds.cbData);
    // notifications.balloons (phase 9): the balloon goes to the dock's centre only, not to Explorer
    // (which would show it as a Windows toast); the rest of the message is mirrored as it is.
    constexpr size_t kFlagsAt = offsetof(TrayNotifyData, nid) + offsetof(NotifyIconData32, uFlags);
    if (stripBalloons_ && m.data.size() >= kFlagsAt + sizeof(UINT)) {
        UINT flags = 0;
        std::memcpy(&flags, m.data.data() + kFlagsAt, sizeof(flags));
        if (flags & NIF_INFO) {
            flags &= ~static_cast<UINT>(NIF_INFO);
            std::memcpy(m.data.data() + kFlagsAt, &flags, sizeof(flags));
            ++balloonsStripped_;
        }
    }
    mirrors_.push_back(std::move(m));
    if (!mirrorPosted_ && hwnd_) mirrorPosted_ = PostMessageW(hwnd_, kMirrorMessage, 0, 0) != FALSE;
}

void TrayHost::FlushMirrors() {
    mirrorPosted_ = false;
    // Taken out first: the sends below may dispatch new notify messages, which queue (and post) anew.
    std::deque<Mirror> batch;
    batch.swap(mirrors_);
    HWND ex = ExplorerTray();
    for (const Mirror& m : batch) {
        if (!ex) break;
        COPYDATASTRUCT copy{1, static_cast<DWORD>(m.data.size()), const_cast<BYTE*>(m.data.data())};
        DWORD_PTR ignored = 0;
        SendMessageTimeoutW(ex, WM_COPYDATA, reinterpret_cast<WPARAM>(m.sender), reinterpret_cast<LPARAM>(&copy),
                            SMTO_ABORTIFHUNG, kMirrorTimeoutMs, &ignored);
    }
}

LRESULT TrayHost::OnIconRect(const COPYDATASTRUCT& cds) {
    if (cds.cbData < sizeof(IconIdentifier32) || !cds.lpData) return 0;
    IconIdentifier32 q;
    std::memcpy(&q, cds.lpData, sizeof(q));
    if (q.dwMagic != kSignature) return 0;
    ++rectQueries_;
    const bool byGuid = !IsEqualGUID(q.guidItem, GUID_NULL);
    const int i = Find(ToHwnd(q.hWnd), q.uID, byGuid ? &q.guidItem : nullptr);
    RECT r{};
    if (i < 0 || !resolver_ || !resolver_(icons_[static_cast<size_t>(i)], r)) return 0;
    if (q.dwMessage == 1) return MAKELONG(static_cast<WORD>(r.left), static_cast<WORD>(r.top));
    return MAKELONG(static_cast<WORD>(r.right - r.left), static_cast<WORD>(r.bottom - r.top));
}

const TrayIcon* TrayHost::ById(uint64_t id) const {
    for (const TrayIcon& t : icons_) {
        if (t.id == id) return &t;
    }
    return nullptr;
}

const TrayIcon* TrayHost::ByKey(const std::wstring& key) const {
    for (const TrayIcon& t : icons_) {
        if (t.key == key) return &t;
    }
    return nullptr;
}

void TrayHost::Remove(size_t index) {
    if (index >= icons_.size()) return;
    if (icons_[index].icon) DestroyIcon(icons_[index].icon);
    icons_.erase(icons_.begin() + static_cast<ptrdiff_t>(index));
}

void TrayHost::RemoveOwner(DWORD pid) {
    auto w = waits_.find(pid);
    if (w != waits_.end()) {
        if (w->second.wait) UnregisterWaitEx(w->second.wait, nullptr);  // it fired (once): no wait
        if (w->second.process) CloseHandle(w->second.process);
        waits_.erase(w);
    }
    images_.erase(pid);  // the pid may be reused by another program
    bool any = false;
    for (size_t i = icons_.size(); i-- > 0;) {
        if (icons_[i].pid == pid) {
            Remove(i);
            any = true;
        }
    }
    if (any) PostChanged();
}

void TrayHost::WatchOwner(DWORD pid) {
    if (!pid || waits_.count(pid)) return;
    OwnerWait w;
    w.process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (w.process &&
        !RegisterWaitForSingleObject(&w.wait, w.process, &TrayHost::OnOwnerExit, reinterpret_cast<PVOID>(static_cast<ULONG_PTR>(pid)),
                                     INFINITE, WT_EXECUTEONLYONCE)) {
        w.wait = nullptr;
    }
    waits_.emplace(pid, w);  // also without a handle (elevated owner): one try per pid
}

void CALLBACK TrayHost::OnOwnerExit(PVOID context, BOOLEAN) {
    // Thread pool: only post. Stop() unregisters every wait (and waits for callbacks) first.
    if (HWND hwnd = live_.load()) PostMessageW(hwnd, kOwnerGoneMessage, reinterpret_cast<WPARAM>(context), 0);
}

void TrayHost::PostChanged() {
    if (changedPosted_ || !hwnd_) return;
    changedPosted_ = PostMessageW(hwnd_, kChangedMessage, 0, 0) != FALSE;
}

// ---------------------------------------------------------------------------------------------
// Input

bool TrayHost::Post(const TrayIcon& icon, UINT event, POINT anchor) const {
    WPARAM wp;
    LPARAM lp;
    if (icon.version >= NOTIFYICON_VERSION_4) {
        wp = MAKEWPARAM(static_cast<WORD>(anchor.x), static_cast<WORD>(anchor.y));
        lp = MAKELPARAM(static_cast<WORD>(event), static_cast<WORD>(icon.uid));
    } else {
        wp = icon.uid;
        lp = event;
    }
    return PostMessageW(icon.owner, icon.callback, wp, lp) != FALSE;
}

bool TrayHost::Forward(uint64_t id, Click click, POINT anchor) {
    const TrayIcon* icon = ById(id);
    if (!icon || !icon->callback) return false;
    if (!IsWindow(icon->owner)) {
        RemoveOwner(icon->pid);
        return false;
    }
    // The click was ours (the dock had the input): let the owner bring up its menu / window.
    AllowSetForegroundWindow(icon->pid);
    const TrayIcon t = *icon;  // Post never re-enters, but keep no reference anyway
    const bool v3 = t.version >= NOTIFYICON_VERSION;
    bool ok = true;
    switch (click) {
        case Click::Left:
            ok = Post(t, WM_LBUTTONDOWN, anchor) && Post(t, WM_LBUTTONUP, anchor);
            if (ok && v3) ok = Post(t, NIN_SELECT, anchor);
            break;
        case Click::Double:
            ok = Post(t, WM_LBUTTONDBLCLK, anchor) && Post(t, WM_LBUTTONUP, anchor);
            break;
        case Click::Right:
            ok = Post(t, WM_RBUTTONDOWN, anchor) && Post(t, WM_RBUTTONUP, anchor);
            if (ok && v3) ok = Post(t, WM_CONTEXTMENU, anchor);
            break;
        case Click::Middle:
            ok = Post(t, WM_MBUTTONDOWN, anchor) && Post(t, WM_MBUTTONUP, anchor);
            break;
        case Click::KeySelect:
            ok = v3 ? Post(t, NIN_KEYSELECT, anchor) : Post(t, WM_LBUTTONDOWN, anchor) && Post(t, WM_LBUTTONUP, anchor);
            break;
        case Click::ContextKey:
            ok = v3 ? Post(t, WM_CONTEXTMENU, anchor) : Post(t, WM_RBUTTONDOWN, anchor) && Post(t, WM_RBUTTONUP, anchor);
            break;
    }
    if (!ok && !IsWindow(t.owner)) {
        RemoveOwner(t.pid);
        return false;
    }
    return ok;
}

void TrayHost::MouseMove(uint64_t id, POINT anchor) {
    if (const TrayIcon* icon = ById(id); icon && icon->callback) Post(*icon, WM_MOUSEMOVE, anchor);
}

void TrayHost::PopupOpen(uint64_t id, POINT anchor) {
    if (popupOpen_ && popupOpen_ != id) PopupClose(popupOpen_, anchor);
    const TrayIcon* icon = ById(id);
    if (!icon || !icon->callback || icon->StandardTip() || popupOpen_ == id) return;
    if (Post(*icon, NIN_POPUPOPEN, anchor)) popupOpen_ = id;
}

void TrayHost::PopupClose(uint64_t id, POINT anchor) {
    if (!id || id != popupOpen_) return;
    popupOpen_ = 0;
    if (const TrayIcon* icon = ById(id); icon && icon->callback) Post(*icon, NIN_POPUPCLOSE, anchor);
}
