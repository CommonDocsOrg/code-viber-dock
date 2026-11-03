#include "FolderPopup.h"

#include <commctrl.h>
#include <commoncontrols.h>
#include <oleidl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include "Theme.h"

namespace {

// Layout in DIPs.
constexpr float kPad = 8.0f;           // popup edge -> header / grid
constexpr float kHeaderH = 32.0f;
constexpr float kHeaderGap = 6.0f;     // header -> grid (the hairline sits in it)
constexpr float kBackSize = 28.0f;     // back glyph hit box
constexpr float kButtonH = 24.0f;      // "Open in Explorer"
constexpr float kButtonPadX = 10.0f;
constexpr float kCellW = 88.0f;
constexpr float kCellH = 84.0f;
constexpr float kListRowH = 36.0f;
constexpr float kListWidth = 340.0f;
constexpr float kMessageH = 56.0f;     // "This folder is empty"
constexpr float kMinWidth = 260.0f;
constexpr float kMinTitleW = 96.0f;    // the header keeps at least this much room for the name
constexpr float kCellRadius = 6.0f;
constexpr float kTitleFont = 14.0f;
constexpr float kButtonFont = 12.0f;
constexpr float kMonitorMargin = 8.0f;
constexpr size_t kMaxColumns = 6;
constexpr size_t kMaxGridRows = 8;     // visible; more scroll (6 x 8 = the round-3 maximum of 48)
constexpr size_t kMaxListRows = 14;
constexpr size_t kMaxEntries = 20000;  // enumeration cap (a pathological folder)
constexpr double kScrollSeconds = 0.16;
constexpr wchar_t kExplorerText[] = L"Open in Explorer";
constexpr wchar_t kPathSeparator[] = L" \u203A ";  // " › "

LONG Px(float dip, float scale) { return static_cast<LONG>(std::lround(dip * scale)); }

D2D1_RECT_F ToRectF(const RECT& r) {
    return D2D1::RectF(static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right),
                       static_cast<float>(r.bottom));
}

bool Empty(const RECT& r) { return r.right <= r.left || r.bottom <= r.top; }

std::wstring Join(const std::wstring& dir, const std::wstring& name) {
    if (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) return dir + name;
    return dir + L'\\' + name;
}

// "C:\Users\me\" -> "C:\Users\me"; a drive root keeps its backslash ("C:\").
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

// `path` is `dir` or inside it.
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

// "12.3 KB · 02/11/2026" (folders: the date only).
std::wstring Secondary(bool folder, ULONGLONG size, const FILETIME& modified) {
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
            if (!text.empty()) text += L"  \u00B7  ";
            text += date;
        }
    }
    return text;
}

// The files of a CF_HDROP drag.
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

// A move / copy of dropped files, run by IFileOperation on its own STA thread (it may show the
// shell's progress / conflict UI); the popup is told when it is done.
struct FileJob {
    std::vector<std::wstring> files;
    std::wstring dest;
    bool move = false;
    HWND notify = nullptr;
};

DWORD WINAPI FileJobThread(void* param) {
    FileJob* job = static_cast<FileJob*>(param);
    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        winrt::com_ptr<IFileOperation> op;
        winrt::com_ptr<IShellItem> dest;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(op.put()))) &&
            SUCCEEDED(SHCreateItemFromParsingName(job->dest.c_str(), nullptr, IID_PPV_ARGS(dest.put())))) {
            op->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR);
            size_t queued = 0;
            for (const std::wstring& f : job->files) {
                winrt::com_ptr<IShellItem> item;
                if (FAILED(SHCreateItemFromParsingName(f.c_str(), nullptr, IID_PPV_ARGS(item.put())))) continue;
                const HRESULT hr = job->move ? op->MoveItem(item.get(), dest.get(), nullptr, nullptr)
                                             : op->CopyItem(item.get(), dest.get(), nullptr, nullptr);
                if (SUCCEEDED(hr)) ++queued;
            }
            if (queued) op->PerformOperations();
        }
        op = nullptr;
        dest = nullptr;
        CoUninitialize();
    }
    if (job->notify && IsWindow(job->notify)) PostMessageW(job->notify, FolderPopup::kDropDoneMessage, 0, 0);
    delete job;
    return 0;
}

}  // namespace

