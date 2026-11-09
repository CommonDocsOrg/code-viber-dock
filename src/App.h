#pragma once

#include <windows.h>
#include <shellapi.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Config.h"
#include "ContextMenu.h"
#include "DockManager.h"
#include "DockWindow.h"
#include "DrawerPopup.h"  // round 12 phase 12D
#include "EditBar.h"      // round 12 phase 12D
#include "HelpWindow.h"
#include "KeepBanner.h"
#include "PresetDialog.h"
#include "CoachMarks.h"   // round 12 phase 12G
#include "QuickSetup.h"   // round 12 phase 12G
#include "SettingsSearch.h"
#include "SettingsWindow.h"
#include "TaskbarController.h"
#include "WidgetConsole.h"

// Names shared between the running instance and command-line invocations.
namespace AppIpc {
inline constexpr wchar_t kMutexName[] = L"Local\\WinDock.SingleInstance.6b1f0c2e";
inline constexpr wchar_t kQuitMessage[] = L"WinDock.Quit";
inline constexpr wchar_t kRestoreTaskbarMessage[] = L"WinDock.RestoreTaskbar";
// The app's own hidden top-level window (tray icon, shared services, broadcasts). The automation
// messages below may be sent to it as well as to any dock window.
inline constexpr wchar_t kAppWindowClass[] = L"WinDock.App";
// Automation, every dock-level message: the dock is selected by the HIGH word of lParam (bits
// 16-31; 0 = the first dock, so lParam values below 0x10000 keep their old meaning) and the low
// word carries what lParam carried before (an item index ...). Docks are numbered in config order,
// a monitor "each" dock once per monitor (QueryState 47 = the count). It does not matter which
// dock window (or the app window) receives the message.
//
// SendMessage(dock, RegisterWindowMessage(kItemCommandMessage), cmd, target) runs a per-item menu
// command (ContextMenu::kTogglePin / kNewWindow / kOpenFileLocation / kOpenInExplorer /
// kChooseIcon / kResetIcon / kToggleFolderPopup / kEmptyRecycleBin / kChooseImage /
// kCloseAllWindows) on the item at index LOWORD(target) of dock HIWORD(target), or on the item
// owning window `target` when it is a top-level window one of the docks shows (the first dock
// showing it). Returns 1 if the item was found. The two pickers are modal: a sent kChooseIcon /
// kChooseImage is posted back and runs after the call returned.
inline constexpr wchar_t kItemCommandMessage[] = L"WinDock.ItemCommand";
// Automation: SendMessage(dock, RegisterWindowMessage(kDockCommandMessage), cmd, dock << 16) runs a
// dock / tray menu command (ContextMenu::kAutoHide ... kLastDockCommand, kOpenSettings included)
// for that dock, exactly as if it was chosen from its menu (a greyed one does nothing: Add
// separator / Add widget while the dock is locked). Returns 1 for a known command and dock.
inline constexpr wchar_t kDockCommandMessage[] = L"WinDock.DockCommand";
// Posted by `WinDock.exe --settings` to the running dock: open (or activate) the settings window.
inline constexpr wchar_t kOpenSettingsMessage[] = L"WinDock.OpenSettings";
// Round 9 phase 9C: posted by `WinDock.exe --welcome` to the running dock: the welcome dialog
// (round 12 phase 12G: Quick setup).
inline constexpr wchar_t kWelcomeMessage[] = L"WinDock.Welcome";
}  // namespace AppIpc

// Wires the modules together: config, the docks (DockManager), tray icon, taskbar controller,
// settings window. Owns a hidden top-level window for the tray icon, the shared services' timers
// and messages, and the system broadcasts (display changes, TaskbarCreated, end of session).
class App {
public:
    App() = default;
    App(const App&) = delete;
    App& operator=(const App&) = delete;
    ~App();

