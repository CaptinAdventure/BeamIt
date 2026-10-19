import AppKit
import AVFoundation
import CoreMedia
import CoreVideo
import Foundation
import VideoToolbox

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

private final class BeamItVideoDecoder {
    let generation: Int

    private let codec: String
    private let width: Int
    private let height: Int
    private let onError: (Error) -> Void
    private var formatDescription: CMVideoFormatDescription?
    private var session: VTDecompressionSession?
    private let diagnostics: BeamItStreamDiagnostics
    private let timingLock = NSLock()
    private var decodeStarts: [Int64: (start: TimeInterval, captureUS: UInt64)] = [:]
    private var nextDecodeID: Int64 = 0

    init(codec: String, width: Int, height: Int, generation: Int, diagnostics: BeamItStreamDiagnostics, onError: @escaping (Error) -> Void) throws {
        self.diagnostics = diagnostics
        self.codec = codec
        self.width = width
        self.height = height
        self.generation = generation
        self.onError = onError
    }

    deinit {
        if let session {
            VTDecompressionSessionInvalidate(session)
        }
    }

    func decode(_ accessUnit: Data, timestamp: UInt64, output: @escaping (CVPixelBuffer, UInt64) -> Void) throws {
        let nalUnits = Self.splitNALUnits(accessUnit)
        guard !nalUnits.isEmpty else {
            throw DecoderError.invalidAccessUnit
        }
        if session == nil {
            let parameterSets = Self.parameterSets(in: nalUnits, codec: codec)
            try createSession(parameterSets: parameterSets, output: output)
        }

        let mediaNALUnits = nalUnits.filter { !Self.isNonPictureNAL($0, codec: codec) }
        let sampleData = Self.lengthPrefixed(mediaNALUnits)
        guard !sampleData.isEmpty else { throw DecoderError.invalidAccessUnit }
        try submit(sampleData, timestamp: timestamp)
    }

    private func createSession(parameterSets: [[UInt8]], output: @escaping (CVPixelBuffer, UInt64) -> Void) throws {
        let requiredParameterSets = codec == "h264" ? 2 : 3
        guard parameterSets.count >= requiredParameterSets else {
            throw DecoderError.missingParameterSets
        }
        var description: CMVideoFormatDescription?
        let formatStatus = Self.makeFormatDescription(codec: codec, parameterSets: parameterSets, output: &description)
        guard formatStatus == noErr, let description else {
            throw DecoderError.videoToolbox(formatStatus)
        }
        formatDescription = description

        var callback = VTDecompressionOutputCallbackRecord(
            decompressionOutputCallback: { refCon, _, status, _, imageBuffer, presentationTimeStamp, _ in
                guard let refCon else { return }
                let decoder = Unmanaged<BeamItVideoDecoder>.fromOpaque(refCon).takeUnretainedValue()
                decoder.timingLock.lock()
                let start = decoder.decodeStarts.removeValue(forKey: presentationTimeStamp.value)
                decoder.timingLock.unlock()
                guard status == noErr, let imageBuffer else {
                    decoder.onError(DecoderError.videoToolbox(status))
                    return
                }
                if let start {
                    decoder.diagnostics.decodedFrame(imageBuffer, milliseconds: (ProcessInfo.processInfo.systemUptime - start.start) * 1000)
                    decoder.output?(imageBuffer, start.captureUS)
                }
            },
            decompressionOutputRefCon: Unmanaged.passUnretained(self).toOpaque()
        )
        self.output = output
        let attributes: [NSString: Any] = [
            kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_32BGRA,
            kCVPixelBufferWidthKey: width,
            kCVPixelBufferHeightKey: height,
            kCVPixelBufferIOSurfacePropertiesKey: [:] as [String: Any]
        ]
        let status = withUnsafeMutablePointer(to: &callback) { callbackPointer in
            VTDecompressionSessionCreate(
                allocator: kCFAllocatorDefault,
                formatDescription: description,
                decoderSpecification: nil,
                imageBufferAttributes: attributes as CFDictionary,
                outputCallback: callbackPointer,
                decompressionSessionOut: &session
            )
        }
        guard status == noErr else { throw DecoderError.videoToolbox(status) }
        if let session {
            let realtimeStatus = VTSessionSetProperty(
                session,
                key: kVTDecompressionPropertyKey_RealTime,
                value: kCFBooleanTrue
            )
            guard realtimeStatus == noErr else { throw DecoderError.videoToolbox(realtimeStatus) }
        }
    }