// OLE drop target of the popup window (calls on the UI thread).
class FolderDropTarget final : public IDropTarget {
public:
    explicit FolderDropTarget(FolderPopup* popup) : popup_(popup) {}
    void Detach() { popup_ = nullptr; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDropTarget)) {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG n = InterlockedDecrement(&refs_);
        if (n == 0) delete this;
        return static_cast<ULONG>(n);
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override {
        if (!effect) return E_INVALIDARG;
        *effect = popup_ ? popup_->DragEnter(data, POINT{pt.x, pt.y}, keys, *effect) : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL pt, DWORD* effect) override {
        if (!effect) return E_INVALIDARG;
        *effect = popup_ ? popup_->DragOver(POINT{pt.x, pt.y}, keys, *effect) : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        if (popup_) popup_->DragLeave();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override {
        if (!effect) return E_INVALIDARG;
        *effect = popup_ ? popup_->Drop(data, POINT{pt.x, pt.y}, keys, *effect) : DROPEFFECT_NONE;
        return S_OK;
    }

private:
    ~FolderDropTarget() = default;
    FolderPopup* popup_;
    LONG refs_ = 1;
};

void FolderPopup::Init(HINSTANCE instance, Renderer* renderer, IconCache* icons, Callbacks callbacks) {
    PopupWindow::Init(instance, renderer);
    icons_ = icons;
    callbacks_ = std::move(callbacks);
    SetClosedCallback(callbacks_.closed);
}

void FolderPopup::Release() {
    view_.Detach();
    view_.SetItems({});
    entries_.clear();
    entries_.shrink_to_fit();
    iconByIndex_.clear();
    members_.clear();
    group_ = false;
    groupCallbacks_ = GroupCallbacks{};
    root_.clear();
    rootName_.clear();
    current_.clear();
    readable_ = true;
    dragFiles_.clear();
    selectAfterLoad_.clear();
}

void FolderPopup::OnHidden() { Release(); }

void FolderPopup::OnDestroying() {
    if (dropTarget_) {
        if (Window()) RevokeDragDrop(Window());
        dropTarget_->Detach();
        dropTarget_->Release();
        dropTarget_ = nullptr;
    }
    Release();
    viewReady_ = false;
    titleFormat_ = nullptr;
    buttonFormat_ = nullptr;
    titleEllipsis_ = nullptr;
    formatScale_ = 0.0f;
}

bool FolderPopup::Show(const std::wstring& folder, const std::wstring& name, ItemView view, const Style& style,
                       const PopupAnchor& anchor) {
    if (folder.empty()) return false;
    style_ = style;
    frame_ = Frame{};
    frame_.scale = style.scale;
    frame_.acrylic = style.acrylic;
    frame_.cornerRadiusDip = style.cornerRadiusDip;
    frame_.monitor = style.monitor;
    anchor_ = anchor;
    viewSetting_ = view;
    group_ = false;
    members_.clear();
    groupCallbacks_ = GroupCallbacks{};
    root_ = Normalize(folder);
    rootName_ = name.empty() ? LastComponent(root_) : name;
    Load(root_);
    if (!ShowAt(frame_, anchor_)) {
        if (!OnScreen()) Release();
        return false;
    }
    return true;
}

bool FolderPopup::ShowGroup(const std::wstring& name, std::vector<GroupEntry> members, ItemView view,
                            const Style& style, const PopupAnchor& anchor, GroupCallbacks callbacks) {
    style_ = style;
    frame_ = Frame{};
    frame_.scale = style.scale;
    frame_.acrylic = style.acrylic;
    frame_.cornerRadiusDip = style.cornerRadiusDip;
    frame_.monitor = style.monitor;
    anchor_ = anchor;
    viewSetting_ = view;
    group_ = true;
    groupCallbacks_ = std::move(callbacks);
    entries_.clear();
    root_.clear();
    current_.clear();
    rootName_ = name.empty() ? std::wstring(L"Group") : name;
    members_ = std::move(members);
    LoadGroup();
    if (!ShowAt(frame_, anchor_)) {
        if (!OnScreen()) Release();
        return false;
    }
    return true;
}

void FolderPopup::UpdateGroup(const std::wstring& name, std::vector<GroupEntry> members) {
    if (!OnScreen() || !group_) return;
    rootName_ = name.empty() ? std::wstring(L"Group") : name;
    members_ = std::move(members);
    const LONG scroll = view_.ScrollOffset();
    LoadGroup();
    if (ShowAt(frame_, anchor_)) view_.ScrollTo(static_cast<float>(scroll), false);
}

void FolderPopup::LoadGroup() {
    readable_ = true;
    ClearHot();
    std::vector<GridItem> items;
    items.reserve(members_.size());
    for (size_t i = 0; i < members_.size(); ++i) {
        GridItem item;
        item.label = members_[i].label;
        item.icon = members_[i].icon;
        item.id = i;
        item.resolved = true;
        items.push_back(std::move(item));
    }
    const bool list = viewSetting_ == ItemView::List || (viewSetting_ == ItemView::Auto && members_.size() > kAutoGridMax);
    view_.SetItems(std::move(items));
    view_.SetMode(list ? ItemGridView::Mode::List : ItemGridView::Mode::Grid);
    view_.SetDragOut(groupCallbacks_.drag != nullptr);
    pendingSelect_ = -1;
}

void FolderPopup::Load(const std::wstring& folder) {
    current_ = folder;
    entries_.clear();
    iconByIndex_.clear();
    readable_ = true;
    ClearHot();

    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileExW(Join(folder, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                   FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) {
        readable_ = false;
    } else {
        do {
            const wchar_t* n = fd.cFileName;
            if ((n[0] == L'.' && n[1] == 0) || (n[0] == L'.' && n[1] == L'.' && n[2] == 0)) continue;
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
            Entry e;
            e.name = n;
            e.folder = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            e.size = (static_cast<ULONGLONG>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            e.modified = fd.ftLastWriteTime;
            entries_.push_back(std::move(e));
        } while (entries_.size() < kMaxEntries && FindNextFileW(find, &fd));
        FindClose(find);
    }
    // Folders first, then Explorer's logical order ("file2" before "file10").
    std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
        if (a.folder != b.folder) return a.folder;
        return StrCmpLogicalW(a.name.c_str(), b.name.c_str()) < 0;
    });
    const bool list = viewSetting_ == ItemView::List || (viewSetting_ == ItemView::Auto && entries_.size() > kAutoGridMax);
    std::vector<GridItem> items;
    items.reserve(entries_.size());
    for (size_t i = 0; i < entries_.size(); ++i) {
        Entry& e = entries_[i];
        e.path = Join(folder, e.name);
        GridItem item;
        item.label = e.name;  // the shell's display name replaces it when the row is drawn
        item.id = i;
        item.flags = e.folder ? 1u : 0u;
        if (list) item.secondary = Secondary(e.folder, e.size, e.modified);
        items.push_back(std::move(item));
    }
    view_.SetItems(std::move(items));
    view_.SetMode(list ? ItemGridView::Mode::List : ItemGridView::Mode::Grid);
    view_.SetDragOut(false);  // a drag on an entry scrolls
    pendingSelect_ = -1;
    if (!selectAfterLoad_.empty()) {
        // Back from a sub-folder: it is selected and scrolled into view (Measure).
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (SamePath(entries_[i].path, selectAfterLoad_)) pendingSelect_ = static_cast<int>(i);
        }
        selectAfterLoad_.clear();
    }
}

