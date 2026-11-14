#include "FolderListing.h"

#include <new>

#include "Crash.h"
#include "ShellFiles.h"

namespace FolderListing {

Listing Read(const std::wstring& folder, const Filter& filter, size_t max) {
    Listing out;
    out.folder = folder;
    if (folder.empty()) return out;
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileExW(ShellFiles::Join(folder, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                   nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) return out;
    out.readable = true;
    do {
        const wchar_t* n = fd.cFileName;
        if ((n[0] == L'.' && n[1] == 0) || (n[0] == L'.' && n[1] == L'.' && n[2] == 0)) continue;
        const DWORD a = fd.dwFileAttributes;
        const bool hidden = (a & FILE_ATTRIBUTE_HIDDEN) != 0, system = (a & FILE_ATTRIBUTE_SYSTEM) != 0;
        if ((hidden && system && filter.skipProtected) || (hidden && filter.skipHidden) || (system && filter.skipSystem)) continue;
        Entry e;
        e.name = n;
        e.folder = (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.size = (static_cast<ULONGLONG>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        e.modified = fd.ftLastWriteTime;
        out.entries.push_back(std::move(e));
    } while (out.entries.size() < max && FindNextFileW(find, &fd));
    FindClose(find);
    return out;
}

bool Slow(const std::wstring& folder) {
    if (folder.size() < 2) return false;
    if (folder[0] == L'\\' && folder[1] == L'\\') {
        // \\?\C:\... is a local path written long; \\?\UNC\..., \\server\share and device paths are not.
        if (folder.size() >= 7 && folder.compare(0, 4, L"\\\\?\\") == 0 && folder[5] == L':') {
            return Slow(folder.substr(4));
        }
        return true;
    }
    if (folder[1] != L':') return false;  // a relative path: as local
    const wchar_t root[4] = {folder[0], L':', L'\\', 0};
    switch (GetDriveTypeW(root)) {
        case DRIVE_FIXED:
        case DRIVE_RAMDISK:
            return false;
        default:  // DRIVE_REMOTE, DRIVE_REMOVABLE, DRIVE_CDROM, DRIVE_UNKNOWN, DRIVE_NO_ROOT_DIR
            return true;
    }
}

namespace {

struct Job {
    HWND hwnd = nullptr;
    UINT msg = 0;
    WPARAM tag = 0;
    std::wstring folder;
    Filter filter;
    size_t max = 0;
};

void JobBody(void* param) {
    std::unique_ptr<Job> job(static_cast<Job*>(param));
    auto* listing = new (std::nothrow) Listing;
    if (!listing) return;
    try {
        *listing = Read(job->folder, job->filter, job->max);
    } catch (...) {  // std::bad_alloc: an unreadable folder then
        listing->entries.clear();
        listing->readable = false;
        listing->folder = job->folder;
    }
    if (!PostMessageW(job->hwnd, job->msg, job->tag, reinterpret_cast<LPARAM>(listing))) delete listing;
}

// The body catches what the listing throws; Crash::Job is the backstop and the thread's terminate
// handler (Crash.h), as for the weather and calendar fetches.
DWORD WINAPI JobThread(LPVOID param) {
    Crash::Job(L"folder listing", [&] { JobBody(param); }, [] {});
    return 0;
}

}  // namespace

bool ReadAsync(HWND hwnd, UINT msg, WPARAM tag, const std::wstring& folder, const Filter& filter, size_t max) {
    if (!hwnd) return false;
    auto* job = new (std::nothrow) Job{hwnd, msg, tag, folder, filter, max};
    if (!job) return false;
    HANDLE thread = CreateThread(nullptr, 64 * 1024, &JobThread, job, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!thread) {
        delete job;
        return false;
    }
    CloseHandle(thread);  // it ends by itself after posting
    return true;
}

}  // namespace FolderListing
