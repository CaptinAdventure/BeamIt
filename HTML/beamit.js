const PROTOCOL_VERSION = 1;
const AUTH_MECHANISM = "pbkdf2-sha256-hmac";
const PBKDF2_ITERATIONS = 600_000;
const MAX_SIGNALING_BYTES = 64 * 1024;
const FRAME_VERSION = 1;
const VIDEO_MESSAGE = 1;
const FRAME_HEADER_MIN_SIZE = 32;
const AUDIO_VERSION = 2;
const AUDIO_MESSAGE = 2;
const AUDIO_HEADER_SIZE = 32;
const MAX_AUDIO_PACKET_SIZE = 1307;
const MAX_AUDIO_QUEUE = 3;

const encoder = new TextEncoder();

if (typeof globalThis.AudioWorkletProcessor === "function" &&
    typeof globalThis.registerProcessor === "function") {
  class BeamItMicrophoneProcessor extends AudioWorkletProcessor {
    constructor(options) {
      super();
      this.frameSize = options.processorOptions.frameSize;
      this.channels = options.processorOptions.channels;
      this.frame = new Float32Array(this.frameSize * this.channels);
      this.position = 0;
      this.sequence = 0;
      this.pending = 0;
      this.port.onmessage = (event) => {
        if (event.data?.type === "consumed") this.pending = Math.max(0, this.pending - 1);
      };
    }

    process(inputs, outputs) {
      const input = inputs[0] ?? [];
      const output = outputs[0] ?? [];
      for (const channel of output) channel.fill(0);
      const frames = input[0]?.length ?? 0;
      for (let index = 0; index < frames; index += 1) {
        let mono = 0;
        for (const channel of input) mono += channel[index] ?? 0;
        if (input.length > 0) mono /= input.length;
        for (let channel = 0; channel < this.channels; channel += 1) {
          this.frame[this.position * this.channels + channel] =
            this.channels === 1 || input.length === 1
              ? mono
              : (input[channel]?.[index] ?? mono);
        }
        this.position += 1;
        if (this.position !== this.frameSize) continue;
        const sequence = this.sequence++;
        if (this.pending < MAX_AUDIO_QUEUE) {
          const samples = this.frame;
          this.frame = new Float32Array(this.frameSize * this.channels);
          this.pending += 1;
          this.port.postMessage({ sequence, samples }, [samples.buffer]);
        }
        this.position = 0;
      }
      return true;
    }
  }
  globalThis.registerProcessor("beamit-microphone", BeamItMicrophoneProcessor);
}

export class BeamItError extends Error {
  constructor(message, { code = "client_error", id, streamId, cause } = {}) {
    super(message, { cause });
    this.name = "BeamItError";
    this.code = code;
    this.id = id;
    this.streamId = streamId;
  }
}

class EventHub {
  #listeners = new Map();

  on(type, listener) {
    if (typeof listener !== "function") {
      throw new TypeError("listener must be a function");
    }
    let listeners = this.#listeners.get(type);
    if (!listeners) {
      listeners = new Set();
      this.#listeners.set(type, listeners);
    }
    listeners.add(listener);
    return () => listeners.delete(listener);
  }

  listenerCount(type) {
    return this.#listeners.get(type)?.size ?? 0;
  }

  emit(type, detail) {
    for (const listener of this.#listeners.get(type) ?? []) {
      listener(detail);
    }
  }
}

export function base64UrlEncode(bytes) {
  let binary = "";
  for (const byte of bytes) binary += String.fromCharCode(byte);
  return btoa(binary).replaceAll("+", "-").replaceAll("/", "_").replace(/=+$/u, "");
}

export function base64UrlDecode(text) {
  if (typeof text !== "string" || !/^[A-Za-z0-9_-]*$/u.test(text) || text.length % 4 === 1) {
    throw new BeamItError("Invalid base64url value", { code: "invalid_message" });
  }
  const base64 = text.replaceAll("-", "+").replaceAll("_", "/");
  let binary;
  try {
    binary = atob(base64 + "=".repeat((4 - (base64.length % 4)) % 4));
  } catch (cause) {
    throw new BeamItError("Invalid base64url value", { code: "invalid_message", cause });
  }
  const bytes = Uint8Array.from(binary, (character) => character.charCodeAt(0));
  if (base64UrlEncode(bytes) !== text) {
    throw new BeamItError("Non-canonical base64url value", { code: "invalid_message" });
  }
  return bytes;
}

function concatenate(...parts) {
  const result = new Uint8Array(parts.reduce((length, part) => length + part.byteLength, 0));
  let offset = 0;
  for (const part of parts) {
    result.set(part, offset);
    offset += part.byteLength;
  }
  return result;
}

export async function computeAuthProofs(password, salt, iterations, serverNonce, clientNonce, cryptoApi = globalThis.crypto) {
  if (typeof password !== "string" || !(salt instanceof Uint8Array) ||
      !Number.isSafeInteger(iterations) || iterations < 1 ||
      !(serverNonce instanceof Uint8Array) || !(clientNonce instanceof Uint8Array)) {
    throw new TypeError("Invalid authentication inputs");
  }
  if (!cryptoApi?.subtle) {
    throw new BeamItError("Web Crypto is unavailable; use a secure context", { code: "crypto_unavailable" });
  }

  const passwordKey = await cryptoApi.subtle.importKey("raw", encoder.encode(password), "PBKDF2", false, ["deriveBits"]);
  const derived = await cryptoApi.subtle.deriveBits(
    { name: "PBKDF2", hash: "SHA-256", salt, iterations },
    passwordKey,
    256,
  );
  const hmacKey = await cryptoApi.subtle.importKey("raw", derived, { name: "HMAC", hash: "SHA-256" }, false, ["sign"]);
  const prefix = (role) => concatenate(encoder.encode(`BeamIt v1 ${role}`), new Uint8Array([0]), serverNonce, clientNonce);
  const [clientProof, serverProof] = await Promise.all([
    cryptoApi.subtle.sign("HMAC", hmacKey, prefix("client")),
    cryptoApi.subtle.sign("HMAC", hmacKey, prefix("server")),
  ]);
  return {
    clientProof: new Uint8Array(clientProof),
    serverProof: new Uint8Array(serverProof),
  };
}

function equalBytes(left, right) {
  if (left.byteLength !== right.byteLength) return false;
  let difference = 0;
  for (let index = 0; index < left.byteLength; index += 1) {
    difference |= left[index] ^ right[index];
  }
  return difference === 0;
}

function protocolError(message, code = "invalid_message", properties = {}) {
  return new BeamItError(message, { code, ...properties });
}

function validateMouseMode(mode) {
  if (mode !== "absolute" && mode !== "relative") {
    throw new TypeError('mouse mode must be "absolute" or "relative"');
  }
  return mode;
}

function validateSource(source) {
  if (!source || typeof source !== "object") throw new TypeError("source is required");
  const capture = {};
  if (source.capture !== undefined) {
    if (!["dxgi", "wgc"].includes(source.capture)) {
      throw new TypeError('source.capture must be "dxgi" or "wgc"');
    }
    if (source.capture === "dxgi" && source.kind !== "desktop") {
      throw new TypeError("DXGI capture is supported only for desktop sources");
    }
    capture.capture = source.capture;
  }
  switch (source.kind) {
    case "window":
      if (typeof source.hwnd !== "string" || source.hwnd.length === 0) {
        throw new TypeError('window source requires an opaque string "hwnd"');
      }
      return { kind: "window", hwnd: source.hwnd, ...capture };
    case "application":
      if (!Number.isSafeInteger(source.pid) || source.pid < 0) {
        throw new TypeError('application source requires an integer "pid"');
      }
      return { kind: "application", pid: source.pid, ...capture };
    case "desktop":
      return { kind: "desktop", ...capture };
    default:
      throw new TypeError('source.kind must be "window", "application", or "desktop"');
  }
}

function validateVideo(video) {
  if (video === undefined) return undefined;
  if (!video || typeof video !== "object" || Array.isArray(video)) {
    throw new TypeError("video must be an object");
  }
  const result = {};
  if (video.codec !== undefined) {
    if (!["h264", "h265"].includes(video.codec)) throw new TypeError('video.codec must be "h264" or "h265"');
    result.codec = video.codec;
  }
  if (video.frame_rate !== undefined) {
    if (!Number.isInteger(video.frame_rate) || video.frame_rate < 1 || video.frame_rate > 240) {
      throw new TypeError("video.frame_rate must be an integer from 1 to 240");
    }
    result.frame_rate = video.frame_rate;
  }
  if (video.bitrate_kbps !== undefined) {
    if (!Number.isInteger(video.bitrate_kbps) || video.bitrate_kbps < 1 || video.bitrate_kbps > 1_000_000) {
      throw new TypeError("video.bitrate_kbps must be an integer from 1 to 1000000");
    }
    result.bitrate_kbps = video.bitrate_kbps;
  }
  if (video.max_bitrate_kbps !== undefined) {
    if (!Number.isInteger(video.max_bitrate_kbps) || video.max_bitrate_kbps < 0 ||
        video.max_bitrate_kbps > 4_000_000) {
      throw new TypeError("video.max_bitrate_kbps must be an integer from 0 to 4000000");
    }
    result.max_bitrate_kbps = video.max_bitrate_kbps;
  }
  return result;
}

