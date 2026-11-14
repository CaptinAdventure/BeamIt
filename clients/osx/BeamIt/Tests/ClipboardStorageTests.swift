import Foundation
import CryptoKit
import Darwin

// Standalone executable. Uses private fixtures only; never opens NSPasteboard.
@main
enum ClipboardStorageTests {
    typealias Bulk = BeamItClipboardBulk

    static func rejects(_ code: String? = nil, _ operation: () async throws -> Void) async throws {
        do {
            try await operation()
            preconditionFailure("An invalid transfer was accepted.")
        } catch let failure as BeamItClipboardFailure {
            precondition(code == nil || failure.code == code)
        }
    }

    static func main() async throws {
        // Check the production default without creating or migrating user directories.
        precondition(BeamItClipboardStorage.defaultRoot ==
            URL.homeDirectory.appendingPathComponent("Library/Application Support/BeamIt/ClipboardStaging"))
        let fs = FileManager.default
        guard let temporaryPath = realpath(fs.temporaryDirectory.path, nil) else {
            throw BeamItClipboardFailure.filesystem("filesystem_unavailable")
        }
        defer { free(temporaryPath) }
        let base = URL(fileURLWithPath: String(cString: temporaryPath)).appendingPathComponent(UUID().uuidString)
        try fs.createDirectory(at: base, withIntermediateDirectories: true)
        defer {
            do { try fs.removeItem(at: base) }
            catch { preconditionFailure("Private fixture cleanup failed.") }
        }
        let home = base.appendingPathComponent("home")
        try fs.createDirectory(at: home, withIntermediateDirectories: true)
        let homeStorage = BeamItClipboardStorage(stagingHome: home)
        try await homeStorage.recover()
        precondition(fs.fileExists(atPath: home.appendingPathComponent(
            "Library/Application Support/BeamIt/ClipboardStaging/client/partial").path))

        let unsafeHome = base.appendingPathComponent("unsafe-home")
        let outside = base.appendingPathComponent("outside")
        try fs.createDirectory(at: unsafeHome, withIntermediateDirectories: true)
        try fs.createDirectory(at: outside, withIntermediateDirectories: true)
        try fs.createSymbolicLink(at: unsafeHome.appendingPathComponent("Library"), withDestinationURL: outside)
        try await rejects { try await BeamItClipboardStorage(stagingHome: unsafeHome).recover() }
        let outsideContents = try fs.contentsOfDirectory(atPath: outside.path)
        precondition(outsideContents.isEmpty)

        let selected = base.appendingPathComponent("selection")
        try fs.createDirectory(at: selected.appendingPathComponent("empty"), withIntermediateDirectories: true)
        let payload = Data((0..<600_000).map { UInt8($0 % 251) })
        try payload.write(to: selected.appendingPathComponent("item"))
        try Data([42]).write(to: selected.appendingPathComponent(".hidden"))
        try Data().write(to: selected.appendingPathComponent("zero"))
        let senderRoot = base.appendingPathComponent("sender")
        let receiverRoot = base.appendingPathComponent("receiver")
        let sender = BeamItClipboardStorage(root: senderRoot)
        let receiver = BeamItClipboardStorage(root: receiverRoot)
        let selection = try await sender.inspect(urls: [selected], png: nil, width: nil, height: nil)
        precondition(selection.manifest.selectedRoots == 1 && selection.manifest.totalFileBytes == 600_001)
        precondition(selection.manifest.nodes.count == 5)
        precondition(selection.manifest.nodes.contains { $0.path == "selection/empty" && $0.kind == .directory })
        let manifest = try await sender.prepare(selection)
        precondition(manifest.nodes.first { $0.path == "selection/item" }?.sha256 == Data(SHA256.hash(data: payload)))
        let roundTrip = try Bulk.parse(Bulk.json(manifest))
        precondition(roundTrip == manifest)
        let wireID = Data(repeating: 7, count: 16), id = Bulk.base64URL(wireID)
        try await receiver.beginReceive(id, manifest: manifest)
        try await rejects("incomplete_transfer") { _ = try await receiver.promote(id) }
        let earlyEnd = try Bulk.encode(kind: .end, transferID: wireID, nodeIndex: .max,
                                      offset: 600_001, payload: Data())
        try await rejects("early_end") { _ = try await receiver.accept(id, frame: earlyEnd) }
        var frames = 0
        while true {
            let frame = try await sender.senderFrame(selection.id, transferID: wireID)
            let ended = try await receiver.accept(id, frame: frame)
            frames += 1
            if ended { break }
        }
        precondition(frames >= 5) // Hidden file, three bounded chunks, END; no empty-file payload.
        try await rejects("invalid_frame") { _ = try await receiver.accept(id, frame: earlyEnd) }
        let publication = try await receiver.promote(id)
        precondition(publication.files.count == 1 && publication.png == nil)
        let handle = try FileHandle(forReadingFrom: publication.files[0].appendingPathComponent("item"))
        let actual = try handle.readToEnd()
        try handle.close()
        precondition(actual == payload)
        precondition(fs.fileExists(atPath: publication.files[0].appendingPathComponent("empty").path))
        try await sender.finish(selection.id)
        try await receiver.finish(id, published: true)
        try await receiver.recover()
        precondition(fs.fileExists(atPath: publication.files[0].path))
        let outgoing = try fs.contentsOfDirectory(atPath: senderRoot.appendingPathComponent("client/outgoing").path)
        precondition(outgoing.isEmpty)

        // Revision/identity drift invalidates the captured preliminary selection.
        let changed = try await sender.inspect(urls: [selected], png: nil, width: nil, height: nil)
        try Data([1, 2]).write(to: selected.appendingPathComponent("item"), options: .atomic)
        let same = try await sender.unchanged(changed.id)
        precondition(!same)
        try await rejects("source_changed") { _ = try await sender.prepare(changed) }
        try await sender.finish(changed.id)
        let link = base.appendingPathComponent("link")
        try fs.createSymbolicLink(at: link, withDestinationURL: selected)
        try await rejects { _ = try await sender.inspect(urls: [link], png: nil, width: nil, height: nil) }

        // Cancellation removes only the unpublished partial operation.
        let smallBytes = Data([1, 2, 3])
        let small = Bulk.Manifest(selectedRoots: 1, totalFileBytes: 3, nodes: [
            .init(kind: .file, path: "item", length: 3, sha256: Data(SHA256.hash(data: smallBytes)), width: nil, height: nil)
        ])
        let cancelled = Bulk.base64URL(Data(repeating: 8, count: 16))
        try await receiver.beginReceive(cancelled, manifest: small)
        try await receiver.finish(cancelled)
        precondition(!fs.fileExists(atPath: receiverRoot.appendingPathComponent("client/partial/" + cancelled).path))
        precondition(fs.fileExists(atPath: publication.files[0].path))

        // A sparse logical retained file exercises quota without allocating 8 GB.
        let quotaRoot = base.appendingPathComponent("quota")
        let first = BeamItClipboardStorage(root: quotaRoot), second = BeamItClipboardStorage(root: quotaRoot)
        try await first.recover()
        let retained = quotaRoot.appendingPathComponent("client/committed/retained")
        precondition(fs.createFile(atPath: retained.path, contents: nil))
        let retainedHandle = try FileHandle(forWritingTo: retained)
        try retainedHandle.truncate(atOffset: 8_000_000_000 - 128 * 1024 * 1024 - 3)
        try retainedHandle.close()
        let liveID = Bulk.base64URL(Data(repeating: 9, count: 16))
        let otherID = Bulk.base64URL(Data(repeating: 10, count: 16))
        try await first.beginReceive(liveID, manifest: small)
        try await second.recover() // Another owner must not reclaim a held kernel lease.
        precondition(fs.fileExists(atPath: quotaRoot.appendingPathComponent("client/partial/" + liveID).path))
        try await rejects("quota_exceeded") { try await second.beginReceive(otherID, manifest: small) }
        try await first.finish(liveID)

        // An unlocked lease simulates process death; retained committed data survives.
        let abandoned = Bulk.base64URL(Data(repeating: 11, count: 16))
        let partial = quotaRoot.appendingPathComponent("client/partial/" + abandoned)
        try fs.createDirectory(at: partial, withIntermediateDirectories: true)
        try JSONSerialization.data(withJSONObject: ["kind": "partial", "bytes": 3])
            .write(to: quotaRoot.appendingPathComponent(".reservations/" + abandoned))
        try await second.recover()
        precondition(!fs.fileExists(atPath: partial.path) && fs.fileExists(atPath: retained.path))

        // The PNG decoder validates actual bytes and declared dimensions before promotion.
        let png = Data(base64Encoded: "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAAC0lEQVR4nGNgAAIAAAUAAXpeqz8AAAAASUVORK5CYII=")!
        let decoded = try await sender.image(png, isPNG: true)
        precondition(decoded.width == 1 && decoded.height == 1)
        let validImage = try await sender.inspect(urls: [], png: png, width: 1, height: 1)
        let validManifest = try await sender.prepare(validImage)
        let validRaw = Data(repeating: 13, count: 16), validID = Bulk.base64URL(validRaw)
        try await receiver.beginReceive(validID, manifest: validManifest)
        while true {
            let frame = try await sender.senderFrame(validImage.id, transferID: validRaw)
            if try await receiver.accept(validID, frame: frame) { break }
        }
        let validPublication = try await receiver.promote(validID)
        precondition(validPublication.files.isEmpty && validPublication.png == png)
        try await receiver.finish(validID, published: true)
        try await sender.finish(validImage.id)
        let imageSelection = try await sender.inspect(urls: [], png: png, width: 2, height: 1)
        let imageManifest = try await sender.prepare(imageSelection)
        let pngIDBytes = Data(repeating: 12, count: 16), pngID = Bulk.base64URL(pngIDBytes)
        try await receiver.beginReceive(pngID, manifest: imageManifest)
        while true {
            let frame = try await sender.senderFrame(imageSelection.id, transferID: pngIDBytes)
            if try await receiver.accept(pngID, frame: frame) { break }
        }
        try await rejects("invalid_png_dimensions") { _ = try await receiver.promote(pngID) }
        try await receiver.finish(pngID)
        try await sender.finish(imageSelection.id)
        print("Clipboard storage, ordering, revision, quota and retention checks passed.")
    }
}