void FolderPopup::ResolveItem(size_t index, GridItem& item) {
    if (index >= entries_.size()) return;
    const Entry& e = entries_[index];
    SHFILEINFOW sfi{};
    if (!SHGetFileInfoW(e.path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_DISPLAYNAME)) return;
    if (sfi.szDisplayName[0]) item.label = sfi.szDisplayName;
    auto it = iconByIndex_.find(sfi.iIcon);
    if (it == iconByIndex_.end()) {
        winrt::com_ptr<ID2D1Bitmap> bitmap;
        winrt::com_ptr<IImageList> images;
        ID2D1DeviceContext* dc = Render() ? Render()->ResourceContext() : nullptr;
        if (dc && icons_ && SUCCEEDED(SHGetImageList(SHIL_EXTRALARGE, IID_PPV_ARGS(images.put())))) {
            HICON icon = nullptr;
            if (SUCCEEDED(images->GetIcon(sfi.iIcon, ILD_TRANSPARENT, &icon)) && icon) {
                bitmap = icons_->FromHicon(icon, dc);
                DestroyIcon(icon);
            }
        }
        it = iconByIndex_.emplace(sfi.iIcon, std::move(bitmap)).first;
    }
    item.icon = it->second;
}

void FolderPopup::Navigate(const std::wstring& folder) {
    if (folder.empty()) return;
    Load(folder);
    if (!ShowAt(frame_, anchor_) && callbacks_.dismiss) callbacks_.dismiss();
}

void FolderPopup::GoBack() {
    if (current_.empty() || SamePath(current_, root_)) return;
    const std::wstring parent = Parent(current_);
    selectAfterLoad_ = current_;
    Navigate(parent.empty() || !IsInside(parent, root_) ? root_ : parent);
}

std::wstring FolderPopup::Title(float maxWidth) const {
    if (SamePath(current_, root_) || !IsInside(current_, root_)) return SamePath(current_, root_) ? rootName_ : LastComponent(current_);
    // The pinned folder's name, then the path below it.
    std::vector<std::wstring> parts{rootName_};
    std::wstring rest = current_.substr(std::min(current_.size(), root_.size()));
    size_t start = 0;
    while (start < rest.size()) {
        const size_t slash = rest.find_first_of(L"\\/", start);
        const std::wstring part = rest.substr(start, slash == std::wstring::npos ? std::wstring::npos : slash - start);
        if (!part.empty()) parts.push_back(part);
        if (slash == std::wstring::npos) break;
        start = slash + 1;
    }
    IDWriteFactory* dw = Render() ? Render()->DWrite() : nullptr;
    auto build = [&](size_t from) {
        std::wstring text = from > 0 ? std::wstring(L"\u2026") + kPathSeparator : std::wstring();
        for (size_t i = from; i < parts.size(); ++i) {
            if (i > from) text += kPathSeparator;
            text += parts[i];
        }
        return text;
    };
    // Leading parts give way to "…" until the rest fits (the last one is ellipsized if it must).
    for (size_t from = 0; from + 1 < parts.size(); ++from) {
        const std::wstring text = build(from);
        winrt::com_ptr<IDWriteTextLayout> layout;
        DWRITE_TEXT_METRICS tm{};
        if (!dw || !titleFormat_ ||
            FAILED(dw->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), titleFormat_.get(), 10000.0f,
                                        100.0f, layout.put())) ||
            FAILED(layout->GetMetrics(&tm)) || tm.widthIncludingTrailingWhitespace <= maxWidth) {
            return text;
        }
    }
    return build(parts.size() - 1);
}

