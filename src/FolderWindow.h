#pragma once

#include <windows.h>
#include <shobjidl.h>

#include <winrt/base.h>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "Backdrop.h"
#include "Config.h"
#include "IconCache.h"
#include "ItemGridView.h"
#include "Renderer.h"
#include "ShellFiles.h"

// A desktop folder window (round 4 phase 12, docks[] kind "folderWindow"; Fences-like): a titled
// panel on the desktop layer (DesktopLayer: a top-level window directly above Progman, under every
// app window, activatable without rising) showing a folder's entries in an ItemGridView.
//
// - Look: the theme's acrylic (a Backdrop window kept directly beneath it) or its bar fill, the
//   bar hairline, the title (the folder's name unless `title` is set) in a 30 DIP title bar.
// - Move by the title bar, resize by the edges and corners (own capture loop: no system move /
//   size loop, so no Aero Snap), snapping within 8 DIP to the work area's edges and to the other
//   folder windows; x / y / w / h are saved in DIPs relative to the work area. A double click on
//   the title (or a click on its chevron) collapses the window to its title bar.
// - Contents: the folder's entries (folders first, then `sort`), hidden ones with showHidden;
//   icons from the system image list (IShellItemImageFactory above 48 px), loaded as rows come
//   into view; auto view = a grid up to 48 entries, a list above. Live: SHChangeNotifyRegister on
//   the folder, coalesced by one 250 ms one-shot timer (no polling).
// - 12b: rules (Desktop files matching `rules` are moved here by DockManager: SortDesktop), the
//   appearance keys (opacity, titleColor, hideTitle, labelLines, transparentBackground,
//   rollUpOnHover) and the clipboard (Ctrl+C / Ctrl+X: the shell data object with "Preferred
//   DropEffect"; Ctrl+V pastes CF_HDROP files like Explorer).
// - Input: a click selects (Ctrl / Shift: several), a double click or Enter opens with the shell's
//   default verb (singleClick: a click opens), F2 renames inline, Delete recycles (IFileOperation;
//   Explorer's confirmation setting applies), Ctrl+A / arrows / Home / End / typing select, the
//   shell's context menu on entries (IContextMenu: Open, Rename, Delete, Properties ...), the
//   window's own menu on the background. Entries drag out as a shell data object
//   (SHDoDragDrop: Explorer and apps accept it); files dropped in are moved / copied like
//   Explorer does (ShellFiles, the folder popup's rules).
class FolderWindow {
public:
    static constexpr wchar_t kClassName[] = L"WinDockFolderWindow";

    struct Host {
        Renderer* renderer = nullptr;  // shared by every folder window (DockManager)
        // The config entry changed by the window itself (moved, resized, collapsed, a menu setting).
        std::function<void()> changed;
        // The other folder windows' screen rects (snapping).
        std::function<std::vector<RECT>(const FolderWindow*)> neighbours;
        // A commit failed with a device-lost error: the shared device must be rebuilt (posted).
        std::function<void()> deviceLost;
        // The background menu's "Remove this folder window" / "New folder window…" (posted by the owner).
        std::function<void(size_t cfg)> remove;
        std::function<void()> newWindow;
    };

    FolderWindow() = default;
    FolderWindow(const FolderWindow&) = delete;
    FolderWindow& operator=(const FolderWindow&) = delete;
    ~FolderWindow() { Destroy(); }

    HRESULT Create(HINSTANCE instance, Config* config, size_t cfg, HMONITOR monitor, Host host);
    void Destroy();
    HWND Hwnd() const { return hwnd_; }
    size_t ConfigIndex() const { return cfg_; }
    void SetConfigIndex(size_t cfg) { cfg_ = cfg; }
    // Another monitor (or the same one after a display change): placed again.
    void SetMonitor(HMONITOR monitor);
    // The config entry may have changed (Settings, Reload config): applies what differs.
    void ApplyConfig();
    // The shared device is about to be rebuilt / was rebuilt.
    void ReleaseDevice();
    HRESULT RestoreDevice();
    RECT ScreenRect() const { return rect_; }

    // Automation (README "Desktop folder windows").
    LRESULT QueryState(WPARAM what, LPARAM arg) const;
    // ItemCommand 839 - 849 on this window; `arg` = LOWORD(lParam), `high` = HIWORD(wParam).
    LRESULT Command(UINT cmd, int arg, int high);
    // The paths of the selected entries (the drag / test drop source).
    std::vector<std::wstring> SelectedPaths() const;
    // Runs the drop path as if `files` were dropped on entry `entry` (-1: the background) with
    // `keys` held (test hook: OLE's drag loop needs real input). Returns the effect.
    DWORD TestDrop(const std::vector<std::wstring>& files, int entry, DWORD keys);

private:
    static constexpr UINT kChangeMessage = WM_APP + 10;   // SHChangeNotifyRegister
    static constexpr UINT kJobDoneMessage = WM_APP + 11;  // a file operation finished
    static constexpr UINT kMenuMessage = WM_APP + 12;     // posted: the context menu (automation)
    static constexpr UINT_PTR kRefreshTimer = 1;          // 250 ms one-shot after a change notification
    static constexpr UINT_PTR kRollTimer = 2;             // rollUpOnHover: 400 ms after the pointer left
    static constexpr UINT_PTR kViewTimerBase = 100;       // the view's (100, 101)

    enum class Grab { None, Move, Left, Right, Top, Bottom, TopLeft, TopRight, BottomLeft, BottomRight };
    struct Entry {
        std::wstring name;  // file name
        std::wstring path;
        bool folder = false;
        ULONGLONG size = 0;
        FILETIME modified{};
    };

    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK EditProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

