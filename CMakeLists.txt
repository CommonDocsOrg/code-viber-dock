cmake_minimum_required(VERSION 3.21)

project(WinDock VERSION 0.1.0 LANGUAGES CXX RC)

if(NOT MSVC)
  message(FATAL_ERROR "WinDock is built with MSVC (run from a VS developer environment).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT: no VC++ redistributable needed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(WinDock WIN32
  src/main.cpp
  src/App.cpp
  src/Config.cpp
  src/Theme.cpp
  src/DockWindow.cpp
  src/DockManager.cpp
  src/DesktopLayer.cpp
  src/DockElement.cpp
  src/WidgetElements.cpp
  src/Backdrop.cpp
  src/PopupWindow.cpp
  src/PreviewPopup.cpp
  src/FolderPopup.cpp
  src/WeatherPopup.cpp
  src/CalendarPopup.cpp
  src/Weather.cpp
  src/WeatherGlyphs.cpp
  src/Renderer.cpp
  src/Clock.cpp
  src/TaskbarController.cpp
  src/ContextMenu.cpp
  src/AppIdentity.cpp
  src/WindowTracker.cpp
  src/ForegroundWatch.cpp
  src/IconCache.cpp
  src/IconUtil.cpp
  src/IconEffects.cpp
  src/Activation.cpp
  src/Launcher.cpp
  src/DarkMode.cpp
  src/SettingsWindow.cpp
  src/SettingsNav.cpp
  src/SettingsPages.cpp
  src/SettingsPagesAppearance.cpp
  src/SettingsPagesMore.cpp
  src/SettingsPagesWeather.cpp
  src/SettingsPagesDocks.cpp
  src/SettingsPagesLayout.cpp
  src/SettingsPreview.cpp
  res/app.rc
  res/app.manifest
)

target_include_directories(WinDock PRIVATE src)

target_compile_definitions(WinDock PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
  _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A00000C
)

target_compile_options(WinDock PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /utf-8 /Zc:__cplusplus /bigobj>
  $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CONFIG:Release>>:/O2 /GL /Gy /Gw>
)

target_link_options(WinDock PRIVATE
  $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>
)

target_link_libraries(WinDock PRIVATE
  d3d11 dxgi d2d1 dxguid dcomp dwrite windowscodecs
  shell32 ole32 shlwapi dwmapi propsys user32 gdi32 version advapi32
  comctl32 uxtheme comdlg32 winhttp shcore
  windowsapp
)
