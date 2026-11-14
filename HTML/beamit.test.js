import assert from "node:assert/strict";
import { createHmac, pbkdf2Sync, webcrypto } from "node:crypto";
import test from "node:test";

import {
  base64UrlDecode,
  base64UrlEncode,
  computeAuthProofs,
  parseVideoFrame,
  parseAudioPacket,
  createAudioPacket,
  BeamItClient,
  bindBrowserInput,
} from "./beamit.js";

test("base64url uses the unpadded RFC 4648 URL alphabet", () => {
  assert.equal(base64UrlEncode(new Uint8Array()), "");
  assert.equal(base64UrlEncode(new TextEncoder().encode("foobar")), "Zm9vYmFy");
  assert.equal(base64UrlEncode(Uint8Array.of(0xfb, 0xff)), "-_8");
  assert.deepEqual(base64UrlDecode("-_8"), Uint8Array.of(0xfb, 0xff));
  assert.throws(() => base64UrlDecode("Zm9v="), /Invalid base64url/);
  assert.throws(() => base64UrlDecode("Zh"), /Non-canonical base64url/);
});

test("authentication proofs match PBKDF2-HMAC-SHA256 protocol construction", async () => {
  const salt = new TextEncoder().encode("beamit-test-salt");
  const serverNonce = Uint8Array.from({ length: 32 }, (_, index) => index);
  const clientNonce = Uint8Array.from({ length: 32 }, (_, index) => 255 - index);
  const password = "correct horse battery staple";
  const iterations = 1200;
  const proofs = await computeAuthProofs(password, salt, iterations, serverNonce, clientNonce, webcrypto);
  const key = pbkdf2Sync(password, salt, iterations, 32, "sha256");
  const message = (role) => Buffer.concat([
    Buffer.from(`BeamIt v1 ${role}\0`, "utf8"),
    Buffer.from(serverNonce),
    Buffer.from(clientNonce),
  ]);

  assert.deepEqual(Buffer.from(proofs.clientProof), createHmac("sha256", key).update(message("client")).digest());
  assert.deepEqual(Buffer.from(proofs.serverProof), createHmac("sha256", key).update(message("server")).digest());
  assert.notDeepEqual(proofs.clientProof, proofs.serverProof);
});

test("video frame parser honors header length, network byte order, and flags", () => {
  const bytes = new Uint8Array(41);
  const view = new DataView(bytes.buffer);
  view.setUint8(0, 1);
  view.setUint8(1, 1);
  view.setUint16(2, 36);
  view.setUint32(4, 0x80000003);
  view.setUint32(8, 7);
  view.setBigUint64(16, 0x0102030405060708n);
  view.setBigUint64(24, 0x1112131415161718n);
  bytes.set([1, 2, 3, 4, 5], 36);

  const frame = parseVideoFrame(bytes);
  assert.equal(frame.keyFrame, true);
  assert.equal(frame.discontinuity, true);
  assert.equal(frame.flags, 0x80000003);
  assert.equal(frame.generation, 7);
  assert.equal(frame.sequence, 0x0102030405060708n);
  assert.equal(frame.captureTimeUs, 0x1112131415161718n);
  assert.deepEqual(frame.payload, Uint8Array.of(1, 2, 3, 4, 5));
});

test("video frame parser rejects malformed headers and ignores unknown message types", () => {
  assert.throws(() => parseVideoFrame(new Uint8Array([1, 1, 0, 31])), /Truncated|Invalid/);
  const invalidHeader = new Uint8Array(33);
  invalidHeader[0] = 1;
  invalidHeader[1] = 1;
  new DataView(invalidHeader.buffer).setUint16(2, 31);
  assert.throws(() => parseVideoFrame(invalidHeader), /header length/);

  assert.equal(parseVideoFrame(Uint8Array.of(1, 2)), null);
});

