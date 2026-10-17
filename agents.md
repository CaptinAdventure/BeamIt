# Agent guidance for BeamIt

This repository is a Windows 11+ low-latency application-remoting host project, with custom macOS and Windows clients planned.
Treat the implementation as performance-sensitive and architecture-sensitive.

## Core product intent

BeamIt should stream either:

- the **desktop**, or
- a **single application/window**

to a remote computer with very low latency. Application remoting should present the selected application's main and secondary windows as native client-side windows, not a single desktop-video surface.

Start with the macOS client, followed by Windows. Moonlight compatibility and a browser client are no longer targets. Taskbar/Dock attention is sufficient for the initial native integration; native notification bridging is not required.

The product should stay focused on keyboard, mouse, and optionally audio.
Gamepad support is out of scope.

### Current milestone

The immediate target is **Microsoft Teams**: stream it to another computer and ensure all of its external/secondary windows appear correctly on the remote side.

For this milestone, prioritize window behavior and capture fidelity. The shared capture-frame and encoder contracts, an initial D3D12 encoder and single-HWND WGC capture are implemented. The host reports all top-level windows (PROTOCOL.md §5.12); define the macOS client, which chooses the windows to stream, and validate it with Teams. Defer audio and broader streaming features.

### LAN access and instance model

Internet connectivity will never be a requirement: this is strictly LAN-only, with no cloud, NAT traversal or port-forwarding requirements.

Multiple server instances must be able to run on one Windows machine, with independently configurable listen endpoints, application targets and credentials.

A per-instance shared password, supplied through a config file or CLI parameter, is the combined authentication/authorization policy. No separate accounts or roles are required. Password holders share the same instance-level access; clients without the password must not decode data or issue accepted input/control commands.

AES-256 is requested. Derive keys from passwords using a standard KDF rather than treating password text as a raw AES key. The selected transport is WSS (TLS 1.2/1.3 via Windows Schannel/boost-wintls) with PBKDF2-HMAC-SHA256 key derivation and mutual HMAC nonce proofs inside TLS, specified in `PROTOCOL.md` and implemented by the host executable (its only mode). Treat `PROTOCOL.md` as the contract: change it together with the host and keep the version number meaningful. Password proofs are not channel-bound, so native clients should pin the logged certificate fingerprint. Do not equate undecodable video alone with authentication. Prefer restricted config-file permissions and never log secrets; CLI passwords can leak through process inspection/history.

The former plaintext TCP test stream, file recorder, window listing/selectors, stream-default/backend options (`--codec`, `--fps`, `--bitrate`, `--encoder`, `--capture`), `--serve` flag and capability probes were removed at the user's request; the WSS host is the only CLI mode. Do not reintroduce unauthenticated test transports without explicit approval. The host uses high-resolution waitable timers for pacing/GPU polling and WGC prefers the newest queued frame. One-frame end-to-end latency is a target, not a verified guarantee.

DXGI passes borrowed duplication surfaces without a capture copy. Preserve `CapturedFrame::lifetime` through GPU completion; retaining just the texture is insufficient. The encoder retains the full frame while pending. The stream pipeline releases borrowed frames after output and uses `Encoder::try_repeat_frame()` during idle periods to encode the retained NV12 image as a new picture. Never replay old compressed P-packets or drop arbitrary encoded reference pictures. WGC still returns copied textures.

### Capture API direction

Use **Windows.Graphics.Capture** as the primary capture backend for the Teams milestone because it is the better fit for window-level capture and external/secondary windows.

Keep **DXGI Desktop Duplication** available as a secondary backend for full-desktop capture and fallback scenarios.

The codebase exposes a common pure-virtual `Capturer` interface. The capture API is chosen per stream by the client: window/application sources always use WGC; desktop sources default to DXGI, or WGC via `source.capture` (PROTOCOL.md §5.3). Do not switch APIs mid-stream.

Both capture backends must expose frames through the same GPU-backed `CapturedFrame` type. The common path should use a reference-counted D3D11 texture and capture timestamp; avoid CPU readback unless a backend or encoder limitation requires it.

Define a separate pure-virtual `Encoder` interface that accepts these frames and yields encoded packets. Keep capture and encoding decoupled so WGC and DXGI use the same contract and asynchronous encoders can apply backpressure. The host always uses `AutoVideoEncoder`: preserve the client-requested codec, prefer available 4:4:4 backends before lowering sampling on D3D12. H.264 skips D3D12 4:4:4 (no API profile), tries NVENC High444, then D3D12 4:2:0. HEVC uses D3D12 4:4:4 -> 4:2:2 -> 4:2:0; NVENC HEVC is not implemented. Log skipped/rejected candidates and selected sampling. Fallback applies only to initialization availability/capability rejection, not allocation/device/encoding failures. Decide before submitting the first picture; never switch mid-stream. Resize/reconfiguration restarts selection. NVENC is SDR H.264 High 4:4:4 Predictive only, with no native chroma fallback.