function audioError(message, code = "invalid_audio") {
  return new BeamItError(message, { code });
}

function parsePacketDuration(value) {
  if (typeof value !== "string") throw audioError("Invalid Opus packet duration");
  const match = /^PT(?:(\d+(?:\.\d+)?)H)?(?:(\d+(?:\.\d+)?)M)?(?:(\d+(?:\.\d+)?)S)?$/u.exec(value);
  if (!match || !match.slice(1).some((part) => part !== undefined)) {
    throw audioError("Invalid Opus packet duration");
  }
  const seconds = Number(match[1] ?? 0) * 3600 + Number(match[2] ?? 0) * 60 + Number(match[3] ?? 0);
  if (!Number.isFinite(seconds) || seconds <= 0) throw audioError("Invalid Opus packet duration");
  return seconds;
}

function validateOpusConfiguration(configuration) {
  if (!configuration || typeof configuration !== "object" ||
      !Number.isInteger(configuration.sampleRate) || configuration.sampleRate < 8000 ||
      configuration.sampleRate > 192000 ||
      !Number.isInteger(configuration.channels) || configuration.channels < 1 || configuration.channels > 2 ||
      !Number.isInteger(configuration.frameSize) || configuration.frameSize < 1 ||
      typeof configuration.useFec !== "boolean" ||
      typeof configuration.application !== "string") {
    throw audioError("Invalid negotiated Opus configuration");
  }
  const duration = parsePacketDuration(configuration.packetDuration);
  const actualDuration = configuration.frameSize / configuration.sampleRate;
  const legalDurations = [0.0025, 0.005, 0.01, 0.02, 0.04, 0.06];
  if (Math.abs(duration - actualDuration) > 1e-9 ||
      !legalDurations.some((legal) => Math.abs(duration - legal) < 1e-9)) {
    throw audioError("Unsupported Opus frame size or packet duration");
  }
  if (!["VOIP", "AUDIO", "LOWDELAY"].includes(configuration.application.toUpperCase())) {
    throw audioError("Unsupported Opus application mode");
  }
  if (configuration.useFec) {
    throw audioError("This browser client cannot configure the negotiated Opus FEC mode");
  }
  return {
    sampleRate: configuration.sampleRate,
    channels: configuration.channels,
    frameSize: configuration.frameSize,
    packetDuration: configuration.packetDuration,
    duration,
    useFec: configuration.useFec,
    application: configuration.application,
  };
}

function validateAudioTrack(track, { sourceKind } = {}) {
  if (!track || typeof track !== "object" || !Number.isInteger(track.track_id) ||
      track.track_id < 1 || track.codec !== "opus") {
    throw audioError("Invalid Opus track metadata");
  }
  const configuration = validateOpusConfiguration(track.opusAudioConfiguration);
  if (sourceKind === "application" &&
      (track.source?.kind !== "application" || !Number.isInteger(track.source.pid) || track.source.pid < 1 ||
       track.source.include_process_tree !== true)) {
    throw audioError("Invalid application audio source metadata");
  }
  if (sourceKind === "desktop" &&
      (track.source?.kind !== "desktop" || track.source.exclude_host_process_tree !== true)) {
    throw audioError("Invalid desktop audio source metadata");
  }
  return { ...track, configuration };
}

export function parseAudioPacket(data) {
  let bytes;
  if (data instanceof ArrayBuffer) {
    bytes = new Uint8Array(data);
  } else if (ArrayBuffer.isView(data)) {
    bytes = new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
  } else {
    throw audioError("Audio packet must be an ArrayBuffer or typed-array view");
  }
  if (bytes.byteLength < AUDIO_HEADER_SIZE + 1 || bytes.byteLength > MAX_AUDIO_PACKET_SIZE) {
    throw audioError("Invalid audio packet size");
  }
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (view.getUint8(0) !== AUDIO_VERSION || view.getUint8(1) !== AUDIO_MESSAGE ||
      view.getUint16(2) !== AUDIO_HEADER_SIZE) {
    throw audioError("Invalid version 2 Opus packet header");
  }
  const trackId = view.getUint32(12);
  if (trackId === 0) throw audioError("Audio packet has an invalid track ID");
  return {
    flags: view.getUint32(4),
    discontinuity: (view.getUint32(4) & 2) !== 0,
    frameSize: view.getUint32(8),
    trackId,
    sequence: view.getBigUint64(16),
    sampleTimestamp: view.getBigUint64(24),
    payload: bytes.subarray(AUDIO_HEADER_SIZE),
  };
}

export function createAudioPacket({ trackId, frameSize, sequence, samples, discontinuity = false }) {
  const payload = samples instanceof Uint8Array ? samples : new Uint8Array(samples);
  if (!Number.isInteger(trackId) || trackId < 1 || trackId > 0xffffffff ||
      !Number.isInteger(frameSize) || frameSize < 1 || frameSize > 0xffffffff ||
      typeof sequence !== "bigint" || sequence < 0n ||
      payload.byteLength < 1 || payload.byteLength > 1275) {
    throw new TypeError("Invalid Opus packet fields");
  }
  const packet = new Uint8Array(AUDIO_HEADER_SIZE + payload.byteLength);
  const view = new DataView(packet.buffer);
  view.setUint8(0, AUDIO_VERSION);
  view.setUint8(1, AUDIO_MESSAGE);
  view.setUint16(2, AUDIO_HEADER_SIZE);
  view.setUint32(4, discontinuity ? 2 : 0);
  view.setUint32(8, frameSize);
  view.setUint32(12, trackId);
  view.setBigUint64(16, sequence);
  view.setBigUint64(24, sequence * BigInt(frameSize));
  packet.set(payload, AUDIO_HEADER_SIZE);
  return packet;
}

function validateVideoParameters(video) {
  if (!video || typeof video !== "object" ||
      !Number.isInteger(video.generation) || video.generation < 1 ||
      !["h264", "h265"].includes(video.codec) ||
      typeof video.codec_string !== "string" || video.codec_string.length === 0 ||
      !Number.isInteger(video.width) || video.width < 1 ||
      !Number.isInteger(video.height) || video.height < 1) {
    throw protocolError("Invalid video parameters");
  }
  return video;
}

function validateMessage(message) {
  if (!message || typeof message !== "object" || Array.isArray(message) || typeof message.type !== "string") {
    throw protocolError("Signaling message must be a JSON object with a string type");
  }
}

export function parseVideoFrame(data) {
  let bytes;
  if (data instanceof ArrayBuffer) {
    bytes = new Uint8Array(data);
  } else if (ArrayBuffer.isView(data)) {
    bytes = new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
  } else {
    throw protocolError("Binary frame must be an ArrayBuffer or typed-array view");
  }
  if (bytes.byteLength < 2) throw protocolError("Truncated binary frame");

  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (view.getUint8(0) !== FRAME_VERSION) {
    throw protocolError("Unsupported binary frame version", "unsupported_frame_version");
  }
  if (view.getUint8(1) !== VIDEO_MESSAGE) return null;
  if (bytes.byteLength < FRAME_HEADER_MIN_SIZE) throw protocolError("Truncated video frame header");

  const headerLength = view.getUint16(2);
  if (headerLength < FRAME_HEADER_MIN_SIZE || headerLength > bytes.byteLength) {
    throw protocolError("Invalid video frame header length");
  }
  if (headerLength === bytes.byteLength) throw protocolError("Video frame payload is empty");

  return {
    flags: view.getUint32(4),
    keyFrame: (view.getUint32(4) & 1) !== 0,
    discontinuity: (view.getUint32(4) & 2) !== 0,
    generation: view.getUint32(8),
    sequence: view.getBigUint64(16),
    captureTimeUs: view.getBigUint64(24),
    payload: bytes.subarray(headerLength),
  };
}