test("version 2 Opus envelopes use network byte order and reject malformed packets", () => {
  const packet = createAudioPacket({
    trackId: 7,
    frameSize: 480,
    sequence: 3n,
    samples: Uint8Array.of(0x12, 0x34),
    discontinuity: true,
  });
  const parsed = parseAudioPacket(packet);
  assert.equal(parsed.trackId, 7);
  assert.equal(parsed.frameSize, 480);
  assert.equal(parsed.sequence, 3n);
  assert.equal(parsed.sampleTimestamp, 1440n);
  assert.equal(parsed.discontinuity, true);
  assert.deepEqual(parsed.payload, Uint8Array.of(0x12, 0x34));

  const invalid = packet.slice();
  invalid[0] = 1;
  assert.throws(() => parseAudioPacket(invalid), /version 2/);
  assert.throws(() => parseAudioPacket(packet.subarray(0, 32)), /size/);
});

class FakeTarget extends EventTarget {
  fire(type, properties = {}) {
    const event = new Event(type, { cancelable: true });
    Object.assign(event, properties);
    this.dispatchEvent(event);
    return event;
  }
}

class FakeSocket extends FakeTarget {
  static instances = [];
  readyState = 0;
  sent = [];
  constructor(url) {
    super();
    this.url = url;
    FakeSocket.instances.push(this);
  }
  send(text) { this.sent.push(typeof text === "string" ? JSON.parse(text) : text); }
  open() { this.readyState = 1; this.fire("open"); }
  receive(message) { this.fire("message", { data: JSON.stringify(message) }); }
  receiveBinary(data) { this.fire("message", { data }); }
  close(code = 1000, reason = "") { this.readyState = 3; this.fire("close", { code, reason }); }
}

const tick = () => new Promise((resolve) => setImmediate(resolve));

const opusConfiguration = {
  sampleRate: 48000, channels: 2, frameSize: 480, packetDuration: "PT0.01S",
  useFec: false, application: "VOIP",
};

class FakeAudioContext {
  constructor({ sampleRate }) {
    this.sampleRate = sampleRate;
    this.currentTime = 0;
    this.state = "running";
    this.destination = {};
    this.audioWorklet = { addModule: async () => {} };
  }
  resume() { return Promise.resolve(); }
  close() { this.state = "closed"; return Promise.resolve(); }
  createMediaStreamSource() { return { connect() {}, disconnect() {} }; }
  createBuffer(channels, frames, sampleRate) {
    const data = Array.from({ length: channels }, () => new Float32Array(frames));
    return { sampleRate, getChannelData: (channel) => data[channel] };
  }
  createBufferSource() { return { connect() {}, start() {}, stop() {} }; }
}

class FakeAudioData {
  constructor(init) {
    Object.assign(this, init);
    this.closed = false;
  }
  close() { this.closed = true; }
}

class FakeAudioDecoder {
  static async isConfigSupported() { return { supported: true }; }
  constructor(callbacks) { this.callbacks = callbacks; this.decodeQueueSize = 0; this.state = "unconfigured"; }
  configure(config) { this.config = config; this.state = "configured"; }
  reset() { this.state = "unconfigured"; }
  decode(chunk) {
    this.callbacks.output({
      sampleRate: this.config.sampleRate,
      numberOfChannels: this.config.numberOfChannels,
      numberOfFrames: 480,
      copyTo(destination) { destination.fill(0); },
      close() {},
    });
  }
  close() { this.state = "closed"; }
}

class FakeAudioEncoder {
  static async isConfigSupported() { return { supported: true }; }
  constructor(callbacks) { this.callbacks = callbacks; this.encodeQueueSize = 0; this.state = "unconfigured"; }
  configure(config) { this.config = config; this.state = "configured"; }
  encode(frame) {
    const chunk = {
      timestamp: frame.timestamp,
      duration: 10000,
      byteLength: 2,
      copyTo(destination) { destination.set([0x22, 0x33]); },
    };
    this.callbacks.output(chunk);
  }
  close() { this.state = "closed"; }
}

class FakeAudioWorkletNode {
  static instances = [];
  constructor(context, name, options) {
    this.options = options;
    this.port = { onmessage: null, postMessage() {} };
    FakeAudioWorkletNode.instances.push(this);
  }
  connect() {}
  disconnect() {}
}

function fakeAudioOptions() {
  return {
    AudioContext: FakeAudioContext,
    AudioDecoder: FakeAudioDecoder,
    EncodedAudioChunk: class { constructor(init) { Object.assign(this, init); } },
    AudioEncoder: FakeAudioEncoder,
    AudioData: FakeAudioData,
    AudioWorkletNode: FakeAudioWorkletNode,
    mediaDevices: {
      async getUserMedia() {
        return { getTracks: () => [{ stop() {} }] };
      },
    },
  };
}

