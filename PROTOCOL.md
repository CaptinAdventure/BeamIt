# BeamIt streaming protocol, version 1

This document is the authoritative contract between the BeamIt host and
clients. The test web client will be written against this document in a later
pass. All values below are normative unless marked *informative*.

## 1. Overview

A BeamIt host serves one TLS port. Everything on that port is HTTPS / WSS:

| Path                                | Kind                    | Purpose                                         |
|-------------------------------------|-------------------------|-------------------------------------------------|
| `GET /`                             | HTTPS                   | Test web client page                            |
| `GET /control`                      | WebSocket (text, JSON)  | The single **signaling** ("master") socket      |
| `GET /streams/{stream_id}?token=…`  | WebSocket (binary)      | One **stream** socket per video stream          |

Plaintext HTTP and `ws://` are not served. Any other path returns `404`.

Typical session:

```text
client                                   host
  |--- WSS /control ------------------------>|
  |<-------------------------- hello --------|  (auth challenge)
  |--- authenticate ------------------------>|
  |<------------------- authenticated -------|  (mutual proof)
  |<------------------- window_opened -------|  (every host window, §5.12)
  |--- list_windows ------------------------>|
  |<------------------------- windows -------|
  |--- start_stream ------------------------>|  (host starts capture + encoder)
  |<-------------------- stream_offer -------|  (codec parameters + dedicated URL)
  |=== WSS /streams/{id}?token=… ===========>|
  |<========== binary video frames ==========|
  |<-------------------- stream_config ------|  (only when the encoder is reconfigured)
  |--- mouse_move / mouse_button / key ----->|  (input, tagged with stream_id)
  |<------------------------ attention ------|  (taskbar flash set/clear, §5.10)
  |<-------- window_changed / window_closed -|  (§5.12)
  |<----------------- clipboard_changed -----|  (host clipboard changed, §5.11)
  |--- get_clipboard ----------------------->|
  |<------------------------- clipboard -----|  (text / HTML / PNG)
  |--- set_clipboard ----------------------->|
  |<--------------------- clipboard_set -----|
  |--- stop_stream ------------------------->|
  |<------------------ stream_stopped -------|
```

## 2. Transport and security

* TLS is provided by Windows Schannel (TLS 1.2 or 1.3, AEAD suites). The host
  presents either an operator-provided certificate or a persistent
  self-signed certificate. The host logs the certificate SHA-256 fingerprint
  at startup; clients that cannot validate the chain **should** pin it.
  Browsers must accept a self-signed certificate once by opening
  `https://host:port/`.
* The host is protected by one shared password. The password itself never
  crosses the wire (see §4). Authentication is required before any other
  signaling command is accepted.
* Only one authenticated signaling session exists at a time. A newer
  successful authentication **supersedes** the older one: all of the older
  session's streams are stopped and its socket is closed with code `4001`.
  (This keeps page reloads working.)
* Stream sockets are authorized by a single-use, unguessable token that is
  bound to the signaling session that requested the stream (§6).
* If a browser `Origin` header is present on any WebSocket upgrade, its
  `host:port` must equal the request `Host` header, otherwise the upgrade is
  rejected with `403`. Non-browser clients may omit `Origin`.
* Limits: signaling messages are at most 64 KiB before authentication and
  16 MiB afterwards (for `set_clipboard`, §5.11.3); an unauthenticated control
  socket is closed after 10 s; at most 8 concurrent streams per session.
  The WebSocket layer uses ping/pong keep-alives; idle peers are dropped
  after about 30 s without a response.

## 3. Signaling messages

Signaling frames are UTF-8 **text** WebSocket messages, each containing one
JSON object. Binary messages on `/control` are a protocol violation (close
code `1003`).

Every message has a string `type`. Client requests may carry an `id` (a
string, or an integer in ±2^53); the host copies it into the direct reply or
error. Receivers **must** ignore unknown fields. Unknown `type` values
produce an `error` with code `unknown_type` but do not close the socket.

### 3.1 Errors

```json
{ "type": "error", "id": 7, "code": "invalid_request", "message": "…", "stream_id": "…" }
```

`id` and `stream_id` are present only when applicable. Codes:

