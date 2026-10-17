# BeamIt

BeamIt is a **low-latency Windows application-remoting host** for mirroring a selected application's windows on another computer as native client-side windows. Full-desktop capture remains available for testing and desktop scenarios.

The project is intentionally close to the problem space of RDP and Sunshine, but with a narrower scope:

- **Host OS:** Windows 11 and newer only
- **Client OS:** macOS first, Windows afterward
- **Language:** C++26
- **APIs:** prefer recent Windows APIs; avoid legacy approaches unless there is a strong reason
- **Input support:** keyboard and mouse only
- **Audio:** optional
- **Gamepad support:** not in scope
- **Local GUI:** not required
- **Remote client:** custom seamless-remoting client; no Moonlight compatibility requirement
- **Network:** strictly LAN-only; Internet connectivity will never be a requirement

## Current status

This repository has full-monitor DXGI capture, monitor or single-window WGC capture, and automatic codec-preserving selection between D3D12 Video Encode and native NVENC DirectX 11 encoding, prioritizing 4:4:4 sampling.
The host serves the authenticated **WSS protocol** in [`PROTOCOL.md`](PROTOCOL.md) : one JSON signaling WebSocket plus one binary WebSocket per video stream, over TLS with a shared password. This is the executable's only mode; the earlier recorder, plaintext TCP test streamer, window listing and capability probes were removed. Remote keyboard and mouse input is routed per stream over the signaling socket. The `HTML/` browser test client is served at `/`. The host reports every top-level window (`window_opened`/`window_changed`/`window_closed`) so clients can choose secondary windows to stream. Native clients are not implemented.

### Current near-term goal

The immediate goal is to mirror **Microsoft Teams** to a custom macOS client, with its main and external/secondary windows presented as native local windows. A Windows client follows. Taskbar attention on Windows and Dock attention on macOS are in scope; native notification forwarding is not required for this milestone.

Both capture backends expose frames to a generic encoder interface. Single-window and desktop streams can be viewed with the `HTML/` test client. Next, define the macOS client and window/control protocol, then implement WGC application-window tracking beyond a single HWND. Broader GPU/driver coverage still needs testing. Audio and gamepad support remain out of scope for this milestone.

## Project goal

Build a host application that can:

1. Capture the desktop or a chosen app/window with minimal latency
2. Encode video efficiently on Windows 11+
3. Send video, input, and optionally audio to a remote client
4. Keep end-to-end delay as low as possible while remaining stable

## Client and session direction

BeamIt is a seamless application-remoting system, not a Moonlight-compatible streaming host. A conventional Moonlight or browser video surface does not provide the required native window lifecycle and attention integration, so those are no longer the client target.

Start with the macOS client, retaining Windows client support as a design requirement. The client must associate stable remote window identifiers with local windows and synchronize creation/closure, visibility, ownership, geometry, titles/icons, focus and attention, alongside video and keyboard/mouse input. Application selection must include secondary windows and related processes, not just one HWND. Exact window grouping, popup behavior and rendering/stream layout remain design decisions.

AppKit windows with VideoToolbox decoding and Metal presentation are a candidate macOS-native approach, not yet a selected implementation. Compare that with a cross-platform client framework before committing; macOS-first does not by itself choose a language or UI toolkit.

### LAN access and multiple hosts

The system is strictly LAN-only. Do not add cloud services, Internet endpoints, NAT traversal or port forwarding requirements. Multiple independent BeamIt server instances may run on the same Windows machine; each needs independently configurable listen endpoints, application selection and credentials. Never assume a single fixed machine-wide port or session.

Each instance accepts a shared password from a configuration file or command-line parameter. Possession of that instance's password is the combined authentication/authorization policy: no separate user accounts or roles are required. Clients without it must neither decode the stream nor issue accepted input/control commands. Password sharing grants all holders the same instance-level access; there is no individual identity or revocation.

AES-256 is the requested encryption primitive, but an arbitrary password is not a 256-bit key. Before networking implementation, select a standard password-based key derivation and authenticated session protocol. Use authenticated encryption (for example AES-256-GCM), fresh session/directional keys, unique nonces, replay protection, and verified key possession before granting control. Protect window metadata and input/control traffic as well as video. Encryption without integrity or merely failing video decode is not sufficient authentication. Do not invent a cryptographic protocol.

Prefer configuration files with restrictive permissions; command-line secrets can appear in process inspection and shell history. Never log passwords or derived keys. LAN-only deployment does not remove the need for authentication and integrity. The exact KDF, handshake, transport and configuration/CLI syntax are not implemented or selected yet.

## Recommended high-level architecture

The implementation is being organized around the following pieces:

- **Capture pipeline**: desktop/window/app capture on Windows 11+ that emits a common GPU-backed frame type
- **Encoder**: a generic interface accepting frames from either capture backend; automatic sampling-first selection between D3D12 H.264/H.265 and NVENC DirectX 11 H.264 4:4:4
- **Transport**: authenticated, encrypted LAN video and window/control channel(s)
- **Client integration**: native windows, remote-window mapping and taskbar/Dock attention
- **Input injection**: keyboard and mouse handling
- **Audio**: optional capture and streaming path
- **Session management**: connect, disconnect, resync, bitrate/frame pacing
- **Configuration**: resolution, bitrate, FPS, capture target, audio toggle

## Capture API direction

BeamIt should be able to support both of the relevant Windows capture APIs:

- **Windows.Graphics.Capture** as the primary backend for window-oriented capture
- **DXGI Desktop Duplication** as the alternate backend for desktop/monitor capture and fallback scenarios

For the current Teams milestone, **Windows.Graphics.Capture is the better first choice** because the immediate requirement is correct handling of external and secondary windows.
DXGI Desktop Duplication is implemented as a full-monitor capture backend and remains valuable for full-desktop streaming and fallback scenarios.

