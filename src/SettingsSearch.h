#pragma once

// Round 9 phase 9B: the settings search ("Find a setting"). A modeless dialog (WinDock.SettingsSearch)
// owned by the settings window it was opened from (Ctrl+F in any settings window, the Global nav
// column's Find a setting… box, the Dock window's ⋯ menu, DockCommand 151): a search box, a list of
// every setting (Setting / Where / Value) filtered as you type, a status line and Go to.
//
// The index covers the Global window's pages, every docks[] entry's Dock window (a dock, a
// standalone widget, a folder window; a window that is not open is created hidden for it and closed
// again when the last search dialog closes, unless the user went to it) and every widget / clock /
// weather item's generated page, read from its manifest or built-in schema and the config (no window;
// Go to opens the Item window). Values come from the live controls (the items': the config), so the
// Value column is current; the index is rebuilt when the dialog opens, when it becomes active again,
// after Go to and after a change made in it. Space on a check box / radio result changes it in place,
// on a drop-down result (or a click on its Value cell) opens a drop-down there.

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "DarkMode.h"
#include "SettingsWindow.h"

// What the dialog needs of the app (App::MakeSearchHost).
struct SettingsSearchHost {
    Config* config = nullptr;
    // The windows the index reads: the Global window and every docks[] entry's Dock window, in that
    // order; one that is not open is created hidden.
    std::function<std::vector<SettingsWindow*>()> windows;
    // Go to: opens (or activates, or shows a hidden one) the window as DockCommand 107 / 143 / 142
    // do, on page `page` (an Item window: its one page), and returns it (null: it could not).
    std::function<SettingsWindow*(SettingsKind kind, const std::wstring& dock, size_t item, int page)> open;
    // An item's Item window for a change in place: the open one, else one created hidden.
    std::function<SettingsWindow*(const std::wstring& dock, size_t item)> itemWindow;
    // The dialog's window is gone (posted: the app frees the object).
    std::function<void()> destroyed;
};

class SettingsSearch {
public:
    static constexpr wchar_t kClassName[] = L"WinDock.SettingsSearch";

    SettingsSearch(SettingsSearchHost host, SettingsWindow* owner);
    ~SettingsSearch();
    SettingsSearch(const SettingsSearch&) = delete;
    SettingsSearch& operator=(const SettingsSearch&) = delete;

    // Builds the index, creates the dialog centred over its owner with `text` in its box and shows it.
    bool Create(HINSTANCE instance, const std::wstring& text);
    // Brings the open dialog forward; a non-empty `text` replaces the box's text.
    void Activate(const std::wstring& text);
    HWND Hwnd() const { return hwnd_; }
    SettingsWindow* Owner() const { return owner_; }
    // IsDialogMessage for the dialog (Tab, Esc, Enter) and its keys (Down from the box, Space in
    // the list). True if handled.
    bool PreTranslateMessage(MSG& msg);
    // The rows shown now (QueryState 283).
    int ResultCount() const { return static_cast<int>(shown_.size()); }

private:
    // One indexed setting.
    struct Row {
        SettingsKind kind = SettingsKind::Global;  // the window it is in
        std::wstring dock;     // its dock id (Global: "")
        size_t item = SIZE_MAX;  // Item: the item's index in the dock's items
        int page = 0;
        int id = 0;            // the control Go to focuses
        CtlKind ctl = CtlKind::Label;
        std::wstring setting, where, value, tip;
        std::wstring label;    // `setting` lowercased (ranking)
        std::wstring hay;      // everything searched, lowercased
        std::vector<std::wstring> choices;  // a drop-down list's rows (empty: Go to only)
        int choice = -1;
        bool enabled = true;
        std::wstring Key() const;
    };

    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK ListProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);
    int Px(int dip) const { return MulDiv(dip, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI); }

    // The index (Rebuild keeps the query and the selected row).
    void Rebuild();
    void IndexItems(std::vector<Row>& rows);
    void Filter();
    void SetSelected(int row);  // a row of shown_ (-1 none)
    int Selected() const;
    void UpdateStatus();
    // Go to the selected row (or the first one).
    void GoTo();
    // Space / a click on the Value cell: toggle, choose, or the drop-down over the cell.
    void ChangeSelected();
    void ShowCombo(int shownIndex);
    void HideCombo(bool focusList);
    void ApplyChoice(int shownIndex, int choice);
    bool Change(const Row& r, int choice);
    // After Go to: moves the dialog off `target` (screen px) when it covers it.
    void MoveOff(const RECT& target, HWND window);

    void ApplyTheme();
    void CreateFonts();
    void Layout();
    void SetColumns();

    SettingsSearchHost host_;
    SettingsWindow* owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND edit_ = nullptr, list_ = nullptr, go_ = nullptr, status_ = nullptr, combo_ = nullptr;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    DarkMode::Palette palette_;
    HFONT font_ = nullptr;
    HBRUSH layerBrush_ = nullptr, controlBrush_ = nullptr;
    bool darkPopups_ = false;

    std::vector<Row> rows_;        // the index, in window / page / table order
    std::vector<size_t> shown_;    // rows_ indexes that match, ranked
    std::wstring query_;           // lowercased
    int comboRow_ = -1;            // the shown_ row the drop-down edits (-1: hidden)
    int pressRow_ = -1;            // the row under the last button press and whether it was selected
    bool pressWasSelected_ = false;
    HWND lastFocus_ = nullptr;
    bool rebuildPosted_ = false;
    static constexpr UINT kRebuildMessage = WM_APP + 41;  // posted: the index again (after a change / Go to)
    static constexpr int kWidthDip = 600, kHeightDip = 440;
    static constexpr int kMinWidthDip = 440, kMinHeightDip = 300;
};
