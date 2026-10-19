import Foundation

enum BeamItControlMessage {
    static let handshakeLimit = 64 * 1024
    static let sendLimit = 16 * 1024 * 1024
    // Clipboard content is limited before escaping. A JSON string can require six bytes per
    // content byte (\u0000); leave room for the reply envelope as well.
    static let receiveLimit = 64 * 1024 * 1024

    static func encode(_ message: [String: Any], authenticated: Bool) throws -> String {
        let data = try JSONSerialization.data(withJSONObject: message, options: [.sortedKeys])
        guard data.count <= (authenticated ? sendLimit : handshakeLimit) else {
            throw MessageError(message["type"] as? String == "set_clipboard"
                ? "The clipboard is too large after JSON encoding (16 MiB limit). Nothing was sent; streams remain connected."
                : "The outgoing signaling message exceeds the size limit.")
        }
        return String(decoding: data, as: UTF8.self)
    }

    static func decode(_ text: String, authenticated: Bool) throws -> [String: Any] {
        guard text.utf8.count <= (authenticated ? receiveLimit : handshakeLimit),
              let object = try JSONSerialization.jsonObject(with: Data(text.utf8)) as? [String: Any],
              let type = object["type"] as? String,
              !authenticated || type == "clipboard" || text.utf8.count <= sendLimit else {
            throw MessageError("The host sent an invalid signaling message.")
        }
        return object
    }

    private struct MessageError: LocalizedError {
        let errorDescription: String?
        init(_ message: String) { errorDescription = message }
    }
}
