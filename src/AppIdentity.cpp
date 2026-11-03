#include "AppIdentity.h"

#include <Unknwn.h>
#include <appmodel.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl_core.h>

#include <winrt/base.h>

#include <cwctype>
#include <iterator>
#include <string>
#include <vector>
#include <winioctl.h>

namespace AppIdentity {

namespace {

// --- Alias table: keys that name the same app. The canonical key is the exe path. ---
struct Alias {
    const wchar_t* aumid;        // as reported by windows / shortcuts
    const wchar_t* path;         // canonical exe path (environment variables allowed)
    const wchar_t* displayName;  // shown instead of the exe's FileDescription
};
constexpr Alias kAliases[] = {
    {L"Microsoft.Windows.Explorer", L"%WINDIR%\\explorer.exe", L"File Explorer"},
};

std::wstring Expand(const std::wstring& s) {
    if (s.find(L'%') == std::wstring::npos) return s;
    const DWORD n = ExpandEnvironmentStringsW(s.c_str(), nullptr, 0);
    if (n == 0) return s;
    std::wstring out(n, L'\0');
    const DWORD m = ExpandEnvironmentStringsW(s.c_str(), out.data(), n);
    if (m == 0 || m > n) return s;
    out.resize(m - 1);
    return out;
}

struct ExpandedAlias {
    std::wstring aumidId;  // normalized
    std::wstring pathId;   // normalized
    std::wstring path;     // expanded, original case
    const wchar_t* displayName;
};

const std::vector<ExpandedAlias>& Aliases();
const ExpandedAlias* FindAlias(const AppKey& key);

std::wstring ClassNameOf(HWND hwnd) {
    wchar_t buf[256]{};
    const int n = GetClassNameW(hwnd, buf, static_cast<int>(std::size(buf)));
    return std::wstring(buf, n > 0 ? static_cast<size_t>(n) : 0);
}

std::wstring WindowAumid(HWND hwnd) {
    winrt::com_ptr<IPropertyStore> store;
    if (FAILED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(store.put())))) return {};
    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::wstring result;
    if (SUCCEEDED(store->GetValue(PKEY_AppUserModel_ID, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) {
        result = pv.pwszVal;
    }
    PropVariantClear(&pv);
    return result;
}

std::wstring LongPath(const std::wstring& path) {
    if (path.find(L'~') == std::wstring::npos) return path;
    const DWORD n = GetLongPathNameW(path.c_str(), nullptr, 0);
    if (n == 0) return path;
    std::wstring out(n, L'\0');
    const DWORD m = GetLongPathNameW(path.c_str(), out.data(), n);
    if (m == 0 || m >= n) return path;
    out.resize(m);
    return out;
}

// Identity of a process: its package AUMID when packaged (single-app packages), else its exe path.
std::optional<AppKey> IdentifyProcess(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return std::nullopt;

    std::optional<AppKey> key;
    UINT32 len = 0;
    if (GetApplicationUserModelId(process, &len, nullptr) == ERROR_INSUFFICIENT_BUFFER && len > 1) {
        std::wstring aumid(len, L'\0');
        if (GetApplicationUserModelId(process, &len, aumid.data()) == ERROR_SUCCESS) {
            aumid.resize(wcsnlen(aumid.c_str(), aumid.size()));
            if (!aumid.empty()) key = MakeAumidKey(aumid);
        }
    }
    if (!key) {
        std::wstring path(MAX_PATH, L'\0');
        for (int attempt = 0; attempt < 3; ++attempt) {
            DWORD size = static_cast<DWORD>(path.size());
            if (QueryFullProcessImageNameW(process, 0, path.data(), &size)) {
                path.resize(size);
                key = MakePathKey(LongPath(path));
                break;
            }
            if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) break;
            path.resize(path.size() * 4);
        }
    }
    CloseHandle(process);
    return key;
}

struct CoreWindowSearch {
    DWORD framePid = 0;
    HWND found = nullptr;
};

BOOL CALLBACK FindCoreWindow(HWND child, LPARAM lp) {
    auto* s = reinterpret_cast<CoreWindowSearch*>(lp);
    if (ClassNameOf(child) != L"Windows.UI.Core.CoreWindow") return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(child, &pid);
    if (pid == s->framePid) return TRUE;  // still the host's own placeholder
    s->found = child;
    return FALSE;
}

}  // namespace

std::wstring Normalize(const std::wstring& s) {
    std::wstring out = s;
    if (!out.empty()) {
        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, s.c_str(), static_cast<int>(s.size()), out.data(),
                      static_cast<int>(out.size()), nullptr, nullptr, 0);
    }
    return out;
}