The application exposes a common pure-virtual `Capturer` abstraction. The backend is chosen per stream: window and application sources always use WGC; desktop sources use DXGI by default, or WGC when the client sets `"capture": "wgc"` in `start_stream`. A stream never switches APIs while running.

Suggested shape of the capture layer:

- `Capturer` as the shared interface for lifecycle and frame acquisition
- `WgcCapturer` for `Windows.Graphics.Capture`
- `DxgiCapturer` for `DXGI Desktop Duplication`

That keeps the rest of the streaming pipeline agnostic to the backend while preserving the ability to choose the most appropriate API for the scenario.

The DXGI implementation uses Desktop Duplication to capture one monitor at a time. A null monitor target selects the primary display; an explicit `HMONITOR` selects another display. It passes the acquired D3D11 surface directly to the encoder without a capture texture allocation or GPU copy. A shared lifetime lease keeps the duplication frame acquired while consumers use it; callers must retain the complete `CapturedFrame`, not just its texture. The encoder retains the frame through GPU completion. `last_error()` distinguishes a no-frame timeout (`S_OK`) from failures. Cursor composition and multi-monitor composition are not implemented yet.

The duplication frame stays acquired until consumers release their leases, then is released immediately before the next nonblocking acquisition (or shutdown). Acquisition returns no new frame while a consumer still holds the previous lease. Keeping `ReleaseFrame` adjacent to `AcquireNextFrame` avoids unnecessary desktop-update copies during encoding/pacing. The test harness releases borrowed frames after encoded output is drained; when DXGI has no update, `Encoder::try_repeat_frame()` encodes the retained GPU image (converted YUV with D3D12, owned BGRA with NVENC) as a new picture with advancing reference/order state. It never replays compressed packets or accesses a released duplication surface. WGC still uses owned texture copies. Capture-to-display latency still needs comparison on the receiving computer.

The WGC implementation captures one monitor or one HWND per capturer instance, with the same null-monitor primary-display default. It uses `CreateForMonitor` or `CreateForWindow`, a free-threaded frame pool, and an owned D3D11 texture for each acquired frame. Cursor capture follows `CaptureOptions::capture_cursor`. Resizing recreates the frame pool and reconfigures the test encoder. Dimensions are rounded down to even pixels for NV12 encoding, dropping at most one pixel at the right/bottom edge. Multi-monitor composition is not implemented; window events (§5.12) let clients stream several windows.

## Frame-to-encoder boundary

Both `WgcCapturer` and `DxgiCapturer` expose frames through the same `CapturedFrame` type. It carries a reference-counted D3D11 texture and capture timestamp.

The pure-virtual `Encoder` interface accepts those frames and exposes encoded packets separately, allowing asynchronous hardware encoders to apply backpressure and return output when ready. `D3D12VideoEncoder` selects H.264 or H.265 explicitly, validates the requested codec/profile/input format/resolution/rate-control mode with the adapter's D3D12 Video Encode capability queries, and does not silently switch codecs.

The D3D12 encoder chooses the same adapter as the incoming capture texture. It uses a shared D3D12 input texture in the selected YUV format, opened on the capture D3D11 device, and a D3D11 video-processor GPU conversion from the captured BGRA frame. Shared fences coordinate D3D11 conversion and D3D12 encode/copy work. Submission is bounded to one in-flight frame; callers receive `WouldBlock` until the previous packet is drained. Encoded bytes are copied back to CPU memory only at the packet handoff.

### Automatic encoder selection (default)

The host always uses `AutoVideoEncoder`, which includes NVENC in its best-codec search: for the default H.264 codec, it tries NVENC High444 before falling back to D3D12 4:2:0. It prioritizes 4:4:4 while preserving the codec the client requested in `start_stream`:

| Requested codec | Initialization priority |
| --- | --- |
| H.264 (default) | NVENC High444 / 8-bit 4:4:4, then D3D12 Main / 8-bit 4:2:0 |
| HEVC (`"codec": "h265"`) | D3D12 8-bit 4:4:4, 10-bit 4:4:4, 10-bit 4:2:2, then 8-bit 4:2:0 |

D3D12's H.264 API exposes no 4:4:4 profile, so that impossible candidate is skipped with a log. NVENC HEVC is not implemented and is not tried for an HEVC request. No codec change is permitted: the selected codec's available 4:4:4 backends are considered before lowering sampling on D3D12.

Selection happens when the first frame supplies the capture device and is repeated on reconfiguration/resize. NVENC availability/profile/configuration rejection (including missing SDK integration or an incompatible/missing driver API) is logged before trying D3D12. Allocation, device, GPU submission and encoding failures abort; they do not trigger backend switching. Once a backend accepts a picture, it stays selected until reconfiguration. Both encoders remain on the capture adapter. There is no option to force a backend.

On the development T1000, default H.264 selects NVENC and produces `yuv444p`; HEVC tries D3D12's higher-sampling candidates and currently falls back to `yuv420p`. The selected sampling is logged. Clients must support the selected profile; automatic host selection does not negotiate client hardware-decoding support. The hidden `[.auto-hardware]` Catch2 test checks the NVENC-to-D3D12 fallback on a non-NVIDIA adapter; on the development Intel adapter, NVENC is correctly rejected and D3D12 is attempted, but D3D12 also rejects encoding with the current driver.

### Native NVENC DirectX 11 encoder

`NvencVideoEncoder` implements the same `Encoder` interface; the automatic H.264 search tries it first when available. NVENC currently requires **8-bit SDR H.264 High 4:4:4 Predictive** and rejects initialization if that profile, 4:4:4 encoding, RGB input, or asynchronous encoding is unavailable, after which the automatic search falls back to D3D12 4:2:0.

