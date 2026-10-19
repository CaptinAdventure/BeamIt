# BeamIt Windows client

**Status: integration scaffold, not a functional streaming client.** Windows
build, runtime, GPU decoding, and Teams behavior have not been validated.
The prototype gates must pass before the remaining client components are
implemented.

For cross-machine continuation, read the
[implementation plan and WSL handoff](IMPLEMENTATION-PLAN.md), including the
prototype validation gates and remaining feature-parity work.

## Selected architecture

The planned Windows 11+ client uses C++26/C++/WinRT and WinUI 3, with HWND
interop for system-tray integration, native system menus, input, owned
popup surfaces, and taskbar attention. Media Foundation will prefer hardware
decoders, then installed MF software decoders. D3D11 presents decoded video
in WinUI. There is no FFmpeg dependency.

One process will connect to one host instance and support multiple streams.
Separate processes can connect to distinct host instances. The existing
host protocol allows only one authenticated session per host instance:
connecting another client to that instance supersedes the previous session.

The intended global options live in the tray; stream-specific controls live
in each window's native system menu. A toolbar was not selected because it
adds permanent chrome; a video context menu would consume remote right-clicks.
Alt+Space will expose local stream options in fullscreen as well.

### Implemented scaffold

- Separate unpackaged x64 WinUI MSBuild solution; no host UI or host build
  changes.
- Native core/test CMake entry point, independent of host capture, encoding,
  HTML embedding, and NVIDIA SDK setup.
- Programmatic WinUI window with an explicitly labeled D3D11 blue test
  surface, native system-menu commands, fullscreen restoration, and a
  double-Escape escape path.
- Tray registration, hide-on-close, Show/Exit, and Explorer-restart recovery.
  A failed tray registration keeps the control window accessible.
- Borderless owned/nonactivating native popup prototype, without a taskbar
  entry, using a separate D3D11 surface.
- Media Foundation decoder enumeration and a separate one-IDR decode
  diagnostic.
- Portable binary-frame parsing and generation/reference gating tests.
  Future-generation frames wait for their signaling configuration rather
  than being treated as protocol failures.

The blue test surfaces contain **no remote content**. There is no connection
UI, authentication/transport implementation, streaming decoder/presenter
integration, remote input, production popup coordinator, clipboard sync,
rate-control dialog, text sending, anti-idle, or live diagnostics yet.
Prototype menu commands are diagnostic actions, not the final menu inventory.
The popup prototype uses a fixed-size local test surface, not production
remote geometry/DPI mapping.

## Windows prerequisites

- Windows 11 x64.
- Visual Studio 2022 with the v143 C++ desktop toolset, WinUI/C++ development
  components, and Windows SDK `10.0.26100.0`. Use a toolset with the required
  C++26 preview support.
- An IDE-managed CMake 4.3 or newer. The build script requires its explicit
  executable path; it does not pick an arbitrary `cmake` from `PATH`.
- The x64 Windows App Runtime for Windows App SDK 1.7.
- NuGet access for the initial restore. The application project pins
  `Microsoft.WindowsAppSDK` to `1.7.250606001` and
  `Microsoft.Windows.CppWinRT` to `2.0.240111.5`.

The selected NuGet/toolset combination still needs its first Windows restore
and build. No package installation or Windows build was attempted on macOS.
The packaged/MSIX deployment path and ARM64 are not part of this scaffold.
Run unelevated; the application manifest requests `asInvoker`.

## Build

From a Windows PowerShell terminal at the repository root:

```powershell
.\scripts\Build-WindowsClient.ps1 `
    -CMakeExecutable 'C:\path\to\IDE-managed\cmake.exe' `
    -Configuration Debug
```

Repeat with `-Configuration Release` when the Debug gates pass. The script
locates MSBuild through Visual Studio Installer, configures/builds the native
core and diagnostics, runs the frame-contract checks, restores the NuGet
dependencies, and builds the WinUI application.

The two-stage build uses matching x64 configurations and the dynamic MSVC
runtime (`/MDd` in Debug, `/MD` in Release). Building the solution directly
requires that its corresponding core library has already been built.
Generated output is under `clients/windows/build/`, ignored by Git.

```powershell
.\clients\windows\build\app\Debug\BeamIt.Client.exe
.\clients\windows\build\core\Debug\BeamItFrameProtocolTests.exe
```

No server is needed for these initial integration diagnostics.

## Prototype checks on Windows

Check that the blue WinUI/D3D11 surface appears, resizes, and survives monitor
changes. Exercise Alt+Space and the title-bar icon menu; enter fullscreen,
invoke the same menu, exit with double Escape within one second, and confirm
that the original window placement/title bar return. Test both focused
controls and the presentation area.

Close the window and restore it from the tray. Restart Explorer and check
that the icon returns. Verify tray Exit removes the icon and ends the
process. If tray creation/recovery fails, the window must stay accessible.

Open the popup prototype. Confirm that it does not steal keyboard focus or
add a taskbar button; move/minimize/deactivate the owner and close the app.
This is an interop check, not evidence that Teams popup streaming works.

### Media Foundation diagnostic

List registered H.264/HEVC decoder candidates:

```powershell
.\clients\windows\build\core\Debug\BeamItDecoderProbe.exe --list
```

Enumeration is **not** proof of successful decoding or profile support.
To actually exercise decoding, supply a local fixture containing exactly one
complete Annex B IDR access unit, including SPS/PPS and, for HEVC, VPS:

```powershell
.\clients\windows\build\core\Debug\BeamItDecoderProbe.exe `
    h264 1920 1080 'C:\fixtures\h264-main-idr.annexb'

.\clients\windows\build\core\Debug\BeamItDecoderProbe.exe `
    h265 1920 1080 'C:\fixtures\hevc-main-idr.annexb'
```

Use the fixture's actual codec and dimensions. The fixture limit is 16 MiB.
The diagnostic tries hardware candidates before installed software candidates,
prints candidate-specific errors, and reports whether output was a DXGI
surface or system memory. Exit code 0 means a picture was produced, 2 means
no candidate decoded it, and 1 means diagnostic setup/input failed.
It does not change codecs or download a decoder. A failed fixture/configuration
is not by itself proof that the GPU lacks codec support.

Validate supported H.264 and HEVC fixtures, unsupported High444/range-extension
fixtures, and missing HEVC decoder installations before choosing the production
decoder implementation. Registered MF hardware decoders may not accept
the host's default H.264 High444 output. That limitation will be surfaced
explicitly, not hidden by a host sampling downgrade or codec switch.

## Remaining gates

Windows build/restore, WinUI native-menu/fullscreen behavior, owned popup
activation, and actual MF decode results are pending Windows access.
After those gates, the implementation plan proceeds with authenticated WSS,
connection/source selection, production video/windows/input/popups, clipboard,
text sending, anti-idle, rate controls, diagnostics, and hardware/Teams
validation. None of those remaining components should be inferred from
the integration prototype.
