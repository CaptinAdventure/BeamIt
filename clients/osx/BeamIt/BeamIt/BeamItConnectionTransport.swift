import Foundation

/// Shared pinned session state for the application connection. The final bulk
/// entry is inherited, not wrapped, so native tests execute its production body.
@MainActor
class BeamItConnectionTransport {
    nonisolated enum BulkSocketError: LocalizedError {
        case invalidBulkURL
        var errorDescription: String? {
            "The host returned a bulk URL outside the authenticated WSS endpoint."
        }
    }

    let session: URLSession
    let controlURL: URL
    private let certificateDelegate: CertificatePinDelegate

    init(session: URLSession, certificateDelegate: CertificatePinDelegate, controlURL: URL) {
        self.session = session
        self.certificateDelegate = certificateDelegate
        self.controlURL = controlURL
    }

    final func openBulkSocket(_ offeredURL: URL, transferID: String) async throws -> URLSessionWebSocketTask {
        let components = URLComponents(url: offeredURL, resolvingAgainstBaseURL: false)
        let pathComponents = offeredURL.path.split(separator: "/")
        let queryItems = components?.queryItems ?? []
        let validBase64URL: (String, Int) -> Bool = { value, count in
            value.utf8.count == count && value.utf8.allSatisfy {
                (65...90).contains($0) || (97...122).contains($0) ||
                    (48...57).contains($0) || $0 == 45 || $0 == 95
            }
        }
        guard offeredURL.scheme?.lowercased() == "wss",
              offeredURL.host?.lowercased() == controlURL.host?.lowercased(),
              offeredURL.port == controlURL.port,
              offeredURL.user == nil, offeredURL.password == nil, offeredURL.fragment == nil,
              components?.percentEncodedPath == offeredURL.path,
              pathComponents.count == 2, pathComponents[0] == "bulk",
              String(pathComponents[1]) == transferID, validBase64URL(transferID, 22),
              queryItems.count == 1, queryItems[0].name == "token",
              queryItems[0].value.map({ validBase64URL($0, 43) }) == true else {
            throw BulkSocketError.invalidBulkURL
        }
        _ = try BeamItClipboardBulk.decodeID(transferID, bytes: 16)
        _ = try BeamItClipboardBulk.decodeID(queryItems[0].value!, bytes: 32)
        let socket = session.webSocketTask(with: offeredURL)
        socket.maximumMessageSize = 40 + 256 * 1024
        do {
            // The host can send binary data before pong. Do not gate the caller's
            // first receive on a pong queued behind that data.
            try await certificateDelegate.openWebSocket(socket)
        } catch CertificatePinDelegate.WebSocketOpenError.timeout {
            throw BeamItClipboardFailure("bulk_open_timeout")
        } catch CertificatePinDelegate.WebSocketOpenError.closed {
            throw BeamItClipboardFailure("bulk_open_closed")
        }
        return socket
    }
}