export function bindBrowserInput(stream, element, { onError = (error) => console.error(error) } = {}) {
  const document = element.ownerDocument;
  const window = document.defaultView;
  const removers = [];
  const buttons = ["left", "middle", "right", "back", "forward"];
  const heldButtons = new Set();
  const heldKeys = new Set();
  let wheelX = 0;
  let wheelY = 0;
  let disposed = false;
  const focused = () => document.activeElement === element && stream.ready && !disposed;
  const locked = () => document.pointerLockElement === element;
  const run = (operation) => {
    try {
      operation();
    } catch (error) {
      onError(error);
    }
  };
  const listen = (target, type, listener, options) => {
    target.addEventListener(type, listener, options);
    removers.push(() => target.removeEventListener(type, listener, options));
  };
  const release = () => {
    heldButtons.clear();
    heldKeys.clear();
    wheelX = 0;
    wheelY = 0;
    if (stream.ready && !disposed) run(() => stream.releaseAll());
  };
  const move = (event) => {
    if (stream.mouseMode === "relative") {
      if (!locked()) return false;
      stream.mouseMove({ dx: event.movementX, dy: event.movementY });
    } else {
      const rectangle = element.getBoundingClientRect();
      if (rectangle.width <= 0 || rectangle.height <= 0) return false;
      stream.mouseMove({
        x: Math.max(0, Math.min(stream.video.width - 1, (event.clientX - rectangle.left) * stream.video.width / rectangle.width)),
        y: Math.max(0, Math.min(stream.video.height - 1, (event.clientY - rectangle.top) * stream.video.height / rectangle.height)),
      });
    }
    return true;
  };

  listen(element, "mousemove", (event) => {
    if (focused()) run(() => move(event));
  });
  listen(element, "mousedown", (event) => {
    if (!stream.ready || !buttons[event.button]) return;
    event.preventDefault();
    element.focus({ preventScroll: true });
    if (stream.mouseMode === "relative" && !locked()) {
      if (typeof element.requestPointerLock !== "function") {
        onError(new BeamItError("Pointer lock is unavailable", { code: "pointer_lock_unavailable" }));
      } else {
        run(() => {
          const result = element.requestPointerLock();
          result?.catch(onError);
        });
      }
      return;
    }
    run(() => {
      if (!move(event)) return;
      stream.mouseButton(buttons[event.button], true);
      heldButtons.add(event.button);
    });
  });
  listen(window, "mouseup", (event) => {
    if (!heldButtons.delete(event.button)) return;
    event.preventDefault();
    run(() => {
      if (focused()) move(event);
      stream.mouseButton(buttons[event.button], false);
    });
  });
  listen(element, "contextmenu", (event) => {
    if (focused()) event.preventDefault();
  });
  listen(element, "auxclick", (event) => {
    if (focused()) event.preventDefault();
  });
  listen(element, "wheel", (event) => {
    if (!focused() || (stream.mouseMode === "relative" && !locked())) return;
    event.preventDefault();
    run(() => {
      if (!move(event)) return;
      // Preserve fractional pixel-wheel motion across events; line mode is one notch.
      const scale = event.deltaMode === 1 ? 120 : event.deltaMode === 2 ? stream.video.height * 1.2 : 1.2;
      wheelX += event.deltaX * scale;
      wheelY += event.deltaY * scale;
      const dx = Math.trunc(wheelX);
      const dy = Math.trunc(wheelY);
      wheelX -= dx;
      wheelY -= dy;
      let remainingX = dx;
      let remainingY = dy;
      while (remainingX !== 0 || remainingY !== 0) {
        const x = Math.max(-12000, Math.min(12000, remainingX));
        const y = Math.max(-12000, Math.min(12000, remainingY));
        stream.mouseWheel({ dx: x, dy: y });
        remainingX -= x;
        remainingY -= y;
      }
    });
  }, { passive: false });
  listen(element, "keydown", (event) => {
    if (!focused() || event.isComposing || event.keyCode === 229) return;
    if (event.code === "Escape") {
      event.preventDefault();
      release();
      if (locked()) document.exitPointerLock();
      element.blur();
      return;
    }
    event.preventDefault();
    run(() => {
      stream.key(event.code, true);
      heldKeys.add(event.code);
    });
  });
  listen(window, "keyup", (event) => {
    if (!heldKeys.delete(event.code)) return;
    event.preventDefault();
    run(() => stream.key(event.code, false));
  });
  listen(element, "compositionstart", release);
  listen(element, "compositionend", (event) => {
    if (focused() && event.data) run(() => stream.text(event.data));
  });
  listen(element, "blur", release);
  listen(window, "blur", release);
  listen(document, "visibilitychange", () => {
    if (document.hidden) release();
  });
  listen(document, "pointerlockchange", () => {
    if (!locked()) {
      release();
      element.blur();
    }
  });
  listen(document, "pointerlockerror", () => {
    onError(new BeamItError("The browser denied pointer lock", { code: "pointer_lock_failed" }));
  });

  const dispose = () => {
    if (disposed) return;
    release();
    disposed = true;
    if (locked()) document.exitPointerLock();
    for (const remove of removers) remove();
  };
  removers.push(stream.on("close", dispose));
  return { release, dispose };
}

export async function createWebCodecsDecoder(stream, {
  output,
  onError = (error) => console.error(error),
  VideoDecoderImpl = globalThis.VideoDecoder,
  EncodedVideoChunkImpl = globalThis.EncodedVideoChunk,
} = {}) {
  if (typeof output !== "function") throw new TypeError("output callback is required");
  if (typeof VideoDecoderImpl !== "function" || typeof EncodedVideoChunkImpl !== "function") {
    throw new BeamItError("WebCodecs is unavailable in this browser", { code: "webcodecs_unavailable" });
  }

  const decoder = new VideoDecoderImpl({
    output,
    error: (error) => {
      onError(error);
      void stream.requestKeyFrame().catch(onError);
    },
  });
  let waitingForKeyFrame = true;
  const configure = async (video) => {
    const config = {
      codec: video.codec_string,
      codedWidth: video.width,
      codedHeight: video.height,
    };
    const support = await VideoDecoderImpl.isConfigSupported(config);
    if (!support.supported) {
      throw new BeamItError(`WebCodecs does not support ${video.codec_string}`, { code: "unsupported_video_config" });
    }
    decoder.configure(config);
  };
  const initialConfiguration = configure(stream.video);
  let operation = initialConfiguration;
  const removeConfigListener = stream.on("config", (video) => {
    operation = operation.then(async () => {
      await configure(video);
      waitingForKeyFrame = true;
      await stream.requestKeyFrame();
    }).catch(onError);
  });
  const removeFrameListener = stream.on("frame", (frame) => {
    operation = operation.then(() => {
      if (waitingForKeyFrame && !frame.keyFrame) return;
      if (frame.keyFrame) waitingForKeyFrame = false;
      const timestamp = Number(frame.captureTimeUs);
      if (!Number.isSafeInteger(timestamp)) {
        throw new BeamItError("Frame timestamp exceeds WebCodecs precision", { code: "invalid_timestamp" });
      }
      decoder.decode(new EncodedVideoChunkImpl({
        type: frame.keyFrame ? "key" : "delta",
        timestamp,
        data: frame.payload,
      }));
    }).catch(onError);
  });

  try {
    await initialConfiguration;
    await stream.requestKeyFrame();
  } catch (error) {
    removeConfigListener();
    removeFrameListener();
    decoder.close();
    throw error;
  }

  return {
    decoder,
    close() {
      removeConfigListener();
      removeFrameListener();
      if (decoder.state !== "closed") decoder.close();
    },
  };
}

export class BeamItAudio extends EventHub {
  #client;
  #WebSocket;
  #controlUrl;
  #socket = null;
  #opened = false;
  #closed = false;
  #tracks = new Map();
  #pendingSources = new Map();
  #AudioContext;
  #AudioDecoder;
  #EncodedAudioChunk;
  #AudioEncoder;
  #AudioData;
  #AudioWorkletNode;
  #mediaDevices;
  #context = null;
  #microphone = null;
  #source = null;
  #worklet = null;
  #encoder = null;
  #encoderPackets = new Map();
  #encoderDiscontinuity = true;
  #lastSentSequence = null;
  #messageQueue = Promise.resolve();
  #queuedPackets = 0;

  constructor(client, offer, WebSocketImpl, controlUrl, options = {}) {
    super();
    this.#client = client;
    this.#WebSocket = WebSocketImpl;
    this.#controlUrl = controlUrl;
    this.#AudioContext = options.AudioContext ?? globalThis.AudioContext;
    this.#AudioDecoder = options.AudioDecoder ?? globalThis.AudioDecoder;
    this.#EncodedAudioChunk = options.EncodedAudioChunk ?? globalThis.EncodedAudioChunk;
    this.#AudioEncoder = options.AudioEncoder ?? globalThis.AudioEncoder;
    this.#AudioData = options.AudioData ?? globalThis.AudioData;
    this.#AudioWorkletNode = options.AudioWorkletNode ?? globalThis.AudioWorkletNode;
    this.#mediaDevices = options.mediaDevices ?? globalThis.navigator?.mediaDevices;
    this.#context = options.audioContext ?? null;
    this.audioId = offer.audio_id;
    this.offer = offer;
    this.clientToHost = validateAudioTrack({
      track_id: offer.clientToHost?.track_id,
      source: offer.clientToHost?.source,
      codec: offer.clientToHost?.codec,
      opusAudioConfiguration: offer.clientToHost?.opusAudioConfiguration,
    }, { sourceKind: "microphone" });
    if (this.clientToHost.source?.kind !== "microphone") {
      throw audioError("Invalid negotiated microphone route");
    }
    if (!Array.isArray(offer.hostToClient?.tracks)) {
      throw audioError("Invalid host-to-client audio tracks");
    }
    if (typeof this.#AudioContext !== "function" || typeof this.#AudioDecoder !== "function" ||
        typeof this.#EncodedAudioChunk !== "function" || typeof this.#AudioEncoder !== "function" ||
        typeof this.#AudioData !== "function" || typeof this.#mediaDevices?.getUserMedia !== "function") {
      throw new BeamItError("Web Audio, WebCodecs Opus, and microphone capture are required for audio", {
        code: "audio_capability_unavailable",
      });
    }
  }

