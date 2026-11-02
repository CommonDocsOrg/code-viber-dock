@echo off
setlocal
rem WinDock: build the release exe and start the dock.
rem   run.bat          build (if needed) and start
rem   run.bat --stop   stop a running dock and restore the taskbar
rem   run.bat --norun  build only

set "ROOT=%~dp0"
set "EXE=%ROOT%build\x64-release\WinDock.exe"

if /i "%~1"=="--stop" (
    if exist "%EXE%" "%EXE%" --quit
    echo WinDock stopped.
    exit /b 0
)

rem Stop a running instance so the fresh build can replace the exe.
if exist "%EXE%" "%EXE%" --quit >nul 2>&1

rem Locate the Visual Studio developer environment.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSROOT="
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
)
if not defined VSROOT set "VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community"
if not exist "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" (
    echo Could not find vcvars64.bat under "%VSROOT%".
    exit /b 1
)
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

cd /d "%ROOT%"
cmake --preset x64-release || goto :fail
cmake --build --preset x64-release || goto :fail

if /i "%~1"=="--norun" (
    echo Build finished: "%EXE%"
    exit /b 0
)

start "" "%EXE%" >nul 2>&1
echo WinDock started.
echo   Stop it:            run.bat --stop   (or Quit from the tray icon)
echo   Taskbar missing?    "%EXE%" --restore-taskbar
echo   Config file:        %LocalAppData%\WinDock\config.json
exit /b 0

:fail
echo Build failed.
exit /b 1