AppKey MakeAumidKey(const std::wstring& aumid) {
    AppKey k;
    k.id = Normalize(aumid);
    k.isAumid = true;
    k.original = aumid;
    return k;
}

AppKey MakePathKey(const std::wstring& path) {
    AppKey k;
    k.id = Normalize(path);
    k.isAumid = false;
    k.original = path;
    return k;
}

namespace {
std::optional<AppKey> IdentifyWindowRaw(HWND hwnd);
}

std::optional<AppKey> IdentifyWindow(HWND hwnd) {
    std::optional<AppKey> key = IdentifyWindowRaw(hwnd);
    if (key) key = Canonicalize(std::move(*key));
    return key;
}

AppKey Canonicalize(AppKey key) {
    if (!key.isAumid) return key;  // path keys are already canonical
    const ExpandedAlias* a = FindAlias(key);
    return a ? MakePathKey(a->path) : key;
}

namespace {

std::optional<AppKey> IdentifyWindowRaw(HWND hwnd) {
    if (!IsWindow(hwnd)) return std::nullopt;

    if (std::wstring aumid = WindowAumid(hwnd); !aumid.empty()) return MakeAumidKey(aumid);

    const std::wstring cls = ClassNameOf(hwnd);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);

    if (cls == L"ApplicationFrameWindow") {
        // UWP host frame without the property: identify the hosted app's process instead of
        // ApplicationFrameHost.exe. If the content isn't attached yet, try again later.
        CoreWindowSearch search{pid, nullptr};
        EnumChildWindows(hwnd, FindCoreWindow, reinterpret_cast<LPARAM>(&search));
        if (!search.found) return std::nullopt;
        if (std::wstring aumid = WindowAumid(search.found); !aumid.empty()) return MakeAumidKey(aumid);
        DWORD childPid = 0;
        GetWindowThreadProcessId(search.found, &childPid);
        if (auto key = IdentifyProcess(childPid)) return key;
        return std::nullopt;
    }

    if (auto key = IdentifyProcess(pid)) return key;

    AppKey k;
    k.id = L"class:" + Normalize(cls);
    k.original = L"class:" + cls;
    k.isAumid = false;
    return k;
}

}  // namespace

std::wstring DisplayNameFor(const AppKey& key) {
    if (key.isAumid) {
        const std::wstring parsing = L"shell:AppsFolder\\" + key.Source();
        winrt::com_ptr<IShellItem> item;
        if (SUCCEEDED(SHCreateItemFromParsingName(parsing.c_str(), nullptr, IID_PPV_ARGS(item.put())))) {
            PWSTR name = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) && name) {
                std::wstring result = name;
                CoTaskMemFree(name);
                if (!result.empty()) return result;
            }
        }
        // "Publisher.App_hash!Entry" -> "App"
        std::wstring s = key.Source();
        if (size_t bang = s.find(L'!'); bang != std::wstring::npos) s.resize(bang);
        if (size_t us = s.find(L'_'); us != std::wstring::npos) s.resize(us);
        if (size_t dot = s.rfind(L'.'); dot != std::wstring::npos && dot + 1 < s.size()) s.erase(0, dot + 1);
        return s;
    }

    if (key.IsClassFallback()) return key.Source().substr(6);
    if (const ExpandedAlias* a = FindAlias(key)) return a->displayName;

    const std::wstring& path = key.Source();
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeExW(FILE_VER_GET_NEUTRAL, path.c_str(), &handle);
    if (size) {
        std::vector<BYTE> data(size);
        if (GetFileVersionInfoExW(FILE_VER_GET_NEUTRAL, path.c_str(), 0, size, data.data())) {
            struct LangCodePage {
                WORD lang;
                WORD codePage;
            };
            LangCodePage* translations = nullptr;
            UINT bytes = 0;
            std::vector<LangCodePage> candidates;
            if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations),
                               &bytes) &&
                translations) {
                candidates.assign(translations, translations + bytes / sizeof(LangCodePage));
            }
            candidates.push_back({0x0409, 0x04B0});  // en-US, Unicode
            candidates.push_back({0x0409, 0x04E4});  // en-US, Windows-1252
            for (const auto& t : candidates) {
                wchar_t sub[64];
                swprintf_s(sub, L"\\StringFileInfo\\%04x%04x\\FileDescription", t.lang, t.codePage);
                wchar_t* value = nullptr;
                UINT len = 0;
                if (VerQueryValueW(data.data(), sub, reinterpret_cast<void**>(&value), &len) && value && len > 1) {
                    std::wstring desc(value, wcsnlen(value, len));
                    while (!desc.empty() && iswspace(desc.back())) desc.pop_back();
                    if (!desc.empty()) return desc;
                }
            }
        }
    }

    // File stem.
    size_t start = path.find_last_of(L"\\/");
    start = start == std::wstring::npos ? 0 : start + 1;
    size_t end = path.rfind(L'.');
    if (end == std::wstring::npos || end < start) end = path.size();
    return path.substr(start, end - start);
}

