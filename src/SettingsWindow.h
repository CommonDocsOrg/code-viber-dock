#pragma once

#include <windows.h>
#include <commctrl.h>

#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#include "AppIdentity.h"
#include "Config.h"
#include "DarkMode.h"
#include "Weather.h"

// What a settings change affects; the host applies it to the dock, then saves (debounced).
enum class SettingsScope {
    Appearance,  // sizes, magnification, theme, clock, monitor: DockWindow::ApplyConfig
    AutoHide,    // config.Edit().autoHide: DockWindow::ApplyAutoHide
    Pins,        // the items of config.Edit() (Dock contents): DockWindow::ReloadPins
    Taskbar,     // config.hideTaskbar: TaskbarController::SetEnabled
    Autostart,   // config.startWithWindows: the HKCU Run value
    Weather,     // config.weather: DockWindow::ApplyWeather
    SaveOnly,    // read from the config when used (delays, renderer): just save
};

// Docks page edits of config.docks (SettingsHost::editDocks).
enum class SettingsDockEdit { Add, Duplicate, Remove };

// Everything the settings window may use of the app, so it never sees App internals. "The dock"
// below is config.Edit() (config.editDock, chosen on the Docks page): its first window.
struct SettingsHost {
    Config* config = nullptr;                              // &App::config_, edited in place (stable address)
    std::function<void(SettingsScope)> changed;
    std::function<const std::vector<AppIdentity::ResolvedPin>&()> pins;
    std::function<void(const std::vector<std::wstring>&)> pinFiles;  // App::PinFiles(paths, end)
    std::function<void(UINT)> command;                     // App::OnCommand (ContextMenu ids)
    std::function<bool()> usingWarp;
    std::function<RECT()> dockMonitor;
    std::function<UINT()> dockDpi;
    std::function<void(bool)> keepDockShown;               // DockWindow::SetKeepShown
    // Beyond the plan's interface:
    std::function<RECT()> dockBar;            // the dock's bar at rest (screen px): stay above it
    std::function<bool()> taskbarHidden;      // the taskbar controller is enabled (session state)
    std::function<bool()> autostartEnabled;   // the HKCU Run value exists
    std::function<void()> destroyed;          // the window is gone: delete the object (posted)
    // Weather page.
    std::function<std::wstring()> weatherStatus;  // WeatherService::StatusText of the dock's service
    // Starts a place search; `done` gets the places (or an error) later, on the UI thread, unless
    // a newer search replaced it or the window closed. False if it could not be started.
    std::function<bool(const std::wstring& query,
                       std::function<void(const std::vector<WeatherService::Place>&, const std::wstring& error)> done)>
        geocode;
    // Docks page. Adds a dock (index ignored), duplicates / removes docks[index] (never the last
    // one); the app re-syncs the dock windows and saves. False if refused.
    std::function<bool(SettingsDockEdit, size_t index)> editDocks;
    // The dock windows of docks[index] (0: parked, its monitor is not connected).
    std::function<int(size_t index)> dockWindows;
};

class SettingsWindow;
class SettingsPreview;

// What a control is: decides its font, colours and visual-style class.
enum class CtlKind {
    Title,     // page title (large semibold)
    Section,   // section header (semibold)
    Label,     // row label (left column)
    Note,      // secondary text
    Value,     // plain text in the control column
    Caption,   // owner-drawn clickable caption of a check box / radio button (forwards BM_CLICK)
    Button,
    Check,     // caption-less BS_AUTOCHECKBOX (its window text is the accessible name, clipped)
    Radio,     // caption-less BS_AUTORADIOBUTTON
    Edit,
    Spin,      // up-down attached to the preceding Edit
    Combo,
    Trackbar,
    Link,      // SysLink
    Swatch,    // owner-drawn push button (BS_OWNERDRAW): the page paints it in OnDrawItem
    List,      // SysListView32 (report view): dark items view theme, palette colours
};

