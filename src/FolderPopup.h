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
#include "ShellFiles.h"

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
//
// The same popup shows a shortcut group (ShowGroup): the group's name, its members (icons from
// the dock), a click opens / activates one (the owner does it), right click Open / Remove from
// group / Move to dock, a member dragged out starts the owner's drag, files dropped are offered
// to the owner (added as members). No Explorer button, no navigation. Round 24 phase 24B: a group
// is laid out by a GroupLayout (the tray drawer's icon size, names below / beside / none, font,
// columns, title on / off; the default is the look above) and the tray drawer has a settings gear
// at the right of its title row (GroupCallbacks::settings).
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
    // A member's right-click menu commands (GroupCallbacks::command).
    enum MemberCommand : UINT { kMemberOpen = 1, kMemberRemove = 2, kMemberToDock = 3 };
    struct GroupEntry {
        std::wstring label;
        winrt::com_ptr<ID2D1Bitmap> icon;
    };
    struct GroupCallbacks {
        std::function<void(size_t member)> open;                  // a click / Enter on a member
        std::function<void(size_t member, UINT command)> command; // its menu (MemberCommand)
        std::function<void(size_t member, POINT screen)> drag;    // dragged out (the button is still down)
        std::function<bool(const std::vector<std::wstring>& files)> accepts;  // a file drag: can these be added?
        std::function<void(const std::vector<std::wstring>& files)> drop;     // ... they were dropped
        // The tray drawer (round 4 phase 8): set, a right click (or the Apps key) on a member calls
        // it instead of the member menu, a middle click calls `middle`, and Kind() reports 3.
        std::function<void(size_t member, POINT screen)> menu;
        std::function<void(size_t member)> middle;
        // Round 19 phase 19A, the tray drawer: a tray icon dragged over it (the owner's private
        // drag format, DockWindow::DragTrayIcon, from the drawer itself, the grid or a trayApp slot).
        // `trayAccepts`: the drag is one the owner takes (an insert marker then follows the pointer);
        // `trayDrop`: it was dropped before member `before` (the member count: after the last).
        std::function<bool(IDataObject* data)> trayAccepts;
        std::function<void(IDataObject* data, size_t before)> trayDrop;
        // Round 24 phase 24B, the tray drawer: set, a gear at the right of the title row (when the
        // layout shows the title) calls it: the owner opens the tray item's settings window.
        std::function<void()> settings;
    };
    // Round 24 phase 24B: how a group is laid out (the tray drawer's drawer* keys). The default is
    // the round-3 look exactly (88 x 84 DIP cells, 40 DIP icons, 12 DIP names in two lines under
    // them, up to 6 columns, the title row with its hairline), which folders and shortcut groups
    // always get. Cell sizes follow the icon and the font (as FolderWindow::ViewStyle derives them):
    //  - Below: cells max(88 x font / 12, icon + 48) wide, 6 + icon + 4 + two lines + 2 tall;
    //  - Beside: list rows (the icon, then the name on one line), icon + 12 tall (at least a line
    //    + 12), one column whatever `columns` says (ItemGridView's list mode has one);
    //  - None: icons only, icon + 12 square cells.
    struct GroupLayout {
        enum class Labels { Below, Beside, None };
        float iconDip = 40.0f;
        Labels labels = Labels::Below;
        float fontDip = 12.0f;   // the names' size
        size_t columns = 0;      // grid: 0 automatic (min(6, ceil(sqrt(n))) within the room), else at most this many
        bool title = true;       // the title row and the hairline under it
        bool operator==(const GroupLayout&) const = default;
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
    // Shows a shortcut group's members (titled `name`) at `anchor`, laid out as `layout` (24B).
    bool ShowGroup(const std::wstring& name, std::vector<GroupEntry> members, ItemView view, const Style& style,
                   const PopupAnchor& anchor, GroupCallbacks callbacks, const GroupLayout& layout = GroupLayout{});
    // The group shown got other members / another name: shown again in place (same anchor). The
    // layout is kept, unless `layout` gives another one (24B: the tray keys changed while open).
    void UpdateGroup(const std::wstring& name, std::vector<GroupEntry> members, const GroupLayout* layout = nullptr);
    // 0 hidden, 1 a folder, 2 a group, 3 the tray drawer.
    int Kind() const { return !OnScreen() ? 0 : group_ ? (groupCallbacks_.menu ? 3 : 2) : 1; }
    // Member `index`'s cell on screen (px; false while hidden, or the member is scrolled away).
    bool MemberScreenRect(size_t index, RECT& screen) const;

    // Entries of the folder (members of the group) shown (0 while hidden).
    size_t EntryCount() const { return OnScreen() ? view_.Count() : 0; }
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
    // Round 24 phase 24B: the group's layout in use, the members' icon size in px as laid out,
    // whether the title row / the gear is shown (all while on screen), and a press on the gear
    // (false: no gear shown).
    const GroupLayout& GetGroupLayout() const { return layout_; }
    int IconPx() const;
    bool TitleShown() const { return OnScreen() && !Empty(headerRect_); }
    bool GearShown() const { return OnScreen() && !Empty(gearRect_); }
    bool PressGear();

protected:
    SIZE Measure() override;
    void Paint(ID2D1DeviceContext* dc) override;
    int HitAt(POINT pt, int* part) const override;
    void OnClick(int index, int part, Button button) override;
    void OnHidden() override;
    void OnDestroying() override;
    bool OnMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) override;

private:
    // HitAt indices beyond the cells.
    static constexpr int kHitBack = 1000;
    static constexpr int kHitExplorer = 1001;
    static constexpr int kHitGear = 1002;  // round 24 phase 24B
    static bool Empty(const RECT& r) { return r.right <= r.left || r.bottom <= r.top; }
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

    // --- file drops (ShellFiles::DropTarget, on the UI thread) ---
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
    bool group_ = false;              // showing a group (members_), not a folder
    GroupCallbacks groupCallbacks_;
    GroupLayout layout_;              // round 24 phase 24B (folders: always the default)
    std::vector<GroupEntry> members_;
    // Loads members_ into the view.
    void LoadGroup();
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
    RECT gearRect_{};        // round 24 phase 24B: the tray drawer's settings gear (empty: none)
    RECT messageRect_{};     // "This folder is empty" / "Cannot open this folder"
    RECT viewRect_{};
    float explorerTextW_ = 0.0f;

    float formatScale_ = 0.0f;
    winrt::com_ptr<IDWriteTextFormat> titleFormat_;   // header
    winrt::com_ptr<IDWriteTextFormat> buttonFormat_;  // "Open in Explorer", messages
    winrt::com_ptr<IDWriteInlineObject> titleEllipsis_;
    winrt::com_ptr<IDWriteTextFormat> glyphFormat_;   // 24B: the gear (Segoe Fluent Icons / MDL2 Assets; null: drawn)

    // Drops
    ShellFiles::DropTarget* dropTarget_ = nullptr;  // registered on the window (one reference held)
    std::vector<std::wstring> dragFiles_;     // the files of the drag over the popup
    bool trayDrag_ = false;                   // a tray icon drag the drawer takes is over it (19A)
    // The insert index for a drag at a screen point (19A): the view's nearest cell edge, else the end.
    size_t TrayInsertAt(POINT screen) const;
};
