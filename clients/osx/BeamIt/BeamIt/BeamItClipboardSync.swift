import AppKit
import Observation

@MainActor @Observable
final class BeamItClipboardMenu {
    static let shared = BeamItClipboardMenu()
    weak var sync: BeamItClipboardSync?
    func synchronize(hostToMac: Bool) {
        guard let sync else { return }
        Task {
            do {
                if hostToMac { try await sync.downloadHostClipboard() }
                else { try await sync.uploadLocalClipboard(manual: true) }
            } catch is CancellationError {
                // Decline, supersession, focus loss and explicit cancel are not success.
            } catch {
                sync.onError?(error.localizedDescription)
                NSAlert(error: error).runModal()
            }
        }
    }
}

@MainActor
final class BeamItClipboardSync: NSObject {
    typealias Bulk = BeamItClipboardBulk
    private final class Operation {
        let snapshot: Bulk.RevisionGuard
        var revision: UInt64 { snapshot.revision }
        var sequence: Int { snapshot.sequence }
        let upload: Bool
        let manual: Bool
        let automatic: Bool
        weak var window: NSWindow?
        var localID = ""
        var snapshotID = ""
        var transferID = ""
        var remoteSequence: UInt64 = 0
        var manifest = Bulk.Manifest(selectedRoots: 0, totalFileBytes: 0, nodes: [])
        var roots: [[String: Any]] = []
        var task: Task<Void, Error>?
        var socket: URLSessionWebSocketTask?
        var trailing: Task<Void, Never>?
        var offer: [String: Any]?
        var buffered: [[String: Any]] = []
        var waitingOffer = false
        var ended = false
        var commitSent = false
        var acknowledged = false
        var published = false
        var reconfirmed = false
        var failure: String?
        var remoteEnded = false
        var outcomeSent = false
        var succeeded = false
        var alert: NSAlert?
        var bytes: UInt64 = 0
        var nodes: Int = 0
        var lastReportedBytes: UInt64 = .max
        var lastReportedNodes = -1
        var preparation = BeamItClipboardProgress()
        var preparationSeen: UInt64 = 0
        let created = ProcessInfo.processInfo.systemUptime
        var progress = ProcessInfo.processInfo.systemUptime
        var reported: TimeInterval = 0
        var replenished = ProcessInfo.processInfo.systemUptime
        var tokens = Double(1024 * 1024)
        init(revision: UInt64, upload: Bool, manual: Bool, automatic: Bool) {
            snapshot = .init(revision: revision, sequence: NSPasteboard.general.changeCount)
            self.upload = upload; self.manual = manual; self.automatic = automatic
            window = NSApp.keyWindow ?? NSApp.mainWindow
        }
    }
    private weak var connection: BeamItConnection?
    private let storage = BeamItClipboardStorage()
    private var current: Operation?
    private var pollTask: Task<Void, Never>?
    private var automaticTask: Task<Void, Never>?
    private var observers: [NSObjectProtocol] = []
    private var revision: UInt64 = 0
    private var stopped = false
    private var seenCount = NSPasteboard.general.changeCount
    private var ownCount = -1
    private var uploadedCount = -1
    private var panel: NSPanel?
    private var progressLabel: NSTextField?
    private var progressBar: NSProgressIndicator?
    private(set) var bulkEnabled = false
    var onError: ((String) -> Void)?
    var enabled = false {
        didSet { if enabled != oldValue { cancel() } }
    }

    init(connection: BeamItConnection) {
        self.connection = connection
        super.init()
        for name in [NSApplication.didResignActiveNotification, NSWindow.didResignKeyNotification] {
            observers.append(NotificationCenter.default.addObserver(forName: name, object: nil, queue: .main) {
                [weak self] _ in
                MainActor.assumeIsolated {
                    guard let self, let op = self.current else { return }
                    // A confirmation sheet belongs to the captured window, not a new paste target.
                    if !self.valid(op) { self.cancel() }
                }
            })
        }
    }

