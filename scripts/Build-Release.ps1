<#
.SYNOPSIS
    Builds BeamIt in Release and produces ready-to-copy folders and zips in dist\.

.DESCRIPTION
    dist\BeamIt-Host\            BeamIt.exe, BeamItInputHelper.exe, Start-BeamIt.cmd and launcher scripts.
                                 Static C++ runtime: nothing else to install on the host.
    dist\BeamIt-Client-Windows\  BeamIt.Client.exe with a self-contained Windows App SDK and app-local
                                 C++ runtime: nothing else to install except the Store HEVC Video Extension.
    dist\*.zip                   The same folders, zipped.

    The NVENC (H.264 4:4:4) encoder requires vendor\Video_Codec_SDK_13.0.37.zip; pass -NoNvenc to build
    without it (H.264 then uses D3D12 4:2:0). The macOS client is built on a Mac with
    scripts/build-macos-release.sh.

.EXAMPLE
    .\scripts\Build-Release.ps1
.EXAMPLE
    .\scripts\Build-Release.ps1 -SkipClient -SkipTests
#>
[CmdletBinding()]
param(
    # Defaults to the CMake bundled with Visual Studio (Build Tools) 2026.
    [string] $CMakeExecutable,
    [string] $OutputDirectory,
    [switch] $SkipClient,
    [switch] $SkipTests,
    [switch] $NoNvenc
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repository = Split-Path -Parent $PSScriptRoot
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repository 'dist' }

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio (Build Tools) 2026 is required (vswhere.exe not found).' }
$visualStudio = & $vswhere -latest -version '[18.0,19.0)' -products '*' `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
if (-not $visualStudio) { throw 'Install Visual Studio (Build Tools) 2026 with the C++ desktop workload.' }
if (-not $CMakeExecutable) {
    $CMakeExecutable = Join-Path $visualStudio 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
}
if (-not (Test-Path -LiteralPath $CMakeExecutable)) { throw "CMake not found: $CMakeExecutable (pass -CMakeExecutable)." }

$nvenc = if ($NoNvenc) { 'OFF' } else { 'ON' }
if (-not $NoNvenc -and -not (Test-Path -LiteralPath (Join-Path $repository 'vendor\Video_Codec_SDK_13.0.37.zip'))) {
    throw 'vendor\Video_Codec_SDK_13.0.37.zip is missing. Download it from developer.nvidia.com or pass -NoNvenc.'
}

function Invoke-Checked([string] $What, [scriptblock] $Command) {
    Write-Host "==> $What" -ForegroundColor Cyan
    & $Command
    if ($LASTEXITCODE -ne 0) { throw "$What failed (exit code $LASTEXITCODE)." }
}

function Copy-License([string] $Source, [string] $Destination) {
    if (Test-Path -LiteralPath $Source) { Copy-Item -LiteralPath $Source -Destination $Destination }
    else { Write-Warning "License file not found: $Source" }
}

function New-Package([string] $Name) {
    $path = Join-Path $OutputDirectory $Name
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force }
    New-Item -ItemType Directory -Path (Join-Path $path 'licenses') | Out-Null
    $path
}

function Complete-Package([string] $Path) {
    $zip = "$Path.zip"
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    Compress-Archive -Path (Join-Path $Path '*') -DestinationPath $zip
    Write-Host "Packaged $Path and $zip" -ForegroundColor Green
}

# ---- Host -------------------------------------------------------------------------------------------------
$hostBuild = Join-Path $repository 'cmake-build-release'
$testing = if ($SkipTests) { 'OFF' } else { 'ON' }
Invoke-Checked 'Configure host' {
    & $CMakeExecutable -S $repository -B $hostBuild -G 'Visual Studio 18 2026' -A x64 `
        "-DBEAMIT_FETCH_NVENC_INTERFACE=$nvenc" "-DBUILD_TESTING=$testing"
}
$targets = @('BeamIt') + $(if ($SkipTests) { @() } else { @('BeamItTests') })
Invoke-Checked 'Build host' { & $CMakeExecutable --build $hostBuild --config Release --parallel --target @targets }
$hostOutput = Join-Path $hostBuild 'Release'
if (-not $SkipTests) {
    Invoke-Checked 'Host tests' { & (Join-Path $hostOutput 'BeamItTests.exe') '[protocol],[host-control]' }
}