| Code                 | Meaning                                                         | Closes socket |
|----------------------|-----------------------------------------------------------------|---------------|
| `protocol_version`   | Client's `protocol` is not supported                            | yes (`1002`)  |
| `auth_failed`        | Wrong proof or malformed `authenticate`                         | yes (`1008`)  |
| `not_authenticated`  | Command sent before authentication                              | yes (`1008`)  |
| `invalid_request`    | Malformed JSON, missing/invalid field                           | no            |
| `unknown_type`       | Unknown `type`                                                  | no            |
| `not_found`          | Unknown window / PID / stream                                   | no            |
| `limit_reached`      | Too many concurrent streams                                     | no            |
| `capture_failed`     | Capture could not start for the requested source                | no            |
| `encoder_failed`     | No encoder could be configured                                  | no            |
| `not_ready`          | Input sent before the stream socket opened                      | no            |
| `focus_failed`       | Target window could not be brought to the foreground (§5.9)     | no            |
| `input_failed`       | The OS rejected injected input (§5.9)                           | no            |
| `clipboard_unavailable` | The host clipboard could not be opened, read or written (§5.11) | no         |
| `icon_unavailable`   | Neither the window nor its executable has a usable icon (§5.13) | no            |
| `internal_error`     | Unexpected host failure                                         | no            |

## 4. Handshake and authentication

### 4.1 `hello` (host → client, first message)

Sent immediately after the `/control` upgrade.

```json
{
  "type": "hello",
  "protocol": 1,
  "server": "BeamIt",
  "auth": {
    "mechanism": "pbkdf2-sha256-hmac",
    "salt": "base64url",
    "iterations": 600000,
    "server_nonce": "base64url (32 bytes)"
  }
}
```

`salt` is random per host start. `server_nonce` is random per connection.
All binary values in JSON are **base64url without padding** (RFC 4648 §5).

### 4.2 `authenticate` (client → host)

```text
key          = PBKDF2-HMAC-SHA256(password_utf8, salt, iterations, 32 bytes)
client_proof = HMAC-SHA256(key, "BeamIt v1 client" || 0x00 || server_nonce || client_nonce)
server_proof = HMAC-SHA256(key, "BeamIt v1 server" || 0x00 || server_nonce || client_nonce)
```

`||` is byte concatenation of the raw (decoded) nonces. `client_nonce` is
32 random bytes chosen by the client. Browsers can compute all of this with
`crypto.subtle` (`PBKDF2`, `HMAC`, `SHA-256`) and `crypto.getRandomValues`.

```json
{ "type": "authenticate", "protocol": 1, "client_nonce": "base64url", "proof": "base64url" }
```

### 4.3 `authenticated` (host → client)

```json
{ "type": "authenticated", "server_proof": "base64url", "host_time_us": 123456789 }
```

The client **must** compare `server_proof` with its own computation and
disconnect if it differs; this proves the host also knows the password.
On failure the host sends `error` `auth_failed`, waits about one second,
and closes with `1008`. There is one attempt per connection.

*Informative:* an active attacker who impersonates the host (e.g. a user
blindly accepting a different self-signed certificate) can capture one
`proof` and attempt an offline dictionary attack. Use a long random password
and verify the certificate fingerprint.

## 5. Commands (after authentication)

### 5.1 `ping` / `pong`

```json
{ "type": "ping", "id": 1 }
{ "type": "pong", "id": 1, "host_time_us": 123456789 }
```

`host_time_us` uses the same monotonic host clock as `capture_time_us` in
video frames (§7), so clients can estimate host-to-display latency.

### 5.2 `list_windows`

```json
{ "type": "list_windows", "id": 2 }
{ "type": "windows", "id": 2, "windows": [
    { "hwnd": "0x1A2B3C", "pid": 4242, "title": "Microsoft Teams", "primary": true }
] }
```

Lists visible, capturable top-level windows. `primary` marks windows that
are candidates to be an application's main window (unowned, not tool
windows). For `{"kind":"application"}` the host picks the largest primary
candidate of that process, so several windows of one process may be
`primary: true`.
`hwnd` is an opaque string; clients must echo it back unchanged.

### 5.3 `start_stream`

```json
{
  "type": "start_stream",
  "id": 3,
  "source": { "kind": "window", "hwnd": "0x1A2B3C" },
  "video": { "codec": "h264", "frame_rate": 60, "bitrate_kbps": 20000 }
}
```

`source.kind` is one of:

* `"window"` with `hwnd` — one top-level window (Windows.Graphics.Capture);
* `"application"` with integer `pid` — that process's primary window;
* `"desktop"` — the primary monitor.

`source.capture` optionally selects the capture API: `"dxgi"` (DXGI Desktop
Duplication, the default for `"desktop"`) or `"wgc"` (Windows.Graphics.Capture).
Window and application sources always use `"wgc"`; `"dxgi"` for them is an
`invalid_request`. `stream_offer.source.capture` reports the API used.

`video` and each of its fields are optional; omitted fields default to
`"h264"`, 60 FPS and 48000 kbps. `codec` is `"h264"` or `"h265"`; `frame_rate` is
1–240; `bitrate_kbps` is 1–1000000. The host keeps the codec and picks the
best available chroma sampling (§8).

