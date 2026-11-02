#include "Launcher.h"

#include <shellapi.h>

#include <string>

namespace Launcher {

namespace {

std::wstring DirectoryOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash);
}

bool Execute(const std::wstring& file, const std::wstring& directory) {
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpFile = file.c_str();
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

}  // namespace Launcher
