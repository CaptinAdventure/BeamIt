import CommonCrypto
import CryptoKit
import Foundation
import Security

final class BeamItConnection {
    private struct PendingRequest {
        let expectedType: String
        let continuation: CheckedContinuation<[String: Any], Error>
        var sentAt: TimeInterval?
    }

    private let session: URLSession
    private let task: URLSessionWebSocketTask
    private let controlURL: URL
    private var nextRequestID = 1
    private var pendingRequests: [Int: PendingRequest] = [:]
    private var receiveTask: Task<Void, Never>?
    private var clockTask: Task<Void, Never>?
    let clockEstimate = BeamItClockEstimate()
    private var streams: [String: BeamItVideoStream] = [:]
    private var openingStreamAttention: [String: Set<String>] = [:]
    private var isDisconnected = false
    private var isAuthenticated = false
    lazy var clipboardSync = BeamItClipboardSync(connection: self)
    lazy var popups = BeamItPopups(connection: self)
    let hostWindows = BeamItHostWindowList()
    var onStreamStopped: ((String, String?) -> Void)?
    var onDisconnect: ((Error) -> Void)?

    private init(session: URLSession, task: URLSessionWebSocketTask, controlURL: URL) {
        self.session = session
        self.task = task
        self.controlURL = controlURL
    }

    static func connect(
        serverURL: String,
        password: String,
        certificateFingerprint: String,
        trustAnyCertificate: Bool
    ) async throws -> BeamItConnection {
        let pin = normalizedFingerprint(certificateFingerprint)
        guard trustAnyCertificate || pin != nil else {
            throw ConnectionError.invalidFingerprint
        }

        let controlURL = try makeControlURL(serverURL)
        let delegate = CertificatePinDelegate(fingerprint: pin, trustAnyCertificate: trustAnyCertificate)
        let configuration = URLSessionConfiguration.ephemeral
        configuration.waitsForConnectivity = false
        configuration.timeoutIntervalForRequest = 15
        let session = URLSession(configuration: configuration, delegate: delegate, delegateQueue: nil)
        var request = URLRequest(url: controlURL)
        request.timeoutInterval = 15

        let task = session.webSocketTask(with: request)
        task.maximumMessageSize = BeamItControlMessage.handshakeLimit
        let connection = BeamItConnection(session: session, task: task, controlURL: controlURL)
        task.resume()

        do {
            try await connection.authenticate(password: password)
            connection.isAuthenticated = true
            BeamItClipboardMenu.shared.sync = connection.clipboardSync
            task.maximumMessageSize = BeamItControlMessage.receiveLimit
            connection.startControlReceiver()
            connection.startClockSampling()
            return connection
        } catch {
            connection.disconnect()
            throw error
        }
    }

    func startStream(source: BeamItSource) async throws -> BeamItVideoStream {
        let sourceObject: [String: Any]
        switch source {
        case .desktopDXGI:
            sourceObject = ["kind": "desktop", "capture": "dxgi"]
        case .desktopWGC:
            sourceObject = ["kind": "desktop", "capture": "wgc"]
        case .application(let pid):
            sourceObject = ["kind": "application", "pid": pid]
        case .window(let hwnd):
            sourceObject = ["kind": "window", "hwnd": hwnd]
        }

        let response = try await request(
            type: "start_stream",
            fields: ["source": sourceObject],
            expectedType: "stream_offer"
        )
        let data = try JSONSerialization.data(withJSONObject: response)
        let offer = try JSONDecoder().decode(BeamItStreamOffer.self, from: data)
        guard offer.tokenExpiresInMS > 0, offer.video.width > 0, offer.video.height > 0,
              ["h264", "h265"].contains(offer.video.codec)
        else {
            throw ConnectionError.invalidMessage
        }
        guard offer.url.scheme?.lowercased() == "wss",
              offer.url.host?.lowercased() == controlURL.host?.lowercased(),
              offer.url.port == controlURL.port
        else {
            throw ConnectionError.invalidStreamURL
        }

        let stream = BeamItVideoStream(offer: offer, session: session, control: self)
        streams[offer.streamID] = stream
        for hwnd in openingStreamAttention.removeValue(forKey: offer.streamID) ?? [] {
            stream.updateAttention(hwnd: hwnd, requesting: true)
        }
        do {
            try await stream.open()
            return stream
        } catch {
            try? await stopStream(offer.streamID)
            streams.removeValue(forKey: offer.streamID)
            throw error
        }
    }

