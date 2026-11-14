#include "WidgetInstall.h"

#include <shellapi.h>
#include <shobjidl.h>

#include <winrt/base.h>

#include <algorithm>
#include <cwctype>

#include "Config.h"
#include "JsonUtil.h"
#include "WidgetPackage.h"
#include "Zip.h"

// Round 29 phase 29B. Why an in-tree reader and not the Shell's zip folder (IShellItem /
// IFileOperation copying out of the zip namespace): the Shell's extraction gives no say over what
// it writes. Its names, sizes and the ratio are known only after the copy (a zip bomb fills the
// disk before anything can refuse it), it decides by itself how to treat "..", absolute and
// device names and links, it may show its own dialogs (conflicts, passwords, progress) from a
// worker thread, and zipfldr.dll's behaviour differs between Windows builds. Zip.cpp is ~450 lines
// with a test suite (tests/zip), checks everything before a byte is written and writes only into a
// fresh folder of its own; what it does not support (zip64, encryption, other methods) is refused,
// which costs nothing for a widget of a few hundred KB.

namespace Widgets {

namespace {

using JsonUtil::Utf8ToWide;

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

// Deletes a folder tree we created (staging). A reparse point is removed as itself, never followed.
void DeleteTree(const std::wstring& dir, int depth = 0) {
    if (depth > 32) return;
    WIN32_FIND_DATAW fd{};
    HANDLE f = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            const std::wstring p = dir + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(p.c_str(), fd.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                DeleteTree(p, depth + 1);
            } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                RemoveDirectoryW(p.c_str());
            } else {
                DeleteFileW(p.c_str());
            }
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    RemoveDirectoryW(dir.c_str());
}

// A fresh folder %LocalAppData%\WinDock\install-<tick>-<pid>-<n> (created by this call: never one
// that existed before).
bool MakeStagingRoot(std::wstring& root) {
    CreateDirectoryW(Config::Directory().c_str(), nullptr);
    for (int n = 0; n < 100; ++n) {
        wchar_t name[96];
        swprintf_s(name, L"\\install-%llx-%lx-%d", static_cast<unsigned long long>(GetTickCount64()),
                   static_cast<unsigned long>(GetCurrentProcessId()), n);
        root = Config::Directory() + name;
        if (CreateDirectoryW(root.c_str(), nullptr)) return true;
        if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
    }
    return false;
}

// A widget id from a zip's file name ("My Clock (2).zip" -> "my-clock-2"): lower case, anything but
// a-z 0-9 - _ . becomes '-', runs of '-' collapse, no '-' or '.' at either end.
std::wstring IdFromFileName(const std::wstring& path) {
    std::wstring stem = path.substr(path.find_last_of(L"\\/") == std::wstring::npos ? 0 : path.find_last_of(L"\\/") + 1);
    if (const size_t dot = stem.find_last_of(L'.'); dot != std::wstring::npos && dot > 0) stem.resize(dot);
    std::wstring id;
    for (wchar_t c : Lower(stem)) {
        const bool ok = (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'_' || c == L'.';
        if (ok) {
            id += c;
        } else if (!id.empty() && id.back() != L'-') {
            id += L'-';
        }
    }
    while (!id.empty() && (id.back() == L'-' || id.back() == L'.')) id.pop_back();
    while (!id.empty() && (id.front() == L'-' || id.front() == L'.')) id.erase(0, 1);
    if (id.size() > 64) id.resize(64);
    return id;
}

bool ReadArchive(const std::wstring& path, size_t cap, std::string& out, std::wstring& error) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = L"the file cannot be opened";
        return false;
    }
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) != FALSE;
    if (ok && static_cast<unsigned long long>(size.QuadPart) > cap) {
        error = L"the file is larger than " + std::to_wstring(cap >> 20) + L" MB";
        ok = false;
    } else if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = out.empty() || (ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) && read == out.size());
        if (!ok) error = L"the file cannot be read";
    } else {
        error = L"the file cannot be read";
    }
    CloseHandle(h);
    return ok;
}