async function inputSession(t, beforeOpen) {
  const client = new BeamItClient({ url: "https://test:8443", WebSocket: FakeSocket, crypto: webcrypto });
  t.after(() => client.disconnect());
  const connecting = client.connect("password");
  const socket = FakeSocket.instances.at(-1);
  socket.open();
  const salt = Buffer.from("test-salt");
  const nonce = Buffer.alloc(32, 5);
  socket.receive({
    type: "hello", protocol: 1,
    auth: { mechanism: "pbkdf2-sha256-hmac", salt: base64UrlEncode(salt), server_nonce: base64UrlEncode(nonce), iterations: 600000 },
  });
  while (socket.sent.length === 0) await tick();
  const auth = socket.sent[0];
  const key = pbkdf2Sync("password", salt, 600000, 32, "sha256");
  const proof = createHmac("sha256", key).update(Buffer.concat([
    Buffer.from("BeamIt v1 server\0"), nonce, base64UrlDecode(auth.client_nonce),
  ])).digest();
  socket.receive({ type: "authenticated", server_proof: base64UrlEncode(proof), host_time_us: 0 });
  await connecting;
  const starting = client.startStream({ source: { kind: "desktop" }, input: { mouse_mode: "relative" } });
  const request = socket.sent.at(-1);
  assert.deepEqual(request.input, { mouse_mode: "relative" });
  socket.receive({
    type: "stream_offer", id: request.id, stream_id: "test-stream",
    url: `wss://test:8443/streams/test-stream?token=${base64UrlEncode(new Uint8Array(32))}`,
    token_expires_in_ms: 10000, input: { mouse_mode: "relative" },
    video: { generation: 1, codec: "h264", codec_string: "avc1.4D001F", width: 1920, height: 1080 },
  });
  await tick();
  const videoSocket = FakeSocket.instances.at(-1);
  if (beforeOpen) await beforeOpen(client, socket);
  videoSocket.open();
  return { client, socket, stream: await starting };
}

