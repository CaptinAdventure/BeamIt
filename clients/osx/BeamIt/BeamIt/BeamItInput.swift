import AppKit

/// Translates AppKit mouse and keyboard events into PROTOCOL.md §5.9 input messages for one stream.
///
/// Keys are sent as physical `KeyboardEvent.code` names, so the host keyboard layout decides the
/// produced character. Mac shortcuts are kept familiar: Command sends Ctrl and Control sends the
/// Windows key. ⌘W and ⌘Q stay local to the Mac client.
final class BeamItInputHandler {
    private let stream: BeamItVideoStream
    private var heldKeys = Set<String>()
    private var heldButtons = Set<String>()
    private var lastPosition: CGPoint?
    private var wheelRemainder = CGPoint.zero
    private var pasteTask: Task<Void, Never>?

    init(stream: BeamItVideoStream) {
        self.stream = stream
    }

    // MARK: Mouse

    func mouseMoved(_ event: NSEvent, in view: NSView) {
        sendPosition(event, in: view)
    }

    func mouseButton(_ event: NSEvent, in view: NSView, down: Bool) {
        guard let button = Self.buttonName(event) else { return }
        sendPosition(event, in: view)
        if down {
            heldButtons.insert(button)
        } else if heldButtons.remove(button) == nil {
            return
        }
        stream.sendInput("mouse_button", ["button": button, "down": down])
    }

    func scrollWheel(_ event: NSEvent, in view: NSView) {
        sendPosition(event, in: view)
        // AppKit deltas are positive when content moves down/right; the protocol uses the DOM
        // sign (positive scrolls down/right). Precise (trackpad) deltas are in points, others in lines.
        let scale = event.hasPreciseScrollingDeltas ? 1.2 : 120.0
        wheelRemainder.x -= event.scrollingDeltaX * scale
        wheelRemainder.y -= event.scrollingDeltaY * scale
        let dx = Int(wheelRemainder.x.rounded(.towardZero))
        let dy = Int(wheelRemainder.y.rounded(.towardZero))
        guard dx != 0 || dy != 0 else { return }
        wheelRemainder.x -= CGFloat(dx)
        wheelRemainder.y -= CGFloat(dy)
        var fields: [String: Any] = [:]
        if dx != 0 { fields["dx"] = max(-12_000, min(12_000, dx)) }
        if dy != 0 { fields["dy"] = max(-12_000, min(12_000, dy)) }
        stream.sendInput("mouse_wheel", fields)
    }

    private func sendPosition(_ event: NSEvent, in view: NSView) {
        let bounds = view.bounds
        guard bounds.width > 0, bounds.height > 0 else { return }
        let point = view.convert(event.locationInWindow, from: nil)
        let video = stream.video
        let x = (point.x - bounds.minX) / bounds.width * CGFloat(video.width)
        let yFromTop = view.isFlipped ? point.y - bounds.minY : bounds.maxY - point.y
        let y = yFromTop / bounds.height * CGFloat(video.height)
        let position = CGPoint(x: (x * 100).rounded() / 100, y: (y * 100).rounded() / 100)
        guard position != lastPosition else { return }
        lastPosition = position
        stream.sendInput("mouse_move", ["x": Double(position.x), "y": Double(position.y)])
    }

    private static func buttonName(_ event: NSEvent) -> String? {
        switch event.type {
        case .leftMouseDown, .leftMouseUp:
            return "left"
        case .rightMouseDown, .rightMouseUp:
            return "right"
        case .otherMouseDown, .otherMouseUp:
            switch event.buttonNumber {
            case 2: return "middle"
            case 3: return "back"
            case 4: return "forward"
            default: return nil
            }
        default:
            return nil
        }
    }

    // MARK: Keyboard

    /// Returns `true` when the event was forwarded to the host and must not be handled locally.
    func handleKeyEvent(_ event: NSEvent) -> Bool {
        switch event.type {
        case .flagsChanged:
            handleModifier(event)
            return true
        case .keyDown:
            if Self.isLocalShortcut(event) {
                return false
            }
            if stream.clipboardSyncEnabled, event.modifierFlags.contains(.command),
               event.charactersIgnoringModifiers?.lowercased() == "v" {
                pasteClipboard(event)
                return true
            }
            guard let code = Self.keyCodes[event.keyCode] else { return true }
            heldKeys.insert(code)
            stream.sendInput("key", ["code": code, "down": true])
            return true
        case .keyUp:
            guard let code = Self.keyCodes[event.keyCode] else { return true }
            if heldKeys.remove(code) != nil {
                stream.sendInput("key", ["code": code, "down": false])
            }
            return true
        default:
            return false
        }
    }

    /// Releases everything held on the host for this stream, e.g. when the window loses focus.
    func releaseAll() {
        pasteTask?.cancel()
        heldKeys.removeAll()
        heldButtons.removeAll()
        wheelRemainder = .zero
        stream.sendInput("release_all")
    }

