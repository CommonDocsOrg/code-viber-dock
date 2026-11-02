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
    // Opens the settings window, or activates it if it is open; `page` >= 0 selects that page
    // (SettingsIds::kPage*).
    void OpenSettings(int page = -1);
    // Called by the message loop for every message: keyboard navigation in the settings window.
    // Returns true if the message was handled (do not translate / dispatch it).
    bool PreTranslateMessage(MSG& msg) { return settings_ && settings_->PreTranslateMessage(msg); }

private:
    static constexpr UINT kTrayCallback = WM_APP + 1;
    static constexpr UINT kTrayId = 1;
    static constexpr UINT kSettingsClosed = WM_APP + 2;  // posted: delete the destroyed settings window
    static constexpr UINT kDialogActivated = WM_APP + 3;  // posted (wParam = window): ChooseItemIcon's focus fix
    static constexpr UINT kDisplaySync = WM_APP + 4;      // posted: re-enumerate the monitors (coalesced)
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
    // The dock / tray menu of `dock` (null: the first dock, or none at all).
    void ShowMenu(DockWindow* dock, POINT screenPt);
    void ShowItemMenu(DockWindow& dock, int index, POINT screenPt);
    // Runs a per-item command on the item with `key` of `dock` (looked up again: the item list may
    // have changed while a menu was open). `windows` are the windows the menu listed.
    void RunItemCommand(DockWindow& dock, UINT cmd, const AppKey& key, const std::vector<HWND>& windows);
    void TogglePin(DockWindow& dock, const AppKey& key);
    // Sets (an empty `icon`: clears) the `icon` of every pinned entry of the dock's config that
    // resolves to `key` (apps, folders, the Recycle Bin; not separators), then reloads the pins,
    // saves and refreshes the settings window. Returns false if no such pin exists.
    bool SetPinIcon(DockWindow& dock, const AppKey& key, const std::wstring& icon);
    // kChooseIcon / kChooseImage: runs the picker (owned by the dock, with the dock held shown)
    // for the pin `key`, then SetPinIcon.
    void ChooseItemIcon(DockWindow& dock, UINT cmd, const AppKey& key);
    // The config entry that pins this item (nullopt: nothing launchable to pin).
    static std::optional<DockItemEntry> EntryForItem(const DockItem& item);
    // Reorders the pins of the dock's config to the given key order (drag to reorder); a key that
    // is not pinned yet (a running item dragged into the pinned section) is pinned at its position.
    void ApplyPinOrder(DockWindow& dock, const std::vector<AppKey>& order);
    // Inserts .lnk / .exe files (and folders) so the first becomes pin `at` of config dock `cfg`
    // (past the last pin: after it), skipping apps that are already pinned (`pins`: its resolved pins).
    void PinFiles(size_t cfg, const std::vector<AppIdentity::ResolvedPin>& pins, const std::vector<std::wstring>& paths,
                  size_t at);
    // Moves config item `from` of config dock `cfg` right before item `to` (a widget dragged on the
    // dock), then reloads, saves and refreshes the settings window.
    void MoveItem(size_t cfg, size_t from, size_t to);
    // Add widget > Clock / Weather / Running apps (DockConfig::AddWidget's placement).
    void AddWidget(size_t cfg, ItemKind kind);
    // Every window of config dock `cfg` re-reads its items (DockWindow::ReloadPins).
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
    // A setting was changed in the settings window: apply it to the docks, then save (debounced).
    void OnSettingsChanged(SettingsScope scope);
    // Docks page: add / duplicate / remove a config dock (refused while a menu or dialog is open).
    bool EditDocks(SettingsDockEdit edit, size_t index);
    // Reloads the settings window's controls from the config (no-op while it is closed).
    void RefreshSettings();
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
    // The auto-hide mode the kAutoHide toggle turns back on (from the config, default Always).
    AutoHideMode lastOnMode_ = AutoHideMode::Always;
    HICON trayIcon_ = nullptr;
    bool trayAdded_ = false;
    bool shutDown_ = false;
    int modalDepth_ = 0;          // menus / dialogs open (RunMenu, ChooseItemIcon)
    bool syncPending_ = false;    // a display sync is posted or waits for the modal loop to end
    bool keepShown_ = false;      // the settings window keeps the docks shown (new docks too)
    std::unique_ptr<SettingsWindow> settings_;  // null (nothing allocated) while closed
    // The settings window's place search in flight (host.geocode): its tag and where its result
    // goes (cleared when the window closes, so a late result is dropped).
    WPARAM geocodeTag_ = 0;
    std::function<void(const std::vector<WeatherService::Place>&, const std::wstring&)> geocodeDone_;
};