    private var output: ((CVPixelBuffer, UInt64) -> Void)?

    private func submit(_ data: Data, timestamp: UInt64) throws {
        guard let session, let formatDescription else { throw DecoderError.decoderUnavailable }
        // Idle-repeat pictures can share a host capture timestamp; use a unique local token
        // for callback timing instead of conflating those separate decode submissions.
        let decodeID = nextDecodeID
        nextDecodeID &+= 1
        var blockBuffer: CMBlockBuffer?
        let createBlockStatus = CMBlockBufferCreateWithMemoryBlock(
            allocator: kCFAllocatorDefault,
            memoryBlock: nil,
            blockLength: data.count,
            blockAllocator: kCFAllocatorDefault,
            customBlockSource: nil,
            offsetToData: 0,
            dataLength: data.count,
            flags: 0,
            blockBufferOut: &blockBuffer
        )
        guard createBlockStatus == kCMBlockBufferNoErr, let blockBuffer else {
            throw DecoderError.videoToolbox(createBlockStatus)
        }
        let copyStatus = data.withUnsafeBytes {
            CMBlockBufferReplaceDataBytes(
                with: $0.baseAddress!,
                blockBuffer: blockBuffer,
                offsetIntoDestination: 0,
                dataLength: data.count
            )
        }
        guard copyStatus == kCMBlockBufferNoErr else { throw DecoderError.videoToolbox(copyStatus) }

        var timing = CMSampleTimingInfo(
            duration: .invalid,
            presentationTimeStamp: CMTime(value: decodeID, timescale: 1_000_000),
            decodeTimeStamp: .invalid
        )
        var sampleSize = data.count
        var sampleBuffer: CMSampleBuffer?
        let sampleStatus = CMSampleBufferCreateReady(
            allocator: kCFAllocatorDefault,
            dataBuffer: blockBuffer,
            formatDescription: formatDescription,
            sampleCount: 1,
            sampleTimingEntryCount: 1,
            sampleTimingArray: &timing,
            sampleSizeEntryCount: 1,
            sampleSizeArray: &sampleSize,
            sampleBufferOut: &sampleBuffer
        )
        guard sampleStatus == noErr, let sampleBuffer else { throw DecoderError.videoToolbox(sampleStatus) }
        var infoFlags = VTDecodeInfoFlags()
        timingLock.lock()
        decodeStarts[decodeID] = (ProcessInfo.processInfo.systemUptime, timestamp)
        timingLock.unlock()
        let decodeStatus = VTDecompressionSessionDecodeFrame(
            session,
            sampleBuffer: sampleBuffer,
            flags: [._EnableAsynchronousDecompression],
            frameRefcon: nil,
            infoFlagsOut: &infoFlags
        )
        if decodeStatus != noErr || infoFlags.contains(.frameDropped) {
            timingLock.lock()
            decodeStarts.removeValue(forKey: decodeID)
            timingLock.unlock()
        }
        guard decodeStatus == noErr else { throw DecoderError.videoToolbox(decodeStatus) }
    }

    private static func makeFormatDescription(
        codec: String,
        parameterSets: [[UInt8]],
        output: inout CMVideoFormatDescription?
    ) -> OSStatus {
        var pointers: [UnsafePointer<UInt8>] = []
        let sizes = parameterSets.map(\.count)

        func withPointers<T>(_ index: Int, _ body: () -> T) -> T {
            guard index < parameterSets.count else { return body() }
            return parameterSets[index].withUnsafeBufferPointer { buffer in
                pointers.append(buffer.baseAddress!)
                defer { pointers.removeLast() }
                return withPointers(index + 1, body)
            }
        }

        return withPointers(0) {
            pointers.withUnsafeBufferPointer { pointerBuffer in
                sizes.withUnsafeBufferPointer { sizeBuffer in
                    if codec == "h264" {
                        return CMVideoFormatDescriptionCreateFromH264ParameterSets(
                            allocator: kCFAllocatorDefault,
                            parameterSetCount: parameterSets.count,
                            parameterSetPointers: pointerBuffer.baseAddress!,
                            parameterSetSizes: sizeBuffer.baseAddress!,
                            nalUnitHeaderLength: 4,
                            formatDescriptionOut: &output
                        )
                    }
                    return CMVideoFormatDescriptionCreateFromHEVCParameterSets(
                        allocator: kCFAllocatorDefault,
                        parameterSetCount: parameterSets.count,
                        parameterSetPointers: pointerBuffer.baseAddress!,
                        parameterSetSizes: sizeBuffer.baseAddress!,
                        nalUnitHeaderLength: 4,
                        extensions: nil,
                        formatDescriptionOut: &output
                    )
                }
            }
        }
    }

