import AppKit
import Observation

@Observable
final class BeamItClipboardMenu {
    static let shared = BeamItClipboardMenu()
    weak var sync: BeamItClipboardSync?

    func synchronize(hostToMac: Bool) {
        guard let sync else { return }
        Task {
            do {
                if hostToMac {
                    try await sync.downloadHostClipboard()
                } else {
                    try await sync.uploadLocalClipboard(manual: true)
                }
            } catch is CancellationError {
                // Focus loss or disconnect cancels manual synchronization.
            } catch {
                sync.onError?(error.localizedDescription)
                let alert = NSAlert(error: error)
                alert.runModal()
            }
        }
    }
}

final class BeamItClipboardSync {
    private weak var connection: BeamItConnection?
    private var fetchTask: Task<Void, Never>?
    private var focusObserver: NSObjectProtocol?
    private var revision = 0
    private var stopped = false
    var onError: ((String) -> Void)?
    var enabled = false {
        didSet {
            if enabled != oldValue { invalidate() }
        }
    }

    init(connection: BeamItConnection) {
        self.connection = connection
        focusObserver = NotificationCenter.default.addObserver(
            forName: NSApplication.didResignActiveNotification, object: nil, queue: .main
        ) { [weak self] _ in
            MainActor.assumeIsolated { self?.invalidate() }
        }
    }

    func stop() {
        stopped = true
        if BeamItClipboardMenu.shared.sync === self {
            BeamItClipboardMenu.shared.sync = nil
        }
        invalidate()
        if let focusObserver {
            NotificationCenter.default.removeObserver(focusObserver)
            self.focusObserver = nil
        }
    }

    private func invalidate() {
        revision &+= 1
        fetchTask?.cancel()
        fetchTask = nil
    }

    func hostClipboardChanged(_ message: [String: Any]) {
        invalidate()
        guard enabled, !stopped, NSApp.isActive,
              message["excluded"] as? Bool != true else { return }
        let currentRevision = revision
        let changeCount = NSPasteboard.general.changeCount
        fetchTask = Task { [weak self] in
            do {
                try await Task.sleep(for: .milliseconds(80))
                guard let self, let connection = self.connection else { return }
                let reply = try await connection.getClipboard()
                try Task.checkCancellation()
                guard self.enabled, !self.stopped, NSApp.isActive,
                      self.revision == currentRevision,
                      NSPasteboard.general.changeCount == changeCount else { return }
                try Self.applyHostClipboard(reply)
            } catch is CancellationError {
                // Focus changes, newer notifications and local paste supersede an old fetch.
            } catch {
                guard let self, !Task.isCancelled, self.revision == currentRevision else { return }
                self.onError?(error.localizedDescription)
            }
        }
    }

    func downloadHostClipboard() async throws {
        guard !stopped, NSApp.isActive, let connection else { throw CancellationError() }
        invalidate()
        let currentRevision = revision
        let changeCount = NSPasteboard.general.changeCount
        let reply = try await connection.getClipboard()
        try Task.checkCancellation()
        guard !stopped, NSApp.isActive, revision == currentRevision,
              NSPasteboard.general.changeCount == changeCount else { throw CancellationError() }
        try Self.applyHostClipboard(reply)
    }

    func uploadLocalClipboard(manual: Bool = false) async throws {
        guard (enabled || manual), !stopped, NSApp.isActive, let connection else {
            throw CancellationError()
        }
        // A pending host read must not replace the local content chosen for this paste.
        invalidate()
        let currentRevision = revision
        let fields = try Self.localClipboard()
        try await connection.setClipboard(fields)
        try Task.checkCancellation()
        guard (enabled || manual), !stopped, NSApp.isActive, revision == currentRevision else {
            throw CancellationError()
        }
    }

    private static let budget = 10 * 1024 * 1024

