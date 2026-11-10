import AVFoundation
import AudioToolbox
import CoreAudio
import Observation

struct BeamItAudioTrackStatus: Identifiable {
    let id: Int
    let title: String
    let source: BeamItAudioSource
}

@MainActor
@Observable
final class BeamItAudioSession {
    let audioID: String
    let microphoneConfiguration: BeamItAudioTrackConfiguration
    private(set) var captureTracks: [Int: BeamItAudioTrackStatus] = [:]
    private(set) var isActive = true
    @ObservationIgnored var onError: ((String) -> Void)?
    @ObservationIgnored var onEnded: (() -> Void)?

    private let socket: URLSessionWebSocketTask
    private let inputEngine = AVAudioEngine()
    private let outputEngine = AVAudioEngine()
    private var receiveTask: Task<Void, Never>?
    private var sendTask: Task<Void, Never>?
    private var microphoneEncoder: BeamItOpusEncoder?
    private var microphoneConverter: AVAudioConverter?
    private var microphoneOutputFormat: AVAudioFormat?
    private var microphonePendingSamples: [Float] = []
    private var microphoneSequence: UInt64 = 0
    private var microphoneDiscontinuity = true
    private var microphoneTapInstalled = false
    private var outgoingPackets: BeamItAudioSendQueue
    private var incomingTracks: [UInt32: IncomingTrack] = [:]

    init(offer: BeamItAudioOffer, session: URLSession) {
        audioID = offer.audioID
        microphoneConfiguration = offer.microphone
        outgoingPackets = BeamItAudioSendQueue(configuration: offer.microphone.opus)
        socket = session.webSocketTask(with: offer.url)
        socket.maximumMessageSize = BeamItAudioPacket.maxPacketLength
    }

    func open() async throws {
        socket.resume()
        receiveTask = Task { await receivePackets() }
        do {
            try await startMicrophone()
        } catch {
            close()
            throw error
        }
    }

    func addCaptureTrack(_ configuration: BeamItAudioTrackConfiguration, source: BeamItAudioSource) throws {
        try validateCaptureSource(source)
        guard configuration.trackID > 0, configuration.codec == "opus",
              configuration.trackID <= Int(UInt32.max),
              incomingTracks[UInt32(configuration.trackID)] == nil else {
            throw BeamItAudioError.invalidPacket
        }
        try configuration.opus.validate()
        let format = AVAudioFormat(
            standardFormatWithSampleRate: Double(configuration.opus.sampleRate),
            channels: AVAudioChannelCount(configuration.opus.channels)
        )
        guard let format else {
            throw BeamItAudioError.deviceFailure("The offered PCM format is unavailable.")
        }
        let player = AVAudioPlayerNode()
        outputEngine.attach(player)
        outputEngine.connect(player, to: outputEngine.mainMixerNode, format: format)
        incomingTracks[UInt32(configuration.trackID)] = try IncomingTrack(
            configuration: configuration,
            player: player,
            format: format
        )
        captureTracks[configuration.trackID] = BeamItAudioTrackStatus(
            id: configuration.trackID,
            title: source.title,
            source: source
        )
        do {
            if !outputEngine.isRunning { try outputEngine.start() }
            player.play()
        } catch {
            removeCaptureTrack(configuration.trackID)
            throw BeamItAudioError.deviceFailure(error.localizedDescription)
        }
    }

    func validateCaptureSource(_ source: BeamItAudioSource) throws {
        guard isActive, captureTracks.count < 8 else { throw BeamItAudioError.invalidPacket }
        let activeSources = captureTracks.values.map(\.source)
        let hasDesktop = activeSources.contains(.desktop)
        if source == .desktop ? !activeSources.isEmpty : hasDesktop {
            throw BeamItAudioError.deviceFailure("Desktop audio is exclusive with application audio tracks.")
        }
        if let pid = source.pid, activeSources.contains(where: { $0.pid == pid }) {
            throw BeamItAudioError.deviceFailure("This application already has an audio capture track.")
        }
    }

    func removeCaptureTrack(_ trackID: Int) {
        guard let track = incomingTracks.removeValue(forKey: UInt32(trackID)) else { return }
        track.player.stop()
        outputEngine.disconnectNodeOutput(track.player)
        outputEngine.detach(track.player)
        captureTracks.removeValue(forKey: trackID)
        if incomingTracks.isEmpty {
            outputEngine.stop()
        }
    }

    func close() {
        guard isActive else { return }
        isActive = false
        receiveTask?.cancel()
        receiveTask = nil
        sendTask?.cancel()
        sendTask = nil
        outgoingPackets = BeamItAudioSendQueue(configuration: microphoneConfiguration.opus)
        socket.cancel(with: .normalClosure, reason: nil)
        if microphoneTapInstalled {
            inputEngine.inputNode.removeTap(onBus: 0)
            microphoneTapInstalled = false
        }
        inputEngine.stop()
        for track in incomingTracks.values { track.player.stop() }
        incomingTracks.removeAll()
        captureTracks.removeAll()
        outputEngine.stop()
        microphoneEncoder = nil
        microphoneConverter = nil
        onError = nil
        onEnded?()
        onEnded = nil
    }