The NVENC session uses `NV_ENC_DEVICE_TYPE_DIRECTX` on the capture texture's D3D11 device. Each fresh frame is copied on the GPU to one reusable encoder-owned BGRA texture, registered as NVENC's word-ordered ARGB format (BGRA bytes). NVENC performs RGB-to-YUV conversion for 4:4:4 encoding; no CPU pixel readback, CUDA toolkit, manual planar packing, or packed AYUV intermediate is required. The copy is deliberately encoder-side: it preserves the last image for DXGI idle-frame repeats without accessing a released duplication surface. Capture remains copy-free for DXGI. Capture must already be on an NVENC-capable NVIDIA device; cross-GPU transfer is not implemented.

The P1 ultra-low-latency preset favors throughput over compression efficiency. Encoding uses CBR with a one-frame VBV, a 60-picture I/P GOP, one reference frame, no B-frames, no lookahead, no temporal AQ, and no multipass. Async Windows completion events bound submission to one outstanding picture; output is Annex B with AUD and repeated SPS/PPS on IDRs. The complete captured-frame lease is retained through completion, with a D3D11 copy fence also protecting shutdown/error cleanup. Reconfiguration rebuilds the session for resized windows; the next picture is an IDR.

Both WGC and DXGI 1080p60 recordings made with the former recorder on the T1000 decoded as `High 4:4:4 Predictive`, `yuv444p`, with 5 I-frames and 295 P-frames per five-second recording and no B-frames. This does not prove one-frame end-to-end latency or hardware decoding on macOS: High444 support must be checked on the intended client, and software decoding can increase latency. HDR and HEVC NVENC encoding are not implemented.

The SDK integration described below must be enabled. Driver-independent lifecycle/configuration tests run with the normal test suite; run the hidden `[.nvenc-hardware]` Catch2 tag through CLion's `BeamItTests` configuration for D3D11 submission/repeat, backpressure, lease release, in-flight shutdown and session-recreation checks on an NVIDIA GPU.

### Automatic chroma sampling

Capability data below was collected with a former `--probe-encoders` diagnostic (since removed), which queried D3D12 H.264 Main/NV12, the HEVC sampling candidates and Main10/P010 on every hardware adapter for a 1920x1080 CBR I/P configuration (GOP 60, one reference, no B-frames). Such queries are not proof of working conversion, resource interoperability, live encode or cross-adapter transfer.

On the development system, the 1080p60/24,000 kbps query reported:

| Adapter | D3D12 query results |
| --- | --- |
| NVIDIA Quadro T1000 (`10DE:1FB9`) | H.264 Main/NV12, HEVC Main/NV12 and HEVC Main10/P010 I/P supported; queried HEVC 4:4:4/4:2:2 profiles unsupported |
| Intel UHD Graphics (`8086:9BC4`) | H.264 and HEVC encoder codec support reported false through D3D12 |
| Microsoft Basic Render Driver | Software adapter, skipped |

These results describe this driver/API combination, not every API capability of the silicon. Prefer D3D12 when it exposes the desired sampling; use NVENC for the demonstrated NVIDIA H.264 4:4:4 gap before lowering sampling. A driver update may change results. Do not assume that swapping D3D12 adapter pointers enables cross-GPU capture: the existing D3D11/D3D12 bridge is same-adapter.

HEVC tries these formats in order, retaining the first supported capture-to-encoder path:

| Priority | Sampling | Input format | HEVC profile |
| --- | --- | --- | --- |
| 1 | 4:4:4, 8-bit | AYUV | Main 4:4:4 |
| 2 | 4:4:4, 10-bit | Y410 | Main10 4:4:4 |
| 3 | 4:2:2, 10-bit | Y210 | Main10 4:2:2 |
| 4 | 4:2:0, 8-bit | NV12 | Main |

Each candidate must pass codec/profile/input-format checks, level and I/P configuration checks, resource creation and D3D11 conversion/shared-resource interoperability. Capability failures are logged before trying the next candidate; device removal, out-of-memory and other operational failures abort rather than silently reducing quality. If none works, encoding fails explicitly. Selection occurs at initialization and is repeated after reconfiguration/resizing, not per frame. D3D12 H.264 exposes no 4:4:4/4:2:2 profiles, so D3D12 H.264 uses 4:2:0. Automatic H.264 selection tries NVENC 4:4:4 first, without changing codecs.

The log reports the actual selection, for example `Encoder sampling: 4:4:4 8-bit AYUV (HEVC)`, and includes the initialization stage/HRESULT for rejected candidates. The development GPU/driver currently rejects AYUV/Y410/Y210 encoder input support and falls back to `4:2:0 8-bit NV12`; installing newer SDK headers alone does not add hardware/driver support. The headers must expose the HEVC range-extension profiles used here (the updated Windows SDK does).

Generated HEVC VPS/SPS profile constraints, chroma format, bit depth and cropping match the selected format. Reconstruction transitions use the queried plane count rather than assuming NV12's two planes. Higher sampling preserves more chroma detail but is not lossless, and 10-bit candidates are still **SDR**, not HDR. Capture remains BGRA8; no PQ/scRGB/HDR metadata or tone mapping is implemented. Selection is based on host capability only: the receiver must support the selected HEVC range-extension profile, potentially using software decoding if its hardware cannot. `stream_offer` reports the selected profile; clients should check decoder support and request another codec if needed.

Packets contain an Annex B access unit delimiter and encoded slice, with codec parameter sets (H.264 SPS/PPS or HEVC VPS/SPS/PPS) on IDRs. Parameter sets match the negotiated level and HEVC coding-unit/transform settings, including resolution cropping, nominal frame-rate timing and zero picture reordering. HEVC configuration queries select supported 64x64 or 32x32 coding units and transform depths, honoring required asymmetric motion partition without switching codecs. The output packet also carries codec, capture timestamp and an accurate IDR key-frame flag. Hardware availability is adapter/driver-specific; unsupported configurations fail with an HRESULT. Production network framing and remote-window association are not implemented.

