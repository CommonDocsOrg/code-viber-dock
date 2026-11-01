#pragma once

#include <windows.h>
#include <shellapi.h>

#include "Config.h"
#include "DockWindow.h"
#include "TaskbarController.h"

// Names shared between the running instance and command-line invocations.
namespace AppIpc {
inline constexpr wchar_t kMutexName[] = L"Local\\WinDock.SingleInstance.6b1f0c2e";
inline constexpr wchar_t kQuitMessage[] = L"WinDock.Quit";
inline constexpr wchar_t kRestoreTaskbarMessage[] = L"WinDock.RestoreTaskbar";
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

private:
    static constexpr UINT kTrayCallback = WM_APP + 1;
    static constexpr UINT kTrayId = 1;

    bool OnMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT& result);
    void AddTrayIcon();
    void RemoveTrayIcon();
    void ShowMenu(POINT screenPt);
    void OnCommand(UINT cmd);
    void OpenConfigFile();
    void Quit();

    HINSTANCE instance_ = nullptr;
    Config config_;
    DockWindow dock_;
    TaskbarController taskbar_;

    UINT msgQuit_ = 0;
    UINT msgRestoreTaskbar_ = 0;
    UINT msgTaskbarCreated_ = 0;
    HICON trayIcon_ = nullptr;
    bool trayAdded_ = false;
    bool shutDown_ = false;
};