  get ready() {
    return !this.#closed && this.#socket?.readyState === 1;
  }

  get tracks() {
    return [...this.#tracks.values()].map(({ metadata }) => metadata);
  }

  async open() {
    const audioUrl = new URL(this.offer.url);
    const token = audioUrl.searchParams.get("token");
    if (audioUrl.protocol !== "wss:" || audioUrl.host !== this.#controlUrl.host ||
        audioUrl.pathname !== `/audio/${encodeURIComponent(this.audioId)}` ||
        audioUrl.username || audioUrl.password || audioUrl.hash || audioUrl.searchParams.size !== 1 ||
        !token || base64UrlDecode(token).byteLength !== 32 ||
        this.offer.audio_version !== AUDIO_VERSION ||
        !Number.isFinite(this.offer.token_expires_in_ms) || this.offer.token_expires_in_ms <= 0) {
      throw audioError("Audio offer does not match the authenticated WSS host", "invalid_audio_offer");
    }
    for (const track of this.offer.hostToClient.tracks) {
      await this.#installTrack(validateAudioTrack(track));
    }
    return new Promise((resolve, reject) => {
      try {
        this.#socket = new this.#WebSocket(audioUrl.href);
      } catch (cause) {
        reject(new BeamItError("Could not create the audio WebSocket", { cause }));
        return;
      }
      this.#socket.binaryType = "arraybuffer";
      const socket = this.#socket;
      socket.addEventListener("open", () => {
        if (this.#closed) return;
        this.#opened = true;
        this.emit("open", this.offer);
        resolve(this);
      }, { once: true });
      socket.addEventListener("message", (event) => {
        if (this.#queuedPackets >= MAX_AUDIO_QUEUE) {
          this.#failAudio(audioError("Incoming audio socket queue exceeded three packets", "audio_overloaded"), 1011);
          return;
        }
        this.#queuedPackets += 1;
        this.#messageQueue = this.#messageQueue.then(() => this.#handlePacket(event.data)).catch((error) => {
          this.#failAudio(error, 1007);
        }).finally(() => { this.#queuedPackets -= 1; });
      });
      socket.addEventListener("close", (event) => {
        if (this.#socket !== socket) return;
        const unopened = !this.#opened;
        this.close(event.code, event.reason || "audio socket closed", false);
        if (unopened) reject(new BeamItError(`Audio socket closed before opening (${event.code})`, {
          code: "audio_open_failed",
        }));
        this.emit("close", { code: event.code, reason: event.reason });
      });
      socket.addEventListener("error", () => this.emit("socketError", { socket: "audio", audioId: this.audioId }));
    });
  }

  async startTrack(source) {
    if (!this.ready) throw new BeamItError("Audio socket is not open", { code: "not_ready" });
    const requestSource = validateAudioSource(source);
    if (this.#tracks.size + this.#pendingSources.size >= 8) {
      throw new TypeError("At most eight audio capture tracks can be selected");
    }
    const existing = [
      ...[...this.#tracks.values()].map(({ metadata }) => metadata.source),
      ...this.#pendingSources.values(),
    ];
    if (requestSource.kind === "desktop" && existing.length > 0 ||
        requestSource.kind === "application" && existing.some((item) => item?.kind === "desktop") ||
        requestSource.kind === "application" && existing.some((item) =>
          item?.kind === "application" && item.pid === requestSource.pid)) {
      throw new TypeError("Desktop audio is exclusive and application PIDs cannot be duplicated");
    }
    const key = requestSource.kind === "desktop" ? "desktop" : `application:${requestSource.pid}`;
    this.#pendingSources.set(key, requestSource);
    let failedTrackId;
    try {
      const response = await this.#client._startAudioTrack(this.audioId, requestSource);
      if (response.audio_id !== this.audioId) throw audioError("Audio track reply belongs to another session");
      if (response.type === "audio_track_stopped") {
        throw new BeamItError(response.message ?? `Audio capture failed: ${response.reason ?? "unknown"}`, {
          code: response.reason ?? "capture_failed",
          id: response.id,
        });
      }
      if (Number.isInteger(response.track?.track_id) && response.track.track_id > 0 &&
          !this.#tracks.has(response.track.track_id)) failedTrackId = response.track.track_id;
      const track = validateAudioTrack(response.track, { sourceKind: requestSource.kind });
      if (requestSource.kind === "application" && track.source.pid !== requestSource.pid) {
        throw audioError("Audio track reply changed the requested application PID");
      }
      if ([...this.#tracks.keys()].includes(track.track_id)) throw audioError("Audio track ID was reused");
      await this.#installTrack(track);
      failedTrackId = undefined;
      this.emit("trackStarted", track);
      return track;
    } catch (error) {
      if (failedTrackId !== undefined) {
        void this.#client._stopAudioTrack(this.audioId, failedTrackId).catch(() => {});
      }
      throw error;
    } finally {
      this.#pendingSources.delete(key);
    }
  }

  async stopTrack(trackId) {
    if (!this.#tracks.has(trackId)) throw new BeamItError("Audio track is not active", { code: "not_found" });
    const response = await this.#client._stopAudioTrack(this.audioId, trackId);
    if (response.audio_id !== this.audioId || response.track_id !== trackId) {
      throw audioError("Audio track stop reply does not match the request");
    }
    this.#removeTrack(trackId);
    this.emit("trackStopped", response);
    return response;
  }

  async startMicrophone() {
    if (!this.ready) throw new BeamItError("Audio socket is not open", { code: "not_ready" });
    if (this.#encoder) throw new BeamItError("Microphone capture is already running");
    const configuration = this.clientToHost.configuration;
    if (configuration.application.toUpperCase() !== "VOIP") {
      throw new BeamItError("This browser encoder supports only the negotiated VOIP Opus application", {
        code: "audio_config_unsupported",
      });
    }
    const codecConfig = {
      codec: "opus",
      sampleRate: configuration.sampleRate,
      numberOfChannels: configuration.channels,
    };
    if (typeof this.#AudioEncoder.isConfigSupported !== "function") {
      throw new BeamItError("AudioEncoder.isConfigSupported is required for safe Opus setup", {
        code: "audio_capability_unavailable",
      });
    }
    const support = await this.#AudioEncoder.isConfigSupported(codecConfig);
    if (this.#closed) throw new BeamItError("Audio session ended during microphone setup", { code: "audio_stopped" });
    if (!support?.supported) {
      throw new BeamItError("The browser cannot encode the negotiated Opus configuration", {
        code: "audio_config_unsupported",
      });
    }
    try {
      if (this.#context && this.#context.sampleRate !== configuration.sampleRate) {
        const previousContext = this.#context;
        this.#context = null;
        if (previousContext.state !== "closed") await previousContext.close();
      }
      this.#context ??= new this.#AudioContext({
        sampleRate: configuration.sampleRate,
        latencyHint: "interactive",
      });
      if (this.#context.sampleRate !== configuration.sampleRate) {
        throw new BeamItError("Web Audio cannot provide the negotiated microphone sample rate", {
          code: "audio_config_unsupported",
        });
      }
      await this.#context.resume();
      if (this.#closed) throw new BeamItError("Audio session ended during microphone setup", { code: "audio_stopped" });
      if (!this.#context.audioWorklet?.addModule || typeof this.#AudioWorkletNode !== "function") {
        throw new BeamItError("AudioWorklet microphone capture is unavailable", {
          code: "audio_capability_unavailable",
        });
      }
      await this.#context.audioWorklet.addModule(new URL("./beamit.js", import.meta.url));
      if (this.#closed) throw new BeamItError("Audio session ended during microphone setup", { code: "audio_stopped" });
      const microphone = await this.#mediaDevices.getUserMedia({
        audio: { channelCount: { ideal: configuration.channels }, sampleRate: { ideal: configuration.sampleRate } },
      });
      if (this.#closed) {
        for (const track of microphone.getTracks?.() ?? []) track.stop();
        throw new BeamItError("Audio session ended during microphone setup", { code: "audio_stopped" });
      }
      this.#microphone = microphone;
      const encoderInstance = new this.#AudioEncoder({
        output: (chunk) => this.#handleEncodedChunk(chunk),
        error: (error) => this.#microphoneFailed(error),
      });
      encoderInstance.configure(codecConfig);
      this.#encoder = encoderInstance;
      this.#source = this.#context.createMediaStreamSource(microphone);
      this.#worklet = new this.#AudioWorkletNode(this.#context, "beamit-microphone", {
        numberOfInputs: 1,
        numberOfOutputs: 1,
        outputChannelCount: [1],
        channelCount: configuration.channels,
        channelCountMode: "explicit",
        processorOptions: {
          frameSize: configuration.frameSize,
          channels: configuration.channels,
        },
      });
      this.#worklet.port.onmessage = (event) => this.#handleMicrophoneFrame(event.data);
      this.#source.connect(this.#worklet);
      this.#worklet.connect(this.#context.destination);
      this.emit("microphoneStarted", this.clientToHost);
      return this.clientToHost;
    } catch (error) {
      this.#stopMicrophone();
      throw error;
    }
  }

  async stop() {
    if (this.#closed) return;
    return this.#client.stopAudio(this.audioId);
  }

  handleTrackStopped(message) {
    if (message.audio_id !== this.audioId || !Number.isInteger(message.track_id)) return;
    this.#removeTrack(message.track_id);
    this.emit("trackStopped", message);
  }

  handleStopped(message) {
    if (this.#closed) return;
    this.emit("stopped", message);
    this.close(1000, "audio stopped");
  }

  controlClosed(error) {
    this.emit("error", error);
    this.close(1001, "control connection closed");
  }

  close(code = 1000, reason = "audio closed", closeSocket = true) {
    if (this.#closed) return;
    this.#closed = true;
    for (const trackId of this.#tracks.keys()) this.#removeTrack(trackId);
    this.#stopMicrophone();
    if (closeSocket && this.#socket && this.#socket.readyState < 2) {
      this.#socket.close(code, reason.slice(0, 120));
    }
    const context = this.#context;
    this.#context = null;
    if (context && context.state !== "closed") void context.close().catch(() => {});
    this.#client._removeAudio(this.audioId, this);
  }

  async #installTrack(metadata) {
    if (this.#tracks.has(metadata.track_id)) throw audioError("Duplicate audio track ID");
    const AudioDecoderImpl = this.#AudioDecoder;
    const configuration = metadata.configuration ?? validateOpusConfiguration(metadata.opusAudioConfiguration);
    const decoderConfig = {
      codec: "opus",
      sampleRate: configuration.sampleRate,
      numberOfChannels: configuration.channels,
    };
    if (typeof AudioDecoderImpl.isConfigSupported !== "function" ||
        !(await AudioDecoderImpl.isConfigSupported(decoderConfig))?.supported) {
      throw new BeamItError("The browser cannot decode the negotiated Opus configuration", {
        code: "audio_config_unsupported",
      });
    }
    const trackState = {
      metadata,
      decoder: null,
      lastSequence: null,
      nextPlaybackTime: 0,
      sources: new Set(),
    };
    const decoder = new AudioDecoderImpl({
      output: (audioData) => this.#playDecoded(trackState, audioData),
      error: (error) => this.#microphoneFailed(error),
    });
    try {
      decoder.configure(decoderConfig);
    } catch (cause) {
      decoder.close();
      throw new BeamItError("Could not configure the negotiated Opus decoder", {
        code: "audio_config_unsupported", cause,
      });
    }
    trackState.decoder = decoder;
    this.#tracks.set(metadata.track_id, trackState);
  }

  async #handlePacket(data) {
    if (this.#closed) return;
    if (typeof data === "string") throw audioError("Audio socket accepts binary packets only");
    const packet = parseAudioPacket(data instanceof Blob ? await data.arrayBuffer() : data);
    const track = this.#tracks.get(packet.trackId);
    if (!track) return;
    const configuration = track.metadata.configuration;
    if (packet.frameSize !== configuration.frameSize ||
        packet.sampleTimestamp !== packet.sequence * BigInt(configuration.frameSize)) {
      throw audioError("Audio packet timing does not match negotiated Opus metadata");
    }
    const previous = track.lastSequence;
    if (previous !== null && packet.sequence <= previous) {
      throw audioError("Audio packet sequence is duplicate or reversed");
    }
    const gap = previous === null ? packet.sequence !== 0n : packet.sequence !== previous + 1n;
    if ((gap || previous === null) && !packet.discontinuity) {
      throw audioError("Audio packet gap is missing its discontinuity flag");
    }
    if (packet.discontinuity) {
      track.decoder.reset();
      track.decoder.configure({
        codec: "opus",
        sampleRate: configuration.sampleRate,
        numberOfChannels: configuration.channels,
      });
      track.nextPlaybackTime = 0;
      for (const source of track.sources) {
        try { source.stop(); } catch {}
      }
      track.sources.clear();
    }
    track.lastSequence = packet.sequence;
    if (track.decoder.decodeQueueSize >= MAX_AUDIO_QUEUE) {
      throw audioError("Incoming Opus decoder queue exceeded three packets", "audio_overloaded");
    }
    const chunk = new this.#EncodedAudioChunk({
      type: "key",
      timestamp: Number(packet.sampleTimestamp * 1_000_000n / BigInt(configuration.sampleRate)),
      duration: Math.round(configuration.duration * 1_000_000),
      data: packet.payload,
    });
    track.decoder.decode(chunk);
    this.emit("packet", { trackId: packet.trackId, sequence: packet.sequence, discontinuity: packet.discontinuity });
  }

  #playDecoded(track, audioData) {
    if (this.#closed || !this.#context || !this.#tracks.has(track.metadata.track_id)) {
      audioData.close();
      return;
    }
    try {
      const configuration = track.metadata.configuration;
      const duration = audioData.numberOfFrames / configuration.sampleRate;
      if (audioData.sampleRate !== configuration.sampleRate ||
          audioData.numberOfChannels !== configuration.channels ||
          audioData.numberOfFrames !== configuration.frameSize) {
        throw audioError("Decoded Opus frame does not match negotiated audio metadata");
      }
      if (track.sources.size >= MAX_AUDIO_QUEUE ||
          track.nextPlaybackTime - this.#context.currentTime > duration * MAX_AUDIO_QUEUE + 0.015) {
        throw audioError("Incoming decoded audio queue exceeded three packets", "audio_overloaded");
      }
      const buffer = this.#context.createBuffer(
        configuration.channels, audioData.numberOfFrames, configuration.sampleRate,
      );
      for (let channel = 0; channel < configuration.channels; channel += 1) {
        audioData.copyTo(buffer.getChannelData(channel), { planeIndex: channel, format: "f32-planar" });
      }
      const source = this.#context.createBufferSource();
      source.buffer = buffer;
      source.connect(this.#context.destination);
      source.onended = () => track.sources.delete(source);
      const startAt = Math.max(this.#context.currentTime + 0.015, track.nextPlaybackTime);
      track.nextPlaybackTime = startAt + duration;
      track.sources.add(source);
      source.start(startAt);
    } catch (error) {
      this.#microphoneFailed(error);
    } finally {
      audioData.close();
    }
  }

  #handleMicrophoneFrame(message) {
    const configuration = this.clientToHost.configuration;
    const { sequence, samples } = message ?? {};
    try {
      if (!Number.isSafeInteger(sequence) || !(samples instanceof Float32Array) ||
          samples.length !== configuration.frameSize * configuration.channels) {
        throw audioError("AudioWorklet returned an invalid PCM frame");
      }
      if (this.#encoder.encodeQueueSize >= MAX_AUDIO_QUEUE ||
          this.#encoderPackets.size >= MAX_AUDIO_QUEUE ||
          this.#socket.bufferedAmount > MAX_AUDIO_QUEUE * MAX_AUDIO_PACKET_SIZE) {
        this.#encoderDiscontinuity = true;
        return;
      }
      const timestamp = Number(BigInt(sequence) * BigInt(configuration.frameSize) *
        1_000_000n / BigInt(configuration.sampleRate));
      const frame = new this.#AudioData({
        format: "f32",
        sampleRate: configuration.sampleRate,
        numberOfFrames: configuration.frameSize,
        numberOfChannels: configuration.channels,
        timestamp,
        data: samples,
      });
      this.#encoderPackets.set(timestamp, {
        sequence: BigInt(sequence),
        discontinuity: this.#encoderDiscontinuity ||
          this.#lastSentSequence === null || BigInt(sequence) !== this.#lastSentSequence + 1n,
      });
      this.#encoderDiscontinuity = false;
      this.#encoder.encode(frame);
      frame.close();
    } catch (error) {
      this.#microphoneFailed(error);
    } finally {
      this.#worklet?.port.postMessage({ type: "consumed" });
    }
  }

  #handleEncodedChunk(chunk) {
    try {
      const configuration = this.clientToHost.configuration;
      const metadata = this.#encoderPackets.get(chunk.timestamp);
      if (!metadata) throw audioError("Opus encoder returned an unmatched timestamp");
      this.#encoderPackets.delete(chunk.timestamp);
      if (chunk.duration !== Math.round(configuration.duration * 1_000_000)) {
        throw audioError("Browser Opus encoder did not honor the negotiated packet duration", "audio_config_unsupported");
      }
      if (!Number.isInteger(chunk.byteLength) || chunk.byteLength < 1 || chunk.byteLength > 1275) {
        throw audioError("Browser Opus encoder returned an invalid packet size");
      }
      if (this.#socket?.readyState !== 1 ||
          this.#socket.bufferedAmount > MAX_AUDIO_QUEUE * MAX_AUDIO_PACKET_SIZE) {
        this.#encoderDiscontinuity = true;
        return;
      }
      const payload = new Uint8Array(chunk.byteLength);
      chunk.copyTo(payload);
      const packet = createAudioPacket({
        trackId: this.clientToHost.track_id,
        frameSize: configuration.frameSize,
        sequence: metadata.sequence,
        samples: payload,
        discontinuity: metadata.discontinuity,
      });
      this.#socket.send(packet);
      this.#lastSentSequence = metadata.sequence;
    } catch (error) {
      this.#microphoneFailed(error);
    }
  }

