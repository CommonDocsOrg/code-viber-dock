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
    // Opens the Global settings window, or activates it if it is open; `page` >= 0 selects that
    // page (SettingsIds::kPage*). DockCommand 107, the tray / dock menu's Global settings…, --settings.
    void OpenSettings(int page = -1);
    // The settings window of docks[`dock`] (a dock, or a folder window entry), or activates it;
    // `tab` >= 0 selects that tab (SettingsIds::kTab*). The dock menu's Dock settings…, DockCommand 143.
    void OpenDockSettings(size_t dock, int tab = -1);
    // The settings window of item `item` of docks[`dock`] (a widget, clock, weather, folder or
    // group item; another kind opens the dock's window), or activates it. The one way in: the item
    // menu's Settings…, Contents > Settings…, the weather widget's map pin, weather.openSettings(),
    // DockCommand 113 / 142, ItemCommand 854.
    void OpenItemSettings(size_t dock, size_t item);
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
    static constexpr UINT_PTR kSaveTimerId = 5;  // debounced config save (one-shot)
    static constexpr UINT kSaveDelayMs = 300;

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
    // The dock / tray menu of `dock` (null: the first dock, or none at all). `slot`: the slot the
    // right click was on (a widget / clock / weather slot adds its "settings…" entry; -1 none).
    void ShowMenu(DockWindow* dock, POINT screenPt, int slot = -1);
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
    // Creates, shows and keeps `w` (false: it could not be created).
    bool AddSettingsWindow(std::unique_ptr<SettingsWindow> w);
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
    // The place search in flight (host.geocode): its tag, its window and where its result goes
    // (cleared when that window closes, so a late result is dropped).
    WPARAM geocodeTag_ = 0;
    SettingsWindow* geocodeOwner_ = nullptr;
    std::function<void(const std::vector<WeatherService::Place>&, const std::wstring&)> geocodeDone_;
};