The host captures the first frame and configures the encoder before
replying, so the reply already carries exact decoder parameters. A failure
produces an `error` (`not_found`, `capture_failed`, `encoder_failed`,
`limit_reached`).

### 5.4 `stream_offer` (reply to `start_stream`)

```json
{
  "type": "stream_offer",
  "id": 3,
  "stream_id": "Q2hOc3kV2bTqjG6dAVu0Sg",
  "url": "wss://host:port/streams/Q2hOc3kV2bTqjG6dAVu0Sg?token=…",
  "token_expires_in_ms": 10000,
  "source": { "kind": "window", "hwnd": "0x1A2B3C", "pid": 4242, "title": "Microsoft Teams", "capture": "wgc", "dpi": { "x": 144, "y": 144 } },
  "video": { …video parameters, §5.5… },
  "input": { "mouse_mode": "absolute" }
}
```

The `url` authority is copied from the `Host` header of the `/control`
upgrade so it matches the certificate exception the browser already holds.
The client should open it within `token_expires_in_ms`; otherwise the host
stops the stream with reason `token_expired`.

`source.dpi` reports the target monitor's effective Windows display DPI at
stream startup, with positive integer `x` and `y` values. Window/application
streams use the monitor containing the target window; desktop streams use
the primary monitor. This is display scaling metadata, not physical panel
density or the source application's DPI-awareness setting. 96 DPI means
100% scaling; 144 DPI means 150%. Video dimensions and input coordinates
remain video pixels. To obtain a Windows logical size, multiply video width
by `96 / dpi.x` and height by `96 / dpi.y`; clients must then account for
their own display scale. This startup snapshot is not updated by
`stream_config` when the window moves to another monitor. Older hosts may
omit this additive field; clients must not assume a measured DPI in that case.

### 5.5 Video parameters object

```json
{
  "generation": 1,
  "codec": "h264",
  "codec_string": "avc1.F4001F",
  "format": "annexb",
  "width": 1920,
  "height": 1080,
  "chroma_format": "4:4:4",
  "bit_depth": 8,
  "frame_rate": 60,
  "bitrate_kbps": 20000,
  "key_frame_interval": 60,
  "b_frames": 0,
  "encoder": "NVENC D3D11 H.264 4:4:4"
}
```

* `codec_string` is the RFC 6381 string derived from the actual SPS, usable
  directly as WebCodecs `VideoDecoderConfig.codec`. Configure the decoder
  **without** `description` so it accepts Annex B input.
* `width`/`height` are the displayed (captured) size; `codedWidth`/
  `codedHeight` may be passed as the same values.
* `generation` starts at 1 and increments on every encoder reconfiguration.
* `encoder` is informative only.
* Pictures are output in decode order equal to display order (I/P only, no
  B-frames), each packet decodes to exactly one picture.

### 5.6 `stream_config` (host → client)

Sent when the source size changes and the encoder is reconfigured. The new
generation starts with a key frame.

```json
{ "type": "stream_config", "stream_id": "…", "video": { "generation": 2, … } }
```

Each binary frame carries its generation (§7). A client that receives a
frame with a generation for which it has not yet seen parameters must hold
or drop it until the matching `stream_config` arrives. The host sends
`stream_config` before the first frame of the new generation, but the two
sockets are independent so ordering across them is not guaranteed.

### 5.7 `request_key_frame`

```json
{ "type": "request_key_frame", "stream_id": "…" }
```

Asks for an IDR, e.g. after a decoder error. No reply on success.

### 5.8 `stop_stream` / `stream_stopped`

```json
{ "type": "stop_stream", "id": 4, "stream_id": "…" }
{ "type": "stream_stopped", "id": 4, "stream_id": "…", "reason": "requested" }
```

The host also sends `stream_stopped` unsolicited (without `id`). Reasons:

| Reason           | Meaning                                                    |
|------------------|------------------------------------------------------------|
| `requested`      | Client sent `stop_stream`                                  |
| `source_closed`  | The captured window closed                                 |
| `token_expired`  | The stream socket was not opened in time                   |
| `disconnected`   | The stream socket closed                                   |
| `error`          | Capture/encode failure; `message` gives details            |

The stream socket is closed with code `1000` after `stream_stopped`.

### 5.9 Input (keyboard and mouse)

Input is sent as JSON on the signaling socket. Every input message carries
the `stream_id` of the stream it targets, so a session with several streams
routes each event to the right window. Using the single signaling socket
gives one total order for all input across streams.

Rules common to all input messages:

* The stream must belong to the session and its stream socket must be open;
  otherwise the host replies `not_found` or `not_ready`.
* Successful input produces **no reply** (lowest latency). `id` is optional
  and is only echoed in `error` replies.