    func negotiate() async throws {
        guard let connection else { throw CancellationError() }
        do {
            let reply = try await connection.clipboardRequest("clipboard_bulk_negotiate", ["version": 1],
                                                            reply: "clipboard_bulk_capabilities")
            let limits: [String: UInt64] = [
                "version": 1, "max_file_bytes": 2_000_000_000, "max_roots": 64, "max_nodes": 4096,
                "max_manifest_bytes": 4 * 1024 * 1024, "max_chunk_bytes": 256 * 1024,
                "max_png_bytes": 64 * 1024 * 1024, "max_png_dimension": 16_384,
                "max_png_pixels": 64 * 1024 * 1024, "max_component_bytes": 255,
                "max_path_bytes": 4096, "staging_quota_bytes": 8_000_000_000, "scratch_bytes": 128 * 1024 * 1024]
            for (key, value) in limits where try Bulk.unsigned(reply[key]) != value {
                throw BeamItClipboardFailure("invalid_capabilities")
            }
            bulkEnabled = true
            try await storage.recover()
        } catch let error as BeamItConnection.ClipboardHostFailure where error.code == "unknown_type" && !error.fatal {
            bulkEnabled = false
        }
        pollTask = Task { [weak self] in
            while !Task.isCancelled {
                do { try await Task.sleep(for: .milliseconds(80)) }
                catch { return }
                guard let self, !self.stopped else { return }
                self.poll()
            }
        }
    }

    func stop() {
        stopped = true
        cancel()
        pollTask?.cancel(); pollTask = nil
        if BeamItClipboardMenu.shared.sync === self { BeamItClipboardMenu.shared.sync = nil }
        for observer in observers { NotificationCenter.default.removeObserver(observer) }
        observers.removeAll()
    }
    @objc func cancel() {
        invalidate(cancelAutomatic: true)
    }
    private func invalidate(cancelAutomatic: Bool) {
        revision &+= 1
        if cancelAutomatic { automaticTask?.cancel(); automaticTask = nil }
        guard let op = current else { return }
        op.task?.cancel()
        op.socket?.cancel(with: .goingAway, reason: nil)
        if let alert = op.alert, let parent = alert.window.sheetParent {
            parent.endSheet(alert.window, returnCode: .cancel)
        }
    }
    private func valid(_ op: Operation, publishing: Bool = false) -> Bool {
        guard !stopped, NSApp.isActive, enabled || op.manual,
              op.snapshot.matches(revision: revision, sequence: publishing ? op.sequence : NSPasteboard.general.changeCount) else {
            return false
        }
        guard let window = op.window else { return false }
        return window.isKeyWindow || (op.alert != nil && window.attachedSheet === NSApp.keyWindow)
    }
    private func check(_ op: Operation, publishing: Bool = false) throws {
        try Task.checkCancellation()
        guard current === op, valid(op, publishing: publishing) else { throw CancellationError() }
        if let code = op.failure { throw BeamItClipboardFailure(code) }
        let now = ProcessInfo.processInfo.systemUptime
        guard now - op.created < 30 * 60, now - op.progress < 30 else { throw BeamItClipboardFailure("timeout") }
    }
    private func poll() {
        if let op = current, op.revision == revision {
            let bytes = op.preparation.read()
            if bytes > op.preparationSeen {
                op.preparationSeen = bytes; op.progress = ProcessInfo.processInfo.systemUptime
                show(op, "Preparing", bytes: bytes)
            }
            if !valid(op, publishing: op.published) ||
                ProcessInfo.processInfo.systemUptime - op.created >= 30 * 60 ||
                ProcessInfo.processInfo.systemUptime - op.progress >= 30 {
                op.failure = "stale_or_timeout"
                cancel()
            }
        }
        let count = NSPasteboard.general.changeCount
        guard count != seenCount else { return }
        seenCount = count
        guard count != ownCount, enabled, NSApp.isActive else { return }
        guard NSPasteboard.general.types?.contains(.init("org.nspasteboard.ConcealedType")) != true else { return }
        if let op = current, !op.upload { cancel(); return }
        automaticTask?.cancel()
        automaticTask = Task { [weak self] in
            do {
                try await Task.sleep(for: .milliseconds(80))
                try await self?.uploadLocalClipboard(automatic: true)
            } catch is CancellationError {}
            catch { self?.onError?(error.localizedDescription) }
        }
    }

