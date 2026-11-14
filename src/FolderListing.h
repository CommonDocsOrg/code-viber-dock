#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

// Round 26 phase 26B: a folder's entries, read for the folder popups and the desktop folder windows
// (FindFirstFileExW, the same filters and cap they used each on their own). Read runs on the
// calling thread; ReadAsync runs it on a short-lived worker thread (Crash::Job) and posts the
// Listing to a window, for folders that can block (Slow): before this both views enumerated on the
// UI thread, which an offline share or a sleeping NAS held for the network's timeout (a folder
// window during startup's Sync, before the first frame).
namespace FolderListing {

struct Entry {
    std::wstring name;  // file name (sorting)
    std::wstring path;  // filled by the view (the folder joined with the name)
    bool folder = false;
    ULONGLONG size = 0;
    FILETIME modified{};
};

// What to leave out. Always "." and ".."; a hidden and system file (desktop.ini) whenever
// skipProtected; any hidden one with skipHidden; any system one with skipSystem.
struct Filter {
    bool skipHidden = true;
    bool skipSystem = true;
    bool skipProtected = true;
};

struct Listing {
    std::wstring folder;  // as asked
    bool readable = false;
    std::vector<Entry> entries;  // in the order FindNextFile gave them (the views sort)
};

// At most `max` entries (a pathological folder).
Listing Read(const std::wstring& folder, const Filter& filter, size_t max);

// The folder may block on I/O for long: a UNC path (\\server\share, \\?\UNC\...), a drive that is
// remote (a mapped share), removable, optical or unknown. GetDriveTypeW answers from the mount
// manager without touching the device (a mapped drive's server is not contacted).
bool Slow(const std::wstring& folder);

// Reads on a worker thread and posts `msg` to `hwnd` with wParam = `tag` and lParam = a Listing*
// that the receiver owns (delete it; a window that is gone cannot, so the worker frees it when the
// post fails). False when the thread could not start (the caller reads synchronously instead).
bool ReadAsync(HWND hwnd, UINT msg, WPARAM tag, const std::wstring& folder, const Filter& filter, size_t max);

}  // namespace FolderListing