    // Returns false (after telling the user) if the dock can't start.
    bool Initialize(HINSTANCE instance);
    void Shutdown();
    // Round 5 phase B: three kinds of settings window (SettingsKind), any number open at once.
    // Opens the Global settings window (shown as "App settings" since round 10 phase 10A), or
    // activates it if it is open; `page` >= 0 selects that page (SettingsIds::kPage*). DockCommand
    // 107, the tray / dock menu's App settings…, --settings.
    void OpenSettings(int page = -1);
    // The settings window of docks[`dock`] (a dock, or a folder window entry), or activates it;
    // `tab` >= 0 selects that tab (SettingsIds::kTab*). The dock menu's Dock settings…, DockCommand 143.
    void OpenDockSettings(size_t dock, int tab = -1);
    // The settings window of item `item` of docks[`dock`] (a widget, clock, weather, folder or
    // group item; another kind opens the dock's window), or activates it. The one way in: the item
    // menu's Settings…, Contents > Settings…, the weather widget's map pin, weather.openSettings(),
    // DockCommand 113 / 142, ItemCommand 854.
    void OpenItemSettings(size_t dock, size_t item);
    // Round 9 phase 9C: the welcome dialog (the preset chooser's welcome variant for the first dock;
    // posted: it opens once the message loop runs). The first run, `--welcome`, About.
    // Round 12 phase 12G: Quick setup (its first-run variant on the first run).
    void OpenWelcome();
    // Called by the message loop for every message: keyboard navigation in the settings windows.
    // Returns true if the message was handled (do not translate / dispatch it).
    bool PreTranslateMessage(MSG& msg);

private:
    static constexpr UINT kTrayCallback = WM_APP + 1;
    static constexpr UINT kTrayId = 1;
    static constexpr UINT kSettingsClosed = WM_APP + 2;  // posted: delete the destroyed settings windows
    static constexpr UINT kDialogActivated = WM_APP + 3;  // posted (wParam = window): ChooseItemIcon's focus fix
    static constexpr UINT kDisplaySync = WM_APP + 4;      // posted: re-enumerate the monitors (coalesced)
    static constexpr UINT kConsoleClosed = WM_APP + 6;    // posted: delete the destroyed widget Console (WM_APP + 5: kReapMessage)
    static constexpr UINT kSearchClosed = WM_APP + 8;     // posted: delete the closed search dialogs (WM_APP + 7: kRemoveFolderMessage)
    static constexpr UINT kPresetClosed = WM_APP + 9;     // round 9 phase 9C: posted: delete the closed preset chooser
    static constexpr UINT kShowWelcome = WM_APP + 12;     // posted: open the welcome dialog
    static constexpr UINT kHelpClosed = WM_APP + 13;      // round 9 phase 9D: posted: delete the closed Help window
    static constexpr UINT kTimelineCommit = WM_APP + 14;  // round 12 phase 12A: posted (coalesced): record the gesture's change
    static constexpr UINT_PTR kSaveTimerId = 5;  // debounced config save (one-shot)
    static constexpr UINT kSaveDelayMs = 300;
    // Round 12 phase 12A: the Keep / Revert bar's countdown (1 s steps, only while the bar shows;
    // 40 - 43 are CalendarService's / DockManager's on the same window).
    static constexpr UINT_PTR kKeepTimerId = 44;
    static constexpr int kKeepSeconds = 12;
    static constexpr size_t kTimelineMax = 50;