* Malformed messages produce `invalid_request` and are not injected.
* Input goes to the host's interactive desktop with `SendInput`. Password
  holders have full input access by policy; confining the pointer to a
  window stream is a usability boundary, not a security boundary.

#### 5.9.1 Mouse mode

Each stream has a mouse mode, initially `"absolute"`:

| Mode         | `mouse_move` fields | Meaning                                                         |
|--------------|---------------------|-----------------------------------------------------------------|
| `"absolute"` | `x`, `y`            | Position in video pixels from the stream's top-left (0,0)       |
| `"relative"` | `dx`, `dy`          | Movement delta (e.g. `movementX/Y` under pointer lock)          |

```json
{ "type": "set_mouse_mode", "id": 9, "stream_id": "…", "mode": "relative" }
{ "type": "mouse_mode", "id": 9, "stream_id": "…", "mode": "relative" }
```

`mouse_mode` is the reply. `start_stream` may also include
`"input": { "mouse_mode": "relative" }`; `stream_offer` always reports the
current mode as `"input": { "mouse_mode": "…" }`.

#### 5.9.2 `mouse_move`

```json
{ "type": "mouse_move", "stream_id": "…", "x": 640.5, "y": 360 }
{ "type": "mouse_move", "stream_id": "…", "dx": -3, "dy": 7 }
```

Sending the fields of the other mode is `invalid_request`. Values are
numbers (fractions allowed).

* **Absolute:** `x`/`y` are in the coordinate space of the video
  `width`×`height` of the latest `stream_offer`/`stream_config` (§5.5). The
  host maps them proportionally onto the source's *current* screen
  rectangle — the window's visible frame for window/application streams, the
  primary monitor for desktop streams — and clamps to its edges. While a
  window is being resized, positions may be briefly off until the client
  receives the new `stream_config`.
* **Relative, desktop stream:** deltas are injected as raw relative motion
  (host pointer speed/acceleration apply; games and raw-input applications
  see the deltas).
* **Relative, window stream:** the host adds the delta to the current
  cursor position, clamps it to the window rectangle and moves the cursor
  there, keeping the pointer inside the streamed window (no acceleration;
  raw-input applications see an absolute move). Deltas are in video pixels.

#### 5.9.3 `mouse_button`

```json
{ "type": "mouse_button", "stream_id": "…", "button": "left", "down": true }
```

`button` is `left`, `right`, `middle`, `back` or `forward` (DOM
`MouseEvent.button` 0,2,1,3,4). `left` is the *primary* button even when the
host user swapped buttons. Clients should send a `mouse_move` first so the
click lands at the intended position.

#### 5.9.4 `mouse_wheel`

```json
{ "type": "mouse_wheel", "stream_id": "…", "dx": 0, "dy": 120 }
```

Integers in Windows wheel units: `120` = one notch; each field is optional
(default `0`) and at most ±12000. Positive `dy` scrolls
**down** (DOM `deltaY` sign); positive `dx` scrolls right. Fine-grained
values (high-resolution wheels / touchpads) are allowed. Clients converting
DOM `WheelEvent`s in pixel mode should accumulate and scale (e.g. 1 px ≈
1.2 units).

#### 5.9.5 `key`

```json
{ "type": "key", "stream_id": "…", "code": "KeyA", "down": true }
```

`code` is a physical key name from the UI Events `KeyboardEvent.code`
specification (`KeyA`, `Digit1`, `Enter`, `ShiftLeft`, `ArrowUp`, `F5`,
`Numpad0`, `MetaLeft`, `IntlBackslash`, …). The host translates it to a PC
scan code, so the character produced depends on the **host** keyboard
layout — like a physical keyboard plugged into the host. Unknown codes give
`invalid_request`. Clients should forward auto-repeat `keydown`s as
additional `down: true` messages.

Keys that the client OS or browser reserves (e.g. Ctrl+Alt+Del,
Cmd+Tab) cannot be captured by the client; Ctrl+Alt+Del cannot be injected
on the host at all.

#### 5.9.6 `text`

```json
{ "type": "text", "stream_id": "…", "text": "héllo 👋" }
```

Types Unicode text independent of the host layout (e.g. IME output,
paste-as-typing). At most 1024 UTF-16 code units per message; control
characters other than tab and newline are rejected. A client should send
either `key` or `text` for a given keystroke, never both.

#### 5.9.7 `release_all`

```json
{ "type": "release_all", "stream_id": "…" }
```

Releases every key and button the host holds down for this stream. Clients
should send it when they lose focus (e.g. `blur`, pointer-lock exit). The
host also does this automatically when the stream stops or the signaling
session ends.

#### 5.9.8 `get_input_state` / `input_state`

