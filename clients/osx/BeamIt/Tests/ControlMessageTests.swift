import Foundation

@main
struct ControlMessageTests {
    static func main() throws {
        // Ten MiB of content fits the clipboard budget, but escaping makes the wire reply larger.
        let content = String(repeating: "\u{0001}", count: 10 * 1024 * 1024)
        let reply = String(decoding: try JSONSerialization.data(withJSONObject: [
            "type": "clipboard", "id": 1, "text": content
        ]), as: UTF8.self)
        precondition(reply.utf8.count > BeamItControlMessage.sendLimit)
        precondition(reply.utf8.count < BeamItControlMessage.receiveLimit)
        let decoded = try BeamItControlMessage.decode(reply, authenticated: true)
        precondition(decoded["text"] as? String == content)

        do {
            _ = try BeamItControlMessage.encode([
                "type": "set_clipboard", "text": content
            ], authenticated: true)
            preconditionFailure("Oversize upload must be rejected before socket send")
        } catch {
            precondition(error.localizedDescription.contains("clipboard is too large"))
        }
        // A transfer error must not prevent the next control message from being processed.
        let rejected = try BeamItControlMessage.decode(
            #"{"type":"error","id":1,"code":"invalid_request","message":"clipboard too big"}"#,
            authenticated: true
        )
        precondition(rejected["type"] as? String == "error")
        let pong = try BeamItControlMessage.decode(#"{"type":"pong","id":2}"#, authenticated: true)
        precondition(pong["type"] as? String == "pong")
        let ping = try BeamItControlMessage.encode(["type": "ping", "id": 3], authenticated: true)
        precondition(ping.contains("ping"))

        do {
            _ = try BeamItControlMessage.decode(reply, authenticated: false)
            preconditionFailure("Handshake must retain the 64 KiB limit")
        } catch {}
        print("Control message size/error checks passed.")
    }
}
