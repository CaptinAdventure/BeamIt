import AppKit
import UniformTypeIdentifiers

/// "Send Text to Target…": types pasted or file-loaded text into the remote stream one character at a
/// time using PROTOCOL.md §5.9.6 `text` messages, so it arrives like keyboard typing.
extension BeamItStreamWindowController {
    /// Delay between characters so remote applications see a natural typing cadence.
    private static let characterInterval = Duration.milliseconds(8)

    static func presentSendTextForActiveStream() {
        guard let controller = activeController, controller.window?.isVisible == true else {
            NSSound.beep()
            return
        }
        controller.presentSendText()
    }

    func presentSendText() {
        guard let window else { return }
        if isTypingText {
            confirmStopTyping(in: window)
            return
        }

        let textView = NSTextView()
        textView.isRichText = false
        textView.importsGraphics = false
        textView.allowsUndo = true
        textView.font = .monospacedSystemFont(ofSize: NSFont.systemFontSize, weight: .regular)
        textView.isAutomaticQuoteSubstitutionEnabled = false
        textView.isAutomaticDashSubstitutionEnabled = false
        textView.isAutomaticTextReplacementEnabled = false
        textView.isAutomaticSpellingCorrectionEnabled = false
        textView.isContinuousSpellCheckingEnabled = false
        textView.isVerticallyResizable = true
        textView.autoresizingMask = [.width]
        textView.textContainer?.widthTracksTextView = true

        let scrollView = NSScrollView(frame: NSRect(x: 0, y: 0, width: 420, height: 200))
        scrollView.hasVerticalScroller = true
        scrollView.borderType = .bezelBorder
        scrollView.documentView = textView
        textView.frame = NSRect(origin: .zero, size: scrollView.contentSize)

        let pasteButton = BlockButton(title: "Paste Clipboard") {
            if let text = NSPasteboard.general.string(forType: .string) {
                textView.insertText(text, replacementRange: textView.selectedRange())
            } else {
                NSSound.beep()
            }
        }
        let openButton = BlockButton(title: "Open File…") { [weak window] in
            let panel = NSOpenPanel()
            panel.allowedContentTypes = [.text]
            panel.allowsMultipleSelection = false
            panel.canChooseDirectories = false
            guard let window = window?.attachedSheet ?? window else { return }
            panel.beginSheetModal(for: window) { response in
                guard response == .OK, let url = panel.url else { return }
                do {
                    textView.string = try Self.readText(from: url)
                } catch {
                    NSAlert(error: error).runModal()
                }
            }
        }
        let buttons = NSStackView(views: [pasteButton, openButton])
        buttons.orientation = .horizontal

        let accessory = NSStackView(views: [scrollView, buttons])
        accessory.orientation = .vertical
        accessory.alignment = .leading
        accessory.spacing = 8
        accessory.frame = NSRect(x: 0, y: 0, width: 420, height: 232)
        scrollView.widthAnchor.constraint(equalToConstant: 420).isActive = true
        scrollView.heightAnchor.constraint(equalToConstant: 200).isActive = true

        let alert = NSAlert()
        alert.messageText = "Send Text to Target"
        alert.informativeText = "The text is typed into “\(title)” one character at a time."
        alert.accessoryView = accessory
        alert.addButton(withTitle: "Send")
        alert.addButton(withTitle: "Cancel")
        alert.window.initialFirstResponder = textView

        alert.beginSheetModal(for: window) { [weak self] response in
            withExtendedLifetime([pasteButton, openButton]) {}
            guard response == .alertFirstButtonReturn, let self else { return }
            self.typeText(textView.string)
        }
    }

    private func typeText(_ text: String) {
        let characters = Self.typeableCharacters(text)
        guard !characters.isEmpty else { return }
        typingTask = Task { [weak self] in
            for character in characters {
                guard !Task.isCancelled, let self else { break }
                self.stream.sendInput("text", ["text": character])
                try? await Task.sleep(for: Self.characterInterval)
            }
            self?.typingTask = nil
        }
    }

    private func confirmStopTyping(in window: NSWindow) {
        let alert = NSAlert()
        alert.messageText = "Text is still being sent"
        alert.informativeText = "Stop typing the remaining text into “\(title)”?"
        alert.addButton(withTitle: "Stop Sending")
        alert.addButton(withTitle: "Continue")
        alert.beginSheetModal(for: window) { [weak self] response in
            if response == .alertFirstButtonReturn {
                self?.stopTypingText()
            }
        }
    }

    /// Splits text into characters the host accepts: CRLF/CR become a single newline (typed as Enter),
    /// and control characters other than tab/newline are dropped because the host rejects them.
    private static func typeableCharacters(_ text: String) -> [String] {
        text.compactMap { character -> String? in
            if character == "\r\n" || character == "\r" {
                return "\n"
            }
            let filtered = String(character.unicodeScalars.filter { scalar in
                scalar == "\t" || scalar == "\n" || !(scalar.value < 0x20 || scalar.value == 0x7F)
            })
            return filtered.isEmpty ? nil : filtered
        }
    }

    private static func readText(from url: URL) throws -> String {
        var encoding = String.Encoding.utf8
        if let text = try? String(contentsOf: url, usedEncoding: &encoding) {
            return text
        }
        return try String(contentsOf: url, encoding: .isoLatin1)
    }
}

private final class BlockButton: NSButton {
    private let handler: () -> Void

    init(title: String, handler: @escaping () -> Void) {
        self.handler = handler
        super.init(frame: .zero)
        self.title = title
        bezelStyle = .push
        target = self
        action = #selector(run)
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    @objc private func run() {
        handler()
    }
}