```json
{ "type": "get_input_state", "id": 11, "stream_id": "…" }
{
  "type": "input_state", "id": 11, "stream_id": "…",
  "x": 412, "y": 87,
  "modifiers": { "shift": false, "control": true, "alt": false, "meta": false,
                 "caps_lock": false, "num_lock": true, "scroll_lock": false },
  "mouse_mode": "absolute"
}
```

Reports the host's current pointer position and keyboard modifiers. It
may be sent once `stream_offer` was received (before that: `not_ready`) and
does not require the stream socket. `x`/`y` are integers in the same video
pixel space as absolute `mouse_move` (§5.9.2), so they can be sent back
unchanged:

* **Desktop stream:** the absolute pointer position relative to the
  primary monitor's top-left. When the pointer is on another monitor the
  values lie outside `0…width-1` / `0…height-1` and may be negative.
* **Window/application stream:** the position relative to the window's
  visible frame, or `-1, -1` when the pointer is outside that rectangle or
  the window is minimized. Containment is geometric: a pointer over another
  window that overlaps the streamed window still reports coordinates.

`modifiers` reflects the whole host keyboard (physical keys and injected
input from any source): `shift`, `control`, `alt`, `meta` (Windows key) are
"held down"; `caps_lock`, `num_lock`, `scroll_lock` are the toggle states.
Clients can use this to synchronize lock keys and to resume relative mode
from the host cursor.

#### 5.9.9 Focus

