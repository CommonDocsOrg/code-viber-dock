#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "IconCache.h"
#include "PopupWindow.h"
#include "Renderer.h"

// The contents of a pinned folder (a macOS "stack"): a click popup (PopupWindow, class
// WinDockFolder) above the folder's icon with a header (the folder's name, a back glyph once
// navigated into a sub-folder, an "Open in Explorer" button) and a grid of the folder's entries
// (sub-folders first, then files, in Explorer's logical name order; hidden and system entries
// skipped). At most kMaxEntries cells (fewer if the monitor is small); the rest is counted in a
// "+N more" footer that opens the folder in Explorer.
//
// Clicking a file opens it (ShellExecuteEx) and closes the popup; clicking a sub-folder shows its
// contents in place (same anchor); a middle click on a sub-folder opens it in Explorer. Backspace
// or the back glyph goes up again (never above the pinned folder). The entries' icons come from
// the system image list (48 px, SHIL_EXTRALARGE), are loaded synchronously when the popup opens
// or navigates and are released when it hides. Nothing is redrawn except on a hot-cell change or
// a navigation.
class FolderPopup : public PopupWindow {
public:
    static constexpr wchar_t kClassName[] = L"WinDockFolder";
    static constexpr size_t kMaxEntries = 48;

    struct Callbacks {
        std::function<void()> dismiss;  // an entry was opened: the owner hides the popup
        std::function<void()> closed;   // closed by itself (deactivated, Escape)
    };
    struct Style {
        float scale = 1.0f;
        bool acrylic = false;
        uint32_t tint = 0xCC202020;           // ARGB background
        uint32_t text = 0xFFFFFFFF;
        uint32_t border = 0x24FFFFFF;         // hairline around the popup and under the header
        uint32_t hotBackground = 0x1AFFFFFF;  // hovered cell / button
        uint32_t hotBorder = 0x33FFFFFF;
        float cornerRadiusDip = 8.0f;         // without acrylic
        RECT monitor{};
    };

    FolderPopup() : PopupWindow(kClassName, L"WinDock folder", Mode::Click) {}
    ~FolderPopup() override { Destroy(); }

    void Init(HINSTANCE instance, Renderer* renderer, IconCache* icons, Callbacks callbacks);
    // Shows the contents of `folder` (titled `name`) at `anchor` (beside the dock, away from its
    // edge). Returns false if it could not be shown.
    bool Show(const std::wstring& folder, const std::wstring& name, const Style& style, const PopupAnchor& anchor);

    // Cells shown (0 while hidden).
    size_t EntryCount() const { return OnScreen() ? entries_.size() : 0; }
    // The pinned folder the popup was opened for (empty while hidden).
    const std::wstring& Root() const { return root_; }

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
    static constexpr int kHitMore = 1002;

    struct Entry {
        std::wstring name;   // file name (sorting)
        std::wstring label;  // display name (the shell's: e.g. no ".lnk")
        std::wstring path;
        bool folder = false;
        winrt::com_ptr<ID2D1Bitmap> icon;
        winrt::com_ptr<IDWriteTextLayout> text;
        RECT rect{};
    };

    // Enumerates `folder` into entries_ (capped to what fits) and loads their icons.
    void Load(const std::wstring& folder);
    // Shows another folder in place (keeps the anchor).
    void Navigate(const std::wstring& folder);
    void GoBack();
    // Opens a file or folder with the shell; closes the popup first.
    void Open(std::wstring path, bool explorer);
    void EnsureFormats();
    std::wstring Title() const;
    void Release();

    Callbacks callbacks_;
    IconCache* icons_ = nullptr;
    Style style_;
    Frame frame_;
    PopupAnchor anchor_;

    std::wstring root_;      // the pinned folder
    std::wstring rootName_;  // its name on the dock
    std::wstring current_;   // the folder shown
    std::vector<Entry> entries_;
    size_t total_ = 0;       // visible entries in the folder (shown + "+N more")
    bool readable_ = true;   // the folder could be enumerated
    size_t columns_ = 1;

    RECT headerRect_{};
    RECT backRect_{};
    RECT explorerRect_{};
    RECT moreRect_{};
    RECT messageRect_{};     // "This folder is empty" / "Cannot open this folder"
    float explorerTextW_ = 0.0f;

    float formatScale_ = 0.0f;
    winrt::com_ptr<IDWriteTextFormat> nameFormat_;    // cells: two centred lines, ellipsized
    winrt::com_ptr<IDWriteTextFormat> titleFormat_;   // header
    winrt::com_ptr<IDWriteTextFormat> buttonFormat_;  // "Open in Explorer", "+N more", messages
    winrt::com_ptr<IDWriteInlineObject> nameEllipsis_;
    winrt::com_ptr<IDWriteInlineObject> titleEllipsis_;
};