    func hostClipboardChanged(_ message: [String: Any]) {
        uploadedCount = -1
        cancel()
        guard enabled, !stopped, NSApp.isActive, message["excluded"] as? Bool != true else { return }
        automaticTask = Task { [weak self] in
            do {
                try await Task.sleep(for: .milliseconds(80))
                try await self?.downloadHostClipboard(manual: false)
            } catch is CancellationError {}
            catch { self?.onError?(error.localizedDescription) }
        }
    }
    func bulkEvent(_ message: [String: Any]) {
        guard let op = current, let id = message["transfer_id"] as? String else { return }
        if op.transferID.isEmpty && op.waitingOffer {
            guard op.buffered.count < 8 else { op.failure = "event_overflow"; cancel(); return }
            op.buffered.append(message); return
        }
        guard id == op.transferID else { return }
        switch message["type"] as? String {
        case "clipboard_bulk_offer", "clipboard_bulk_reconfirm":
            guard !op.upload, op.offer == nil else { op.failure = "unexpected_offer"; cancel(); return }
            op.offer = message
        case "clipboard_bulk_complete":
            guard op.upload, op.ended, op.commitSent else { op.failure = "premature_completion"; cancel(); return }
            do { _ = try Bulk.unsigned(message["sequence"]); op.acknowledged = true }
            catch { op.failure = "invalid_completion"; cancel() }
        case "clipboard_bulk_failed", "clipboard_bulk_cancelled":
            op.remoteEnded = true
            op.failure = message["code"] as? String ?? "remote_failure"
            cancel()
        case "clipboard_bulk_progress":
            do {
                let bytes = try Bulk.unsigned(message["completed_bytes"])
                guard try Bulk.unsigned(message["total_bytes"]) == total(op),
                      try Bulk.unsigned(message["total_nodes"]) == UInt64(op.manifest.nodes.count),
                      try Bulk.unsigned(message["completed_nodes"]) <= UInt64(op.manifest.nodes.count),
                      bytes <= total(op) else { throw BeamItClipboardFailure("invalid_progress") }
                if !op.upload && message["phase"] as? String == "preparing" && bytes > op.bytes {
                    op.bytes = bytes; op.progress = ProcessInfo.processInfo.systemUptime
                    show(op, "Preparing", bytes: bytes)
                }
            } catch { op.failure = "invalid_progress"; cancel() }
        default: break
        }
    }

    func uploadLocalClipboard(manual: Bool = false, automatic: Bool = false) async throws {
        guard !stopped, NSApp.isActive, enabled || manual else { throw CancellationError() }
        if let op = current, op.upload, valid(op), let task = op.task {
            try await task.value
            guard valid(op), !op.reconfirmed else { throw CancellationError() }
            return
        }
        if uploadedCount == NSPasteboard.general.changeCount, !manual { return }
        try await run(upload: true, manual: manual, automatic: automatic)
    }
    func downloadHostClipboard(manual: Bool = true) async throws { try await run(upload: false, manual: manual) }
    private func run(upload: Bool, manual: Bool, automatic: Bool = false) async throws {
        guard !stopped, NSApp.isActive, enabled || manual else { throw CancellationError() }
        let previous = current
        invalidate(cancelAutomatic: false)
        let op = Operation(revision: revision, upload: upload, manual: manual, automatic: automatic)
        if let previous {
            if let task = previous.task {
                do { try await task.value; previous.succeeded = true }
                catch {
                    // Its original caller owns the error. Join shutdown before starting
                    // another session transfer, recording a path-free terminal code.
                    if let failure = error as? BeamItClipboardFailure { previous.failure = failure.code }
                    else if !(error is CancellationError) { previous.failure = "transport_or_native_failure" }
                }
            }
            await cleanup(previous, failed: !previous.succeeded)
        }
        try Task.checkCancellation()
        guard valid(op) else { throw CancellationError() }
        current = op
        show(op, "Preparing", bytes: 0, force: true)
        let task = Task {
            if upload { try await self.send(op) }
            else { try await self.receive(op) }
        }
        op.task = task
        var forwardable = false
        do {
            try await task.value
            op.succeeded = true
            show(op, "Complete", bytes: total(op), force: true)
            await cleanup(op, failed: false)
            forwardable = current === op && valid(op, publishing: op.published) && op.failure == nil
            if current === op { current = nil; panel?.orderOut(nil) }
        } catch {
            if let failure = error as? BeamItClipboardFailure { op.failure = failure.code }
            else if !(error is CancellationError) && op.failure == nil { op.failure = "transport_or_native_failure" }
            op.socket?.cancel(with: .goingAway, reason: nil)
            await cleanup(op, failed: true)
            if current === op { current = nil; panel?.orderOut(nil) }
            if let code = op.failure { throw BeamItClipboardFailure(code) }
            throw error
        }
        if op.reconfirmed || (upload && (!forwardable || Task.isCancelled)) {
            throw CancellationError() // Do not release an old pending paste.
        }
    }
    private func cleanup(_ op: Operation, failed: Bool) async {
        op.task = nil
        op.trailing?.cancel(); op.trailing = nil
        if failed, !op.outcomeSent, !op.remoteEnded, !op.transferID.isEmpty, let connection {
            op.outcomeSent = true
            do { try await connection.clipboardEvent(!op.upload && op.failure != nil ? "clipboard_bulk_failed" : "clipboard_bulk_cancel",
                ["transfer_id": op.transferID, "code": op.failure ?? "cancelled"]) }
            catch { NSLog("BeamIt clipboard cancellation could not be delivered.") }
        }
        for id in Set([op.localID, op.upload ? "" : op.transferID]).filter({ !$0.isEmpty }) {
            do { try await storage.finish(id, published: op.published) }
            catch { NSLog("BeamIt clipboard staging cleanup failed; retained data remains quota-accounted.") }
        }
        op.socket?.cancel(with: .normalClosure, reason: nil)
    }