bool WriteNew(const std::wstring& path, const std::string& bytes) {
    // CREATE_NEW: a name that exists (it cannot, in a fresh folder, unless the archive named it
    // twice, which ReadDirectory refuses) is never written through.
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = bytes.empty() || (WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size());
    CloseHandle(h);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

void Prepare(ZipInstall& job) {
    const Zip::Limits limits;
    std::string archive;
    if (!ReadArchive(job.zipPath, limits.maxArchiveBytes, archive, job.error)) return;
    std::vector<Zip::Entry> entries;
    std::string zipError;
    if (!Zip::ReadDirectory(archive, limits, entries, zipError)) {
        job.error = Utf8ToWide(zipError);
        return;
    }
    // A Finder-made archive's __MACOSX/ (resource forks) and .DS_Store are not the widget's.
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [](const Zip::Entry& e) {
                                     const size_t slash = e.name.find_last_of('/', e.directory ? e.name.size() - 2 : std::string::npos);
                                     const std::string last = e.name.substr(slash == std::string::npos ? 0 : slash + 1);
                                     return e.name.rfind("__MACOSX/", 0) == 0 || last == ".DS_Store";
                                 }),
                  entries.end());
    // The layout: widget.json at the root, or the only top-level folder holds it.
    std::string prefix;
    const bool flat = std::any_of(entries.begin(), entries.end(), [](const Zip::Entry& e) { return e.name == "widget.json"; });
    if (flat) {
        job.id = IdFromFileName(job.zipPath);
        if (!ValidId(job.id)) {
            job.error = L"the zip's name does not make a widget id (rename it to the widget's id, such as my-widget.zip)";
            return;
        }
    } else {
        std::string top;
        for (const Zip::Entry& e : entries) {
            const std::string first = e.name.substr(0, e.name.find('/'));
            if (e.name.find('/') == std::string::npos) {
                top.clear();
                break;  // a file at the root without a widget.json there
            }
            if (top.empty()) top = first;
            if (first != top) {
                top.clear();
                break;
            }
        }
        const bool hasManifest = !top.empty() && std::any_of(entries.begin(), entries.end(), [&](const Zip::Entry& e) {
            return e.name == top + "/widget.json";
        });
        if (!hasManifest) {
            job.error = L"the zip holds no widget: widget.json must be at its root, or in its only top-level folder";
            return;
        }
        job.id = Lower(Utf8ToWide(top));
        if (!ValidId(job.id)) {
            job.error = L"the folder in the zip, \"" + Utf8ToWide(top) + L"\", is not a widget id (1 - 64 of a-z 0-9 - _ .)";
            return;
        }
        prefix = top + "/";
    }
    if (!MakeStagingRoot(job.stagingRoot)) {
        job.error = L"a folder to unpack it into cannot be made in " + Config::Directory();
        job.stagingRoot.clear();
        return;
    }
    job.staged = job.stagingRoot + L"\\" + job.id;
    if (!CreateDirectoryW(job.staged.c_str(), nullptr)) {
        job.error = L"cannot create " + job.staged;
        return;
    }
    for (const Zip::Entry& e : entries) {
        std::string rel = e.name.substr(prefix.size());
        if (rel.empty()) continue;  // the top-level folder itself
        std::wstring path = job.staged + L"\\" + Utf8ToWide(rel);
        std::replace(path.begin(), path.end(), L'/', L'\\');
        if (e.directory) path.pop_back();
        if (path.size() > 250) {
            job.error = L"\"" + Utf8ToWide(e.name) + L"\" would be too long a path once installed";
            return;
        }
        // Its folders, each made by us (the staging folder is fresh).
        for (size_t at = job.staged.size() + 1; (at = path.find(L'\\', at)) != std::wstring::npos; ++at) {
            const std::wstring dir = path.substr(0, at);
            if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
                job.error = L"cannot create " + dir;
                return;
            }
        }
        if (e.directory) {
            if (!CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
                job.error = L"cannot create " + path;
                return;
            }
            continue;
        }
        std::string bytes;
        if (!Zip::Extract(archive, e, bytes, zipError)) {
            job.error = Utf8ToWide(zipError);
            return;
        }
        if (!WriteNew(path, bytes)) {
            job.error = L"cannot write " + path;
            return;
        }
        ++job.files;
    }
    // The widget check (what --validate-widget does: the manifest, every script's syntax).
    std::vector<Issue> issues;
    std::wstring name;
    Manifest manifest;
    if (!ValidateFolder(job.staged, issues, name, &manifest)) {
        job.error = issues.empty() ? L"it is not a widget" : issues.front().Text();
        return;
    }
    std::vector<std::wstring> errors;
    for (const Issue& i : issues) {
        if (i.error) {
            errors.push_back(i.Text());
        } else {
            ++job.warnings;
        }
    }
    if (!errors.empty()) {
        job.error = L"it does not pass the widget check: " + errors.front() +
                    (errors.size() > 1 ? L" (and " + std::to_wstring(errors.size() - 1) + L" more)" : L"");
        return;
    }
    job.name = manifest.name.empty() ? job.id : manifest.name;
    job.version = manifest.version;
    job.author = manifest.author;
    job.permissionLines = PermissionLines(manifest);
    job.ok = true;
}

}  // namespace

