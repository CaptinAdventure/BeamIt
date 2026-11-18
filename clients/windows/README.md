# BeamIt Windows client

**Status: functional streaming client with two-way audio, feature parity in progress.** The
client connects to a host, pins its certificate, authenticates, lists host
applications and streams windows, applications and desktops into native
windows with keyboard and mouse input. Bidirectional clipboard text/HTML/PNG
and file/folder integration is implemented; native clipboard/UI and LAN
validation remain pending. Text sending, anti-idle and the remote popup
coordinator are not implemented yet
(see [the implementation plan](IMPLEMENTATION-PLAN.md)).
Teams and cross-machine LAN behavior have not been validated.

## Architecture

The Windows 11+ client uses C++26 (MSVC `/std:c++latest`), C++/WinRT and
WinUI 3, with HWND interop for the tray, native system menus, raw mouse input
and taskbar attention.

- **`core/` (CMake static library, testable without UI):**
  `ClientProtocol` (URL/fingerprint parsing, `hello`/`authenticate`, stream
  offer and video-parameter validation, clock estimate), `ClientSession`
  (network thread, WSS transport, pinning, requests, stream sockets),
  `FrameProtocol` (binary frames, generation/key-frame gate), `HostWindows`
  (window model, application grouping, search, window following),
  `ClipboardManager` (negotiation, background spool/staging, confirmation,
  receiver acknowledgement and paste guards), `AudioClient` (Opus,
  event-driven WASAPI microphone capture and mixed host-track playback),
  `KeyCodes` (scan code to
  `KeyboardEvent.code`) and `VideoPipeline`
  (Media Foundation decoding and D3D11 presentation).
- **`app/` (WinUI MSBuild project):** `ClientApp` (connection and source
  window, tray, stream bookkeeping) and `StreamWindow` (one window per
  stream: presentation, system menu, input).

The core reuses the host's `src/net/Crypto` and `src/net/Protocol.hpp` and the
checksum-pinned Boost.Beast/JSON and boost-wintls declarations in
`cmake/BeamItDependencies.cmake`, shared with the host build. The core
build generates `build/core/<Config>/BeamItWindowsCore.props`, which gives
the WinUI project its include paths, definitions and static libraries.

### Transport and security

- One process holds one authenticated session (PROTOCOL.md §2). Connecting
  another client to the same host instance supersedes it (close code `4001`,
  shown to the user). Use separate processes for separate host instances.
- Schannel TLS via boost-wintls. boost-wintls 1.0.0 has no public peer
  certificate accessor, so `cmake/patches/wintls-native-handle.cmake` adds
  `stream::native_handle()` during the dependency's patch step. The SHA-256
  of the presented leaf certificate is compared (constant time) **after the
  TLS handshake and before any HTTP/WebSocket byte is sent**, on the control
  and every stream socket. A mismatch reports the presented fingerprint.
- "Trust any server certificate" skips the pin and is labeled as unsafe.
- PBKDF2-HMAC-SHA256 (at least 600,000 iterations; weaker challenges are
  rejected) and mutual HMAC proofs. The password is never stored; the
  session's copy and the derived key are cleared after use. Nonsecret
  settings are saved in `%LOCALAPPDATA%\BeamIt\windows-client.json`.
- Requests time out after 10 s. Signaling pings (200 ms at first, then every
  5 s) feed the clock estimate; an unanswered ping ends the session. Beast's
  WebSocket idle timer is disabled because it kept the network thread alive
  for up to 15 s after sockets were closed.
- Writes are serialized with a 64 MiB bound. Pre-authentication messages are
  limited to 64 KiB, later ones to 16 MiB (64 MiB for `clipboard` replies).
- Stream offers must use `wss://` on the control socket's authority and the
  `/streams/{stream_id}?token=` path.

### Video

- The client always requests **HEVC** (`video.codec = "h265"`). The host's
  default H.264 path on NVIDIA GPUs is NVENC High 4:4:4, which the inbox
  Microsoft H.264 decoder rejects (see results below). HEVC 4:2:0 and 4:4:4
  decode with the HEVC Video Extension. There is no codec picker, retry or
  host sampling downgrade; an undecodable stream fails visibly, the client
  stops it on the host, and the session stays usable.