Both codecs use **low-delay I/P encoding without B-frames**: an IDR starts each fixed 60-frame GOP, followed by 59 P-frames, each referencing only the immediately preceding reconstructed picture. At 60 FPS an IDR occurs every second; at 30 FPS, every two seconds. Two GPU reconstruction slots alternate between previous-reference read and current-output write, using a texture array when required by the driver. Capability checks request one DPB reference. Frame numbers/POCs reset at each IDR; initial configuration and resize start a fresh IDR with parameter sets. There is still only one submitted picture in flight, with decode order equal to presentation order and no encoder lookahead enabled.

This replaces the bandwidth-heavy all-IDR test path so static areas and terminal glyphs can be predicted rather than recompressed independently every frame. Unlike B-frame GOPs, it does not require future pictures or presentation reordering. An IDR every 60 frames bounds random-access recovery without making GOP tuning another CLI setting yet. CBR bitrate selection is unchanged. On a 4:2:0 fallback, colored/subpixel text still loses chroma detail regardless of bitrate; higher sampling is not a lossless desktop codec or a one-frame end-to-end latency guarantee.

The shared input texture permits simultaneous-access interop for D3D11, and conversion commands are flushed before signaling work to D3D12. Video resource transitions stay on the encode queue; buffers cross to the graphics readback queue in `COMMON` state. This avoids using video-only resource states on a graphics command list.

## Secure WebSocket host

```powershell
# Store a long random password in a file only you can read (never on the command line):
Set-Content -NoNewline -Path $env:USERPROFILE\beamit.pw -Value '<long random password>'
.\BeamIt.exe --port 17389 --password-file $env:USERPROFILE\beamit.pw
# Optional: --bind 192.168.1.10, --tls-cert host.pem --tls-key host-key.pem
```

`--port` and `--password-file` are required; there are no other modes. Everything about a stream is chosen by the client in `start_stream`: the source, the capture API for desktop sources (`"capture": "dxgi"` default or `"wgc"`; windows always use WGC), and codec, frame rate and bitrate (defaults H.264, 60 FPS, 48,000 kbps). The encoder backend is always automatic. Different instances must use distinct endpoints. If Windows Firewall blocks access, allow the executable/port only on the intended private LAN; BeamIt does not alter firewall rules.

Or use the launcher script, which prepares everything per instance and starts the host:

```powershell
.\scripts\Start-BeamItServer.ps1                       # instance "default", port 17389, all interfaces
.\scripts\Start-BeamItServer.ps1 -Instance teams -Port 17390 -Bind 192.168.1.10
```

