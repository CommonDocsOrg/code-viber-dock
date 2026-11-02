#include "Launcher.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <winrt/base.h>

#include <iterator>
#include <string>

namespace Launcher {

namespace {

std::wstring DirectoryOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash);
}

bool Execute(const std::wstring& file, const std::wstring& directory, const std::wstring& parameters = {}) {
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpFile = file.c_str();
    sei.lpParameters = parameters.empty() ? nullptr : parameters.c_str();
    sei.lpDirectory = directory.empty() ? nullptr : directory.c_str();
    sei.nShow = SW_SHOWNORMAL;
    // We got the click, so we may hand the foreground right to whatever starts.
    AllowSetForegroundWindow(ASFW_ANY);
    if (ShellExecuteExW(&sei)) return true;
    wchar_t buf[96];
    swprintf_s(buf, L"WinDock: ShellExecuteEx failed (%lu)\n", GetLastError());
    OutputDebugStringW(buf);
    return false;
}

bool HasExtension(const std::wstring& path, const wchar_t* ext) {
    const size_t dot = path.rfind(L'.');
    return dot != std::wstring::npos && CompareStringOrdinal(path.c_str() + dot, -1, ext, -1, TRUE) == CSTR_EQUAL;
}

// The files as one shell item array (null if any of them cannot be parsed).
winrt::com_ptr<IShellItemArray> ItemArray(const std::vector<std::wstring>& files) {
    std::vector<PIDLIST_ABSOLUTE> pidls;
    pidls.reserve(files.size());
    bool ok = true;
    for (const std::wstring& f : files) {
        PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(f.c_str());
        if (!pidl) {
            ok = false;
            break;
        }
        pidls.push_back(pidl);
    }
    winrt::com_ptr<IShellItemArray> items;
    if (ok && !pidls.empty()) {
        if (FAILED(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(pidls.size()),
                                                     const_cast<PCIDLIST_ABSOLUTE_ARRAY>(pidls.data()), items.put()))) {
            items = nullptr;
        }
    }
    for (PIDLIST_ABSOLUTE p : pidls) ILFree(p);
    return items;
}

std::wstring QuotedList(const std::vector<std::wstring>& files) {
    std::wstring out;
    for (const std::wstring& f : files) {
        if (!out.empty()) out += L' ';
        out += L'"' + f + L'"';
    }
    return out;
}

void LogFailure(const wchar_t* what, HRESULT hr) {
    wchar_t buf[128];
    swprintf_s(buf, L"WinDock: %s failed (0x%08X)\n", what, static_cast<unsigned>(hr));
    OutputDebugStringW(buf);
}

bool SameText(const std::wstring& a, const std::wstring& b) {
    return !a.empty() && CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

// Opens the files through the item's app as a registered handler of the first file's type
// (SHAssocEnumHandlers; packaged apps are matched by AUMID, desktop apps by exe path). False when
// the app is not a handler of that type.
bool InvokeRegisteredHandler(const DockItem& item, const std::vector<std::wstring>& files) {
    const std::wstring& first = files.front();
    const size_t dot = first.rfind(L'.');
    const size_t slash = first.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return false;
    const std::wstring ext = first.substr(dot);
    winrt::com_ptr<IEnumAssocHandlers> handlers;
    if (FAILED(SHAssocEnumHandlers(ext.c_str(), ASSOC_FILTER_NONE, handlers.put()))) return false;
    winrt::com_ptr<IAssocHandler> match;
    for (;;) {
        winrt::com_ptr<IAssocHandler> h;
        ULONG fetched = 0;
        if (handlers->Next(1, h.put(), &fetched) != S_OK || fetched != 1) break;
        if (item.key.isAumid) {
            winrt::com_ptr<IObjectWithAppUserModelID> app;
            PWSTR id = nullptr;
            if (SUCCEEDED(h->QueryInterface(IID_PPV_ARGS(app.put()))) && SUCCEEDED(app->GetAppID(&id)) && id) {
                const bool same = SameText(item.key.Source(), id);
                CoTaskMemFree(id);
                if (same) match = h;
            }
        } else {
            PWSTR name = nullptr;
            if (SUCCEEDED(h->GetName(&name)) && name) {
                const std::wstring exe = name;
                CoTaskMemFree(name);
                if (SameText(item.key.Source(), exe) || SameText(item.targetPath, exe)) match = h;
            }
        }
        if (match) break;
    }
    if (!match) return false;
    winrt::com_ptr<IShellItemArray> items = ItemArray(files);
    winrt::com_ptr<IDataObject> data;
    if (!items || FAILED(items->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(data.put())))) return false;
    const HRESULT hr = match->Invoke(data.get());
    if (FAILED(hr)) LogFailure(L"IAssocHandler::Invoke", hr);
    return SUCCEEDED(hr);
}


}  // namespace