// One row of a page's layout table. Rects are in DIPs, page coordinates (unscrolled); the window
// converts them on every layout pass (creation, DPI change, scrolling).
struct Ctl {
    int id = 0;
    CtlKind kind = CtlKind::Label;
    const wchar_t* cls = nullptr;
    std::wstring text;
    DWORD style = 0;
    DWORD exStyle = 0;
    RECT dip{};
    int buddy = 0;  // Check / Radio: caption id; Caption: its button; Edit: its Spin; Spin: its Edit
    int lo = 0, hi = 0, step = 1;  // Trackbar / Spin range; Edit: max text length (hi)
    HWND hwnd = nullptr;
};

// A page: a child window (WinDock.SettingsPage, WS_EX_CONTROLPARENT) holding the controls of its
// table. Subclasses (SettingsPages.cpp) build the table, load the config into the controls and
// handle notifications; the window does creation, fonts, colours, theming, layout and scrolling.
class SettingsPage {
public:
    SettingsPage(SettingsWindow& window, const wchar_t* name, wchar_t glyph);
    virtual ~SettingsPage() = default;
    SettingsPage(const SettingsPage&) = delete;
    SettingsPage& operator=(const SettingsPage&) = delete;

    const std::wstring& Name() const { return name_; }
    wchar_t Glyph() const { return glyph_; }  // Segoe Fluent Icons code point (0: none)
    HWND Hwnd() const { return hwnd_; }
    HWND Item(int id) const;
    const Ctl* Find(int id) const;
    const Ctl* Find(HWND hwnd) const;

protected:
    friend class SettingsWindow;
class SettingsPreview;

    // --- content (SettingsPages.cpp) ---
    // Fills the control table with the builders below. Called once, before the controls exist.
    virtual void Build() = 0;
    // Config -> controls. Runs under the window's loading guard (notifications are ignored).
    virtual void Load() = 0;
    // After the controls were created (once): fill lists that do not depend on the config.
    virtual void Created() {}
    virtual void OnCommand(int /*id*/, int /*code*/, HWND /*ctl*/) {}
    virtual LRESULT OnNotify(const NMHDR& /*nm*/) { return 0; }
    // WM_HSCROLL from a trackbar (TB_* code) or WM_VSCROLL from an up-down (SB_* code).
    virtual void OnScroll(int /*id*/, int /*code*/) {}
    // Enter in edit / editable combo `id`, or it lost the focus: commit its text.
    virtual void OnCommit(int /*id*/) {}
    virtual void OnDisplayChange() {}
    // WM_DRAWITEM for an owner-drawn button (CtlKind::Swatch). Return true if drawn.
    virtual bool OnDrawItem(const DRAWITEMSTRUCT& /*di*/) { return false; }
    // WM_SETTINGCHANGE reached the window (after its own handling).
    virtual void OnSettingChange(WPARAM /*wp*/, LPARAM /*lp*/) {}
    // The DPI, the fonts or the palette changed (after the window updated them): rebuild image
    // lists and other DPI / colour dependent resources of the page.
    virtual void OnStyleChanged() {}
    // The dock's weather data / status changed (SettingsWindow::WeatherChanged).
    virtual void OnWeatherChanged() {}