- Decoding uses synchronous Media Foundation transforms with the stream's
  D3D11 device manager, so the Microsoft/HEVC Video Extension decoders use
  DXVA where the driver supports the profile. Candidates are tried in
  enumeration order on the first key frame. The first candidate that decodes
  it is used. Asynchronous vendor hardware MFTs are not used (none are
  registered on the development machine). Stream info reports the decoder
  name and whether the output is a GPU surface or a system-memory upload.
- A per-stream pipeline thread decodes in order. Only decoded pictures are
  coalesced; compressed pictures are never dropped selectively. If the
  bounded queue overflows, decoding restarts at a key frame and one is
  requested. Pictures that arrive before their generation's
  `stream_config` are held (bounded), then replayed.
- `ID3D11VideoProcessor` converts NV12/P010/AYUV/Y410/... to a BGRA
  composition swap chain (flip-discard, frame-latency waitable, maximum
  latency 1), letterboxes with black bars, and crops the coded size to the
  displayed size.
- Presentation counters measure swap-chain enqueue, not display completion;
  one-frame end-to-end latency is not verified.

### Windows and input

- Tray: open the connection/source window, active streams, disconnect, exit.
  Closing the connection window hides it to the tray. If the tray icon
  cannot be registered, the window stays visible.
- Sources: desktop (DXGI or WGC), application search by title, executable,
  PID or window title, and **All windows** mode. This mode also streams the
  application's main/owned/tool windows as they open, like the macOS client.
  Minimized windows wait until they are restored. A secondary window is
  placed relative to a visible stream of the same process.
- Stream windows open at 1× (one video pixel per physical screen pixel,
  limited to the work area). The system menu (title-bar icon or Alt+Space,
  also in fullscreen) offers mouse capture, scaling with 0.5×/1×/2× sizes,
  fullscreen (double Escape within one second exits), PNG screenshot, stream
  info and **Change parameters…** (`set_stream_rate`: average 1–1,000,000
  kbit/s, frame rate 1–240, optional peak 0–4,000,000 where 0 means
  automatic and an empty field keeps the current policy). Submitted changes
  cannot be cancelled; the acknowledged values are shown in stream info.
  Host window icons and taskbar attention (`FlashWindowEx`) are
  mirrored.
- Keyboard: physical scan codes, including extended keys and left/right
  modifiers, map to `KeyboardEvent.code`. Alt+Space and Ctrl+Alt+G are
  handled locally, and OS-reserved shortcuts stay local; Alt+F4 closes the
  local stream window.
- Mouse: absolute coordinates are mapped through the displayed video
  rectangle and exclude the letterbox bars, except during a drag. The wheel
  uses the protocol's DOM sign convention. Mouse capture switches to
  relative mode only after the host acknowledges `set_mouse_mode`, then uses
  raw input, cursor clipping and hiding. Focus loss, menus, capture release
  and closing send `release_all`.
- Pointer shape: streams request `input.cursor: "client"` (PROTOCOL.md
  §5.9.11), so the host stops drawing its pointer into the video and the
  client shows the matching standard Windows cursor (I-beam, hand, resize
  arrows, busy…) over the video. `none` hides the local pointer over the
  video. Custom application cursors appear as the arrow. Hosts without this
  feature keep the pointer drawn into the video.
