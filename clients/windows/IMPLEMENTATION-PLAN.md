# Windows client: implementation plan and WSL handoff

## Resume instruction

Give Copilot this instruction in the target machine's BeamIt checkout:

> Read the repository agent instructions, `clients/windows/IMPLEMENTATION-PLAN.md`,
> `clients/windows/README.md`, and `PROTOCOL.md`. Resume the Windows client work
> at the scaffold validation gate. Inspect the current worktree before editing
> and preserve unrelated macOS/README changes. The scaffold has not been built
> on Windows. First establish Windows toolchain access from WSL, build and fix
> the prototype, run its tests, and arrange the interactive Windows checks.
> Do not mistake enumeration or a blue test surface for streaming support.
> After those gates pass, implement the remaining parity features in the order
> below. Keep the host protocol and selected product decisions unchanged.
> Maintain task tracking and update this handoff with actual results and blockers.

This file is the portable handoff. The original Copilot session's `plan.md`
and SQL todos are local session artifacts and must not be assumed to exist
on the target machine.

## 1. Objective and confirmed decisions

Build a Windows 11 client matching the **current working-tree macOS client**
in `clients/osx`, adapting global menus to Windows conventions.

| Area | Confirmed choice |
|---|---|
| Platform | Windows 11+, x64 initially |
| UI | WinUI 3 with C++/WinRT and C++26 |
| Native interop | HWND system menus, tray, input, owned popups, taskbar attention |
| Video | Media Foundation decoding, D3D11 presentation in WinUI |
| Decoder policy | Hardware first, then installed MF software decoders; report actual mode |
| Unsupported profiles | Visible stream failure; no FFmpeg, silent codec switch or host chroma downgrade |
| Build | Separate WinUI MSBuild solution; client-only native core/test CMake entry point |
| Deployment | Unpackaged x64 app; documented Windows App SDK runtime prerequisite |
| Session model | One host connection per client process, multiple streams |
| Multiple hosts | Separate client processes for separate host instances |
| Global UI | System tray for application/session options |
| Stream controls | Native window system menu; Alt+Space also opens it in fullscreen |
| Lifecycle | Closing connection window hides to tray; closing stream stops it and dependent popups; tray Exit shuts down all resources |
| Security | Certificate fingerprint pinning by default; explicit warned insecure bypass retained for parity |
| Preferences | Clipboard sync and anti-idle are global, initially off |
| Shortcuts | Alt+Space local menu; Ctrl+Alt+G always releases capture; double Escape within one second exits fullscreen |
| Input | Windows modifier conventions; OS-reserved shortcuts remain local |
| Out of scope | Audio, gamepads, file transfer, cloud/NAT/discovery, auto-reconnect, password storage, ARM64, installer/MSIX, new host capabilities |

Do not reopen these choices unless a concrete blocker requires a product
decision. Document the blocker/tradeoff and ask before changing architecture
or wire behavior.

### Media compatibility caveat

The host selects sampling based on host capabilities, not client decoder
negotiation. The development T1000's default H.264 path uses NVENC High444.
Windows MF decoders may not support this. HEVC decoding may be absent, and
range-extension profiles may be unsupported even with HEVC installed.

An unsupported stream must fail visibly, release host resources, and leave
the authenticated session usable. Explain codec/profile/chroma/bit depth and
decoder availability. Do not add a codec picker, automatic retry using another
codec, host fallback, or protocol negotiation as a workaround without approval.
Supported fixtures prove positive playback; unsupported fixtures prove
honest failure handling. Neither proves universal codec/GPU coverage.

## 2. Exact stopping point

Implementation was approved and **only the first scaffold/prototype phase
was started**. The user deferred Windows validation while on the macOS machine.
This is not a functional Windows streaming client.

### New files created

| Files | Purpose |
|---|---|
| `clients/windows/BeamIt.sln` | Debug/Release x64 Visual Studio solution |
| `clients/windows/app/BeamIt.Windows.vcxproj` | WinUI app; links native core; pinned NuGet references |
| `clients/windows/app/app.manifest` | Unelevated execution and per-monitor-v2 DPI |
| `clients/windows/app/Main.cpp` | Programmatic WinUI application startup |
| `clients/windows/app/PrototypeWindow.hpp/.cpp` | Tray, native menu, fullscreen, owned popup integration prototype |
| `clients/windows/app/Surface.hpp/.cpp` | D3D11 blue test surfaces in SwapChainPanel and popup HWND |
| `clients/windows/core/CMakeLists.txt` | Isolated core, Windows probe, portable frame tests |
| `clients/windows/core/FrameProtocol.hpp/.cpp` | Frame parser and generation/reference/keyframe gate |
| `clients/windows/core/DecoderProbe.hpp/.cpp` | MF enumeration and single-IDR decode diagnostic |
| `clients/windows/core/DecoderProbeMain.cpp` | Local diagnostic CLI; not a network transport |
| `clients/windows/tests/FrameProtocolTests.cpp` | Standalone frame-contract regression executable |
| `clients/windows/.gitignore` | Generated Windows outputs |
| `clients/windows/README.md` | Prototype limitations, prerequisites, build/run/check steps |
| `scripts/Build-WindowsClient.ps1` | Explicit-CMake core build/test, NuGet restore and MSBuild |