  #microphoneFailed(error) {
    if (this.#closed) return;
    const failure = error instanceof Error ? error : new BeamItError(String(error));
    const stopping = this.ready ? this.stop() : Promise.resolve();
    this.close(1011, "audio processing failed");
    this.emit("error", failure);
    void stopping.catch(() => {});
  }

  #failAudio(error, code) {
    if (this.#closed) return;
    const stopping = this.ready ? this.stop() : Promise.resolve();
    this.close(code, "audio protocol failure");
    this.emit("error", error);
    void stopping.catch(() => {});
  }

  #stopMicrophone() {
    this.#source?.disconnect();
    this.#worklet?.disconnect();
    if (this.#worklet?.port) this.#worklet.port.onmessage = null;
    if (this.#encoder && this.#encoder.state !== "closed") this.#encoder.close();
    this.#encoder = null;
    this.#encoderPackets.clear();
    for (const track of this.#microphone?.getTracks?.() ?? []) track.stop();
    this.#microphone = null;
    this.#source = null;
    this.#worklet = null;
  }

  #removeTrack(trackId) {
    const track = this.#tracks.get(trackId);
    if (!track) return;
    this.#tracks.delete(trackId);
    for (const source of track.sources) {
      try { source.stop(); } catch {}
    }
    track.sources.clear();
    if (track.decoder.state !== "closed") track.decoder.close();
  }
}

function validateAudioSource(source) {
  if (!source || typeof source !== "object" || Array.isArray(source)) {
    throw new TypeError("Audio source is required");
  }
  if (source.kind === "desktop") return { kind: "desktop" };
  if (source.kind === "application" && Number.isInteger(source.pid) &&
      source.pid > 0 && source.pid <= 0xffffffff) {
    return { kind: "application", pid: source.pid };
  }
  throw new TypeError('Audio source must be { kind: "desktop" } or { kind: "application", pid }');
}

export class BeamItClient extends EventHub {
  #WebSocket;
  #crypto;
  #requestTimeoutMs;
  #control = null;
  #controlUrl = null;
  #state = "disconnected";
  #pending = new Map();
  #streams = new Map();
  #audios = new Map();
  #audioStarting = false;
  #nextId = 1;
  #messageQueue = Promise.resolve();
  #connectResolve = null;
  #connectReject = null;
  #expectedServerProof = null;
  #password = null;