The native NVENC encoder opens a DirectX 11 session on the capture device, copies each fresh BGRA frame into one encoder-owned registered ARGB input texture, and lets NVENC convert to 4:4:4. Do not add a capture-side copy, CPU pixel readback, or CUDA dependency. The owned image supports new I/P pictures during DXGI idle periods. It uses P1 ultra-low-latency, CBR/one-frame VBV, GOP 60, one reference, no B-frames/lookahead/temporal AQ/multipass and asynchronous completion events. Retain borrowed capture leases through GPU completion; protect the encoder-side copy during error/shutdown using its D3D11 fence. Destroy encoders before capturers on exception paths. Submission stays bounded to one frame. SDK initialization is deferred until the first input supplies the device, like D3D12. T1000 WGC and DXGI recordings decode as High444/yuv444p with no B-frames; macOS hardware decoding and end-to-end latency remain unverified. Hardware lifecycle coverage is available with the hidden `[.nvenc-hardware]` Catch2 tag.

The encoder bridges D3D11 capture textures to a shared D3D12 YUV texture using D3D11 video processing and shared-fence synchronization. HEVC tries 4:4:4 AYUV/Main444, 4:4:4 Y410/Main10_444, 4:2:2 Y210/Main10_422, then 4:2:0 NV12/Main. Validate the complete initialization and conversion path, log rejected candidates and selected sampling, and do not mask device/operational failures as capability fallback. H.264 remains 4:2:0 because the D3D12 H.264 profiles have no 4:4:4/4:2:2 option; never switch codecs silently. The updated SDK must expose HEVC range-extension profiles; actual support is GPU/driver-dependent. The development GPU currently falls back to NV12. Ten-bit sampling candidates do not implement HDR. Packets contain Annex B access unit delimiters and matching SPS/PPS (plus VPS for HEVC), including negotiated chroma format, bit depth, profile constraints, timing and cropping. HEVC configuration queries select supported coding-unit sizes and transform depths. Video-only transitions must stay on the encode queue; cross-queue buffer handoffs use COMMON. Reconstruction transitions must use the queried format plane count, not a hard-coded NV12 count. Production network framing and remote-window association are not implemented.

## Hard constraints

`cmake/NvidiaNvenc.cmake` extracts the checksum-pinned local `vendor/Video_Codec_SDK_13.0.37.zip` to `vendor/nvidia_nvend` (both Git-ignored), controlled by `BEAMIT_FETCH_NVENC_INTERFACE`. The ZIP must be supplied separately; no SDK network download is performed. This is the full SDK, but BeamIt uses only its `Interface/` headers through `NVIDIA::NVENCHeaders`, not the wrapper/samples/libraries. The encoder loads the system driver DLL dynamically. With SDK integration disabled, auto logs the omission and uses D3D12. With genuine API 13.0 headers matching the development driver, the former NVENC probe showed the Quadro T1000 reports High444 profile YES, YUV444 encode capability YES and planar YUV444 input YES. Never fake an SDK downgrade by rewriting struct versions. Preserve upstream license files and notices.

Prefer D3D12 encoding; explore native NVENC/Intel VPL only for demonstrated D3D12 gaps. A former D3D12 capability probe (removed) queried all DXGI hardware adapters at 1080p with the current I/P GOP. The development Quadro T1000 reports D3D12 H.264/NV12, HEVC/NV12 and HEVC Main10/P010 I/P support but rejects queried 4:4:4/4:2:2 profiles. Intel UHD (`8086:9BC4`) reports no H.264/HEVC codec support through D3D12 with the current driver. These are API/driver results, not proof of missing silicon capabilities. See README for the recorded results. Do not assume same-adapter interop works across GPUs.

- **Host platform:** Windows 11+
- **Client platforms:** macOS first, Windows afterward
- **Language:** C++26
- **APIs:** use recent APIs where possible; do not default to legacy approaches
- **Host UI:** no local GUI is required
- **Client direction:** custom seamless application-remoting client; no Moonlight compatibility
- **Network:** LAN-only, never an Internet requirement
- **Latency:** optimize for end-to-end latency first, not just implementation convenience
- **Current milestone scope:** prioritize Teams window streaming and secondary-window correctness before broader audio/video work

## Working rules for agents

1. **Do not assume** product or protocol decisions that are not written down.
2. **Ask questions** when a design choice changes the architecture, interoperability, or latency profile.
3. **Document tradeoffs** before choosing between alternatives.
4. **Prefer small, reversible changes** until the transport and capture strategy are confirmed.
5. **Use modern Windows APIs** and avoid legacy code paths unless there is a clear reason.
6. **Keep the implementation compatible with headless operation** on the host.
7. **Do not assume command-line CMake is available or correct**; use the IDE-managed CMake version for configure/build validation when needed.