The root README's native-client status sentence was also corrected.
No host implementation, protocol, root CMake dependency extraction, or
macOS implementation was changed for the Windows work.

### What has actually been checked

- Portable `FrameProtocol.cpp` and its tests compiled and passed on macOS
  with Apple Clang 21, C++2c, strict warnings, AddressSanitizer and UBSan.
- Project/manifest XML parsed; referenced source files existed.
- New files were checked for whitespace/newline issues; `git diff --check`
  passed for tracked changes.

### What has NOT been checked or implemented

- No Windows restore, compile, link, app launch, GPU probe or interactive test.
- NuGet package/toolset/bootstrap/API compatibility remains unverified.
- The diagnostic does not constitute the production decoder.
- The blue surfaces do not render remote content.
- The popup is a fixed-size local prototype, not remote geometry coordination.
- No production transport/authentication, source picker, streaming integration,
  input, clipboard, text sending, anti-idle, rates or diagnostics UI.
- No Windows end-to-end/Teams validation.

The scaffold core currently has no third-party dependencies and the initial
tests use a small standalone runner, not Catch2. Networking dependency sharing
and the planned Catch2 test integration remain future work.

### Worktree preservation and transfer

At handoff the Windows files and build script were **untracked**, with no
commit created. Root README also contained pre-existing user edits.
Several macOS files were modified or untracked, including recent controller,
decoder, input, fullscreen and anti-idle additions. Treat them as the parity
baseline, not disposable experiments.

Transfer the actual working-tree changes, not just a clean clone or tracked
diff. A normal `git diff` excludes the new Windows files and untracked macOS
sources. The user should choose their normal commit/sync/copy mechanism;
do not commit or overwrite unrelated work automatically. Exclude credentials,
certificates/private keys, build outputs, `.DS_Store`, and IDE user state.
Compare target worktree state before applying transferred changes.

## 3. Start on the target WSL machine

### Establish the real environment

1. Confirm repository root, branch, `git status --short`, and that all files
   in the scaffold table and current macOS source baseline are present.
2. Confirm WSL and Windows interop: inspect `uname -s`, `/mnt/c`, and
   availability of `powershell.exe` or `cmd.exe`. Do not assume the old macOS
   `/Users/...` paths apply.
3. Locate Windows Visual Studio 2022 through `vswhere.exe`, the Windows SDK,
   the x64 Windows App Runtime, and the **Windows IDE-managed CMake executable**.
   Do not use Linux CMake/MSVC substitutes to claim a native WinUI build.
4. Prefer a Windows-accessible checkout on an NTFS drive such as
   `/mnt/c/.../BeamIt`. If the repo is in WSL's Linux filesystem, deliberately
   arrange a Windows-native build location/path; do not assume MSBuild or
   NuGet handles UNC source paths correctly.
5. Record actual versions and paths. Install/restore dependencies only when
   the selected command reports them missing, following repository rules.

### Build through Windows PowerShell

From WSL in a Windows-accessible checkout, the pattern is:

```bash
repo_win="$(wslpath -w "$PWD")"
build_script="$(wslpath -w "$PWD/scripts/Build-WindowsClient.ps1")"
powershell.exe -NoProfile -File "$build_script" \
  -CMakeExecutable 'C:\ACTUAL\IDE-CMAKE\bin\cmake.exe' \
  -Configuration Debug
```

Replace the CMake placeholder after locating the installed IDE-managed copy.
`repo_win` is useful for subsequent PowerShell commands; it is not consumed
by the script. The script derives source/build paths from its own location.
It needs CMake 4.3+, Windows SDK `10.0.26100.0`, and VS 2022/v143.

The current app pins:

- `Microsoft.WindowsAppSDK` `1.7.250606001`.
- `Microsoft.Windows.CppWinRT` `2.0.240111.5`.

