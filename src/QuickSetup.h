#pragma once

// Round 12 phase 12G: Quick setup (WinDock.QuickSetup), which replaces the welcome dialog: four
// steps in one window, every one skippable, everything a live Keep / Cancel preview on the real
// desktop (App starts the preview when it opens: the docks behind the window ARE the preview).
//   1 Your docks: up to three rows (name, layout, display, edge, Remove), "Add a dock".
//   2 Their looks: "Every dock uses the same look", the 12B gallery of looks, Quick look's
//     Transparency and Corners for the dock shown (or the app look).
//   3 How it behaves: Start WinDock when Windows starts and Hide the Windows taskbar (held here,
//     applied at Finish: they touch the registry and Explorer), the first dock's auto-hide, Animation.
//   4 Done: Finish (Keep: one undo entry "Quick setup") and Show me around (Finish, then the tips).
// Cancel, Esc and closing the window put everything back (App's Cancel of the preview).
//
// A new file rather than PresetDialog's welcome variant grown into a stepped host: the chooser keeps
// its one job (a list and a preview), and Quick setup's controls (rows, galleries, sliders) share
// nothing with it but the window boilerplate. 960 x 640 DIPs on a display wider than 1400 DIPs,
// else 760 x 540; the system light / dark palette as the other windows. It runs in the app's message
// loop (App::PreTranslateMessage hands it its keys), owned by the hidden app window like the welcome
// dialog was. Its edits go to App through QuickSetupHost::change; App answers with Refresh (posted:
// the controls are read again from the config, never inside one of their own notifications).

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "Config.h"
#include "DarkMode.h"
#include "Presets.h"
#include "SettingsPreview.h"

// One edit of Quick setup, applied by App (inside the preview).
struct QuickSetupChange {
    enum class Kind {
        Layout,      // dock `dock`: built-in layout `value` (Presets::All(); its pins kept); -1: as it was when Quick setup opened
        Rename,      // dock `dock`: name `text`
        Display,     // dock `dock`: docks[].monitor = `value`
        Edge,        // dock `dock`: edge `value` (DockEdge)
        AddDock,     // a Basic dock on the next free edge of the primary display: `text` gets its id
        RemoveDock,  // dock `dock` removed
        SameLook,    // `value` 1: the docks in `docks` use the app look (the first one's look becomes it); 0: each gets its own
        Look,        // look `value` (Presets::Looks()) for dock `dock` ("" the app look)
        Quick,       // Quick look `quick` (Theme::Quick) = `value` on dock `dock`'s look ("" the app look)
        AutoHide,    // the first dock's auto-hide = `value` (AutoHideMode)
        Animation,   // `value` 0 Full, 1 Reduced, 2 Off
    };
    Kind kind = Kind::Layout;
    std::wstring dock;
    std::vector<std::wstring> docks;
    int value = 0;
    int quick = 0;
    std::wstring text;
};

struct QuickSetupHost {
    const Config* config = nullptr;  // read for every control (App's live config)
    // An edit; false: refused (the control goes back on the next Refresh).
    std::function<bool(QuickSetupChange&)> change;
    // Finish: the two held choices written, the preview kept; `showTips`: Show me around.
    std::function<bool(bool startWithWindows, bool hideTaskbar, bool showTips)> finish;
    // Cancel: the preview's snapshot back. False: refused (a dock menu is open): the window stays.
    std::function<bool()> cancel;
    std::function<void()> destroyed;  // the window is gone (App frees the object later: posted)
};

class QuickSetup {
public:
    static constexpr wchar_t kClassName[] = L"WinDock.QuickSetup";
    static constexpr wchar_t kStepsClass[] = L"WinDock.QuickSetupSteps";
    static constexpr wchar_t kRowsClass[] = L"WinDock.QuickSetupRows";
    static constexpr int kMaxDocks = 3;
    static constexpr int kSteps = 4;
    // The row controls inside the rows container (SettingsIds::kSetupRows): id 10 * row + part.
    enum RowPart { kRowName = 1, kRowLayout = 2, kRowDisplay = 3, kRowEdge = 4, kRowRemove = 5 };

    // `firstRun`: the first start's variant ("Welcome to WinDock"; the first row's layout is Basic,
    // which App applied before the preview). `startWithWindows`: the Run key now.
    QuickSetup(QuickSetupHost host, bool firstRun, bool startWithWindows);
    ~QuickSetup();
    QuickSetup(const QuickSetup&) = delete;
    QuickSetup& operator=(const QuickSetup&) = delete;

