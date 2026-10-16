# BeamIt JavaScript protocol library

`beamit.js` is a browser-native client library for the signaling and video
frame contract in the repository's `PROTOCOL.md`. `index.html` is a test UI
that exercises the host connection, authentication, window listing, stream
controls, WebCodecs video playback, and keyboard/mouse input.

Load it from a secure origin (HTTPS). The BeamIt host uses WSS only; browsers
must first trust the host certificate, for example by visiting its HTTPS
origin. The host serves this test page at `/` and the module at `/beamit.js`.
Open the host HTTPS URL; the page defaults its address field to the current
host. The library performs the protocol's PBKDF2-HMAC-SHA256 mutual
authentication, exposes signaling requests, opens token-authorized stream
sockets, tracks encoder generations, parses binary video frames, and forwards
input according to protocol section 5.9. Audio and the reserved window
lifecycle events are not implemented.

## Window attention

The test page handles protocol section 5.10 `attention` events. A banner lists
each window currently requesting attention for the active stream, and the
browser tab title shows the count. Secondary windows are tracked by their
own opaque HWND, even when they differ from the captured window. Initial
events received while the stream socket is opening are retained.

The host's `clear` removes only the matching window; stopping the stream or
disconnecting clears the display and restores the tab title. Local focus
does not dismiss attention: the host clears it when the window is activated
or destroyed. No notifications, sounds, or automatic host focus are requested.
Browsers do not expose native Dock/taskbar flashing through this page.

## Browser input testing

Start a stream, then click the video to focus it and forward mouse and
physical keyboard events. Coordinates are scaled from the rendered canvas
to the latest video dimensions. Mouse buttons, including back/forward, and
both wheel axes are supported. Pixel-wheel fractions are accumulated and
scaled at 1.2 Windows wheel units per pixel; a line is one 120-unit notch.

Choose **Relative** mouse mode and click the video to acquire pointer lock.
The initial click only acquires the lock; subsequent clicks go to the host.
Press **Escape** to leave control. Focus loss, hidden tabs, pointer-lock exit,
and binding disposal send `release_all` while the stream is still open.
The host also releases held input on stream/session teardown. Input is only
captured on the focused video, not in the page's form fields.

Physical keys use `KeyboardEvent.code`, including repeated keydowns, so
characters depend on the **host** keyboard layout. Escape is reserved by the
test page to release control. OS/browser-reserved shortcuts may not reach the
page, and Ctrl+Alt+Del cannot be injected. Use the separate **Unicode text**
box for IME output or paste-as-typing, then click **Send text**; typing in
that box does not also send physical key events. Text is limited to 1024
UTF-16 code units, with no control characters except tab/newline.

**Get input state** displays the host pointer coordinates, held modifiers,
lock toggles, and mouse mode. It does not toggle local lock keys or move the
local pointer automatically. Input success is unacknowledged by design;
host errors such as `focus_failed` or `input_failed` appear in the protocol
log. Password holders have full interactive-desktop input access; the window
boundary is not a security restriction.

## Library input API

`startStream` accepts optional `input: { mouse_mode: "absolute" | "relative" }`.
Each returned stream exposes `mouseMode` from its offer, `setMouseMode(mode)`
and `getInputState()` (both return reply promises), and these synchronous,
fire-and-forget methods:

```js
stream.mouseMove({ x: 640.5, y: 360 }); // absolute mode
await stream.setMouseMode("relative");
stream.mouseMove({ dx: -3, dy: 7 });   // relative mode
stream.mouseButton("left", true);
stream.mouseButton("left", false);
stream.mouseWheel({ dx: 0, dy: 120 }); // positive dy scrolls down
stream.key("KeyA", true);
stream.key("KeyA", false);
stream.text("Unicode text");
stream.releaseAll();
const state = await stream.getInputState();
```

The equivalent client methods take `streamId` as their first argument.
Commands go over the authenticated control socket, never the video socket.
Input requires a session-owned, open stream; `getInputState` can also be
used with an offered stream whose video socket has not opened yet. Listen
to `client.on("protocolError", ...)` for unacknowledged input errors.

To bind a focusable canvas without building event handling yourself:

```js
import { bindBrowserInput } from "./beamit.js";

canvas.tabIndex = 0;
const binding = bindBrowserInput(stream, canvas, { onError: console.error });
// Before replacing the stream, disabling control, or changing its mouse mode:
binding.dispose();
```

The binding owns DOM listeners, not the stream. Rebind after a mouse-mode
change. It uses raw pointer-lock deltas in relative mode and sends a mouse
move before button presses and wheel events. It does not forward paste
automatically: call `stream.text()` explicitly instead of forwarding physical
keys for the same text.

## Video API

```js
import { BeamItClient, createWebCodecsDecoder } from "./beamit.js";

const client = new BeamItClient({ url: "https://192.168.1.20:8443/" });
await client.connect(password); // Prompt for the password; do not hard-code it.

const windows = await client.listWindows();
const stream = await client.startStream({
  source: { kind: "window", hwnd: windows[0].hwnd },
  video: { codec: "h264", frame_rate: 60, bitrate_kbps: 20000 },
});

const decoder = await createWebCodecsDecoder(stream, {
  output(videoFrame) {
    // Present immediately; close the VideoFrame when presentation is done.
    render(videoFrame);
  },
});

// Later:
await stream.stop();
decoder.close();
client.disconnect();
```

Without WebCodecs, consume `stream.on("frame", frame => ...)`; each frame
contains the Annex B access unit in `payload`, the capture timestamp in
microseconds, sequence/generation metadata, and its matching `video`
parameters. Unknown parameter generations are held briefly while the
signaling socket delivers `stream_config`; the library drops older unmatched
frames instead of building a latency queue.

Run the dependency-free unit tests with `npm test` in this directory (Node.js
with Web Crypto and `node:test`).