test("audio offers, capture tracks, playback decode, and microphone uplink use negotiated metadata", async (t) => {
  const { client, socket } = await inputSession(t);
  FakeAudioWorkletNode.instances = [];
  const starting = client.startAudio(fakeAudioOptions());
  const startRequest = socket.sent.at(-1);
  assert.deepEqual(startRequest, { type: "start_audio", id: startRequest.id });
  socket.receive({
    type: "audio_offer",
    id: startRequest.id,
    audio_id: "test-audio",
    url: `wss://test:8443/audio/test-audio?token=${base64UrlEncode(new Uint8Array(32))}`,
    token_expires_in_ms: 10000,
    audio_version: 2,
    hostToClient: { tracks: [] },
    clientToHost: {
      track_id: 1, source: { kind: "microphone" }, codec: "opus",
      opusAudioConfiguration: opusConfiguration,
    },
  });
  await tick();
  const audioSocket = FakeSocket.instances.at(-1);
  audioSocket.open();
  const audio = await starting;
  await audio.startMicrophone();

  const adding = audio.startTrack({ kind: "application", pid: 4242 });
  const addRequest = socket.sent.at(-1);
  assert.deepEqual(addRequest.source, { kind: "application", pid: 4242 });
  socket.receive({
    type: "audio_track_started", id: addRequest.id, audio_id: audio.audioId,
    track: {
      track_id: 2, source: { kind: "application", pid: 4242, include_process_tree: true },
      codec: "opus", opusAudioConfiguration: opusConfiguration,
    },
  });
  const track = await adding;
  assert.equal(track.track_id, 2);

  audioSocket.receiveBinary(createAudioPacket({
    trackId: 2, frameSize: 480, sequence: 0n, samples: Uint8Array.of(0x55), discontinuity: true,
  }));
  await tick();
  assert.equal(FakeAudioWorkletNode.instances.length, 1);
  const worklet = FakeAudioWorkletNode.instances[0];
  worklet.port.onmessage({
    data: { sequence: 0, samples: new Float32Array(opusConfiguration.frameSize * opusConfiguration.channels) },
  });
  const uplink = parseAudioPacket(audioSocket.sent.at(-1));
  assert.equal(uplink.trackId, 1);
  assert.equal(uplink.sequence, 0n);
  assert.equal(uplink.discontinuity, true);
  assert.deepEqual(uplink.payload, Uint8Array.of(0x22, 0x33));

  const stoppingTrack = audio.stopTrack(track.track_id);
  const stopTrackRequest = socket.sent.at(-1);
  socket.receive({
    type: "audio_track_stopped", id: stopTrackRequest.id,
    audio_id: audio.audioId, track_id: track.track_id, reason: "requested",
  });
  await stoppingTrack;
  assert.equal(audio.tracks.length, 0);

  const addingDesktop = audio.startTrack({ kind: "desktop" });
  const desktopRequest = socket.sent.at(-1);
  assert.deepEqual(desktopRequest.source, { kind: "desktop" });
  socket.receive({
    type: "audio_track_started", id: desktopRequest.id, audio_id: audio.audioId,
    track: {
      track_id: 3, source: { kind: "desktop", exclude_host_process_tree: true },
      codec: "opus", opusAudioConfiguration: opusConfiguration,
    },
  });
  const desktopTrack = await addingDesktop;
  const countBeforeRejectedSource = socket.sent.length;
  await assert.rejects(audio.startTrack({ kind: "application", pid: 4242 }), TypeError);
  assert.equal(socket.sent.length, countBeforeRejectedSource);
  const stoppingDesktop = audio.stopTrack(desktopTrack.track_id);
  const stopDesktopRequest = socket.sent.at(-1);
  socket.receive({
    type: "audio_track_stopped", id: stopDesktopRequest.id,
    audio_id: audio.audioId, track_id: desktopTrack.track_id, reason: "requested",
  });
  await stoppingDesktop;

  const stoppingAudio = audio.stop();
  const stopAudioRequest = socket.sent.at(-1);
  socket.receive({ type: "audio_stopped", id: stopAudioRequest.id, audio_id: audio.audioId, reason: "requested" });
  await stoppingAudio;
  assert.equal(audio.ready, false);
});

test("input commands use the authenticated control socket without waiting for success replies", async (t) => {
  const unauthenticated = new BeamItClient({ WebSocket: FakeSocket });
  assert.throws(() => unauthenticated.key("test-stream", "KeyA", true), { code: "not_authenticated" });
  const { client, socket, stream } = await inputSession(t, (client) => {
    assert.throws(() => client.mouseMove("test-stream", { dx: 0, dy: 0 }), { code: "not_ready" });
  });
  assert.equal(stream.mouseMode, "relative");
  const offset = socket.sent.length;
  stream.mouseMove({ dx: -3.5, dy: 7 });
  stream.mouseButton("back", true);
  stream.mouseButton("back", false);
  stream.mouseWheel({ dy: 1 });
  stream.key("KeyA", true);
  stream.key("KeyA", true);
  stream.key("KeyA", false);
  stream.text("héllo 👋\n\t");
  stream.releaseAll();
  assert.deepEqual(socket.sent.slice(offset), [
    { type: "mouse_move", stream_id: stream.id, dx: -3.5, dy: 7 },
    { type: "mouse_button", stream_id: stream.id, button: "back", down: true },
    { type: "mouse_button", stream_id: stream.id, button: "back", down: false },
    { type: "mouse_wheel", stream_id: stream.id, dx: 0, dy: 1 },
    ...[true, true, false].map((down) => ({ type: "key", stream_id: stream.id, code: "KeyA", down })),
    { type: "text", stream_id: stream.id, text: "héllo 👋\n\t" },
    { type: "release_all", stream_id: stream.id },
  ]);
  const changing = stream.setMouseMode("absolute");
  const request = socket.sent.at(-1);
  socket.receive({ type: "mouse_mode", id: request.id, stream_id: stream.id, mode: "absolute" });
  await changing;
  stream.mouseMove({ x: 640.5, y: 360 });
  assert.equal(socket.sent.at(-1).x, 640.5);
  assert.equal(stream.mouseMode, "absolute");

  assert.throws(() => stream.mouseMove({ dx: 1, dy: 2 }), /requires/);
  assert.throws(() => stream.mouseMove({ x: NaN, y: 2 }), /requires/);
  assert.throws(() => stream.mouseWheel({ dy: 12001 }), /integer/);
  assert.throws(() => stream.mouseWheel({ dy: 0.5 }), /integer/);
  assert.throws(() => stream.mouseButton("invalid", true), /supported/);
  assert.throws(() => stream.text("a".repeat(1025)), /1024/);
  assert.throws(() => stream.text("\r"), /controls/);
  assert.throws(() => stream.text("\ud800"), /Unicode/);
  assert.throws(() => client.releaseAll("foreign-stream"), { code: "not_found" });
  let inputError;
  client.on("protocolError", (error) => { inputError = error; });
  socket.receive({ type: "error", code: "focus_failed", stream_id: stream.id, message: "Focus failed" });
  await tick();
  assert.equal(inputError.code, "focus_failed");
  assert.equal(inputError.streamId, stream.id);
  stream.close();
  assert.throws(() => stream.key("KeyA", true), { code: "not_found" });
});

