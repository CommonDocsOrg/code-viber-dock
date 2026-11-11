<#
.SYNOPSIS
  Builds the release exe and packages it as the ViberDock by CodeViber.org installer (MSI) in dist\.

.DESCRIPTION
  Steps:
    1. Builds build\x64-release\WinDock.exe through run.bat --norun (skip with -NoBuild).
       Note: run.bat quits a running dock first (the exe is locked while it runs).
    2. Installs the WiX toolset as a global dotnet tool if it is missing (needs the .NET SDK;
       WiX 5.0.2 is pinned: v6+ asks to accept a maintenance-fee EULA).
    3. Converts LICENSE to RTF for the installer's license page.
    4. Runs wix build on installer\WinDock.wxs with the version from CMakeLists.txt.

  Output: dist\ViberDock-<version>-x64.msi (an unsigned MSI; SmartScreen warns on first run of
  unsigned installers downloaded from the web, which only code signing avoids).

.PARAMETER NoBuild
  Package the exe already in build\x64-release without rebuilding.

.PARAMETER Version
  Override the version (default: project(WinDock VERSION x.y.z) in CMakeLists.txt, which is
  also what res\app.rc carries; keep the two in sync).

.EXAMPLE
  .\installer\build-installer.ps1
  .\installer\build-installer.ps1 -NoBuild
#>
[CmdletBinding()]
param(
    [switch]$NoBuild,
    [string]$Version
)

$ErrorActionPreference = 'Stop'
$WixVersion = '5.0.2'

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'build\x64-release\WinDock.exe'
$dist = Join-Path $root 'dist'
$work = Join-Path $root 'build\installer'

# 1. Version from CMakeLists.txt.
if (-not $Version) {
    $cmake = Get-Content (Join-Path $root 'CMakeLists.txt') -Raw
    if ($cmake -notmatch 'project\(WinDock\s+VERSION\s+(\d+\.\d+\.\d+)') {
        throw 'Could not read the version from CMakeLists.txt (project(WinDock VERSION x.y.z)).'
    }
    $Version = $Matches[1]
}
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "Version must be x.y.z (got '$Version')." }
Write-Host "ViberDock by CodeViber.org $Version"

# 2. Build the release exe.
if (-not $NoBuild) {
    Write-Host '== Building x64-release'
    & cmd /c "`"$(Join-Path $root 'run.bat')`" --norun"
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
}
if (-not (Test-Path $exe)) { throw "Missing $exe (build first, or drop -NoBuild)." }

# 3. WiX as a global dotnet tool, plus its UI extension.
if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) {
    throw 'The .NET SDK is required (dotnet not found): https://dotnet.microsoft.com/download'
}
$wix = Get-Command wix -ErrorAction SilentlyContinue
$installed = if ($wix) { (& wix --version) -replace '\+.*$', '' } else { '' }
if ($installed -ne $WixVersion) {
    Write-Host "== Installing WiX $WixVersion (dotnet tool)"
    if ($wix) { & dotnet tool uninstall --global wix | Out-Null }
    & dotnet tool install --global wix --version $WixVersion
    if ($LASTEXITCODE -ne 0) { throw 'Could not install the wix dotnet tool.' }
    # A freshly installed tool may not be on this session's PATH yet.
    $toolDir = Join-Path $env:USERPROFILE '.dotnet\tools'
    if ($env:PATH -notlike "*$toolDir*") { $env:PATH = "$toolDir;$env:PATH" }
}
$extensions = & wix extension list -g
if (-not ($extensions -match "WixToolset\.UI\.wixext $WixVersion")) {
    Write-Host '== Adding the WiX UI extension'
    & wix extension add -g "WixToolset.UI.wixext/$WixVersion"
    if ($LASTEXITCODE -ne 0) { throw 'Could not add WixToolset.UI.wixext.' }
}

# 4. LICENSE as RTF (the license page needs RTF; escape the three RTF specials, one \par per line).
New-Item -ItemType Directory -Force $work, $dist | Out-Null
$licenseRtf = Join-Path $work 'license.rtf'
$lines = Get-Content (Join-Path $root 'LICENSE') | ForEach-Object {
    ($_ -replace '\\', '\\\\' -replace '\{', '\{' -replace '\}', '\}') + '\par'
}
$rtf = "{\rtf1\ansi\deff0{\fonttbl{\f0\fnil Consolas;}}\f0\fs18 " + ($lines -join "`r`n") + "}"
Set-Content -Path $licenseRtf -Value $rtf -Encoding ascii

# 5. Build the MSI.
$msi = Join-Path $dist "ViberDock-$Version-x64.msi"
Write-Host "== wix build -> $msi"
& wix build -arch x64 `
    -ext WixToolset.UI.wixext `
    -d "Version=$Version" `
    -d "ExePath=$exe" `
    -d "ProjectRoot=$root" `
    -d "LicenseRtf=$licenseRtf" `
    -intermediateFolder (Join-Path $work 'obj') `
    -pdb (Join-Path $work 'WinDock.wixpdb') `
    -o $msi `
    (Join-Path $PSScriptRoot 'WinDock.wxs')
if ($LASTEXITCODE -ne 0) { throw 'wix build failed.' }

$size = [math]::Round((Get-Item $msi).Length / 1MB, 1)
Write-Host "Done: $msi ($size MB)"
Write-Host 'Install:   double-click, or  msiexec /i <msi>'
Write-Host 'Quiet:     msiexec /i <msi> /qn'
Write-Host 'Uninstall: Apps & features, or  msiexec /x <msi>'