    func handleHostStopped(trackID: Int, reason: String?, message: String?) {
        removeCaptureTrack(trackID)
        if let reason, reason != "requested" {
            onError?(message ?? "Host audio track stopped: \(reason).")
        }
    }

    private func startMicrophone() async throws {
        let permission: Bool
        switch AVCaptureDevice.authorizationStatus(for: .audio) {
        case .authorized:
            permission = true
        case .notDetermined:
            permission = await AVCaptureDevice.requestAccess(for: .audio)
        default:
            permission = false
        }
        guard permission else { throw BeamItAudioError.microphonePermissionDenied }

        let configuration = microphoneConfiguration.opus
        let outputFormat = AVAudioFormat(
            commonFormat: .pcmFormatFloat32,
            sampleRate: Double(configuration.sampleRate),
            channels: AVAudioChannelCount(configuration.channels),
            interleaved: false
        )
        guard let outputFormat else {
            throw BeamItAudioError.deviceFailure("The microphone output format is unavailable.")
        }
        let input = inputEngine.inputNode
        let inputFormat = input.outputFormat(forBus: 0)
        guard inputFormat.sampleRate > 0, inputFormat.channelCount > 0,
              let converter = AVAudioConverter(from: inputFormat, to: outputFormat) else {
            throw BeamItAudioError.deviceFailure("The microphone format could not be converted.")
        }
        microphoneEncoder = try BeamItOpusEncoder(configuration: configuration)
        microphoneConverter = converter
        microphoneOutputFormat = outputFormat

        input.installTap(onBus: 0, bufferSize: 1_024, format: inputFormat) { [weak self] buffer, _ in
            guard let copy = Self.copy(buffer) else { return }
            Task { @MainActor [weak self] in
                self?.convertMicrophoneBuffer(copy)
            }
        }
        microphoneTapInstalled = true
        do {
            try inputEngine.start()
        } catch {
            input.removeTap(onBus: 0)
            throw BeamItAudioError.deviceFailure(error.localizedDescription)
        }
    }

    private func convertMicrophoneBuffer(_ input: AVAudioPCMBuffer) {
        guard isActive, let converter = microphoneConverter,
              let outputFormat = microphoneOutputFormat,
              let encoder = microphoneEncoder else { return }
        let capacity = AVAudioFrameCount(
            ceil(Double(input.frameLength) * outputFormat.sampleRate / input.format.sampleRate) + 64
        )
        guard let output = AVAudioPCMBuffer(pcmFormat: outputFormat, frameCapacity: capacity) else {
            microphoneDiscontinuity = true
            return
        }
        var suppliedInput = false
        var conversionError: NSError?
        let status = converter.convert(to: output, error: &conversionError) { _, inputStatus in
            guard !suppliedInput else {
                inputStatus.pointee = .noDataNow
                return nil
            }
            suppliedInput = true
            inputStatus.pointee = .haveData
            return input
        }
        guard status != .error, conversionError == nil,
              let channels = output.floatChannelData else {
            microphoneDiscontinuity = true
            if let conversionError { onError?(conversionError.localizedDescription) }
            return
        }

        let channelCount = Int(output.format.channelCount)
        microphonePendingSamples.reserveCapacity(
            microphonePendingSamples.count + Int(output.frameLength) * channelCount
        )
        for frame in 0..<Int(output.frameLength) {
            for channel in 0..<channelCount {
                microphonePendingSamples.append(channels[channel][frame])
            }
        }
        let config = microphoneConfiguration.opus
        while microphonePendingSamples.count >= config.sampleCount {
            let samples = Array(microphonePendingSamples.prefix(config.sampleCount))
            microphonePendingSamples.removeFirst(config.sampleCount)
            do {
                let payload = try encoder.encode(samples)
                let packet = try BeamItAudioPacket.encode(
                    payload: payload,
                    trackID: UInt32(microphoneConfiguration.trackID),
                    sequence: microphoneSequence,
                    frameSize: UInt32(config.frameSize),
                    discontinuity: microphoneDiscontinuity
                )
                microphoneSequence += 1
                microphoneDiscontinuity = false
                enqueueOutgoingPacket(packet)
            } catch {
                microphoneDiscontinuity = true
                onError?(error.localizedDescription)
            }
        }
    }

    private func enqueueOutgoingPacket(_ packet: Data) {
        outgoingPackets.append(packet)
        sendNextPacket()
    }

