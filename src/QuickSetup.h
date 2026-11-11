#pragma once

// Round 12 phase 12G: Quick setup (WinDock.QuickSetup), which replaces the welcome dialog: four
// steps in one window, every one skippable, everything a live Keep / Cancel preview on the real
// desktop (App starts the preview when it opens: the docks behind the window ARE the preview).
//   1 Docks: up to three dock cards (name, layout, display, edge, remove), "Add a dock".
//   2 Look: the 12B gallery of looks, "Same look for every dock", Quick look's Transparency and
//     Corners for the dock shown (or the app look).
//   3 Behaviour: Start with Windows and Hide the Windows taskbar (held here, applied at Finish: they
//     touch the registry and Explorer), the first dock's auto-hide, Animation.
//   4 Review: a picture of the desktop and the summary, Finish (Keep: one undo entry "Quick setup")
//     and Show me around (Finish, then the tips).
// Cancel, Esc and closing the window put everything back (App's Cancel of the preview).
//
// Round 13 follow-up (2026-11-09, "the quick setup windows are kind of basic"): cards, hints and a
// painted summary over the same controls.
//
// Round 13 phase 13H (2026-11-10, the approved prototype "Quick setup redesign"): the layout of a
// modern setup assistant. A step RAIL on the left (the 12G step indicator, WinDock.QuickSetupSteps,
// now a column: number / check circles, the step's name and a live one-line caption of what is
// chosen; the app mark above, an info box below), the content column on the right (heading,
// sentence, body, a footer with Cancel on the left and Back / Next / Finish on the right). The
// edge, the corners, the auto-hide mode and the animation are segmented controls / tiles
// (QsUi::Choice, QuickSetupControls.h) instead of combos; the three check boxes are painted as
// toggle switches; the buttons are painted in the accent / ghost / dashed kinds. Every 12G control
// and id stays: a combo a choice replaces is kept, hidden, as the state the automation drives (its
// CBN_SELCHANGE runs the same handler, the choice follows on the next Sync), and the new visible
// controls have ids of their own (SettingsIds "Quick setup, round 13 phase 13H").
//
// A new file rather than PresetDialog's welcome variant grown into a stepped host: the chooser keeps
// its one job (a list and a preview), and Quick setup's controls (rows, galleries, sliders) share
// nothing with it but the window boilerplate. 960 x 640 DIPs on a display wider than 1400 DIPs,
// else 760 x 540 (a narrower rail, the dock cards without their monitor, step 3's tiles compact,
// the dock cards scroll); the system light / dark palette as the other windows. It runs in the
// app's message loop (App::PreTranslateMessage hands it its keys), owned by the hidden app window
// like the welcome dialog was. Its edits go to App through QuickSetupHost::change; App answers with
// Refresh (posted: the controls are read again from the config, never inside one of their own
// notifications).

#include <windows.h>
#include <commctrl.h>

#include <functional>
#include <string>
#include <vector>