    // --- layout table builders (DIPs; the cursor y_ moves down row by row) ---
    static constexpr int kMargin = 24;
    static constexpr int kLabelW = 196;
    static constexpr int kColX = kMargin + kLabelW + 12;  // control column
    static constexpr int kRowH = 32;
    static constexpr int kRowGap = 2;
    static constexpr int kCaptionDx = 26;  // check box / radio -> its caption
    int Width() const;  // content width in DIPs
    Ctl& Add(int id, CtlKind kind, const wchar_t* cls, std::wstring text, DWORD style, RECT dip);
    void Title(const wchar_t* text);
    void Section(int id, const wchar_t* text);
    void Label(int id, const wchar_t* text);
    Ctl& Text(int id, CtlKind kind, std::wstring text, int x, int w, int h = kRowH);
    void Trackbar(int id, int x, int w, int lo, int hi, int page);
    void Spin(int editId, int spinId, int x, int w, int lo, int hi, int step);
    void Combo(int id, int x, int w, bool editable);
    void Check(int id, int captionId, const wchar_t* text, int x = kMargin);
    struct RadioDef {
        int id;
        int captionId;
        const wchar_t* text;
    };
    // Consecutive radio buttons, one row each, in one WS_GROUP. Their captions are created after
    // them so the group stays contiguous (auto-radio unchecking, arrow-key navigation).
    void Radios(std::initializer_list<RadioDef> radios, int x = kMargin);
    void Button(int id, const wchar_t* text, int x, int w);
    void Link(int id, std::wstring markup, int x, int w);
    void NextRow(int h = kRowH + kRowGap) { y_ += h; }
    int y_ = 0;

    // --- helpers ---
    Config& Cfg();
    SettingsHost& Host();
    SettingsWindow& Window() { return window_; }
    bool Loading() const;
    void Changed(SettingsScope scope);
    void SetText(int id, const std::wstring& text);
    std::wstring GetText(int id) const;
    void SetCheck(int id, bool checked);
    bool GetCheck(int id) const;
    void SetSpin(int spinId, int value);
    int GetSpin(int spinId) const;
    void SetTrack(int id, int pos);
    int GetTrack(int id) const;
    void Enable(int id, bool enable);
    void Show(int id, bool show);
    // The top-level settings window (owner for dialogs).
    HWND Owner() const;
    // Parses the edit's integer, clamps it to [lo, hi] and writes the clamped text back.
    // Returns false (the text is restored by the caller's Load) if it is not a number.
    bool ReadInt(int editId, int lo, int hi, int& value);

private:
    SettingsWindow& window_;
    std::wstring name_;
    wchar_t glyph_ = 0;
    HWND hwnd_ = nullptr;
    std::vector<Ctl> table_;
    int contentPx_ = 0;  // height of the content at the current DPI
    int scrollPx_ = 0;
};

// The settings window (WinDock.Settings): a fixed-size top-level window with a navigation column
// (WinDock.SettingsNav, Mica behind it where available) and one page visible at a time on a solid
// content layer. Nothing of it exists while it is closed: no timers, fonts, brushes or windows.
class SettingsWindow {
public:
    static constexpr wchar_t kClassName[] = L"WinDock.Settings";
    static constexpr wchar_t kNavClassName[] = L"WinDock.SettingsNav";
    static constexpr wchar_t kPageClassName[] = L"WinDock.SettingsPage";

    explicit SettingsWindow(SettingsHost host);
    ~SettingsWindow();
    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;

    // Creates and shows the window (centred on the dock's monitor, above the bar) and activates
    // it. Returns false if the window could not be created.
    bool Create(HINSTANCE instance);
    // Restores (if minimized) and activates the existing window.
    void Activate();
    HWND Hwnd() const { return hwnd_; }
    // Keyboard navigation (IsDialogMessage) for messages to this window or its children.
    // Returns true if the message was handled (do not translate / dispatch it).
    bool PreTranslateMessage(MSG& msg);
    // Reloads every control from *host.config (after tray toggles, Reload config).
    void Refresh();
    // Selects a page (SettingsIds::kPage*), as a click in the nav column would.
    void ShowPage(int index);
    // The dock's weather data / status changed: pages update what shows it (no config reload).
    void WeatherChanged();

    // --- for pages ---
    SettingsHost& Host() { return host_; }
    bool Loading() const { return loading_; }
    UINT Dpi() const { return dpi_; }
    int Px(int dip) const { return MulDiv(dip, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI); }
    const DarkMode::Palette& Colors() const { return palette_; }
    // A setting was applied to the dock: repaint the preview strip.
    void ConfigApplied();
    static constexpr int ContentWidthDip() { return kWidthDip - kNavWidthDip; }

private:
    friend class SettingsPage;