    func stopStream(_ streamID: String) async throws {
        guard streams[streamID] != nil else { return }
        defer { streams.removeValue(forKey: streamID)?.close() }
        _ = try await request(
            type: "stop_stream",
            fields: ["stream_id": streamID],
            expectedType: "stream_stopped"
        )
    }

    func getClipboard() async throws -> [String: Any] {
        try await request(type: "get_clipboard", fields: ["formats": ["text", "html", "image"]], expectedType: "clipboard")
    }

    func getWindowIcon(_ hwnd: String) async throws -> Data {
        let reply = try await request(
            type: "get_window_icon", fields: ["hwnd": hwnd, "size": 256], expectedType: "window_icon"
        )
        guard reply["hwnd"] as? String == hwnd,
              reply["mime_type"] as? String == "image/png",
              let width = reply["width"] as? Int, (1...256).contains(width),
              let height = reply["height"] as? Int, (1...256).contains(height),
              let encoded = reply["data"] as? String, encoded.utf8.count <= 1024 * 1024,
              let data = Data(base64Encoded: encoded),
              data.starts(with: [137, 80, 78, 71, 13, 10, 26, 10]) else {
            throw ConnectionError.invalidMessage
        }
        return data
    }

    func setClipboard(_ fields: [String: Any]) async throws {
        _ = try await request(type: "set_clipboard", fields: fields, expectedType: "clipboard_set")
    }

    func setStreamRate(_ streamID: String, bitrate: Int, frameRate: Int, peak: Int? = nil) async throws {
        var fields: [String: Any] = ["stream_id": streamID, "bitrate_kbps": bitrate, "frame_rate": frameRate]
        if let peak { fields["max_bitrate_kbps"] = peak }
        let reply = try await request(
            type: "set_stream_rate",
            fields: fields,
            expectedType: "stream_rate"
        )
        guard reply["stream_id"] as? String == streamID,
              let bitrate = reply["bitrate_kbps"] as? Int, (1...1_000_000).contains(bitrate),
              let frameRate = reply["frame_rate"] as? Int, (1...240).contains(frameRate),
              let generation = reply["generation"] as? Int else {
            throw ConnectionError.invalidMessage
        }
        let effectivePeak = reply["max_bitrate_kbps"] as? Int
        if let effectivePeak, !(1...4_000_000).contains(effectivePeak) {
            throw ConnectionError.invalidMessage
        }
        streams[streamID]?.updateRate(bitrate: bitrate, frameRate: frameRate, generation: generation, peak: effectivePeak)
    }

    func setMouseMode(_ streamID: String, mode: String) async throws {
        let reply = try await request(
            type: "set_mouse_mode",
            fields: ["stream_id": streamID, "mode": mode],
            expectedType: "mouse_mode"
        )
        guard reply["stream_id"] as? String == streamID,
              reply["mode"] as? String == mode else {
            throw ConnectionError.invalidMessage
        }
    }

    func requestKeyFrame(_ streamID: String) {
        Task {
            try? await sendJSON(["type": "request_key_frame", "stream_id": streamID])
        }
    }

    /// Sends a fire-and-forget input message (PROTOCOL.md §5.9). Sending synchronously from the main
    /// thread keeps the socket's send queue in event order.
    func sendInput(_ message: [String: Any]) {
        guard !isDisconnected,
              let data = try? JSONSerialization.data(withJSONObject: message),
              let text = String(data: data, encoding: .utf8)
        else { return }
        task.send(.string(text)) { @Sendable error in
            if let error {
                NSLog("BeamIt input send failed: %@", error.localizedDescription)
            }
        }
    }

    func disconnect() {
        isDisconnected = true
        clockTask?.cancel()
        clockTask = nil
        popups.stop()
        clipboardSync.stop()
        receiveTask?.cancel()
        receiveTask = nil
        for stream in streams.values {
            stream.close()
        }
        streams.removeAll()
        openingStreamAttention.removeAll()
        failPendingRequests(ConnectionError.disconnected)
        task.cancel(with: .normalClosure, reason: nil)
        session.invalidateAndCancel()
    }