namespace {

const std::vector<ExpandedAlias>& Aliases() {
    static const std::vector<ExpandedAlias> table = [] {
        std::vector<ExpandedAlias> t;
        for (const Alias& a : kAliases) {
            const std::wstring path = Expand(a.path);
            t.push_back({Normalize(a.aumid), Normalize(path), path, a.displayName});
        }
        return t;
    }();
    return table;
}

const ExpandedAlias* FindAlias(const AppKey& key) {
    for (const ExpandedAlias& a : Aliases()) {
        if (key.isAumid ? key.id == a.aumidId : key.id == a.pathId) return &a;
    }
    return nullptr;
}

std::wstring DirectoryOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash);
}

std::wstring FileStem(const std::wstring& path) {
    size_t start = path.find_last_of(L"\\/");
    start = start == std::wstring::npos ? 0 : start + 1;
    size_t end = path.rfind(L'.');
    if (end == std::wstring::npos || end < start) end = path.size();
    return path.substr(start, end - start);
}

bool HasExtension(const std::wstring& path, const wchar_t* ext) {
    const size_t dot = path.rfind(L'.');
    return dot != std::wstring::npos && Normalize(path.substr(dot)) == ext;
}

bool FileExists(const std::wstring& path) {
    return !path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool IsDirectory(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

// Absolute, long-form path (environment variables expanded, no 8.3 names or relative parts).
std::wstring FullPath(const std::wstring& path) {
    const std::wstring expanded = Expand(path);
    const DWORD n = GetFullPathNameW(expanded.c_str(), 0, nullptr, nullptr);
    if (n == 0) return expanded;
    std::wstring out(n, L'\0');
    const DWORD m = GetFullPathNameW(expanded.c_str(), n, out.data(), nullptr);
    if (m == 0 || m >= n) return expanded;
    out.resize(m);
    return LongPath(out);
}

// App execution aliases (%LOCALAPPDATA%\Microsoft\WindowsApps\*.exe) are IO_REPARSE_TAG_APPEXECLINK
// reparse points. Their data: ULONG version, then NUL-terminated strings: package family,
// AUMID, target exe, (version 3) app type. Returns the AUMID, or empty for other files.
std::wstring AppExecLinkAumid(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return {};
    HANDLE h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::vector<BYTE> buf(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
    DWORD bytes = 0;
    const BOOL ok = DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, nullptr, 0, buf.data(), static_cast<DWORD>(buf.size()),
                                    &bytes, nullptr);
    CloseHandle(h);
    // Header: ULONG tag, USHORT data length, USHORT reserved; then the ULONG version.
    constexpr size_t kData = 8 + 4;
    if (!ok || bytes < kData) return {};
    ULONG tag = 0;
    memcpy(&tag, buf.data(), sizeof(tag));
    if (tag != IO_REPARSE_TAG_APPEXECLINK) return {};
    std::vector<std::wstring> parts;
    std::wstring cur;
    const size_t count = (bytes - kData) / sizeof(wchar_t);
    for (size_t i = 0; i < count && parts.size() < 3; ++i) {
        wchar_t c;
        memcpy(&c, buf.data() + kData + i * sizeof(wchar_t), sizeof(c));
        if (c == L'\0') {
            parts.push_back(std::move(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    return parts.size() >= 2 ? parts[1] : std::wstring{};
}

// Key for an executable: the package AUMID behind an app execution alias, else the path.
AppKey KeyForExe(const std::wstring& exe) {
    if (std::wstring aumid = AppExecLinkAumid(exe); !aumid.empty()) return Canonicalize(MakeAumidKey(aumid));
    return Canonicalize(MakePathKey(exe));
}

std::wstring LinkAumid(IShellLinkW* link) {
    winrt::com_ptr<IPropertyStore> store;
    if (FAILED(link->QueryInterface(IID_PPV_ARGS(store.put())))) return {};
    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::wstring result;
    if (SUCCEEDED(store->GetValue(PKEY_AppUserModel_ID, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) {
        result = pv.pwszVal;
    }
    PropVariantClear(&pv);
    return result;
}

void ResolveShortcut(const std::wstring& lnk, ResolvedPin& pin) {
    winrt::com_ptr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.put())))) return;
    winrt::com_ptr<IPersistFile> file = link.try_as<IPersistFile>();
    if (!file || FAILED(file->Load(lnk.c_str(), STGM_READ))) return;
    // No UI and don't rewrite the .lnk; with SLR_NO_UI the high word is the search timeout (ms).
    link->Resolve(nullptr, MAKELONG(SLR_NO_UI | SLR_NOUPDATE, 1000));

    wchar_t buf[MAX_PATH * 2]{};
    if (SUCCEEDED(link->GetPath(buf, static_cast<int>(std::size(buf)), nullptr, 0)) && buf[0]) {
        pin.targetPath = FullPath(buf);
    }
    buf[0] = 0;
    if (SUCCEEDED(link->GetWorkingDirectory(buf, static_cast<int>(std::size(buf)))) && buf[0]) {
        pin.workingDir = Expand(buf);
    } else if (!pin.targetPath.empty()) {
        pin.workingDir = DirectoryOf(pin.targetPath);
    }
    buf[0] = 0;
    int index = 0;
    if (SUCCEEDED(link->GetIconLocation(buf, static_cast<int>(std::size(buf)), &index)) && buf[0]) {
        pin.iconFile = Expand(buf);
        pin.iconIndex = index;
    }

    if (std::wstring aumid = LinkAumid(link.get()); !aumid.empty()) {
        pin.key = Canonicalize(MakeAumidKey(aumid));
    } else if (!pin.targetPath.empty()) {
        pin.key = KeyForExe(pin.targetPath);
        if (!FileExists(pin.targetPath)) pin.missing = true;
    }
}

}  // namespace

bool AumidInstalled(const std::wstring& aumid) {
    const size_t bang = aumid.find(L'!');
    if (bang == std::wstring::npos || bang == 0) return false;
    const std::wstring family = aumid.substr(0, bang);
    UINT32 count = 0;
    UINT32 bufferLength = 0;
    const LONG rc = GetPackagesByPackageFamily(family.c_str(), &count, nullptr, &bufferLength, nullptr);
    return (rc == ERROR_SUCCESS || rc == ERROR_INSUFFICIENT_BUFFER) && count > 0;
}

AppKey KeyForNotificationAumid(const std::wstring& aumid) {
    if (aumid.empty()) return AppKey{};
    // "{GUID}\rest" / "{GUID}/rest": a known folder (FOLDERID_*) and a path inside it.
    if (aumid.size() > 39 && aumid[0] == L'{' && aumid[37] == L'}' && (aumid[38] == L'\\' || aumid[38] == L'/')) {
        GUID id{};
        if (SUCCEEDED(CLSIDFromString(aumid.substr(0, 38).c_str(), &id))) {
            PWSTR base = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &base)) && base) {
                std::wstring path = base;
                CoTaskMemFree(base);
                std::wstring rest = aumid.substr(39);
                for (wchar_t& c : rest) {
                    if (c == L'/') c = L'\\';
                }
                if (!path.empty() && path.back() != L'\\') path += L'\\';
                return Canonicalize(MakePathKey(path + rest));
            }
            if (base) CoTaskMemFree(base);
        }
    }
    // A full path (some apps use their exe path as the id).
    if (aumid.size() > 3 && ((aumid[1] == L':' && aumid[2] == L'\\') || aumid.rfind(L"\\\\", 0) == 0)) {
        return Canonicalize(MakePathKey(aumid));
    }
    // A package's "Family!App", or an explicit id such as "Chrome".
    return Canonicalize(MakeAumidKey(aumid));
}

