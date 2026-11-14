import Foundation
import CryptoKit
import Darwin
import Security
import Synchronization
import ImageIO
import UniformTypeIdentifiers

nonisolated final class BeamItClipboardProgress: Sendable {
    private let value = Mutex<UInt64>(0)
    func add(_ bytes: Int) { value.withLock { $0 += UInt64(bytes) } }
    func read() -> UInt64 { value.withLock { $0 } }
}

nonisolated struct BeamItClipboardFailure: LocalizedError, Sendable {
    let code: String
    var errorDescription: String? { "Clipboard transfer failed (\(code)). No success was reported." }
    init(_ code: String) { self.code = code }
    static func filesystem(_ fallback: String, code: Int32 = errno) -> Self {
        switch code {
        case EACCES, EPERM: return Self("permission_denied")
        case ENOSPC, EDQUOT: return Self("disk_full")
        case ELOOP, ENOTDIR: return Self("unsafe_path")
        default: return Self(fallback)
        }
    }
}

// All filesystem access and hashing are isolated here, never on the UI actor.
// No descriptor, mutable digest or filesystem object crosses the actor boundary.
actor BeamItClipboardStorage {
    typealias Bulk = BeamItClipboardBulk
    nonisolated struct Selection: Sendable { let id: String; let manifest: Bulk.Manifest }
    nonisolated struct Publication: Sendable { let files: [URL]; let png: Data? }
    private nonisolated struct Stamp: Equatable {
        let device: Int32
        let inode: UInt64
        let mode: UInt16
        let size: Int64
        let links: UInt64
        let modified: timespec
        let changed: timespec
        static func == (a: Self, b: Self) -> Bool {
            a.device == b.device && a.inode == b.inode && a.mode == b.mode && a.size == b.size && a.links == b.links &&
            a.modified.tv_sec == b.modified.tv_sec && a.modified.tv_nsec == b.modified.tv_nsec &&
            a.changed.tv_sec == b.changed.tv_sec && a.changed.tv_nsec == b.changed.tv_nsec
        }
    }
    private nonisolated final class FD {
        let raw: Int32
        init(_ value: Int32) throws {
            guard value >= 0 else { throw BeamItClipboardFailure.filesystem("filesystem_unavailable") }
            raw = value
        }
        deinit { Darwin.close(raw) }
    }
    private nonisolated final class SecurityScope {
        let url: URL
        let started: Bool
        init(_ url: URL) { self.url = url; started = url.startAccessingSecurityScopedResource() }
        deinit { if started { url.stopAccessingSecurityScopedResource() } }
    }
    private nonisolated struct Source {
        let urls: [URL]
        let scopes: [SecurityScope]
        var manifest: Bulk.Manifest
        let stamps: [String: Stamp]
        let rootIndices: [String: Int]
        let png: Data?
    }
    private nonisolated final class Transfer {
        let id: String
        let kind: String
        let lease: FD
        let directory: FD
        var manifest: Bulk.Manifest
        var node = 0
        var offset: UInt64 = 0
        var digest = SHA256()
        var file: FD?
        var identities: [String: Stamp] = [:]
        var ended = false
        var promoted = false
        init(id: String, kind: String, lease: FD, directory: FD, manifest: Bulk.Manifest) {
            self.id = id; self.kind = kind; self.lease = lease
            self.directory = directory; self.manifest = manifest
        }
    }
    private let root: URL
    private let stagingHome: URL?
    private var sources: [String: Source] = [:]
    private var transfers: [String: Transfer] = [:]
    private let quota: UInt64 = 8_000_000_000
    private let scratch: UInt64 = 128 * 1024 * 1024
    private nonisolated static let stagingPath = "Library/Application Support/BeamIt/ClipboardStaging"

    nonisolated static var defaultRoot: URL {
        URL.homeDirectory.appendingPathComponent(stagingPath)
    }

    init(root: URL? = nil, stagingHome: URL? = nil) {
        let home = stagingHome ?? URL.homeDirectory
        self.root = root ?? home.appendingPathComponent(Self.stagingPath)
        self.stagingHome = root == nil ? home : nil
    }

    private func stamp(_ fd: FD) throws -> Stamp {
        var info = stat()
        guard fstat(fd.raw, &info) == 0,
              (info.st_mode & S_IFMT) == S_IFREG || (info.st_mode & S_IFMT) == S_IFDIR else {
            throw BeamItClipboardFailure("unsupported_entry")
        }
        return Stamp(device: info.st_dev, inode: info.st_ino, mode: info.st_mode, size: info.st_size, links: UInt64(info.st_nlink),
                     modified: info.st_mtimespec, changed: info.st_ctimespec)
    }

    private func openPath(_ url: URL, create: Bool = false) throws -> FD {
        guard url.isFileURL, url.path.hasPrefix("/"), url.host == nil || url.host == "" || url.host == "localhost" else {
            throw BeamItClipboardFailure("unsafe_path")
        }
        // Metadata-only ancestor descriptors permit sandbox traversal without
        // requesting directory-listing access outside the selected subtree.
        var current = try FD(Darwin.open("/", O_EVTONLY | O_DIRECTORY | O_CLOEXEC))
        let parts = url.path.split(separator: "/").map(String.init)
        for (index, part) in parts.enumerated() {
            guard part != ".", part != ".." else { throw BeamItClipboardFailure("unsafe_path") }
            let ancestor = index + 1 < parts.count
            let flags = (ancestor ? O_EVTONLY : O_RDONLY) | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC |
                (ancestor || create ? O_DIRECTORY : 0)
            var descriptor = openat(current.raw, part, flags)
            if descriptor < 0 && errno == ENOENT && create {
                guard mkdirat(current.raw, part, 0o700) == 0 || errno == EEXIST else {
                    throw BeamItClipboardFailure.filesystem("staging_unavailable")
                }
                descriptor = openat(current.raw, part, flags)
            }
            current = try FD(descriptor)
            _ = try stamp(current)
        }
        return current
    }

    private func child(_ parent: FD, _ path: String, createDirectories: Bool = false,
                       createFile: Bool = false) throws -> FD {
        try Bulk.validatePath(path)
        var current = try FD(dup(parent.raw))
        let parts = path.split(separator: "/").map(String.init)
        for (index, part) in parts.enumerated() {
            let last = index == parts.count - 1
            if createDirectories && (!last || !createFile) &&
                mkdirat(current.raw, part, 0o700) != 0 && errno != EEXIST {
                throw BeamItClipboardFailure.filesystem("staging_unavailable")
            }
            let flags = O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC |
                (last && createFile ? O_RDWR | O_CREAT | O_EXCL : O_RDONLY | (!last || createDirectories ? O_DIRECTORY : 0))
            current = try FD(openat(current.raw, part, flags, 0o600))
            _ = try stamp(current)
        }
        return current
    }

    private func names(_ directory: FD, maximum: Int? = nil) throws -> [String] {
        let copy = dup(directory.raw)
        guard copy >= 0 else { throw BeamItClipboardFailure("filesystem_unavailable") }
        guard let stream = fdopendir(copy) else { Darwin.close(copy); throw BeamItClipboardFailure("filesystem_unavailable") }
        defer { closedir(stream) }
        rewinddir(stream)
        var result: [String] = []
        errno = 0
        while let entry = readdir(stream) {
            let count = Int(entry.pointee.d_namlen) + 1
            let name = withUnsafePointer(to: entry.pointee.d_name) {
                $0.withMemoryRebound(to: CChar.self, capacity: count) { String(validatingCString: $0) }
            }
            guard let name else { throw BeamItClipboardFailure("invalid_unicode") }
            if name != "." && name != ".." {
                if let maximum, result.count >= maximum { throw BeamItClipboardFailure("too_many_nodes") }
                result.append(name)
            }
            errno = 0
        }
        guard errno == 0 else { throw BeamItClipboardFailure("filesystem_unavailable") }
        return result.sorted()
    }

    private func write(_ data: Data, to fd: FD) throws {
        try data.withUnsafeBytes { bytes in
            var offset = 0
            while offset < bytes.count {
                let count = Darwin.write(fd.raw, bytes.baseAddress!.advanced(by: offset), bytes.count - offset)
                if count < 0 && errno == EINTR { continue }
                guard count > 0 else { throw BeamItClipboardFailure.filesystem("disk_write_failed") }
                offset += count
            }
        }
    }
    private func read(_ fd: FD, maximum: Int) throws -> Data {
        var data = Data(count: maximum)
        let count = data.withUnsafeMutableBytes { Darwin.read(fd.raw, $0.baseAddress, maximum) }
        if count < 0 && errno == EINTR { return try read(fd, maximum: maximum) }
        guard count >= 0 else { throw BeamItClipboardFailure.filesystem("disk_read_failed") }
        data.count = count
        return data
    }

    private func enumerate(_ urls: [URL], png: Data?, width: UInt32?, height: UInt32?) throws -> Source {
        guard urls.count <= Bulk.maxRoots else { throw BeamItClipboardFailure("too_many_roots") }
        let scopes = urls.map(SecurityScope.init)
        var nodes: [Bulk.Node] = [], stamps: [String: Stamp] = [:], indices: [String: Int] = [:]
        var total: UInt64 = 0
        for (index, url) in urls.enumerated() {
            let base = url.lastPathComponent
            var pending = [base]
            while let path = pending.popLast() {
                try Task.checkCancellation()
                try Bulk.validatePath(path)
                guard nodes.count < Bulk.maxNodes, stamps[path] == nil else { throw BeamItClipboardFailure("unsupported_selection") }
                let relative = String(path.dropFirst(base.count)).trimmingCharacters(in: CharacterSet(charactersIn: "/"))
                let root = try openPath(url)
                let fd = relative.isEmpty ? root : try child(root, relative)
                let info = try stamp(fd)
                let directory = (info.mode & S_IFMT) == S_IFDIR
                guard info.size >= 0 else { throw BeamItClipboardFailure("unsupported_entry") }
                let size = directory ? 0 : UInt64(info.size)
                guard size <= Bulk.maxFileBytes - total else { throw BeamItClipboardFailure("too_many_bytes") }
                total += size; stamps[path] = info; indices[path] = index
                nodes.append(.init(kind: directory ? .directory : .file, path: path,
                                   length: directory ? nil : size,
                                   sha256: directory ? nil : Data(repeating: 0, count: 32), width: nil, height: nil))
                if directory {
                    for name in try names(fd, maximum: Bulk.maxNodes - nodes.count - pending.count).reversed() {
                        pending.append(path + "/" + name)
                    }
                    guard try stamp(fd) == info else { throw BeamItClipboardFailure("source_changed") }
                }
            }
        }
        if let png {
            nodes.append(.init(kind: .png, path: "clipboard-image-\(UUID().uuidString).png",
                               length: UInt64(png.count), sha256: Data(repeating: 0, count: 32), width: width, height: height))
        }
        let manifest = Bulk.Manifest(selectedRoots: UInt32(urls.count), totalFileBytes: total, nodes: nodes)
        try Bulk.validate(manifest, encodedSize: JSONSerialization.data(withJSONObject: Bulk.json(manifest)).count)
        return Source(urls: urls, scopes: scopes, manifest: manifest, stamps: stamps, rootIndices: indices, png: png)
    }

    func inspect(urls: [URL], png: Data?, width: UInt32?, height: UInt32?) throws -> Selection {
        let source = try enumerate(urls, png: png, width: width, height: height)
        var random = Data(count: 16)
        guard random.withUnsafeMutableBytes({ SecRandomCopyBytes(kSecRandomDefault, 16, $0.baseAddress!) }) == errSecSuccess else {
            throw BeamItClipboardFailure("random_failed")
        }
        let id = Bulk.base64URL(random)
        sources[id] = source
        return Selection(id: id, manifest: source.manifest)
    }
    private func openSource(_ source: Source, path: String) throws -> FD {
        guard let index = source.rootIndices[path] else { throw BeamItClipboardFailure("source_changed") }
        let root = try openPath(source.urls[index])
        let relative = path.split(separator: "/").dropFirst().joined(separator: "/")
        return relative.isEmpty ? root : try child(root, relative)
    }
    func unchanged(_ id: String) throws -> Bool {
        guard let source = sources[id] else { return false }
        let fresh = try enumerate(source.urls, png: nil, width: nil, height: nil)
        return fresh.stamps == source.stamps && fresh.manifest.nodes == source.manifest.nodes.filter { $0.kind != .png }
    }

    private func rootFD() throws -> FD {
        let fd: FD
        if let stagingHome {
            // Sandboxed apps cannot open ancestors of their container with openat.
            let home = try FD(Darwin.open(stagingHome.path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC))
            fd = try child(home, Self.stagingPath, createDirectories: true)
        } else {
            fd = try openPath(root, create: true)
        }
        var info = stat()
        guard fstat(fd.raw, &info) == 0, info.st_uid == geteuid(), fchmod(fd.raw, 0o700) == 0 else {
            throw BeamItClipboardFailure("unsafe_staging")
        }
        for path in ["client", "client/partial", "client/outgoing", "client/committed", ".reservations"] {
            _ = try child(fd, path, createDirectories: true)
        }
        return fd
    }
    private func lock(_ root: FD) throws -> FD {
        let fd = try FD(openat(root.raw, ".quota-lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0o600))
        guard flock(fd.raw, LOCK_EX) == 0 else { throw BeamItClipboardFailure("quota_lock_failed") }
        return fd // Closing releases the kernel lock.
    }
    private func usage(_ directory: FD) throws -> UInt64 {
        var total: UInt64 = 0
        for name in try names(directory) {
            let fd = try child(directory, name)
            let info = try stamp(fd)
            let size = (info.mode & S_IFMT) == S_IFDIR ? try usage(fd) : UInt64(max(0, info.size))
            guard size <= quota, total <= quota - size else { throw BeamItClipboardFailure("quota_exceeded") }
            total += size
        }
        return total
    }
    private func removeTree(_ parent: FD, name: String) throws {
        var info = stat()
        if fstatat(parent.raw, name, &info, AT_SYMLINK_NOFOLLOW) != 0 {
            if errno == ENOENT { return }
            throw BeamItClipboardFailure("cleanup_failed")
        }
        if info.st_mode & S_IFMT == S_IFDIR {
            let fd = try child(parent, name)
            for entry in try names(fd) { try removeTree(fd, name: entry) }
            guard unlinkat(parent.raw, name, AT_REMOVEDIR) == 0 else { throw BeamItClipboardFailure("cleanup_failed") }
        } else {
            guard unlinkat(parent.raw, name, 0) == 0 else { throw BeamItClipboardFailure("cleanup_failed") }
        }
    }
    private func reconcile(_ root: FD) throws -> UInt64 {
        let ledgers = try child(root, ".reservations")
        var outstanding: UInt64 = 0
        for name in try names(ledgers) {
            _ = try Bulk.decodeID(name, bytes: 16)
            let fd = try FD(openat(ledgers.raw, name, O_RDWR | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC))
            let ledgerInfo = try stamp(fd)
            guard ledgerInfo.mode & S_IFMT == S_IFREG, ledgerInfo.links == 1 else {
                throw BeamItClipboardFailure("invalid_quota_ledger")
            }
            let record = try read(fd, maximum: 4097)
            guard record.count <= 4096,
                  let object = try JSONSerialization.jsonObject(with: record) as? [String: Any],
                  let kind = object["kind"] as? String, ["partial", "outgoing"].contains(kind) else {
                throw BeamItClipboardFailure("invalid_quota_ledger")
            }
            let bytes = try Bulk.unsigned(object["bytes"])
            guard bytes <= quota else { throw BeamItClipboardFailure("invalid_quota_ledger") }
            let operations = try child(root, "client/" + kind)
            if flock(fd.raw, LOCK_EX | LOCK_NB) == 0 {
                // Only an abandoned kernel lease can be reclaimed; live owners
                // remain locked, independent of PID reuse and process inspection permissions.
                try removeTree(operations, name: name)
                guard unlinkat(ledgers.raw, name, 0) == 0 else { throw BeamItClipboardFailure("cleanup_failed") }
            } else {
                guard errno == EWOULDBLOCK else { throw BeamItClipboardFailure("quota_lock_failed") }
                let materialized: UInt64
                var info = stat()
                if fstatat(operations.raw, name, &info, AT_SYMLINK_NOFOLLOW) == 0 {
                    materialized = try usage(child(operations, name))
                } else if errno == ENOENT { materialized = 0 }
                else { throw BeamItClipboardFailure("unsafe_staging") }
                let remaining = bytes - min(bytes, materialized)
                guard outstanding <= quota - scratch, remaining <= quota - scratch - outstanding else {
                    throw BeamItClipboardFailure("quota_exceeded")
                }
                outstanding += remaining + scratch
            }
        }
        return outstanding
    }
    func recover() throws {
        let root = try rootFD(), mutex = try lock(root)
        _ = try reconcile(root)
        withExtendedLifetime(mutex) {}
    }
    private func reserve(_ id: String, manifest: Bulk.Manifest, kind: String) throws -> Transfer {
        _ = try Bulk.decodeID(id, bytes: 16)
        guard transfers[id] == nil else { throw BeamItClipboardFailure("transfer_busy") }
        let root = try rootFD(), mutex = try lock(root)
        defer { withExtendedLifetime(mutex) {} }
        let outstanding = try reconcile(root)
        var actual: UInt64 = 0
        for name in try names(root) where name != ".reservations" && name != ".quota-lock" {
            let fd = try child(root, name)
            let info = try stamp(fd)
            let bytes = (info.mode & S_IFMT) == S_IFDIR ? try usage(fd) : UInt64(max(0, info.size))
            guard bytes <= quota, actual <= quota - bytes else { throw BeamItClipboardFailure("quota_exceeded") }
            actual += bytes
        }
        let bytes = manifest.nodes.reduce(UInt64(0)) { $0 + ($1.length ?? 0) }
        guard bytes <= quota - scratch, actual <= quota - bytes - scratch,
              outstanding <= quota - bytes - scratch - actual else { throw BeamItClipboardFailure("quota_exceeded") }
        var space = statfs()
        guard fstatfs(root.raw, &space) == 0,
              UInt64(space.f_bavail) * UInt64(space.f_bsize) >= bytes + scratch + outstanding else {
            throw BeamItClipboardFailure("insufficient_space")
        }
        let ledgers = try child(root, ".reservations")
        let lease = try FD(openat(ledgers.raw, id, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0o600))
        guard flock(lease.raw, LOCK_EX | LOCK_NB) == 0 else { throw BeamItClipboardFailure("quota_lock_failed") }
        do {
            try write(JSONSerialization.data(withJSONObject: ["kind": kind, "bytes": bytes]), to: lease)
            guard fsync(lease.raw) == 0 else { throw BeamItClipboardFailure("disk_write_failed") }
            let directory = try child(root, "client/" + kind + "/" + id, createDirectories: true)
            let transfer = Transfer(id: id, kind: kind, lease: lease, directory: directory, manifest: manifest)
            transfers[id] = transfer
            return transfer
        } catch {
            // If cleanup itself fails, retain the locked ledger until this owner
            // unwinds; recovery can then reclaim the abandoned partial safely.
            try removeTree(child(root, "client/" + kind), name: id)
            guard unlinkat(ledgers.raw, id, 0) == 0 else { throw BeamItClipboardFailure("cleanup_failed") }
            throw error
        }
    }

    func prepare(_ selection: Selection, progress: BeamItClipboardProgress = BeamItClipboardProgress()) throws -> Bulk.Manifest {
        guard let source = sources[selection.id], try unchanged(selection.id) else {
            throw BeamItClipboardFailure("source_changed")
        }
        let transfer = try reserve(selection.id, manifest: source.manifest, kind: "outgoing")
        var nodes: [Bulk.Node] = []
        for node in source.manifest.nodes {
            try Task.checkCancellation()
            if node.kind == .directory {
                _ = try child(transfer.directory, node.path, createDirectories: true)
                nodes.append(node); continue
            }
            let output = try child(transfer.directory, node.path, createDirectories: true, createFile: true)
            var digest = SHA256(), copied: UInt64 = 0
            if node.kind == .png {
                guard let png = source.png else { throw BeamItClipboardFailure("invalid_png") }
                for offset in stride(from: 0, to: png.count, by: Bulk.chunkSize) {
                    try Task.checkCancellation()
                    let part = png.subdata(in: offset..<min(png.count, offset + Bulk.chunkSize))
                    try write(part, to: output); digest.update(data: part); copied += UInt64(part.count); progress.add(part.count)
                }
            } else {
                let input = try openSource(source, path: node.path)
                guard try stamp(input) == source.stamps[node.path] else { throw BeamItClipboardFailure("source_changed") }
                while copied < node.length! {
                    try Task.checkCancellation()
                    let part = try read(input, maximum: Int(min(UInt64(Bulk.chunkSize), node.length! - copied)))
                    guard !part.isEmpty else { throw BeamItClipboardFailure("source_changed") }
                    try write(part, to: output); digest.update(data: part); copied += UInt64(part.count); progress.add(part.count)
                }
                guard try stamp(input) == source.stamps[node.path] else { throw BeamItClipboardFailure("source_changed") }
            }
            guard copied == node.length else { throw BeamItClipboardFailure("source_changed") }
            guard fsync(output.raw) == 0, fchmod(output.raw, 0o400) == 0 else {
                throw BeamItClipboardFailure.filesystem("disk_write_failed")
            }
            transfer.identities[node.path] = try stamp(output)
            nodes.append(.init(kind: node.kind, path: node.path, length: copied, sha256: Data(digest.finalize()),
                               width: node.width, height: node.height))
        }
        guard try unchanged(selection.id) else { throw BeamItClipboardFailure("source_changed") }
        transfer.manifest = .init(selectedRoots: selection.manifest.selectedRoots,
                                  totalFileBytes: selection.manifest.totalFileBytes, nodes: nodes)
        return transfer.manifest
    }
    func senderFrame(_ id: String, transferID: Data) throws -> Data {
        guard let transfer = transfers[id], !transfer.ended else { throw BeamItClipboardFailure("invalid_transfer") }
        while transfer.node < transfer.manifest.nodes.count &&
            transfer.offset == (transfer.manifest.nodes[transfer.node].length ?? 0) {
            transfer.node += 1; transfer.offset = 0; transfer.file = nil
        }
        if transfer.node == transfer.manifest.nodes.count {
            guard try unchanged(id) else { throw BeamItClipboardFailure("source_changed") }
            transfer.ended = true
            return try Bulk.encode(kind: .end, transferID: transferID, nodeIndex: .max,
                                   offset: transfer.manifest.nodes.reduce(0) { $0 + ($1.length ?? 0) }, payload: Data())
        }
        let node = transfer.manifest.nodes[transfer.node]
        if transfer.file == nil { transfer.file = try child(transfer.directory, node.path) }
        guard try stamp(transfer.file!) == transfer.identities[node.path] else { throw BeamItClipboardFailure("spool_changed") }
        let part = try read(transfer.file!, maximum: Int(min(UInt64(Bulk.chunkSize), node.length! - transfer.offset)))
        guard !part.isEmpty, try stamp(transfer.file!) == transfer.identities[node.path] else {
            throw BeamItClipboardFailure("spool_changed")
        }
        let frame = try Bulk.encode(kind: node.kind == .png ? .png : .file, transferID: transferID,
                                    nodeIndex: UInt32(transfer.node), offset: transfer.offset, payload: part)
        transfer.offset += UInt64(part.count)
        return frame
    }
    func beginReceive(_ id: String, manifest: Bulk.Manifest) throws {
        try Bulk.validate(manifest, encodedSize: JSONSerialization.data(withJSONObject: Bulk.json(manifest)).count)
        let transfer = try reserve(id, manifest: manifest, kind: "partial")
        for node in manifest.nodes {
            let fd = try child(transfer.directory, node.path, createDirectories: true, createFile: node.kind != .directory)
            transfer.identities[node.path] = try stamp(fd)
        }
        for node in manifest.nodes where node.kind == .directory {
            transfer.identities[node.path] = try stamp(child(transfer.directory, node.path))
        }
    }
    private func skipEmpty(_ transfer: Transfer) throws {
        while transfer.node < transfer.manifest.nodes.count {
            let node = transfer.manifest.nodes[transfer.node]
            if node.kind == .directory { transfer.node += 1; continue }
            guard node.length == 0 else { break }
            guard node.sha256 == Data(SHA256.hash(data: Data())) else { throw BeamItClipboardFailure("digest_mismatch") }
            transfer.node += 1
        }
    }
    func accept(_ id: String, frame: Data) throws -> Bool {
        guard let transfer = transfers[id], !transfer.ended else { throw BeamItClipboardFailure("invalid_frame") }
        let header = try Bulk.decode(frame)
        guard header.transferID == (try Bulk.decodeID(id, bytes: 16)) else { throw BeamItClipboardFailure("invalid_frame") }
        try skipEmpty(transfer)
        if header.kind == .end {
            guard transfer.node == transfer.manifest.nodes.count, header.offset ==
                    transfer.manifest.nodes.reduce(0, { $0 + ($1.length ?? 0) }) else {
                throw BeamItClipboardFailure("early_end")
            }
            transfer.ended = true
            return true
        }
        guard transfer.node < transfer.manifest.nodes.count, header.nodeIndex == UInt32(transfer.node),
              header.offset == transfer.offset else { throw BeamItClipboardFailure("invalid_offset") }
        let node = transfer.manifest.nodes[transfer.node]
        guard header.kind == (node.kind == .png ? .png : .file),
              UInt64(header.payload.count) <= node.length! - transfer.offset else { throw BeamItClipboardFailure("invalid_frame") }
        // Open relative to pinned parent descriptors; never truncate or follow links.
        let components = node.path.split(separator: "/").map(String.init)
        let parentPath = components.dropLast().joined(separator: "/")
        let parent = parentPath.isEmpty ? transfer.directory : try child(transfer.directory, parentPath)
        let output = try FD(openat(parent.raw, components.last!, O_WRONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC))
        let identity = try stamp(output)
        guard let original = transfer.identities[node.path], original.device == identity.device,
              original.inode == identity.inode, identity.links == 1, identity.size == Int64(transfer.offset) else {
            throw BeamItClipboardFailure("unsafe_staging")
        }
        guard lseek(output.raw, off_t(transfer.offset), SEEK_SET) >= 0 else { throw BeamItClipboardFailure("disk_write_failed") }
        try write(header.payload, to: output)
        transfer.digest.update(data: header.payload); transfer.offset += UInt64(header.payload.count)
        if transfer.offset == node.length {
            guard Data(transfer.digest.finalize()) == node.sha256 else {
                throw BeamItClipboardFailure("digest_mismatch")
            }
            guard fsync(output.raw) == 0 else { throw BeamItClipboardFailure.filesystem("disk_write_failed") }
            transfer.node += 1; transfer.offset = 0; transfer.digest = SHA256(); transfer.file = nil
            transfer.identities[node.path] = try stamp(output)
        }
        return false
    }
    func promote(_ id: String) throws -> Publication {
        guard let transfer = transfers[id], transfer.ended, !transfer.promoted else {
            throw BeamItClipboardFailure("incomplete_transfer")
        }
        for node in transfer.manifest.nodes {
            try Task.checkCancellation()
            let actual = try stamp(child(transfer.directory, node.path))
            guard actual == transfer.identities[node.path], node.kind == .directory || actual.links == 1 else {
                throw BeamItClipboardFailure("unsafe_staging")
            }
        }
        var png: Data?
        for node in transfer.manifest.nodes where node.kind == .png {
            let fd = try child(transfer.directory, node.path)
            var data = Data()
            while data.count < Int(node.length!) {
                let part = try read(fd, maximum: min(Bulk.chunkSize, Int(node.length!) - data.count))
                guard !part.isEmpty else { throw BeamItClipboardFailure("invalid_png") }
                data.append(part)
            }
            let checked = try image(data, isPNG: true)
            guard checked.width == node.width, checked.height == node.height else {
                throw BeamItClipboardFailure("invalid_png_dimensions")
            }
            png = data
        }
        let root = try rootFD(), mutex = try lock(root)
        defer { withExtendedLifetime(mutex) {} }
        let partial = try child(root, "client/partial"), committed = try child(root, "client/committed")
        let current = try stamp(child(partial, id)), expected = try stamp(transfer.directory)
        guard current.device == expected.device, current.inode == expected.inode else {
            throw BeamItClipboardFailure("unsafe_staging")
        }
        guard renameatx_np(partial.raw, id, committed.raw, id, UInt32(RENAME_EXCL)) == 0 else {
            throw BeamItClipboardFailure.filesystem("promotion_failed")
        }
        transfer.promoted = true
        let ledgers = try child(root, ".reservations")
        guard unlinkat(ledgers.raw, id, 0) == 0 else { throw BeamItClipboardFailure("quota_release_failed") }
        let directory = rootURL(id)
        return Publication(files: transfer.manifest.nodes.filter { $0.kind != .png && !$0.path.contains("/") }
            .map { directory.appendingPathComponent($0.path) }, png: png)
    }
    private func rootURL(_ id: String) -> URL { root.appendingPathComponent("client/committed/" + id) }
    nonisolated struct Image: Sendable { let png: Data; let width: UInt32; let height: UInt32 }
    func image(_ data: Data, isPNG: Bool) throws -> Image {
        guard !isPNG || data.count <= Bulk.maxPNGBytes else { throw BeamItClipboardFailure("png_too_large") }
        guard let source = CGImageSourceCreateWithData(data as CFData, nil),
              let properties = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any],
              let width = properties[kCGImagePropertyPixelWidth] as? UInt32,
              let height = properties[kCGImagePropertyPixelHeight] as? UInt32,
              width > 0, height > 0, width <= 16_384, height <= 16_384,
              UInt64(width) * UInt64(height) <= 64 * 1024 * 1024,
              let decoded = CGImageSourceCreateImageAtIndex(source, 0, nil) else {
            throw BeamItClipboardFailure("invalid_png")
        }
        if isPNG {
            guard let type = CGImageSourceGetType(source), type as String == UTType.png.identifier else {
                throw BeamItClipboardFailure("invalid_png")
            }
        }
        let output: Data
        if isPNG { output = data }
        else {
            let buffer = NSMutableData()
            guard let destination = CGImageDestinationCreateWithData(buffer, UTType.png.identifier as CFString, 1, nil) else {
                throw BeamItClipboardFailure("image_conversion_failed")
            }
            CGImageDestinationAddImage(destination, decoded, nil)
            guard CGImageDestinationFinalize(destination) else { throw BeamItClipboardFailure("image_conversion_failed") }
            output = buffer as Data
        }
        guard output.count <= Bulk.maxPNGBytes else { throw BeamItClipboardFailure("png_too_large") }
        return Image(png: output, width: width, height: height)
    }
    func finish(_ id: String, published: Bool = false) throws {
        sources.removeValue(forKey: id)
        guard let transfer = transfers.removeValue(forKey: id) else { return }
        transfer.file = nil
        let root = try rootFD(), mutex = try lock(root)
        defer { withExtendedLifetime(mutex) {}; withExtendedLifetime(transfer.lease) {} }
        if !published || !transfer.promoted {
            let container = try child(root, "client/" + (transfer.promoted ? "committed" : transfer.kind))
            try removeTree(container, name: id)
        }
        let ledgers = try child(root, ".reservations")
        if unlinkat(ledgers.raw, id, 0) != 0 && errno != ENOENT { throw BeamItClipboardFailure("cleanup_failed") }
    }
}