    private static func localClipboard() throws -> [String: Any] {
        let pasteboard = NSPasteboard.general
        let changeCount = pasteboard.changeCount
        guard pasteboard.pasteboardItems?.count == 1,
              pasteboard.data(forType: .fileURL) == nil else {
            throw ClipboardError("The clipboard must contain one text, HTML or image item. File transfer is not supported.")
        }
        var fields: [String: Any] = [:]
        var size = 0
        if let text = pasteboard.string(forType: .string) {
            fields["text"] = text
            size += text.utf8.count
        }
        if let html = pasteboard.string(forType: .html) {
            fields["html"] = html
            size += html.utf8.count
        }
        var png = pasteboard.data(forType: .png)
        if png == nil, let tiff = pasteboard.data(forType: .tiff) {
            guard let bitmap = NSBitmapImageRep(data: tiff),
                  let converted = bitmap.representation(using: .png, properties: [:]) else {
                throw ClipboardError("The clipboard image could not be converted to PNG.")
            }
            png = converted
        }
        if let png {
            guard let bitmap = NSBitmapImageRep(data: png),
                  bitmap.pixelsWide > 0, bitmap.pixelsHigh > 0,
                  bitmap.pixelsWide <= 16_384, bitmap.pixelsHigh <= 16_384,
                  bitmap.pixelsWide * bitmap.pixelsHigh <= 64 * 1024 * 1024 else {
                throw ClipboardError("The clipboard image exceeds the host's image size limit or is invalid.")
            }
            let encoded = png.base64EncodedString()
            fields["image"] = ["mime_type": "image/png", "data": encoded]
            size += encoded.utf8.count
        }
        guard !fields.isEmpty else {
            throw ClipboardError("The clipboard has no supported text, HTML or PNG image.")
        }
        guard size <= budget else {
            throw ClipboardError("The clipboard exceeds the host's 10 MiB transfer limit.")
        }
        fields["sensitive"] = pasteboard.types?.contains(
            NSPasteboard.PasteboardType("org.nspasteboard.ConcealedType")
        ) == true
        guard pasteboard.changeCount == changeCount else {
            throw ClipboardError("The local clipboard changed while it was being read. Please paste again.")
        }
        return fields
    }

    private static func applyHostClipboard(_ reply: [String: Any]) throws {
        let omitted = reply["omitted"] as? [[String: Any]] ?? []
        if omitted.contains(where: { $0["reason"] as? String == "excluded" }) { return }
        guard omitted.isEmpty else {
            throw ClipboardError("The host could not transfer every clipboard format. The local clipboard was left unchanged.")
        }
        let item = NSPasteboardItem()
        var hasContent = false
        var size = 0
        size += (reply["text"] as? String)?.utf8.count ?? 0
        size += (reply["html"] as? String)?.utf8.count ?? 0
        size += ((reply["image"] as? [String: Any])?["data"] as? String)?.utf8.count ?? 0
        guard size <= budget else { throw ClipboardError("The host clipboard exceeds the 10 MiB limit. The local clipboard was left unchanged.") }
        if let text = reply["text"] as? String {
            item.setString(text, forType: .string)
            hasContent = true
        }
        if let html = reply["html"] as? String {
            item.setString(html, forType: .html)
            hasContent = true
        }
        if let image = reply["image"] as? [String: Any] {
            guard image["mime_type"] as? String == "image/png",
                  let encoded = image["data"] as? String,
                  let data = Data(base64Encoded: encoded), NSBitmapImageRep(data: data) != nil else {
                throw ClipboardError("The host returned an invalid clipboard image.")
            }
            item.setData(data, forType: .png)
            hasContent = true
        }
        guard hasContent else { return }
        let pasteboard = NSPasteboard.general
        pasteboard.clearContents()
        guard pasteboard.writeObjects([item]) else {
            throw ClipboardError("macOS could not update the local clipboard.")
        }
    }

    private struct ClipboardError: LocalizedError {
        let errorDescription: String?
        init(_ message: String) { errorDescription = message }
    }
}