For window and application streams, a `mouse_button` press, a `key` press
or `text` first makes sure the target window has keyboard focus. The window
counts as focused when the foreground window belongs to the same process
(so the application's own dialogs and pop-ups keep working). Otherwise the
host restores it if minimized and brings it to the foreground. Moves, wheel
and releases never change focus. If focus cannot be obtained, the input is
dropped and an `error` with `focus_failed` is sent (once, until focus
succeeds again). Desktop streams never change focus.

#### 5.9.10 Limitations (informative)

* Windows blocks input into windows of elevated (administrator) processes
  unless the host itself runs elevated; such input is silently ignored.
* The secure desktop (UAC prompt, lock screen, Ctrl+Alt+Del screen) cannot
  receive input; the host reports `input_failed` (rate-limited).
* The host must run in the interactive user session (not as a service).
* Pop-up menus outside the window's rectangle cannot be reached through the
  stream of the window that opened them. Clients can open a stream for the
  pop-up itself after its `window_opened` (§5.12).

### 5.10 `attention` (host → client)

Reports taskbar attention requests (`FlashWindowEx`/`FlashWindow`, e.g. a
Teams chat or incoming call) so the client can replicate them locally (macOS
Dock bounce via `NSApp.requestUserAttention`, Windows `FlashWindowEx`). It is
an unsolicited event with no `id` and no reply.

```json
{
  "type": "attention",
  "stream_id": "Q2hOc3kV2bTqjG6dAVu0Sg",
  "state": "set",
  "window": { "hwnd": "0x1A2B3C", "pid": 4242, "title": "Microsoft Teams" }
}
```

| Field      | Meaning                                                                  |
|------------|--------------------------------------------------------------------------|
| `state`    | `"set"`: the window started requesting attention; `"clear"`: it stopped  |
| `window`   | The requesting window; `title` is omitted if the window no longer exists |

Scope:

* **Window streams** receive events for every top-level window of the
  streamed window's process (`pid`), not only the streamed `hwnd`, because
  applications such as Teams flash secondary windows. `window.hwnd` tells the client which one; it
  may differ from the stream's `source.hwnd`.
* **Desktop streams** receive events for all windows.
* Each event is sent once per matching stream, so a client with several
  streams may receive the same transition several times (distinct
  `stream_id`).

Delivery:

* Right after `stream_offer`, the host sends one `"set"` for every window in
  the stream's scope that is currently requesting attention, then live
  transitions. Events are only sent for streams whose `stream_offer` has been
  sent; nothing is replayed after `stream_stopped`.
* `"set"` is sent once per request; repeated flash cycles do not resend it.
* `"clear"` is sent when the window is activated (on the host, including by
  remote input focus, §5.9.9) or destroyed. A destroyed streamed window's
  `"clear"` precedes its `stream_stopped`.
* Flash requests of the window that is already in the foreground are
  ignored, matching the Windows taskbar.

Limitations (informative): the host observes the Windows shell hook
(`HSHELL_FLASH`/`HSHELL_WINDOWACTIVATED`/`HSHELL_WINDOWDESTROYED`). Windows
does not report `FLASHW_STOP`, or the end of a counted flash, distinguishably
from ordinary title/icon redraws. Such windows remain `"set"` until activated
or destroyed, which matches the taskbar button that stays highlighted until
activation. Badge overlays (e.g. unread counts on the taskbar icon) and toast
notifications are not reported. The host must run in the interactive user
session.

### 5.11 Clipboard

Session-level (no `stream_id`): lets a client mirror the host clipboard
locally (§5.11.1–2) and copy its own clipboard to the host (§5.11.3).

#### 5.11.1 `clipboard_changed` (host → client)

Sent to the authenticated session whenever the host clipboard changes. It
carries no content; the client calls `get_clipboard` if it wants the data.

```json
{ "type": "clipboard_changed", "sequence": 917, "formats": ["text", "html"], "excluded": false }
```

* `sequence` is the Windows clipboard sequence number; it increases on every
  change. One copy operation may produce several notifications (applications
  often add formats one by one); clients should debounce or compare
  `sequence` with the latest `clipboard` reply.
* `formats` lists which of `"text"`, `"html"` and `"image"` are available.
* `excluded` is `true` (and `formats` empty) when the owner marked the content
  with `ExcludeClipboardContentFromMonitorProcessing`, as password managers
  do. Clients should not fetch or mirror such content.

#### 5.11.2 `get_clipboard` / `clipboard`

```json
{ "type": "get_clipboard", "id": 8, "formats": ["text", "html", "image"] }
```

`formats` is optional (default: all three) and must be a non-empty array of
`"text"`, `"html"`, `"image"`. Reply:

```json
{
  "type": "clipboard",
  "id": 8,
  "sequence": 917,
  "text": "Bold & café",
  "html": "<b>Bold</b> &amp; café",
  "image": { "mime_type": "image/png", "width": 320, "height": 200, "data": "iVBORw0KGgo…" },
  "omitted": [ { "format": "image", "reason": "too_large" } ]
}
```

* Each content field is present only if requested and available.
  `sequence` identifies the clipboard state that was read.
* `text`: `CF_UNICODETEXT` as a JSON string (Windows `\r\n` line endings are
  preserved).
* `html`: the fragment of the Windows `"HTML Format"` data (between
  `StartFragment` and `EndFragment`, or the whole document if those are
  missing), suitable for the client's `text/html` clipboard flavour.
  Invalid UTF-8 is replaced with U+FFFD.
* `image`: PNG, **standard base64 with padding** (RFC 4648 §4, usable in a
  `data:image/png;base64,` URL), unlike the base64url used elsewhere. An
  application-provided `"PNG"` format is passed through (keeping
  transparency); otherwise the device-independent bitmap is converted
  (opaque).
* Size limit: text bytes + HTML bytes + base64 image characters must not
  exceed **10 MiB** (10,485,760) per reply, counted before JSON escaping and
  filled in the order text, HTML, image. A format that does not fit is listed
  in `omitted` with reason `"too_large"`; request fewer formats to fetch it
  alone.
* Other `omitted` reasons: `"excluded"` (see §5.11.1; nothing is returned)
  and `"unsupported"` (the data could not be decoded or converted).
* If another application holds the clipboard open for more than about
  100 ms, or it cannot be read, the reply is `error`
  `clipboard_unavailable`.
* Replies to `get_clipboard` may arrive after later replies (reading and PNG
  conversion happen off the network thread); match them by `id`.

#### 5.11.3 `set_clipboard` / `clipboard_set`

Replaces the whole host clipboard, as a local copy would:

```json
{
  "type": "set_clipboard",
  "id": 9,
  "text": "Hello ✓",
  "html": "<b>Hello</b> ✓",
  "image": { "mime_type": "image/png", "data": "iVBORw0KGgo…" },
  "sensitive": false
}
```

* At least one of `text`, `html`, `image` is required; omitted formats are
  not on the clipboard afterwards. Include `text` with `html`, because many
  applications only paste plain text; the host does not derive one from the
  other.
* `text` becomes `CF_UNICODETEXT`. `html` is a fragment that the host wraps
  in the Windows `"HTML Format"` header.
* `image.data` is a PNG in standard base64 (padding optional);
  `mime_type` is optional and must be `"image/png"`. The host places it as
  `"PNG"` (keeping transparency) and as a 32-bit `CF_DIB` (alpha composited
  onto white, since most applications ignore DIB alpha). Images are limited to
  16384 pixels per side and 64 Mpixel.
* `sensitive: true` (default `false`) marks the content as excluded from
  Windows clipboard history, cloud clipboard and clipboard monitors
  (`ExcludeClipboardContentFromMonitorProcessing`,
  `CanIncludeInClipboardHistory` = 0, `CanUploadToCloudClipboard` = 0). Use it
  for passwords (e.g. macOS `org.nspasteboard.ConcealedType`). The host will
  then report it as `excluded` to `get_clipboard` too.
* The same 10 MiB budget applies: text bytes + HTML bytes + base64 image
  characters. Larger requests get `invalid_request`; messages over 16 MiB
  close the socket with `1009`.
* Reply on success: `{ "type": "clipboard_set", "id": 9, "sequence": 962 }`.
  The host does **not** send `clipboard_changed` for its own write, so
  clients do not loop; `sequence` is the new clipboard sequence number.
* Errors: `invalid_request` (missing/ill-typed fields, invalid base64 or PNG,
  too large) and `clipboard_unavailable` (clipboard held by another
  application for more than about 100 ms, or the write failed; nothing is
  changed). Replies may arrive after later replies; match them by `id`.

Files (`CF_HDROP`), RTF and other formats are not exposed or accepted.

### 5.12 Windows (host → client)

Session-level (no `stream_id`): the host always reports every top-level window
on its desktop, so a client can decide which ones to present, for example by
starting a `window` stream (§5.3) for each new Teams window. These are
unsolicited events with no `id` and no reply.

```json
{ "type": "window_opened",  "window": { …window object… } }
{ "type": "window_changed", "window": { …window object… } }
{ "type": "window_closed",  "window": { …window object… } }
```

Window object:

```json
{
  "hwnd": "0x2A0C14",
  "pid": 4242,
  "image_name": "ms-teams.exe",
  "title": "Meeting with Alex | Microsoft Teams",
  "role": "main",
  "owner": "0x4EA0C14",
  "rect": { "x": 807, "y": 150, "width": 1280, "height": 720 },
  "state": "normal"
}
```

| Field        | Meaning |
|--------------|---------|
| `hwnd`       | Opaque window handle string; usable as `start_stream` `source.hwnd` |
| `pid`        | Owning process |
| `image_name` | Executable file name of the process; empty if it cannot be queried (e.g. some elevated or protected processes) |
| `title`      | Window text (UTF-8); may be empty |
| `role`       | `"main"`: has a taskbar button (unowned and activatable, or `WS_EX_APPWINDOW`), e.g. a main window, pop-out chat or meeting window. `"owned"`: owned activatable window such as a dialog. `"popup"`: menu, tooltip, flyout or other non-activatable / frameless transient window. `"tool"`: captioned tool window (palette). Advisory only |
| `owner`      | Owner window handle; omitted for unowned windows |
| `rect`       | Visible frame bounds (DWM extended frame bounds, without invisible resize borders and shadows) in **virtual-screen physical pixels**; the primary monitor's top-left is `0,0`, coordinates may be negative. For minimized windows: the last rectangle seen while not minimized (or the restore position) |
| `state`      | `"normal"`, `"minimized"` or `"maximized"` |

Rules:

* A window is reported while it is top-level, visible, not cloaked by DWM
  (e.g. on another virtual desktop) and has a non-empty rectangle. Hiding or
  cloaking reports `window_closed`; showing it again reports `window_opened`
  with the same `hwnd`. The host's own windows are not reported.
* Right after `authenticated`, the host sends `window_opened` for every
  current window, then live changes.
* `window_changed` carries the complete new object and is sent when any
  field differs. `window_closed` carries the last known object.
* Each batch is ordered: `window_closed`, then `window_opened`, then
  `window_changed`. A handle reused by another process is reported as closed,
  then opened.
* Changes are coalesced: a snapshot is taken about 2 ms after the first
  notification and at most once per 16 ms (about 60 Hz during a drag), plus
  every 250 ms to catch changes Windows does not announce. Intermediate states
  can be skipped.
* Closing a streamed window reports `window_closed` and stops the stream
  (`source_closed`); the order between the two is not guaranteed.

`rect` is for placement, not the video size; use the stream's `video`
dimensions (§5.5) for the picture. Absolute mouse coordinates (§5.9) remain
relative to each stream's video.

Limitations (informative): the host uses out-of-context `SetWinEventHook`
notifications plus `EnumWindows` snapshots and must run in the interactive
user session. Z-order and focus are not reported; icons are fetched on
request (§5.13). Opening a stream for a short-lived `popup` costs capture and
encoder startup time.

### 5.13 `get_window_icon` / `window_icon`

Fetches a window's icon as a PNG, e.g. for a Dock tile or window switcher.

```json
{ "type": "get_window_icon", "id": 12, "hwnd": "0x2A0C14", "size": 256 }
```

| Field  | Meaning |
|--------|---------|
| `hwnd` | Required top-level window handle string (as in §5.12) |
| `size` | Optional maximum width and height in pixels, integer 16–256; default 256 |

Reply:

```json
{
  "type": "window_icon",
  "id": 12,
  "hwnd": "0x2A0C14",
  "source": "executable",
  "mime_type": "image/png",
  "width": 256,
  "height": 256,
  "data": "standard base64 PNG"
}
```

* `data` is a 32-bit PNG with straight (non-premultiplied) alpha, at most
  `size` × `size` (never larger than 256 × 256). `width`/`height` match the
  PNG and may be smaller than `size` when only a smaller icon exists; icons
  are scaled down, never up. Clients scale for display themselves.
* `source` is `"window"` when the window's own icon (`WM_GETICON`, then the
  class icon) is at least `size` pixels wide, otherwise `"executable"` (the
  first icon of the process image, extracted at `size`), falling back to a
  smaller window icon when the executable has none. Per-window icons smaller
  than `size` are therefore replaced by the sharper application icon.