    private struct Local {
        var fields: [String: Any]
        let files: [URL]
        let image: Data?
        let isPNG: Bool
    }
    private func local(_ op: Operation) throws -> Local {
        try check(op)
        let board = NSPasteboard.general
        let excluded = ["org.nspasteboard.TransientType", "org.nspasteboard.AutoGeneratedType"]
        guard !excluded.contains(where: { board.types?.contains(.init($0)) == true }) else {
            throw BeamItClipboardFailure("excluded")
        }
        let items = board.pasteboardItems ?? []
        let smallTypes: [NSPasteboard.PasteboardType] = [.string, .html, .png, .tiff]
        var files: [URL] = []
        var metadataItem: NSPasteboardItem?
        for item in items {
            if let value = item.string(forType: .fileURL) {
                guard let url = URL(string: value), url.isFileURL else { throw BeamItClipboardFailure("unsupported_entry") }
                files.append(url)
            } else {
                guard metadataItem == nil,
                      item.types.contains(where: { smallTypes.contains($0) }) else {
                    throw BeamItClipboardFailure("unsupported_selection")
                }
                metadataItem = item
            }
            guard !item.types.contains(where: {
                let type = $0.rawValue.lowercased()
                return type.contains("promised-file") || type.contains("filepromise") || type.contains("filespromise")
            }) else {
                throw BeamItClipboardFailure("file_promises_unsupported")
            }
        }
        if !files.isEmpty {
            // Read native URL objects, not just file-URL strings, so App Sandbox
            // can grant the user-selected read access carried by the pasteboard.
            guard let granted = board.readObjects(forClasses: [NSURL.self],
                options: [.urlReadingFileURLsOnly: true]) as? [URL],
                  granted.count == files.count,
                  zip(granted, files).allSatisfy({ pair in pair.0.path == pair.1.path }) else {
                throw BeamItClipboardFailure("source_access_unavailable")
            }
            files = granted
        }
        guard files.count <= 64, bulkEnabled || files.isEmpty else { throw BeamItClipboardFailure("legacy_files_unsupported") }
        var fields: [String: Any] = [:]
        let sensitive = board.types?.contains(.init("org.nspasteboard.ConcealedType")) == true
        guard !sensitive || !op.automatic else { throw BeamItClipboardFailure("excluded") }
        fields["sensitive"] = sensitive
        let text = metadataItem != nil ? metadataItem!.string(forType: .string) : board.string(forType: .string)
        let html = metadataItem != nil ? metadataItem!.string(forType: .html) : board.string(forType: .html)
        if let text { fields["text"] = text }
        if let html { fields["html"] = html }
        let bytes = (fields["text"] as? String)?.utf8.count ?? 0
        guard bytes + ((fields["html"] as? String)?.utf8.count ?? 0) <= 10 * 1024 * 1024 else {
            throw BeamItClipboardFailure("text_too_large")
        }
        let png = metadataItem != nil ? metadataItem!.data(forType: .png) : board.data(forType: .png)
        let tiff = metadataItem != nil ? metadataItem!.data(forType: .tiff) : board.data(forType: .tiff)
        let image = png ?? tiff
        guard image != nil || !files.isEmpty || fields["text"] != nil || fields["html"] != nil else {
            throw BeamItClipboardFailure("unsupported_clipboard")
        }
        try check(op)
        return Local(fields: fields, files: files, image: image, isPNG: png != nil)
    }