test("input state and mouse mode requests handle replies and host errors", async (t) => {
  const { socket, stream } = await inputSession(t);
  const querying = stream.getInputState();
  const request = socket.sent.at(-1);
  const response = {
    type: "input_state", id: request.id, stream_id: stream.id, x: -1, y: -1, mouse_mode: "relative",
    modifiers: { shift: false, control: true, alt: false, meta: false, caps_lock: false, num_lock: true, scroll_lock: false },
  };
  socket.receive(response);
  assert.deepEqual(await querying, response);
  const changing = stream.setMouseMode("absolute");
  socket.receive({ type: "error", id: socket.sent.at(-1).id, code: "not_ready", message: "Not ready" });
  await assert.rejects(changing, { code: "not_ready" });
  assert.equal(stream.mouseMode, "relative");
});

test("capture selection is preserved and invalid source/backend combinations are rejected before sending", async (t) => {
  const { client, socket, stream } = await inputSession(t);
  stream.close();
  const cases = [
    { kind: "desktop" },
    { kind: "desktop", capture: "dxgi" },
    { kind: "desktop", capture: "wgc" },
    { kind: "window", hwnd: "0xABC", capture: "wgc" },
    { kind: "application", pid: 4242, capture: "wgc" },
  ];
  for (const [index, source] of cases.entries()) {
    const starting = client.startStream({ source });
    const request = socket.sent.at(-1);
    assert.deepEqual(request.source, source);
    assert.equal(Object.hasOwn(request, "video"), false);
    const id = `capture-${index}`;
    socket.receive({
      type: "stream_offer", id: request.id, stream_id: id,
      source: { ...source, capture: source.capture ?? "dxgi", dpi: { x: 144, y: 144 } },
      url: `wss://test:8443/streams/${id}?token=${base64UrlEncode(new Uint8Array(32))}`,
      token_expires_in_ms: 10000, input: { mouse_mode: "absolute" },
      video: { generation: 1, codec: "h264", codec_string: "avc1.4D001F", width: 1920, height: 1080 },
    });
    await tick();
    FakeSocket.instances.at(-1).open();
    const opened = await starting;
    assert.equal(opened.offer.source.capture, source.capture ?? "dxgi");
    assert.deepEqual(opened.offer.source.dpi, { x: 144, y: 144 });
    opened.close();
  }
  const count = socket.sent.length;
  for (const source of [
    { kind: "window", hwnd: "0xABC", capture: "dxgi" },
    { kind: "application", pid: 4242, capture: "dxgi" },
    { kind: "desktop", capture: "unknown" },
  ]) {
    await assert.rejects(client.startStream({ source }), TypeError);
  }
  assert.equal(socket.sent.length, count);
});