* Errors: `invalid_request` (bad `hwnd` or `size`), `not_found` (not an
  existing top-level window, or it closed meanwhile), `icon_unavailable`.
* The icon is read on a worker thread; replies to other requests may arrive
  first. A hung window delays its reply by at most about 200 ms per query.
  Icon changes are not announced; request again when needed.

Limitations (informative): windows hosted by another process (e.g. some
packaged apps shown by `ApplicationFrameHost.exe`) may report the host
process's icon. Taskbar overlay badges are not included.

## 6. Stream sockets

* URL: `wss://{host}/streams/{stream_id}?token={token}`; `token` is 32
  random bytes, base64url. It is valid once, for 10 s, only while the
  issuing signaling session is authenticated and open. Invalid, reused or
  expired tokens get HTTP `403` (no WebSocket upgrade).
* Only one socket per stream. The host sends binary messages only; any data
  message from the client closes the stream socket with `1003`.
* Encoding starts when the socket opens, and the first frame is a key frame.

## 7. Binary video frame format

Each binary WebSocket message holds exactly one encoded picture. All header
integers are **big-endian** (network order, `DataView` default).

| Offset | Size | Field             | Description                                            |
|--------|------|-------------------|--------------------------------------------------------|
| 0      | 1    | `version`         | `1`                                                    |
| 1      | 1    | `message_type`    | `1` = video access unit; ignore unknown types          |
| 2      | 2    | `header_length`   | Bytes before the payload (currently `32`)              |
| 4      | 4    | `flags`           | bit 0 key frame; bit 1 discontinuity; others reserved  |
| 8      | 4    | `generation`      | Video parameter generation (§5.5)                      |
| 12     | 4    | `reserved`        | `0`                                                    |
| 16     | 8    | `sequence`        | Per-stream counter of produced pictures, starting at 0 |
| 24     | 8    | `capture_time_us` | Host monotonic capture time (same clock as `pong`)     |
| 32     | …    | payload           | One Annex B access unit                                |