$package = New-Package 'BeamIt-Host'
foreach ($file in 'BeamIt.exe', 'BeamItInputHelper.exe', 'Install-BeamItCert.ps1') {
    Copy-Item -LiteralPath (Join-Path $hostOutput $file) -Destination $package
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Start-BeamItServer.ps1') -Destination $package
Set-Content -LiteralPath (Join-Path $package 'Start-BeamIt.cmd') -Encoding Ascii -Value @'
@echo off
rem Starts a BeamIt host instance. Extra arguments are passed through, e.g. -Instance teams -Port 17390
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Start-BeamItServer.ps1" %*
if errorlevel 1 pause
'@
Set-Content -LiteralPath (Join-Path $package 'README.txt') -Encoding UTF8 -Value @'
BeamIt host (Windows 11 x64)

1. Double-click Start-BeamIt.cmd (do not run it as administrator).
   The first start creates %LOCALAPPDATA%\BeamIt\default\ with password.txt, cert.pem and key.pem.
2. The console prints the Server URL(s) and the certificate fingerprint. Enter these in the client,
   with the password from %LOCALAPPDATA%\BeamIt\default\password.txt.
3. If Windows Firewall asks, allow access on Private networks only.

More instances: Start-BeamIt.cmd -Instance teams -Port 17390
Elevated input for administrator windows: Start-BeamIt.cmd --elevate-input (or tray menu > Elevate input...)
Stop: tray icon > Exit, or Ctrl+C in the console.
'@
$deps = Join-Path $hostBuild '_deps'
$licenses = Join-Path $package 'licenses'
Copy-License (Join-Path $deps 'opus-src\COPYING') (Join-Path $licenses 'Opus-COPYING.txt')
Copy-License (Join-Path $deps 'spdlog-src\LICENSE') (Join-Path $licenses 'spdlog-LICENSE.txt')
Copy-License (Join-Path $deps 'cli11-src\LICENSE') (Join-Path $licenses 'CLI11-LICENSE.txt')
Copy-License (Join-Path $deps 'boost-src\LICENSE_1_0.txt') (Join-Path $licenses 'Boost-LICENSE_1_0.txt')
Copy-License (Join-Path $deps 'wintls-src\LICENSE') (Join-Path $licenses 'boost-wintls-LICENSE.txt')
if (-not $NoNvenc) {
    Copy-License (Join-Path $repository 'vendor\nvidia_nvend\NOTICES.txt') (Join-Path $licenses 'NVIDIA-Video-Codec-SDK-NOTICES.txt')
    Copy-License (Join-Path $repository 'vendor\nvidia_nvend\License.pdf') (Join-Path $licenses 'NVIDIA-Video-Codec-SDK-License.pdf')
}
Complete-Package $package

# ---- Windows client ---------------------------------------------------------------------------------------
if (-not $SkipClient) {
    Invoke-Checked 'Build Windows client' {
        & (Join-Path $PSScriptRoot 'Build-WindowsClient.ps1') -CMakeExecutable $CMakeExecutable -Configuration Release
        $global:LASTEXITCODE = 0
    }
    $clientRoot = Join-Path $repository 'clients\windows'
    $clientOutput = Join-Path $clientRoot 'build\app\Release'
    $package = New-Package 'BeamIt-Client-Windows'
    Get-ChildItem -LiteralPath $clientOutput -Exclude '*.pdb', '*.lib', '*.exp', '*.ilk' |
        Copy-Item -Destination $package -Recurse

    # App-local Visual C++ runtime (redistributable files from the Visual Studio installation).
    $crt = Get-ChildItem (Join-Path $visualStudio 'VC\Redist\MSVC') -Directory |
        Where-Object { $_.Name -match '^\d' } | Sort-Object { [version]$_.Name } -Descending |
        ForEach-Object { Get-ChildItem (Join-Path $_.FullName 'x64') -Directory -Filter 'Microsoft.VC*.CRT' } |
        Select-Object -First 1
    if (-not $crt) { throw 'Visual C++ x64 redistributable files not found under VC\Redist\MSVC.' }
    Copy-Item -Path (Join-Path $crt.FullName '*.dll') -Destination $package

    Set-Content -LiteralPath (Join-Path $package 'README.txt') -Encoding UTF8 -Value @'
BeamIt Windows client (Windows 11 x64)

Requires the "HEVC Video Extensions" from the Microsoft Store (streams are HEVC).
Run BeamIt.Client.exe (not as administrator), then enter the Server URL, password and certificate
fingerprint printed by the host's Start-BeamIt.cmd.
'@
    $licenses = Join-Path $package 'licenses'
    Copy-License (Join-Path $clientRoot 'build\core\_deps\boost-src\LICENSE_1_0.txt') (Join-Path $licenses 'Boost-LICENSE_1_0.txt')
    Copy-License (Join-Path $clientRoot 'build\core\_deps\wintls-src\LICENSE') (Join-Path $licenses 'boost-wintls-LICENSE.txt')
    Complete-Package $package
}

Write-Host ''
Write-Host "Done. Copy $OutputDirectory\BeamIt-Host.zip to the host$(if (-not $SkipClient) { " and BeamIt-Client-Windows.zip to Windows clients" })." -ForegroundColor Green
