#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "Config.h"
#include "IconCache.h"
#include "ItemGridView.h"
#include "PopupWindow.h"
#include "Renderer.h"

class FolderDropTarget;

// The contents of a pinned folder (a macOS "stack"): a click popup (PopupWindow, class
// WinDockFolder) beside the dock with a header (the folder's name, or the path from the pinned
// folder once navigated into a sub-folder, with a back glyph; an "Open in Explorer" button) and
// the folder's entries (sub-folders first, then files, in Explorer's logical name order; hidden
// and system entries skipped) in an ItemGridView:
//  - grid: the round-3 look (88 x 84 DIP cells, up to 6 columns, at most 8 rows visible), scrolling
//    when there are more;
//  - list: a scrolling list (icon, name, size / date) 340 DIP wide, at most 14 rows visible.
// The item's `view` picks it: auto (grid up to kAutoGridMax entries, list above), grid or list.
//
// Clicking a file opens it (ShellExecuteEx) and closes the popup; clicking a sub-folder shows its
// contents in place (same anchor); a middle click opens a sub-folder in Explorer. Backspace,
// Alt+Up or the back glyph go up again (never above the pinned folder). Keyboard: the view's
// (arrows, Home / End, Page Up / Down, Enter, typing selects). Right click: Open / Open in
// Explorer. Files dropped onto the popup are moved or copied into the folder shown (or into the
// sub-folder under the pointer) like Explorer does (same volume: move, else copy; Ctrl copies,
// Shift moves), by IFileOperation on a worker thread. Icons come from the system image list
// (48 px), loaded as rows scroll into view and released when the popup hides.
class FolderPopup : public PopupWindow {
public:
    static constexpr wchar_t kClassName[] = L"WinDockFolder";
    // view "auto": a grid up to this many entries, a list above.
    static constexpr size_t kAutoGridMax = 48;
    // Posted to the popup window when a drop's file operation finished (reload the folder).
    static constexpr UINT kDropDoneMessage = WM_APP + 1;

    struct Callbacks {
        std::function<void()> dismiss;  // an entry was opened: the owner hides the popup
        std::function<void()> closed;   // closed by itself (deactivated, Escape)
    };
    struct Style {
        float scale = 1.0f;
        bool acrylic = false;
        uint32_t tint = 0xCC202020;           // ARGB background
        uint32_t text = 0xFFFFFFFF;
        uint32_t secondary = 0xB6FFFFFF;      // list mode: size / date
        uint32_t border = 0x24FFFFFF;         // hairline around the popup and under the header
        uint32_t hotBackground = 0x1AFFFFFF;  // hovered cell / button
        uint32_t hotBorder = 0x33FFFFFF;
        uint32_t accent = 0xFF4CC2FF;         // keyboard selection, drop target
        float cornerRadiusDip = 8.0f;         // without acrylic
        bool reduceMotion = false;            // no scroll animation
        RECT monitor{};
    };

    FolderPopup() : PopupWindow(kClassName, L"WinDock folder", Mode::Click) {}
    ~FolderPopup() override { Destroy(); }

    void Init(HINSTANCE instance, Renderer* renderer, IconCache* icons, Callbacks callbacks);
    // Shows the contents of `folder` (titled `name`) at `anchor` (beside the dock, away from its
    // edge) in `view`. Returns false if it could not be shown.
    bool Show(const std::wstring& folder, const std::wstring& name, ItemView view, const Style& style,
              const PopupAnchor& anchor);

    // Entries of the folder shown (0 while hidden).
    size_t EntryCount() const { return OnScreen() ? entries_.size() : 0; }
    // The pinned folder the popup was opened for (empty while hidden).
    const std::wstring& Root() const { return root_; }
    // --- automation ---
    // The layout in use: 0 grid, 1 list (-1 hidden).
    int ViewMode() const { return OnScreen() ? (view_.GetMode() == ItemGridView::Mode::List ? 1 : 0) : -1; }
    LONG ScrollOffset() const { return OnScreen() ? view_.ScrollOffset() : 0; }
    int Selected() const { return OnScreen() ? view_.Selected() : -1; }
    size_t LiveRows() const { return view_.LiveRows(); }
    size_t Columns() const { return OnScreen() ? view_.Columns() : 0; }
    // Characters in the path of the folder shown (0 while hidden).
    size_t PathLength() const { return OnScreen() ? current_.size() : 0; }
    bool ViewTimerPending() const { return view_.TimerPending(); }
    // Selects entry `index` (scrolled into view) / scrolls to `px` / opens entry `index` as a click.
    bool SelectEntry(int index);
    bool ScrollTo(int px);
    bool OpenEntry(int index);

protected:
    SIZE Measure() override;
    void Paint(ID2D1DeviceContext* dc) override;
    int HitAt(POINT pt, int* part) const override;
    void OnClick(int index, int part, Button button) override;
    void OnHidden() override;
    void OnDestroying() override;
    bool OnMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) override;