void FolderPopup::Open(std::wstring path, bool explorer) {
    // First: hiding may release the entries (and `path` may have come from one).
    if (callbacks_.dismiss) callbacks_.dismiss();
    std::wstring dir = explorer ? std::wstring{} : Parent(path);
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    // No error UI (a message box would block the dock's thread).
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = explorer ? L"open" : nullptr;
    sei.lpFile = path.c_str();
    sei.lpDirectory = dir.empty() ? nullptr : dir.c_str();
    sei.nShow = SW_SHOWNORMAL;
    AllowSetForegroundWindow(ASFW_ANY);  // the click was ours: let what starts take the foreground
    if (ShellExecuteExW(&sei)) return;
    if (!explorer && GetLastError() == ERROR_NO_ASSOCIATION) {
        sei.lpVerb = L"openas";  // "How do you want to open this file?" (a separate process)
        if (ShellExecuteExW(&sei)) return;
    }
    MessageBeep(MB_ICONWARNING);
}

void FolderPopup::Activate(size_t index, ItemGridView::Button button) {
    if (group_) {
        // The owner opens / activates the member (and dismisses the popup).
        if (button == ItemGridView::Button::Left && index < members_.size() && groupCallbacks_.open) {
            groupCallbacks_.open(index);
        } else if (button == ItemGridView::Button::Middle && index < members_.size() && groupCallbacks_.middle) {
            groupCallbacks_.middle(index);
        }
        return;
    }
    if (index >= entries_.size()) return;
    const Entry e = entries_[index];  // a copy: Navigate replaces the entries
    if (e.folder && button == ItemGridView::Button::Left) {
        Navigate(e.path);
        return;
    }
    // A file (left or middle click), or a sub-folder with the middle button (Explorer).
    Open(e.path, e.folder);
}

bool FolderPopup::MemberScreenRect(size_t index, RECT& screen) const {
    if (!OnScreen() || index >= view_.Count()) return false;
    const RECT r = view_.ItemRect(index);
    const RECT b = view_.Bounds();
    if (r.bottom <= b.top || r.top >= b.bottom) return false;
    const RECT win = ScreenRect();
    screen = RECT{win.left + r.left, win.top + std::max(r.top, b.top), win.left + r.right, win.top + std::min(r.bottom, b.bottom)};
    return true;
}

void FolderPopup::ContextMenu(int index, POINT screen) {
    if (group_ && groupCallbacks_.menu) {
        if (index >= 0 && static_cast<size_t>(index) < members_.size()) groupCallbacks_.menu(static_cast<size_t>(index), screen);
        return;
    }
    if (group_) {
        if (index < 0 || static_cast<size_t>(index) >= members_.size() || !groupCallbacks_.command) return;
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        AppendMenuW(menu, MF_STRING, kMemberOpen, L"Open");
        AppendMenuW(menu, MF_STRING, kMemberRemove, L"Remove from group");
        AppendMenuW(menu, MF_STRING, kMemberToDock, L"Move to dock");
        SetMenuDefaultItem(menu, kMemberOpen, FALSE);
        const UINT cmd = static_cast<UINT>(TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                                            screen.x, screen.y, Window(), nullptr));
        DestroyMenu(menu);
        if (cmd != 0 && OnScreen() && group_ && groupCallbacks_.command) groupCallbacks_.command(static_cast<size_t>(index), cmd);
        return;
    }
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    constexpr UINT kOpen = 1, kExplorer = 2, kShowIn = 3;
    const bool entry = index >= 0 && static_cast<size_t>(index) < entries_.size();
    if (entry) {
        AppendMenuW(menu, MF_STRING, kOpen, L"Open");
        if (entries_[static_cast<size_t>(index)].folder) {
            AppendMenuW(menu, MF_STRING, kExplorer, L"Open in Explorer");
        } else {
            AppendMenuW(menu, MF_STRING, kShowIn, L"Show in Explorer");
        }
        SetMenuDefaultItem(menu, kOpen, FALSE);
    } else {
        AppendMenuW(menu, MF_STRING, kExplorer, kExplorerText);
    }
    const UINT cmd = static_cast<UINT>(TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                                        screen.x, screen.y, Window(), nullptr));
    DestroyMenu(menu);
    if (!OnScreen()) return;
    const std::wstring path = entry ? entries_[static_cast<size_t>(index)].path : current_;
    switch (cmd) {
        case kOpen:
            Activate(static_cast<size_t>(index), ItemGridView::Button::Left);
            break;
        case kExplorer:
            Open(path, true);
            break;
        case kShowIn: {
            if (callbacks_.dismiss) callbacks_.dismiss();
            const std::wstring args = L"/select,\"" + path + L"\"";
            AllowSetForegroundWindow(ASFW_ANY);
            ShellExecuteW(nullptr, nullptr, L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
            break;
        }
        default:
            break;
    }
}

void FolderPopup::EnsureFormats() {
    IDWriteFactory* dw = Render() ? Render()->DWrite() : nullptr;
    const float s = style_.scale;
    if (!dw || (titleFormat_ && formatScale_ == s)) return;
    titleFormat_ = buttonFormat_ = nullptr;
    titleEllipsis_ = nullptr;
    const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    if (SUCCEEDED(dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL, kTitleFont * s, L"", titleFormat_.put()))) {
        titleFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        titleFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (SUCCEEDED(dw->CreateEllipsisTrimmingSign(titleFormat_.get(), titleEllipsis_.put()))) {
            titleFormat_->SetTrimming(&trimming, titleEllipsis_.get());
        }
    } else {
        titleFormat_ = nullptr;
    }
    if (SUCCEEDED(dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL, kButtonFont * s, L"", buttonFormat_.put()))) {
        buttonFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        buttonFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        buttonFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    } else {
        buttonFormat_ = nullptr;
    }
    // The button's width follows its text.
    explorerTextW_ = 0.0f;
    winrt::com_ptr<IDWriteTextLayout> layout;
    if (buttonFormat_ && SUCCEEDED(dw->CreateTextLayout(kExplorerText, static_cast<UINT32>(wcslen(kExplorerText)),
                                                        buttonFormat_.get(), 1000.0f, 100.0f, layout.put()))) {
        DWRITE_TEXT_METRICS tm{};
        if (SUCCEEDED(layout->GetMetrics(&tm))) explorerTextW_ = tm.widthIncludingTrailingWhitespace;
    }
    formatScale_ = s;
}

