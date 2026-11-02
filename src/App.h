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
#include "DockWindow.h"
#include "SettingsWindow.h"
#include "TaskbarController.h"

// Names shared between the running instance and command-line invocations.
namespace AppIpc {
inline constexpr wchar_t kMutexName[] = L"Local\\WinDock.SingleInstance.6b1f0c2e";
inline constexpr wchar_t kQuitMessage[] = L"WinDock.Quit";
inline constexpr wchar_t kRestoreTaskbarMessage[] = L"WinDock.RestoreTaskbar";
// Automation: SendMessage(dock, RegisterWindowMessage(kItemCommandMessage), cmd, target) runs a
// per-item menu command (ContextMenu::kTogglePin / kNewWindow / kOpenFileLocation /
// kCloseAllWindows) on the item at index `target`, or on the item owning window `target` when it
// is a window handle. Returns 1 if the item was found.
inline constexpr wchar_t kItemCommandMessage[] = L"WinDock.ItemCommand";
// Automation: SendMessage(dock, RegisterWindowMessage(kDockCommandMessage), cmd, 0) runs a dock /
// tray menu command (ContextMenu::kAutoHide ... kLastDockCommand, kOpenSettings included),
// exactly as if it was chosen from the menu. Returns 1 for a known command.
inline constexpr wchar_t kDockCommandMessage[] = L"WinDock.DockCommand";
// Posted by `WinDock.exe --settings` to the running dock: open (or activate) the settings window.
inline constexpr wchar_t kOpenSettingsMessage[] = L"WinDock.OpenSettings";
}  // namespace AppIpc

// Wires the modules together: config, dock window, tray icon, taskbar controller.
class App {
public:
    App() = default;
    App(const App&) = delete;
    App& operator=(const App&) = delete;
    ~App();

    // Returns false (after telling the user) if the dock can't start.
    bool Initialize(HINSTANCE instance);
    void Shutdown();
    // Opens the settings window, or activates it if it is open.
    void OpenSettings();
    // Called by the message loop for every message: keyboard navigation in the settings window.
    // Returns true if the message was handled (do not translate / dispatch it).
    bool PreTranslateMessage(MSG& msg) { return settings_ && settings_->PreTranslateMessage(msg); }

private:
    static constexpr UINT kTrayCallback = WM_APP + 1;
    static constexpr UINT kTrayId = 1;
    static constexpr UINT kSettingsClosed = WM_APP + 2;  // posted: delete the destroyed settings window
    static constexpr UINT_PTR kSaveTimerId = 5;  // debounced config save (one-shot)
    static constexpr UINT kSaveDelayMs = 300;

    bool OnMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT& result);
    void AddTrayIcon();
    void RemoveTrayIcon();
    void ShowMenu(POINT screenPt);
    void ShowItemMenu(int index, POINT screenPt);
    // Runs a per-item command on the item with `key` (looked up again: the item list may have
    // changed while a menu was open). `windows` are the windows the menu listed.
    void RunItemCommand(UINT cmd, const AppKey& key, const std::vector<HWND>& windows);
    void TogglePin(const AppKey& key);
    // The config entry that pins this item (nullopt: nothing launchable to pin).
    static std::optional<PinnedEntry> EntryForItem(const DockItem& item);
    // Reorders config_.pinned to the given key order (drag to reorder); a key that is not pinned
    // yet (a running item dragged into the pinned section) is pinned at its position.
    void ApplyPinOrder(const std::vector<AppKey>& order);
    // Inserts .lnk / .exe files before config_.pinned[at], skipping apps that are already pinned.
    void PinFiles(const std::vector<std::wstring>& paths, size_t at);
    void SeedDefaultPins();
    int IndexOfKey(const AppKey& key) const;
    void RequestSave();
    void FlushSave();
    void OnCommand(UINT cmd);
    // Sets config_.autoHide, applies it to the dock and saves; remembers on-modes for kAutoHide.
    void SetAutoHide(AutoHideMode mode);
    ContextMenu::TrayMenuState MenuState() const;
    // Runs a modal menu with the dock held shown (no auto-hide while it is open).
    UINT RunMenu(const std::function<UINT()>& show);
    void ReloadConfig();
    void OpenConfigFile();
    // A setting was changed in the settings window: apply it to the dock, then save (debounced).
    void OnSettingsChanged(SettingsScope scope);
    // Reloads the settings window's controls from the config (no-op while it is closed).
    void RefreshSettings();
    void Quit();

    HINSTANCE instance_ = nullptr;
    Config config_;
    DockWindow dock_;
    TaskbarController taskbar_;

    UINT msgQuit_ = 0;
    UINT msgRestoreTaskbar_ = 0;
    UINT msgTaskbarCreated_ = 0;
    UINT msgItemCommand_ = 0;
    UINT msgDockCommand_ = 0;
    UINT msgOpenSettings_ = 0;
    UINT msgQueryState_ = 0;
    bool savePending_ = false;
    // The auto-hide mode the kAutoHide toggle turns back on (from the config, default Always).
    AutoHideMode lastOnMode_ = AutoHideMode::Always;
    HICON trayIcon_ = nullptr;
    bool trayAdded_ = false;
    bool shutDown_ = false;
    std::unique_ptr<SettingsWindow> settings_;  // null (nothing allocated) while closed
};
