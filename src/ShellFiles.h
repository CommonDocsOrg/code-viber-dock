#pragma once

#include <windows.h>
#include <oleidl.h>
#include <shobjidl.h>

#include <winrt/base.h>

#include <functional>
#include <string>
#include <vector>

// File-system helpers shared by the folder popup (round 4 phase 7) and the desktop folder windows
// (phase 12): paths, CF_HDROP data, Explorer's drop effect, and file operations (IFileOperation on
// a short-lived STA worker thread, so the shell's progress / conflict / confirmation UI never
// blocks the dock's thread). Moves and copies are undoable (FOF_ALLOWUNDO); deletes go to the
// Recycle Bin.
namespace ShellFiles {

// --- paths ---
std::wstring Join(const std::wstring& dir, const std::wstring& name);
// "C:\Users\me\" -> "C:\Users\me"; a drive root keeps its backslash ("C:\").
std::wstring Normalize(std::wstring path);
std::wstring Parent(const std::wstring& path);
// The last component ("C:" for a drive root).
std::wstring LastComponent(const std::wstring& path);
bool SamePath(const std::wstring& a, const std::wstring& b);
// `path` is `dir` or inside it.
bool IsInside(const std::wstring& path, const std::wstring& dir);
std::wstring VolumeOf(const std::wstring& path);
// The name without its extension ("report.txt" -> "report"; "a.b.c" -> "a.b"; ".gitignore" stays).
std::wstring Stem(const std::wstring& name);
// The extension with its dot ("" for none).
std::wstring Extension(const std::wstring& name);
// A name not taken in `dir` (nor in `reserved`, compared case-insensitively): `name`, else
// "stem (2).ext", "stem (3).ext" ...
std::wstring FreeName(const std::wstring& dir, const std::wstring& name, const std::vector<std::wstring>& reserved = {});
// "12.3 KB  ·  02/11/2026" (folders: the date only).
std::wstring SizeAndDate(bool folder, ULONGLONG size, const FILETIME& modified);

// --- drags and the clipboard ---
// The files of a CF_HDROP data object.
std::vector<std::wstring> FilesOf(IDataObject* data);
// Explorer's drop effect for `files` dropped into `dest` with `keys` (MK_*) held: Ctrl copies,
// Shift moves, otherwise a move on the same volume and a copy across volumes. NONE when nothing
// would happen (a folder into itself, a move into the folder the files are in).
DWORD DropEffect(const std::vector<std::wstring>& files, const std::wstring& dest, DWORD keys, DWORD allowed);
// A shell data object for these paths (all in one folder; SHCreateDataObject: CF_HDROP, shell ID
// lists, file contents ... so Explorer and other apps accept it). Null on failure.
winrt::com_ptr<IDataObject> DataObjectFor(const std::vector<std::wstring>& paths);

// --- file operations ---
enum class Op { Copy, Move, Recycle, Rename };
struct Job {
    Op op = Op::Copy;
    std::vector<std::wstring> files;
    std::wstring dest;       // Copy / Move: the target folder; Rename: the new name
    bool renameOnCollision = false;  // Copy / Move: the shell's "name - Copy" instead of the conflict dialog
    // Copy / Move: the name each file gets in `dest` (parallel to `files`; empty: its own name).
    std::vector<std::wstring> newNames;
    HWND notify = nullptr;   // gets `message` (wParam: the job's tag) when the operation is done
    UINT message = 0;
    WPARAM tag = 0;
    HWND owner = nullptr;    // the shell's UI is owned by this window
};
// Runs the job on its own STA thread. False if the thread could not be started.
bool Start(Job job);
// Jobs started and not yet finished (process wide).
LONG Running();

// An OLE drop target that forwards to callbacks (on the UI thread). Created with one reference;
// Detach() before releasing it when the owner goes.
class DropTarget final : public IDropTarget {
public:
    struct Sink {
        std::function<DWORD(IDataObject* data, POINT screen, DWORD keys, DWORD allowed)> enter;
        std::function<DWORD(POINT screen, DWORD keys, DWORD allowed)> over;
        std::function<void()> leave;
        std::function<DWORD(IDataObject* data, POINT screen, DWORD keys, DWORD allowed)> drop;
    };
    explicit DropTarget(Sink sink) : sink_(std::move(sink)) {}
    void Detach() { sink_ = Sink{}; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override;
    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL pt, DWORD* effect) override;
    HRESULT STDMETHODCALLTYPE DragLeave() override;
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override;

private:
    ~DropTarget() = default;
    Sink sink_;
    LONG refs_ = 1;
};

}  // namespace ShellFiles