ItemGridView::Style FolderPopup::ViewStyle() const {
    ItemGridView::Style v;
    v.scale = style_.scale;
    v.text = style_.text;
    v.secondary = style_.secondary;
    v.hotBackground = style_.hotBackground;
    v.hotBorder = style_.hotBorder;
    v.accent = style_.accent;
    v.scrollbar = (style_.text & 0x00FFFFFF) | 0x80000000;
    v.cellW = kCellW;
    v.cellH = kCellH;
    v.rowH = kListRowH;
    v.scrollSeconds = kScrollSeconds;
    v.reduceMotion = style_.reduceMotion;
    return v;
}

SIZE FolderPopup::Measure() {
    EnsureFormats();
    RegisterDrop();
    if (!viewReady_ && Window()) {
        view_.Init(Render(), Window(), kViewTimerBase,
                   ItemGridView::Callbacks{
                       [this](size_t i, GridItem& item) { ResolveItem(i, item); },
                       [this](size_t i, ItemGridView::Button b) { Activate(i, b); },
                       [this](int i, POINT screen) { ContextMenu(i, screen); },
                       [this](size_t i, POINT screen) {
                           if (group_ && groupCallbacks_.drag) groupCallbacks_.drag(i, screen);
                       },
                   });
        viewReady_ = true;
    }
    if (!view_.Attached()) view_.Attach(ContentLayer());
    view_.SetStyle(ViewStyle());

    const float s = style_.scale;
    const LONG pad = Px(kPad, s), headerH = Px(kHeaderH, s), cellW = Px(kCellW, s), cellH = Px(kCellH, s);
    const LONG rowH = Px(kListRowH, s);
    // A group has no "Open in Explorer".
    const LONG buttonW = group_ ? 0 : static_cast<LONG>(std::ceil(explorerTextW_)) + 2 * Px(kButtonPadX, s);
    const LONG buttonH = Px(kButtonH, s), back = Px(kBackSize, s), margin = Px(kMonitorMargin, s);
    const bool navigated = !group_ && !SamePath(current_, root_);
    const size_t n = view_.Count();
    const bool list = view_.GetMode() == ItemGridView::Mode::List;

    // The room beside the dock: above a bottom dock (below a top one), the monitor's height beside
    // a vertical one; the width the monitor's (beside a vertical dock: the room on its side).
    const RECT& m = style_.monitor;
    LONG roomW = (m.right - m.left) - 2 * margin;
    LONG roomH = (anchor_.side - m.top) - margin;
    switch (anchor_.edge) {
        case DockEdge::Top: roomH = (m.bottom - anchor_.side) - margin; break;
        case DockEdge::Left:
            roomW = (m.right - anchor_.side) - margin;
            roomH = (m.bottom - m.top) - 2 * margin;
            break;
        case DockEdge::Right:
            roomW = (anchor_.side - m.left) - margin;
            roomH = (m.bottom - m.top) - 2 * margin;
            break;
        case DockEdge::Bottom:
        default: break;
    }
    const LONG availW = roomW - 2 * pad;
    const LONG availH = roomH - 2 * pad - Px(kHeaderH + kHeaderGap, s);

    LONG contentW = 0;
    size_t visibleRows = 0;
    if (n > 0 && !list) {
        // Columns min(6, ceil(sqrt(n))) within the room; rows up to 8 (and the room), then it scrolls.
        const size_t maxColumns = static_cast<size_t>(std::max<LONG>(1, availW / std::max<LONG>(1, cellW)));
        const size_t cols = std::max<size_t>(
            1, std::min({kMaxColumns, static_cast<size_t>(std::ceil(std::sqrt(static_cast<double>(n)))), maxColumns, n}));
        const size_t maxRows = static_cast<size_t>(std::max<LONG>(1, availH / std::max<LONG>(1, cellH)));
        view_.SetColumns(cols);
        visibleRows = std::min({(n + cols - 1) / cols, maxRows, kMaxGridRows});
        contentW = static_cast<LONG>(cols) * cellW;
    } else if (n > 0) {
        const size_t maxRows = static_cast<size_t>(std::max<LONG>(1, availH / std::max<LONG>(1, rowH)));
        visibleRows = std::min({n, maxRows, kMaxListRows});
        contentW = std::min(Px(kListWidth, s) - 2 * pad, availW);
    }

    LONG w = std::max({2 * pad + contentW, Px(kMinWidth, s),
                       2 * pad + (navigated ? back : 0) + Px(kMinTitleW, s) + buttonW + Px(8.0f, s)});
    w = std::min<LONG>(w, std::max<LONG>(Px(kMinWidth, s), (m.right - m.left) - 2 * margin));

    LONG y = pad;
    headerRect_ = RECT{pad, y, w - pad, y + headerH};
    backRect_ = navigated ? RECT{pad, y + (headerH - back) / 2, pad + back, y + (headerH - back) / 2 + back} : RECT{};
    explorerRect_ = group_ ? RECT{}
                           : RECT{w - pad - buttonW, y + (headerH - buttonH) / 2, w - pad, y + (headerH - buttonH) / 2 + buttonH};
    y += headerH + Px(kHeaderGap, s);

    if (n == 0) {
        messageRect_ = RECT{pad, y, w - pad, y + Px(kMessageH, s)};
        viewRect_ = RECT{};
        y = messageRect_.bottom;
    } else {
        messageRect_ = RECT{};
        const LONG h = static_cast<LONG>(visibleRows) * (list ? rowH : cellH);
        viewRect_ = RECT{pad, y, w - pad, y + h};
        y = viewRect_.bottom;
    }
    y += pad;
    view_.SetBounds(viewRect_);
    view_.Refresh();
    if (pendingSelect_ >= 0) {
        const int sel = pendingSelect_;
        pendingSelect_ = -1;
        const RECT r = view_.ItemRect(static_cast<size_t>(sel));
        const LONG viewH = viewRect_.bottom - viewRect_.top;
        view_.ScrollTo(static_cast<float>(r.top - viewRect_.top - (viewH - (r.bottom - r.top)) / 2), false);
        view_.Select(sel, false);
    }
    if (Hot() >= 0 && Hot() < kHitBack) ClearHot();
    return SIZE{w, y};
}

