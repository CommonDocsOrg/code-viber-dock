#include "ShellFiles.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>
#include <iterator>

namespace ShellFiles {

namespace {

LONG g_running = 0;

struct Pidl {
    PIDLIST_ABSOLUTE p = nullptr;
    ~Pidl() {
        if (p) CoTaskMemFree(p);
    }
};

DWORD WINAPI JobThread(void* param) {
    Job* job = static_cast<Job*>(param);
    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        winrt::com_ptr<IFileOperation> op;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(op.put())))) {
            DWORD flags = FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR;
            if (job->op == Op::Recycle) flags |= FOFX_RECYCLEONDELETE | FOFX_ADDUNDORECORD;
            if (job->renameOnCollision) flags |= FOF_RENAMEONCOLLISION;
            op->SetOperationFlags(flags);
            if (job->owner && IsWindow(job->owner)) op->SetOwnerWindow(job->owner);
            winrt::com_ptr<IShellItem> dest;
            const bool needsDest = job->op == Op::Copy || job->op == Op::Move;
            if (!needsDest || SUCCEEDED(SHCreateItemFromParsingName(job->dest.c_str(), nullptr, IID_PPV_ARGS(dest.put())))) {
                size_t queued = 0;
                for (const std::wstring& f : job->files) {
                    winrt::com_ptr<IShellItem> item;
                    if (FAILED(SHCreateItemFromParsingName(f.c_str(), nullptr, IID_PPV_ARGS(item.put())))) continue;
                    HRESULT hr = E_FAIL;
                    switch (job->op) {
                        case Op::Copy: hr = op->CopyItem(item.get(), dest.get(), nullptr, nullptr); break;
                        case Op::Move: hr = op->MoveItem(item.get(), dest.get(), nullptr, nullptr); break;
                        case Op::Recycle: hr = op->DeleteItem(item.get(), nullptr); break;
                        case Op::Rename: hr = op->RenameItem(item.get(), job->dest.c_str(), nullptr); break;
                    }
                    if (SUCCEEDED(hr)) ++queued;
                }
                if (queued) op->PerformOperations();
            }
        }
        op = nullptr;
        CoUninitialize();
    }
    if (job->notify && job->message && IsWindow(job->notify)) PostMessageW(job->notify, job->message, job->tag, 0);
    delete job;
    InterlockedDecrement(&g_running);
    return 0;
}

}  // namespace

std::wstring Join(const std::wstring& dir, const std::wstring& name) {
    if (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) return dir + name;
    return dir + L'\\' + name;
}

std::wstring Normalize(std::wstring path) {
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    if (path.size() == 2 && path[1] == L':') path += L'\\';
    return path;
}

std::wstring Parent(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    return Normalize(path.substr(0, slash == 2 && path[1] == L':' ? 3 : slash));
}

std::wstring LastComponent(const std::wstring& path) {
    const std::wstring p = Normalize(path);
    if (p.size() == 3 && p[1] == L':') return p.substr(0, 2);
    const size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? p : p.substr(slash + 1);
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool IsInside(const std::wstring& path, const std::wstring& dir) {
    if (SamePath(path, dir)) return true;
    const std::wstring prefix = dir.empty() || dir.back() == L'\\' ? dir : dir + L'\\';
    return path.size() > prefix.size() &&
           CompareStringOrdinal(path.c_str(), static_cast<int>(prefix.size()), prefix.c_str(),
                                static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL;
}

std::wstring VolumeOf(const std::wstring& path) {
    wchar_t volume[MAX_PATH]{};
    return GetVolumePathNameW(path.c_str(), volume, MAX_PATH) ? std::wstring(volume) : std::wstring();
}

std::wstring Extension(const std::wstring& name) {
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) return {};
    return name.substr(dot);
}

std::wstring Stem(const std::wstring& name) { return name.substr(0, name.size() - Extension(name).size()); }

std::wstring FreeName(const std::wstring& dir, const std::wstring& name) {
    if (GetFileAttributesW(Join(dir, name).c_str()) == INVALID_FILE_ATTRIBUTES) return name;
    const std::wstring stem = Stem(name), ext = Extension(name);
    for (int n = 2; n < 10000; ++n) {
        std::wstring candidate = stem + L" (" + std::to_wstring(n) + L")" + ext;
        if (GetFileAttributesW(Join(dir, candidate).c_str()) == INVALID_FILE_ATTRIBUTES) return candidate;
    }
    return name;
}

std::wstring SizeAndDate(bool folder, ULONGLONG size, const FILETIME& modified) {
    std::wstring text;
    if (!folder) {
        wchar_t buf[64]{};
        if (SUCCEEDED(StrFormatByteSizeEx(size, SFBS_FLAGS_ROUND_TO_NEAREST_DISPLAYED_DIGIT, buf,
                                          static_cast<UINT>(std::size(buf))))) {
            text = buf;
        }
    }
    FILETIME local{};
    SYSTEMTIME st{};
    if ((modified.dwHighDateTime || modified.dwLowDateTime) && FileTimeToLocalFileTime(&modified, &local) &&
        FileTimeToSystemTime(&local, &st)) {
        wchar_t date[64]{};
        if (GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &st, nullptr, date, static_cast<int>(std::size(date)),
                            nullptr) > 0) {
            if (!text.empty()) text += L"  ·  ";
            text += date;
        }
    }
    return text;
}