    private func authenticate(password: String) async throws {
        let hello = try await receiveJSON()
        guard hello["type"] as? String == "hello",
              hello["protocol"] as? Int == 1,
              let auth = hello["auth"] as? [String: Any],
              auth["mechanism"] as? String == "pbkdf2-sha256-hmac",
              auth["iterations"] as? Int == 600_000,
              let saltText = auth["salt"] as? String,
              let serverNonceText = auth["server_nonce"] as? String,
              let salt = Self.decodeBase64URL(saltText), !salt.isEmpty,
              let serverNonce = Self.decodeBase64URL(serverNonceText), serverNonce.count == 32
        else {
            throw ConnectionError.invalidChallenge
        }

        var clientNonce = Data(count: 32)
        let randomStatus = clientNonce.withUnsafeMutableBytes { bytes in
            SecRandomCopyBytes(kSecRandomDefault, bytes.count, bytes.baseAddress!)
        }
        guard randomStatus == errSecSuccess else {
            throw ConnectionError.randomGenerationFailed
        }

        var key = try Self.deriveKey(password: password, salt: salt)
        let clientProof = Self.hmac(key: key, message: Self.proofMessage(role: "client", serverNonce: serverNonce, clientNonce: clientNonce))
        let serverProof = Self.hmac(key: key, message: Self.proofMessage(role: "server", serverNonce: serverNonce, clientNonce: clientNonce))
        key.resetBytes(in: 0..<key.count)
        try await sendJSON([
            "type": "authenticate",
            "protocol": 1,
            "client_nonce": Self.encodeBase64URL(clientNonce),
            "proof": Self.encodeBase64URL(clientProof)
        ])
        clientNonce.resetBytes(in: 0..<clientNonce.count)

        let reply = try await receiveJSON()
        if reply["type"] as? String == "error" {
            throw ConnectionError.hostRejected(reply["message"] as? String ?? "The host rejected authentication.")
        }
        guard reply["type"] as? String == "authenticated",
              let proofText = reply["server_proof"] as? String,
              let receivedProof = Self.decodeBase64URL(proofText),
              Self.constantTimeEqual(receivedProof, serverProof)
        else {
            throw ConnectionError.serverProofMismatch
        }
    }

    private func request(type: String, fields: [String: Any], expectedType: String) async throws -> [String: Any] {
        let requestID = nextRequestID
        nextRequestID = nextRequestID == Int.max ? 1 : nextRequestID + 1
        var message = fields
        message["type"] = type
        message["id"] = requestID
        return try await withCheckedThrowingContinuation { continuation in
            pendingRequests[requestID] = PendingRequest(expectedType: expectedType, continuation: continuation)
            Task {
                do {
                    pendingRequests[requestID]?.sentAt = ProcessInfo.processInfo.systemUptime
                    try await sendJSON(message)
                } catch {
                    pendingRequests.removeValue(forKey: requestID)?.continuation.resume(throwing: error)
                }
            }
            Task {
                try? await Task.sleep(for: .seconds(10))
                pendingRequests.removeValue(forKey: requestID)?.continuation.resume(throwing: ConnectionError.requestTimedOut)
            }
        }
    }