namespace {

AppKey SyntheticKey(const std::wstring& id) {
    AppKey k;
    k.id = id;  // already lowercase
    k.original = id;
    k.isAumid = false;
    return k;
}

// The directory as one canonical path: full, long form, no trailing separator except for a
// drive root ("C:\").
std::wstring CanonicalDirectory(const std::wstring& dir) {
    std::wstring d = FullPath(dir);
    while (d.size() > 3 && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    return d;
}

void MakeFolder(const std::wstring& dir, ResolvedPin& pin) {
    const std::wstring d = CanonicalDirectory(dir);
    pin.kind = PinKind::Folder;
    pin.key = MakePathKey(d);
    pin.launchPath = d;
    pin.targetPath = d;
    pin.workingDir = d;
    pin.missing = false;
    // "C:\" -> "C:"; otherwise the last component.
    if (d.size() <= 3 && d.size() >= 2 && d[1] == L':') {
        pin.displayName = d.substr(0, 2);
    } else {
        const size_t slash = d.find_last_of(L"\\/");
        pin.displayName = slash == std::wstring::npos ? d : d.substr(slash + 1);
    }
}

// "file", "file,3", "file,-120" (a negative index is a resource id, as in shortcuts and the
// registry). Only an index made of an optional '-' and digits is split off, so a path with a
// comma in it still works. Applied only when the file exists.
void ApplyCustomIcon(const std::wstring& spec, ResolvedPin& pin) {
    if (spec.empty()) return;
    std::wstring file = spec;
    int index = 0;
    if (const size_t comma = spec.rfind(L','); comma != std::wstring::npos) {
        size_t b = comma + 1, e = spec.size();
        while (b < e && iswspace(spec[b])) ++b;
        while (e > b && iswspace(spec[e - 1])) --e;
        const size_t digits = b < e && spec[b] == L'-' ? b + 1 : b;
        bool numeric = digits < e && e - digits <= 9;
        for (size_t i = digits; numeric && i < e; ++i) numeric = iswdigit(spec[i]) != 0;
        if (numeric) {
            index = _wtoi(spec.substr(b, e - b).c_str());
            file = spec.substr(0, comma);
        }
    }
    while (!file.empty() && iswspace(file.back())) file.pop_back();
    while (!file.empty() && iswspace(file.front())) file.erase(0, 1);
    if (file.size() >= 2 && file.front() == L'"' && file.back() == L'"') file = file.substr(1, file.size() - 2);
    if (file.empty()) return;
    file = FullPath(file);  // expands environment variables
    if (!FileExists(file) || IsDirectory(file)) return;  // keep the default icon
    pin.iconFile = file;
    pin.iconIndex = index;
    pin.customIcon = true;
}

}  // namespace

ResolvedPin ResolvePin(const DockItemEntry& entry, size_t ordinal) {
    ResolvedPin pin;
    switch (entry.Kind()) {
        case ItemKind::Separator:
            pin.kind = PinKind::Separator;
            pin.key = SyntheticKey(L"kind:separator#" + std::to_wstring(ordinal));
            pin.displayName = L"Separator";
            return pin;
        case ItemKind::RecycleBin:
            pin.kind = PinKind::RecycleBin;
            pin.key = SyntheticKey(L"kind:recyclebin");
            pin.displayName = L"Recycle Bin";
            ApplyCustomIcon(entry.icon, pin);
            return pin;
        case ItemKind::Group:
            // The members are resolved by the caller (ResolvedPin::members; they are cached).
            pin.kind = PinKind::Group;
            pin.key = SyntheticKey(L"kind:group#" + std::to_wstring(ordinal));
            pin.displayName = entry.name.empty() ? std::wstring(L"Group") : entry.name;
            ApplyCustomIcon(entry.icon, pin);
            return pin;
        default:  // app, folder (detected from the path below)
            break;
    }

    const std::wstring& path = entry.path;
    if (path.empty()) {
        pin.key = Canonicalize(MakeAumidKey(entry.aumid));
        pin.displayName = DisplayNameFor(pin.key);
        ApplyCustomIcon(entry.icon, pin);
        return pin;
    }

    const std::wstring full = FullPath(path);
    if (IsDirectory(full)) {
        MakeFolder(full, pin);
        ApplyCustomIcon(entry.icon, pin);
        return pin;
    }
    pin.launchPath = full;
    pin.workingDir = DirectoryOf(full);
    pin.key = Canonicalize(MakePathKey(full));  // fallback: unreadable link / link without a target

    if (HasExtension(full, L".lnk")) {
        pin.displayName = FileStem(full);
        ResolveShortcut(full, pin);
        if (IsDirectory(pin.targetPath)) {
            // A shortcut to a folder is that folder: same key, name and launch as a folder pin.
            // Its own icon location (if any) is kept as the icon.
            const std::wstring linkIcon = pin.iconFile;
            const int linkIndex = pin.iconIndex;
            MakeFolder(pin.targetPath, pin);
            pin.iconFile = linkIcon;
            pin.iconIndex = linkIndex;
            ApplyCustomIcon(entry.icon, pin);
            return pin;
        }
    } else {
        pin.key = KeyForExe(full);
        pin.targetPath = full;
        pin.displayName = DisplayNameFor(pin.key);
    }
    if (!FileExists(full)) pin.missing = true;
    if (pin.missing) pin.iconFile.clear();
    if (pin.displayName.empty()) pin.displayName = FileStem(full);
    ApplyCustomIcon(entry.icon, pin);
    return pin;
}

}  // namespace AppIdentity