bool Launch(const DockItem& item) {
    if (item.kind == DockItem::Kind::Separator) return false;
    if (item.kind == DockItem::Kind::RecycleBin) return Execute(L"shell:RecycleBinFolder", {});
    if (item.launchPath && !item.launchPath->empty()) {
        const std::wstring dir = item.workingDir.empty() ? DirectoryOf(*item.launchPath) : item.workingDir;
        return Execute(*item.launchPath, dir);
    }
    if (item.key.isAumid) return Execute(L"shell:AppsFolder\\" + item.key.Source(), {});
    if (item.key.IsClassFallback()) return false;
    return Execute(item.key.Source(), DirectoryOf(item.key.Source()));
}

bool LaunchNewInstance(const DockItem& item) { return Launch(item); }

bool OpenWith(const DockItem& item, const std::vector<std::wstring>& files) {
    if (files.empty() || item.kind != DockItem::Kind::App) return false;
    AllowSetForegroundWindow(ASFW_ANY);
    // The way Explorer's "Open with" does it, when the app is registered for the file type.
    if (InvokeRegisteredHandler(item, files)) return true;
    const std::wstring quoted = QuotedList(files);
    if (item.key.isAumid) {
        // Not registered for the type: start the app with the files as arguments. (Not
        // ActivateForFile: a packaged desktop app does not support that contract, and a failed
        // file activation was seen to end the app's running instance.)
        if (item.launchPath && HasExtension(*item.launchPath, L".exe")) {  // an app execution alias
            return Execute(*item.launchPath, DirectoryOf(files.front()), quoted);
        }
        winrt::com_ptr<IApplicationActivationManager> manager;
        HRESULT hr = CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_LOCAL_SERVER,
                                      IID_PPV_ARGS(manager.put()));
        DWORD pid = 0;
        if (SUCCEEDED(hr)) hr = manager->ActivateApplication(item.key.Source().c_str(), quoted.c_str(), AO_NONE, &pid);
        if (FAILED(hr)) LogFailure(L"ActivateApplication", hr);
        return SUCCEEDED(hr);
    }
    if (item.launchPath && HasExtension(*item.launchPath, L".lnk")) {
        // A shortcut ignores extra parameters: run its target with the link's own arguments.
        winrt::com_ptr<IShellLinkW> link;
        winrt::com_ptr<IPersistFile> file;
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.put()))) &&
            SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(file.put()))) &&
            SUCCEEDED(file->Load(item.launchPath->c_str(), STGM_READ))) {
            std::wstring target(MAX_PATH, L'\0'), args(1024, L'\0'), dir(MAX_PATH, L'\0');
            if (SUCCEEDED(link->GetPath(target.data(), MAX_PATH, nullptr, 0)) && target[0]) {
                target.resize(wcslen(target.c_str()));
                if (FAILED(link->GetArguments(args.data(), static_cast<int>(args.size())))) args[0] = 0;
                args.resize(wcslen(args.c_str()));
                if (FAILED(link->GetWorkingDirectory(dir.data(), MAX_PATH))) dir[0] = 0;
                dir.resize(wcslen(dir.c_str()));
                if (dir.empty()) dir = DirectoryOf(target);
                return Execute(target, dir, args.empty() ? quoted : args + L' ' + quoted);
            }
        }
        if (item.targetPath.empty()) return false;
        return Execute(item.targetPath, item.workingDir.empty() ? DirectoryOf(item.targetPath) : item.workingDir,
                       quoted);
    }
    const std::wstring exe = item.launchPath && !item.launchPath->empty() ? *item.launchPath : item.key.Source();
    if (item.key.IsClassFallback() && !item.launchPath) return false;
    return Execute(exe, item.workingDir.empty() ? DirectoryOf(exe) : item.workingDir, quoted);
}