private:
    friend class FolderDropTarget;
    // HitAt indices beyond the cells.
    static constexpr int kHitBack = 1000;
    static constexpr int kHitExplorer = 1001;
    static constexpr UINT_PTR kViewTimerBase = 100;  // the view's timers (100, 101)

    struct Entry {
        std::wstring name;  // file name (sorting)
        std::wstring path;
        bool folder = false;
        ULONGLONG size = 0;
        FILETIME modified{};
    };

    // Enumerates `folder` into entries_ and the view's items (icons load as rows are drawn).
    void Load(const std::wstring& folder);
    // Shows another folder in place (keeps the anchor).
    void Navigate(const std::wstring& folder);
    void GoBack();
    // Opens a file or folder with the shell; closes the popup first.
    void Open(std::wstring path, bool explorer);
    // The view's callbacks.
    void ResolveItem(size_t index, GridItem& item);
    void Activate(size_t index, ItemGridView::Button button);
    void ContextMenu(int index, POINT screen);
    void EnsureFormats();
    // The header text: the pinned folder's name, or "Name › sub › sub" (leading parts elided to fit).
    std::wstring Title(float maxWidth) const;
    ItemGridView::Style ViewStyle() const;
    void Release();

    // --- file drops (FolderDropTarget, on the UI thread) ---
    void RegisterDrop();
    DWORD DragEnter(IDataObject* data, POINT screen, DWORD keys, DWORD allowed);
    DWORD DragOver(POINT screen, DWORD keys, DWORD allowed);
    void DragLeave();
    DWORD Drop(IDataObject* data, POINT screen, DWORD keys, DWORD allowed);
    // Where a drop at the screen point goes (the sub-folder under it, else the folder shown) and
    // the entry to highlight (-1: none).
    std::wstring DropFolderAt(POINT screen, int* entry) const;
    DWORD EffectFor(const std::wstring& dest, DWORD keys, DWORD allowed) const;

    Callbacks callbacks_;
    IconCache* icons_ = nullptr;
    Style style_;
    Frame frame_;
    PopupAnchor anchor_;
    ItemView viewSetting_ = ItemView::Auto;
    ItemGridView view_;
    bool viewReady_ = false;  // view_.Init ran (needs the window)

    std::wstring root_;      // the pinned folder
    std::wstring rootName_;  // its name on the dock
    std::wstring current_;   // the folder shown
    std::vector<Entry> entries_;
    bool readable_ = true;   // the folder could be enumerated
    std::unordered_map<int, winrt::com_ptr<ID2D1Bitmap>> iconByIndex_;  // system image list index -> bitmap
    std::wstring selectAfterLoad_;  // GoBack: the sub-folder we came from (selected)
    int pendingSelect_ = -1;        // ... its index after the load (Measure scrolls to it)

    RECT headerRect_{};
    RECT backRect_{};
    RECT explorerRect_{};
    RECT messageRect_{};     // "This folder is empty" / "Cannot open this folder"
    RECT viewRect_{};
    float explorerTextW_ = 0.0f;

    float formatScale_ = 0.0f;
    winrt::com_ptr<IDWriteTextFormat> titleFormat_;   // header
    winrt::com_ptr<IDWriteTextFormat> buttonFormat_;  // "Open in Explorer", messages
    winrt::com_ptr<IDWriteInlineObject> titleEllipsis_;

    // Drops
    FolderDropTarget* dropTarget_ = nullptr;  // registered on the window (one reference held)
    std::vector<std::wstring> dragFiles_;     // the files of the drag over the popup
};