- Fullscreen bar (like Remote Desktop's connection bar): auto-hide by
  default; in fullscreen it stays hidden until the pointer touches the top
  screen edge, then appears at the top centre. It shows a pin (keep visible),
  the stream title, **Minimize** (minimizes the window and releases input
  while the stream stays connected; restoring returns to fullscreen),
  **Exit fullscreen** and **Close**. It hides again shortly after the
  pointer leaves it unless pinned.

### Two-way audio

- Start audio independently of video, select **Desktop audio** or one of the
  host's listed applications, then start the authenticated full-duplex Opus
  socket. The client captures its default microphone with event-driven WASAPI
  and plays host audio tracks through its default render endpoint.
- Each host track uses its negotiated Opus metadata and has an independent
  decoder. The client bounds packet queues, drops stale playback audio under
  overload, and tears audio down on stop, socket failure or session end.
- On the host, client microphone PCM is delivered only to the installed
  **BeamIt Microphone** virtual recording device; there is no fallback to a
  physical microphone or unrelated endpoint. Install a properly signed host
  driver package, select **BeamIt Microphone** in Teams and keep its recording
  stream active. If the device is missing, busy or inaccessible, audio startup
  fails visibly; if the driver cannot accept PCM, the host stops audio without
  affecting video. Driver deployment and Teams validation are documented in
  [`drivers/BeamItMicrophone/README.md`](../../drivers/BeamItMicrophone/README.md).
- Audio is independent of video stream lifetimes. Ending a video stream does
  not stop the microphone or outgoing audio track; **Stop audio** ends both
  directions for the authenticated session.
## Clipboard transfers

After connecting, enable **Synchronize clipboard while BeamIt is active**,
or use **Send clipboard** / **Get clipboard** manually. Manual transfers work
with automatic synchronization disabled. Keep the intended BeamIt window
active; changing focus, disabling sync or disconnecting cancels pending work.
Ctrl+V/Shift+Insert waits for host clipboard publication before forwarding
paste to the same foreground stream.

Both directions support text, HTML, transparent PNG and multiple ordinary
files/folders, including nested, empty and hidden entries. Automatic bulk
transfer requires regular-file bytes **< 500,000,000 AND roots <= 5**.
A folder is one root, with all descendant bytes counted. Otherwise a
Transfer/Cancel prompt appears before payload. Progress/Cancel is visible
for automatic transfers too. Source changes discard old confirmation.
Completion means receiver native publication, not queued bytes.

Close the app before editing nonsecret
`%LOCALAPPDATA%\BeamIt\windows-client.json`. Set integer
`clipboard_bulk_byte_threshold` (0–2,000,000,000) and
`clipboard_bulk_entry_threshold` (0–64); restart to load them.
Invalid values produce diagnostics and fall back to 500,000,000/5. These
preferences do not increase the hard limits or add a new settings UI.

Limits, unsupported entries, shared quota and manual cleanup are documented
in [the main Clipboard section](../../README.md#clipboard). Windows host and
client share `%LOCALAPPDATA%\BeamIt\ClipboardStaging`; do not delete published
`host/committed` or `client/committed` selections until the clipboard is
cleared and **all** consumers have closed. Published files are never
automatically deleted. Rollback preserves supported native formats where
possible, not arbitrary delayed-render formats; rollback failure is not
transfer success and may require retaining referenced files.

The optional protocol extension uses pinned, client-opened `/bulk` WSS in
both directions. Only `unknown_type` from an old host enables legacy
text/HTML/base64-PNG fallback; files are unavailable there. There is no resume
or cut/move behavior. See [PROTOCOL.md §5.11](../../PROTOCOL.md#511-clipboard).

### Focused clipboard checks

Build the core/tests with the existing script below, then select:

```powershell
& .\clients\windows\build\core\Debug\BeamItClientTests.exe `
  '[clipboard-manager],[clipboard-settings],[client-session]'
```

For an existing isolated core cache, substitute that cache's executable.
The WinUI project can link it without changing toolset/SDK:

```powershell
& '<registered VS installation>\MSBuild\Current\Bin\MSBuild.exe' `
  .\clients\windows\app\BeamIt.Windows.vcxproj `
  /p:Configuration=Debug /p:Platform=x64 `
  /p:BeamItCoreBuildDirectory='<absolute isolated core cache>' /verbosity:minimal
```

Use a fresh shell or the matching VS2026 developer environment for that
MSBuild. An inherited VS2022 `VisualStudioVersion=17.0` can incorrectly
select v17 PRI tasks even when the v18 tools are installed.

Focused authenticated mock-WSS checks and the v145 WinUI build pass on the
development machine. They do **not** verify the real OS clipboard, UI,
Explorer/application paste, LAN or concurrent video/input behavior.
Before release, run both-direction native tests with disposable files:
verify text/HTML and PNG alpha; nested/empty/hidden folders; five/six roots
and exact byte threshold; decline/Cancel; source/destination/focus changes;
disconnect/restart; and pasted-reference retention after clipboard
replacement. During a representative large transfer, measure control/input
latency and video behavior. Native/LAN/stress results remain **NOT RUN**.

## Windows prerequisites

- Windows 11 x64.
- Visual Studio (Build Tools) 2026 with the C++ desktop workload (MSVC v145,
  Windows SDK `10.0.26100.0`, bundled CMake 4.3) and the WinUI C++
  components. On Build Tools 2026, run this from an elevated prompt:

  ```powershell
  & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\setup.exe" modify `
    --installPath 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools' `
    --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    --add Microsoft.VisualStudio.ComponentGroup.UWP.BuildTools `
    --passive --norestart
  ```

  These components provide the `AppxPackage` PRI tasks that the WinUI build
  needs, even for this unpackaged app. Substitute the actual registered
  installation path; keep it quoted. Keep the project's required Windows
  SDK installed. Do not retarget the project to bypass a missing toolset.
- The x64 Windows App Runtime is **not** required on clients: the app is built
  self-contained (`WindowsAppSDKSelfContained`), so its output folder carries
  the Windows App SDK 2.6.5 binaries. The project pins `Microsoft.WindowsAppSDK` `2.6.5` and
  `Microsoft.Windows.CppWinRT` `3.0.260818.1`. C++/WinRT 3 removes only the
  pre-standard coroutine support, which this code does not use.
  `scripts\Build-Release.ps1` also copies the app-local Visual C++ runtime DLLs
  into `dist\BeamIt-Client-Windows\`, so that folder runs on a clean Windows 11 PC.
- NuGet access to nuget.org for the first restore, and network access for
  the first CMake configure (Boost, boost-wintls and Catch2 are downloaded and
  pinned by hash/tag).
- The HEVC Video Extension, to decode streams. Without it, HEVC streams fail
  visibly ("No installed Media Foundation decoder accepted the stream").

Run unelevated; the manifest requests `asInvoker`. The app is unpackaged
x64; MSIX packaging and ARM64 are not supported.

The app has no XAML markup, so `App` in `app\Main.cpp` implements
`IXamlMetadataProvider` by forwarding to `XamlControlsXamlMetaDataProvider`.
Without it the Fluent `XamlControlsResources` fail to load and XAML terminates
the process about 10 s after launch (`0xC000027B` in `Microsoft.UI.Xaml.dll`).
Unhandled XAML exceptions are appended to `%LOCALAPPDATA%\BeamIt\client-crash.log`.

## Build and test

From PowerShell at the repository root:

```powershell
.\scripts\Build-WindowsClient.ps1 `
    -CMakeExecutable 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' `
    -Configuration Debug   # or Release
```

The script configures and builds the core with CMake (`Visual Studio 18 2026`,
x64). It runs the frame checks and the Catch2 suite (`BeamItClientTests`),
restores NuGet packages and builds the WinUI solution with MSBuild. Both
configurations use the dynamic runtime (`/MDd`, `/MD`). Output goes to
`clients/windows/build/` (Git-ignored):

```powershell
.\clients\windows\build\app\Debug\BeamIt.Client.exe
.\clients\windows\build\core\Debug\BeamItClientTests.exe
```

`BeamItClientTests` covers URL/fingerprint/hello/offer validation (including
audio), proofs,
the clock estimate, host-window grouping/search/following, key mapping and,
against an in-process mock WSS host with an ephemeral self-signed
certificate, pinning before the upgrade, password and forged-proof
failures, request routing, supersession, stream sockets, key-frame requests
and held future-generation frames.

### Decoder fixtures

`VideoPipelineTests` decode single-IDR Annex B fixtures from
`clients/windows/build/fixtures` and are skipped when that directory is
missing. FFmpeg is a development tool for generating them, not a client
dependency. The fixtures have no B-frames, like host streams:

```powershell
$out = 'clients\windows\build\fixtures'; New-Item -ItemType Directory -Force $out | Out-Null
$src = '-hide_banner','-y','-f','lavfi','-i','testsrc2=size=1920x1080:rate=60','-frames:v','1'
ffmpeg @src -c:v libx264 -tune zerolatency -bf 0 -profile:v high -pix_fmt yuv420p -f h264 "$out\h264-high-420.annexb"
ffmpeg @src -c:v libx264 -tune zerolatency -bf 0 -profile:v high444 -pix_fmt yuv444p -f h264 "$out\h264-high444.annexb"
ffmpeg @src -c:v libx265 -tune zerolatency -pix_fmt yuv420p -x265-params bframes=0 -f hevc "$out\hevc-main-420.annexb"
ffmpeg @src -c:v libx265 -tune zerolatency -pix_fmt yuv420p10le -x265-params bframes=0 -f hevc "$out\hevc-main10-420.annexb"
ffmpeg @src -c:v libx265 -tune zerolatency -pix_fmt yuv444p -x265-params bframes=0 -f hevc "$out\hevc-main444.annexb"
```

`BeamItDecoderProbe.exe --list` and `BeamItDecoderProbe.exe <h264|h265> <width>
<height> <fixture>` remain available as standalone diagnostics. Standard
MFTs are labeled as such; they may still decode with DXVA on the D3D11 device
they are given.

### End-to-end test against a host

The hidden `[.e2e-host]` test streams the host's primary monitor (DXGI and WGC)
as HEVC, decodes and presents 30 pictures, then stops the stream. It sends no
input:

```powershell
.\scripts\Start-BeamItServer.ps1 -Instance wintest -Port 17489 -Bind 127.0.0.1
$env:BEAMIT_E2E_URL = 'wss://127.0.0.1:17489'
$env:BEAMIT_E2E_PASSWORD_FILE = "$env:LOCALAPPDATA\BeamIt\wintest\password.txt"
$env:BEAMIT_E2E_FINGERPRINT = '<fingerprint printed by the script>'
.\clients\windows\build\core\Debug\BeamItClientTests.exe "[.e2e-host]"
```

## Verified results (development machine)

Windows 11 26100, Build Tools 2026 18.7 (MSVC 14.51), NVIDIA RTX A1000 Laptop
GPU (32.0.15.9658) plus Intel UHD, Windows App Runtime 2.6.5:

- Debug and Release builds, frame checks and `BeamItClientTests` pass. The host
  build and `BeamItTests` also pass after the shared dependency and
  `Crypto.cpp` (`std::format`) changes.
- Interactive scaffold checks passed (surface, system menu, fullscreen, tray,
  Explorer restart, owned popup) before it was replaced by the production
  windows.
- Media Foundation fixtures, all decoded to a GPU surface on the NVIDIA
  device: H.264 High 4:2:0 decodes (coded 1920×1088, cropped).
  **H.264 High 4:4:4 is rejected (E_FAIL)**. HEVC Main, Main 10, 4:4:4 8-bit
  and 4:4:4 10-bit decode with HEVCVideoExtension; HEVC 4:2:2 10-bit
  produced no picture. A truncated H.264 IDR still produced a concealed
  picture, so decoder success does not prove bitstream integrity.
- Local host, end-to-end: D3D12 HEVC Main 4:2:0, 3440×1440, 60 FPS, DXGI
  and WGC. HEVCVideoExtension decoded NV12 on the GPU in about 0.3–1.2 ms per
  picture, and 30 pictures were presented. The capture-to-enqueue estimate
  (about 31–37 ms, including test-loop polling) is an upper bound, not a
  latency measurement.

Not yet verified: the production app's interactive behavior with a host on
another LAN machine, input on a separate desktop, Teams windows/popups,
mixed-DPI placement, Intel-only or AMD decoding, and HEVC-less machines.