void PrepareZipInstall(ZipInstall& job) {
    job.ok = false;
    // Windows.Data.Json (the manifest check) needs the thread in an apartment: a worker joins the
    // MTA; the UI thread (automation runs it there) is in its STA already (RPC_E_CHANGED_MODE).
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        Prepare(job);
    } catch (...) {
        job.ok = false;
        if (job.error.empty()) job.error = L"it could not be read";
    }
    if (SUCCEEDED(com)) CoUninitialize();
    if (!job.ok && !job.stagingRoot.empty()) {
        DeleteTree(job.stagingRoot);
        job.stagingRoot.clear();
        job.staged.clear();
    }
    job.done = true;
}

bool InstalledFolderExists(const std::wstring& id) {
    return ValidId(id) && GetFileAttributesW((Directory() + L"\\" + id).c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool CommitZipInstall(ZipInstall& job, bool replace, HWND owner, std::wstring& error) {
    if (!job.ok || job.staged.empty() || !ValidId(job.id)) {
        error = L"nothing to install";
        return false;
    }
    CreateDirectoryW(Config::Directory().c_str(), nullptr);
    CreateDirectoryW(Directory().c_str(), nullptr);
    const std::wstring dest = Directory() + L"\\" + job.id;
    if (GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (!replace) {
            error = L"a widget \"" + job.id + L"\" is installed already";
            DiscardZipInstall(job);
            return false;
        }
        if (!RecyclePaths({dest}, owner, error) || GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (error.empty()) error = L"the installed " + job.id + L" could not be moved to the Recycle Bin";
            DiscardZipInstall(job);
            return false;
        }
    }
    if (!MoveFileExW(job.staged.c_str(), dest.c_str(), MOVEFILE_WRITE_THROUGH)) {
        error = L"cannot move it into " + dest;
        DiscardZipInstall(job);
        return false;
    }
    job.staged.clear();
    DiscardZipInstall(job);  // the now empty staging folder
    return true;
}

void DiscardZipInstall(ZipInstall& job) {
    if (!job.stagingRoot.empty()) DeleteTree(job.stagingRoot);
    job.stagingRoot.clear();
    job.staged.clear();
}

void RemoveStaleStaging() {
    const std::wstring dir = Config::Directory();
    WIN32_FIND_DATAW fd{};
    HANDLE f = FindFirstFileExW((dir + L"\\install-*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (f == INVALID_HANDLE_VALUE) return;
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const auto ticks = [](const FILETIME& t) { return (static_cast<unsigned long long>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        // Older than ten minutes: no install still uses it (one takes well under a second).
        if (ticks(now) - ticks(fd.ftCreationTime) > 10ull * 60 * 10000000) DeleteTree(dir + L"\\" + fd.cFileName);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

bool RecyclePaths(const std::vector<std::wstring>& paths, HWND owner, std::wstring& error) {
    if (paths.empty()) return true;
    winrt::com_ptr<IFileOperation> op;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(op.put())))) {
        error = L"the shell's file operation is not available";
        return false;
    }
    // FOF_ALLOWUNDO: to the Recycle Bin. FOF_WANTNUKEWARNING: where an item cannot be recycled (a
    // volume without a bin) the shell asks before deleting it for good, even with NOCONFIRMATION.
    op->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING | FOF_SILENT | FOFX_RECYCLEONDELETE);
    if (owner) op->SetOwnerWindow(owner);
    for (const std::wstring& p : paths) {
        winrt::com_ptr<IShellItem> item;
        if (FAILED(SHCreateItemFromParsingName(p.c_str(), nullptr, IID_PPV_ARGS(item.put())))) {
            error = L"cannot find " + p;
            return false;
        }
        if (FAILED(op->DeleteItem(item.get(), nullptr))) {
            error = L"cannot delete " + p;
            return false;
        }
    }
    const HRESULT hr = op->PerformOperations();
    BOOL aborted = FALSE;
    op->GetAnyOperationsAborted(&aborted);
    if (FAILED(hr) || aborted) {
        error = aborted ? L"it was cancelled" : L"it could not be moved to the Recycle Bin";
        return false;
    }
    return true;
}

}  // namespace Widgets
