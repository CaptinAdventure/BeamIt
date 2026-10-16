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
  |--- list_windows ------------------------>|
  |<------------------------- windows -------|
  |--- start_stream ------------------------>|  (host starts capture + encoder)
  |<-------------------- stream_offer -------|  (codec parameters + dedicated URL)
  |=== WSS /streams/{id}?token=… ===========>|
  |<========== binary video frames ==========|
  |<-------------------- stream_config ------|  (only when the encoder is reconfigured)
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
* Limits: signaling messages are at most 64 KiB; an unauthenticated control
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

Lists visible, capturable top-level windows. `primary` marks the largest
window of its process (the window chosen for `{"kind":"application"}`).
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
* `"desktop"` — the primary monitor (host's configured capture backend).

`video` and each of its fields are optional; host command-line defaults are
used for omitted fields. `codec` is `"h264"` or `"h265"`; `frame_rate` is
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
  "source": { "kind": "window", "hwnd": "0x1A2B3C", "pid": 4242, "title": "Microsoft Teams" },
  "video": { …video parameters, §5.5… }
}
```

The `url` authority is copied from the `Host` header of the `/control`
upgrade so it matches the certificate exception the browser already holds.
The client should open it within `token_expires_in_ms`; otherwise the host
stops the stream with reason `token_expired`.

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

The host operator chooses the encoder backend and capture backend on the
command line. With the default automatic encoder, H.264 prefers native NVENC
4:4:4 (High 4:4:4 Predictive) and falls back to D3D12 4:2:0; H.265 tries
D3D12 4:4:4, then 4:2:2, then 4:2:0. The resulting sampling is reported in
`chroma_format` and `codec_string`. Browser hardware decoders often lack
4:4:4 support: a client should call `VideoDecoder.isConfigSupported()` and,
if unsupported, restart the stream with `"codec": "h265"`/a different host
configuration.

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

* `window_opened` / `window_closed` events so clients can open streams for
  secondary windows (e.g. Teams pop-outs and meeting windows) automatically,
  plus window geometry/z-order metadata for remote composition.
* Input: `input` messages for keyboard and mouse.
* Audio: `message_type` `2` on a dedicated audio stream socket.
* Bitrate/frame-rate changes on a running stream.