It keeps files in `%LOCALAPPDATA%\BeamIt\<Instance>` (or `-DataDirectory`). If missing, it creates `password.txt` (32 random bytes, base64url) and a self-signed RSA-2048 `cert.pem`/`key.pem` (PKCS#8) with the host name, `localhost` and local IPs as subject alternative names, valid for two years and regenerated within 30 days of expiry. The directory, password and key are restricted to the current user. The password is not printed; copy it from the file. The script prints the certificate SHA-256 fingerprint for client pinning, then runs the newest `cmake-build-*\BeamIt.exe` (or `-Executable`). Remaining arguments are passed to BeamIt. Using per-instance PEM files instead of the built-in `CurrentUser\My` certificate gives each instance its own pinnable identity. Works with Windows PowerShell 5.1 and PowerShell 7; ACLs are set by SID through .NET, avoiding `Set-Acl` name lookups that fail on machines with a broken domain trust.

The wire protocol is specified in [`PROTOCOL.md`](PROTOCOL.md); clients are written against that document. Summary:

- `https://host:port/` serves the `HTML/` test client (embedded into the executable at build time as byte arrays, because MSVC limits string literals to about 16 KB; the Content-Security-Policy allows only the page's own inline script/styles by SHA-256 hash); `wss://host:port/control` is the JSON signaling socket; each stream gets its own `wss://host:port/streams/{id}?token=…` binary socket, announced with the codec parameters in `stream_offer`.
- The client chooses the source (`window` HWND, `application` PID or `desktop`) and optionally codec/FPS/bitrate. Window/application streams use WGC; desktop streams use DXGI unless `source.capture` is `"wgc"`. The host captures the first frame and configures the encoder before replying, so `stream_offer` contains the exact WebCodecs codec string (for example `avc1.F40028` for H.264 High 4:4:4), size, chroma format and bit depth. A resize sends `stream_config` with a new generation before the next key frame.
- Each binary message is a 32-byte big-endian header (version, type, flags, generation, sequence, capture time) followed by one Annex B access unit. At most two frames are queued per stream socket; on overflow the queue is flushed, frames are skipped until a requested IDR, and the next frame carries the discontinuity flag. The host never sends a P-picture whose references were dropped.
- Keyboard and mouse input are JSON messages on the signaling socket, each naming its `stream_id` (§5.9 of the protocol); see *Remote input* below.
- Taskbar attention requests (window flashing) are reported as `attention` `set`/`clear` events (§5.10); see *Taskbar attention* below.
- Every top-level window is reported with `window_opened`/`window_changed`/`window_closed` (pid, image name, title, role, owner, rect, state; §5.12); see *Window events* below.
- The host clipboard can be read (`get_clipboard`) and written (`set_clipboard`) as text, HTML and PNG, and changes are announced with `clipboard_changed` (§5.11); see *Clipboard* below.

**Security.** TLS comes from Windows Schannel via [boost-wintls](https://github.com/laudrup/boost-wintls) (header-only, pinned SHA-256). Without `--tls-cert`/`--tls-key`, BeamIt creates (and reuses) a self-signed RSA-2048 certificate `CN=BeamIt host` in `CurrentUser\My`, valid for two years and regenerated when near expiry; its SHA-256 fingerprint is logged at startup. Browsers must accept it once by opening `https://host:port/`; native clients should pin the fingerprint, because password authentication is not bound to the TLS channel and an unpinned, accepted certificate allows an active relay. `--tls-key` must be an unencrypted RSA PKCS#8 PEM (`BEGIN PRIVATE KEY`).

The password is read from `--password-file` (trailing CR/LF removed), turned into a key with PBKDF2-HMAC-SHA256 (600,000 iterations, random per-run salt, Windows CNG), then wiped; it is never logged or sent. Client and host prove knowledge of the key with HMAC-SHA256 over both random nonces (mutual authentication). A failed proof closes the socket after one second. Only one authenticated control session exists; a newer one supersedes it (close code 4001). Stream tokens are random, single-use and expire after 10 seconds. WebSocket upgrades with a foreign `Origin` are rejected.

Tested from a stdlib Python WSS client on the development machine: authentication (correct and wrong password), `list_windows`, Teams application and desktop streams (NVENC H.264 4:4:4; the payload decodes with ffprobe as High 4:4:4 Predictive), forced key frames, `stop_stream`, token reuse and expiry, foreign origin and session supersede. Protocol helpers (base64url, PBKDF2/HMAC vectors, authentication proofs, frame header, SPS/codec-string parsing) are covered by the `[protocol]` Catch2 tests.

### Remote input

Input messages (`mouse_move`, `mouse_button`, `mouse_wheel`, `key`, `text`, `release_all`, `set_mouse_mode`, `get_input_state`) carry a `stream_id`; the host injects them with `SendInput` into the interactive desktop (`src/host/InputInjector.cpp`). Success is silent; errors use the normal `error` message.

- **Mouse modes, per stream.** *Absolute* (default): `x`/`y` in video pixels from the stream's top-left, mapped proportionally onto the window's visible frame (`DWMWA_EXTENDED_FRAME_BOUNDS`, which matches what WGC records) or the primary monitor, clamped, and sent as `MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK`. *Relative*: `dx`/`dy` deltas. For desktop streams they are real relative motion (host pointer acceleration applies; raw-input apps see deltas). For window streams the host adds the scaled delta to the cursor and clamps it inside the window, so pointer-locked clients cannot wander onto other host windows.
- **Keyboard.** `KeyboardEvent.code` names are translated to Set-1 scan codes (`KEYEVENTF_SCANCODE`, Chromium's Windows mapping), so layout is applied by the host like a physical keyboard. `text` injects Unicode (`KEYEVENTF_UNICODE`) for IME/paste; newlines and tabs become Enter/Tab keys.
- **Focus.** Key presses, button presses and text bring a window stream's window to the foreground first unless a window of the same process already has it (so the app's dialogs keep working). `SetForegroundWindow` is tried first, then `AttachThreadInput`. Failures report `focus_failed` once until the next success.
- **No stuck input.** Held keys/buttons are tracked per stream and released on `release_all`, `stop_stream`, stream failure and session end/supersede.
- **State query.** `get_input_state` returns the host pointer position in the stream's video pixels (desktop: relative to the primary monitor, possibly outside the video on other monitors; window: relative to the window frame, or `-1,-1` when outside it or minimized) plus Shift/Ctrl/Alt/Win held state and Caps/Num/Scroll Lock toggles. The inverse mapping is exact for the even-cropped video of odd-sized windows. Verified live, including injected Shift and Caps Lock changes.
- **DPI.** The host makes the process per-monitor DPI aware (v2) so window rectangles, the cursor and the virtual desktop are all in physical pixels.

Rejected alternatives: binary input on the stream socket (stream sockets are host→client only, and one signaling socket gives one total order across streams); `PostMessage`/`SendMessage` to the target window (bypasses focus/keyboard state and breaks many UI frameworks); virtual-key codes from the client (layout-dependent); mapping relative motion for window streams as raw deltas (the pointer escapes the streamed window).

Limitations: UIPI silently drops input to elevated windows unless BeamIt runs elevated; the secure desktop (UAC, lock screen, Ctrl+Alt+Del) is unreachable (`input_failed`); the host must run in the interactive user session; popup menus outside the window rectangle need their own stream (they are reported as `popup` windows, §5.12); `AttachThreadInput` may block briefly if the foreground app is hung.

### Taskbar attention

When a host window flashes its taskbar button (`FlashWindowEx`, e.g. a Teams chat or call), the host sends `{"type":"attention","stream_id":…,"state":"set","window":{hwnd,pid,title}}` on the signaling socket, and `"clear"` when the window is activated or destroyed, so the client can bounce the Dock icon or flash its own taskbar button. Window streams receive events for every window of the streamed process (Teams flashes secondary windows); desktop streams receive all. Windows currently requesting attention are reported right after `stream_offer`.

`src/host/AttentionMonitor.cpp` runs one thread with a hidden top-level tool window registered with `RegisterShellHookWindow`. The shell hook reports `HSHELL_FLASH` on the first flash and `HSHELL_WINDOWACTIVATED`/`HSHELL_RUDEAPPACTIVATED`/`HSHELL_WINDOWDESTROYED`; events are posted to the server's I/O context.

Why "set until activated": probing on Windows 11 showed `HSHELL_REDRAW` does not track the flash cycle (with `FLASHW_TIMERNOFG` one redraw arrives after ~2.6 s, then nothing while the button keeps flashing), and `FLASHW_STOP` produces just one `HSHELL_REDRAW`, identical to a title or icon change. A redraw-plus-timeout heuristic therefore cleared live flashes early and was rejected. Consequently `FLASHW_STOP` and the end of a counted flash are not reported; the window stays `set` until activated or closed, like the highlighted taskbar button. Other rejected alternatives: polling taskbar UI Automation (shell-internal, version-specific, polling latency); `SetWinEventHook` (no flash event); hooking `FlashWindowEx` in target processes (injection). Badge overlays and toast notifications are not covered.

Verified live with a WinForms window flashed with `FLASHW_TRAY|FLASHW_TIMERNOFG`, observed through window and desktop streams: `set` on the flash, unchanged by `FLASHW_STOP` and re-flash, `clear` on activation, `set` again on a new flash, and `clear` before `stream_stopped` when the window closed. Tracker logic is covered by the `[attention]` Catch2 tests.

Tested on the development machine with a stdlib Python WSS client against a WinForms text box window: absolute clicks landed on the exact computed client pixel, edge clamping and relative movement/clamping matched the window frame, Shift+keys, digits, Unicode text (`héllo 👋`, newline) and `release_all` arrived, the wheel scrolled down for positive `dy`, and focus was restored from another foreground application. `not_ready`, `invalid_request` and `not_found` errors were checked. The key table, coordinate mapping, normalization and text validation are covered by the `[input]` Catch2 tests.

### Window events

The host always reports every top-level window to the authenticated session: `window_opened` for each current window right after `authenticated`, then `window_opened`/`window_changed`/`window_closed` as windows appear, change or disappear (PROTOCOL.md §5.12). Each event carries the whole window object: `hwnd`, `pid`, `image_name` (e.g. `ms-teams.exe`), `title`, `role` (`main`/`owned`/`popup`/`tool`), `owner`, `rect` (virtual-screen physical pixels) and `state` (`normal`/`minimized`/`maximized`). The client decides which windows to present, e.g. by starting a `window` stream for each new `ms-teams.exe` `main`/`owned` window.

`WindowMonitor` (`src/host/WindowTracker.hpp/.cpp`) runs one thread with out-of-context `SetWinEventHook` hooks for top-level create/destroy/show/hide/location/name/cloak, move-size end and minimize events. A burst of notifications arms a high-resolution waitable timer (about 2 ms after the first event, at most once per 16 ms). The thread then takes an `EnumWindows` snapshot of visible, uncloaked, non-empty windows. Rectangles come from DWM extended frame bounds; image names are cached per live process. A 250 ms poll catches unannounced changes. On the network thread, `WindowList` diffs each snapshot against the previous one into closed → opened → changed events.

Decisions: events are session-level and unfiltered so that application grouping and presentation policy stay in the client (an earlier per-stream, per-application-family design with z-order and an opt-out flag was judged too complex). `role` follows the taskbar rule (unowned activatable or `WS_EX_APPWINDOW` → `main`) and is advisory. Geometry uses DWM extended frame bounds, expected to match what WGC captures, rather than `GetWindowRect` (includes invisible resize borders). Rejected alternatives: polling only (latency and CPU), hooks only (Windows does not announce every change), and delta updates (clients would need merge logic).

Limitations: z-order, focus and icons are not reported; `role` is a heuristic for applications that draw custom frames; cloaked windows (other virtual desktops) are reported as closed; the host must run in the interactive session.

Verified live with a PowerShell/WinForms application: about 15 existing windows reported after authentication; `window_opened` for the new main window and its owned dialog (`role` `owned`, `owner` set); `window_changed` for the moved and renamed dialog and for minimizing the main window (Windows hides the owned dialog, which is then reported closed); `window_closed` on close. `[windows]` Catch2 tests cover role classification, the snapshot diff and handle reuse.

### Clipboard

`ClipboardMonitor` (`src/host/Clipboard.hpp/.cpp`) runs one thread with a message-only window registered with `AddClipboardFormatListener`. Each `WM_CLIPBOARDUPDATE` with a new `GetClipboardSequenceNumber` sends `clipboard_changed` (sequence and available formats, no content) to the authenticated session. `get_clipboard` requests are queued to the same thread, which opens the clipboard (retrying for ~100 ms), copies `CF_UNICODETEXT`, `"HTML Format"` and `"PNG"`/`CF_DIB` out, closes it, then converts: text to UTF-8, the HTML fragment by its header offsets, DIBs to PNG via WIC (the DIB gets a `BITMAPFILEHEADER` and is decoded with the WIC BMP decoder). The JSON reply is serialized on that thread, so large images do not stall video sends; the network thread only queues the finished string.

`set_clipboard` is validated on the network thread (types, 10 MiB budget) and queued to the same thread, which decodes the base64 PNG and converts it with WIC to a premultiplied-BGRA, white-composited, bottom-up 32-bit `CF_DIB`. It builds the `"HTML Format"` header and UTF-16 text, then opens the clipboard and hands over all formats at once. A failed `SetClipboardData` empties the clipboard rather than leaving a partial set. The monitor window becomes the clipboard owner, which is how the resulting `WM_CLIPBOARDUPDATE`s are recognized and not echoed as `clipboard_changed`. After authentication the control socket accepts messages up to 16 MiB (64 KiB before), so that a 10 MiB clipboard plus JSON escaping fits.

Decisions: only notifications are pushed, so content (possibly several MiB) moves only when a client asks; content marked `ExcludeClipboardContentFromMonitorProcessing` (password managers) is neither announced nor returned; images are PNG as standard base64 for direct use in browsers and `NSPasteboard`; one reply is limited to 10 MiB of text + HTML + base64 image, with oversize formats listed in `omitted`. `CF_DIB` is preferred over `CF_DIBV5` because applications often leave zero alpha in synthesized 32-bit bitmaps, which would produce invisible PNGs. Rejected alternatives: pushing content on every change (wastes bandwidth and leaks content the user never pastes remotely); polling the sequence number (latency/CPU); OLE `IDataObject` (needs an STA and gives nothing extra for these formats); a separate binary socket for images (adds a third socket type for rare, small transfers).

`sensitive: true` on writes adds `ExcludeClipboardContentFromMonitorProcessing`, `CanIncludeInClipboardHistory`=0 and `CanUploadToCloudClipboard`=0, so passwords copied on the client stay out of Win+V history and cloud sync. Rejected for writes: delayed rendering (the data must survive BeamIt exiting and the client disconnecting); a binary image upload socket (same reasoning as for reads); a `CF_DIBV5` with alpha (applications disagree on its alpha semantics; the `"PNG"` format carries transparency for those that support it).

Limitations: files, RTF and other formats are not exposed; any authenticated client can read whatever is copied on the host, so treat the password accordingly.

Verified live with a stdlib Python WSS client: a WinForms bitmap+text copy returned text and a 320×200 PNG, a `formats: ["text"]` request returned only text, invalid formats gave `invalid_request`, an `"HTML Format"` copy returned the UTF-8 fragment, `clipboard_changed` arrived for each change, and password-manager-style excluded content was announced as `excluded` and returned only `omitted` entries. `set_clipboard` was verified the same way: text+HTML round-tripped through `get_clipboard`, a 64×32 PNG and a 1400×1400 noise PNG (10.46 MB base64) were readable by WinForms as PNG/DIB/Bitmap, no `clipboard_changed` echoes arrived, and malformed fields, invalid PNG data and a 12 MB request gave `invalid_request` without closing the session. Base64 encode/decode, HTML fragment extraction and document building, DIB headers, DIB→PNG and PNG→DIB are covered by the `[clipboard]` Catch2 tests; the hidden `[.clipboard-live]` test overwrites the real clipboard to check change notification, reading, the size limit, sensitive and plain writes, and rejection of invalid images.

### Transport decisions and rejected alternatives

- **WebSockets over TLS** (requested): easy to configure, browser-compatible and passes proxies/firewalls as HTTPS. TCP head-of-line blocking remains, so latency is bounded by the two-frame send queue and IDR recovery rather than by datagram loss handling. WebRTC/QUIC transports were not chosen for this pass.
- **Schannel (boost-wintls) instead of OpenSSL:** OpenSSL from source needs Perl (unavailable on the build machine), and a prebuilt binary would add an unpinned toolchain dependency. Schannel is the OS TLS stack and gets OS security updates. Limitations: wintls attaches private keys through legacy CryptoAPI (not CNG/NCrypt), so the self-signed key uses the Microsoft Enhanced RSA and AES provider, and only RSA PKCS#8 PEM keys import.
- **Password authentication inside TLS** rather than a PAKE (for example SRP/OPAQUE) or TLS-PSK: browsers cannot use TLS-PSK, and the shared-password policy is satisfied by a PBKDF2 key plus mutual HMAC proofs. Channel binding is not available in browsers, hence the fingerprint pinning recommendation.
- **Previous plaintext TCP stream** (`nc | ffplay` test): removed together with the file recorder; the WSS host is the only mode.

`StreamPipeline::stop()` joins its capture/encode thread on the network thread, which can briefly block other sockets while an encoder initializes; acceptable for this version. The server holds a process-wide COM MTA reference (`CoIncrementMTAUsage`): otherwise the MTA is torn down after the last stream thread exits and C++/WinRT's cached WGC activation factory points into an unloaded DLL (observed as an access violation on the next window stream).

## Removed test modes

The CLI previously also offered a five-second raw Annex B file recorder (`--output`), an unauthenticated one-client plaintext TCP streamer for `nc | ffplay` (`--insecure-test-stream`), window listing and target selectors (`--list-apps`, `--app`, `--hwnd`, `--desktop`), stream defaults and backend overrides (`--codec`, `--fps`, `--bitrate`, `--encoder`, `--capture`), the `--serve` flag and D3D12/NVENC capability probes (`--probe-encoders`, `--probe-nvenc`). They were development aids and have been removed; sources, capture API, codec and rate are now selected by clients over the authenticated protocol (`list_windows`, `start_stream`). Results recorded with them remain in this document as historical measurements.

To inspect stream output, save the binary payloads (after the 32-byte header) from a WSS client and analyze the Annex B access units with FFmpeg, for example `ffprobe -v error -show_entries frame=pict_type,key_frame -of csv=p=0 stream.h264`. Expect one IDR every 60 pictures, only P-pictures in between and no B-pictures.

## Non-goals

Unless requirements change, BeamIt should **not** focus on:

- Windows 10 or older
- Gamepad / controller support
- Legacy capture APIs when modern alternatives exist
- A local end-user GUI on the host machine
- General-purpose remote desktop compatibility with every existing client
- Moonlight/Sunshine protocol compatibility
- Internet connectivity, cloud services and NAT traversal
- Native notification bridging in the initial application-remoting milestone

## Open questions

These remain open before client/network implementation:

- Which macOS UI/language and rendering integration should the first client use?
- How should application windows, popups, geometry, DPI and focus map to local client windows?
- Should video use one stream per window or a shared surface with window metadata?
- Which standard password-based session protocol, KDF and authenticated transport meet the latency requirements?
- How should each server instance configure its listen endpoints and target application?
- Which codec and stream settings best fit macOS hardware decoding?
- Is explicit failure acceptable on GPUs without D3D12 Video Encode, or is a fallback backend required?
- When should optional audio be introduced after Teams window behavior is validated?

## Development notes

- Keep the host Windows 11+ only; build the client for macOS first and Windows afterward.
- Prefer modern C++26 idioms.
- Prefer the lowest-latency path that is still maintainable.
- Avoid assuming protocol choices, codec choices, or capture APIs without documenting the tradeoff.
- If there is uncertainty, ask before locking in a design.

## Build

This project uses CMake and C++26.
Use the IDE's configured CMake version for configure/build steps; do not assume the command-line `cmake` installation matches the project requirements.
For implementation details, inspect `CMakeLists.txt`.

Boost **1.90.0**, including Beast and its transitive dependencies, is downloaded by CPM/CMake from the official CMake release archive with a pinned SHA-256. Only the Beast dependency graph is enabled; upstream tests, examples and fuzzers are disabled. `BeamIt` and `BeamItTests` link `Boost::beast`. The first configuration requires network access; subsequent configurations reuse the populated source. Run the `[beast]` test filter through CLion's `BeamItTests` configuration for an in-memory HTTP/WebSocket upgrade parsing check. Boost.JSON is also enabled for signaling messages, and boost-wintls **1.0.0** is downloaded by CPM (`DOWNLOAD_ONLY`, pinned SHA-256) and wrapped in the local `beamit_wintls` interface target, which links `crypt32`, `secur32` and `bcrypt` and adds MSVC `/bigobj` (the coroutine server exceeds the default object section limit). `_WIN32_WINNT` is set to Windows 10+ (`0x0A00`).

### NVIDIA interface headers for backend exploration

CMake extracts NVIDIA's **Video Codec SDK 13.0.37** from the locally supplied `vendor/Video_Codec_SDK_13.0.37.zip` into `vendor/nvidia_nvend` during configuration, using `cmake/NvidiaNvenc.cmake`. The archive's SHA-256 is pinned (`828c5382a9999191eebd26fc5e3c62dc545aeccd95b6e307d9f7de5103889954`); extraction retains the upstream `Interface/` directory and license files. Both the ZIP and generated directory are ignored by Git. Obtain the SDK archive from NVIDIA and place it at that path before configuring. Use CLion's CMake profile to configure/build; set `-DBEAMIT_FETCH_NVENC_INTERFACE=OFF` to skip this optional SDK integration. No network download is performed for this SDK.

Unlike the previous headers-only 13.1.15 package, this is the full SDK archive. BeamIt uses only its `Interface/` headers, not the C++ `NvEncoder` wrapper, samples or bundled libraries. Preserve the SDK's license files and header copyright/permission notices. No CUDA toolkit or native encoder runtime is installed.

`NVIDIA::NVENCHeaders` is an interface-only CMake target used by the optional native encoder, which loads `nvEncodeAPI64.dll` from System32 at runtime. Disabling the SDK integration makes the automatic search log the omission and use D3D12.

The previous 13.1 headers exceeded the development driver's maximum API 13.0. With the genuine SDK 13.0.37 headers, the former `--probe-nvenc` diagnostic reported **headers 13.0, driver maximum 13.0** on the Quadro T1000: **H.264 High444 profile YES, YUV444 encode capability YES, planar YUV444 input YES**. This demonstrated native NVENC exposes a 4:4:4 path that the current D3D12 queries do not expose on this driver. The SDK's real versioned structures are used, not rewritten version constants.

## Repository files

- `include/beamit/capture/Capturer.hpp` — common capture interface and target types
- `include/beamit/capture/DxgiCapturer.hpp` and `src/capture/DxgiCapturer.cpp` — DXGI monitor capture backend
- `include/beamit/capture/WgcCapturer.hpp` and `src/capture/WgcCapturer.cpp` — WGC monitor/window capture backend
- `include/beamit/media/Encoder.hpp` — common asynchronous encoder interface
- `include/beamit/media/D3D12VideoEncoder.hpp` and `src/media/D3D12VideoEncoder.cpp` — H.264/H.265 D3D12 hardware encoder
- `include/beamit/media/NvencVideoEncoder.hpp` and `src/media/NvencVideoEncoder.cpp` — DirectX 11 NVENC H.264 4:4:4 encoder
- `include/beamit/media/AutoVideoEncoder.hpp` and `src/media/AutoVideoEncoder.cpp` — default codec-preserving encoder selection
- `include/beamit/media/VideoFrame.hpp` — shared captured-frame and encoded-packet types
- `PROTOCOL.md` — WSS signaling/stream protocol v1 specification
- `src/main.cpp` — CLI entry point for the WSS host
- `src/host/HostServer.hpp/.cpp` — HTTPS/WSS server, signaling sessions and stream sockets (Beast + wintls, Asio coroutines)
- `src/host/StreamPipeline.hpp/.cpp` — per-stream capture/encode thread
- `src/host/InputInjector.hpp/.cpp` — remote keyboard/mouse injection, key table, coordinate mapping and focus
- `src/host/AttentionMonitor.hpp/.cpp` — shell-hook taskbar attention (flash) tracking
- `src/host/Clipboard.hpp/.cpp` — clipboard change listener and text/HTML/PNG reader/writer
- `src/host/WindowTracker.hpp/.cpp` — top-level window tracking (WinEvent hooks, snapshots, diffs)
- `src/host/TlsIdentity.hpp/.cpp` — PEM certificate loading and persistent self-signed certificate
- `src/net/Crypto.hpp/.cpp` — CNG random, base64url, PBKDF2, HMAC and authentication proofs
- `src/net/Protocol.hpp` — protocol constants and binary frame header
- `src/media/BitstreamInfo.hpp/.cpp` — SPS parsing for codec strings, chroma format and bit depth
- `tests/ProtocolTests.cpp` — protocol and bitstream unit tests
- `tests/InputTests.cpp` — input mapping and validation unit tests
- `tests/AttentionTests.cpp` — taskbar attention tracker unit tests
- `tests/WindowTrackerTests.cpp` — window role and window-list unit tests
- `scripts/Start-BeamItServer.ps1` — creates per-instance password/certificate files and starts the WSS host
- `src/ApplicationWindows.hpp` and `src/ApplicationWindows.cpp` — window listing and PID selection heuristic (`list_windows`, `application` sources)
- `src/media/CodecHeaders.hpp` and `src/media/CodecHeaders.cpp` — encoder-specific Annex B parameter-set generation
- `CMakeLists.txt` — CMake build configuration
- `cmake/CPM.cmake` — dependency bootstrap for CPM