    private func startControlReceiver() {
        receiveTask = Task {
            do {
                while !Task.isCancelled {
                    let message = try await receiveJSON()
                    let receivedAt = ProcessInfo.processInfo.systemUptime
                    if message["type"] as? String == "stream_stats" {
                        do {
                            let data = try JSONSerialization.data(withJSONObject: message)
                            let report = try JSONDecoder().decode(BeamItHostStatsReport.self, from: data)
                            for stats in report.streams {
                                streams[stats.stream_id]?.diagnostics.updateHostStats(stats, hostTime: report.host_time_us)
                            }
                        } catch {
                            NSLog("BeamIt invalid stream statistics: %@", error.localizedDescription)
                        }
                        continue
                    }
                    if let type = message["type"] as? String,
                       ["window_opened", "window_changed", "window_closed"].contains(type),
                       let object = message["window"] {
                        let data = try JSONSerialization.data(withJSONObject: object)
                        let window = try JSONDecoder().decode(BeamItHostWindow.self, from: data)
                        popups.update(window, closed: type == "window_closed")
                        hostWindows.update(window, closed: type == "window_closed")
                        continue
                    }
                    if message["type"] as? String == "clipboard_changed" {
                        clipboardSync.hostClipboardChanged(message)
                        continue
                    }
                    if message["type"] as? String == "stream_offer",
                       let streamID = message["stream_id"] as? String {
                        openingStreamAttention[streamID] = []
                    }
                    if message["type"] as? String == "attention" {
                        guard let streamID = message["stream_id"] as? String,
                              let state = message["state"] as? String, ["set", "clear"].contains(state),
                              let window = message["window"] as? [String: Any],
                              let hwnd = window["hwnd"] as? String else {
                            throw ConnectionError.invalidMessage
                        }
                        if let stream = streams[streamID] {
                            stream.updateAttention(hwnd: hwnd, requesting: state == "set")
                        } else if var windows = openingStreamAttention[streamID] {
                            if state == "set" { windows.insert(hwnd) } else { windows.remove(hwnd) }
                            openingStreamAttention[streamID] = windows
                        }
                        continue
                    }
                    if message["type"] as? String == "stream_stopped",
                       let streamID = message["stream_id"] as? String {
                        openingStreamAttention.removeValue(forKey: streamID)
                        streams.removeValue(forKey: streamID)?.handleStopped()
                        popups.streamStopped(streamID)
                        onStreamStopped?(streamID, message["reason"] as? String)
                    }
                    if message["type"] as? String == "stream_config",
                       let streamID = message["stream_id"] as? String,
                       let videoObject = message["video"],
                       let data = try? JSONSerialization.data(withJSONObject: videoObject),
                       let video = try? JSONDecoder().decode(BeamItVideo.self, from: data) {
                        streams[streamID]?.updateVideo(video)
                        continue
                    }

                    guard let id = message["id"] as? Int,
                          let pending = pendingRequests.removeValue(forKey: id)
                    else {
                        if message["type"] as? String == "error" {
                            // Input has no replies; its failures (e.g. focus_failed) arrive unsolicited.
                            NSLog(
                                "BeamIt host error %@: %@",
                                message["code"] as? String ?? "unknown",
                                message["message"] as? String ?? ""
                            )
                            if let streamID = message["stream_id"] as? String {
                                streams[streamID]?.onInputError?(
                                    message["message"] as? String ?? "The host rejected input."
                                )
                            }
                        }
                        continue
                    }
                    if message["type"] as? String == "error" {
                        pending.continuation.resume(throwing: ConnectionError.hostRejected(
                            message["message"] as? String ?? message["code"] as? String ?? "The host rejected the request."
                        ))
                    } else if message["type"] as? String != pending.expectedType {
                        pending.continuation.resume(throwing: ConnectionError.unexpectedReply(
                            message["type"] as? String ?? "unknown"
                        ))
                    } else {
                        if pending.expectedType == "pong", let sentAt = pending.sentAt,
                           let hostTime = message["host_time_us"] as? UInt64 {
                            clockEstimate.record(hostUS: hostTime, sent: sentAt, received: receivedAt)
                        }
                        pending.continuation.resume(returning: message)
                    }
                }
            } catch {
                guard !isDisconnected else { return }
                clockTask?.cancel()
                clockTask = nil
                clipboardSync.stop()
                popups.stop()
                failPendingRequests(error)
                onDisconnect?(error)
            }
        }
    }

    private func startClockSampling() {
        clockTask = Task { [weak self] in
            var count = 0
            while !Task.isCancelled {
                guard let self, !self.isDisconnected else { return }
                do {
                    _ = try await self.request(type: "ping", fields: [:], expectedType: "pong")
                } catch {
                    guard !Task.isCancelled, !self.isDisconnected else { return }
                    NSLog("BeamIt clock sampling failed: %@", error.localizedDescription)
                }
                count += 1
                do {
                    try await Task.sleep(for: count < 5 ? .milliseconds(200) : .seconds(5))
                } catch { return }
            }
        }
    }

    private func failPendingRequests(_ error: Error) {
        let pending = pendingRequests.values
        pendingRequests.removeAll()
        for request in pending {
            request.continuation.resume(throwing: error)
        }
    }

    private func sendJSON(_ message: [String: Any]) async throws {
        let text = try BeamItControlMessage.encode(message, authenticated: isAuthenticated)
        try await task.send(.string(text))
    }

    private func receiveJSON() async throws -> [String: Any] {
        let message = try await task.receive()
        guard case .string(let text) = message else {
            throw ConnectionError.invalidMessage
        }
        return try BeamItControlMessage.decode(text, authenticated: isAuthenticated)
    }

