import AppKit

final class BeamItVideoStream {
    struct Video { let width = 1920; let height = 1080 }
    struct Offer {
        struct Source { let kind = "desktop" }
        let source = Source()
    }
    let video = Video()
    let offer = Offer()
    var clipboardSyncEnabled = true
    var isClosed = false
    var onInputError: ((String) -> Void)?
    var messages: [[String: Any]] = []
    func sendInput(_ type: String, _ fields: [String: Any] = [:]) {
        var message = fields
        message["type"] = type
        messages.append(message)
    }
    func setMouseMode(_ mode: String) async throws {}
    func uploadClipboardForPaste() async throws {}
}

@main
struct InputTests {
    static func key(_ code: UInt16, _ characters: String, flags: NSEvent.ModifierFlags = [],
                    type: NSEvent.EventType = .keyDown) -> NSEvent {
        NSEvent.keyEvent(
            with: type, location: .zero, modifierFlags: flags, timestamp: 0,
            windowNumber: 0, context: nil, characters: characters,
            charactersIgnoringModifiers: characters, isARepeat: false, keyCode: code
        )!
    }

    @MainActor
    static func main() {
        for (physical, normal, captured) in [
            (UInt16(55), "ControlLeft", "MetaLeft"),
            (54, "ControlRight", "MetaRight"),
            (59, "MetaLeft", "ControlLeft"),
            (62, "MetaRight", "ControlRight"),
            (56, "ShiftLeft", "ShiftLeft"),
            (60, "ShiftRight", "ShiftRight"),
            (58, "AltLeft", "AltLeft"),
            (61, "AltRight", "AltRight"),
        ] {
            precondition(BeamItInputHandler.modifierCode(physical, captured: false) == normal)
            precondition(BeamItInputHandler.modifierCode(physical, captured: true) == captured)
        }
        for (code, character) in [(UInt16(13), "w"), (12, "q")] {
            let event = key(code, character, flags: .command)
            precondition(BeamItInputHandler.isLocalShortcut(event, captured: false))
            precondition(!BeamItInputHandler.isLocalShortcut(event, captured: true))
        }
        let paste = key(9, "v", flags: .command)
        precondition(BeamItInputHandler.isClipboardPaste(paste, captured: false))
        precondition(!BeamItInputHandler.isClipboardPaste(paste, captured: true))
        precondition(!BeamItInputHandler.isClipboardPaste(key(9, "v", flags: .control), captured: false))
        precondition(BeamItInputHandler.isCaptureReleaseShortcut(key(5, "g", flags: [.control, .option])))
        for flags: NSEvent.ModifierFlags in [.control, .option, [.command, .option],
                                             [.control, .option, .shift], [.control, .option, .command]] {
            precondition(!BeamItInputHandler.isCaptureReleaseShortcut(key(5, "g", flags: flags)))
        }
        precondition(!BeamItInputHandler.isCaptureReleaseShortcut(
            key(5, "g", flags: [.control, .option], type: .keyUp)
        ))
        precondition(!BeamItInputHandler.isCaptureReleaseShortcut(key(4, "h", flags: [.control, .option])))

        let stream = BeamItVideoStream()
        let handler = BeamItInputHandler(stream: stream)
        precondition(handler.handleKeyEvent(key(0, "a")))
        precondition(handler.handleKeyEvent(key(0, "a", type: .keyUp)))
        precondition(stream.messages.count == 2)
        precondition(stream.messages[0]["code"] as? String == "KeyA")
        precondition(stream.messages[0]["down"] as? Bool == true)
        precondition(stream.messages[1]["down"] as? Bool == false)
        precondition(!handler.handleKeyEvent(key(13, "w", flags: .command)))
        handler.releaseAll()
        precondition(stream.messages.last?["type"] as? String == "release_all")
        print("Input checks passed")
    }
}
