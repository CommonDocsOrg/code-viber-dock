@echo off
rem ViberDock by CodeViber.org: build the release exe and the MSI installer (dist\ViberDock-<version>-x64.msi).
rem Thin wrapper over build-installer.ps1; arguments are passed through (e.g. -NoBuild).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-installer.ps1" %*
exit /b %ERRORLEVEL%
