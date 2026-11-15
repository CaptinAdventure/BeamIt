import CryptoKit
import Foundation
import Security
import Synchronization

nonisolated final class CertificatePinDelegate: NSObject, URLSessionWebSocketDelegate {
    enum WebSocketOpenError: Error, Equatable {
        case timeout, closed, alreadyOpening
    }

    private nonisolated final class PendingOpen: Sendable {
        private struct State {
            var result: Result<Void, Error>?
            var continuation: CheckedContinuation<Void, Error>?
        }
        private let state = Mutex(State())

        var isSuspended: Bool {
            state.withLock { $0.result == nil && $0.continuation != nil }
        }

        func finish(_ result: Result<Void, Error>) {
            let continuation = state.withLock { state -> CheckedContinuation<Void, Error>? in
                guard state.result == nil else { return nil }
                state.result = result
                let continuation = state.continuation
                state.continuation = nil
                return continuation
            }
            continuation?.resume(with: result)
        }

        func wait() async throws {
            try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
                let result = state.withLock { state -> Result<Void, Error>? in
                    if let result = state.result { return result }
                    precondition(state.continuation == nil)
                    state.continuation = continuation
                    return nil
                }
                if let result { continuation.resume(with: result) }
            }
        }
    }

    private let fingerprint: String?
    private let trustAnyCertificate: Bool
    private let openings = Mutex<[Int: PendingOpen]>([:])

    var pendingWebSocketOpenCount: Int { openings.withLock { $0.count } }
    var suspendedWebSocketOpenCount: Int {
        let pending = openings.withLock { Array($0.values) }
        return pending.filter { $0.isSuspended }.count
    }

    init(fingerprint: String?, trustAnyCertificate: Bool) {
        self.fingerprint = fingerprint
        self.trustAnyCertificate = trustAnyCertificate
    }

    /// Register before resume; opening means a successful upgrade, not a pong.
    /// Only explicitly registered tasks participate; other session tasks are unchanged.
    func openWebSocket(_ socket: URLSessionWebSocketTask, timeout: Duration = .seconds(10)) async throws {
        let opening = PendingOpen()
        let registered = openings.withLock { pending in
            guard pending[socket.taskIdentifier] == nil else { return false }
            pending[socket.taskIdentifier] = opening
            return true
        }
        guard registered else { throw WebSocketOpenError.alreadyOpening }
        defer { openings.withLock { _ = $0.removeValue(forKey: socket.taskIdentifier) } }

        do {
            try await withTaskCancellationHandler {
                try Task.checkCancellation()
                try await withThrowingTaskGroup(of: Void.self) { group in
                    group.addTask {
                        do {
                            try await Task.sleep(for: timeout)
                        } catch is CancellationError {
                            return
                        } catch {
                            opening.finish(.failure(error))
                            return
                        }
                        opening.finish(.failure(WebSocketOpenError.timeout))
                    }
                    defer { group.cancelAll() }
                    socket.resume()
                    try await opening.wait()
                }
                try Task.checkCancellation()
            } onCancel: {
                // Resume even if Foundation never delivers another task callback.
                opening.finish(.failure(CancellationError()))
                socket.cancel(with: .goingAway, reason: nil)
            }
        } catch {
            socket.cancel(with: .goingAway, reason: nil)
            throw error
        }
    }

    func urlSession(_ session: URLSession, webSocketTask: URLSessionWebSocketTask,
                    didOpenWithProtocol negotiatedProtocol: String?) {
        openings.withLock { $0[webSocketTask.taskIdentifier] }?.finish(.success(()))
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        openings.withLock { $0[task.taskIdentifier] }?.finish(.failure(error ?? WebSocketOpenError.closed))
    }

    func urlSession(_ session: URLSession, webSocketTask: URLSessionWebSocketTask,
                    didCloseWith closeCode: URLSessionWebSocketTask.CloseCode, reason: Data?) {
        openings.withLock { $0[webSocketTask.taskIdentifier] }?.finish(.failure(WebSocketOpenError.closed))
    }

    func urlSession(_ session: URLSession, didBecomeInvalidWithError error: Error?) {
        let pending = openings.withLock { Array($0.values) }
        for opening in pending { opening.finish(.failure(error ?? WebSocketOpenError.closed)) }
    }

    func urlSession(
        _ session: URLSession,
        didReceive challenge: URLAuthenticationChallenge,
        completionHandler: @escaping (URLSession.AuthChallengeDisposition, URLCredential?) -> Void
    ) {
        guard challenge.protectionSpace.authenticationMethod == NSURLAuthenticationMethodServerTrust,
              let trust = challenge.protectionSpace.serverTrust,
              let certificate = (SecTrustCopyCertificateChain(trust) as? [SecCertificate])?.first
        else {
            completionHandler(.cancelAuthenticationChallenge, nil)
            return
        }

        if !trustAnyCertificate {
            let certificateData = SecCertificateCopyData(certificate) as Data
            let presentedFingerprint = SHA256.hash(data: certificateData)
                .map { String(format: "%02X", $0) }
                .joined()
            guard presentedFingerprint == fingerprint else {
                completionHandler(.cancelAuthenticationChallenge, nil)
                return
            }
        }
        completionHandler(.useCredential, URLCredential(trust: trust))
    }
}