    private static func makeControlURL(_ value: String) throws -> URL {
        let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
        guard var components = URLComponents(string: trimmed),
              ["wss", "https"].contains(components.scheme?.lowercased() ?? ""),
              let host = components.host, !host.isEmpty,
              components.user == nil, components.password == nil
        else {
            throw ConnectionError.invalidServerURL
        }
        components.scheme = "wss"
        components.path = "/control"
        components.query = nil
        components.fragment = nil
        guard let url = components.url else {
            throw ConnectionError.invalidServerURL
        }
        return url
    }

    private static func normalizedFingerprint(_ fingerprint: String) -> String? {
        let normalized = fingerprint.filter { $0 != ":" && !$0.isWhitespace }.uppercased()
        guard normalized.count == 64, normalized.allSatisfy(\.isHexDigit) else { return nil }
        return normalized
    }

    private static func deriveKey(password: String, salt: Data) throws -> Data {
        let keyLength = 32
        var key = Data(count: keyLength)
        let status = password.withCString { passwordPointer in
            salt.withUnsafeBytes { saltBytes in
                key.withUnsafeMutableBytes { keyBytes in
                    CCKeyDerivationPBKDF(
                        CCPBKDFAlgorithm(kCCPBKDF2),
                        passwordPointer,
                        password.utf8.count,
                        saltBytes.baseAddress!.assumingMemoryBound(to: UInt8.self),
                        salt.count,
                        CCPseudoRandomAlgorithm(kCCPRFHmacAlgSHA256),
                        600_000,
                        keyBytes.baseAddress!.assumingMemoryBound(to: UInt8.self),
                        keyLength
                    )
                }
            }
        }
        guard status == kCCSuccess else {
            throw ConnectionError.keyDerivationFailed
        }
        return key
    }

    private static func proofMessage(role: String, serverNonce: Data, clientNonce: Data) -> Data {
        var message = Data("BeamIt v1 \(role)\0".utf8)
        message.append(serverNonce)
        message.append(clientNonce)
        return message
    }

    private static func hmac(key: Data, message: Data) -> Data {
        let authenticationCode = HMAC<SHA256>.authenticationCode(for: message, using: SymmetricKey(data: key))
        return Data(authenticationCode)
    }

    private static func encodeBase64URL(_ data: Data) -> String {
        data.base64EncodedString()
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: "=", with: "")
    }

    private static func decodeBase64URL(_ value: String) -> Data? {
        guard value.range(of: #"^[A-Za-z0-9_-]+$"#, options: .regularExpression) != nil,
              value.count % 4 != 1
        else {
            return nil
        }
        let base64 = value
            .replacingOccurrences(of: "-", with: "+")
            .replacingOccurrences(of: "_", with: "/")
        let padded = base64 + String(repeating: "=", count: (4 - base64.count % 4) % 4)
        guard let data = Data(base64Encoded: padded), encodeBase64URL(data) == value else { return nil }
        return data
    }

    private static func constantTimeEqual(_ lhs: Data, _ rhs: Data) -> Bool {
        guard lhs.count == rhs.count else { return false }
        return zip(lhs, rhs).reduce(UInt8(0)) { $0 | ($1.0 ^ $1.1) } == 0
    }

    private enum ConnectionError: LocalizedError {
        case invalidServerURL
        case invalidFingerprint
        case invalidChallenge
        case randomGenerationFailed
        case keyDerivationFailed
        case serverProofMismatch
        case hostRejected(String)
        case unexpectedReply(String)
        case invalidMessage
        case invalidStreamURL
        case requestTimedOut
        case disconnected

        var errorDescription: String? {
            switch self {
            case .invalidServerURL:
                return "Enter a WSS or HTTPS server URL without embedded credentials."
            case .invalidFingerprint:
                return "Enter the server's 64-digit SHA-256 certificate fingerprint."
            case .invalidChallenge:
                return "The host returned an unsupported authentication challenge."
            case .randomGenerationFailed:
                return "Secure random number generation failed."
            case .keyDerivationFailed:
                return "Password key derivation failed."
            case .serverProofMismatch:
                return "The host's authentication proof did not match."
            case .hostRejected(let message):
                return message
            case .unexpectedReply(let type):
                return "Unexpected host response: \(type)."
            case .invalidMessage:
                return "The host sent an invalid signaling message."
            case .invalidStreamURL:
                return "The host returned a stream URL outside the authenticated WSS endpoint."
            case .requestTimedOut:
                return "The host did not answer within 10 seconds."
            case .disconnected:
                return "The BeamIt control connection was closed."
            }
        }
    }
}
