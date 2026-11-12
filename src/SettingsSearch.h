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
    // The windows the index reads, by number: 0 the Global window, 1 + k docks[k]'s Dock window.
    // Round 18 phase 18D: one at a time (the dialog indexes one per posted message, so creating
    // the hidden ones never freezes it); `window(i)` creates a window that is not open hidden
    // (null: it could not be).
    std::function<size_t()> windowCount;
    std::function<SettingsWindow*(size_t i)> window;
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
    // Round 18 phase 18D: the index is still being built (QueryState 338 = 0 while it is).
    bool Indexing() const { return indexing_; }

private:
    // One indexed setting.
    struct Row {
        SettingsKind kind = SettingsKind::Global;  // the window it is in
        size_t window = SIZE_MAX;  // the host's window number (Item rows: SIZE_MAX)
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

    // The index (Rebuild keeps the query and the selected row). Round 18 phase 18D: Rebuild only
    // starts it; IndexStep (one per posted kIndexStepMessage) reads one window, creating it hidden
    // when it is not open, then the items' fields, and publishes what it has so far, so the dialog
    // shows at once and the list fills while the user types. A Rebuild while one runs starts over
    // (the windows exist by then: cheap).
    void Rebuild();
    void IndexStep();
    void IndexWindow(SettingsWindow& w, size_t number, std::vector<Row>& rows);
    void IndexItems(std::vector<Row>& rows);
    void FinishIndex();
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
    // Round 18 phase 18D: the build in progress.
    bool indexing_ = false;
    bool indexPartial_ = false;      // the list shows the rows read so far (the first build; a rebuild keeps the old rows until it is done)
    size_t indexStep_ = 0;           // the next window number to read
    size_t indexCount_ = 0;          // the windows to read
    std::vector<Row> indexRows_;     // the rows read so far
    std::wstring indexKeep_;         // the selected row's key, selected again when done
    static constexpr UINT kRebuildMessage = WM_APP + 41;  // posted: the index again (after a change / Go to)
    // The next index step (18D): a one-shot timer, not a posted message. Windows dispatches posted
    // messages before input and paint, so a chain of posted steps kept the dialog unpainted (white
    // controls) and deaf until the index was done; WM_TIMER comes only when input and paint are
    // done, so between two steps the dialog paints and takes keys. (WM_APP + 42 is SettingsSearch.cpp's.)
    static constexpr UINT_PTR kIndexTimerId = 1;
    static constexpr int kWidthDip = 600, kHeightDip = 440;
    static constexpr int kMinWidthDip = 440, kMinHeightDip = 300;
};