    bool Create(HINSTANCE instance, HWND owner);
    HWND Hwnd() const { return hwnd_; }
    void Activate();
    bool PreTranslateMessage(MSG& msg);
    // QueryState 317 / 320.
    int Step() const { return step_; }
    int Rows() const { return static_cast<int>(rows_.size()); }
    bool FirstRun() const { return firstRun_; }
    int HeldChoices() const { return (start_ ? 1 : 0) | (hideTaskbar_ ? 2 : 0); }
    // Next (false on the last step), Finish (Show me around with `tips`), Cancel (false: refused).
    bool Next();
    bool Finish(bool tips);
    bool Cancel();
    // The config changed: the controls follow (posted, coalesced).
    void Refresh();

private:
    struct Row {
        std::wstring id;
        bool keepEntry = false;  // the layout combo starts with "As it is now" (a dock that was there before)
        int layoutSel = 0;       // the layout combo's selection
        HWND name = nullptr, layout = nullptr, display = nullptr, edge = nullptr, remove = nullptr;
    };
    static constexpr UINT kMsgRefresh = WM_APP + 1;    // posted to the window: Sync
    static constexpr UINT kMsgRemoveRow = WM_APP + 2;  // posted: row wParam's dock removed (outside its button's click)

    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK StepsProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK RowsProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);
    int Px(int dip) const { return MulDiv(dip, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI); }

    void CreateFonts();
    void ApplyTheme();
    void SetFonts();
    void Layout();
    void LayoutRows();
    void PaintSteps(HWND hwnd, HDC hdc);
    RECT StepRect(int step) const;  // in the step indicator's client px
    void GoTo(int step);
    void ShowStep();
    // Every control from the config (Refresh's work).
    void Sync();
    void CreateRow(size_t r);
    void DestroyRows();
    void RebuildRows();
    void FillRowCombos(size_t r);
    void SyncRow(size_t r);
    void CommitNames();
    void CommitName(size_t r);
    void OnRowCommand(int id, int code, HWND control);
    void RemoveRow(size_t r);
    void AddDock();
    // Step 2: the look the gallery / sliders of look row `r` edit ("" the app look), and the row shown.
    std::wstring LookTarget(size_t r) const;
    size_t LookShown() const;
    bool SameLook() const;
    void SyncLooks();
    void OnLookCommand(int id, int code);
    void OnAlpha(size_t r);
    void SyncBehaviour();
    std::wstring DoneHeading() const;
    const DockConfig* DockOf(const std::wstring& id) const;
    std::wstring NameOf(const std::wstring& id) const;
    bool Apply(QuickSetupChange& c);
    void Close();

    QuickSetupHost host_;
    bool firstRun_ = false;
    bool start_ = false, hideTaskbar_ = true;
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    int widthDip_ = 760, heightDip_ = 540;
    int step_ = 0;
    bool closing_ = false, done_ = false, refreshPosted_ = false, syncing_ = false;
    DarkMode::Palette palette_;
    HFONT font_ = nullptr, fontBold_ = nullptr, fontHeading_ = nullptr;
    HBRUSH layerBrush_ = nullptr, controlBrush_ = nullptr;
    std::vector<Presets::Look> looks_;
    // The frame.
    HWND steps_ = nullptr, heading_ = nullptr, intro_ = nullptr;
    HWND back_ = nullptr, next_ = nullptr, finish_ = nullptr, cancel_ = nullptr, showAround_ = nullptr;
    // Step 1.
    HWND rowsBox_ = nullptr, addDock_ = nullptr, addNote_ = nullptr, layoutNote_ = nullptr;
    HWND rowHeads_[4] = {};
    std::vector<Row> rows_;
    // Step 2.
    HWND same_ = nullptr, sameCaption_ = nullptr, lookDock_ = nullptr, alphaLabel_ = nullptr, cornersLabel_ = nullptr;
    SettingsGallery galleries_[kMaxDocks];
    HWND alpha_[kMaxDocks] = {}, corners_[kMaxDocks] = {};
    size_t lookShown_ = 0;  // the "Look of" combo's row (several docks, not the same look)
    // Step 3.
    HWND startBox_ = nullptr, startCaption_ = nullptr, taskbarBox_ = nullptr, taskbarCaption_ = nullptr, behaviourNote_ = nullptr;
    HWND autoHideLabel_ = nullptr, autoHide_ = nullptr, animationLabel_ = nullptr, animation_ = nullptr, animationNote_ = nullptr;
    // Step 4.
    HWND doneNote_ = nullptr;
};