    private func confirm(_ op: Operation) async throws {
        let policy = Bulk.configuredPolicy()
        guard try Bulk.requiresConfirmation(bytes: op.manifest.totalFileBytes, roots: op.manifest.selectedRoots,
                                            byteThreshold: policy.byteThreshold, rootThreshold: policy.rootThreshold) else { return }
        try check(op)
        guard let window = op.window else { throw CancellationError() }
        let alert = NSAlert()
        alert.messageText = "Transfer clipboard selection?"
        alert.informativeText = "\(op.manifest.selectedRoots) selected roots · \(op.manifest.totalFileBytes) bytes in regular files. Folders include all descendants."
        alert.addButton(withTitle: "Transfer")
        alert.addButton(withTitle: "Cancel")
        op.alert = alert
        show(op, "Waiting for confirmation", bytes: 0, force: true)
        let decision = await withCheckedContinuation { continuation in
            alert.beginSheetModal(for: window) { response in continuation.resume(returning: response) }
        }
        op.alert = nil
        guard decision == .alertFirstButtonReturn else { throw CancellationError() }
        try check(op)
        op.progress = ProcessInfo.processInfo.systemUptime
    }

    private func send(_ op: Operation) async throws {
        guard let connection else { throw CancellationError() }
        var content = try local(op)
        var image: BeamItClipboardStorage.Image?
        if let data = content.image { image = try await storage.image(data, isPNG: content.isPNG) }
        try check(op)
        if !bulkEnabled || (content.files.isEmpty && image == nil) {
            guard image == nil || image!.png.count <= 7_864_320 else { throw BeamItClipboardFailure("legacy_too_large") }
            if let image { content.fields["image"] = ["mime_type": "image/png", "data": image.png.base64EncodedString()] }
            let textBytes = ((content.fields["text"] as? String)?.utf8.count ?? 0) +
                ((content.fields["html"] as? String)?.utf8.count ?? 0)
            guard textBytes + (image?.png.base64EncodedString().utf8.count ?? 0) <= 10 * 1024 * 1024 else {
                throw BeamItClipboardFailure("legacy_too_large")
            }
            try await connection.setClipboard(content.fields)
            try check(op)
            uploadedCount = op.sequence
            return
        }
        var selection = try await storage.inspect(urls: content.files, png: image?.png, width: image?.width, height: image?.height)
        while true {
            op.localID = selection.id; op.snapshotID = selection.id; op.manifest = selection.manifest
            op.preparation = BeamItClipboardProgress(); op.preparationSeen = 0
            op.bytes = 0; op.nodes = 0; op.reported = 0
            try check(op)
            try await confirm(op)
            do {
                op.manifest = try await storage.prepare(selection, progress: op.preparation)
                break
            } catch let error as BeamItClipboardFailure where error.code == "source_changed" {
                try await storage.finish(selection.id)
                try check(op)
                op.reconfirmed = true
                selection = try await storage.inspect(urls: content.files, png: image?.png, width: image?.width, height: image?.height)
            }
        }
        try check(op)
        var fields = content.fields
        fields["manifest"] = Bulk.json(op.manifest); fields["source_snapshot_id"] = op.snapshotID
        let offer = try await connection.clipboardRequest("clipboard_bulk_begin", fields, reply: "clipboard_bulk_offer")
        try validateOffer(op, offer)
        guard try await storage.unchanged(op.localID) else { throw BeamItClipboardFailure("source_changed") }
        try check(op)
        guard let url = (offer["url"] as? String).flatMap(URL.init(string:)) else { throw BeamItClipboardFailure("invalid_offer") }
        let socket = try await connection.openBulkSocket(url, transferID: op.transferID); op.socket = socket
        let transfer = try Bulk.decodeID(op.transferID, bytes: 16)
        while true {
            try check(op)
            let frame = try await storage.senderFrame(op.localID, transferID: transfer)
            let header = try Bulk.decode(frame)
            try await pace(op, bytes: header.payload.count)
            try check(op)
            try await socket.send(.data(frame))
            op.bytes += UInt64(header.payload.count); op.progress = ProcessInfo.processInfo.systemUptime
            show(op, "Sending", bytes: op.bytes)
            if header.kind == .end { op.ended = true; break }
        }
        guard try await storage.unchanged(op.localID) else { throw BeamItClipboardFailure("source_changed") }
        try check(op)
        op.commitSent = true
        let committed = try await connection.clipboardRequest("clipboard_bulk_commit",
            ["transfer_id": op.transferID, "source_snapshot_id": op.snapshotID], reply: "clipboard_bulk_committed")
        guard committed["transfer_id"] as? String == op.transferID else { throw BeamItClipboardFailure("invalid_commit") }
        while !op.acknowledged { try check(op); try await Task.sleep(for: .milliseconds(20)) }
        guard try await storage.unchanged(op.localID) else { throw BeamItClipboardFailure("source_changed") }
        try check(op)
        uploadedCount = op.sequence
    }

