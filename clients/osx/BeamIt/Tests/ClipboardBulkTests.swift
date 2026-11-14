import Foundation

@main
enum ClipboardBulkTests {
    static func main() throws {
        let transferID = Data((0..<16).map(UInt8.init))
        let payload = Data([0x41, 0x42, 0x43])
        let encoded = try BeamItClipboardBulk.encode(
            kind: .file, transferID: transferID, nodeIndex: 2, offset: 0x0102030405060708,
            payload: payload
        )
        let decoded = try BeamItClipboardBulk.decode(encoded)
        precondition(decoded.kind == .file)
        precondition(decoded.transferID == transferID)
        precondition(decoded.nodeIndex == 2)
        precondition(decoded.offset == 0x0102030405060708)
        precondition(decoded.payload == payload)
        let revision = BeamItClipboardBulk.RevisionGuard(revision: 7, sequence: 12)
        precondition(revision.matches(revision: 7, sequence: 12))
        precondition(!revision.matches(revision: 8, sequence: 12))
        precondition(!revision.matches(revision: 7, sequence: 13))

        var reserved = encoded
        reserved[6] = 1
        precondition(throwsError { try BeamItClipboardBulk.decode(reserved) })
        let end = try BeamItClipboardBulk.encode(
            kind: .end, transferID: transferID, nodeIndex: UInt32.max, offset: 0, payload: Data()
        )
        let decodedEnd = try BeamItClipboardBulk.decode(end)
        precondition(decodedEnd.kind == .end)

        let automatic = try BeamItClipboardBulk.requiresConfirmation(
            bytes: 499_999_999, roots: 5, byteThreshold: 500_000_000, rootThreshold: 5
        )
        precondition(!automatic)
        let byteConfirmation = try BeamItClipboardBulk.requiresConfirmation(
            bytes: 500_000_000, roots: 5, byteThreshold: 500_000_000, rootThreshold: 5
        )
        precondition(byteConfirmation)
        let rootConfirmation = try BeamItClipboardBulk.requiresConfirmation(
            bytes: 1, roots: 6, byteThreshold: 500_000_000, rootThreshold: 5
        )
        precondition(rootConfirmation)
        try BeamItClipboardBulk.validatePath("folder/naïve.txt")
        precondition(BeamItClipboardBulk.collisionKey("Å") == BeamItClipboardBulk.collisionKey("A\u{030A}"))
        precondition(throwsError { try BeamItClipboardBulk.validatePath("../outside") })
        for path in ["/absolute", "a//b", "a/..", "CON.txt", "a:b", "a\\b", "trailing.",
                     String(repeating: "é", count: 128), "a\u{0000}b"] {
            precondition(throwsError { try BeamItClipboardBulk.validatePath(path) })
        }
        let invalidNumbers: [Any] = [true, -1, 1.5, "12", NSNull()]
        for value in invalidNumbers {
            precondition(throwsError { try BeamItClipboardBulk.unsigned(value) })
        }
        precondition(BeamItClipboardBulk.isNonfatal(false))
        precondition(!BeamItClipboardBulk.isNonfatal(true))
        precondition(!BeamItClipboardBulk.isNonfatal(0))
        precondition(!BeamItClipboardBulk.isNonfatal("false"))
        precondition(!BeamItClipboardBulk.isNonfatal(nil))
        let validID = BeamItClipboardBulk.base64URL(transferID)
        let parsedID = try BeamItClipboardBulk.decodeID(validID, bytes: 16)
        precondition(parsedID == transferID)
        precondition(throwsError { try BeamItClipboardBulk.decodeID(validID + "=", bytes: 16) })
        let directory = BeamItClipboardBulk.Node(
            kind: .directory, path: "folder", length: nil, sha256: nil, width: nil, height: nil
        )
        let file = BeamItClipboardBulk.Node(
            kind: .file, path: "folder/a.txt", length: 3, sha256: Data(repeating: 0, count: 32),
            width: nil, height: nil
        )
        try BeamItClipboardBulk.validate(
            .init(selectedRoots: 1, totalFileBytes: 3, nodes: [directory, file]), encodedSize: 128
        )
        let manifest = BeamItClipboardBulk.Manifest(selectedRoots: 1, totalFileBytes: 3, nodes: [directory, file])
        let final = try BeamItClipboardBulk.parse(BeamItClipboardBulk.json(manifest))
        precondition(final == manifest)
        let preliminary = try BeamItClipboardBulk.parse(BeamItClipboardBulk.json(manifest, hashes: false), preliminary: true)
        precondition(preliminary == manifest)
        precondition(throwsError { try BeamItClipboardBulk.parse(BeamItClipboardBulk.json(manifest), preliminary: true) })
        let duplicate = BeamItClipboardBulk.Node(kind: .file, path: "FOLDER/A.TXT", length: 0,
            sha256: Data(repeating: 0, count: 32), width: nil, height: nil)
        precondition(throwsError {
            try BeamItClipboardBulk.validate(.init(selectedRoots: 1, totalFileBytes: 3,
                nodes: [directory, file, duplicate]), encodedSize: 300)
        })
        precondition(throwsError {
            try BeamItClipboardBulk.validate(
                .init(selectedRoots: 1, totalFileBytes: 3, nodes: [file]), encodedSize: 128
            )
        })
        let suite = UUID().uuidString
        let defaults = UserDefaults(suiteName: suite)!
        defer { defaults.removePersistentDomain(forName: suite) }
        let defaultPolicy = BeamItClipboardBulk.configuredPolicy(defaults: defaults)
        precondition(defaultPolicy.byteThreshold == 500_000_000 && defaultPolicy.rootThreshold == 5)
        defaults.set(0, forKey: "clipboardBulkByteThreshold")
        defaults.set(64, forKey: "clipboardBulkEntryThreshold")
        let configuredPolicy = BeamItClipboardBulk.configuredPolicy(defaults: defaults)
        precondition(configuredPolicy.byteThreshold == 0 && configuredPolicy.rootThreshold == 64)
        defaults.set(2_000_000_001, forKey: "clipboardBulkByteThreshold")
        defaults.set(65, forKey: "clipboardBulkEntryThreshold")
        let invalidPolicy = BeamItClipboardBulk.configuredPolicy(defaults: defaults)
        precondition(invalidPolicy.byteThreshold == 500_000_000 && invalidPolicy.rootThreshold == 5)
        print("Clipboard bulk protocol checks passed.")
    }

    private static func throwsError(_ operation: () throws -> Any) -> Bool {
        do {
            _ = try operation()
            return false
        } catch {
            return true
        }
    }
}
