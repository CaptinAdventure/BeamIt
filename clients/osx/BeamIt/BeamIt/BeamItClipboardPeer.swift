import Foundation

nonisolated struct BeamItClipboardHostFailure: LocalizedError, Sendable {
    let code: String
    let errorDescription: String?
    let fatal: Bool
}

// Clipboard-only connection boundary. Tests can exercise the real admission
// entry without opening a network session or initializing the user's board.
@MainActor
protocol BeamItClipboardPeer: AnyObject {
    func clipboardRequest(_ type: String, _ fields: [String: Any], reply: String) async throws -> [String: Any]
    func clipboardEvent(_ type: String, _ fields: [String: Any]) async throws
    func setClipboard(_ fields: [String: Any]) async throws
    func getClipboard() async throws -> [String: Any]
    func openBulkSocket(_ offeredURL: URL, transferID: String) async throws -> URLSessionWebSocketTask
}