Treat these pins as scaffold inputs requiring real restore/build verification,
not as proof that the complete project is configured correctly. Use official
SDK documentation and installed templates/headers to resolve build issues;
do not guess compatibility or suppress real errors.

Read `clients/windows/README.md` for executable paths and decoder commands.
Run interactive WinUI checks in the logged-in Windows desktop, not a headless
service context or a Linux/WSLg substitute.

## 4. Mandatory first gate: fix and validate the scaffold

Keep task `win-scaffold` active until the evidence below exists.

### Compile/link checks

- Restore/build Debug using the script, fix real failures, then repeat Release.
- Verify C++26 preview settings, WinUI programmatic startup, generated C++/WinRT
  support, runtime bootstrap/manifest/deployment, SDK headers and link libraries.
- Check both configurations use matching x64 core/app runtime settings
  (`/MDd` Debug, `/MD` Release).
- Run `BeamItFrameProtocolTests.exe` in both configurations. Add regression
  cases for any changed frame behavior.
- Inspect MF transform ownership, events, sample allocation, output negotiation,
  drain/stream-change handling and cleanup against actual SDK contracts.
- Distinguish a diagnostic candidate rejection from a global device/allocation/
  operational error; do not hide operational failures as unsupported hardware.

### Interactive WinUI/Win32 checks

- D3D11 SwapChainPanel blue surface initializes, resizes and survives monitor
  moves. Check resource ownership, sizing and device-error reporting.
- Title-bar icon menu and Alt+Space preserve ordinary system commands.
- Fullscreen keeps local menu access; double Escape restores window state.
  Test with buttons focused and the video region focused, avoiding duplicate
  key delivery through XAML and HWND paths.
- Close hides to tray; Show restores; Exit terminates and removes the icon.
- Explorer restart restores the icon. A registration failure must leave a
  visible recovery window.
- Owned popup does not activate or appear in the taskbar; owner activation,
  deactivation, movement, minimization and teardown work.
- Explicitly check HWND destruction/subclass lifetime, callback exceptions,
  swap-chain/frame-latency resources and delayed UI callbacks.

### Media Foundation checks

- `--list` enumerates installed hardware/software candidates accurately.
- Feed locally available complete single-IDR Annex B fixtures with the actual
  dimensions and parameter sets; no removed recorder/test transport should
  be reintroduced to obtain them.
- Positive cases: supported H.264 and supported HEVC where available.
- Negative cases: unsupported H.264 High444, unsupported HEVC range-extension
  profiles, invalid/truncated fixture, and missing HEVC installation.
- Record candidate/result/HRESULT and GPU-vs-system-memory output. Enumeration
  alone is not success. A successful software decode is not hardware evidence.
- The current diagnostic runs synchronously; its app invocation is only a
  prototype. Production probing/decoding must not block WinUI.

If interactive checks require user participation, request a bounded set of
actions and record results. Do not mark the gate passed on compilation alone.
If the selected framework/decoder policy genuinely blocks the requirement,
ask for a decision rather than silently changing it.

## 5. Production implementation sequence

Add tests with each component; the final validation phase is not the first
time behavior should be tested.

### A. Authenticated signaling and stream state (`win-session`)

Prerequisite: scaffold gate passed.

- Reuse `src/net/Crypto.hpp/.cpp` and `src/net/Protocol.hpp`.
- Reuse the existing checksum-pinned Boost.Beast/JSON and boost-wintls setup
  through minimal shared dependency declarations. Keep the client dependency
  graph free of capture/encoding/HTML assets and NVENC SDK extraction.
- Verify boost-wintls peer certificate inspection/client validation hooks.
  Pin the certificate on control **and stream** sockets before sending proofs
  or accepting video. Support the explicit insecure setting with a warning.
- Implement PBKDF2/HMAC mutual nonce proof validation and typed JSON routing,
  request IDs/expected replies, deadlines, errors, authentication state,
  ping/pong, clock sampling, cancellation and superseded close code `4001`.
- Enforce protocol limits: 64 KiB pre-auth signaling, 16 MiB authenticated,
  eight streams including popups, short-lived single-use stream tokens.
- Validate same-origin WSS offers. Never log passwords, proofs, derived keys
  or token-bearing URLs; never persist passwords.
- Integrate binary parser/generation gate. Cross-socket ordering is not
  guaranteed: future-generation video awaits/drops until matching config,
  then requests/waits for an IDR if necessary.
- Keep writes serialized, buffers bounded, and late completions harmless
  after stop/disconnect. Do not drop arbitrary compressed reference pictures.

### B. Connection/source UI and tray (`win-shell`)

Prerequisite: signaling.

