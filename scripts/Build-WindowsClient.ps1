[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string] $CMakeExecutable,

    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Debug'
)

$ErrorActionPreference = 'Stop'
$client = Join-Path $PSScriptRoot '..\clients\windows'
$build = Join-Path $client 'build\core'
$core = Join-Path $client 'core'
$solution = Join-Path $client 'BeamIt.sln'
$cmake = (Resolve-Path -LiteralPath $CMakeExecutable).Path
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'

if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw 'Visual Studio Installer (vswhere.exe) is required to locate the C++ MSBuild toolset.'
}

$msbuild = & $vswhere -latest -version '[18.0,19.0)' -products '*' `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.ComponentGroup.WindowsAppDevelopment.VC.BuildTools `
    -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($msbuild)) {
    throw 'Install Visual Studio (Build Tools) 2026 with the C++ desktop and C++ WinUI app development components.'
}

& $cmake -S $core -B $build -G 'Visual Studio 18 2026' -A x64 -DBUILD_TESTING=ON
if ($LASTEXITCODE -ne 0) { throw 'Client core configuration failed.' }
& $cmake --build $build --config $Configuration
if ($LASTEXITCODE -ne 0) { throw 'Client core build failed.' }
& (Join-Path $build "$Configuration\BeamItFrameProtocolTests.exe")
if ($LASTEXITCODE -ne 0) { throw 'Client frame protocol checks failed.' }
& (Join-Path $build "$Configuration\BeamItClientTests.exe")
if ($LASTEXITCODE -ne 0) { throw 'Client protocol/session tests failed.' }
& $msbuild $solution /restore /m "/p:Configuration=$Configuration" /p:Platform=x64 /verbosity:minimal
if ($LASTEXITCODE -ne 0) { throw 'WinUI client build failed.' }

Write-Host "Client: $(Join-Path $client "build\app\$Configuration\BeamIt.Client.exe")"
Write-Host "Decoder diagnostic: $(Join-Path $build "$Configuration\BeamItDecoderProbe.exe")"