  constructor({ url, WebSocket: WebSocketImpl = globalThis.WebSocket, crypto: cryptoApi = globalThis.crypto, requestTimeoutMs = 10_000 } = {}) {
    super();
    if (typeof WebSocketImpl !== "function") throw new TypeError("A WebSocket implementation is required");
    if (!Number.isSafeInteger(requestTimeoutMs) || requestTimeoutMs < 1) {
      throw new TypeError("requestTimeoutMs must be a positive integer");
    }
    this.#WebSocket = WebSocketImpl;
    this.#crypto = cryptoApi;
    this.#requestTimeoutMs = requestTimeoutMs;
    this.url = url;
  }

  get connected() {
    return this.#state === "authenticated";
  }

  async connect(password) {
    if (this.#state !== "disconnected") throw new BeamItError("Client is already connecting or connected");
    if (typeof password !== "string") throw new TypeError("password must be a string");
    const controlUrl = new URL(this.url ?? globalThis.location?.href ?? "");
    if (controlUrl.protocol !== "https:" && controlUrl.protocol !== "wss:") {
      throw new BeamItError("BeamIt requires HTTPS/WSS; plaintext connections are not supported", { code: "insecure_transport" });
    }
    if (controlUrl.username || controlUrl.password) {
      throw new BeamItError("Credentials must not be embedded in the WebSocket URL", { code: "invalid_url" });
    }
    controlUrl.protocol = "wss:";
    controlUrl.pathname = "/control";
    controlUrl.search = "";
    controlUrl.hash = "";
    this.#controlUrl = controlUrl;
    this.#state = "connecting";
    this.#password = password;

    return new Promise((resolve, reject) => {
      this.#connectResolve = resolve;
      this.#connectReject = reject;
      try {
        this.#control = new this.#WebSocket(controlUrl.href);
      } catch (cause) {
        this.#state = "disconnected";
        this.#connectResolve = null;
        this.#connectReject = null;
        this.#password = null;
        reject(new BeamItError("Could not create the control WebSocket", { cause }));
        return;
      }
      this.#control.binaryType = "arraybuffer";
      const controlSocket = this.#control;
      this.#control.addEventListener("message", (event) => {
        this.#messageQueue = this.#messageQueue
          .then(() => this.#handleControlMessage(event.data))
          .catch((error) => this.#fail(error));
      });
      this.#control.addEventListener("close", (event) => this.#handleControlClose(event, controlSocket));
      this.#control.addEventListener("error", () => this.emit("socketError", { socket: "control" }));
    });
  }

  async ping() {
    return this.#request("ping", {}, "pong");
  }

  async listWindows() {
    const response = await this.#request("list_windows", {}, "windows");
    if (!Array.isArray(response.windows)) throw protocolError("Invalid windows response");
    return response.windows;
  }

  async startStream({ source, video, input } = {}) {
    const request = { source: validateSource(source) };
    const videoOptions = validateVideo(video);
    if (videoOptions && Object.keys(videoOptions).length > 0) request.video = videoOptions;
    if (input !== undefined) {
      if (!input || typeof input !== "object" || Array.isArray(input)) throw new TypeError("input must be an object");
      request.input = {};
      if (input.mouse_mode !== undefined) request.input.mouse_mode = validateMouseMode(input.mouse_mode);
    }
    const offer = await this.#request("start_stream", request, "stream_offer");
    if (typeof offer.stream_id !== "string" || typeof offer.url !== "string" ||
        !Number.isFinite(offer.token_expires_in_ms) || offer.token_expires_in_ms <= 0) {
      throw protocolError("Invalid stream offer");
    }
    validateVideoParameters(offer.video);

    const stream = new BeamItStream(this, offer, this.#WebSocket, this.#controlUrl);
    this.#streams.set(offer.stream_id, stream);
    try {
      await stream.open();
      return stream;
    } catch (error) {
      this.#streams.delete(offer.stream_id);
      throw error;
    }
  }

  async stopStream(streamId) {
    const response = await this.#request("stop_stream", { stream_id: streamId }, "stream_stopped");
    this.#streams.get(streamId)?.handleStopped(response);
    return response;
  }

  async startAudio(options = {}) {
    this.#assertConnected();
    if (this.#audioStarting || this.#audios.size > 0) {
      throw new BeamItError("An audio session is already starting or active", { code: "limit_reached" });
    }
    this.#audioStarting = true;
    let offer;
    try {
      offer = await this.#request("start_audio", {}, "audio_offer");
      if (typeof offer.audio_id !== "string" || offer.audio_id.length === 0 ||
          typeof offer.url !== "string") {
        throw audioError("Invalid audio offer");
      }
      const audio = new BeamItAudio(this, offer, this.#WebSocket, this.#controlUrl, options);
      this.#audios.set(offer.audio_id, audio);
      try {
        await audio.open();
        return audio;
      } catch (error) {
        audio.close(1002, "audio setup failed");
        throw error;
      }
    } catch (error) {
      if (offer?.audio_id && this.connected) {
        void this.#request("stop_audio", { audio_id: offer.audio_id }, "audio_stopped").catch(() => {});
      }
      throw error;
    } finally {
      this.#audioStarting = false;
    }
  }

  async stopAudio(audioId) {
    const audio = this.#audioSession(audioId);
    const response = await this.#request("stop_audio", { audio_id: audioId }, "audio_stopped");
    if (response.audio_id !== audioId) throw audioError("Audio stop reply does not match the request");
    audio.handleStopped(response);
    return response;
  }

  _startAudioTrack(audioId, source) {
    return this.#request("start_audio_track", { audio_id: audioId, source },
      ["audio_track_started", "audio_track_stopped"]);
  }

  _stopAudioTrack(audioId, trackId) {
    return this.#request("stop_audio_track", { audio_id: audioId, track_id: trackId },
      "audio_track_stopped");
  }

  async requestKeyFrame(streamId) {
    this.#assertConnected();
    this.#send({ type: "request_key_frame", stream_id: streamId });
  }

  async setMouseMode(streamId, mode) {
    const stream = this.#inputStream(streamId);
    const response = await this.#request("set_mouse_mode", {
      stream_id: streamId, mode: validateMouseMode(mode),
    }, "mouse_mode");
    if (response.stream_id !== streamId || response.mode !== mode) throw protocolError("Invalid mouse_mode response");
    stream._updateMouseMode(response.mode);
    return response;
  }

  async getInputState(streamId) {
    const stream = this.#inputStream(streamId, false);
    const response = await this.#request("get_input_state", { stream_id: streamId }, "input_state");
    if (response.stream_id !== streamId || !Number.isInteger(response.x) || !Number.isInteger(response.y) ||
        !["absolute", "relative"].includes(response.mouse_mode) || !response.modifiers ||
        !["shift", "control", "alt", "meta", "caps_lock", "num_lock", "scroll_lock"]
          .every((key) => typeof response.modifiers[key] === "boolean")) {
      throw protocolError("Invalid input_state response");
    }
    stream._updateMouseMode(response.mouse_mode);
    return response;
  }

