const PROTOCOL_VERSION = 1;
const AUTH_MECHANISM = "pbkdf2-sha256-hmac";
const PBKDF2_ITERATIONS = 600_000;
const MAX_SIGNALING_BYTES = 64 * 1024;
const FRAME_VERSION = 1;
const VIDEO_MESSAGE = 1;
const FRAME_HEADER_MIN_SIZE = 32;

const encoder = new TextEncoder();

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
  return result;
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

export class BeamItClient extends EventHub {
  #WebSocket;
  #crypto;
  #requestTimeoutMs;
  #control = null;
  #controlUrl = null;
  #state = "disconnected";
  #pending = new Map();
  #streams = new Map();
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
      if (message.type !== pending.expectedType) {
        pending.reject(protocolError(`Expected ${pending.expectedType}, received ${message.type}`, "unexpected_response", { id: message.id }));
      } else {
        pending.resolve(message);
      }
    } else {
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
      this.#pending.set(id, { resolve, reject, expectedType, timer });
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
