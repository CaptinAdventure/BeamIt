import Foundation
import CoreFoundation

nonisolated enum BeamItClipboardBulk {
    static let headerSize = 40
    static let chunkSize = 256 * 1024
    static let maxManifestBytes = 4 * 1024 * 1024
    static let maxRoots = 64
    static let maxNodes = 4096
    static let maxFileBytes: UInt64 = 2_000_000_000
    static let maxPNGBytes: UInt64 = 64 * 1024 * 1024
    static let defaultByteThreshold: UInt64 = 500_000_000
    static let defaultRootThreshold: UInt32 = 5

    enum FrameKind: UInt8, Sendable {
        case file = 1
        case png = 2
        case end = 3
    }

    struct Frame: Sendable {
        let kind: FrameKind
        let transferID: Data
        let nodeIndex: UInt32
        let offset: UInt64
        let payload: Data
    }

    enum NodeKind: String, Sendable {
        case directory
        case file
        case png
    }

    struct Node: Sendable, Equatable {
        let kind: NodeKind
        let path: String
        let length: UInt64?
        let sha256: Data?
        let width: UInt32?
        let height: UInt32?
    }

    struct Manifest: Sendable, Equatable {
        let selectedRoots: UInt32
        let totalFileBytes: UInt64
        let nodes: [Node]
    }

    struct TransferPolicy: Sendable {
        let byteThreshold: UInt64
        let rootThreshold: UInt32
    }

    struct RevisionGuard: Sendable {
        let revision: UInt64
        let sequence: Int
        func matches(revision: UInt64, sequence: Int) -> Bool {
            self.revision == revision && self.sequence == sequence
        }
    }

    enum ValidationError: Error, Equatable {
        case malformedFrame
        case invalidManifest
        case invalidPath
        case duplicatePath
        case invalidNode
        case invalidPolicy
    }

    static func encode(
        kind: FrameKind, transferID: Data, nodeIndex: UInt32, offset: UInt64, payload: Data
    ) throws -> Data {
        guard transferID.count == 16 else { throw ValidationError.malformedFrame }
        if kind == .end {
            guard nodeIndex == UInt32.max, payload.isEmpty else { throw ValidationError.malformedFrame }
        } else {
            guard nodeIndex != UInt32.max, !payload.isEmpty, payload.count <= chunkSize else {
                throw ValidationError.malformedFrame
            }
        }
        var result = Data([0x42, 0x4d, 0x42, 0x31, 1, kind.rawValue, 0, 0])
        result.append(transferID)
        append(UInt64(nodeIndex), width: 4, to: &result)
        append(offset, width: 8, to: &result)
        append(UInt64(payload.count), width: 4, to: &result)
        result.append(payload)
        return result
    }

    static func decode(_ data: Data) throws -> Frame {
        guard data.count >= headerSize,
              data[0..<4] == Data([0x42, 0x4d, 0x42, 0x31]),
              data[4] == 1, data[6] == 0, data[7] == 0,
              let kind = FrameKind(rawValue: data[5]),
              let node = read(data, offset: 24, width: 4),
              let offset = read(data, offset: 28, width: 8),
              let payloadCount = read(data, offset: 36, width: 4)
        else { throw ValidationError.malformedFrame }
        let end = kind == .end
        guard payloadCount <= UInt64(Int.max), Int(payloadCount) == data.count - headerSize else {
            throw ValidationError.malformedFrame
        }
        if end {
            guard node == UInt64(UInt32.max), payloadCount == 0 else { throw ValidationError.malformedFrame }
        } else {
            guard node < UInt64(UInt32.max), payloadCount > 0,
                  payloadCount <= UInt64(chunkSize) else { throw ValidationError.malformedFrame }
        }
        return Frame(kind: kind, transferID: data.subdata(in: 8..<24), nodeIndex: UInt32(node),
                     offset: offset, payload: data.subdata(in: headerSize..<data.count))
    }

    static func requiresConfirmation(bytes: UInt64, roots: UInt32, byteThreshold: UInt64,
                                     rootThreshold: UInt32) throws -> Bool {
        guard byteThreshold <= maxFileBytes, rootThreshold <= UInt32(maxRoots) else {
            throw ValidationError.invalidPolicy
        }
        return bytes >= byteThreshold || roots > rootThreshold
    }

    static func configuredPolicy(defaults: UserDefaults = .standard) -> TransferPolicy {
        let bytes = configuredInteger(
            "clipboardBulkByteThreshold", default: defaultByteThreshold, maximum: maxFileBytes, defaults: defaults
        )
        let roots = configuredInteger(
            "clipboardBulkEntryThreshold", default: defaultRootThreshold, maximum: UInt64(maxRoots),
            defaults: defaults
        )
        return TransferPolicy(byteThreshold: bytes, rootThreshold: UInt32(roots))
    }

    static func validatePath(_ path: String) throws {
        let components = path.split(separator: "/", omittingEmptySubsequences: false)
        guard !path.isEmpty, path.utf8.count <= 4096, path.first != "/", path.last != "/",
              components.allSatisfy({ component in
                  let value = String(component)
                  guard !value.isEmpty, value != ".", value != "..", value.utf8.count <= 255,
                        value.last != ".", value.last != " ",
                        !value.unicodeScalars.contains(where: { $0.value < 32 || (127...159).contains($0.value) }),
                        !value.contains(where: { "\\:*?\"<>|".contains($0) })
                  else { return false }
                  let stem = value.split(separator: ".", maxSplits: 1, omittingEmptySubsequences: false)
                      .first.map(String.init)?.uppercased() ?? value.uppercased()
                  return !["CON", "PRN", "AUX", "NUL"].contains(stem) &&
                      !(stem.count == 4 && (stem.hasPrefix("COM") || stem.hasPrefix("LPT")) &&
                        ("1"..."9").contains(String(stem.last!)))
              }) else { throw ValidationError.invalidPath }
    }

    static func collisionKey(_ path: String) -> String {
        path.precomposedStringWithCanonicalMapping.lowercased(with: Locale(identifier: "en_US_POSIX"))
    }

    static func validate(_ manifest: Manifest, encodedSize: Int) throws {
        guard encodedSize >= 0, encodedSize <= maxManifestBytes,
              manifest.selectedRoots <= UInt32(maxRoots), manifest.nodes.count <= maxNodes else {
            throw ValidationError.invalidManifest
        }
        var kinds: [String: NodeKind] = [:]
        var totalFileBytes: UInt64 = 0
        for node in manifest.nodes {
            try validatePath(node.path)
            let key = collisionKey(node.path)
            guard kinds[key] == nil else { throw ValidationError.duplicatePath }
            switch node.kind {
            case .directory:
                guard node.length == nil, node.sha256 == nil, node.width == nil, node.height == nil else {
                    throw ValidationError.invalidNode
                }
            case .file:
                guard let length = node.length, let hash = node.sha256, hash.count == 32,
                      node.width == nil, node.height == nil,
                      length <= Self.maxFileBytes - totalFileBytes else { throw ValidationError.invalidNode }
                totalFileBytes += length
            case .png:
                guard let length = node.length, let hash = node.sha256, hash.count == 32,
                      let width = node.width, let height = node.height, length > 0, length <= maxPNGBytes,
                      width > 0, height > 0, width <= 16_384, height <= 16_384,
                      UInt64(width) * UInt64(height) <= 64 * 1024 * 1024 else {
                    throw ValidationError.invalidNode
                }
            }
            kinds[key] = node.kind
        }
        guard totalFileBytes == manifest.totalFileBytes else { throw ValidationError.invalidManifest }
        let roots = Set(manifest.nodes.filter { $0.kind != .png }.map { $0.path.split(separator: "/")[0] })
        guard roots.count == Int(manifest.selectedRoots),
              manifest.nodes.filter({ $0.kind == .png }).count <= 1 else { throw ValidationError.invalidManifest }
        for key in kinds.keys {
            let components = key.split(separator: "/", omittingEmptySubsequences: false)
            if components.count > 1 {
                for count in 1..<components.count {
                    let parent = components.prefix(count).joined(separator: "/")
                    guard kinds[parent] == .directory else { throw ValidationError.invalidNode }
                }

            }
            if kinds[key] != .directory {
                let prefix = key + "/"
                guard !kinds.keys.contains(where: { $0.hasPrefix(prefix) }) else {
                    throw ValidationError.invalidNode
                }
            }
        }
    }

    static func base64URL(_ data: Data) -> String {
        data.base64EncodedString().replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_").replacingOccurrences(of: "=", with: "")
    }

    static func decodeID(_ value: String, bytes: Int) throws -> Data {
        let padded = value.replacingOccurrences(of: "-", with: "+").replacingOccurrences(of: "_", with: "/")
        guard let data = Data(base64Encoded: padded + String(repeating: "=", count: (4 - padded.count % 4) % 4)),
              data.count == bytes, base64URL(data) == value else { throw ValidationError.invalidManifest }
        return data
    }

    static func json(_ manifest: Manifest, hashes: Bool = true) -> [String: Any] {
        ["selected_roots": manifest.selectedRoots, "total_file_bytes": manifest.totalFileBytes,
         "nodes": manifest.nodes.map { node -> [String: Any] in
             var item: [String: Any] = ["type": node.kind.rawValue, "path": node.path]
             if let length = node.length { item["length"] = length }
             if hashes, let hash = node.sha256 { item["sha256"] = base64URL(hash) }
             if let width = node.width { item["width"] = width }
             if let height = node.height { item["height"] = height }
             return item
         }]
    }

    static func unsigned(_ value: Any?) throws -> UInt64 {
        guard let number = value as? NSNumber, CFGetTypeID(number) != CFBooleanGetTypeID(),
              let result = UInt64(number.stringValue) else { throw ValidationError.invalidManifest }
        return result
    }

    static func isNonfatal(_ value: Any?) -> Bool {
        guard let number = value as? NSNumber, CFGetTypeID(number) == CFBooleanGetTypeID() else { return false }
        return !number.boolValue
    }

    static func parse(_ object: [String: Any], preliminary: Bool = false) throws -> Manifest {
        let data = try JSONSerialization.data(withJSONObject: object)
        guard data.count <= maxManifestBytes, let array = object["nodes"] as? [[String: Any]] else {
            throw ValidationError.invalidManifest
        }
        let count = try unsigned(object["selected_roots"])
        guard count <= UInt32.max else { throw ValidationError.invalidManifest }
        let nodes = try array.map { item -> Node in
            guard let kind = (item["type"] as? String).flatMap(NodeKind.init(rawValue:)),
                  let path = item["path"] as? String else { throw ValidationError.invalidNode }
            if kind == .directory {
                guard ["length", "sha256", "width", "height"].allSatisfy({ item[$0] == nil }) else {
                    throw ValidationError.invalidNode
                }
                return Node(kind: kind, path: path, length: nil, sha256: nil, width: nil, height: nil)
            }
            let hash: Data
            if preliminary {
                guard item["sha256"] == nil else { throw ValidationError.invalidNode }
                hash = Data(repeating: 0, count: 32)
            } else {
                guard let encoded = item["sha256"] as? String else { throw ValidationError.invalidNode }
                hash = try decodeID(encoded, bytes: 32)
            }
            let width = kind == .png ? try unsigned(item["width"]) : nil
            let height = kind == .png ? try unsigned(item["height"]) : nil
            guard (width ?? 0) <= UInt64(UInt32.max), (height ?? 0) <= UInt64(UInt32.max),
                  kind == .png || (item["width"] == nil && item["height"] == nil) else {
                throw ValidationError.invalidNode
            }
            return Node(kind: kind, path: path, length: try unsigned(item["length"]), sha256: hash,
                        width: width.map(UInt32.init), height: height.map(UInt32.init))
        }
        let result = Manifest(selectedRoots: UInt32(count), totalFileBytes: try unsigned(object["total_file_bytes"]), nodes: nodes)
        try validate(result, encodedSize: data.count)
        return result
    }

    private static func append(_ value: UInt64, width: Int, to data: inout Data) {
        for shift in (0..<width).reversed() {
            data.append(UInt8((value >> (shift * 8)) & 0xff))
        }
    }

    private static func configuredInteger(
        _ key: String, default fallback: UInt64, maximum: UInt64, defaults: UserDefaults
    ) -> UInt64 {
        guard let raw = defaults.object(forKey: key) else { return fallback }
        guard let number = raw as? NSNumber, CFGetTypeID(number) != CFBooleanGetTypeID(),
              let value = UInt64(number.stringValue), value <= maximum else {
            NSLog("BeamIt: Invalid %@; using the documented default.", key)
            return fallback
        }
        return value
    }

    private static func read(_ data: Data, offset: Int, width: Int) -> UInt64? {
        guard offset >= 0, width > 0, offset <= data.count, width <= data.count - offset else { return nil }
        var value: UInt64 = 0
        for index in offset..<(offset + width) {
            value = (value << 8) | UInt64(data[index])
        }
        return value
    }
}