    private func sendNextPacket() {
        guard isActive, sendTask == nil, !outgoingPackets.isEmpty else { return }
        sendTask = Task {
            do {
                while isActive, !outgoingPackets.isEmpty {
                    if let deadline = outgoingPackets.nextSendTime {
                        try await ContinuousClock().sleep(until: deadline)
                    }
                    try Task.checkCancellation()
                    guard isActive else { break }
                    // Dequeue after sleeping so overflow can discard stale waiting audio.
                    if let packet = outgoingPackets.take(at: .now) {
                        try await socket.send(.data(packet))
                    }
                }
                sendTask = nil
            } catch {
                sendTask = nil
                finish(error)
            }
        }
    }

    private func receivePackets() async {
        do {
            while !Task.isCancelled, isActive {
                let message = try await socket.receive()
                guard case .data(let packet) = message else { throw BeamItAudioError.invalidPacket }
                try receivePacket(packet)
            }
        } catch {
            guard isActive else { return }
            finish(error)
        }
    }

    private func receivePacket(_ packet: Data) throws {
        let frame = try BeamItAudioPacket.decode(packet)
        guard let track = incomingTracks[frame.trackID] else { return }
        let config = track.configuration.opus
        guard frame.frameSize == config.frameSize,
              frame.sequence <= UInt64.max / UInt64(config.frameSize),
              frame.sampleTimestamp == frame.sequence * UInt64(config.frameSize),
              track.lastSequence.map({ $0 < UInt64.max && frame.sequence > $0 }) ?? true else {
            throw BeamItAudioError.invalidPacket
        }
        let gap = track.lastSequence.map { $0 == UInt64.max || frame.sequence != $0 + 1 } ?? (frame.sequence != 0)
        let samples = try track.decoder.decode(frame.payload, discontinuity: frame.discontinuity || gap)
        track.lastSequence = frame.sequence
        guard let buffer = makeBuffer(samples, format: track.format) else {
            throw BeamItAudioError.deviceFailure("Decoded audio could not be played.")
        }
        if track.pendingBuffers.count >= 3 {
            track.pendingBuffers.removeFirst()
        }
        track.pendingBuffers.append(buffer)
        scheduleNext(track)
    }

    private func scheduleNext(_ track: IncomingTrack) {
        guard !track.isPlaying, let buffer = track.pendingBuffers.first else { return }
        track.pendingBuffers.removeFirst()
        track.isPlaying = true
        track.player.scheduleBuffer(buffer, completionCallbackType: .dataPlayedBack) { [weak self, weak track] _ in
            Task { @MainActor [weak self, weak track] in
                guard let self, let track, self.isActive else { return }
                track.isPlaying = false
                self.scheduleNext(track)
            }
        }
        if !track.player.isPlaying { track.player.play() }
    }

    private func makeBuffer(_ samples: [Float], format: AVAudioFormat) -> AVAudioPCMBuffer? {
        let channels = Int(format.channelCount)
        guard channels > 0, samples.count % channels == 0,
              let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: AVAudioFrameCount(samples.count / channels)),
              let channelData = buffer.floatChannelData else { return nil }
        buffer.frameLength = AVAudioFrameCount(samples.count / channels)
        for frame in 0..<Int(buffer.frameLength) {
            for channel in 0..<channels {
                channelData[channel][frame] = samples[frame * channels + channel]
            }
        }
        return buffer
    }

    private func finish(_ error: Error) {
        guard isActive else { return }
        onError?(error.localizedDescription)
        close()
    }

    private static func copy(_ buffer: AVAudioPCMBuffer) -> AVAudioPCMBuffer? {
        guard let copy = AVAudioPCMBuffer(pcmFormat: buffer.format, frameCapacity: buffer.frameLength) else {
            return nil
        }
        copy.frameLength = buffer.frameLength
        let source = UnsafeMutableAudioBufferListPointer(UnsafeMutablePointer(mutating: buffer.audioBufferList))
        let destination = UnsafeMutableAudioBufferListPointer(copy.mutableAudioBufferList)
        guard source.count == destination.count else { return nil }
        for index in source.indices {
            guard let sourceData = source[index].mData, let destinationData = destination[index].mData else {
                return nil
            }
            memcpy(destinationData, sourceData, Int(source[index].mDataByteSize))
            destination[index].mDataByteSize = source[index].mDataByteSize
        }
        return copy
    }

    private final class IncomingTrack {
        let configuration: BeamItAudioTrackConfiguration
        let decoder: BeamItOpusDecoder
        let player: AVAudioPlayerNode
        let format: AVAudioFormat
        var lastSequence: UInt64?
        var pendingBuffers: [AVAudioPCMBuffer] = []
        var isPlaying = false

        init(configuration: BeamItAudioTrackConfiguration, player: AVAudioPlayerNode,
             format: AVAudioFormat) throws {
            self.configuration = configuration
            decoder = try BeamItOpusDecoder(configuration: configuration.opus)
            self.player = player
            self.format = format
        }
    }
}