    private func pasteClipboard(_ event: NSEvent) {
        guard pasteTask == nil, !event.isARepeat, NSApp.isActive,
              let window = event.window, window.isKeyWindow, window.attachedSheet == nil else { return }
        pasteTask = Task { [weak self, weak window] in
            guard let self else { return }
            defer { self.pasteTask = nil }
            do {
                try await stream.uploadClipboardForPaste()
                try Task.checkCancellation()
                guard NSApp.isActive, let window, window.isKeyWindow,
                      window.attachedSheet == nil else { return }
                // Command may have been released while waiting for clipboard_set.
                let needsControl = !heldKeys.contains("ControlLeft") && !heldKeys.contains("ControlRight")
                if needsControl { stream.sendInput("key", ["code": "ControlLeft", "down": true]) }
                stream.sendInput("key", ["code": "KeyV", "down": true])
                stream.sendInput("key", ["code": "KeyV", "down": false])
                if needsControl { stream.sendInput("key", ["code": "ControlLeft", "down": false]) }
            } catch is CancellationError {
                // Do not paste into a different stream after focus changed.
            } catch {
                stream.onInputError?("Clipboard paste failed: \(error.localizedDescription)")
            }
        }
    }

    private func handleModifier(_ event: NSEvent) {
        if event.keyCode == 57 {
            // Caps Lock reports a toggle rather than press/release; replay it as a full key press.
            stream.sendInput("key", ["code": "CapsLock", "down": true])
            stream.sendInput("key", ["code": "CapsLock", "down": false])
            return
        }
        guard let modifier = Self.modifiers[event.keyCode] else { return }
        let down = event.modifierFlags.rawValue & modifier.deviceMask != 0
        if down {
            guard heldKeys.insert(modifier.code).inserted else { return }
        } else {
            guard heldKeys.remove(modifier.code) != nil else { return }
        }
        stream.sendInput("key", ["code": modifier.code, "down": down])
    }

    private static func isLocalShortcut(_ event: NSEvent) -> Bool {
        guard event.modifierFlags.contains(.command),
              let key = event.charactersIgnoringModifiers?.lowercased()
        else { return false }
        return key == "w" || key == "q"
    }

    // Device-dependent modifier bits (IOKit NX_DEVICE*KEYMASK) distinguish left and right keys.
    private static let modifiers: [UInt16: (code: String, deviceMask: UInt)] = [
        55: ("ControlLeft", 0x0008),   // Left Command → Ctrl
        54: ("ControlRight", 0x0010),  // Right Command → Ctrl
        59: ("MetaLeft", 0x0001),      // Left Control → Windows key
        62: ("MetaRight", 0x2000),     // Right Control → Windows key
        56: ("ShiftLeft", 0x0002),
        60: ("ShiftRight", 0x0004),
        58: ("AltLeft", 0x0020),
        61: ("AltRight", 0x0040),
    ]

    // macOS virtual key codes (Carbon kVK_*) → UI Events KeyboardEvent.code.
    private static let keyCodes: [UInt16: String] = [
        0: "KeyA", 1: "KeyS", 2: "KeyD", 3: "KeyF", 4: "KeyH", 5: "KeyG", 6: "KeyZ", 7: "KeyX",
        8: "KeyC", 9: "KeyV", 10: "IntlBackslash", 11: "KeyB", 12: "KeyQ", 13: "KeyW", 14: "KeyE",
        15: "KeyR", 16: "KeyY", 17: "KeyT", 18: "Digit1", 19: "Digit2", 20: "Digit3", 21: "Digit4",
        22: "Digit6", 23: "Digit5", 24: "Equal", 25: "Digit9", 26: "Digit7", 27: "Minus", 28: "Digit8",
        29: "Digit0", 30: "BracketRight", 31: "KeyO", 32: "KeyU", 33: "BracketLeft", 34: "KeyI",
        35: "KeyP", 36: "Enter", 37: "KeyL", 38: "KeyJ", 39: "Quote", 40: "KeyK", 41: "Semicolon",
        42: "Backslash", 43: "Comma", 44: "Slash", 45: "KeyN", 46: "KeyM", 47: "Period", 48: "Tab",
        49: "Space", 50: "Backquote", 51: "Backspace", 53: "Escape",
        64: "F17", 65: "NumpadDecimal", 67: "NumpadMultiply", 69: "NumpadAdd", 71: "NumLock",
        75: "NumpadDivide", 76: "NumpadEnter", 78: "NumpadSubtract", 79: "F18", 80: "F19",
        81: "NumpadEqual", 82: "Numpad0", 83: "Numpad1", 84: "Numpad2", 85: "Numpad3", 86: "Numpad4",
        87: "Numpad5", 88: "Numpad6", 89: "Numpad7", 90: "F20", 91: "Numpad8", 92: "Numpad9",
        93: "IntlYen", 94: "IntlRo", 95: "NumpadComma",
        96: "F5", 97: "F6", 98: "F7", 99: "F3", 100: "F8", 101: "F9", 102: "Lang2", 103: "F11",
        104: "Lang1", 105: "F13", 106: "F16", 107: "F14", 109: "F10", 110: "ContextMenu", 111: "F12",
        113: "F15", 114: "Insert", 115: "Home", 116: "PageUp", 117: "Delete", 118: "F4", 119: "End",
        120: "F2", 121: "PageDown", 122: "F1", 123: "ArrowLeft", 124: "ArrowRight", 125: "ArrowDown",
        126: "ArrowUp",
    ]
}