function browserFixture() {
  const window = new FakeTarget();
  const document = new FakeTarget();
  const element = new FakeTarget();
  document.defaultView = window;
  document.activeElement = null;
  document.pointerLockElement = null;
  element.ownerDocument = document;
  element.focus = () => { document.activeElement = element; };
  element.blur = () => { document.activeElement = null; element.fire("blur"); };
  element.getBoundingClientRect = () => ({ left: 10, top: 20, width: 400, height: 200 });
  element.requestPointerLock = () => { document.pointerLockElement = element; document.fire("pointerlockchange"); };
  document.exitPointerLock = () => { document.pointerLockElement = null; document.fire("pointerlockchange"); };
  const calls = [];
  const stream = {
    ready: true, mouseMode: "absolute", video: { width: 800, height: 400 },
    on() { return () => {}; },
  };
  for (const name of ["mouseMove", "mouseButton", "mouseWheel", "key", "text", "releaseAll"]) {
    stream[name] = (...args) => calls.push([name, ...args]);
  }
  const errors = [];
  const binding = bindBrowserInput(stream, element, { onError: (error) => errors.push(error) });
  return { window, document, element, stream, calls, errors, binding };
}

test("browser binding scales coordinates, orders clicks, preserves repeats, and releases on blur", () => {
  const { window, element, calls, binding, errors } = browserFixture();
  element.fire("mousemove", { clientX: 210, clientY: 120 });
  assert.equal(calls.length, 0);
  element.fire("mousedown", { button: 0, clientX: 210, clientY: 120 });
  assert.deepEqual(calls.slice(0, 2), [["mouseMove", { x: 400, y: 200 }], ["mouseButton", "left", true]]);
  window.fire("mouseup", { button: 0, clientX: 900, clientY: 900 });
  assert.deepEqual(calls.at(-2), ["mouseMove", { x: 799, y: 399 }]);
  assert.deepEqual(calls.at(-1), ["mouseButton", "left", false]);
  element.fire("keydown", { code: "KeyA" });
  element.fire("keydown", { code: "KeyA", repeat: true });
  window.fire("keyup", { code: "KeyA" });
  assert.deepEqual(calls.slice(-3), [["key", "KeyA", true], ["key", "KeyA", true], ["key", "KeyA", false]]);
  element.fire("keydown", { code: "KeyA", isComposing: true });
  assert.equal(calls.at(-1)[2], false);
  element.fire("compositionend", { data: "é" });
  assert.deepEqual(calls.at(-1), ["text", "é"]);
  element.fire("blur");
  assert.deepEqual(calls.at(-1), ["releaseAll"]);
  binding.dispose();
  const count = calls.length;
  element.fire("mousedown", { button: 0, clientX: 210, clientY: 120 });
  assert.equal(calls.length, count);
  assert.deepEqual(errors, []);
});

test("browser wheel accumulation, relative lock, and Escape release", () => {
  const { document, element, stream, calls, binding } = browserFixture();
  element.focus();
  for (let index = 0; index < 2; index += 1) {
    element.fire("wheel", { deltaMode: 0, deltaX: 0, deltaY: 0.5, clientX: 20, clientY: 30 });
  }
  assert.deepEqual(calls.filter(([name]) => name === "mouseWheel"), [["mouseWheel", { dx: 0, dy: 1 }]]);
  stream.mouseMode = "relative";
  calls.length = 0;
  element.fire("mousedown", { button: 0 });
  assert.equal(document.pointerLockElement, element);
  assert.equal(calls.length, 0);
  element.fire("mousemove", { movementX: -3, movementY: 7 });
  assert.deepEqual(calls.at(-1), ["mouseMove", { dx: -3, dy: 7 }]);
  element.fire("keydown", { code: "Escape" });
  assert.equal(document.pointerLockElement, null);
  assert.ok(calls.some(([name]) => name === "releaseAll"));
  assert.ok(!calls.some(([name, code]) => name === "key" && code === "Escape"));
  binding.dispose();
});

test("browser binding releases on page hide, window blur, and pointer-lock exit", () => {
  const { document, window, element, calls, binding, stream } = browserFixture();
  element.focus();
  document.hidden = true;
  document.fire("visibilitychange");
  assert.deepEqual(calls.at(-1), ["releaseAll"]);
  calls.length = 0;
  window.fire("blur");
  assert.deepEqual(calls.at(-1), ["releaseAll"]);
  calls.length = 0;
  document.pointerLockElement = element;
  document.exitPointerLock();
  assert.ok(calls.some(([name]) => name === "releaseAll"));
  stream.ready = false;
  calls.length = 0;
  binding.dispose();
  assert.equal(calls.length, 0);
});
