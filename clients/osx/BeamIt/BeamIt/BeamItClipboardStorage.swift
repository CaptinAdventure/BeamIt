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
    nonisolated struct DirectoryIdentity: Sendable, Equatable {
        let device: Int32
        let inode: UInt64
    }
    nonisolated struct CommittedRecord: Sendable, Equatable {
        let id: String
        let identity: DirectoryIdentity
    }
    nonisolated struct Publication: Sendable {
        let files: [URL]
        let png: Data?
        let record: CommittedRecord
    }
    nonisolated enum CleanupPoint: Sendable {
        case afterLegacyClaimRegistration, beforeIdentityCheck, beforeEntryRemoval
    }
    nonisolated enum LedgerPoint: Sendable, Equatable {
        case afterLeaseCreation, afterDirectoryCreation, afterRecordDirectoryCreation
        case beforeRecordPublication, afterRecordPublication, afterRecordSync
        case afterDeletionProof, afterLeaseRetirement
        case afterMeasurementLookup, afterMeasurementOpen, afterMeasurementUsage
        case beforeQuotaLockAcquire, afterQuotaLockAcquired
        case afterLeasePinLink
        case beforePromotionRename, afterPromotionRename
        case beforePromotionSourceSync, afterPromotionSourceSync
        case beforePromotionDestinationSync, afterPromotionDestinationSync
        case beforeTreeDirectorySync(String), afterTreeDirectorySync(String)
        case beforeStageParentSync, beforeDeletionProofSync, beforeCanonicalLeaseRetirement
        case beforeCanonicalLeaseSync, beforePinRetirement, beforePinRetirementSync
        case beforeDeletionProofRename, beforeRecordRetirement, beforeRecordDirectorySync
    }
    private nonisolated struct ReservationRecord: Equatable {
        let kind: String
        let bytes: UInt64
        let identity: DirectoryIdentity?
        let leaseIdentity: DirectoryIdentity?
        let version: Int
        var deleted = false
    }
    nonisolated struct CleanupReport: Sendable {
        var removed: [String] = []
        var problems: [String] = []
        mutating func report(_ code: String) {
            if problems.count < 8 && !problems.contains(code) { problems.append(code) }
        }
    }
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
        private(set) var raw: Int32
        init(_ value: Int32) throws {
            guard value >= 0 else { throw BeamItClipboardFailure.filesystem("filesystem_unavailable") }
            raw = value
        }
        func close() {
            guard raw >= 0 else { return }
            Darwin.close(raw); raw = -1
        }
        deinit { close() }
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
    // Promotion does not unlink the existing kernel lease. Keeping the same
    // locked descriptor closes the promotion/publication/crash ownership gap.
    private var committedOwners: [String: Transfer] = [:]
    private var cleanupClaims: [String: FD] = [:]
    private var cleanupRecords: [String: CommittedRecord] = [:]
    private nonisolated final class CleanupRun {
        var result: Result<Bool, Error>?
        var waiters = 0
    }
    private var cleanupRuns: [String: CleanupRun] = [:]
    var activeCleanupWaiters: Int { cleanupRuns.values.reduce(0) { $0 + $1.waiters } }
    private let cleanupCheckpoint: (@Sendable (CleanupPoint) async throws -> Void)?
    private let ledgerCheckpoint: (@Sendable (LedgerPoint) throws -> Void)?
    private let treeDirectorySync: (@Sendable (Int32, String) -> Int32)?
    private var recoveryReporter: (@MainActor @Sendable () -> Void)?
    private(set) var metadataRecoveryIncomplete = false
    private let quota: UInt64 = 8_000_000_000
    private let scratch: UInt64 = 128 * 1024 * 1024
    private nonisolated static let stagingPath = "Library/Application Support/BeamIt/ClipboardStaging"

    nonisolated static var defaultRoot: URL {
        URL.homeDirectory.appendingPathComponent(stagingPath)
    }

    init(root: URL? = nil, stagingHome: URL? = nil,
         cleanupCheckpoint: (@Sendable (CleanupPoint) async throws -> Void)? = nil,
         ledgerCheckpoint: (@Sendable (LedgerPoint) throws -> Void)? = nil,
         treeDirectorySync: (@Sendable (Int32, String) -> Int32)? = nil) {
        precondition((cleanupCheckpoint == nil && ledgerCheckpoint == nil && treeDirectorySync == nil) ||
                     root != nil || stagingHome != nil,
                     "Filesystem test injection requires an explicit private root.")
        let home = stagingHome ?? URL.homeDirectory
        self.root = root ?? home.appendingPathComponent(Self.stagingPath)
        self.stagingHome = root == nil ? home : nil
        self.cleanupCheckpoint = cleanupCheckpoint
        self.ledgerCheckpoint = ledgerCheckpoint
        self.treeDirectorySync = treeDirectorySync
    }
    func setRecoveryReporter(_ reporter: @escaping @MainActor @Sendable () -> Void) {
        recoveryReporter = reporter
        if metadataRecoveryIncomplete { Task { @MainActor in reporter() } }
    }
    private func reportRecoveryProblem() {
        guard !metadataRecoveryIncomplete else { return }
        metadataRecoveryIncomplete = true
        if let recoveryReporter { Task { @MainActor in recoveryReporter() } }
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
        for path in ["client", "client/partial", "client/outgoing", "client/committed", ".reservations", ".reservation-records"] {
            let directory = try child(fd, path, createDirectories: true)
            var privateInfo = stat()
            guard fstat(directory.raw, &privateInfo) == 0, privateInfo.st_uid == geteuid(),
                  privateInfo.st_mode & 0o077 == 0 else { throw BeamItClipboardFailure("unsafe_staging") }
        }
        return fd
    }
    private func lock(_ root: FD) throws -> FD {
        let fd = try FD(openat(root.raw, ".quota-lock", O_RDWR | O_CREAT | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC, 0o600))
        let value = try stamp(fd)
        var info = stat()
        guard value.mode & S_IFMT == S_IFREG, value.links == 1, fstat(fd.raw, &info) == 0,
              info.st_uid == geteuid(), info.st_mode & 0o077 == 0 else {
            throw BeamItClipboardFailure("quota_lock_failed")
        }
        try ledgerCheckpoint?(.beforeQuotaLockAcquire)
        guard flock(fd.raw, LOCK_EX) == 0 else { throw BeamItClipboardFailure("quota_lock_failed") }
        try ledgerCheckpoint?(.afterQuotaLockAcquired)
        // Opening and then locking a descriptor does not prove that the name
        // still identifies that inode. Check again after acquiring the lock.
        try verifyEntry(root, name: ".quota-lock", expected: stamp(fd))
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
    // Quota measurement only: the returned byte count never grants deletion
    // authority or changes the canonical transfer-ID cleanup target.
    private func reservationMaterialized(_ root: FD, record: ReservationRecord?) throws -> UInt64 {
        guard let original = record?.identity else { return 0 }
        for kind in ["partial", "outgoing", "committed"] {
            let parent = try child(root, "client/" + kind)
            for name in try names(parent) {
                var info = stat()
                if fstatat(parent.raw, name, &info, AT_SYMLINK_NOFOLLOW) != 0 {
                    if errno == ENOENT { continue }
                    throw BeamItClipboardFailure.filesystem("quota_measurement_failed")
                }
                guard info.st_mode & S_IFMT == S_IFDIR, info.st_uid == geteuid(),
                      info.st_dev == original.device, info.st_ino == original.inode else { continue }
                try ledgerCheckpoint?(.afterMeasurementLookup)
                let directory = try child(parent, name)
                guard try identity(directory) == original else {
                    reportRecoveryProblem()
                    return 0 // A racing substitution cannot reduce the promise.
                }
                try ledgerCheckpoint?(.afterMeasurementOpen)
                let bytes = try usage(directory)
                try ledgerCheckpoint?(.afterMeasurementUsage)
                do { try verifyEntry(parent, name: name, expected: stamp(directory)) }
                catch {
                    reportRecoveryProblem()
                    return 0 // A rename or replacement during measurement cannot reduce the promise.
                }
                return bytes
            }
        }
        reportRecoveryProblem()
        return 0 // Missing identity is uncertainty, not transfer completion.
    }
    private func reconcile(_ root: FD) throws -> UInt64 {
        let ledgers = try child(root, ".reservations")
        let leasedIDs = Set(try names(ledgers))
        var outstanding: UInt64 = 0
        for name in leasedIDs {
            _ = try Bulk.decodeID(name, bytes: 16)
            let fd = try FD(openat(ledgers.raw, name, O_RDWR | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC))
            let ledgerInfo = try stamp(fd)
            guard ledgerInfo.mode & S_IFMT == S_IFREG, ledgerInfo.links == 1 || ledgerInfo.links == 2 else {
                throw BeamItClipboardFailure("invalid_quota_ledger")
            }
            let record = try reservationRecord(root, id: name, lease: fd)
            guard let record else {
                reportRecoveryProblem()
                throw BeamItClipboardFailure("cleanup_ownership_uncertain")
            }
            var originalLeaseLocked = false
            if record.version == 2 {
                if ledgerInfo.links == 2,
                   record.leaseIdentity == DirectoryIdentity(device: ledgerInfo.device, inode: ledgerInfo.inode) {
                    // This is the canonical link to the pinned original inode.
                } else {
                    reportRecoveryProblem()
                    let pin = try openPinnedLease(root, id: name, record: record)
                    if flock(pin.raw, LOCK_EX | LOCK_NB) != 0 && errno != EWOULDBLOCK {
                        throw BeamItClipboardFailure("quota_lock_failed")
                    }
                    originalLeaseLocked = true
                }
            } else if record.version == 1, ledgerInfo.links == 2 {
                // A crash during migration may leave its old marker and pin.
                // Bind both current names to this exact lease before counting.
                let pin = try child(try child(root, ".reservation-records"), name)
                let pinFD = try child(pin, "lease.pin"), pinInfo = try stamp(pinFD)
                guard pinInfo.device == ledgerInfo.device, pinInfo.inode == ledgerInfo.inode,
                      pinInfo.links == 2 else {
                    reportRecoveryProblem()
                    throw BeamItClipboardFailure("cleanup_ownership_uncertain")
                }
            } else {
                guard ledgerInfo.links == 1 else {
                    reportRecoveryProblem()
                    throw BeamItClipboardFailure("cleanup_ownership_uncertain")
                }
            }
            if record.kind == "legacy_cleanup" || (record.deleted && record.version == 2) {
                continue // Cleanup claims promise no new payload or scratch.
            }
            if record.deleted && record.version < 2 {
                reportRecoveryProblem()
                throw BeamItClipboardFailure("cleanup_ownership_uncertain")
            }
            // A partial ledger intentionally survives promotion unchanged.
            // The committed directory is authoritative under this same lock.
            // Thus a crash either side of rename never loses the live lease.
            let committed = try child(root, "client/committed")
            if try exists(committed, name: name),
               try !exists(child(root, "client/partial"), name: name),
               try !exists(child(root, "client/outgoing"), name: name) {
                let committedIdentity = try identity(child(committed, name))
               if record.identity == nil || record.identity == .some(committedIdentity) { continue }
            }
            if !originalLeaseLocked, flock(fd.raw, LOCK_EX | LOCK_NB) != 0 {
                guard errno == EWOULDBLOCK else { throw BeamItClipboardFailure("quota_lock_failed") }
            }
            // Even an unlocked pathname may replace an unlinked live lease.
            // Retain promises until authorized cleanup proves exact deletion.
            let materialized = try reservationMaterialized(root, record: record)
            let bytes = record.bytes
            let remaining = bytes - min(bytes, materialized)
            let remainingScratch = scratch - min(scratch, UInt64(max(0, ledgerInfo.size)))
            guard outstanding <= quota - remainingScratch, remaining <= quota - remainingScratch - outstanding else {
                throw BeamItClipboardFailure("quota_exceeded")
            }
            outstanding += remaining + remainingScratch
        }
        // A missing pathname does not prove that its kernel descriptor died.
        // Marker-only ordinary work might have an unlinked live lease; keep
        // possible payload/scratch promises until ownership is resolved.
        for id in try names(child(root, ".reservation-records")) where !leasedIDs.contains(id) {
            _ = try Bulk.decodeID(id, bytes: 16)
            let record = try reservationRecord(root, id: id, lease: nil)
            guard let record else {
                reportRecoveryProblem()
                throw BeamItClipboardFailure("cleanup_ownership_uncertain")
            }
            if record.kind == "legacy_cleanup" || record.deleted {
                if record.deleted, record.version == 2,
                   try entryExists(child(root, ".reservation-records/" + id), name: "lease.pin") {
                    let pin = try openPinnedLease(root, id: id, record: record)
                    guard try stamp(pin).links == 1 else {
                        reportRecoveryProblem()
                        throw BeamItClipboardFailure("cleanup_ownership_uncertain")
                    }
                }
                continue
            }
            if record.version == 2, let leaseIdentity = record.leaseIdentity {
                let pin = try openPinnedLease(root, id: id, record: record)
                guard try stamp(pin).links == 1 else {
                    reportRecoveryProblem()
                    throw BeamItClipboardFailure("cleanup_ownership_uncertain")
                }
                _ = leaseIdentity
            } else {
                reportRecoveryProblem()
                throw BeamItClipboardFailure("cleanup_ownership_uncertain")
            }
            reportRecoveryProblem()
            let materialized = try reservationMaterialized(root, record: record)
            let bytes = record.bytes
            let remaining = bytes - min(bytes, materialized)
            guard outstanding <= quota - scratch, remaining <= quota - scratch - outstanding else {
                throw BeamItClipboardFailure("quota_exceeded")
            }
            outstanding += remaining + scratch
        }
        return outstanding
    }
    func recover(legacyAllowed: Bool = false) async throws {
        let root = try rootFD()
        let ids: [String]
        do {
            let mutex = try lock(root)
            let leased = try names(child(root, ".reservations"))
            let recorded = try names(child(root, ".reservation-records"))
            ids = Array(Set(leased + recorded)).sorted()
            withExtendedLifetime(mutex) {}
        }
        for id in ids {
            try Task.checkCancellation()
            // Committed recovery belongs to the app coordinator, not connection
            // negotiation. Ordinary transfer recovery never retires publication.
            do {
                let mutex = try lock(root)
                defer { withExtendedLifetime(mutex) {} }
                if try exists(child(root, "client/committed"), name: id) {
                    if try exists(child(root, ".reservation-records"), name: id),
                       try reservationRecord(root, id: id, lease: nil) == nil { reportRecoveryProblem() }
                    continue
                }
            }
            do { _ = try await clean(id, expected: nil, legacyAllowed: legacyAllowed, abandonedOnly: true) }
            catch is CancellationError { throw CancellationError() }
            catch { reportRecoveryProblem() } // Continue authorized recovery of unrelated IDs.
        }
        if metadataRecoveryIncomplete { throw BeamItClipboardFailure("staging_recovery_incomplete") }
    }
    private func exists(_ parent: FD, name: String) throws -> Bool {
        var info = stat()
        if fstatat(parent.raw, name, &info, AT_SYMLINK_NOFOLLOW) != 0 {
            guard errno == ENOENT else { throw BeamItClipboardFailure.filesystem("cleanup_failed") }
            return false
        }
        guard info.st_mode & S_IFMT == S_IFDIR, info.st_uid == geteuid() else {
            throw BeamItClipboardFailure("unsafe_staging")
        }
        return true
    }
    private func entryExists(_ parent: FD, name: String) throws -> Bool {
        var info = stat()
        if fstatat(parent.raw, name, &info, AT_SYMLINK_NOFOLLOW) == 0 { return true }
        guard errno == ENOENT else { throw BeamItClipboardFailure.filesystem("filesystem_unavailable") }
        return false
    }
    private func identity(_ directory: FD) throws -> DirectoryIdentity {
        let value = try stamp(directory)
        return .init(device: value.device, inode: value.inode)
    }
    private func ledgerIdentity(_ object: [String: Any]) throws -> DirectoryIdentity? {
        if object["directory_device"] == nil && object["directory_inode"] == nil { return nil }
        let device = try Bulk.unsigned(object["directory_device"]), inode = try Bulk.unsigned(object["directory_inode"])
        guard device <= UInt32.max else { throw BeamItClipboardFailure("invalid_quota_ledger") }
        return .init(device: Int32(bitPattern: UInt32(device)), inode: inode)
    }
    // Version 2 binds the original locked lease inode and a hard-link pin.
    // Version 1 markers remain parseable only for explicit safe migration.
    private func reservationRecord(_ root: FD, id: String, lease: FD?) throws -> ReservationRecord? {
        let records = try child(root, ".reservation-records")
        if try exists(records, name: id) {
            let directory = try child(records, id), entries = try names(directory, maximum: 4)
            let markers = entries.filter { $0 != "lease.pin" }
            guard !markers.isEmpty else { return nil }
            guard markers.filter({ $0.hasPrefix("v2.") || $0.hasPrefix("deleted_v2.") }).count <= 1 else {
                return nil
            }
            // A migration crash may leave the old marker beside a complete v2
            // marker. The v2 binding is authoritative only after full pin
            // validation; callers retire the old marker under the root lock.
            guard let selected = markers.first(where: { $0.hasPrefix("v2.") || $0.hasPrefix("deleted_v2.") })
                    ?? (markers.count == 1 ? markers.first : nil) else { return nil }
            let deleted = selected.hasPrefix("deleted_")
            let name = deleted ? String(selected.dropFirst("deleted_".count)) : selected
            let parts = name.split(separator: ".", omittingEmptySubsequences: false).map(String.init)
            let v2 = parts.first == "v2"
            let values = v2 ? Array(parts.dropFirst()) : parts
            if v2 {
                guard values.count == 7, values[0] == id else { return nil }
            } else {
                guard values.count == 4 else { return nil }
            }
            let shift = v2 ? 1 : 0
            let bytesIndex = v2 ? 2 : 3
            let deviceIndex = v2 ? 3 : 1
            let inodeIndex = v2 ? 4 : 2
            guard ["partial", "outgoing", "legacy_cleanup"].contains(values[shift]),
                  let bytes = UInt64(values[bytesIndex]), bytes <= quota,
                  String(bytes) == values[bytesIndex],
                  let device = UInt32(values[deviceIndex]), String(device) == values[deviceIndex],
                  let inode = UInt64(values[inodeIndex]), inode > 0,
                  String(inode) == values[inodeIndex],
                  values[shift] != "legacy_cleanup" || bytes == 0 else { return nil }
            let stageIdentity = DirectoryIdentity(device: Int32(bitPattern: device), inode: inode)
            var leaseIdentity: DirectoryIdentity?
            if v2 {
                guard let leaseDevice = UInt32(values[shift + 4]), String(leaseDevice) == values[shift + 4],
                          let leaseInode = UInt64(values[shift + 5]), leaseInode > 0,
                          String(leaseInode) == values[shift + 5],
                          entries.contains("lease.pin") || deleted else { return nil }
                leaseIdentity = .init(device: Int32(bitPattern: leaseDevice), inode: leaseInode)
                if entries.contains("lease.pin") {
                        let pin = try child(directory, "lease.pin"), pinStamp = try stamp(pin)
                        var pinInfo = stat()
                        guard pinStamp.mode & S_IFMT == S_IFREG,
                              pinStamp.device == leaseIdentity!.device, pinStamp.inode == leaseIdentity!.inode,
                              pinStamp.links == 1 || pinStamp.links == 2,
                              fstat(pin.raw, &pinInfo) == 0, pinInfo.st_uid == geteuid(),
                              pinInfo.st_mode & 0o077 == 0 else {
                            throw BeamItClipboardFailure("cleanup_ownership_uncertain")
                        }
                }
            } else {
                guard markers.count == 1 else { return nil }
                if entries.contains("lease.pin") {
                    let pin = try child(directory, "lease.pin"), pinStamp = try stamp(pin)
                    var pinInfo = stat()
                    guard pinStamp.mode & S_IFMT == S_IFREG,
                          pinStamp.links == 2,
                          fstat(pin.raw, &pinInfo) == 0, pinInfo.st_uid == geteuid(),
                          pinInfo.st_mode & 0o077 == 0 else {
                        throw BeamItClipboardFailure("cleanup_ownership_uncertain")
                    }
                }
            }
            let marker = try child(directory, selected), info = try stamp(marker)
            var native = stat()
            guard info.mode & S_IFMT == S_IFREG, info.links == 1, info.size == 0,
                  fstat(marker.raw, &native) == 0, native.st_uid == geteuid(),
                  native.st_mode & 0o077 == 0 else { throw BeamItClipboardFailure("invalid_quota_ledger") }
            return .init(kind: values[shift], bytes: bytes, identity: stageIdentity,
                         leaseIdentity: leaseIdentity, version: v2 ? 2 : 1, deleted: deleted)
        }
        // Compatibility with intact, pre-marker records. Unbound records can
        // describe live quota promises but cannot authorize dead-owner deletion.
        guard let lease, lseek(lease.raw, 0, SEEK_SET) == 0 else { return nil }
        let data = try read(lease, maximum: 4097)
        guard data.count <= 4096, let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let kind = object["kind"] as? String, ["partial", "outgoing", "legacy_cleanup"].contains(kind),
              let bytes = try? Bulk.unsigned(object["bytes"]), bytes <= quota else { return nil }
        guard let parsed = try? ledgerIdentity(object) else {
            // try? flattens Optional: absence is handled separately below.
            if object["directory_device"] != nil || object["directory_inode"] != nil { return nil }
            return kind == "legacy_cleanup" ? nil :
                .init(kind: kind, bytes: bytes, identity: nil, leaseIdentity: nil, version: 0)
        }
        guard kind != "legacy_cleanup" || bytes == 0 else { return nil }
        return .init(kind: kind, bytes: bytes, identity: parsed, leaseIdentity: nil, version: 0)
    }
    private func publishRecord(_ root: FD, id: String, record: ReservationRecord,
                               lease: FD, mutex: FD) throws {
        guard let identity = record.identity else { throw BeamItClipboardFailure("cleanup_ownership_uncertain") }
        let records = try child(root, ".reservation-records")
        let directory = try child(records, id, createDirectories: true)
        try ledgerCheckpoint?(.afterRecordDirectoryCreation)
        let leaseStamp = try stamp(lease)
        var leaseInfo = stat()
        guard leaseStamp.mode & S_IFMT == S_IFREG, leaseStamp.links == 1 || leaseStamp.links == 2,
              fstat(lease.raw, &leaseInfo) == 0, leaseInfo.st_uid == geteuid(),
              leaseInfo.st_mode & 0o077 == 0 else {
            throw BeamItClipboardFailure("invalid_quota_ledger")
        }
        let ledgers = try child(root, ".reservations")
        try verifyHardlinkEntry(ledgers, name: id, expected: leaseStamp)
        try verifyEntry(root, name: ".quota-lock", expected: stamp(mutex))
        if try !entryExists(directory, name: "lease.pin") {
            guard linkat(ledgers.raw, id, directory.raw, "lease.pin", 0) == 0 else {
                throw BeamItClipboardFailure.filesystem("cleanup_claim_failed")
            }
            guard fsync(lease.raw) == 0, fsync(ledgers.raw) == 0, fsync(directory.raw) == 0,
                  fsync(records.raw) == 0 else { throw BeamItClipboardFailure("cleanup_claim_failed") }
        }
        let pin = try child(directory, "lease.pin"), pinStamp = try stamp(pin)
        guard pinStamp.device == leaseStamp.device, pinStamp.inode == leaseStamp.inode,
              pinStamp.links == 2 else { throw BeamItClipboardFailure("cleanup_ownership_uncertain") }
        let versioned = ReservationRecord(kind: record.kind, bytes: record.bytes,
                                          identity: identity,
                                          leaseIdentity: .init(device: leaseStamp.device, inode: leaseStamp.inode),
                                          version: 2, deleted: false)
        let name = "v2." + id + "." + record.kind + "." + String(record.bytes) + "." +
            String(UInt32(bitPattern: identity.device)) + "." + String(identity.inode) + "." +
            String(UInt32(bitPattern: leaseStamp.device)) + "." + String(leaseStamp.inode)
        let entries = try names(directory, maximum: 4)
        if let current = try reservationRecord(root, id: id, lease: lease),
           current.version == 2, current == versioned {
            let marker = try child(directory, name)
            let currentPin = try child(directory, "lease.pin")
            guard try stamp(currentPin).links == 2,
                  fsync(marker.raw) == 0, fsync(lease.raw) == 0,
                  fsync(ledgers.raw) == 0, fsync(directory.raw) == 0,
                  fsync(records.raw) == 0 else {
                throw BeamItClipboardFailure("cleanup_claim_failed")
            }
            try verifyBoundRecord(root, id: id, record: current, lease: lease)
            try verifyEntry(root, name: ".quota-lock", expected: stamp(mutex))
            return
        }
        let legacyName = record.kind + "." + String(UInt32(bitPattern: identity.device)) + "." +
            String(identity.inode) + "." + String(record.bytes)
        guard entries.filter({ $0 != "lease.pin" }).allSatisfy({ $0 == legacyName }) else {
            throw BeamItClipboardFailure("cleanup_ownership_uncertain")
        }
        try ledgerCheckpoint?(.beforeRecordPublication)
        try verifyEntry(root, name: ".quota-lock", expected: stamp(mutex))
        let marker = try FD(openat(directory.raw, name, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0o600))
        try ledgerCheckpoint?(.afterRecordPublication)
        guard fsync(marker.raw) == 0, fsync(directory.raw) == 0, fsync(records.raw) == 0 else {
            throw BeamItClipboardFailure("cleanup_claim_failed")
        }
        try ledgerCheckpoint?(.afterRecordSync)
        try verifyEntry(root, name: ".quota-lock", expected: stamp(mutex))
        // Keep the old marker until v2 has been synced. Then retire only
        // recognized old names and sync the directory again.
        for oldName in entries where oldName != "lease.pin" && oldName != name {
            guard oldName == legacyName else { throw BeamItClipboardFailure("cleanup_ownership_uncertain") }
            let old = try child(directory, oldName)
            try verifyEntry(root, name: ".quota-lock", expected: stamp(mutex))
            try verifyEntry(directory, name: oldName, expected: stamp(old))
            guard unlinkat(directory.raw, oldName, 0) == 0 else {
                throw BeamItClipboardFailure("cleanup_claim_failed")
            }
        }
        guard fsync(directory.raw) == 0 else { throw BeamItClipboardFailure("cleanup_claim_failed") }
        try verifyEntry(root, name: ".quota-lock", expected: stamp(mutex))
    }
    // Persist the selected directory identity before creating a lease for an
    // old unleased committed selection. This migration journal is not itself
    // ownership proof; later recovery still requires positive legacy
    // eligibility and rechecks the unchanged stage before creating v2.
    private func publishLegacyMarker(_ root: FD, id: String, stage: DirectoryIdentity,
                                     mutex: FD) throws {
        let records = try child(root, ".reservation-records")
        try verifyEntry(root, name: ".quota-lock", expected: stamp(mutex))
        guard try !entryExists(records, name: id),
              mkdirat(records.raw, id, 0o700) == 0 else {
            throw BeamItClipboardFailure("cleanup_ownership_uncertain")
        }
        let directory = try child(records, id)
        try ledgerCheckpoint?(.afterRecordDirectoryCreation)
        let name = "legacy_cleanup." + String(UInt32(bitPattern: stage.device)) + "." +
            String(stage.inode) + ".0"
        try verifyEntry(root, name: ".quota-lock", expected: stamp(mutex))
        try ledgerCheckpoint?(.beforeRecordPublication)
        let marker = try FD(openat(directory.raw, name,
                                   O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0o600))
        try ledgerCheckpoint?(.afterRecordPublication)
        guard fsync(marker.raw) == 0, fsync(directory.raw) == 0, fsync(records.raw) == 0 else {
            throw BeamItClipboardFailure("cleanup_claim_failed")
        }
        try ledgerCheckpoint?(.afterRecordSync)
        try verifyEntry(root, name: ".quota-lock", expected: stamp(mutex))
    }
    private func createLeasePin(_ root: FD, id: String, lease: FD, mutex: FD) throws {
        let ledgers = try child(root, ".reservations")
        let records = try child(root, ".reservation-records")
        let directory = try child(records, id, createDirectories: true)
        let value = try stamp(lease)
        guard value.mode & S_IFMT == S_IFREG, value.links == 1 else {
            throw BeamItClipboardFailure("invalid_quota_ledger")
        }
        try verifyEntry(ledgers, name: id, expected: value)
        try verifyEntry(root, name: ".quota-lock", expected: stamp(mutex))
        guard linkat(ledgers.raw, id, directory.raw, "lease.pin", 0) == 0 else {
            throw BeamItClipboardFailure.filesystem("cleanup_claim_failed")
        }
        try ledgerCheckpoint?(.afterLeasePinLink)
        try verifyEntry(root, name: ".quota-lock", expected: stamp(mutex))
        let pin = try child(directory, "lease.pin"), pinStamp = try stamp(pin)
        guard pinStamp.device == value.device, pinStamp.inode == value.inode, pinStamp.links == 2,
              fsync(lease.raw) == 0, fsync(ledgers.raw) == 0,
              fsync(directory.raw) == 0, fsync(records.raw) == 0 else {
            throw BeamItClipboardFailure("cleanup_claim_failed")
        }
        try verifyEntry(root, name: ".quota-lock", expected: stamp(mutex))
    }
    private func retireMigratedMarker(_ root: FD, id: String, record: ReservationRecord,
                                     mutex: FD) throws {
        guard let stage = record.identity, let lease = record.leaseIdentity else {
            throw BeamItClipboardFailure("cleanup_ownership_uncertain")
        }
        let directory = try child(root, ".reservation-records/" + id)
        let names = try names(directory, maximum: 4).filter { $0 != "lease.pin" }
        let currentName = (record.deleted ? "deleted_" : "") + "v2." + id + "." +
            record.kind + "." + String(record.bytes) + "." +
            String(UInt32(bitPattern: stage.device)) + "." + String(stage.inode) + "." +
            String(UInt32(bitPattern: lease.device)) + "." + String(lease.inode)
        let extras = names.filter { $0 != currentName }
        guard extras.count <= 1 else { throw BeamItClipboardFailure("cleanup_ownership_uncertain") }
        guard let old = extras.first else { return }
        let expected = record.kind + "." + String(UInt32(bitPattern: stage.device)) + "." +
            String(stage.inode) + "." + String(record.bytes)
        guard old == expected else { throw BeamItClipboardFailure("cleanup_ownership_uncertain") }
        let marker = try child(directory, old)
        try verifyEntry(root, name: ".quota-lock", expected: stamp(mutex))
        try verifyEntry(directory, name: old, expected: stamp(marker))
        guard unlinkat(directory.raw, old, 0) == 0, fsync(directory.raw) == 0 else {
            throw BeamItClipboardFailure("cleanup_failed")
        }
    }
    private func openPinnedLease(_ root: FD, id: String, record: ReservationRecord) throws -> FD {
        guard record.version == 2, let expected = record.leaseIdentity else {
            throw BeamItClipboardFailure("cleanup_ownership_uncertain")
        }
        let directory = try child(root, ".reservation-records/" + id)
        let pin = try withExtendedLifetime(directory) {
            try FD(openat(directory.raw, "lease.pin", O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC))
        }
        let value = try stamp(pin)
        var info = stat()
        guard value.mode & S_IFMT == S_IFREG, value.device == expected.device,
              value.inode == expected.inode, value.links == 2 || value.links == 1,
              fstat(pin.raw, &info) == 0, info.st_uid == geteuid(), info.st_mode & 0o077 == 0 else {
            throw BeamItClipboardFailure("cleanup_ownership_uncertain")
        }
        return pin
    }
    private func verifyCanonicalLease(_ root: FD, id: String, record: ReservationRecord,
                                      pinned: FD) throws -> Bool {
        guard let expected = record.leaseIdentity else { return false }
        let ledgers = try child(root, ".reservations")
        let pinStamp = try stamp(pinned)
        var info = stat()
        if fstatat(ledgers.raw, id, &info, AT_SYMLINK_NOFOLLOW) != 0 {
            guard errno == ENOENT else { throw BeamItClipboardFailure("cleanup_ownership_uncertain") }
            guard pinStamp.links == 1 else { throw BeamItClipboardFailure("cleanup_ownership_uncertain") }
            return false
        }
        guard info.st_mode & S_IFMT == S_IFREG, info.st_uid == geteuid(),
              info.st_dev == expected.device, info.st_ino == expected.inode,
              pinStamp.device == expected.device, pinStamp.inode == expected.inode,
              pinStamp.links == 2 else { throw BeamItClipboardFailure("cleanup_ownership_uncertain") }
        return true
    }
    private func verifyBoundRecord(_ root: FD, id: String, record: ReservationRecord,
                                   lease: FD, canonicalMustExist: Bool = true) throws {
        guard record.version == 2, let expected = record.leaseIdentity else {
            throw BeamItClipboardFailure("cleanup_ownership_uncertain")
        }
        let value = try stamp(lease)
        guard value.mode & S_IFMT == S_IFREG, value.device == expected.device,
              value.inode == expected.inode else { throw BeamItClipboardFailure("cleanup_ownership_uncertain") }
        let directory = try child(root, ".reservation-records/" + id)
        let pin = try FD(openat(directory.raw, "lease.pin", O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC))
        let pinStamp = try stamp(pin)
        var pinInfo = stat()
        guard pinStamp.mode & S_IFMT == S_IFREG, pinStamp.device == expected.device,
              pinStamp.inode == expected.inode,
              fstat(pin.raw, &pinInfo) == 0, pinInfo.st_uid == geteuid(),
              pinInfo.st_mode & 0o077 == 0 else { throw BeamItClipboardFailure("cleanup_ownership_uncertain") }
        let ledgers = try child(root, ".reservations")
        if canonicalMustExist {
            guard pinStamp.links == 2 else { throw BeamItClipboardFailure("cleanup_ownership_uncertain") }
            try verifyHardlinkEntry(ledgers, name: id, expected: value)
        } else {
            if try entryExists(ledgers, name: id) {
                guard try verifyCanonicalLease(root, id: id, record: record, pinned: pin),
                      pinStamp.links == 2 else { throw BeamItClipboardFailure("cleanup_ownership_uncertain") }
            } else {
                guard pinStamp.links == 1 else { throw BeamItClipboardFailure("cleanup_ownership_uncertain") }
            }
        }
    }
    // Called only while holding the root quota lock after legacy eligibility
    // and the unchanged stage identity have been established.
    private func migrateLegacyClaim(_ root: FD, id: String, old: ReservationRecord,
                                    stageIdentity: DirectoryIdentity, mutex: FD) throws -> FD? {
        guard old.version < 2, !old.deleted, old.identity == stageIdentity,
              old.kind != "legacy_cleanup" || old.bytes == 0 else {
            throw BeamItClipboardFailure("cleanup_ownership_uncertain")
        }
        let ledgers = try child(root, ".reservations")
        let recordDirectory = try child(root, ".reservation-records")
        let recordDirectoryExists = try entryExists(recordDirectory, name: id)
        let pinAlreadyExists: Bool
        if recordDirectoryExists {
            pinAlreadyExists = try entryExists(child(recordDirectory, id), name: "lease.pin")
        } else {
            pinAlreadyExists = false
        }
        let canonicalAlreadyExists = try entryExists(ledgers, name: id)
        guard canonicalAlreadyExists || !pinAlreadyExists else {
            throw BeamItClipboardFailure("cleanup_ownership_uncertain")
        }
        let lease: FD
        if canonicalAlreadyExists {
            let existing = try FD(openat(ledgers.raw, id, O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC))
            let value = try stamp(existing)
            var info = stat()
            let expectedLinks: UInt64 = pinAlreadyExists ? 2 : 1
            guard value.mode & S_IFMT == S_IFREG, value.links == expectedLinks,
                  fstat(existing.raw, &info) == 0, info.st_uid == geteuid(),
                  info.st_mode & 0o077 == 0 else {
                throw BeamItClipboardFailure("cleanup_ownership_uncertain")
            }
            if flock(existing.raw, LOCK_EX | LOCK_NB) != 0 {
                guard errno == EWOULDBLOCK else { throw BeamItClipboardFailure("cleanup_claim_failed") }
                return nil
            }
            lease = existing
        } else {
            let created = try FD(openat(ledgers.raw, id,
                O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0o600))
            guard flock(created.raw, LOCK_EX | LOCK_NB) == 0 else {
                throw BeamItClipboardFailure("cleanup_claim_failed")
            }
            lease = created
        }
        if pinAlreadyExists {
            let transferRecordDirectory = try child(recordDirectory, id)
            let pin = try child(transferRecordDirectory, "lease.pin")
            let pinStamp = try stamp(pin), leaseStamp = try stamp(lease)
            guard pinStamp.mode & S_IFMT == S_IFREG, pinStamp.links == 2,
                  pinStamp.device == leaseStamp.device, pinStamp.inode == leaseStamp.inode else {
                throw BeamItClipboardFailure("cleanup_ownership_uncertain")
            }
            try verifyHardlinkEntry(ledgers, name: id, expected: pinStamp)
        } else {
            try createLeasePin(root, id: id, lease: lease, mutex: mutex)
        }
        try publishRecord(root, id: id, record: ReservationRecord(kind: old.kind, bytes: old.bytes,
            identity: stageIdentity, leaseIdentity: nil, version: 0), lease: lease, mutex: mutex)
        // Old JSON may occupy the lease inode. The new durable marker and pin
        // now bind that same inode; clear only its legacy payload, never the ID.
        try verifyEntry(root, name: ".quota-lock", expected: stamp(mutex))
        if ftruncate(lease.raw, 0) != 0 || fsync(lease.raw) != 0 {
            throw BeamItClipboardFailure("cleanup_claim_failed")
        }
        let migrated = try reservationRecord(root, id: id, lease: lease)
        guard let migrated, migrated.version == 2, migrated.identity == stageIdentity else {
            throw BeamItClipboardFailure("cleanup_ownership_uncertain")
        }
        try verifyBoundRecord(root, id: id, record: migrated, lease: lease)
        return lease
    }
    private func markDeleted(_ root: FD, id: String, record: ReservationRecord, lease: FD,
                             parent: FD, expected: DirectoryIdentity, mutex: FD) throws {
        guard record.version == 2, record.identity == expected,
              let leaseIdentity = record.leaseIdentity else {
            throw BeamItClipboardFailure("cleanup_ownership_uncertain")
        }
        try verifyBoundRecord(root, id: id, record: record, lease: lease)
        try verifyEntry(root, name: ".quota-lock", expected: stamp(mutex))
        guard try !entryExists(parent, name: id) else {
            throw BeamItClipboardFailure("cleanup_identity_changed")
        }
        guard fsync(parent.raw) == 0, fsync(lease.raw) == 0 else {
            throw BeamItClipboardFailure("cleanup_failed")
        }
        let records = try child(root, ".reservation-records")
        let directory = try child(records, id)
        let entries = try names(directory, maximum: 3)
        guard entries.contains("lease.pin") else { throw BeamItClipboardFailure("cleanup_ownership_uncertain") }
        let marker = entries.first(where: { $0.hasPrefix("v2.") })
        guard let marker else { throw BeamItClipboardFailure("cleanup_ownership_uncertain") }
        let expectedName = "v2." + id + "." + record.kind + "." + String(record.bytes) + "." +
            String(UInt32(bitPattern: expected.device)) + "." + String(expected.inode) + "." +
            String(UInt32(bitPattern: leaseIdentity.device)) + "." + String(leaseIdentity.inode)
        try verifyEntry(root, name: ".quota-lock", expected: stamp(mutex))
        guard marker == expectedName else {
            throw BeamItClipboardFailure.filesystem("cleanup_failed")
        }
        try ledgerCheckpoint?(.beforeDeletionProofRename)
        try Task.checkCancellation()
        guard renameat(directory.raw, marker, directory.raw, "deleted_" + marker) == 0 else {
            throw BeamItClipboardFailure.filesystem("cleanup_failed")
        }
        do {
            try ledgerCheckpoint?(.beforeDeletionProofSync)
            try Task.checkCancellation()
            guard fsync(directory.raw) == 0 else {
                throw BeamItClipboardFailure("cleanup_failed")
            }
        } catch {
            // Do not leave an unconfirmed deletion proof as authorization.
            if renameat(directory.raw, "deleted_" + marker, directory.raw, marker) == 0 {
                _ = fsync(directory.raw)
            }
            throw error
        }
        try ledgerCheckpoint?(.afterDeletionProof)
    }
    private func retireDeletedRecord(_ root: FD, id: String, record: ReservationRecord,
                                     lease: FD?, mutex: FD) throws -> Bool {
        guard record.version == 2, record.deleted,
              let stageIdentity = record.identity, let leaseIdentity = record.leaseIdentity else {
            throw BeamItClipboardFailure("cleanup_ownership_uncertain")
        }
        try verifyEntry(root, name: ".quota-lock", expected: stamp(mutex))
        let ledgers = try child(root, ".reservations")
        let records = try child(root, ".reservation-records")
        let directory = try child(records, id)
        let pinPresent = try entryExists(directory, name: "lease.pin")
        let canonicalPresent = try entryExists(ledgers, name: id)
        var pin: FD?
        func releaseLeaseDescriptors() {
            lease?.close(); pin?.close()
            cleanupClaims.removeValue(forKey: id)
            transfers.removeValue(forKey: id)
            committedOwners.removeValue(forKey: id)
        }
        if pinPresent {
            pin = try openPinnedLease(root, id: id, record: record)
            let value = try stamp(pin!)
            guard value.device == leaseIdentity.device, value.inode == leaseIdentity.inode else {
                throw BeamItClipboardFailure("cleanup_ownership_uncertain")
            }
            if canonicalPresent {
                try verifyHardlinkEntry(ledgers, name: id, expected: value)
                guard value.links == 2 else { throw BeamItClipboardFailure("cleanup_ownership_uncertain") }
            } else {
                guard value.links == 1 else { throw BeamItClipboardFailure("cleanup_ownership_uncertain") }
            }
            if let lease {
                let ownerStamp = try stamp(lease)
                guard ownerStamp.device == value.device, ownerStamp.inode == value.inode else {
                    throw BeamItClipboardFailure("cleanup_ownership_uncertain")
                }
            } else if flock(pin!.raw, LOCK_EX | LOCK_NB) != 0 {
                guard errno == EWOULDBLOCK else { throw BeamItClipboardFailure("cleanup_claim_failed") }
                return false
            }
        } else {
            guard record.deleted, !canonicalPresent else {
                throw BeamItClipboardFailure("cleanup_ownership_uncertain")
            }
        }
        if canonicalPresent {
            let canonical = try FD(openat(ledgers.raw, id, O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC))
            let value = try stamp(canonical)
            let pinValue = try pin.map { try stamp($0) }
            guard value.device == leaseIdentity.device, value.inode == leaseIdentity.inode,
                  let pinValue, value.device == pinValue.device, value.inode == pinValue.inode else {
                throw BeamItClipboardFailure("cleanup_ownership_uncertain")
            }
            try verifyEntry(root, name: ".quota-lock", expected: stamp(mutex))
            try ledgerCheckpoint?(.beforeCanonicalLeaseRetirement)
            try verifyHardlinkEntry(ledgers, name: id, expected: value)
            guard unlinkat(ledgers.raw, id, 0) == 0 else {
                throw BeamItClipboardFailure.filesystem("cleanup_failed")
            }
            try ledgerCheckpoint?(.beforeCanonicalLeaseSync)
            guard fsync(ledgers.raw) == 0 else { throw BeamItClipboardFailure("cleanup_failed") }
            canonical.close()
        }
        if pinPresent {
            try verifyEntry(root, name: ".quota-lock", expected: stamp(mutex))
            let pinStamp = try stamp(pin!)
            var named = stat()
            guard fstatat(directory.raw, "lease.pin", &named, AT_SYMLINK_NOFOLLOW) == 0,
                  named.st_dev == pinStamp.device, named.st_ino == pinStamp.inode else {
                throw BeamItClipboardFailure("cleanup_ownership_uncertain")
            }
            try ledgerCheckpoint?(.beforePinRetirement)
            guard unlinkat(directory.raw, "lease.pin", 0) == 0 else {
                throw BeamItClipboardFailure("cleanup_failed")
            }
            do { try ledgerCheckpoint?(.beforePinRetirementSync) }
            catch {
                releaseLeaseDescriptors()
                throw error
            }
            if fsync(directory.raw) != 0 {
                releaseLeaseDescriptors()
                try? ledgerCheckpoint?(.afterLeaseRetirement)
                throw BeamItClipboardFailure("cleanup_failed")
            }
            do { try ledgerCheckpoint?(.afterLeaseRetirement) }
            catch {
                releaseLeaseDescriptors()
                throw error
            }
        }
        // Close the original descriptor before releasing the quota lock, even
        // when it is also retained by the former Transfer object.
        lease?.close()
        pin?.close()
        cleanupClaims.removeValue(forKey: id)
        transfers.removeValue(forKey: id)
        committedOwners.removeValue(forKey: id)
        for kind in ["partial", "outgoing", "committed"] {
            guard try !entryExists(child(root, "client/" + kind), name: id) else {
                throw BeamItClipboardFailure("cleanup_identity_changed")
            }
        }
        let markerNames = try names(directory, maximum: 3)
        guard markerNames.count == 1, markerNames[0].hasPrefix("deleted_v2.") else {
            throw BeamItClipboardFailure("cleanup_ownership_uncertain")
        }
        let markerName = markerNames[0]
        let marker = try child(directory, markerName)
        try verifyEntry(directory, name: markerName, expected: stamp(marker))
        try verifyEntry(root, name: ".quota-lock", expected: stamp(mutex))
        try ledgerCheckpoint?(.beforeRecordRetirement)
        guard unlinkat(directory.raw, markerName, 0) == 0 else {
            throw BeamItClipboardFailure("cleanup_failed")
        }
        guard fsync(directory.raw) == 0 else {
            // Best-effort restore the proof if syncing marker retirement fails.
            if let restored = try? FD(openat(directory.raw, markerName, O_RDWR | O_CREAT | O_EXCL |
                                             O_NOFOLLOW | O_CLOEXEC, 0o600)) {
                _ = fsync(restored.raw); _ = fsync(directory.raw)
            }
            throw BeamItClipboardFailure("cleanup_failed")
        }
        try verifyEntry(root, name: ".quota-lock", expected: stamp(mutex))
        try verifyEntry(records, name: id, expected: stamp(directory))
        guard unlinkat(records.raw, id, AT_REMOVEDIR) == 0 else {
            throw BeamItClipboardFailure("cleanup_failed")
        }
        do {
            try ledgerCheckpoint?(.beforeRecordDirectorySync)
            guard fsync(records.raw) == 0 else { throw BeamItClipboardFailure("cleanup_failed") }
        } catch {
            // Preserve a proof-only tombstone if the last directory sync fails.
            if mkdirat(records.raw, id, 0o700) == 0,
               let restoredDirectory = try? child(records, id),
               let restoredMarker = try? FD(openat(restoredDirectory.raw, markerName,
                    O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0o600)) {
                _ = fsync(restoredMarker.raw); _ = fsync(restoredDirectory.raw); _ = fsync(records.raw)
            }
            throw error
        }
        _ = stageIdentity // The proof binds it; no path is used after deletion.
        return true
    }
    private func verifyEntry(_ parent: FD, name: String, expected: Stamp) throws {
        var info = stat()
        guard fstatat(parent.raw, name, &info, AT_SYMLINK_NOFOLLOW) == 0,
              info.st_uid == geteuid(), info.st_dev == expected.device,
              info.st_ino == expected.inode, info.st_mode & S_IFMT == expected.mode & S_IFMT,
              info.st_mode & S_IFMT == S_IFDIR || info.st_nlink == 1 else {
            throw BeamItClipboardFailure("cleanup_identity_changed")
        }
    }
    private func verifyHardlinkEntry(_ parent: FD, name: String, expected: Stamp) throws {
        var info = stat()
        guard fstatat(parent.raw, name, &info, AT_SYMLINK_NOFOLLOW) == 0,
              info.st_uid == geteuid(), info.st_dev == expected.device,
              info.st_ino == expected.inode, info.st_mode & S_IFMT == S_IFREG else {
            throw BeamItClipboardFailure("cleanup_identity_changed")
        }
    }
    // Validate the whole bounded tree before removing anything. Unexpected
    // symlinks/special/hard-linked entries remain untouched and block cleanup.
    private func validateCleanupTree(_ directory: FD) async throws {
        var pending = [""], remaining = Bulk.maxNodes
        while let path = pending.popLast() {
            try Task.checkCancellation()
            let parent = path.isEmpty ? directory : try child(directory, path)
            for name in try names(parent, maximum: Bulk.maxNodes) {
                let relative = path.isEmpty ? name : path + "/" + name
                try Bulk.validatePath(relative)
                guard remaining > 0 else { throw BeamItClipboardFailure("cleanup_unexpected_entry") }
                remaining -= 1
                let fd = try child(parent, name), value = try stamp(fd)
                var info = stat()
                guard fstat(fd.raw, &info) == 0, info.st_uid == geteuid() else {
                    throw BeamItClipboardFailure("cleanup_unexpected_entry")
                }
                if value.mode & S_IFMT == S_IFDIR { pending.append(relative) }
                else if value.links != 1 { throw BeamItClipboardFailure("cleanup_unexpected_entry") }
                await Task.yield()
            }
        }
    }
    private func removeValidatedTree(_ parent: FD, name: String, expected: Stamp,
                                     relative: String, remaining: Int) async throws -> Int {
        try Task.checkCancellation()
        guard remaining > 0 else { throw BeamItClipboardFailure("cleanup_unexpected_entry") }
        var left = remaining - 1
        let fd = try child(parent, name)
        guard try identity(fd) == DirectoryIdentity(device: expected.device, inode: expected.inode) else {
            throw BeamItClipboardFailure("cleanup_identity_changed")
        }
        if expected.mode & S_IFMT == S_IFDIR {
            for entry in try names(fd, maximum: Bulk.maxNodes) {
                let path = relative.isEmpty ? entry : relative + "/" + entry
                try Bulk.validatePath(path)
                let nested = try child(fd, entry), value = try stamp(nested)
                guard value.mode & S_IFMT == S_IFDIR || value.links == 1 else {
                    throw BeamItClipboardFailure("cleanup_unexpected_entry")
                }
                left = try await removeValidatedTree(fd, name: entry, expected: value,
                                                     relative: path, remaining: left)
            }
        }
        if let cleanupCheckpoint { try await cleanupCheckpoint(.beforeEntryRemoval) }
        try Task.checkCancellation()
        // Check after every suspension and immediately before unlink. No path
        // from the pasteboard is ever an input to this operation.
        try verifyEntry(parent, name: name, expected: expected)
        guard unlinkat(parent.raw, name, expected.mode & S_IFMT == S_IFDIR ? AT_REMOVEDIR : 0) == 0 else {
            throw BeamItClipboardFailure.filesystem("cleanup_failed")
        }
        if relative.isEmpty { try ledgerCheckpoint?(.beforeStageParentSync) }
        try ledgerCheckpoint?(.beforeTreeDirectorySync(relative))
        let syncResult = treeDirectorySync?(parent.raw, relative) ?? fsync(parent.raw)
        guard syncResult == 0 else { throw BeamItClipboardFailure("cleanup_failed") }
        try ledgerCheckpoint?(.afterTreeDirectorySync(relative))
        await Task.yield()
        return left
    }
    private func clean(_ id: String, expected: DirectoryIdentity?, legacyAllowed: Bool,
                       abandonedOnly: Bool) async throws -> Bool {
        // Actor reentrancy must not create two recursive deleters for one
        // lease. Duplicate terminal/lifecycle callers join the same result.
        if let pending = cleanupRuns[id] {
            pending.waiters += 1
            defer { pending.waiters -= 1 }
            while pending.result == nil {
                try Task.checkCancellation()
                try await Task.sleep(for: .milliseconds(10))
            }
            return try pending.result!.get()
        }
        let pending = CleanupRun()
        cleanupRuns[id] = pending
        defer { cleanupRuns.removeValue(forKey: id) }
        do {
            let result = try await performCleanV2(id, expected: expected, legacyAllowed: legacyAllowed,
                                                 abandonedOnly: abandonedOnly)
            pending.result = .success(result)
            return result
        } catch {
            pending.result = .failure(error)
            throw error
        }
    }
    private func performCleanV2(_ id: String, expected: DirectoryIdentity?, legacyAllowed: Bool,
                                abandonedOnly: Bool) async throws -> Bool {
        _ = try Bulk.decodeID(id, bytes: 16)
        let root = try rootFD(), ledgers = try child(root, ".reservations")
        var targetParent: FD?
        var targetStamp: Stamp?
        var lockedLease: FD?
        var record: ReservationRecord?
        var isLegacyClaim = false
        do {
          let mutex = try lock(root)
          do {
            try verifyEntry(root, name: ".quota-lock", expected: stamp(mutex))
            let committed = try child(root, "client/committed")
            let isCommitted = try exists(committed, name: id)
            let owner = committedOwners[id] ?? transfers[id]
            if abandonedOnly && owner != nil { return false }
            if let owner, isCommitted != owner.promoted, isCommitted {
                throw BeamItClipboardFailure("cleanup_ownership_uncertain")
            }
            let recordDirectoryExists = try exists(child(root, ".reservation-records"), name: id)
            let canonicalExists = try entryExists(ledgers, name: id)
            var current = try reservationRecord(root, id: id, lease: nil)
            if current == nil, canonicalExists {
                let old = try FD(openat(ledgers.raw, id, O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC))
                current = try reservationRecord(root, id: id, lease: old)
            }
            if let owner {
                guard let current, current.version == 2 else {
                    throw BeamItClipboardFailure("cleanup_ownership_uncertain")
                }
                lockedLease = owner.lease
                if current.deleted,
                    !(try entryExists(try child(root, ".reservation-records/" + id), name: "lease.pin")) {
                    guard !(try entryExists(ledgers, name: id)) else {
                        throw BeamItClipboardFailure("cleanup_ownership_uncertain")
                    }
                } else {
                    try verifyBoundRecord(root, id: id, record: current, lease: owner.lease,
                                          canonicalMustExist: !current.deleted)
                }
            } else if let claim = cleanupClaims[id], let current, current.version == 2 {
                lockedLease = claim
                if current.deleted,
                    !(try entryExists(try child(root, ".reservation-records/" + id), name: "lease.pin")) {
                    guard !(try entryExists(ledgers, name: id)) else {
                        throw BeamItClipboardFailure("cleanup_ownership_uncertain")
                    }
                } else {
                    try verifyBoundRecord(root, id: id, record: current, lease: claim,
                                          canonicalMustExist: !current.deleted)
                }
            } else if let current, current.version == 2 {
                record = current
                let pinPath = try child(root, ".reservation-records/" + id)
                if try entryExists(pinPath, name: "lease.pin") {
                    let pin = try openPinnedLease(root, id: id, record: current)
                    let canonicalMatches: Bool
                    do { canonicalMatches = try verifyCanonicalLease(root, id: id, record: current, pinned: pin) }
                    catch {
                        reportRecoveryProblem()
                        if flock(pin.raw, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK { return false }
                        cleanupClaims[id] = pin; reportRecoveryProblem()
                        throw BeamItClipboardFailure("cleanup_ownership_uncertain")
                    }
                    let canonicalEntryExists = try entryExists(ledgers, name: id)
                    let deletedProofOnly = current.deleted && !canonicalEntryExists
                    if !canonicalMatches && !deletedProofOnly {
                        reportRecoveryProblem()
                        if flock(pin.raw, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK { return false }
                        cleanupClaims[id] = pin; reportRecoveryProblem()
                        throw BeamItClipboardFailure("cleanup_ownership_uncertain")
                    }
                    if flock(pin.raw, LOCK_EX | LOCK_NB) != 0 {
                        guard errno == EWOULDBLOCK else { throw BeamItClipboardFailure("cleanup_claim_failed") }
                        return false
                    }
                    lockedLease = pin
                } else if !current.deleted {
                    throw BeamItClipboardFailure("cleanup_ownership_uncertain")
                }
            } else {
                guard legacyAllowed else {
                    if isCommitted || canonicalExists || recordDirectoryExists {
                        reportRecoveryProblem()
                        throw BeamItClipboardFailure("cleanup_ownership_uncertain")
                    }
                    guard try !exists(child(root, "client/partial"), name: id),
                          try !exists(child(root, "client/outgoing"), name: id) else {
                        throw BeamItClipboardFailure("cleanup_ownership_uncertain")
                    }
                    return true
                }
                guard !recordDirectoryExists || current != nil else {
                    reportRecoveryProblem()
                    throw BeamItClipboardFailure("cleanup_ownership_uncertain")
                }
                if current == nil {
                    guard isCommitted, !canonicalExists else {
                        reportRecoveryProblem()
                        throw BeamItClipboardFailure("cleanup_ownership_uncertain")
                    }
                    let stage = try child(committed, id)
                    let stageIdentity = try identity(stage)
                    try publishLegacyMarker(root, id: id, stage: stageIdentity, mutex: mutex)
                    current = .init(kind: "legacy_cleanup", bytes: 0, identity: stageIdentity,
                                    leaseIdentity: nil, version: 1)
                }
                guard let old = current, !old.deleted, let stageIdentity = old.identity else {
                    reportRecoveryProblem()
                    throw BeamItClipboardFailure("cleanup_ownership_uncertain")
                }
                let kind = isCommitted ? "committed" : old.kind
                guard ["partial", "outgoing", "committed", "legacy_cleanup"].contains(kind) else {
                    throw BeamItClipboardFailure("cleanup_ownership_uncertain")
                }
                let stageParent = kind == "committed" || kind == "legacy_cleanup" ?
                    committed : try child(root, "client/" + kind)
                guard try exists(stageParent, name: id),
                      try identity(child(stageParent, id)) == stageIdentity,
                      expected == nil || expected == stageIdentity else {
                    reportRecoveryProblem()
                    throw BeamItClipboardFailure("cleanup_identity_changed")
                }
                guard let migrated = try migrateLegacyClaim(root, id: id, old: old,
                                                            stageIdentity: stageIdentity, mutex: mutex) else { return false }
                lockedLease = migrated
                current = try reservationRecord(root, id: id, lease: migrated)
                isLegacyClaim = true
            }
            guard let current, current.version == 2,
                  let authorization = current.identity else {
                reportRecoveryProblem()
                throw BeamItClipboardFailure("cleanup_ownership_uncertain")
            }
            if isCommitted {
                cleanupRecords[id] = CommittedRecord(id: id, identity: authorization)
            }
            try retireMigratedMarker(root, id: id, record: current, mutex: mutex)
            if isCommitted {
                guard current.kind != "outgoing",
                      try !exists(child(root, "client/partial"), name: id),
                      try !exists(child(root, "client/outgoing"), name: id) else {
                    throw BeamItClipboardFailure("cleanup_ownership_uncertain")
                }
            }
            if current.kind == "legacy_cleanup" {
                guard current.bytes == 0 else { throw BeamItClipboardFailure("invalid_quota_ledger") }
                isLegacyClaim = isLegacyClaim || owner == nil
            }
            let parent = isCommitted || current.kind == "legacy_cleanup" ?
                committed : try child(root, "client/" + current.kind)
            if current.deleted {
                guard try !exists(parent, name: id) else {
                    throw BeamItClipboardFailure("cleanup_identity_changed")
                }
            } else {
                guard try exists(parent, name: id) else {
                    reportRecoveryProblem()
                    throw BeamItClipboardFailure("cleanup_identity_changed")
                }
                let stage = try child(parent, id), value = try stamp(stage)
                guard DirectoryIdentity(device: value.device, inode: value.inode) == authorization,
                      expected == nil || expected == authorization else {
                    throw BeamItClipboardFailure("cleanup_identity_changed")
                }
                targetStamp = value
            }
            record = current
            targetParent = parent
            if let lease = lockedLease {
                if current.deleted {
                    // Proof-only recovery may legitimately have no lease path
                    // or pin, but a surviving descriptor must be same inode.
                    if try entryExists(try child(root, ".reservation-records/" + id), name: "lease.pin") {
                        try verifyBoundRecord(root, id: id, record: current, lease: lease,
                                              canonicalMustExist: !current.deleted)
                    }
                } else {
                    try verifyBoundRecord(root, id: id, record: current, lease: lease)
                }
                cleanupClaims[id] = lease
            } else if !current.deleted {
                throw BeamItClipboardFailure("cleanup_ownership_uncertain")
            }
            try verifyEntry(root, name: ".quota-lock", expected: stamp(mutex))
          } catch {
            throw error
          }
        }
        if isLegacyClaim, let cleanupCheckpoint {
            try await cleanupCheckpoint(.afterLegacyClaimRegistration)
        }
        guard let parent = targetParent, let current = record else {
            throw BeamItClipboardFailure("cleanup_ownership_uncertain")
        }
        if let targetStamp {
            guard let lease = lockedLease else { throw BeamItClipboardFailure("cleanup_ownership_uncertain") }
            if let cleanupCheckpoint { try await cleanupCheckpoint(.beforeIdentityCheck) }
            try Task.checkCancellation()
            try verifyBoundRecord(root, id: id, record: current, lease: lease)
            try verifyEntry(parent, name: id, expected: targetStamp)
            let stage = try child(parent, id)
            try await validateCleanupTree(stage)
            _ = try await removeValidatedTree(parent, name: id, expected: targetStamp,
                                              relative: "", remaining: Bulk.maxNodes + 1)
            try Task.checkCancellation()
        }
        let finalLock = try lock(root)
        defer { finalLock.close() }
        try verifyEntry(root, name: ".quota-lock", expected: stamp(finalLock))
        guard try !exists(parent, name: id) else {
            throw BeamItClipboardFailure("cleanup_identity_changed")
        }
        if !current.deleted {
            guard let targetStamp, let lease = lockedLease else {
                throw BeamItClipboardFailure("cleanup_ownership_uncertain")
            }
            try Task.checkCancellation()
            let identity = DirectoryIdentity(device: targetStamp.device, inode: targetStamp.inode)
            try markDeleted(root, id: id, record: current, lease: lease, parent: parent,
                            expected: identity, mutex: finalLock)
            record = ReservationRecord(kind: current.kind, bytes: current.bytes, identity: current.identity,
                leaseIdentity: current.leaseIdentity, version: 2, deleted: true)
        }
        let cleaned = try retireDeletedRecord(root, id: id, record: record!, lease: lockedLease,
                                              mutex: finalLock)
        if cleaned {
            cleanupClaims.removeValue(forKey: id)
            cleanupRecords.removeValue(forKey: id)
            committedOwners.removeValue(forKey: id)
            transfers.removeValue(forKey: id)
            _ = try reconcile(root)
        }
        return cleaned
    }
    func cleanupCommitted(_ record: CommittedRecord) async throws -> Bool {
        try await clean(record.id, expected: record.identity, legacyAllowed: false, abandonedOnly: false)
    }
    func ownedCommittedRecords() throws -> [CommittedRecord] {
        var owned = cleanupRecords
        for transfer in committedOwners.values {
            owned[transfer.id] = CommittedRecord(id: transfer.id, identity: try identity(transfer.directory))
        }
        return Array(owned.values)
    }
    func cleanupStartup(legacyAllowed: Bool) async -> CleanupReport {
        var report = CleanupReport()
        do {
            let root = try rootFD(), committed = try child(root, "client/committed")
            let copy = dup(committed.raw)
            guard copy >= 0 else { throw BeamItClipboardFailure("cleanup_failed") }
            guard let stream = fdopendir(copy) else {
                Darwin.close(copy); throw BeamItClipboardFailure("cleanup_failed")
            }
            defer { closedir(stream) }
            rewinddir(stream)
            // Stream legacy inventories instead of retaining every path/ID in
            // memory. Each candidate is copied before an actor suspension.
            while true {
                try Task.checkCancellation()
                errno = 0
                guard let entry = readdir(stream) else {
                    guard errno == 0 else { throw BeamItClipboardFailure("cleanup_failed") }
                    break
                }
                let count = Int(entry.pointee.d_namlen) + 1
                let name = withUnsafePointer(to: entry.pointee.d_name) {
                    $0.withMemoryRebound(to: CChar.self, capacity: count) { String(validatingCString: $0) }
                }
                guard let id = name else { report.report("cleanup_unexpected_entry"); continue }
                if id == "." || id == ".." { continue }
                do {
                    _ = try Bulk.decodeID(id, bytes: 16)
                    let directory = try child(committed, id)
                    let value = try identity(directory)
                    if try await clean(id, expected: value, legacyAllowed: legacyAllowed, abandonedOnly: true) {
                        // The diagnostic report is bounded; no path inventory
                        // is needed for startup decisions.
                        if report.removed.count < Bulk.maxNodes { report.removed.append(id) }
                    } else { report.report("cleanup_owner_or_legacy_uncertain") }
                } catch is CancellationError { throw CancellationError() }
                catch let failure as BeamItClipboardFailure { report.report(failure.code) }
                catch { report.report("cleanup_failed") }
                await Task.yield()
            }
        } catch is CancellationError { report.report("cleanup_interrupted") }
        catch let failure as BeamItClipboardFailure { report.report(failure.code) }
        catch { report.report("cleanup_failed") }
        return report
    }
    private func reserve(_ id: String, manifest: Bulk.Manifest, kind: String) throws -> Transfer {
        _ = try Bulk.decodeID(id, bytes: 16)
        guard transfers[id] == nil else { throw BeamItClipboardFailure("transfer_busy") }
        let root = try rootFD(), mutex = try lock(root)
        defer { withExtendedLifetime(mutex) {} }
        guard try !exists(child(root, ".reservation-records"), name: id) else {
            throw BeamItClipboardFailure("transfer_busy")
        }
        for kind in ["partial", "outgoing", "committed"] {
            guard try !exists(child(root, "client/" + kind), name: id) else {
                throw BeamItClipboardFailure("transfer_busy")
            }
        }
        let outstanding = try reconcile(root)
        var actual: UInt64 = 0
        // Include residual metadata bytes too. New immutable markers/leases
        // are empty, but a damaged old ledger can contain arbitrary real usage.
        for name in try names(root) where name != ".quota-lock" {
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
        try verifyEntry(root, name: ".quota-lock", expected: stamp(mutex))
        let ledgers = try child(root, ".reservations")
        let lease = try FD(openat(ledgers.raw, id, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0o600))
        guard flock(lease.raw, LOCK_EX | LOCK_NB) == 0 else { throw BeamItClipboardFailure("quota_lock_failed") }
        try ledgerCheckpoint?(.afterLeaseCreation)
        try createLeasePin(root, id: id, lease: lease, mutex: mutex)
        try verifyEntry(root, name: ".quota-lock", expected: stamp(mutex))
        var createdDirectory: FD?
        do {
            try verifyEntry(root, name: ".quota-lock", expected: stamp(mutex))
            let directory = try child(root, "client/" + kind + "/" + id, createDirectories: true)
            createdDirectory = directory
            try ledgerCheckpoint?(.afterDirectoryCreation)
            let stageParent = try child(root, "client/" + kind)
            guard fsync(directory.raw) == 0, fsync(stageParent.raw) == 0 else {
                throw BeamItClipboardFailure("disk_write_failed")
            }
            try publishRecord(root, id: id, record: .init(kind: kind, bytes: bytes,
                identity: identity(directory), leaseIdentity: nil, version: 0), lease: lease, mutex: mutex)
            try verifyEntry(root, name: ".quota-lock", expected: stamp(mutex))
            try verifyBoundRecord(root, id: id,
                record: ReservationRecord(kind: kind, bytes: bytes, identity: identity(directory),
                    leaseIdentity: DirectoryIdentity(device: stamp(lease).device, inode: stamp(lease).inode),
                    version: 2), lease: lease)
            guard fsync(lease.raw) == 0, fsync(ledgers.raw) == 0 else {
                throw BeamItClipboardFailure("disk_write_failed")
            }
            let transfer = Transfer(id: id, kind: kind, lease: lease, directory: directory, manifest: manifest)
            transfers[id] = transfer
            return transfer
        } catch {
            // Preserve the locked original and every possible identity. An
            // interrupted creation is quarantined, not inferred or adopted.
            cleanupClaims[id] = lease
            reportRecoveryProblem()
            _ = createdDirectory
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
        try verifyEntry(root, name: ".quota-lock", expected: stamp(mutex))
        let binding = try reservationRecord(root, id: id, lease: transfer.lease)
        guard let binding, binding.version == 2, !binding.deleted else {
            throw BeamItClipboardFailure("cleanup_ownership_uncertain")
        }
        try verifyBoundRecord(root, id: id, record: binding, lease: transfer.lease)
        let current = try stamp(child(partial, id)), expected = try stamp(transfer.directory)
        guard current.device == expected.device, current.inode == expected.inode else {
            throw BeamItClipboardFailure("unsafe_staging")
        }
        try verifyEntry(root, name: ".quota-lock", expected: stamp(mutex))
        try ledgerCheckpoint?(.beforePromotionRename)
        guard renameatx_np(partial.raw, id, committed.raw, id, UInt32(RENAME_EXCL)) == 0 else {
            throw BeamItClipboardFailure.filesystem("promotion_failed")
        }
        transfer.promoted = true
        committedOwners[id] = transfer
        try ledgerCheckpoint?(.afterPromotionRename)
        try ledgerCheckpoint?(.beforePromotionSourceSync)
        guard fsync(partial.raw) == 0 else {
            throw BeamItClipboardFailure("promotion_failed")
        }
        try ledgerCheckpoint?(.afterPromotionSourceSync)
        try ledgerCheckpoint?(.beforePromotionDestinationSync)
        guard fsync(committed.raw) == 0 else {
            throw BeamItClipboardFailure("promotion_failed")
        }
        try ledgerCheckpoint?(.afterPromotionDestinationSync)
        try verifyEntry(root, name: ".quota-lock", expected: stamp(mutex))
        try verifyBoundRecord(root, id: id, record: binding, lease: transfer.lease)
        try verifyEntry(committed, name: id, expected: expected)
        let directory = rootURL(id)
        return Publication(files: transfer.manifest.nodes.filter { $0.kind != .png && !$0.path.contains("/") }
            .map { directory.appendingPathComponent($0.path) }, png: png,
            record: .init(id: id, identity: .init(device: expected.device, inode: expected.inode)))
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
    func finish(_ id: String, published: Bool = false) async throws {
        // Terminal cleanup must still execute when its originating automatic
        // transfer task is already cancelled. This unstructured utility task
        // does not inherit that cancellation; app termination bounds the drain
        // separately and leaves blocked/failed work leased for startup recovery.
        let terminal = Task(priority: .utility) {
            try await self.finishTerminal(id, published: published)
        }
        try await terminal.value
    }
    private func finishTerminal(_ id: String, published: Bool) async throws {
        sources.removeValue(forKey: id)
        guard let transfer = transfers[id] else { return }
        transfer.file = nil
        if published && transfer.promoted {
            // Live publication remains leased and actual-byte charged after
            // connection cleanup. Only app-lifetime policy retires it.
            committedOwners[id] = transfer
            transfers.removeValue(forKey: id)
            return
        }
        _ = try await clean(id, expected: identity(transfer.directory),
                            legacyAllowed: false, abandonedOnly: false)
    }
}