    private func snapshot(_ op: Operation, _ reply: [String: Any]) throws {
        guard reply["excluded"] as? Bool != true,
              (reply["omitted"] as? [[String: Any]] ?? []).isEmpty,
              let id = reply["snapshot_id"] as? String,
              let object = reply["manifest"] as? [String: Any],
              let roots = object["roots"] as? [[String: Any]] else { throw BeamItClipboardFailure("excluded_or_invalid_snapshot") }
        _ = try Bulk.decodeID(id, bytes: 16)
        op.snapshotID = id; op.remoteSequence = try Bulk.unsigned(reply["sequence"])
        op.manifest = try Bulk.parse(object, preliminary: true); op.roots = roots
        var paths = Set<String>()
        guard roots.count == Int(op.manifest.selectedRoots) else { throw BeamItClipboardFailure("invalid_roots") }
        for root in roots {
            guard let path = root["path"] as? String, let identity = root["identity"] as? String,
                  paths.insert(path).inserted, !path.contains("/"),
                  op.manifest.nodes.contains(where: { $0.path == path && $0.kind != .png }) else {
                throw BeamItClipboardFailure("invalid_roots")
            }
            _ = try Bulk.decodeID(identity, bytes: 32)
        }
    }
    private func validateOffer(_ op: Operation, _ offer: [String: Any]) throws {
        guard offer["direction"] as? String == (op.upload ? "client_to_host" : "host_to_client"),
              offer["source_snapshot_id"] as? String == op.snapshotID,
              try Bulk.unsigned(offer["expires_in_ms"]) == 10_000,
              let id = offer["transfer_id"] as? String,
              let object = offer["manifest"] as? [String: Any] else { throw BeamItClipboardFailure("invalid_offer") }
        _ = try Bulk.decodeID(id, bytes: 16)
        guard op.transferID.isEmpty || op.transferID == id else { throw BeamItClipboardFailure("invalid_transfer") }
        let final = try Bulk.parse(object)
        if op.upload {
            guard final == op.manifest, try Bulk.unsigned(offer["destination_sequence"]) <= UInt32.max else {
                throw BeamItClipboardFailure("selection_changed")
            }
        } else {
            guard try Bulk.unsigned(offer["source_sequence"]) == op.remoteSequence,
                  let roots = offer["roots"] as? [[String: Any]], NSArray(array: roots).isEqual(to: op.roots),
                  NSDictionary(dictionary: Bulk.json(final, hashes: false))
                    .isEqual(to: Bulk.json(op.manifest, hashes: false)) else { throw BeamItClipboardFailure("selection_changed") }
        }
        op.transferID = id; op.manifest = final
        try check(op)
    }
    private func receive(_ op: Operation) async throws {
        guard let connection else { throw CancellationError() }
        var metadata = try await connection.getClipboard()
        try check(op)
        if !bulkEnabled {
            var png: Data?
            let textBytes = ((metadata["text"] as? String)?.utf8.count ?? 0) +
                ((metadata["html"] as? String)?.utf8.count ?? 0)
            if let image = metadata["image"] as? [String: Any] {
                guard image["mime_type"] as? String == "image/png", let encoded = image["data"] as? String,
                      textBytes + encoded.utf8.count <= 10 * 1024 * 1024,
                      let bytes = Data(base64Encoded: encoded) else {
                    throw BeamItClipboardFailure("invalid_png")
                }
                let decoded = try await storage.image(bytes, isPNG: true)
                png = decoded.png
            }
            try publish(op, metadata: metadata, files: [], png: png)
            return
        }
        while true {
            try snapshot(op, metadata)
            if op.manifest.nodes.isEmpty { try publish(op, metadata: metadata, files: [], png: nil); return }
            try await confirm(op)
            op.waitingOffer = true
            let preparing = try await connection.clipboardRequest("clipboard_bulk_request",
                ["snapshot_id": op.snapshotID], reply: "clipboard_bulk_progress")
            guard preparing["phase"] as? String == "preparing",
                  preparing["source_snapshot_id"] as? String == op.snapshotID,
                  let id = preparing["transfer_id"] as? String else { throw BeamItClipboardFailure("invalid_preparing") }
            _ = try Bulk.decodeID(id, bytes: 16); op.transferID = id; op.waitingOffer = false
            for event in op.buffered { bulkEvent(event) }
            op.buffered.removeAll()
            while op.offer == nil { try check(op); try await Task.sleep(for: .milliseconds(20)) }
            let offer = op.offer!; op.offer = nil
            if offer["type"] as? String == "clipboard_bulk_reconfirm" {
                metadata = offer; op.transferID = ""; op.bytes = 0; op.progress = ProcessInfo.processInfo.systemUptime
                continue // New snapshot and a new confirmation decision, never the old approval.
            }
            try validateOffer(op, offer)
            op.bytes = 0
            try await storage.beginReceive(op.transferID, manifest: op.manifest)
            try check(op)
            guard let url = (offer["url"] as? String).flatMap(URL.init(string:)) else { throw BeamItClipboardFailure("invalid_offer") }
            let socket = try await connection.openBulkSocket(url, transferID: op.transferID); op.socket = socket
            while true {
                try check(op)
                let message = try await socket.receive()
                guard case let .data(frame) = message else { throw BeamItClipboardFailure("invalid_frame") }
                let header = try Bulk.decode(frame)
                try await pace(op, bytes: header.payload.count)
                try check(op)
                let ended = try await storage.accept(op.transferID, frame: frame)
                op.bytes += UInt64(header.payload.count)
                op.nodes = ended ? op.manifest.nodes.count : Int(header.nodeIndex) +
                    (header.offset + UInt64(header.payload.count) == op.manifest.nodes[Int(header.nodeIndex)].length ? 1 : 0)
                op.progress = ProcessInfo.processInfo.systemUptime
                if show(op, ended ? "Publishing" : "Receiving", bytes: op.bytes) {
                    try await connection.clipboardEvent("clipboard_bulk_progress", [
                        "transfer_id": op.transferID, "phase": "receiving",
                        "completed_bytes": op.bytes, "total_bytes": total(op),
                        "completed_nodes": op.nodes, "total_nodes": op.manifest.nodes.count])
                }
                if ended { op.ended = true; break }
            }
            op.trailing = Task {
                do {
                    _ = try await socket.receive()
                    if !Task.isCancelled { op.failure = "data_after_end"; op.task?.cancel() }
                } catch {
                    if !Task.isCancelled && !op.published { op.failure = "interrupted"; op.task?.cancel() }
                }
            }
            let publication = try await storage.promote(op.transferID)
            await Task.yield() // Let a queued trailing-frame failure reach the publication guard.
            try check(op)
            try publish(op, metadata: metadata, files: publication.files, png: publication.png)
            // Publication and this retention flag are synchronous on MainActor.
            // Cancellation cannot delete paths between an OS write and marking ownership.
            op.published = true
            try await connection.clipboardEvent("clipboard_bulk_complete",
                ["transfer_id": op.transferID, "sequence": NSPasteboard.general.changeCount])
            return
        }
    }