    const DockConfig& Cfg() const { return config_->docks[cfg_]; }
    DockConfig& Cfg() { return config_->docks[cfg_]; }
    // Shown collapsed: the config's `collapsed`, unless rolled down by the pointer (rollUpOnHover).
    bool Collapsed() const { return Cfg().folder.collapsed && !peek_; }
    // The title bar's height in DIPs (hideTitle: a thin strip to move the window by).
    float TitleDip() const { return Cfg().folder.hideTitle ? 10.0f : 30.0f; }
    // rollUpOnHover: a collapsed window shown expanded while the pointer is over it (not saved).
    void Peek(bool on);
    // --- the clipboard (12b) ---
    void CopySelection(bool cut);
    void Paste();
    float Px(float dip) const { return std::round(dip * scale_); }
    // --- placement ---
    RECT WorkArea() const;
    RECT RectFromConfig() const;
    void StoreRect();  // rect_ -> config x / y / w (h only expanded)
    void Place(const RECT& r, bool relayout);
    void Relayout();
    Grab GrabAt(POINT client) const;
    void SnapMove(RECT& r) const;
    void SnapEdges(RECT& r, Grab grab) const;
    void SetCollapsed(bool collapsed);
    // --- drawing ---
    HRESULT EnsureTarget();
    void DrawPanel();
    void Commit();
    void EnsureFormats();
    ItemGridView::Style ViewStyle() const;
    std::wstring TitleText() const;
    void UpdateBackdrop();
    // --- contents ---
    std::wstring FolderPath() const;  // the config path, environment variables expanded
    void Load();
    // Load again keeping the scroll position and the selection (by path).
    void Reload();
    void Watch();
    void Unwatch();
    void ResolveItem(size_t index, GridItem& item);
    winrt::com_ptr<ID2D1Bitmap> IconFor(const std::wstring& path, int sysIndex);
    void Activate(size_t index);
    void Open(const std::wstring& path, const wchar_t* verb);
    // --- menus, rename, delete, drag ---
    void ContextMenu(int index, POINT screen);
    void ShellMenu(const std::vector<std::wstring>& paths, int index, POINT screen);
    void BackgroundMenu(POINT screen);
    void BeginRename(int index);
    void CommitRename(const std::wstring* text);
    void CancelRename();
    void RecycleSelection();
    void BeginDrag(size_t index, POINT screen);
    // --- drops ---
    void RegisterDrop();
    DWORD DragEnter(IDataObject* data, POINT screen, DWORD keys, DWORD allowed);
    DWORD DragOver(POINT screen, DWORD keys, DWORD allowed);
    void DragLeave();
    DWORD Drop(IDataObject* data, POINT screen, DWORD keys, DWORD allowed);
    std::wstring DropFolderAt(POINT screen, int* entry) const;
    DWORD DropInto(std::vector<std::wstring> files, const std::wstring& dest, DWORD keys, DWORD allowed);
    bool StartJob(ShellFiles::Op op, std::vector<std::wstring> files, const std::wstring& dest);

    HINSTANCE instance_ = nullptr;
    Config* config_ = nullptr;
    size_t cfg_ = 0;
    HMONITOR monitor_ = nullptr;
    Host host_;
    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    float scale_ = 1.0f;
    RECT rect_{};
    DockConfig applied_;  // the config entry as last applied (ApplyConfig compares)

    // Composition (on the shared device).
    winrt::com_ptr<IDCompositionTarget> target_;
    Layer root_;
    Layer panel_;  // background, hairline, title
    Backdrop backdrop_;
    bool acrylic_ = false;
    ItemGridView view_;
    bool viewReady_ = false;
    RECT titleRect_{}, chevronRect_{}, contentRect_{};
    winrt::com_ptr<IDWriteTextFormat> titleFormat_, messageFormat_;
    winrt::com_ptr<IDWriteInlineObject> titleEllipsis_;
    float formatScale_ = 0.0f;

    // Contents.
    std::wstring folder_;     // expanded path being shown
    std::vector<Entry> entries_;
    bool readable_ = true;
    IconCache icons_;         // FromHicon only (per window; the bitmaps are on the shared device)
    std::unordered_map<int, winrt::com_ptr<ID2D1Bitmap>> iconByIndex_;
    winrt::com_ptr<IWICImagingFactory> wic_;
    ULONG notifyId_ = 0;
    bool refreshPending_ = false;
    size_t refreshes_ = 0;      // reloads after change notifications (QueryState 194)
    size_t notifications_ = 0;  // change notifications received (QueryState 201)
    size_t resolved_ = 0;       // entries whose icon / label was resolved (QueryState 196)
    std::vector<std::wstring> selectAfterLoad_;  // a rename / drop: select these when they appear
    LONG jobs_ = 0;             // this window's file operations in flight

    // Move / resize.
    Grab grab_ = Grab::None;
    POINT grabPt_{};    // screen
    RECT grabRect_{};
    bool grabMoved_ = false;
    bool tracking_ = false;  // TrackMouseEvent
    bool peek_ = false;      // rolled down by the pointer (rollUpOnHover)
    bool rollPending_ = false;

    // Menus, rename, drops.
    winrt::com_ptr<IContextMenu2> menu2_;
    winrt::com_ptr<IContextMenu3> menu3_;
    bool menuOpen_ = false;
    HWND edit_ = nullptr;   // the inline rename box (an owned popup EDIT)
    HFONT editFont_ = nullptr;
    int renameIndex_ = -1;
    std::wstring renamePath_;
    bool dragging_ = false;  // our own SHDoDragDrop is running
    ShellFiles::DropTarget* dropTarget_ = nullptr;
    std::vector<std::wstring> dragFiles_;
};