    private static func splitNALUnits(_ data: Data) -> [[UInt8]] {
        let bytes = [UInt8](data)
        var starts: [(offset: Int, length: Int)] = []
        var index = 0
        while index + 3 < bytes.count {
            if bytes[index] == 0, bytes[index + 1] == 0, bytes[index + 2] == 1 {
                starts.append((index, 3))
                index += 3
            } else if index + 4 <= bytes.count,
                      bytes[index] == 0, bytes[index + 1] == 0, bytes[index + 2] == 0, bytes[index + 3] == 1 {
                starts.append((index, 4))
                index += 4
            } else {
                index += 1
            }
        }

        return starts.enumerated().compactMap { position, start in
            let begin = start.offset + start.length
            let end = position + 1 < starts.count ? starts[position + 1].offset : bytes.count
            guard begin < end else { return nil }
            return Array(bytes[begin..<end])
        }
    }

    private static func parameterSets(in nalUnits: [[UInt8]], codec: String) -> [[UInt8]] {
        if codec == "h264" {
            let sps = nalUnits.first { $0[0] & 0x1F == 7 }
            let pps = nalUnits.first { $0[0] & 0x1F == 8 }
            return [sps, pps].compactMap { $0 }
        }
        let vps = nalUnits.first { ($0[0] >> 1) & 0x3F == 32 }
        let sps = nalUnits.first { ($0[0] >> 1) & 0x3F == 33 }
        let pps = nalUnits.first { ($0[0] >> 1) & 0x3F == 34 }
        return [vps, sps, pps].compactMap { $0 }
    }

    private static func isNonPictureNAL(_ nal: [UInt8], codec: String) -> Bool {
        if codec == "h264" {
            return [7, 8, 9].contains(Int(nal[0] & 0x1F))
        }
        return [32, 33, 34, 35].contains(Int((nal[0] >> 1) & 0x3F))
    }

    private static func lengthPrefixed(_ nalUnits: [[UInt8]]) -> Data {
        var data = Data()
        for nal in nalUnits {
            var length = UInt32(nal.count).bigEndian
            withUnsafeBytes(of: &length) { data.append(contentsOf: $0) }
            data.append(contentsOf: nal)
        }
        return data
    }

    private enum DecoderError: LocalizedError {
        case invalidAccessUnit
        case missingParameterSets
        case decoderUnavailable
        case videoToolbox(OSStatus)