void FolderPopup::Paint(ID2D1DeviceContext* dc) {
    EnsureFormats();
    IDWriteFactory* dw = Render() ? Render()->DWrite() : nullptr;
    const float s = style_.scale;
    const float stroke = std::max(1.0f, std::round(s));
    const float inset = stroke / 2.0f;
    const float radius = std::round(kCellRadius * s);
    const int hot = Hot();

    PaintPanel(dc, style_.tint, style_.border);
    winrt::com_ptr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(Theme::ToColor(style_.text), brush.put()))) return;

    auto hotBox = [&](const RECT& r, float fillFactor, bool outline) {
        const D2D1_RECT_F f = ToRectF(r);
        brush->SetColor(Theme::ToColor(style_.hotBackground, fillFactor));
        dc->FillRoundedRectangle(D2D1::RoundedRect(f, radius, radius), brush.get());
        if (!outline) return;
        brush->SetColor(Theme::ToColor(style_.hotBorder));
        const D2D1_RECT_F edge = D2D1::RectF(f.left + inset, f.top + inset, f.right - inset, f.bottom - inset);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(edge, radius - inset, radius - inset), brush.get(), stroke);
    };

    // Header: back glyph, title (the path below the pinned folder), "Open in Explorer".
    if (!Empty(backRect_)) {
        if (hot == kHitBack) hotBox(backRect_, 1.0f, true);
        const D2D1_RECT_F b = ToRectF(backRect_);
        const float cx = (b.left + b.right) / 2.0f - std::round(1.0f * s);
        const float cy = (b.top + b.bottom) / 2.0f;
        const float g = std::round(5.0f * s);
        brush->SetColor(Theme::ToColor(style_.text, hot == kHitBack ? 1.0f : 0.8f));
        const float glyphStroke = std::max(1.0f, 1.5f * s);
        dc->DrawLine(D2D1::Point2F(cx + g / 2.0f, cy - g), D2D1::Point2F(cx - g / 2.0f, cy), brush.get(), glyphStroke);
        dc->DrawLine(D2D1::Point2F(cx - g / 2.0f, cy), D2D1::Point2F(cx + g / 2.0f, cy + g), brush.get(), glyphStroke);
    }
    {
        const float left = static_cast<float>(Empty(backRect_) ? headerRect_.left + Px(4.0f, s)
                                                               : backRect_.right + Px(6.0f, s));
        const float right =
            static_cast<float>(group_ ? headerRect_.right - Px(4.0f, s) : explorerRect_.left - Px(8.0f, s));
        const std::wstring title = Title(right - left);
        winrt::com_ptr<IDWriteTextLayout> layout;
        if (dw && titleFormat_ && !title.empty() && right > left &&
            SUCCEEDED(dw->CreateTextLayout(title.c_str(), static_cast<UINT32>(title.size()), titleFormat_.get(),
                                           right - left, static_cast<float>(headerRect_.bottom - headerRect_.top),
                                           layout.put()))) {
            brush->SetColor(Theme::ToColor(style_.text));
            dc->DrawTextLayout(D2D1::Point2F(left, static_cast<float>(headerRect_.top)), layout.get(), brush.get(),
                               D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
    if (!Empty(explorerRect_)) hotBox(explorerRect_, hot == kHitExplorer ? 1.0f : 0.6f, hot == kHitExplorer);
    if (buttonFormat_ && !Empty(explorerRect_)) {
        brush->SetColor(Theme::ToColor(style_.text, hot == kHitExplorer ? 1.0f : 0.85f));
        dc->DrawText(kExplorerText, static_cast<UINT32>(wcslen(kExplorerText)), buttonFormat_.get(),
                     ToRectF(explorerRect_), brush.get());
    }
    // Hairline under the header.
    {
        const float y = static_cast<float>(headerRect_.bottom) + std::round(kHeaderGap * s / 2.0f) - inset;
        brush->SetColor(Theme::ToColor(style_.border));
        dc->DrawLine(D2D1::Point2F(static_cast<float>(headerRect_.left), y),
                     D2D1::Point2F(static_cast<float>(headerRect_.right), y), brush.get(), stroke);
    }
    // The entries are the view's own layers, in front of this surface.
    if (!Empty(messageRect_) && buttonFormat_) {
        const wchar_t* text = group_      ? L"This group is empty"
                              : readable_ ? L"This folder is empty"
                                          : L"Cannot open this folder";
        brush->SetColor(Theme::ToColor(style_.text, 0.6f));
        dc->DrawText(text, static_cast<UINT32>(wcslen(text)), buttonFormat_.get(), ToRectF(messageRect_), brush.get());
    }
}

int FolderPopup::HitAt(POINT pt, int* part) const {
    if (part) *part = 0;
    if (!Empty(backRect_) && PtInRect(&backRect_, pt)) return kHitBack;
    if (!Empty(explorerRect_) && PtInRect(&explorerRect_, pt)) return kHitExplorer;
    return -1;  // the entries are the view's
}

void FolderPopup::OnClick(int index, int, Button button) {
    if (button == Button::Right) return;
    switch (index) {
        case kHitBack:
            if (button == Button::Left) GoBack();
            return;
        case kHitExplorer:
            Open(current_, true);
            return;
        default:
            return;
    }
}

bool FolderPopup::SelectEntry(int index) {
    if (!OnScreen() || index < 0 || static_cast<size_t>(index) >= view_.Count()) return false;
    view_.Select(index, true);
    return true;
}

bool FolderPopup::ScrollTo(int px) {
    if (!OnScreen()) return false;
    view_.ScrollTo(static_cast<float>(px), true);
    return true;
}

bool FolderPopup::OpenEntry(int index) {
    if (!OnScreen() || index < 0 || static_cast<size_t>(index) >= view_.Count()) return false;
    Activate(static_cast<size_t>(index), ItemGridView::Button::Left);
    return true;
}

bool FolderPopup::OnMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) {
    const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    result = 0;
    switch (msg) {
        case WM_KEYDOWN:
            if (wp == VK_BACK) {
                GoBack();
                return true;
            }
            return view_.KeyDown(static_cast<UINT>(wp));
        case WM_SYSKEYDOWN:
            if (wp == VK_UP) {  // Alt+Up
                GoBack();
                return true;
            }
            if (wp == VK_F10 && GetKeyState(VK_SHIFT) < 0) return view_.KeyDown(VK_F10);
            return false;
        case WM_CHAR:
            return view_.Char(static_cast<wchar_t>(wp));
        case WM_MOUSEMOVE:
            view_.MouseMove(pt);
            return false;  // the base tracks the header's hot button and the leave
        case WM_MOUSELEAVE:
            view_.MouseLeave();
            return false;
        case WM_LBUTTONDOWN:
            return view_.ButtonDown(ItemGridView::Button::Left, pt);
        case WM_LBUTTONUP:
            return view_.ButtonUp(ItemGridView::Button::Left, pt);
        case WM_MBUTTONDOWN:
            return view_.ButtonDown(ItemGridView::Button::Middle, pt);
        case WM_MBUTTONUP:
            return view_.ButtonUp(ItemGridView::Button::Middle, pt);
        case WM_RBUTTONDOWN:
            return view_.ButtonDown(ItemGridView::Button::Right, pt);
        case WM_RBUTTONUP:
            return view_.ButtonUp(ItemGridView::Button::Right, pt);
        case WM_MOUSEWHEEL: {
            POINT client = pt;  // screen coordinates
            if (Window()) ScreenToClient(Window(), &client);
            view_.Wheel(GET_WHEEL_DELTA_WPARAM(wp), client);
            return true;
        }
        case WM_CAPTURECHANGED:
            view_.CaptureLost();
            return false;
        case WM_TIMER:
            return view_.Timer(wp);
        case kDropDoneMessage:
            // A drop's files arrived: show them (the folder shown, same anchor).
            if (OnScreen() && !current_.empty()) {
                const LONG scroll = view_.ScrollOffset();
                Load(std::wstring(current_));
                if (ShowAt(frame_, anchor_)) view_.ScrollTo(static_cast<float>(scroll), false);
            }
            return true;
        default:
            return false;
    }
}

// --- File drops ------------------------------------------------------------------------------------

void FolderPopup::RegisterDrop() {
    if (dropTarget_ || !Window()) return;
    dropTarget_ = new FolderDropTarget(this);
    if (FAILED(RegisterDragDrop(Window(), dropTarget_))) {
        dropTarget_->Detach();
        dropTarget_->Release();
        dropTarget_ = nullptr;
    }
}

std::wstring FolderPopup::DropFolderAt(POINT screen, int* entry) const {
    if (entry) *entry = -1;
    POINT client = screen;
    if (Window()) ScreenToClient(Window(), &client);
    const int i = view_.HitTest(client);
    if (i >= 0 && static_cast<size_t>(i) < entries_.size() && entries_[static_cast<size_t>(i)].folder) {
        const std::wstring& dest = entries_[static_cast<size_t>(i)].path;
        // Not into one of the dragged folders itself.
        const bool self = std::any_of(dragFiles_.begin(), dragFiles_.end(), [&](const std::wstring& f) { return IsInside(dest, f); });
        if (!self) {
            if (entry) *entry = i;
            return dest;
        }
    }
    return current_;
}

DWORD FolderPopup::EffectFor(const std::wstring& dest, DWORD keys, DWORD allowed) const {
    if (dragFiles_.empty() || dest.empty() || !readable_) return DROPEFFECT_NONE;
    // Nothing to do: every file is already there (a move), or a folder into itself.
    const bool intoSelf =
        std::any_of(dragFiles_.begin(), dragFiles_.end(), [&](const std::wstring& f) { return IsInside(dest, f); });
    if (intoSelf) return DROPEFFECT_NONE;
    DWORD effect = 0;
    if (keys & MK_CONTROL) {
        effect = DROPEFFECT_COPY;
    } else if (keys & MK_SHIFT) {
        effect = DROPEFFECT_MOVE;
    } else {
        // Explorer's default: a move on the same volume, a copy across volumes.
        const std::wstring volume = VolumeOf(dest);
        const bool same = !volume.empty() && std::all_of(dragFiles_.begin(), dragFiles_.end(), [&](const std::wstring& f) {
            return SamePath(VolumeOf(f), volume);
        });
        effect = same ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
    }
    if (effect == DROPEFFECT_MOVE) {
        const bool already = std::all_of(dragFiles_.begin(), dragFiles_.end(), [&](const std::wstring& f) {
            return SamePath(Parent(f), dest);
        });
        if (already) return DROPEFFECT_NONE;
    }
    if (!(allowed & effect)) effect = (allowed & DROPEFFECT_COPY) ? DROPEFFECT_COPY : (allowed & DROPEFFECT_MOVE);
    return effect;
}

DWORD FolderPopup::DragEnter(IDataObject* data, POINT screen, DWORD keys, DWORD allowed) {
    dragFiles_ = Visible() ? FilesOf(data) : std::vector<std::wstring>{};
    if (group_ && !dragFiles_.empty() && !(groupCallbacks_.accepts && groupCallbacks_.accepts(dragFiles_))) {
        dragFiles_.clear();  // nothing that can become a member: the no-drop cursor
    }
    return DragOver(screen, keys, allowed);
}

DWORD FolderPopup::DragOver(POINT screen, DWORD keys, DWORD allowed) {
    if (dragFiles_.empty() || !Visible()) return DROPEFFECT_NONE;
    if (group_) return (allowed & DROPEFFECT_LINK) ? DROPEFFECT_LINK : (allowed & DROPEFFECT_COPY);
    int entry = -1;
    const std::wstring dest = DropFolderAt(screen, &entry);
    const DWORD effect = EffectFor(dest, keys, allowed);
    view_.SetDropHighlight(effect != DROPEFFECT_NONE ? entry : -1);
    return effect;
}

void FolderPopup::DragLeave() {
    dragFiles_.clear();
    view_.SetDropHighlight(-1);
}

DWORD FolderPopup::Drop(IDataObject* data, POINT screen, DWORD keys, DWORD allowed) {
    dragFiles_ = Visible() ? FilesOf(data) : std::vector<std::wstring>{};
    if (group_) {
        // Shortcuts / apps / folders become members (the owner decides); the files stay where they are.
        std::vector<std::wstring> files = std::move(dragFiles_);
        dragFiles_.clear();
        if (files.empty() || !groupCallbacks_.accepts || !groupCallbacks_.accepts(files) || !groupCallbacks_.drop) {
            return DROPEFFECT_NONE;
        }
        groupCallbacks_.drop(files);
        return (allowed & DROPEFFECT_LINK) ? DROPEFFECT_LINK : (allowed & DROPEFFECT_COPY);
    }
    int entry = -1;
    const std::wstring dest = DropFolderAt(screen, &entry);
    const DWORD effect = EffectFor(dest, keys, allowed);
    view_.SetDropHighlight(-1);
    if (effect == DROPEFFECT_NONE) {
        dragFiles_.clear();
        return DROPEFFECT_NONE;
    }
    // Done by us (also a move): the source is told "copied", so it never deletes anything itself.
    FileJob* job = new FileJob{std::move(dragFiles_), dest, effect == DROPEFFECT_MOVE, Window()};
    dragFiles_.clear();
    if (job->move) {
        std::erase_if(job->files, [&](const std::wstring& f) { return SamePath(Parent(f), dest); });
    }
    if (HANDLE thread = CreateThread(nullptr, 0, &FileJobThread, job, 0, nullptr)) {
        CloseHandle(thread);
    } else {
        delete job;
        MessageBeep(MB_ICONWARNING);
        return DROPEFFECT_NONE;
    }
    return DROPEFFECT_COPY & allowed ? DROPEFFECT_COPY : effect;
}