#include "Config.h"
#include "DarkMode.h"
#include "Presets.h"
#include "QuickSetupControls.h"
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
    static constexpr int kSummaryMax = kMaxDocks + 5;  // step 4's lines: a dock each, look, start, taskbar, auto-hide, animation
    // The row controls inside the rows container (SettingsIds::kSetupRows): id 10 * row + part.
    // Round 13 phase 13H: part 6, the edge as a segmented control (part 4, the combo, stays hidden).
    enum RowPart { kRowName = 1, kRowLayout = 2, kRowDisplay = 3, kRowEdge = 4, kRowRemove = 5, kRowEdgeChoice = 6 };

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
        HWND edgeChoice = nullptr;  // round 13 phase 13H
    };
    struct SummaryLine {
        std::wstring label, value;
        int step = 0;  // the step its Edit link goes to
    };
    // Round 13 phase 13H: the window's geometry, DIPs (Layout fills it).
    struct Metrics {
        bool narrow = false;  // the 760 x 540 window
        int rail = 236, pad = 36, x0 = 0, x1 = 0, cw = 0;
        int buttonsTop = 0, footerLine = 0, bodyBottom = 0;
        int introH[kSteps] = {}, bodyTop[kSteps] = {};
    };
    static constexpr UINT kMsgRefresh = WM_APP + 1;    // posted to the window: Sync
    static constexpr UINT kMsgRemoveRow = WM_APP + 2;  // posted: row wParam's dock removed (outside its button's click)

    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK StepsProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK RowsProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);
    int Px(int dip) const { return MulDiv(dip, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI); }
    float Scale() const { return static_cast<float>(dpi_) / USER_DEFAULT_SCREEN_DPI; }
    RECT PxRect(const RECT& dip) const { return RECT{Px(dip.left), Px(dip.top), Px(dip.right), Px(dip.bottom)}; }

    void CreateFonts();
    void ApplyTheme();
    void SetFonts();
    void ApplyChoiceLooks();
    void Layout();
    void LayoutRows();
    void LayoutSummary();  // step 4's lines: each as tall as its value (up to two lines), Edit beside it
    // The height (DIPs) of `text` wrapped at `widthDip` in `font`.
    int TextHeight(HFONT font, const std::wstring& text, int widthDip) const;
    void PaintSteps(HWND hwnd, HDC hdc);
    RECT StepRect(int step) const;  // in the step rail's client px
    std::wstring StepSub(int step) const;  // the rail's caption under a step's name
    // The rows container: a card per dock; the window: the rail, the step's cards, the footer
    // hairline and step 4's preview and summary (all double-buffered; the children are clipped out).
    void PaintRows(HWND hwnd, HDC hdc);
    void PaintContent(HDC hdc);
    void PaintPreview(QsUi::Canvas& c);
    bool OnCard(HWND control) const;  // the control sits on one of the step's cards
    COLORREF BackOf(HWND control) const;  // what a control sits on (a card or the window)
    LRESULT OnCustomDraw(NMCUSTOMDRAW& cd);
    std::wstring LayoutName(size_t r) const;  // "Modern" / "As it is now"
    std::wstring LookName(size_t r) const;    // the look row `r` edits ("Glass")
    std::vector<SummaryLine> SummaryLines() const;
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
    // The rows container scrolls when its cards do not fit (the small window with three docks).
    void ScrollRowsTo(int px);
    void EnsureRowVisible(size_t r);
    // Step 2: the look the gallery / sliders of look row `r` edit ("" the app look), and the row shown.
    std::wstring LookTarget(size_t r) const;
    size_t LookShown() const;
    bool SameLook() const;
    void SyncLooks();
    void OnLookCommand(int id, int code);
    void OnAlpha(size_t r);
    void SyncBehaviour();
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
    QsUi::Colors colors_;
    HFONT font_ = nullptr, fontBold_ = nullptr, fontHeading_ = nullptr, fontSmall_ = nullptr, fontLabel_ = nullptr;
    HBRUSH baseBrush_ = nullptr, controlBrush_ = nullptr, cardBrush_ = nullptr;
    std::vector<Presets::Look> looks_;
    Metrics m_;
    struct Card {
        int step;            // shown on this step only
        RECT dip;            // in the window's client DIPs
        QsUi::Glyph icon = QsUi::Glyph::None;  // painted in an accent-tinted square at `iconDip`
        RECT iconDip{};
    };
    std::vector<Card> cards_;  // Layout fills it
    RECT infoDip_{};           // the rail's info box
    // The frame.
    HWND steps_ = nullptr, heading_ = nullptr, intro_ = nullptr;
    HWND back_ = nullptr, next_ = nullptr, finish_ = nullptr, cancel_ = nullptr, showAround_ = nullptr;
    int stepsHot_ = -1;
    bool stepsTracking_ = false;
    // Step 1.
    HWND rowsBox_ = nullptr, addDock_ = nullptr, addNote_ = nullptr, layoutNote_ = nullptr;
    std::vector<Row> rows_;
    int rowsScroll_ = 0;          // px the dock cards are scrolled by
    int rowsVisibleDip_ = 0, rowsContentDip_ = 0;
    bool layoutNoteFits_ = true;
    // Step 2.
    HWND same_ = nullptr, sameCaption_ = nullptr, lookDock_ = nullptr, alphaLabel_ = nullptr, cornersLabel_ = nullptr;
    HWND alphaValue_ = nullptr;  // round 13 phase 13H: "35 %" beside the Transparency label
    SettingsGallery galleries_[kMaxDocks];
    HWND alpha_[kMaxDocks] = {}, corners_[kMaxDocks] = {};
    HWND cornersChoice_[kMaxDocks] = {};  // round 13 phase 13H (corners_ stays, hidden)
    size_t lookShown_ = 0;  // the "Look of" combo's row (several docks, not the same look)
    // Step 3.
    HWND startBox_ = nullptr, startCaption_ = nullptr, taskbarBox_ = nullptr, taskbarCaption_ = nullptr, behaviourNote_ = nullptr;
    HWND autoHideLabel_ = nullptr, autoHide_ = nullptr, animationLabel_ = nullptr, animation_ = nullptr, animationNote_ = nullptr;
    HWND startHint_ = nullptr, taskbarHint_ = nullptr, autoHideHint_ = nullptr;  // one line under each choice
    HWND autoHideTiles_ = nullptr, animationChoice_ = nullptr;  // round 13 phase 13H (the combos stay, hidden)
    bool behaviourNoteFits_ = true, firstFloating_ = false;
    // Step 4.
    HWND doneNote_ = nullptr;
    HWND edits_[kSummaryMax] = {};  // round 13 phase 13H: a summary line's "Edit"
    int editStep_[kSummaryMax] = {};
    RECT previewDip_{}, captionDip_{}, summaryDip_{};
    std::vector<int> lineTops_;  // DIPs: line k from lineTops_[k] to lineTops_[k + 1] (LayoutSummary)
    bool twoLine_ = true;        // the label over its value (false: a short window, both on one line)
};