        var errorDescription: String? {
            switch self {
            case .invalidAccessUnit:
                return "The host sent an invalid or empty encoded access unit."
            case .missingParameterSets:
                return "The stream's key frame did not include the codec parameter sets required by VideoToolbox."
            case .decoderUnavailable:
                return "The VideoToolbox decoder is not ready."
            case .videoToolbox(let status):
                return "VideoToolbox could not decode this stream (status \(status))."
            }
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

final class BeamItStreamWindowController: NSWindowController, NSWindowDelegate {
    var infoWindow: NSWindow?
    var parametersWindow: NSWindow?
    let stream: BeamItVideoStream
    let sourceKind: String
    var onClose: (() -> Void)?
    var onError: ((String) -> Void)?
    private var isClosingFromHost = false
    private let frameView: BeamItFrameView
    private let inputHandler: BeamItInputHandler
    private var keyMonitor: Any?
    private var videoPixelSize: NSSize
    private var isShowingInputError = false
    private var attentionRequest: Int?
    private var iconTask: Task<Void, Never>?
    let isPopup: Bool
    /// The most recently focused stream window, the target of "Send Text to Target…".
    private(set) static weak var activeController: BeamItStreamWindowController?
    var typingTask: Task<Void, Never>?

    var isTypingText: Bool { typingTask != nil }

    func stopTypingText() {
        typingTask?.cancel()
        typingTask = nil
    }

    var title: String {
        if let title = stream.offer.source.title, !title.isEmpty {
            return title
        }
        if stream.offer.source.kind == "desktop" {
            return "Desktop · \((stream.offer.source.capture ?? "").uppercased())"
        }
        return "Stream \(stream.offer.streamID)"
    }

    init(stream: BeamItVideoStream, isPopup: Bool = false) {
        self.isPopup = isPopup
        self.stream = stream
        sourceKind = stream.offer.source.kind
        videoPixelSize = NSSize(width: stream.offer.video.width, height: stream.offer.video.height)
        let scale = NSScreen.main?.backingScaleFactor ?? 1
        let pointSize = NSSize(width: videoPixelSize.width / scale, height: videoPixelSize.height / scale)
        frameView = BeamItFrameView(frame: NSRect(origin: .zero, size: pointSize))
        frameView.diagnostics = stream.diagnostics
        inputHandler = BeamItInputHandler(stream: stream)
        frameView.inputHandler = inputHandler
        let window: NSWindow
        if isPopup {
            let panel = BeamItPopupPanel(
                contentRect: frameView.frame, styleMask: [.borderless, .nonactivatingPanel],
                backing: .buffered, defer: false
            )
            panel.hidesOnDeactivate = true
            panel.becomesKeyOnlyIfNeeded = true
            window = panel
        } else {
            window = NSWindow(
                contentRect: frameView.frame, styleMask: [.titled, .closable, .miniaturizable],
                backing: .buffered, defer: false
            )
        }
        window.isReleasedWhenClosed = false
        window.isOpaque = true
        window.backgroundColor = .black
        window.contentView = frameView
        window.center()
        super.init(window: window)
        window.title = title
        window.delegate = self
        window.initialFirstResponder = frameView
        // Key events are taken from a local monitor because AppKit never delivers keyUp while ⌘ is
        // held, and the main menu would otherwise consume ⌘-shortcuts meant for the host.
        keyMonitor = NSEvent.addLocalMonitorForEvents(matching: [.keyDown, .keyUp, .flagsChanged]) { [weak self] event in
            guard let self, let window = self.window, event.window === window, window.isKeyWindow,
                  window.attachedSheet == nil else {
                return event
            }
            return self.inputHandler.handleKeyEvent(event) ? nil : event
        }
        applyPixelSize()
        stream.onFrame = { [weak self] pixelBuffer, captureUS in
            self?.frameView.display(pixelBuffer, captureUS: captureUS)
        }
        stream.onVideoConfig = { [weak self] video in
            DispatchQueue.main.async {
                guard let self else { return }
                self.videoPixelSize = NSSize(width: video.width, height: video.height)
                self.applyPixelSize()
                self.stream.popupCoordinator?.reposition()
            }
        }
        stream.onError = { [weak self] error in
            DispatchQueue.main.async {
                self?.onError?(error.localizedDescription)
                self?.closeAfterHostStop()
            }
            NSLog("BeamIt stream error: %@", error.localizedDescription)
        }
        stream.onInputError = { [weak self] message in
            self?.handleInputError(message)
        }
        stream.onAttention = { [weak self] in
            self?.updateDockAttention()
        }
        iconTask = Task { [weak self, stream] in
            guard !isPopup else { return }
            do {
                guard let image = try await stream.windowIcon() else { return }
                try Task.checkCancellation()
                guard let self, let window = self.window else { return }
                window.miniwindowImage = image
                let accessory = NSTitlebarAccessoryViewController()
                accessory.layoutAttribute = .left
                let container = NSView(frame: NSRect(x: 0, y: 0, width: 24, height: 22))
                let icon = NSImageView(frame: NSRect(x: 4, y: 3, width: 16, height: 16))
                icon.image = image
                icon.imageScaling = .scaleProportionallyUpOrDown
                icon.setAccessibilityLabel("\(self.title) icon")
                container.addSubview(icon)
                accessory.view = container
                window.addTitlebarAccessoryViewController(accessory)
            } catch is CancellationError {
                // Closing the stream cancels icon loading.
            } catch {
                NSLog("BeamIt window icon unavailable: %@", error.localizedDescription)
            }
        }
    }

    private func cancelDockAttention() {
        if let attentionRequest {
            NSApp.cancelUserAttentionRequest(attentionRequest)
            self.attentionRequest = nil
        }
    }

    private func updateDockAttention() {
        guard !isPopup else { return }
        guard !stream.attentionWindows.isEmpty else {
            cancelDockAttention()
            return
        }
        guard let window, window.isVisible,
              !(NSApp.isActive && window.isKeyWindow), attentionRequest == nil else { return }
        attentionRequest = NSApp.requestUserAttention(.informationalRequest)
    }

    private func handleInputError(_ message: String) {
        stopTypingText()
        inputHandler.releaseAll()
        onError?(message)
        guard !isShowingInputError, let window, window.attachedSheet == nil else { return }
        isShowingInputError = true
        let alert = NSAlert()
        alert.messageText = "Remote input was rejected"
        alert.informativeText = "\(message)\n\nText sending has stopped; some characters may not have arrived. On the Windows host, activate the target window and select its text field, then retry. Elevated applications require an elevated host; input is unavailable on the lock or UAC screen."
        alert.addButton(withTitle: "OK")
        alert.beginSheetModal(for: window) { [weak self] _ in
            self?.isShowingInputError = false
        }
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    func show() {
        if isPopup {
            window?.orderFront(nil)
        } else {
            showWindow(nil)
            window?.makeKeyAndOrderFront(nil)
            stream.popupCoordinator?.register(self)
            updateDockAttention()
        }
    }

    func closeAfterHostStop() {
        isClosingFromHost = true
        stream.handleStopped()
        close()
    }

    /// Sizes the window so one video pixel maps to one device pixel on its current screen.
    private func applyPixelSize() {
        guard let window else { return }
        let scale = window.backingScaleFactor > 0 ? window.backingScaleFactor : 1
        let size = NSSize(width: videoPixelSize.width / scale, height: videoPixelSize.height / scale)
        frameView.layer?.contentsScale = scale
        guard window.contentRect(forFrameRect: window.frame).size != size else { return }
        let oldFrame = window.frame
        var newFrame = window.frameRect(forContentRect: NSRect(origin: .zero, size: size))
        newFrame.origin = NSPoint(x: oldFrame.minX, y: oldFrame.maxY - newFrame.height)
        window.setFrame(newFrame, display: true)
    }

    func windowDidChangeBackingProperties(_ notification: Notification) {
        applyPixelSize()
        stream.popupCoordinator?.reposition()
    }

    func windowDidChangeScreen(_ notification: Notification) {
        applyPixelSize()
        stream.popupCoordinator?.reposition()
    }

    func windowDidMove(_ notification: Notification) {
        if !isPopup { stream.popupCoordinator?.reposition() }
    }

    func windowDidBecomeKey(_ notification: Notification) {
        Self.activeController = self
        cancelDockAttention()
    }

    func windowDidResignKey(_ notification: Notification) {
        inputHandler.releaseAll()
    }

    func windowWillClose(_ notification: Notification) {
        if let parametersWindow {
            window?.endSheet(parametersWindow)
            parametersWindow.close()
            self.parametersWindow = nil
        }
        infoWindow?.close()
        infoWindow = nil
        if !isPopup { stream.popupCoordinator?.unregister(self) }
        iconTask?.cancel()
        iconTask = nil
        cancelDockAttention()
        stopTypingText()
        if Self.activeController === self {
            Self.activeController = nil
        }
        if let keyMonitor {
            NSEvent.removeMonitor(keyMonitor)
            self.keyMonitor = nil
        }
        frameView.inputHandler = nil
        stream.onFrame = nil
        if !isClosingFromHost {
            Task { await stream.stop() }
        }
        onClose?()
    }
}

private final class BeamItFrameView: NSView {
    var diagnostics: BeamItStreamDiagnostics?
    private var pendingSince: TimeInterval = 0
    private var pendingCaptureUS: UInt64 = 0
    var inputHandler: BeamItInputHandler?
    private var trackingArea: NSTrackingArea?

    override var acceptsFirstResponder: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let trackingArea {
            removeTrackingArea(trackingArea)
        }
        let area = NSTrackingArea(
            rect: .zero,
            options: [.mouseMoved, .activeAlways, .inVisibleRect],
            owner: self,
            userInfo: nil
        )
        addTrackingArea(area)
        trackingArea = area
    }

    override func mouseMoved(with event: NSEvent) { inputHandler?.mouseMoved(event, in: self) }
    override func mouseDragged(with event: NSEvent) { inputHandler?.mouseMoved(event, in: self) }
    override func rightMouseDragged(with event: NSEvent) { inputHandler?.mouseMoved(event, in: self) }
    override func otherMouseDragged(with event: NSEvent) { inputHandler?.mouseMoved(event, in: self) }
    override func mouseDown(with event: NSEvent) { inputHandler?.mouseButton(event, in: self, down: true) }
    override func mouseUp(with event: NSEvent) { inputHandler?.mouseButton(event, in: self, down: false) }
    override func rightMouseDown(with event: NSEvent) { inputHandler?.mouseButton(event, in: self, down: true) }
    override func rightMouseUp(with event: NSEvent) { inputHandler?.mouseButton(event, in: self, down: false) }
    override func otherMouseDown(with event: NSEvent) { inputHandler?.mouseButton(event, in: self, down: true) }
    override func otherMouseUp(with event: NSEvent) { inputHandler?.mouseButton(event, in: self, down: false) }
    override func scrollWheel(with event: NSEvent) { inputHandler?.scrollWheel(event, in: self) }
    private let frameLock = NSLock()
    private var pendingPixelBuffer: CVPixelBuffer?
    private var presentationScheduled = false
    private var formatDescription: CMVideoFormatDescription?

    override var wantsUpdateLayer: Bool { true }

    override func makeBackingLayer() -> CALayer {
        let layer = AVSampleBufferDisplayLayer()
        layer.backgroundColor = NSColor.black.cgColor
        layer.videoGravity = .resize
        return layer
    }

    func display(_ pixelBuffer: CVPixelBuffer, captureUS: UInt64) {
        frameLock.lock()
        if pendingPixelBuffer != nil { diagnostics?.coalescedFrame() }
        pendingSince = ProcessInfo.processInfo.systemUptime
        pendingCaptureUS = captureUS
        pendingPixelBuffer = pixelBuffer
        let shouldSchedule = !presentationScheduled
        presentationScheduled = true
        frameLock.unlock()

        if shouldSchedule {
            DispatchQueue.main.async { [weak self] in
                self?.presentLatestFrame()
            }
        }
    }

    private func presentLatestFrame() {
        frameLock.lock()
        let pixelBuffer = pendingPixelBuffer
        let queuedSince = pendingSince
        let captureUS = pendingCaptureUS
        pendingPixelBuffer = nil
        presentationScheduled = false
        frameLock.unlock()
        guard let pixelBuffer else { return }

        if let current = formatDescription,
           !CMVideoFormatDescriptionMatchesImageBuffer(current, imageBuffer: pixelBuffer) {
            formatDescription = nil
        }
        if formatDescription == nil,
           CMVideoFormatDescriptionCreateForImageBuffer(
               allocator: kCFAllocatorDefault,
               imageBuffer: pixelBuffer,
               formatDescriptionOut: &formatDescription
           ) != noErr {
            return
        }
        guard let formatDescription else { return }
        var timing = CMSampleTimingInfo(
            duration: .invalid,
            presentationTimeStamp: CMClockGetTime(CMClockGetHostTimeClock()),
            decodeTimeStamp: .invalid
        )
        var sampleBuffer: CMSampleBuffer?
        guard CMSampleBufferCreateReadyWithImageBuffer(
            allocator: kCFAllocatorDefault,
            imageBuffer: pixelBuffer,
            formatDescription: formatDescription,
            sampleTiming: &timing,
            sampleBufferOut: &sampleBuffer
        ) == noErr, let sampleBuffer else { return }
        let displayLayer = layer as? AVSampleBufferDisplayLayer
        displayLayer?.sampleBufferRenderer.enqueue(sampleBuffer)
        if displayLayer != nil {
            let now = ProcessInfo.processInfo.systemUptime
            diagnostics?.queued(milliseconds: (now - queuedSince) * 1000, captureUS: captureUS, now: now)
        }
    }
}