* Clients must locate the payload with `header_length`, not a constant, and
  must ignore unknown flag bits.
* Payload: start-code delimited NAL units including an access unit
  delimiter. Key frames repeat SPS/PPS (and VPS for H.265).
* `capture_time_us` is suitable as the WebCodecs `EncodedVideoChunk`
  `timestamp`; key frames map to `type: "key"`.
* **Discontinuity** (bit 1) is set on the first frame after the host
  dropped frames. Gaps in `sequence` show how many pictures were dropped.

### 7.1 Latency and back-pressure policy

The host never buffers more than a few frames per stream. If the socket
cannot keep up, queued frames are discarded, a key frame is requested, and
frames are skipped until that key frame, which is then sent with the
discontinuity flag. Clients should decode and present immediately (no
jitter buffer) and may drop late frames only at key-frame boundaries.

## 8. Encoder selection (informative)

The host always selects the encoder backend automatically. H.264 prefers native NVENC
4:4:4 (High 4:4:4 Predictive) and falls back to D3D12 4:2:0; H.265 tries
D3D12 4:4:4, then 4:2:2, then 4:2:0. The resulting sampling is reported in
`chroma_format` and `codec_string`. Browser hardware decoders often lack
4:4:4 support: a client should call `VideoDecoder.isConfigSupported()` and,
if unsupported, restart the stream with `"codec": "h265"`.

## 9. Close codes

| Code   | Use                                                       |
|--------|-----------------------------------------------------------|
| `1000` | Normal close / stream stopped                             |
| `1001` | Host shutting down                                        |
| `1002` | Unsupported protocol version                              |
| `1003` | Unexpected message type (binary on control, data on stream) |
| `1008` | Authentication failure or policy violation                |
| `1009` | Signaling message too large                               |
| `1011` | Internal host error                                       |
| `4001` | Signaling session superseded by a newer login             |

## 10. Reserved for future versions

These are not implemented in version 1; the names are reserved:

* Touch and pen input, clipboard file transfer.
* Audio: `message_type` `2` on a dedicated audio stream socket.
* Bitrate/frame-rate changes on a running stream.
