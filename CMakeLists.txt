cmake_minimum_required(VERSION 3.21)

project(WinDock VERSION 0.1.0 LANGUAGES C CXX RC)

if(NOT MSVC)
  message(FATAL_ERROR "WinDock is built with MSVC (run from a VS developer environment).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT: no VC++ redistributable needed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# QuickJS-ng (third_party/quickjs-ng, see its VERSION.md): our own static library with upstream's
# defines, warnings off (/W0: ~1,500 at /W4), the same static runtime as the dock (CMP0091 is NEW
# with cmake_minimum_required 3.21, so CMAKE_MSVC_RUNTIME_LIBRARY applies to it too).
add_library(quickjs STATIC
  third_party/quickjs-ng/quickjs.c
  third_party/quickjs-ng/dtoa.c
  third_party/quickjs-ng/libregexp.c
  third_party/quickjs-ng/libunicode.c
)
set_target_properties(quickjs PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_include_directories(quickjs PUBLIC third_party/quickjs-ng)
target_compile_definitions(quickjs PRIVATE QUICKJS_NG_BUILD WIN32_LEAN_AND_MEAN _GNU_SOURCE _WIN32_WINNT=0x0601)
target_compile_options(quickjs PRIVATE /W0 /experimental:c11atomics /utf-8
  $<$<CONFIG:Release>:/O2 /GL /Gy /Gw>
)

add_executable(WinDock WIN32
  src/main.cpp
  src/App.cpp
  src/Config.cpp
  src/Theme.cpp
  src/DockWindow.cpp
  src/DockWindowTray.cpp
  src/TrayHost.cpp
  src/TrayElements.cpp
  src/DockManager.cpp
  src/DesktopLayer.cpp
  src/DockElement.cpp
  src/WidgetElements.cpp
  src/Backdrop.cpp
  src/PopupWindow.cpp
  src/PreviewPopup.cpp
  src/FolderPopup.cpp
  src/FolderWindow.cpp
  src/ShellFiles.cpp
  src/ItemGridView.cpp
  src/InputDialog.cpp
  src/WeatherPopup.cpp
  src/CalendarPopup.cpp
  src/Weather.cpp
  src/WeatherGlyphs.cpp
  src/Renderer.cpp
  src/Clock.cpp
  src/ClockFaces.cpp
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
  src/SettingsPagesDocks.cpp
  src/SettingsPagesLayout.cpp
  src/SettingsPagesTray.cpp
  src/SettingsPagesNotifications.cpp
  src/SettingsPreview.cpp
  src/QuickJsEngine.cpp
  src/WidgetCanvas.cpp
  src/WidgetPackage.cpp
  src/WidgetInstance.cpp
  src/WidgetRuntime.cpp
  src/HttpClient.cpp
  src/ScriptPopup.cpp
  src/WidgetConsole.cpp
  src/SettingsPagesWidgets.cpp
  src/SettingsPagesItems.cpp
  src/AudioService.cpp
  src/MediaService.cpp
  src/NotificationService.cpp
  src/NotificationElements.cpp
  src/NotificationPopup.cpp
  src/Thumbnails.cpp
  src/Ics.cpp
  src/CalendarService.cpp
  src/SettingsPagesCalendar.cpp
  src/SettingsPagesFolderWindow.cpp
  src/ReminderBanner.cpp
  res/app.rc
  res/app.manifest
)

target_include_directories(WinDock PRIVATE src)

# The built-in widgets and clock faces are RCDATA in app.rc: rebuild the resources when one of them changes.
file(GLOB_RECURSE WINDOCK_BUILTIN_WIDGETS CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/widgets/*.json
  ${CMAKE_CURRENT_SOURCE_DIR}/widgets/*.js
  ${CMAKE_CURRENT_SOURCE_DIR}/res/clock-faces/*.json
)
set_source_files_properties(res/app.rc PROPERTIES OBJECT_DEPENDS "${WINDOCK_BUILTIN_WIDGETS}")

target_compile_definitions(WinDock PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
  _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A00000C
)

target_compile_options(WinDock PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /utf-8 /Zc:__cplusplus /bigobj>
  $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CONFIG:Release>>:/O2 /GL /Gy /Gw>
)

# An 8 MB main-thread stack: the widget scripts run on the UI thread, and QuickJS's own stack
# limit (JS_SetMaxStackSize, WidgetRuntime.h) must stay well inside the real stack (MSVC frames are
# ~4.8 KB per JS call; with the default 1 MB stack deep recursion would crash the process).
target_link_options(WinDock PRIVATE
  /STACK:8388608
  $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>
)

target_link_libraries(WinDock PRIVATE
  d3d11 dxgi d2d1 dxguid dcomp dwrite windowscodecs
  shell32 ole32 shlwapi dwmapi propsys user32 gdi32 version advapi32
  comctl32 uxtheme comdlg32 winhttp shcore
  windowsapp coremessaging
  quickjs
)

# The ICS parser's tests (round 4 phase 13, tests/ics): not built by default.
#   cmake --build --preset x64-release --target WinDockTests, then build/x64-release/WinDockTests.exe
add_executable(WinDockTests EXCLUDE_FROM_ALL tests/ics/IcsTests.cpp src/Ics.cpp)
target_include_directories(WinDockTests PRIVATE src)
target_compile_definitions(WinDockTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00)
target_compile_options(WinDockTests PRIVATE /W4 /permissive- /utf-8)