    private func publish(_ op: Operation, metadata: [String: Any], files: [URL], png: Data?) throws {
        try check(op)
        guard metadata["excluded"] as? Bool != true,
              (metadata["omitted"] as? [[String: Any]] ?? []).isEmpty else {
            throw BeamItClipboardFailure("excluded_or_omitted")
        }
        let text = metadata["text"] as? String, html = metadata["html"] as? String
        guard (text?.utf8.count ?? 0) + (html?.utf8.count ?? 0) <= 10 * 1024 * 1024 else {
            throw BeamItClipboardFailure("text_too_large")
        }
        var items: [NSPasteboardWriting] = files.map { $0 as NSURL }
        if text != nil || html != nil || png != nil {
            let item = NSPasteboardItem()
            if let text, !item.setString(text, forType: .string) { throw BeamItClipboardFailure("clipboard_prepare_failed") }
            if let html, !item.setString(html, forType: .html) { throw BeamItClipboardFailure("clipboard_prepare_failed") }
            if let png, !item.setData(png, forType: .png) { throw BeamItClipboardFailure("clipboard_prepare_failed") }
            items.append(item)
        }
        guard !items.isEmpty else { throw BeamItClipboardFailure("unsupported_clipboard") }
        let board = NSPasteboard.general
        // Snapshot currently materialized supported representations before clear.
        // Arbitrary delayed-render third-party representations cannot be restored atomically.
        let supported: [NSPasteboard.PasteboardType] = [.string, .html, .png, .tiff, .fileURL,
            .init("org.nspasteboard.ConcealedType"), .init("org.nspasteboard.TransientType"),
            .init("org.nspasteboard.AutoGeneratedType")]
        let old = try (board.pasteboardItems ?? []).compactMap { source -> NSPasteboardItem? in
            let copy = NSPasteboardItem()
            for type in source.types where supported.contains(type) {
                guard let data = source.data(forType: type), copy.setData(data, forType: type) else {
                    throw BeamItClipboardFailure("clipboard_backup_failed")
                }
            }
            return copy.types.isEmpty ? nil : copy
        }
        try check(op)
        board.clearContents()
        guard board.writeObjects(items) else {
            board.clearContents()
            let restored = old.isEmpty || board.writeObjects(old)
            if !restored { op.published = true } // Partial OS references may survive; retain conservatively.
            throw BeamItClipboardFailure(restored ? "clipboard_unavailable_supported_formats_restored_delayed_formats_not_atomic" :
                "clipboard_unavailable_rollback_failed_files_retained")
        }
        op.published = true
        ownCount = board.changeCount; seenCount = ownCount; uploadedCount = ownCount
    }
    private func total(_ op: Operation) -> UInt64 { op.manifest.nodes.reduce(0) { $0 + ($1.length ?? 0) } }
    private func pace(_ op: Operation, bytes: Int) async throws {
        let now = ProcessInfo.processInfo.systemUptime
        op.tokens = min(Double(1024 * 1024), op.tokens + (now - op.replenished) * 8 * 1024 * 1024)
        op.replenished = now
        if op.tokens < Double(bytes) {
            try await Task.sleep(for: .seconds((Double(bytes) - op.tokens) / (8 * 1024 * 1024)))
            try check(op)
            let later = ProcessInfo.processInfo.systemUptime
            op.tokens = min(Double(1024 * 1024), op.tokens + (later - op.replenished) * 8 * 1024 * 1024)
            op.replenished = later
        }
        op.tokens -= Double(bytes)
    }
    @discardableResult private func show(_ op: Operation, _ phase: String, bytes: UInt64, force: Bool = false) -> Bool {
        guard current === op else { return false }
        let now = ProcessInfo.processInfo.systemUptime
        guard force || (now - op.reported >= 0.25 &&
            (bytes != op.lastReportedBytes || op.nodes != op.lastReportedNodes)) else { return false }
        op.reported = now; op.lastReportedBytes = bytes; op.lastReportedNodes = op.nodes
        if panel == nil {
            let window = NSPanel(contentRect: NSRect(x: 0, y: 0, width: 430, height: 118),
                                 styleMask: [.titled, .nonactivatingPanel], backing: .buffered, defer: false)
            window.title = "Clipboard transfer"
            window.isFloatingPanel = true; window.isReleasedWhenClosed = false
            let label = NSTextField(labelWithString: "")
            label.frame = NSRect(x: 16, y: 74, width: 400, height: 26)
            let bar = NSProgressIndicator(frame: NSRect(x: 16, y: 49, width: 400, height: 18))
            bar.isIndeterminate = false; bar.minValue = 0
            let button = NSButton(title: "Cancel", target: self, action: #selector(cancel))
            button.frame = NSRect(x: 330, y: 10, width: 86, height: 30)
            window.contentView?.addSubview(label); window.contentView?.addSubview(bar); window.contentView?.addSubview(button)
            progressLabel = label; progressBar = bar; panel = window
        }
        progressLabel?.stringValue = "\(phase) · \(bytes) / \(total(op)) bytes"
        progressBar?.maxValue = Double(max(1, total(op))); progressBar?.doubleValue = Double(bytes)
        panel?.center(); panel?.orderFrontRegardless()
        return true
    }
}
