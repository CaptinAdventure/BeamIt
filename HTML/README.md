# BeamIt JavaScript protocol library

`beamit.js` is a browser-native client library for the signaling and video
frame contract in the repository's `PROTOCOL.md`. `index.html` is a test UI
that exercises the host connection, authentication, window listing, stream
controls, and WebCodecs video playback.

Load it from a secure origin (HTTPS). The BeamIt host uses WSS only; browsers
must first trust the host certificate, for example by visiting its HTTPS
origin. The host serves this test page at `/` and the module at `/beamit.js`.
Open the host HTTPS URL; the page defaults its address field to the current
host. The library performs the protocol's PBKDF2-HMAC-SHA256 mutual
authentication, exposes signaling requests, opens token-authorized stream
sockets, tracks encoder generations, and parses binary video frames. It does
not implement input forwarding, audio, or the protocol's reserved window
lifecycle events.

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
