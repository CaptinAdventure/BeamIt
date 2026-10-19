import AppKit
import CoreVideo

final class BeamItVideoStream {
    let diagnostics: BeamItStreamDiagnostics
    let offer: BeamItStreamOffer
    private(set) var video: BeamItVideo
    var onFrame: ((CVPixelBuffer, UInt64) -> Void)?
    var onError: ((Error) -> Void)?
    var onInputError: ((String) -> Void)?
    var onVideoConfig: ((BeamItVideo) -> Void)?
    var onAttention: (() -> Void)?
    private(set) var attentionWindows = Set<String>()

    func updateAttention(hwnd: String, requesting: Bool) {
        let changed: Bool
        if requesting {
            changed = attentionWindows.insert(hwnd).inserted
        } else {
            changed = attentionWindows.remove(hwnd) != nil
        }
        if changed { onAttention?() }
    }

    private let session: URLSession
    private weak var control: BeamItConnection?
    private var socket: URLSessionWebSocketTask?
    private var receiveTask: Task<Void, Never>?
    private var decoder: BeamItVideoDecoder?
    private var closed = false

    init(offer: BeamItStreamOffer, session: URLSession, control: BeamItConnection) {
        diagnostics = BeamItStreamDiagnostics(clock: control.clockEstimate)
        self.offer = offer
        video = offer.video
        self.session = session
        self.control = control
    }

    func updateVideo(_ video: BeamItVideo) {
        guard video.generation > self.video.generation,
              video.width > 0, video.height > 0,
              video.codec == self.video.codec
        else {
            onError?(StreamError.invalidFrame("The host sent invalid stream configuration."))
            return
        }
        self.video = video
        decoder = nil
        onVideoConfig?(video)
        control?.requestKeyFrame(offer.streamID)
    }

    func open() async throws {
        let socket = session.webSocketTask(with: offer.url)
        self.socket = socket
        socket.resume()
        receiveTask = Task {
            await receiveFrames()
        }
    }

    func stop() async {
        guard !closed else { return }
        do {
            try await control?.stopStream(offer.streamID)
        } catch {
            onError?(error)
            close()
        }
    }

    func close() {
        guard !closed else { return }
        closed = true
        receiveTask?.cancel()
        receiveTask = nil
        socket?.cancel(with: .normalClosure, reason: nil)
        socket = nil
        decoder = nil
        onFrame = nil
        onError = nil
        onInputError = nil
        onVideoConfig = nil
        attentionWindows.removeAll()
        onAttention?()
        onAttention = nil
    }

    func handleStopped() {
        close()
    }

    func sendInput(_ type: String, _ fields: [String: Any] = [:]) {
        guard !closed, socket != nil else { return }
        var message = fields
        message["type"] = type
        message["stream_id"] = offer.streamID
        control?.sendInput(message)
    }

    var clipboardSyncEnabled: Bool { control?.clipboardSync.enabled == true }
    var popupCoordinator: BeamItPopups? { control?.popups }
    var isClosed: Bool { closed }

    func setMouseMode(_ mode: String) async throws {
        guard !closed, let control else { throw CancellationError() }
        try await control.setMouseMode(offer.streamID, mode: mode)
    }

    func setRate(bitrate: Int, frameRate: Int, peak: Int? = nil) async throws {
        guard !closed, let control else { throw CancellationError() }
        try await control.setStreamRate(offer.streamID, bitrate: bitrate, frameRate: frameRate, peak: peak)
    }

    func updateRate(bitrate: Int, frameRate: Int, generation: Int, peak: Int? = nil) {
        guard !closed, generation == video.generation else { return }
        video.bitrate_kbps = bitrate
        video.frame_rate = frameRate
        if let peak { video.max_bitrate_kbps = peak }
    }

    func windowIcon() async throws -> NSImage? {
        guard let hwnd = offer.source.hwnd else { return nil }
        guard !closed, let control else { throw CancellationError() }
        let data = try await control.getWindowIcon(hwnd)
        try Task.checkCancellation()
        guard !closed else { throw CancellationError() }
        guard let bitmap = NSBitmapImageRep(data: data),
              (1...256).contains(bitmap.pixelsWide), (1...256).contains(bitmap.pixelsHigh) else {
            throw StreamError.invalidFrame("The host returned an invalid window icon.")
        }
        let image = NSImage(size: NSSize(width: bitmap.pixelsWide, height: bitmap.pixelsHigh))
        image.addRepresentation(bitmap)
        return image
    }

    func uploadClipboardForPaste() async throws {
        guard !closed, let control else { throw CancellationError() }
        try await control.clipboardSync.uploadLocalClipboard()
        guard !closed else { throw CancellationError() }
    }

    private func receiveFrames() async {
        guard let socket else { return }
        do {
            while !Task.isCancelled {
                let message = try await socket.receive()
                guard case .data(let data) = message else {
                    throw StreamError.invalidFrame("The host sent a non-binary video message.")
                }
                guard let frame = try parseFrame(data) else { continue }
                diagnostics.receivedPacket(bytes: frame.payload.count)
                if decoder == nil || decoder?.generation != frame.generation {
                    guard frame.isKeyFrame else { continue }
                    guard frame.generation == video.generation else { continue }
                    decoder = try BeamItVideoDecoder(
                        codec: video.codec,
                        width: video.width,
                        height: video.height,
                        generation: frame.generation,
                        diagnostics: diagnostics,
                        onError: { [weak self] error in self?.onError?(error) }
                    )
                }
                guard let decoder else { continue }
                try decoder.decode(frame.payload, timestamp: frame.timestamp) { [weak self] image, captureUS in
                    self?.onFrame?(image, captureUS)
                }
            }
        } catch {
            guard !closed else { return }
            onError?(error)
        }
    }

    private func parseFrame(_ data: Data) throws -> EncodedFrame? {
        guard data.count >= 32 else {
            throw StreamError.invalidFrame("The host sent a truncated video frame.")
        }
        let version = data[0]
        let messageType = data[1]
        guard version == 1 else {
            throw StreamError.invalidFrame("The host sent an unsupported video frame version.")
        }
        guard messageType == 1 else { return nil }

        let headerLength = Int(data.readUInt16BE(at: 2))
        guard headerLength >= 32, headerLength < data.count else {
            throw StreamError.invalidFrame("The host sent an invalid video frame header.")
        }
        return EncodedFrame(
            generation: Int(data.readUInt32BE(at: 8)),
            isKeyFrame: data.readUInt32BE(at: 4) & 1 != 0,
            timestamp: data.readUInt64BE(at: 24),
            payload: data.subdata(in: headerLength..<data.count)
        )
    }

    private struct EncodedFrame {
        let generation: Int
        let isKeyFrame: Bool
        let timestamp: UInt64
        let payload: Data
    }

    private enum StreamError: LocalizedError {
        case invalidFrame(String)

        var errorDescription: String? {
            if case .invalidFrame(let message) = self { return message }
            return nil
        }
    }
}

private extension Data {
    func readUInt16BE(at offset: Int) -> UInt16 {
        UInt16(self[offset]) << 8 | UInt16(self[offset + 1])
    }

    func readUInt32BE(at offset: Int) -> UInt32 {
        UInt32(self[offset]) << 24 | UInt32(self[offset + 1]) << 16 |
        UInt32(self[offset + 2]) << 8 | UInt32(self[offset + 3])
    }

    func readUInt64BE(at offset: Int) -> UInt64 {
        UInt64(readUInt32BE(at: offset)) << 32 | UInt64(readUInt32BE(at: offset + 4))
    }
}