    static constexpr int kWidthDip = 760;     // client size
    static constexpr int kHeightDip = 560;
    static constexpr int kNavWidthDip = 200;
    static constexpr int kNavTopDip = 12;
    static constexpr int kNavItemDip = 36;
    static constexpr int kAboveBarDip = 24;   // gap between the window and the dock's bar
    // The dock preview strip (SettingsPreview) under every page, content-pane wide.
    static constexpr int kFooterDip = 96;
    // Set to false to drop the Mica nav column (frame extension) if a driver misbehaves: the
    // nav column is then painted solid, as when transparency effects are off.
    static constexpr bool kNavMica = true;
    static constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;

    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK NavProcThunk(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK PageProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);
    LRESULT NavProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT PageProc(SettingsPage& page, UINT msg, WPARAM wp, LPARAM lp);

    static bool RegisterClasses(HINSTANCE instance);
    RECT InitialRect() const;
    SIZE WindowSizeFor(UINT dpi) const;
    bool CreatePage(SettingsPage& page, int index);
    void CreateFonts();
    void DestroyFonts();
    void CreateBrushes();
    void DestroyBrushes();
    void UpdateIcons();
    void DestroyIcons();
    // Dark / light, high contrast, Mica, palette, control themes; repaints everything.
    void ApplyTheme();
    void ApplyBackdrop();
    void Layout();
    void LayoutPage(SettingsPage& page);
    void ScrollPage(SettingsPage& page, int px);
    void EnsureVisible(HWND control);
    void SelectPage(int index);
    int CheckGlyphPx(bool radio) const;
    SettingsPage* ActivePage() const;
    SettingsPage* PageOf(HWND control) const;
    void OnEnterKey();

    // Nav pane (SettingsNav.cpp)
    static bool FocusCuesHidden(HWND hwnd);
    RECT NavItemRect(int index) const;
    int NavHitTest(POINT pt) const;
    void PaintNav(HWND hwnd, HDC hdc);
    // Page colours
    LRESULT CtlColor(UINT msg, HDC hdc, HWND control, const SettingsPage& page);
    void DrawCaption(const SettingsPage& page, const DRAWITEMSTRUCT& di);
    // Dark mode trackbars (no dark visual style): channel and thumb in palette colours.
    LRESULT DrawTrackbar(NMCUSTOMDRAW& cd);
    void ThemeList(HWND list);
    static LRESULT CALLBACK ListSubclassProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    void StyleChanged();

    SettingsHost host_;
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND nav_ = nullptr;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    std::vector<std::unique_ptr<SettingsPage>> pages_;
    std::unique_ptr<SettingsPreview> preview_;  // the strip under the pages; freed with the window
    int active_ = 0;
    int navHot_ = -1;
    bool navTracking_ = false;
    HWND lastFocus_ = nullptr;
    bool loading_ = false;
    bool mica_ = false;        // Mica behind the caption and the nav column (frame extended)
    bool darkPopups_ = false;  // DarkMode::TryEnableDarkPopups succeeded (undone on close)

    DarkMode::Palette palette_;
    HFONT font_ = nullptr;         // message font
    HFONT fontSemibold_ = nullptr; // section headers
    HFONT fontTitle_ = nullptr;    // page titles
    HFONT fontIcons_ = nullptr;    // nav glyphs (Segoe Fluent Icons / MDL2 Assets); null if absent
    HBRUSH layerBrush_ = nullptr;
    HBRUSH baseBrush_ = nullptr;
    HBRUSH controlBrush_ = nullptr;
    HICON iconSmall_ = nullptr;
    HICON iconBig_ = nullptr;
};

// The pages in nav order (SettingsPages.cpp); index = SettingsIds::kPage*.
std::vector<std::unique_ptr<SettingsPage>> CreateSettingsPages(SettingsWindow& window);