bool Recycle(const std::vector<std::wstring>& files, HWND owner) {
    if (files.empty()) return false;
    winrt::com_ptr<IFileOperation> op;
    HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(op.put()));
    winrt::com_ptr<IShellItemArray> items = SUCCEEDED(hr) ? ItemArray(files) : nullptr;
    if (SUCCEEDED(hr) && !items) hr = E_INVALIDARG;
    if (SUCCEEDED(hr)) hr = op->SetOperationFlags(FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE);
    if (SUCCEEDED(hr) && owner) op->SetOwnerWindow(owner);
    if (SUCCEEDED(hr)) hr = op->DeleteItems(items.get());
    if (SUCCEEDED(hr)) hr = op->PerformOperations();
    if (FAILED(hr)) LogFailure(L"Recycle", hr);
    return SUCCEEDED(hr);
}

namespace {

std::wstring Trim(const std::wstring& s) {
    const size_t b = s.find_first_not_of(L" \t");
    if (b == std::wstring::npos) return {};
    const size_t e = s.find_last_not_of(L" \t");
    return s.substr(b, e - b + 1);
}

std::wstring Expand(const std::wstring& s) {
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = ExpandEnvironmentStringsW(s.c_str(), buf, static_cast<DWORD>(std::size(buf)));
    return n > 0 && n <= std::size(buf) ? std::wstring(buf) : s;
}

// "scheme:..." with a scheme of two or more letters (a drive letter is one).
bool IsUrl(const std::wstring& s) {
    const size_t colon = s.find(L':');
    if (colon == std::wstring::npos || colon < 2) return false;
    for (size_t i = 0; i < colon; ++i) {
        const wchar_t c = s[i];
        if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'+' ||
              c == L'-' || c == L'.')) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool OpenTarget(const std::wstring& target) {
    std::wstring t = Trim(target);
    if (t.empty()) return false;
    if (IsUrl(t)) return Execute(t, {});
    std::wstring file = t, args;
    if (t.front() == L'"') {
        const size_t close = t.find(L'"', 1);
        file = close == std::wstring::npos ? t.substr(1) : t.substr(1, close - 1);
        if (close != std::wstring::npos) args = Trim(t.substr(close + 1));
    }
    file = Expand(file);
    return Execute(file, DirectoryOf(file), Expand(args));
}

std::wstring TargetName(const std::wstring& target) {
    std::wstring t = Trim(target);
    if (t.empty()) return {};
    if (IsUrl(t)) {
        const size_t slashes = t.find(L"://");
        if (slashes == std::wstring::npos) return t.substr(0, t.find(L':'));  // mailto:, ms-settings: ...
        const size_t start = slashes + 3;
        const size_t end = t.find_first_of(L"/?#", start);
        return t.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
    }
    if (t.front() == L'"') {
        const size_t close = t.find(L'"', 1);
        t = close == std::wstring::npos ? t.substr(1) : t.substr(1, close - 1);
    }
    const size_t slash = t.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? t : t.substr(slash + 1);
    const size_t dot = name.rfind(L'.');
    if (dot != std::wstring::npos && dot > 0) name.resize(dot);
    return name;
}

}  // namespace Launcher