  mouseMove(streamId, position) {
    const stream = this.#inputStream(streamId);
    const fields = stream.mouseMode === "relative" ? ["dx", "dy"] : ["x", "y"];
    const other = stream.mouseMode === "relative" ? ["x", "y"] : ["dx", "dy"];
    if (!position || !fields.every((field) => Number.isFinite(position[field])) ||
        other.some((field) => position[field] !== undefined)) {
      throw new TypeError(`mouse_move requires finite ${fields.join("/")} in ${stream.mouseMode} mode`);
    }
    this.#sendInput("mouse_move", streamId, Object.fromEntries(fields.map((field) => [field, position[field]])));
  }

  mouseButton(streamId, button, down) {
    if (!["left", "right", "middle", "back", "forward"].includes(button) || typeof down !== "boolean") {
      throw new TypeError("mouse_button requires a supported button and boolean down");
    }
    this.#sendInput("mouse_button", streamId, { button, down });
  }

  mouseWheel(streamId, { dx = 0, dy = 0 } = {}) {
    if (![dx, dy].every((value) => Number.isInteger(value) && Math.abs(value) <= 12000)) {
      throw new TypeError("mouse_wheel requires integer deltas within +/-12000");
    }
    this.#sendInput("mouse_wheel", streamId, { dx, dy });
  }

  key(streamId, code, down) {
    if (typeof code !== "string" || code.length === 0 || typeof down !== "boolean") {
      throw new TypeError("key requires a physical KeyboardEvent.code and boolean down");
    }
    this.#sendInput("key", streamId, { code, down });
  }

  text(streamId, text) {
    if (typeof text !== "string" || text.length > 1024 || /[\u0000-\u0008\u000b-\u001f\u007f-\u009f]/u.test(text) ||
        /[\uD800-\uDBFF](?![\uDC00-\uDFFF])|(?<![\uD800-\uDBFF])[\uDC00-\uDFFF]/u.test(text)) {
      throw new TypeError("text must contain at most 1024 UTF-16 units, valid Unicode, and no controls except tab/newline");
    }
    this.#sendInput("text", streamId, { text });
  }

  releaseAll(streamId) {
    this.#sendInput("release_all", streamId, {});
  }

  #inputStream(streamId, requireOpen = true) {
    this.#assertConnected();
    const stream = this.#streams.get(streamId);
    if (!stream) throw new BeamItError("Stream does not belong to this session", { code: "not_found", streamId });
    if (requireOpen && !stream.ready) throw new BeamItError("Stream socket is not open", { code: "not_ready", streamId });
    return stream;
  }

  #sendInput(type, streamId, fields) {
    this.#inputStream(streamId);
    this.#send({ type, stream_id: streamId, ...fields });
  }

  disconnect() {
    if (this.#state === "disconnected") return;
    this.#state = "closing";
    for (const stream of this.#streams.values()) stream.close(1000, "client disconnected");
    this.#streams.clear();
    for (const audio of this.#audios.values()) audio.close(1000, "client disconnected");
    this.#audios.clear();
    const error = new BeamItError("Control connection closed", { code: "disconnected" });
    this.#rejectPending(error);
    this.#connectReject?.(error);
    this.#connectResolve = null;
    this.#connectReject = null;
    this.#password = null;
    if (this.#control && this.#control.readyState < 2) {
      this.#control.close(1000, "client disconnected");
    } else {
      this.#control = null;
      this.#state = "disconnected";
    }
  }

  async #handleControlMessage(data) {
    if (typeof data !== "string") {
      this.#fail(protocolError("Control socket accepts text messages only", "unexpected_message"), 1003);
      return;
    }
    if (encoder.encode(data).byteLength > MAX_SIGNALING_BYTES) {
      this.#fail(protocolError("Signaling message exceeds 64 KiB", "message_too_large"), 1009);
      return;
    }
    let message;
    try {
      message = JSON.parse(data);
      validateMessage(message);
    } catch (error) {
      this.#fail(error instanceof BeamItError ? error : protocolError("Malformed signaling JSON", "invalid_json"), 1002);
      return;
    }

    if (this.#state === "connecting") {
      if (message.type !== "hello") {
        this.#fail(protocolError("Expected hello as the first signaling message"), 1002);
        return;
      }
      await this.#authenticate(message);
      return;
    }
    if (this.#state === "authenticating") {
      if (message.type === "error") {
        this.#fail(new BeamItError(message.message ?? "Authentication failed", { code: message.code ?? "auth_failed" }), 1008);
        return;
      }
      if (message.type !== "authenticated") {
        this.#fail(protocolError("Expected authenticated response"), 1002);
        return;
      }
      let received;
      try {
        received = base64UrlDecode(message.server_proof);
      } catch (error) {
        this.#fail(error, 1008);
        return;
      }
      if (!equalBytes(received, this.#expectedServerProof)) {
        this.#fail(new BeamItError("Host authentication proof did not match", { code: "server_auth_failed" }), 1008);
        return;
      }
      this.#expectedServerProof = null;
      this.#state = "authenticated";
      const resolve = this.#connectResolve;
      this.#connectResolve = null;
      this.#connectReject = null;
      resolve?.(this);
      this.emit("authenticated", message);
      return;
    }

    if (message.type === "stream_config") {
      const stream = this.#streams.get(message.stream_id);
      if (stream) stream.updateConfig(validateVideoParameters(message.video));
      this.emit("stream_config", message);
      return;
    }
    if (message.type === "stream_stopped") {
      this.#streams.get(message.stream_id)?.handleStopped(message);
      this.#streams.delete(message.stream_id);
    }
    if (message.type === "audio_track_stopped") {
      this.#audios.get(message.audio_id)?.handleTrackStopped(message);
    }

    if (message.type === "error") {
      const error = new BeamItError(message.message ?? message.code ?? "Protocol error", {
        code: message.code ?? "protocol_error",
        id: message.id,
        streamId: message.stream_id,
      });
      if (message.id !== undefined && this.#pending.has(message.id)) {
        const pending = this.#pending.get(message.id);
        clearTimeout(pending.timer);
        this.#pending.delete(message.id);
        pending.reject(error);
      } else {
        this.emit("protocolError", error);
      }
      return;
    }

    if (message.id !== undefined && this.#pending.has(message.id)) {
      const pending = this.#pending.get(message.id);
      clearTimeout(pending.timer);
      this.#pending.delete(message.id);
      const expectedTypes = Array.isArray(pending.expectedType) ? pending.expectedType : [pending.expectedType];
      if (!expectedTypes.includes(message.type)) {
        pending.reject(protocolError(`Expected ${expectedTypes.join(" or ")}, received ${message.type}`, "unexpected_response", { id: message.id }));
      } else {
        pending.resolve(message);
      }
      if (message.type === "audio_stopped") {
        this.#audios.get(message.audio_id)?.handleStopped(message);
      }
    } else {
      if (message.type === "audio_stopped") {
        this.#audios.get(message.audio_id)?.handleStopped(message);
      }
      this.emit("message", message);
    }
  }

  async #authenticate(hello) {
    if (hello.protocol !== PROTOCOL_VERSION) {
      this.#fail(new BeamItError(`Unsupported protocol version: ${hello.protocol}`, { code: "protocol_version" }), 1002);
      return;
    }
    if (hello.auth?.mechanism !== AUTH_MECHANISM ||
        hello.auth.iterations !== PBKDF2_ITERATIONS) {
      this.#fail(protocolError("Unsupported authentication challenge"), 1008);
      return;
    }
    try {
      const salt = base64UrlDecode(hello.auth.salt);
      const serverNonce = base64UrlDecode(hello.auth.server_nonce);
      if (salt.byteLength === 0 || serverNonce.byteLength !== 32) {
        throw protocolError("Invalid authentication challenge");
      }
      const clientNonce = this.#crypto.getRandomValues(new Uint8Array(32));
      const proofs = await computeAuthProofs(this.#password, salt, hello.auth.iterations, serverNonce, clientNonce, this.#crypto);
      this.#expectedServerProof = proofs.serverProof;
      this.#state = "authenticating";
      this.#password = null;
      this.#send({ type: "authenticate", protocol: PROTOCOL_VERSION, client_nonce: base64UrlEncode(clientNonce), proof: base64UrlEncode(proofs.clientProof) });
    } catch (cause) {
      this.#password = null;
      this.#fail(cause instanceof BeamItError ? cause : new BeamItError("Could not compute authentication proof", { cause }), 1008);
    }
  }

  #request(type, fields, expectedType) {
    this.#assertConnected();
    const id = this.#allocateId();
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.#pending.delete(id);
        reject(new BeamItError(`${type} request timed out`, { code: "timeout", id }));
      }, this.#requestTimeoutMs);
      this.#pending.set(id, { resolve, reject, expectedType, timer, audioId: fields.audio_id });
      try {
        this.#send({ type, id, ...fields });
      } catch (error) {
        clearTimeout(timer);
        this.#pending.delete(id);
        reject(error);
      }
    });
  }

  #allocateId() {
    for (let attempts = 0; attempts <= this.#pending.size; attempts += 1) {
      const id = this.#nextId;
      this.#nextId = this.#nextId >= Number.MAX_SAFE_INTEGER ? 1 : this.#nextId + 1;
      if (!this.#pending.has(id)) return id;
    }
    throw new BeamItError("No signaling request IDs are available");
  }

  #send(message) {
    if (!this.#control || this.#control.readyState !== 1) {
      throw new BeamItError("Control socket is not open", { code: "disconnected" });
    }
    const text = JSON.stringify(message);
    if (encoder.encode(text).byteLength > MAX_SIGNALING_BYTES) {
      throw new BeamItError("Signaling message exceeds 64 KiB", { code: "message_too_large" });
    }
    this.#control.send(text);
  }

  #assertConnected() {
    if (!this.connected) throw new BeamItError("Authenticate before sending commands", { code: "not_authenticated" });
  }

  #handleControlClose(event, socket) {
    if (this.#control !== socket) return;
    const error = new BeamItError(`Control socket closed (${event.code}${event.reason ? `: ${event.reason}` : ""})`, {
      code: "disconnected",
    });
    this.#state = "disconnected";
    this.#control = null;
    this.#rejectPending(error);
    this.#connectReject?.(error);
    this.#connectResolve = null;
    this.#connectReject = null;
    this.#expectedServerProof = null;
    this.#password = null;
    for (const stream of this.#streams.values()) stream.controlClosed(error);
    this.#streams.clear();
    for (const audio of this.#audios.values()) audio.controlClosed(error);
    this.#audios.clear();
    this.emit("close", { code: event.code, reason: event.reason });
  }

  #rejectPending(error) {
    for (const pending of this.#pending.values()) {
      clearTimeout(pending.timer);
      pending.reject(error);
    }
    this.#pending.clear();
  }

  #fail(error, closeCode = 1011) {
    if (!(error instanceof Error)) error = new BeamItError(String(error));
    const reject = this.#connectReject;
    this.#connectResolve = null;
    this.#connectReject = null;
    this.#state = "failed";
    this.#password = null;
    reject?.(error);
    this.#rejectPending(error);
    this.emit("error", error);
    if (this.#control && this.#control.readyState < 2) {
      this.#control.close(closeCode, error.message.slice(0, 120));
    }
  }

  _removeStream(streamId, stream) {
    if (this.#streams.get(streamId) === stream) this.#streams.delete(streamId);
  }

  _removeAudio(audioId, audio) {
    if (this.#audios.get(audioId) === audio) this.#audios.delete(audioId);
    for (const [id, pending] of this.#pending) {
      if (pending.audioId !== audioId) continue;
      clearTimeout(pending.timer);
      this.#pending.delete(id);
      pending.reject(new BeamItError("Audio session ended", { code: "audio_stopped", id }));
    }
  }

  #audioSession(audioId) {
    this.#assertConnected();
    const audio = this.#audios.get(audioId);
    if (!audio) throw new BeamItError("Audio session does not belong to this connection", {
      code: "not_found",
    });
    return audio;
  }

  _sendRequestKeyFrame(streamId) {
    return this.requestKeyFrame(streamId);
  }
}