- Connection screen: secure URL, password, fingerprint, explicit insecure
  warning, progress and actionable errors.
- Match current macOS application search by title/window/PID, desktop DXGI/WGC,
  application selection, Add Stream, active-stream Show/Stop and Disconnect.
- Persist only successful endpoint/fingerprint/trust preference and nonsecret
  settings; clear password fields after connection.
- Tray: Open Connection/Add Stream; explicitly targeted Active Streams;
  Clipboard Sync; both manual clipboard directions; Anti-idle; Disconnect; Exit.
- Disable unauthenticated commands. Hide connection window on close, preserve
  tray recovery, and show picker when last top-level stream closes or errors
  require action. Keep one connection per process.

### C. Production decoder/presentation and stream windows (`win-media`)

Prerequisite: signaling and validated media prototype.

- MF hardware-first then installed-software decoding; actual mode is visible.
- Implement Annex B/parameter sets, negotiated profile/output/size, low-latency
  configuration, async event/sample lifetimes and generation reset.
- D3D11 device-manager decode to GPU presentation where supported; bounded
  upload if MF software output needs it. No routine CPU readback.
- WinUI per-stream window with remote title/icon, `FlashWindowEx` attention,
  black letterboxing, resize/fullscreen state and error cleanup.
- 1x means one remote video pixel per local physical display pixel, including
  mixed DPI; optional aspect-preserving scaling and 0.5x/1x/2x sizing.
- Coalesce decoded frames only. Invalidate stale decoder callbacks across
  reconfiguration/close; retain resource lifetimes through GPU completion.
- Unsupported profile and device failure are explicit failures, not fake
  success or hidden codec/backend changes.

### D. Windows input and local-command arbitration (`win-input`)

Prerequisites: connection/tray UI and media windows.

- Map physical scan codes/extended keys and left/right Windows modifiers to
  `KeyboardEvent.code`; do not copy macOS Command/Ctrl remapping.
- Absolute pointer coordinates use the displayed video rectangle, not title
  bars/letterboxing. Support buttons and wheel with correct ordering.
- Focused raw relative input, cursor clip/hide/restore and acknowledged host
  `set_mouse_mode` transitions. Unknown mode blocks input.
- Avoid duplicate XAML/Win32/raw-input delivery. Reserve local menu/release/
  fullscreen shortcuts before forwarding; remote right-click stays remote.
- Release held keys/buttons/capture on focus loss, menus/dialogs, capture loss,
  errors, stream close and disconnect. OS secure/reserved shortcuts stay local.

### E. Remote popup coordinator (`win-popups`)

Prerequisites: source UI and media windows; validate interaction with input.

- Match `BeamItPopups.swift` ownership-chain/same-PID policy, cycle protection,
  sole/focused unowned-parent selection, duplicate suppression and attempt
  tracking. No new arbitrary cross-process family discovery.
- Separate owned borderless nonactivating video surfaces, no taskbar buttons.
- Position from host physical rects relative to the displayed parent video,
  respecting scaling, fullscreen bars, mixed DPI and negative monitor coords.
- Handle nested popups, minimization/role changes, parent focus/close, async
  open cancellation, stale offers and stream slot failures.
- Normal secondary windows remain selectable per the macOS baseline; do not
  automatically stream every normal top-level window.

### F. Clipboard, paced text and anti-idle (`win-utilities`)

Prerequisite: input and session lifecycle.

- Focus-gated/debounced host clipboard downloads; explicit manual directions;
  revision/sequence checks prevent stale overwrite.
- Upload local clipboard and wait for `clipboard_set` before forwarding Ctrl+V.
- Text/CF_HTML/PNG/DIB conversions, sensitive/exclusion flags, 10 MiB transfer
  budget, no file transfer, no echo loop, visible partial/invalid failures.
  Inspect existing `src/host/Clipboard.*` helpers before duplicating conversion.
- Text dialog: paste/file load off UI thread, normalized newlines/control
  characters, approximately 8 ms character pacing, Stop and cancellation.
- Anti-idle initially off; most recently focused visible top-level stream,
  only while no stream is focused. Match 30 Hz smooth five-second segments
  inside a central 20%-diameter region; suppress during local menus/dialogs
  and unknown mouse mode, reset position caches on stop.

### G. Per-stream controls and diagnostics (`win-options`)

Prerequisites: media/input/popups/utilities.

- Bind native menu commands to the **owning stream**, not last-active global
  state: capture/release; scaling toggle and sizes; fullscreen; parameters;
  info; Send/Stop Text; standard close.