    static LRESULT CALLBACK AppWndProc(HWND, UINT, WPARAM, LPARAM);
    // The app window's messages.
    LRESULT OnAppMessage(UINT msg, WPARAM wp, LPARAM lp);
    // Every dock window's messages, before the dock handles them (true: consumed).
    bool OnDockMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT& result);
    // The registered messages both kinds of window answer (automation, IPC). True if handled.
    bool OnSharedMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result);
    LRESULT QueryState(WPARAM wp, LPARAM lp);
    // A new dock window: its pin requests and callbacks.
    void SetUpDock(DockWindow& dock);

    void AddTrayIcon();
    void RemoveTrayIcon();
    // Round 10 phase 10A: three menus. The dock menu of `dock` (a right click on its background),
    // the notification-area icon's app menu, and an item's menu (any slot, the widget kinds
    // included; its shared dock commands in a Dock submenu).
    void ShowMenu(DockWindow* dock, POINT screenPt);
    void ShowAppMenu(POINT screenPt);
    void ShowItemMenu(DockWindow& dock, int index, POINT screenPt);
    // Runs a per-item command on the item with `key` of `dock` (looked up again: the item list may
    // have changed while a menu was open). `windows` are the windows the menu listed.
    void RunItemCommand(DockWindow& dock, UINT cmd, const AppKey& key, const std::vector<HWND>& windows);
    void TogglePin(DockWindow& dock, const AppKey& key);
    // Sets (an empty `icon`: clears) the `icon` of every pinned entry of the dock's config that
    // resolves to `key` (apps, folders, the Recycle Bin; not separators), then reloads the pins,
    // saves and refreshes the settings window. Returns false if no such pin exists.
    bool SetPinIcon(DockWindow& dock, const AppKey& key, const std::wstring& icon);
    // Sets the `view` of every folder / group entry of the dock's config that resolves to `key`,
    // then reloads, saves and refreshes the settings window.
    void SetItemView(DockWindow& dock, const AppKey& key, ItemView view);
    // --- shortcut groups (the dock's PinRequests, the item menu, automation) ---
    // The config entry of the pin `key` of `dock` and its pin index (null: none).
    DockItemEntry* PinEntryOf(DockWindow& dock, const AppKey& key, size_t* pinIndex = nullptr);
    // `source` (a pin or a running app) onto the pin `target`: a group of the two at the target's
    // place, or `source` added to `target` when that is a group. A pinned source leaves the pins.
    void GroupWith(DockWindow& dock, const AppKey& source, const AppKey& target);
    void AddToGroup(DockWindow& dock, const AppKey& group, const std::vector<std::wstring>& paths);
    // Member -> a pin at pin index `at` (SIZE_MAX: right after the group). The last member out
    // removes the group (as does removing it).
    void MemberOut(DockWindow& dock, const AppKey& group, size_t member, size_t at);
    void RemoveMember(DockWindow& dock, const AppKey& group, size_t member);
    void Ungroup(DockWindow& dock, const AppKey& group);
    // Rename…: the dialog (owned by the dock, held shown), then the new name.
    void RenameGroup(DockWindow& dock, const AppKey& group);
    // The dock's items changed: reload its windows, save, refresh the settings window.
    void ItemsChanged(size_t cfg);
    // kChooseIcon / kChooseImage: runs the picker (owned by the dock, with the dock held shown)
    // for the pin `key`, then SetPinIcon.
    void ChooseItemIcon(DockWindow& dock, UINT cmd, const AppKey& key);
    // The config entry that pins this item (nullopt: nothing launchable to pin).
    static std::optional<DockItemEntry> EntryForItem(const DockItem& item);
    // Reorders the pins of the dock's config to the given key order (drag to reorder); a key that
    // is not pinned yet (a running item dragged into the pinned section) is pinned at its position.
    // Round 7 phase 7B: `anchor` (when set) goes to the entries of `key` first (the items are then
    // sorted into group order, as ReloadPins always does).
    void ApplyPinOrder(DockWindow& dock, const std::vector<AppKey>& order, const AppKey& key = {},
                       std::optional<ItemAnchor> anchor = std::nullopt);
    // Inserts .lnk / .exe files (and folders) so the first becomes pin `at` of config dock `cfg`
    // (past the last pin: after it), skipping apps that are already pinned (`pins`: its resolved pins).
    void PinFiles(size_t cfg, const std::vector<AppIdentity::ResolvedPin>& pins, const std::vector<std::wstring>& paths,
                  size_t at);
    // Moves config item `from` of config dock `cfg` right before item `to` (a widget dragged on the
    // dock), then reloads, saves and refreshes the settings window.
    // `anchor` (round 7 phase 7B, when set) goes to the moved item, also when the order stays.
    void MoveItem(size_t cfg, size_t from, size_t to, std::optional<ItemAnchor> anchor = std::nullopt);
    // Round 7 phase 7B (ItemCommand 859 / 860): config item `item` of dock `cfg` gets an anchor / an
    // offset (DIPs, clamped); the items are sorted into group order, reloaded, saved.
    void SetItemAnchor(size_t cfg, size_t item, ItemAnchor anchor);
    void SetItemOffset(size_t cfg, size_t item, int offset);
    // Add widget > Clock / Weather / Running apps (DockConfig::AddWidget's placement).
    void AddWidget(size_t cfg, ItemKind kind);
    // --- the tray (8b): edits of config dock `cfg` (each reloads, saves, refreshes Settings) ---
    // Icon `key` moves right before icon `before` in the grid of tray item `item` (empty: to the
    // end; the whole grid order is written to the item's `order`) and is no longer hidden.
    void TrayOrder(size_t cfg, size_t item, const std::wstring& key, const std::wstring& before);
    // Icon `key` becomes a trayApp item at pin index `at` (SIZE_MAX: after the last pin); one per icon.
    void PinTray(size_t cfg, const std::wstring& key, size_t at);
    void UnpinTray(size_t cfg, const std::wstring& key);
    void TrayHide(size_t cfg, size_t item, const std::wstring& key, bool hide);
    // Every window of config dock `cfg` re-reads its items (DockWindow::ReloadPins), after they are
    // sorted into group order (round 7 phase 7B: an item added anywhere joins its group).
    void ReloadPins(size_t cfg);
    void SeedDefaultPins();
    static int IndexOfKey(const DockWindow& dock, const AppKey& key);
    void RequestSave();
    void FlushSave();
    // A menu / dock command for config dock `cfg` (`dock`: the window it came from; may be null).
    void OnCommand(UINT cmd, size_t cfg, DockWindow* dock);
    // taskbar_.SetEnabled with the docks' app bars withdrawn around it (see DockWindow::AllowAppBar).
    void SetTaskbarEnabled(bool enabled);
    // Sets docks[cfg].autoHide, applies it to that dock's windows and saves; remembers on-modes for kAutoHide.
    void SetAutoHide(size_t cfg, AutoHideMode mode);
    ContextMenu::TrayMenuState MenuState(size_t cfg) const;
    // Runs a modal menu / dialog with `dock` held shown (no auto-hide while it is open). Display
    // changes wait until it is over (the dock windows must not go away under it).
    UINT RunMenu(DockWindow* dock, const std::function<UINT()>& show);
    void EndModal();
    // Display / monitor changes: re-sync the docks with the monitors (deferred while a menu or
    // dialog is open).
    void ScheduleDisplaySync();
    // DockManager::Sync, then the reserved work areas and the settings window follow.
    void SyncDocks();
    void UpdateReservedMonitors();
    void ReloadConfig();
    void OpenConfigFile();
    // --- the dashboard hotkey (round 6 phase 6A) ---
    // Registers config.dashboard.hotkey on the app window (RegisterHotKey, id kDashboardHotkeyId),
    // replacing the one registered before; "" registers none. A combination another app holds
    // fails: logged, hotkeyRegistered_ false (Global > General shows it). Skipped when the same
    // text is registered already.
    void ApplyHotkey();
    void RemoveHotkey();
    static constexpr int kDashboardHotkeyId = 1;
    // The widget Console (DockCommand 127, Settings > Widgets): opens or activates it.
    void OpenConsole();
    // A setting was changed in settings window `source` about config dock `cfg`: apply it to the
    // docks, save (debounced), then the other settings windows follow (SettingsChangedElsewhere).
    void OnSettingsChanged(SettingsScope scope, size_t cfg, SettingsWindow* source);
    // --- the settings windows (round 5 phase B) ---
    // The host of window `w`: every callback bound to that window (its dock by id each time).
    SettingsHost MakeSettingsHost(SettingsWindow* w);
    // Creates, shows and keeps `w` (false: it could not be created). `show` false (round 9 phase
    // 9B): created hidden for a settings search (searchHidden_).
    bool AddSettingsWindow(std::unique_ptr<SettingsWindow> w, bool show = true);
    // --- the settings search (round 9 phase 9B) ---
    // Opens (or brings forward, with `text` typed) the search dialog owned by settings window `owner`.
    void OpenSearch(SettingsWindow* owner, const std::wstring& text);
    SettingsSearchHost MakeSearchHost();
    // A settings window created hidden for the search's index (kept in searchHidden_ until the last
    // search dialog closes, unless an Open* path shows it first).
    SettingsWindow* CreateHiddenSettings(SettingsKind kind, const std::wstring& dock, size_t item = SIZE_MAX);
    bool IsSearchHidden(const SettingsWindow* w) const;
    // An Open* path found `w`: it is a window of the user's from now on (shown by Activate).
    void ClaimSettings(SettingsWindow* w);
    // The open search dialog owned by `w` (null: none).
    SettingsSearch* SearchOf(const SettingsWindow* w) const;
    // An open window of that kind (Dock: of dock id `dock`; Item: of (dock, item)); null: none.
    SettingsWindow* FindSettings(SettingsKind kind, const std::wstring& dock = {}, size_t item = SIZE_MAX) const;
    int CountSettings(SettingsKind kind) const;
    size_t DockIndexOf(const std::wstring& id) const;
    // After the config changed: Dock / Item windows whose dock is gone close, Item windows find
    // their item again (same index and equal entry, else the equal entry elsewhere, else (the
    // same number of items) the same index with the same identity: an edit in place; else they
    // close), the Global window's Docks and displays row falls back to the first dock.
    void SyncSettingsTargets();
    // A window became (in)active: the dock of the active Dock / Item window is kept shown.
    void OnSettingsActivated(SettingsWindow* w, bool active);
    void UpdateKeepShown();
    bool KeepShownFor(size_t cfg) const;
    // Flash this dock: every window of config dock `cfg` (DockWindow::Flash in its colour).
    void FlashDock(size_t cfg);
    // An item kind with an Item window (widget, clock, weather, tray (round 5 phase D), folder,
    // group, an app that resolves to a folder).
    bool HasItemWindow(size_t cfg, size_t item);
    // Docks page: add / duplicate / remove a config dock (refused while a menu or dialog is open).
    bool EditDocks(SettingsDockEdit edit, size_t index);
    // --- dock presets (round 9 phase 9C) ---
    // Opens the chooser (or brings the open one forward): Apply to config dock `cfg`, a new dock, or
    // the welcome variant (for the first dock). `owner`: the settings window it is modal to (null:
    // the app window).
    // Round 12 phase 12C: `kind` the kind it shows first (Layouts / Looks / Mine); AppLook: Looks for
    // the app look (`cfg` ignored).
    void OpenPresets(PresetMode mode, size_t cfg, HWND owner, PresetKind kind = PresetKind::Layouts);
    // Round 9 phase 9D: the Help window (one per process) at `topic` ("guide-settings.md#auto-hide";
    // created when closed, near `nearWindow` when that is a shown window).
    void OpenHelp(const std::wstring& topic, HWND nearWindow = nullptr);
    // QueryState 285 for one settings window (made hidden for the count when it is not open).
    LRESULT CountUnexplained(LPARAM lp);
    // Preset `index` (Presets::All()) applied to config dock `cfg` (keeping its pins at the marker
    // when `keepPins`): saved, every window refreshed, the dock flashed. False if refused.
    // Round 12 phase 12A: `preview` makes it a Keep / Cancel preview (nothing saved until Keep;
    // `host`, the settings window it was chosen from, cancels it when it closes).
    // Round 12 phase 12C: `keepLook`: the dock keeps its look (the preset's is not applied).
    bool ApplyPreset(size_t index, size_t cfg, bool keepPins, bool preview = false, HWND host = nullptr, bool keepLook = false);
    // A new dock from preset `index` (the first dock's pins at the marker when `copyPins`); its
    // config index (SIZE_MAX: refused). `preview` / `host` as ApplyPreset.
    size_t AddPresetDock(size_t index, bool copyPins, bool preview = false, HWND host = nullptr);

    // --- round 12 phase 12A: one timeline (undo / redo), the save state, Keep / Revert, Keep / Cancel ---
    // Every change of the config is recorded: RequestSave posts kTimelineCommit, which compares the
    // config's text (Config::ToJson without the settings windows' sizes: the snapshot) with the one
    // recorded last and pushes {label, the state before} when it differs. Everything changed in one
    // message (one gesture: a slider's release, a drag, a preset) is one entry. Undo / Redo apply a
    // snapshot through the Reload config path (ApplySnapshot). Per process, at most 50 entries.
    struct TimelineEntry {
        std::wstring label;
        std::wstring state;    // the config's text before (undo) / after (redo) the change
        uint64_t serial = 0;   // when it was recorded (Revert this window counts the newer ones)
    };
    // What decides whether a change moved or hid a dock (Keep / Revert), per dock id.
    struct Placement {
        std::wstring id;
        DockEdge edge = DockEdge::Bottom;
        bool floating = false;
        int monitor = 0;
        AutoHideMode autoHide = AutoHideMode::Off;
        DockLayer layer = DockLayer::Normal;
        bool enabled = true;
    };
    std::wstring SnapshotText() const;
    static std::vector<Placement> PlacementOf(const Config& c);
    // "Dock moved to the left edge." for the first dock whose placement differs ("" none; `cfg` its index).
    std::wstring DescribePlacement(const std::vector<Placement>& before, const std::vector<Placement>& after, size_t& cfg) const;
    // The label of the change being made (the first one of a gesture wins).
    void NoteLabel(const std::wstring& label);
    void ScheduleCommit();
    // Records the change made since the last entry (posted; also run before anything reads the
    // timeline). `allowBar` false: no Keep / Revert for it (Revert, Keep of a preview, Reload).
    void CommitTimeline(bool allowBar = true);
    void FlushTimeline() {
        if (commitPosted_) CommitTimeline();
    }
    // A snapshot becomes the config (the Reload config path: Sync, ApplyConfig, ReloadPins, the
    // taskbar / Run key only when they differ, every settings window refreshed) and is saved. It is
    // the timeline's current state afterwards (not an entry). False: refused (a menu / dialog open).
    bool ApplySnapshot(const std::wstring& state);
    bool Undo();
    bool Redo();
    // Settings window `w`'s Revert this window: the state when it opened, as one entry.
    bool RevertWindow(SettingsWindow* w);
    // Starts (or extends) a Keep / Cancel preview: the save is held, changes make no entries.
    void StartPreview(const std::wstring& label, const std::wstring& text, HWND host, size_t cfg);
    // Round 12 phase 12B: DockCommand 162 Save as theme, 163 Import theme, 164 Apply to docks.
    bool LookCommand(UINT command, LPARAM lp);
    bool KeepBar();     // the bar's Keep (DockCommand 160)
    bool CancelBar();   // its Revert / Cancel (DockCommand 161; Esc; the preview's window closed)
    void CloseKeepRevert();
    bool RetrySave();
    void SaveNow();     // the save itself (the timer, FlushSave, Retry): its result is the save state
    int SaveStatus() const;  // QueryState 298: 0 saved, 1 pending, 2 failed
    int BarKind() const;     // QueryState 300: 0 none, 1 Keep / Revert, 2 Keep / Cancel, 3 a failed save
    std::wstring SaveErrorText() const;
    SettingsStatus StatusFor(const SettingsWindow* w) const;
    bool TimelineAction(SettingsWindow* w, SettingsAction action);
    // The settings windows' bands and bars, the app window's label statics, the banner.
    void UpdateTimelineUi();
    void OpenConfigFolder();
    void OnBannerCommand(int id);
    // Names for labels: an item of config dock `cfg` ("Clock", "Paint", "a separator").
    std::wstring EntryName(size_t cfg, size_t item) const;

    std::vector<TimelineEntry> undo_, redo_;
    std::wstring lastState_;                // the config's text as of the last entry
    std::vector<Placement> lastPlacement_;  // ... its docks' placements
    std::wstring pendingLabel_;
    bool commitPosted_ = false;
    bool timelineReady_ = false;            // Initialize recorded the starting state
    uint64_t nextSerial_ = 1;
    DWORD saveError_ = 0;                   // the last save's Win32 error (0: it worked)
    bool keepRevert_ = false;               // the Keep / Revert bar is up
    std::wstring keepText_, keepLabel_, keepState_;
    int keepSeconds_ = 0;
    size_t barDock_ = SIZE_MAX;             // the dock the bar is about (the banner's place)
    bool previewing_ = false;               // a Keep / Cancel preview is open
    bool saveHeld_ = false;                 // ... and a change waits for its Keep
    std::wstring previewBefore_, previewLabel_, previewText_;
    HWND previewHost_ = nullptr;            // the settings window it was chosen from (closing it cancels)
    struct WindowMark {
        SettingsWindow* window = nullptr;
        uint64_t serial = 0;  // the newest entry when it opened
        std::wstring state;   // the config then
    };
    std::vector<WindowMark> marks_;
    HWND undoLabel_ = nullptr;  // the app window's hidden statics (SettingsIds::kAppUndoLabel / kAppRedoLabel)
    HWND redoLabel_ = nullptr;
    KeepBanner banner_{[this](int id) { OnBannerCommand(id); }};

    // --- round 12 phase 12C: docks you keep (names, duplicate, saved layouts, looks) ---
    // Layout `p` (a built-in preset or a saved layout) applied to config dock `cfg` / as a new dock;
    // ApplyPreset / AddPresetDock are these for Presets::All()[index]. `label` is the undo label;
    // `from` ends AddLayoutDock's ("Added dock work from layout Work").
    bool ApplyLayoutTo(const Presets::Preset& p, const std::wstring& label, size_t cfg, bool keepPins, bool keepLook, bool preview,
                       HWND host);
    size_t AddLayoutDock(const Presets::Preset& p, const std::wstring& from, bool copyPins, bool preview, HWND host);
    // Look `look` (Presets::Looks()) as config dock `cfg`'s own look (SIZE_MAX: the app look),
    // nothing else changed. `preview`: Keep / Cancel. False if refused.
    bool ApplyLook(size_t look, size_t cfg, bool preview, HWND host);
    // docks[cfg].name = `name` (trimmed; the id or "" clears it), labelled "Renamed main to Work".
    bool RenameDock(size_t cfg, const std::wstring& name);
    // Opens the dock's Dock window with its header name being edited (the dock menu's Rename dock…).
    bool BeginRenameDock(size_t cfg);
    // A copy of docks[cfg] ("<name> copy", the next palette colour, the next free edge of its
    // display, enabled), appended; its config index (SIZE_MAX: refused).
    size_t DuplicateDock(size_t cfg);
    // Save dock as layout…: the name dialog owned by `owner` (a dock window: held shown; null: the
    // dock's Dock window or the App settings window when open), then SaveDockLayout (asks before
    // replacing a layout of that name).
    void AskSaveLayout(size_t cfg, HWND owner);
    unsigned long SaveDockLayout(size_t cfg, const std::wstring& name, bool overwrite);
    // DockCommand 165 - 169 (automation).
    bool KeepCommand(UINT command, LPARAM lp);
    HWND nameText_ = nullptr;  // the app window's hidden static SettingsIds::kAppNameText (QueryState 303)
    HWND layoutQueried_ = nullptr;  // round 12 phase 12F: the settings window QueryState 321 last answered for (322 asks it)
    // --- end round 12 phase 12C ---
    // --- desktop folder windows (round 4 phase 12) ---
    // "New folder window…": a folder picker owned by `owner`, then AddFolderWindow at the pointer.
    void NewFolderWindow(HWND owner);
    // Appends a folder window for `path` with its top-left at screen point `at` (on that monitor),
    // syncs and saves. Returns its automation index (SIZE_MAX: refused).
    size_t AddFolderWindow(const std::wstring& path, POINT at);
    // Removes config entry `cfg` when it is a folder window or (round 6 phase 6B) a standalone
    // widget (posted from its own menu / ItemCommand 856).
    void RemoveDesktopEntry(size_t cfg);
    static constexpr UINT kRemoveFolderMessage = WM_APP + 7;  // posted: wParam = config index
    // --- standalone widgets (round 6 phase 6B) ---
    // Appends a "widget" entry of widget `id`: the manifest's default size, layer "behind",
    // centred on the work area of config dock `nearDock`'s monitor (SIZE_MAX / none: the primary
    // display); syncs, saves, refreshes the settings windows and (open) opens its Dock window.
    // Returns its config index (SIZE_MAX: refused).
    size_t AddWidgetWindow(const std::wstring& id, size_t nearDock, bool open);
    // The right-click menu of standalone widget entry `cfg` at a screen point.
    void ShowWidgetMenu(size_t cfg, POINT pt);
    // ItemCommand 809 / 854 / 855 / 856 on standalone widget entry `cfg`. -1: not one of them.
    LRESULT WidgetEntryCommand(UINT cmd, size_t cfg);
    // After the config changed outside the settings windows: SyncSettingsTargets, then every open
    // window (but `except`) reloads its controls from the config.
    void RefreshSettings(SettingsWindow* except = nullptr);
    // Round 6 phase 6E: every settings window drops its widget previews and renders them again
    // as they are painted (DockCommand 148, Reload widgets, a watched widget's files changed).
    void InvalidatePreviews();
    uint64_t previewGeneration_ = 0;  // part of the preview stamp (SettingsHost::previewStamp)
    void Quit();

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;  // the app window (WinDock.App)
    Config config_;
    DockManager docks_;
    TaskbarController taskbar_;

    UINT msgQuit_ = 0;
    UINT msgRestoreTaskbar_ = 0;
    UINT msgTaskbarCreated_ = 0;
    UINT msgItemCommand_ = 0;
    UINT msgDockCommand_ = 0;
    UINT msgOpenSettings_ = 0;
    UINT msgQueryState_ = 0;
    UINT msgQueryItem_ = 0;
    bool savePending_ = false;
    bool hotkeyRegistered_ = false;  // the dashboard hotkey is registered
    std::wstring hotkeyText_;        // ... the text it was registered from (or tried: hotkeyTried_)
    bool hotkeyTried_ = false;
    // The auto-hide mode the kAutoHide toggle turns back on (from the config, default Always).
    AutoHideMode lastOnMode_ = AutoHideMode::Always;
    HICON trayIcon_ = nullptr;
    bool trayAdded_ = false;
    bool shutDown_ = false;
    int modalDepth_ = 0;          // menus / dialogs open (RunMenu, ChooseItemIcon)
    bool syncPending_ = false;    // a display sync is posted or waits for the modal loop to end
    // The open settings windows (none allocated while none is open): at most one Global, one Dock
    // window per dock id, one Item window per (dock, item). Closed ones are freed by a posted
    // kSettingsClosed.
    std::vector<std::unique_ptr<SettingsWindow>> settings_;
    SettingsWindow* activeSettings_ = nullptr;  // the settings window that is the active window
    std::unique_ptr<WidgetConsole> console_;    // null while closed
    // Round 9 phase 9B: the open settings search dialogs (one per settings window; freed by a posted
    // kSearchClosed) and the settings windows created hidden for their index.
    std::vector<std::unique_ptr<SettingsSearch>> searches_;
    std::vector<SettingsWindow*> searchHidden_;
    // Round 9 phase 9C: the open preset chooser (at most one; freed by a posted kPresetClosed) and
    // whether this run started without a config file (the welcome dialog then opens once).
    std::unique_ptr<PresetDialog> presets_;
    bool firstRun_ = false;
    // Round 9 phase 9D: the Help window (null while closed; freed by a posted kHelpClosed).
    std::unique_ptr<HelpWindow> help_;
    UINT msgWelcome_ = 0;
    // The place search in flight (host.geocode): its tag, its window and where its result goes
    // (cleared when that window closes, so a late result is dropped).
    WPARAM geocodeTag_ = 0;
    SettingsWindow* geocodeOwner_ = nullptr;
    std::function<void(const std::vector<WeatherService::Place>&, const std::wstring&)> geocodeDone_;

    // --- round 12 phase 12D: customize mode on the dock ---
    // One dock at a time (another one's mode ends first). Entered by the dock menu's first entry,
    // the Dock window header's Customize, a long press on the bar's background and DockCommand 170;
    // left by Done, Escape on the dock, a click beside the bar or on the desktop / another app's
    // window (the foreground hook below), the dock going away or getting locked again.
    static constexpr UINT kEditLeaveMessage = WM_APP + 57;  // posted by the foreground hook: another app came to the front
    // Enters customize mode on `dock`; a locked dock asks "Unlock this dock to customize it?" first
    // (Unlock clears `locked` as an undoable change). False: refused or cancelled.
    bool EnterEdit(DockWindow* dock);
    void LeaveEdit();
    // The edit bar follows the dock, the save state and the timeline; the drawer its items.
    void UpdateEditUi();
    void ShowDrawer(bool show);
    void PlaceDrawer();
    void OnEditBarCommand(int id);
    // Customize mode's edits (each one timeline entry, labelled "Removed Weather from main", ...).
    void EditRemoveItem(size_t item);
    void EditAnchorItem(size_t item, ItemAnchor anchor);
    // Drawer entry `ref` added to the dock being customized before config item `at` (SIZE_MAX: at
    // the end); the "Apps and folders" entries open their dialogs (pinned after the last pin).
    bool AddDrawerEntry(int ref, size_t at);
    // `e` inserted into config dock `cfg` before item `at` (refused: a kind the dock has once already).
    bool InsertDockEntry(size_t cfg, DockItemEntry e, size_t at);
    // A drawer tile dragged in-process (the Contents tab's or the drawer popup's): SettingsHost::drawerDrag.
    int DrawerDrag(int phase, POINT screen, const DockItemEntry* entry);
    // The open dialogs of the "Apps and folders" entries (owned by `owner`): the paths chosen.
    std::vector<std::wstring> PickPins(HWND owner, bool folder);
    // A standalone widget of `id` centred on screen point `screen` (a drawer tile dropped on the desktop).
    size_t AddWidgetWindowAt(const std::wstring& id, POINT screen);
    void HookEditForeground(bool on);
    static void CALLBACK OnEditForeground(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD thread,
                                          DWORD time);
    DockWindow* editDock_ = nullptr;  // the dock window in customize mode (null: none)
    HWND editPrevForeground_ = nullptr;  // round 12 phase 12H: another app's window in front when the mode began
    EditBar editBar_{[this](int id) { OnEditBarCommand(id); }};
    DrawerPopup drawerPopup_;
    bool drawerReady_ = false;        // its callbacks are set
    HWINEVENTHOOK editHook_ = nullptr;
    DockWindow* dragDock_ = nullptr;  // the dock showing a drawer drag's marker
    std::optional<POINT> widgetAt_;   // AddWidgetWindow: centre the new widget on this screen point
    // --- end round 12 phase 12D ---

    // --- round 12 phase 12G: Quick setup and the three tips (AppQuickSetup.cpp) ---
    // Quick setup replaces the welcome dialog: the first run, `--welcome`, About's Run quick setup
    // again, the dock menu's App > Quick setup…, DockCommand 180 (152 LOWORD 1 too). It is one Keep /
    // Cancel preview (12A) from open to Finish / Cancel; its edits go through the paths the menus and
    // settings windows use. The tips: a CoachMark bubble beside the first dock (one at a time).
    static constexpr UINT kQuickSetupClosed = WM_APP + 66;  // posted (app window): free the closed Quick setup
    // Opens Quick setup or brings it forward. `firstRun`: the first start's variant (the Basic layout
    // applied to the first dock first, as the welcome dialog's Skip gave it, then the preview).
    void OpenQuickSetup(bool firstRun);
    bool QuickSetupOpen() const { return quickSetup_ && quickSetup_->Hwnd(); }
    // QuickSetupHost::change / finish / cancel.
    bool ApplySetupChange(QuickSetupChange& c);
    bool FinishQuickSetup(bool startWithWindows, bool hideTaskbar, bool showTips);
    bool CancelQuickSetup();
    // The preview is (still, or again) open, labelled "Quick setup" (an Undo or a Keep in a settings
    // window may have ended it: the next edit starts a new one).
    void EnsureSetupPreview();
    // Layout "As it is now": docks[cfg] back to its entry in the preview's snapshot.
    bool RestoreSetupDock(size_t cfg);
    size_t FirstDockIndex() const;
    bool QuickSetupCommand(UINT command, LPARAM lp);   // DockCommand 180 - 183
    LRESULT QuickSetupState(WPARAM wp, LPARAM lp);      // QueryState 317 - 320
    // The tips: ShowTip(-1) hides; StartTips(all): Show me around (the unseen ones) or Show the
    // tips again (`all`: every bit cleared, from the first).
    void ShowTip(int index);
    void StartTips(bool all);
    int NextUnseenTip(int from) const;
    void OnTipCommand(int id);
    void MarkTips(int bits);
    // A tip's gesture was done (0 the dock menu opened, 1 customize mode entered, 2 a dock dragged):
    // its bit is set; a tip on screen that it answers moves on.
    void NoteTipGesture(int tip);
    // The bubble beside the first dock's resting bar (again: the dock moved).
    void PlaceTip();
    // The Run key for AppQuickSetup.cpp (App.cpp's AutostartEnabled / SetAutostart).
    static bool AutostartOn();
    static bool SetAutostartOn(bool enable);
    std::unique_ptr<QuickSetup> quickSetup_;
    CoachMark tips_{[this](int id) { OnTipCommand(id); }};
    int tipShown_ = -1;  // the tip on screen (-1 none)
    // --- end round 12 phase 12G ---
    // --- round 12 phase 12E: the Dock settings panel beside its dock, Where on the screen ---
    // The dock window a Dock settings panel sits beside: the instance it was opened from while that
    // exists (an "each" dock has one per display), else the dock's first window (null: parked).
    DockWindow* PanelDockOf(const SettingsWindow* w);
    // Every open panel of the dock that `moved` belongs to (null: every panel; a display change)
    // goes beside its dock again (animated); a panel whose dock has no window any more (parked,
    // disabled) closes.
    void ReanchorPanels(DockWindow* moved, bool animate);
    // The open panel of config dock `cfg` closes (DockCommand 176, Done in customize mode); false:
    // none open.
    bool ClosePanel(size_t cfg);
    // Customize mode's drag of dock `dock` along / off its edge (DockWindow::PinRequests::moveInEdge):
    // one timeline entry "Moved main (+120 px, 24 px from the edge)".
    void MoveInEdge(DockWindow& dock, int offset, int edgeGap);
    // DockCommand 176 - 178.
    bool PanelCommand(UINT command, LPARAM lp);
    HWND panelFrom_ = nullptr;  // the dock window a Dock settings… command came from (OpenDockSettings takes it)
    // --- end round 12 phase 12E ---
};