## Areas that need explicit decision

Before building major pieces, confirm these items:

- macOS client language/framework and native rendering/decoding integration
- Window-family tracking, popup/ownership behavior, geometry/DPI and focus mapping
- Per-window vs shared video surfaces and remote-window identifiers
- Per-instance endpoint/configuration syntax
- Codec and stream settings for macOS hardware decoding
- Supported-GPU fallback strategy if D3D12 Video Encode is unavailable
- When to introduce optional audio after Teams window behavior is validated
- Whether the host is expected to be always-on or session-based

## Suggested implementation priorities

When work begins, a sensible order is:

1. Choose the macOS client foundation and define window/video/control contracts
2. Implement WGC window capture and confirm Teams secondary-window behavior
3. Implement authenticated LAN sessions and macOS native window presentation
4. Add input forwarding, focus and Dock/taskbar attention integration
5. Expand GPU coverage and add the Windows client
6. Add optional audio and broader session management

## Documentation expectations

Any significant implementation should update the docs with:

- chosen architecture
- rejected alternatives
- why the final choice was made
- how to run and test it

## Current repo state

The CLI11 entry point only runs the WSS host (`--port`, `--password-file`, optional `--bind`, `--tls-cert`/`--tls-key`); `scripts/Start-BeamItServer.ps1` creates per-instance password/certificate files and launches it. Clients list windows (`list_windows`) and choose window/application/desktop sources, the desktop capture API and codec/FPS/bitrate in `start_stream`. WGC supports monitors and windows, handles resize and crops odd dimensions to even for sampling fallback. DXGI remains monitor-only. Both codecs use a fixed 60-picture I/P GOP: IDR then 59 P-pictures, one previous-picture reference, two alternating reconstruction slots, no B-frames/lookahead, and no picture reordering. Parameter sets accompany IDRs; resize/reconfigure resets references and restarts with an IDR. Preserve queried texture-array requirements and reference resource transitions. Both codecs have produced 300-frame, 60 FPS streams containing five IDRs and 295 P-pictures that decode with FFmpeg on the development machine. Manual quality testing favored 24,000 kbps over the current 12,000 kbps default; clients may request it without assuming a universal requirement. Sampling auto-selection is host-only; client profile/hardware-decode negotiation is still needed. The host serves the authenticated HTTPS/WSS protocol from `PROTOCOL.md` (JSON signaling socket, one binary socket per stream, the `HTML/` test client embedded at `/` with a hash-based CSP). Remote keyboard/mouse input (PROTOCOL.md §5.9) is JSON on the signaling socket tagged with `stream_id`, per-stream absolute/relative mouse modes, `KeyboardEvent.code` → scan-code keys plus Unicode `text`, auto-focus of window streams, `get_input_state` (pointer in video pixels, `-1,-1` outside a window stream, modifiers/lock toggles), and held-input release on stream/session end; the host is per-monitor-v2 DPI aware. Taskbar attention (PROTOCOL.md §5.10) is host-side implemented: a shell-hook monitor sends `attention` `set` on a window's first flash and `clear` only on activation/destruction (Windows cannot distinguish `FLASHW_STOP` from redraws), scoped to the streamed process for window streams and to all windows for desktop streams. Clipboard reading (PROTOCOL.md §5.11) is session-level: `clipboard_changed` pushes only sequence/formats, `get_clipboard` returns text, the HTML fragment and PNG (standard base64) within a 10 MiB budget, read and serialized on a dedicated clipboard thread; content marked `ExcludeClipboardContentFromMonitorProcessing` is never sent. `set_clipboard` replaces the host clipboard with text, HTML and/or PNG (also written as a white-composited `CF_DIB`), optionally `sensitive` (excluded from history/cloud/monitors), and is not echoed back as `clipboard_changed`; authenticated control messages may be up to 16 MiB. The host always sends every top-level window to the authenticated session as `window_opened`/`window_changed`/`window_closed` (PROTOCOL.md §5.12; full object: hwnd, pid, image_name, title, advisory role, owner, DWM extended-frame rect in virtual-screen physical pixels, show state); clients decide which windows to stream. `get_window_icon` (§5.13) returns a window icon on request as a base64 PNG of at most 256×256 (window icon if large enough, else the executable icon), extracted on a worker thread. Keep this simple: no per-stream families, z-order or opt-out flags unless requested. WinEvent hooks trigger coalesced `EnumWindows` snapshots (2 ms batch, at most 60 Hz, 250 ms poll). Not yet validated with Teams itself. Native clients and broader GPU coverage remain outstanding. See README.md for CLI arguments and test instructions.