- Popup surfaces inherit parent options rather than independent menus.
- Rate controls: average 1..1,000,000 kbps; FPS 1..240; explicit peak up to
  4,000,000 kbps; omitted peak preserves policy, zero requests automatic peak.
  Report acknowledged effective values; Cancel cannot undo submitted changes.
- Diagnostics: codec/profile/chroma/bit depth, generation/size, host encoder,
  effective rate policy, decoder hardware/software and output path,
  receive/decode/present-enqueue rates, coalescing, timing and host stats.
- Use fresh clock estimates with RTT uncertainty/sample age. Enqueue timing
  is not display completion; do not claim verified one-frame latency.

### H. Final parity verification and documentation (`win-validation`)

Prerequisite: all components; maintain focused tests throughout.

- Add Catch2/client mocks without requiring host encoder hardware/NVENC SDK.
- Authentication/pin/errors/origin/limits/supersession tests.
- Frame generation/keyframe/discontinuity/resize/lifetime/bounded-queue tests.
- Input focus/mode/capture/shortcut/menu cleanup tests.
- Popup ownership/races/geometry/limits and taskbar/activation checks.
- Clipboard conversions/budgets/exclusion/stale replies/paste order tests.
- Text normalization/cancellation; anti-idle gating; rate/peak validation;
  diagnostics freshness and honest labels.
- Windows Debug/Release build and supported hardware/software playback;
  negative unsupported-profile/missing-decoder cases.
- Real Teams main window, context menus, secondary windows, move/resize/
  fullscreen/minimize, keyboard/mouse, clipboard, icons and taskbar attention.
- Multiple independent host instances through multiple client processes;
  separately confirm same-instance supersession semantics.
- Run focused existing host tests if shared crypto/protocol/build declarations
  change. Use existing formatting/diagnostics; avoid broad unrelated fixes.
- Update Windows README and relevant root README status/architecture guidance.
  Keep protocol v1 unchanged unless a separately approved wire change is made
  together with the host.

## 6. Reference map for feature parity

All Swift paths below are under `clients/osx/BeamIt/BeamIt/`.

| Reference | Behavior |
|---|---|
| `BeamItApp.swift`, `ContentView.swift` | Commands, source selection, search, streams |
| `BeamItConnection.swift`, `BeamItConnectionModels.swift`, `CertificatePinDelegate.swift` | Session/authentication/TLS/models |
| `BeamItVideoStream.swift`, `BeamItVideoDecoder.swift` | Receive/config/decode |
| `BeamItStreamWindowController.swift`, `BeamItStreamSurface.swift`, `BeamItFrameView.swift` | Windows, scaling, focus, frame coalescing/presentation |
| `BeamItPopups.swift` | Popup lifecycle and placement policy |
| `BeamItInput.swift` | Physical keys, capture, paste ordering, release-all |
| `BeamItClipboardSync.swift` | Clipboard policies and cancellation |
| `BeamItSendText.swift` | Text normalization and pacing |
| `BeamItAntiIdle.swift` | Focus gating and movement math |
| `BeamItStreamParameters.swift` | Rate controls/units/peak behavior |
| `BeamItStreamInfo.swift`, `BeamItHostStats.swift`, `BeamItClockEstimate.swift` | Diagnostics/statistics/clock |
| `clients/osx/BeamIt/Tests/` | Behavioral regression references |

The current macOS presenter is `AVSampleBufferDisplayLayer`, not Metal.
Root README has other stale historical architecture claims; inspect current
source and normative `PROTOCOL.md`, not historical descriptions alone.

## 7. Task reconstruction and completion rules

Recreate session-local tasks if necessary; markdown is the portable source.

| ID | Status at handoff | Prerequisites |
|---|---|---|
| `win-scaffold` | Blocked on deferred Windows validation | None |
| `win-session` | Pending | `win-scaffold` |
| `win-shell` | Pending | `win-session` |
| `win-media` | Pending | `win-session` and scaffold media gate |
| `win-input` | Pending | `win-shell`, `win-media` |
| `win-popups` | Pending | `win-shell`, `win-media` |
| `win-utilities` | Pending | `win-input` |
| `win-options` | Pending | `win-popups`, `win-utilities` |
| `win-validation` | Pending | `win-options` |

On the target, change `win-scaffold` to in progress once access is established.
Record actual commands/results and narrowly scoped unresolved failures in this
file or the active session artifact. Do not mark the client complete until
native Windows builds, automated checks, and required interactive/hardware
outcomes are verified. List unverified Teams/GPU cases explicitly.

Do not commit unless requested, do not revert existing work, and do not
reintroduce removed recorder/plaintext transports or host CLI modes.