std::vector<std::wstring> FilesOf(IDataObject* data) {
    std::vector<std::wstring> files;
    if (!data) return files;
    FORMATETC fmt{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    if (FAILED(data->GetData(&fmt, &medium))) return files;
    if (HDROP drop = static_cast<HDROP>(GlobalLock(medium.hGlobal))) {
        const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            const UINT len = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring path(len, L'\0');
            if (len && DragQueryFileW(drop, i, path.data(), len + 1)) files.push_back(std::move(path));
        }
        GlobalUnlock(medium.hGlobal);
    }
    ReleaseStgMedium(&medium);
    return files;
}

DWORD DropEffect(const std::vector<std::wstring>& files, const std::wstring& dest, DWORD keys, DWORD allowed) {
    if (files.empty() || dest.empty()) return DROPEFFECT_NONE;
    // A folder into itself (or below itself).
    if (std::any_of(files.begin(), files.end(), [&](const std::wstring& f) { return IsInside(dest, f); })) {
        return DROPEFFECT_NONE;
    }
    DWORD effect = 0;
    if (keys & MK_CONTROL) {
        effect = DROPEFFECT_COPY;
    } else if (keys & MK_SHIFT) {
        effect = DROPEFFECT_MOVE;
    } else {
        const std::wstring volume = VolumeOf(dest);
        const bool same = !volume.empty() && std::all_of(files.begin(), files.end(), [&](const std::wstring& f) {
            return SamePath(VolumeOf(f), volume);
        });
        effect = same ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
    }
    if (effect == DROPEFFECT_MOVE &&
        std::all_of(files.begin(), files.end(), [&](const std::wstring& f) { return SamePath(Parent(f), dest); })) {
        return DROPEFFECT_NONE;  // already there
    }
    if (!(allowed & effect)) effect = (allowed & DROPEFFECT_COPY) ? DROPEFFECT_COPY : (allowed & DROPEFFECT_MOVE);
    return effect;
}

winrt::com_ptr<IDataObject> DataObjectFor(const std::vector<std::wstring>& paths) {
    winrt::com_ptr<IDataObject> data;
    if (paths.empty()) return data;
    const std::wstring folder = Parent(paths.front());
    Pidl parent;
    if (FAILED(SHParseDisplayName(folder.c_str(), nullptr, &parent.p, 0, nullptr)) || !parent.p) return data;
    std::vector<PIDLIST_ABSOLUTE> absolute;
    std::vector<PCUITEMID_CHILD> children;
    for (const std::wstring& p : paths) {
        if (!SamePath(Parent(p), folder)) continue;  // one folder only
        PIDLIST_ABSOLUTE a = nullptr;
        if (SUCCEEDED(SHParseDisplayName(p.c_str(), nullptr, &a, 0, nullptr)) && a) {
            absolute.push_back(a);
            children.push_back(ILFindLastID(a));
        }
    }
    if (!children.empty()) {
        SHCreateDataObject(parent.p, static_cast<UINT>(children.size()), children.data(), nullptr, IID_PPV_ARGS(data.put()));
    }
    for (PIDLIST_ABSOLUTE a : absolute) CoTaskMemFree(a);
    return data;
}

bool Start(Job job) {
    Job* heap = new Job(std::move(job));
    InterlockedIncrement(&g_running);
    if (HANDLE thread = CreateThread(nullptr, 0, &JobThread, heap, 0, nullptr)) {
        CloseHandle(thread);
        return true;
    }
    InterlockedDecrement(&g_running);
    delete heap;
    return false;
}

LONG Running() { return InterlockedCompareExchange(&g_running, 0, 0); }

// --- DropTarget ---

HRESULT STDMETHODCALLTYPE DropTarget::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDropTarget)) {
        *ppv = static_cast<IDropTarget*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE DropTarget::AddRef() { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }

ULONG STDMETHODCALLTYPE DropTarget::Release() {
    const LONG n = InterlockedDecrement(&refs_);
    if (n == 0) delete this;
    return static_cast<ULONG>(n);
}

HRESULT STDMETHODCALLTYPE DropTarget::DragEnter(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) {
    if (!effect) return E_INVALIDARG;
    *effect = sink_.enter ? sink_.enter(data, POINT{pt.x, pt.y}, keys, *effect) : DROPEFFECT_NONE;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DropTarget::DragOver(DWORD keys, POINTL pt, DWORD* effect) {
    if (!effect) return E_INVALIDARG;
    *effect = sink_.over ? sink_.over(POINT{pt.x, pt.y}, keys, *effect) : DROPEFFECT_NONE;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DropTarget::DragLeave() {
    if (sink_.leave) sink_.leave();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DropTarget::Drop(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) {
    if (!effect) return E_INVALIDARG;
    *effect = sink_.drop ? sink_.drop(data, POINT{pt.x, pt.y}, keys, *effect) : DROPEFFECT_NONE;
    return S_OK;
}

}  // namespace ShellFiles