export class BeamItStream extends EventHub {
  #client;
  #WebSocket;
  #controlUrl;
  #socket = null;
  #opened = false;
  #closed = false;
  #configs = new Map();
  #waitingFrames = [];
  #unobservedFrames = [];
  #messageQueue = Promise.resolve();
  #mouseMode;

  constructor(client, offer, WebSocketImpl, controlUrl) {
    super();
    this.#client = client;
    this.#WebSocket = WebSocketImpl;
    this.#controlUrl = controlUrl;
    this.id = offer.stream_id;
    this.offer = offer;
    if (!["absolute", "relative"].includes(offer.input?.mouse_mode)) throw protocolError("Invalid stream input parameters");
    this.#mouseMode = offer.input.mouse_mode;
    this.video = validateVideoParameters(offer.video);
    this.#configs.set(this.video.generation, this.video);
  }

  on(type, listener) {
    const hadListener = this.listenerCount(type) > 0;
    const removeListener = super.on(type, listener);
    if (type === "frame" && !hadListener) {
      const pending = this.#unobservedFrames;
      this.#unobservedFrames = [];
      for (const frame of pending) {
        const video = this.#configs.get(frame.generation);
        if (video) super.emit("frame", { ...frame, video });
      }
    }
    return removeListener;
  }

  async open() {
    const streamUrl = new URL(this.offer.url);
    const token = streamUrl.searchParams.get("token");
    if (streamUrl.protocol !== "wss:" || streamUrl.host !== this.#controlUrl.host ||
        streamUrl.pathname !== `/streams/${encodeURIComponent(this.id)}` ||
        !token || base64UrlDecode(token).byteLength !== 32) {
      throw protocolError("Stream URL does not match the authenticated WSS host", "invalid_stream_url");
    }

    return new Promise((resolve, reject) => {
      try {
        this.#socket = new this.#WebSocket(streamUrl.href);
      } catch (cause) {
        reject(new BeamItError("Could not create the stream WebSocket", { cause }));
        return;
      }
      this.#socket.binaryType = "arraybuffer";
      this.#socket.addEventListener("open", () => {
        this.#opened = true;
        this.emit("open", this.offer);
        resolve(this);
      }, { once: true });
      this.#socket.addEventListener("message", (event) => {
        this.#messageQueue = this.#messageQueue.then(() => this.#handleFrame(event.data)).catch((error) => {
          this.emit("error", error);
          this.close(1002, "invalid video frame");
        });
      });
      this.#socket.addEventListener("close", (event) => {
        const wasOpen = this.#opened;
        this.#closed = true;
        this.#client._removeStream(this.id, this);
        if (!wasOpen) reject(new BeamItError(`Stream socket closed before opening (${event.code})`, { code: "stream_open_failed" }));
        this.emit("close", { code: event.code, reason: event.reason });
      });
      this.#socket.addEventListener("error", () => this.emit("socketError", { socket: "stream", streamId: this.id }));
    });
  }

  async stop() {
    return this.#client.stopStream(this.id);
  }

  get ready() {
    return this.#opened && !this.#closed && this.#socket?.readyState === 1;
  }

  get mouseMode() {
    return this.#mouseMode;
  }

  _updateMouseMode(mode) {
    this.#mouseMode = mode;
    this.emit("mouse_mode", mode);
  }

  setMouseMode(mode) { return this.#client.setMouseMode(this.id, mode); }
  getInputState() { return this.#client.getInputState(this.id); }
  mouseMove(position) { this.#client.mouseMove(this.id, position); }
  mouseButton(button, down) { this.#client.mouseButton(this.id, button, down); }
  mouseWheel(deltas) { this.#client.mouseWheel(this.id, deltas); }
  key(code, down) { this.#client.key(this.id, code, down); }
  text(text) { this.#client.text(this.id, text); }
  releaseAll() { this.#client.releaseAll(this.id); }

  requestKeyFrame() {
    return this.#client._sendRequestKeyFrame(this.id);
  }

  close(code = 1000, reason = "client closed stream") {
    if (this.#closed) return;
    this.#closed = true;
    if (this.#socket && this.#socket.readyState < 2) this.#socket.close(code, reason);
    this.#client._removeStream(this.id, this);
  }

  updateConfig(video) {
    if (video.generation <= this.video.generation) {
      throw protocolError("Video generation must increase on reconfiguration");
    }
    this.video = video;
    this.#configs.set(video.generation, video);
    this.emit("config", video);
    const pending = this.#waitingFrames;
    this.#waitingFrames = [];
    for (const frame of pending) {
      if (frame.generation === video.generation) this.#emitFrame(frame, video);
      else this.#waitingFrames.push(frame);
    }
  }

  handleStopped(message) {
    this.close(1000, "stream stopped");
    this.emit("stopped", message);
  }

  controlClosed(error) {
    this.emit("error", error);
    this.close(1001, "control connection closed");
  }

  async #handleFrame(data) {
    if (typeof data === "string") {
      this.close(1003, "stream socket accepts binary only");
      throw protocolError("Stream socket accepts binary messages only", "unexpected_message");
    }
    const frameData = data instanceof Blob ? await data.arrayBuffer() : data;
    const frame = parseVideoFrame(frameData);
    if (!frame) return;
    const config = this.#configs.get(frame.generation);
    if (!config) {
      this.#waitingFrames.push(frame);
      if (this.#waitingFrames.length > 4) this.#waitingFrames.shift();
      return;
    }
    this.#emitFrame(frame, config);
  }

  #emitFrame(frame, video) {
    if (this.listenerCount("frame") === 0) {
      this.#unobservedFrames.push(frame);
      if (this.#unobservedFrames.length > 4) this.#unobservedFrames.shift();
      return;
    }
    this.emit("frame", { ...frame, video });
  }
}
