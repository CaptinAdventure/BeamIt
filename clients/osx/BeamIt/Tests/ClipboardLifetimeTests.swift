import Foundation
import CryptoKit
import Darwin
import AppKit

// Native standalone tests. Every root, source, destination and lifecycle
// dependency is private/injected. Never initializes the shared coordinator,
// NSPasteboard.general, NSApplication.shared or the user's staging directory.
@main
enum ClipboardLifetimeTests {
    typealias Bulk = BeamItClipboardBulk
    typealias Storage = BeamItClipboardStorage
    typealias Lifetime = BeamItClipboardLifetime

    @MainActor final class Board {
        var count = 0
        var countReads = 0
        var urls: [URL] = []
        func sequence() -> Int { countReads += 1; return count }
    }
    @MainActor final class Clock { var now: TimeInterval = 0; var advancing = false }
    actor Gate {
        private var continuation: CheckedContinuation<Void, Never>?
        private(set) var arrived = false
        private var opened = false
        func park() async throws {
            arrived = true
            if !opened {
                // Intentionally models an I/O call that does not respond to
                // cancellation until the fixture releases it.
                await withCheckedContinuation { continuation = $0 }
            }
            try Task.checkCancellation()
        }
        func open() { opened = true; continuation?.resume(); continuation = nil }
    }
    actor Fault {
        var calls = 0
        var failure: Int32? = EACCES
        func checkpoint(_ point: Storage.CleanupPoint) throws {
            switch point {
            case .afterLegacyClaimRegistration, .beforeIdentityCheck: return
            case .beforeEntryRemoval:
                calls += 1
                if calls == 2, let failure {
                    throw BeamItClipboardFailure.filesystem("cleanup_failed", code: failure)
                }
            }
        }
        func disable() { failure = nil }
    }
    actor Poll {
        private var continuation: CheckedContinuation<Void, Never>?
        private var permits = 0
        private(set) var arrivals = 0
        func sleep(_ duration: Duration) async throws {
            guard duration == .milliseconds(80) else { try await Task.sleep(for: duration); return }
            arrivals += 1
            if permits > 0 { permits -= 1 }
            else { await withCheckedContinuation { continuation = $0 } }
            try Task.checkCancellation()
        }
        func advance() {
            if let continuation { self.continuation = nil; continuation.resume() }
            else { permits += 1 }
        }
    }
    @MainActor final class Peer: BeamItClipboardPeer {
        var calls = 0
        var events: [[String: Any]] = []
        var recordEvents = false
        func clipboardRequest(_ type: String, _ fields: [String: Any], reply: String) async throws -> [String: Any] {
            calls += 1; throw BeamItClipboardFailure("unexpected_fixture_request")
        }
        func clipboardEvent(_ type: String, _ fields: [String: Any]) async throws {
            calls += 1
            guard recordEvents else { throw BeamItClipboardFailure("unexpected_fixture_request") }
            var event = fields; event["type"] = type; events.append(event)
        }
        func setClipboard(_ fields: [String: Any]) async throws {
            calls += 1; throw BeamItClipboardFailure("unexpected_fixture_request")
        }
        func getClipboard() async throws -> [String: Any] {
            calls += 1; throw BeamItClipboardFailure("unexpected_fixture_request")
        }
        func openBulkSocket(_ offeredURL: URL, transferID: String) async throws -> URLSessionWebSocketTask {
            calls += 1; throw BeamItClipboardFailure("unexpected_fixture_request")
        }
    }
    static let bytes = Data([1, 2, 3])
    static func id(_ tag: UInt8) -> String { Bulk.base64URL(Data(repeating: tag, count: 16)) }
    static func manifest(twoFiles: Bool = false) -> Bulk.Manifest {
        var nodes: [Bulk.Node] = []
        if twoFiles {
            nodes.append(.init(kind: .file, path: "a", length: 1, sha256: Data(SHA256.hash(data: Data([9]))),
                               width: nil, height: nil))
        }
        nodes.append(.init(kind: .file, path: "b", length: 3, sha256: Data(SHA256.hash(data: bytes)),
                           width: nil, height: nil))
        return .init(selectedRoots: nodes.count, totalFileBytes: twoFiles ? 4 : 3, nodes: nodes)
    }
    static func receive(_ storage: Storage, tag: UInt8, twoFiles: Bool = false,
                        promote: Bool = true) async throws -> Storage.Publication? {
        let value = id(tag), raw = Data(repeating: tag, count: 16), description = manifest(twoFiles: twoFiles)
        try await storage.beginReceive(value, manifest: description)
        for (index, node) in description.nodes.enumerated() {
            let payload = node.path == "a" ? Data([9]) : bytes
            let frame = try Bulk.encode(kind: .file, transferID: raw, nodeIndex: UInt32(index), offset: 0, payload: payload)
            let ended = try await storage.accept(value, frame: frame)
            precondition(!ended)
        }
        let end = try Bulk.encode(kind: .end, transferID: raw, nodeIndex: .max,
                                 offset: description.totalFileBytes, payload: Data())
        let ended = try await storage.accept(value, frame: end)
        precondition(ended)
        if promote { return try await storage.promote(value) }
        return nil
    }
    static func coordinator(_ storage: Storage, board: Board,
                            eligibility: Lifetime.LegacyEligibility = .alone,
                            clock: Clock? = nil) -> Lifetime {
        if let clock {
            return Lifetime(storage: storage, changeCount: { board.sequence() }, legacyEligibility: { eligibility },
                            sleep: { _ in
                                await MainActor.run { if clock.advancing { clock.now += 0.25 } }
                                try await Task.sleep(for: .milliseconds(1))
                            }, uptime: { clock.now })
        }
        return Lifetime(storage: storage, changeCount: { board.sequence() }, legacyEligibility: { eligibility })
    }
    static func wait(_ condition: @MainActor () async -> Bool) async throws {
        let deadline = ContinuousClock.now + .seconds(6)
        while !(await condition()) {
            guard ContinuousClock.now < deadline else { throw BeamItClipboardFailure("fixture_barrier_timeout") }
            try await Task.sleep(for: .milliseconds(5))
        }
    }
    static func publish(_ lifetime: Lifetime, storage: Storage, board: Board,
                        publication: Storage.Publication) async throws {
        let outcome = try await lifetime.publish(publication.record) {
            precondition(lifetime.isPublishing && lifetime.registeredIDs.contains(publication.record.id))
            board.count += 1
            lifetime.observe() // Own intermediate clear count cannot retire either candidate.
            board.urls = publication.files
            board.count += 1
            return .success(board.count)
        }
        guard case .success = outcome else { preconditionFailure("Expected publication success.") }
        try await storage.finish(publication.record.id, published: true)
    }
    static func legacy(_ root: URL, tag: UInt8) throws -> URL {
        let directory = root.appendingPathComponent("client/committed/" + id(tag))
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true,
                                                attributes: [.posixPermissions: 0o700])
        try bytes.write(to: directory.appendingPathComponent("b"))
        return directory
    }
    static func rejects(_ code: String, _ action: () async throws -> Void) async throws {
        do { try await action(); preconditionFailure("Expected " + code) }
        catch let failure as BeamItClipboardFailure { precondition(failure.code == code) }
    }
    static func startupAndReplacement(_ base: URL) async throws {
        let fs = FileManager.default, root = base.appendingPathComponent("startup")
        let storage = Storage(root: root)
        try await storage.recover()
        let abandoned = id(1), partial = root.appendingPathComponent("client/partial/" + abandoned)
        try fs.createDirectory(at: partial, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        try bytes.write(to: partial.appendingPathComponent("b"))
        var originalPartial = stat()
        precondition(lstat(partial.path, &originalPartial) == 0)
        try JSONSerialization.data(withJSONObject: ["kind": "partial", "bytes": 3,
            "directory_device": UInt32(bitPattern: originalPartial.st_dev), "directory_inode": originalPartial.st_ino])
            .write(to: root.appendingPathComponent(".reservations/" + abandoned))
        try fs.setAttributes([.posixPermissions: 0o600],
                             ofItemAtPath: root.appendingPathComponent(".reservations/" + abandoned).path)
        let old = try legacy(root, tag: 2)
        let unexpected = root.appendingPathComponent("client/committed/unexpected")
        try fs.createDirectory(at: unexpected, withIntermediateDirectories: true)
        let board = Board()
        board.urls = [old.appendingPathComponent("b")] // Startup deliberately leaves this URL stale.
        let lifetime = coordinator(storage, board: board)
        let application = BeamItApplicationLifetime(lifetime: lifetime)
        application.start(observeAutomatically: false) // Actual app-start adapter, no connection/window.
        try await lifetime.ready()
        precondition(!fs.fileExists(atPath: partial.path) && !fs.fileExists(atPath: old.path))
        precondition(fs.fileExists(atPath: unexpected.path) && lifetime.diagnostic != nil)
        precondition(board.urls == [old.appendingPathComponent("b")] && board.count == 0)
        let publication = try await receive(storage, tag: 3)!
        // A second storage cannot acquire the actual promoted kernel lease,
        // before any native URL has been exposed.
        let peer = Storage(root: root)
        let protected = await peer.cleanupStartup(legacyAllowed: true)
        precondition(!protected.problems.isEmpty && fs.fileExists(atPath: publication.files[0].path))
        let claimed = try await peer.cleanupCommitted(publication.record)
        precondition(!claimed) // Direct live-lease rejection, not another entry's diagnostic.
        try await publish(lifetime, storage: storage, board: board, publication: publication)
        for _ in 0..<5 { lifetime.observe() }
        try await lifetime.waitForCleanup()
        precondition(fs.fileExists(atPath: publication.files[0].path))
        let staleURLs = board.urls, retainedCount = board.count
        board.count += 1 // External writer; disabled/inactive/disconnected state is irrelevant.
        lifetime.observe()
        try await lifetime.waitForCleanup()
        precondition(!fs.fileExists(atPath: publication.files[0].path))
        precondition(board.urls == staleURLs && board.count == retainedCount + 1)
        let duplicate = try await storage.cleanupCommitted(publication.record)
        precondition(duplicate) // Duplicate cleanup.
        precondition(lifetime.registeredIDs.isEmpty && board.countReads > 0)
    }
    static func publicationAndRollback(_ base: URL) async throws {
        let fs = FileManager.default, storage = Storage(root: base.appendingPathComponent("publication"))
        let board = Board(), lifetime = coordinator(storage, board: board)
        lifetime.start(observeAutomatically: false); try await lifetime.ready()
        let first = try await receive(storage, tag: 10)!
        try await publish(lifetime, storage: storage, board: board, publication: first)
        let failed = try await receive(storage, tag: 11)!
        let outcome = try await lifetime.publish(failed.record) {
            precondition(lifetime.registeredIDs == Set([first.record.id, failed.record.id]))
            board.count += 1; lifetime.observe()
            precondition(fs.fileExists(atPath: first.files[0].path) && fs.fileExists(atPath: failed.files[0].path))
            board.count += 2 // Failed write and supported-format rollback.
            return .restored(board.count)
        }
        guard case .restored = outcome else { preconditionFailure("Rollback result lost.") }
        try await storage.finish(failed.record.id)
        lifetime.observe(); try await lifetime.waitForCleanup()
        precondition(fs.fileExists(atPath: first.files[0].path) && !fs.fileExists(atPath: failed.files[0].path))
        let next = try await receive(storage, tag: 12)!
        try await publish(lifetime, storage: storage, board: board, publication: next)
        try await lifetime.waitForCleanup()
        precondition(!fs.fileExists(atPath: first.files[0].path) && fs.fileExists(atPath: next.files[0].path))
        let uncertain = try await receive(storage, tag: 13)!
        _ = try await lifetime.publish(uncertain.record) {
            board.count += 2
            return .uncertain(board.count)
        }
        try await storage.finish(uncertain.record.id, published: true)
        board.count += 1; lifetime.observe(); lifetime.observe()
        try await lifetime.waitForCleanup()
        precondition(fs.fileExists(atPath: next.files[0].path) && fs.fileExists(atPath: uncertain.files[0].path))
        precondition(lifetime.diagnostic != nil)
        await lifetime.shutdown()
        precondition(!fs.fileExists(atPath: next.files[0].path) && !fs.fileExists(atPath: uncertain.files[0].path))
    }
    static func legacySafety(_ base: URL) async throws {
        let fs = FileManager.default
        for (offset, eligibility) in [Lifetime.LegacyEligibility.anotherInstance, .unavailable].enumerated() {
            let root = base.appendingPathComponent("legacy-" + String(offset)), storage = Storage(root: root)
            try await storage.recover()
            let retained = try legacy(root, tag: UInt8(20 + offset))
            let lifetime = coordinator(storage, board: Board(), eligibility: eligibility)
            lifetime.start(observeAutomatically: false); try await lifetime.ready()
            precondition(fs.fileExists(atPath: retained.path) && lifetime.diagnostic != nil)
            await lifetime.shutdown() // It must not clean another/uncertain legacy owner's data.
            precondition(fs.fileExists(atPath: retained.path))
        }
        let root = base.appendingPathComponent("startup-failure"), fault = Fault()
        let storage = Storage(root: root, cleanupCheckpoint: { try await fault.checkpoint($0) })
        try await storage.recover()
        let old = try legacy(root, tag: 25), board = Board()
        board.urls = [old.appendingPathComponent("b")]
        let lifetime = coordinator(storage, board: board)
        lifetime.start(observeAutomatically: false); try await lifetime.ready()
        precondition(fs.fileExists(atPath: old.path) && lifetime.diagnostic != nil)
        precondition(lifetime.registeredIDs.contains(id(25)))
        await fault.disable()
        board.count += 1; lifetime.observe(); try await lifetime.waitForCleanup()
        precondition(!fs.fileExists(atPath: old.path) && lifetime.registeredIDs.isEmpty)
        precondition(board.urls == [old.appendingPathComponent("b")])
    }
    static func partialFailureAndQuota(_ base: URL, error: Int32, name: String) async throws {
        let fs = FileManager.default, root = base.appendingPathComponent(name), fault = Fault()
        await fault.disable()
        let storage = Storage(root: root, cleanupCheckpoint: { try await fault.checkpoint($0) })
        let board = Board(), lifetime = coordinator(storage, board: board)
        lifetime.start(observeAutomatically: false); try await lifetime.ready()
        let publication = try await receive(storage, tag: 30, twoFiles: true)!
        try await publish(lifetime, storage: storage, board: board, publication: publication)
        let filler = try legacy(root, tag: 31).appendingPathComponent("b")
        let fd = Darwin.open(filler.path, O_WRONLY | O_NOFOLLOW | O_CLOEXEC)
        guard fd >= 0 else { throw BeamItClipboardFailure("fixture_open_failed") }
        guard ftruncate(fd, 8_000_000_000 - 128 * 1024 * 1024 - 3) == 0 else {
            Darwin.close(fd); throw BeamItClipboardFailure("fixture_sparse_failed")
        }
        Darwin.close(fd)
        await fault.set(error)
        // Storage/lifetime failure accounting only. This does NOT exercise
        // receive() or a real clipboard_bulk_complete acknowledgement (L8).
        board.count += 1; lifetime.observe(); try await lifetime.waitForCleanup()
        let directory = root.appendingPathComponent("client/committed/" + publication.record.id)
        precondition(!fs.fileExists(atPath: directory.appendingPathComponent("a").path))
        precondition(fs.fileExists(atPath: directory.appendingPathComponent("b").path))
        precondition(fs.fileExists(atPath: root.appendingPathComponent(".reservations/" + publication.record.id).path))
        precondition(lifetime.diagnostic != nil)
        let calls = await fault.calls
        lifetime.observe(); try await lifetime.waitForCleanup()
        let duplicateCalls = await fault.calls
        precondition(duplicateCalls == calls) // Duplicate observation does not spin retries.
        let peer = Storage(root: root)
        try await rejects("quota_exceeded") { try await peer.beginReceive(id(32), manifest: manifest()) }
        await fault.disable()
        board.count += 1; lifetime.observe(); try await lifetime.waitForCleanup()
        precondition(!fs.fileExists(atPath: directory.path))
        try await peer.beginReceive(id(32), manifest: manifest()) // Exact quota fit after actual deletion.
        try await peer.finish(id(32))
        precondition(!(lifetime.diagnostic ?? "").contains(root.path))
    }
    static func hostileReplacement(_ base: URL) async throws {
        let fs = FileManager.default, root = base.appendingPathComponent("hostile"), gate = Gate()
        let storage = Storage(root: root, cleanupCheckpoint: { point in
            switch point {
            case .beforeIdentityCheck: try await gate.park()
            case .afterLegacyClaimRegistration, .beforeEntryRemoval: break
            }
        })
        let board = Board(), lifetime = coordinator(storage, board: board)
        lifetime.start(observeAutomatically: false); try await lifetime.ready()
        let publication = try await receive(storage, tag: 40)!
        try await publish(lifetime, storage: storage, board: board, publication: publication)
        let source = base.appendingPathComponent("source"), destination = base.appendingPathComponent("destination")
        try bytes.write(to: source); try bytes.write(to: destination)
        let outside = base.appendingPathComponent("outside")
        try fs.createDirectory(at: outside, withIntermediateDirectories: true)
        try bytes.write(to: outside.appendingPathComponent("sentinel"))
        let directory = root.appendingPathComponent("client/committed/" + publication.record.id)
        let moved = root.appendingPathComponent("client/committed/unexpected-moved")
        board.count += 1; lifetime.observe()
        try await wait { await gate.arrived } // Claim acquired, shared lock released, deletion not begun.
        try fs.moveItem(at: directory, to: moved)
        try fs.createSymbolicLink(at: directory, withDestinationURL: outside)
        await gate.open(); try await lifetime.waitForCleanup()
        precondition(fs.fileExists(atPath: moved.appendingPathComponent("b").path))
        let sentinel = try Data(contentsOf: outside.appendingPathComponent("sentinel"))
        let original = try Data(contentsOf: source), copied = try Data(contentsOf: destination)
        precondition(sentinel == bytes && original == bytes && copied == bytes)
        precondition(lifetime.diagnostic != nil)
        try fs.removeItem(at: directory) // Remove only the injected private symlink.
        try fs.moveItem(at: moved, to: directory)
        let link = directory.appendingPathComponent("unexpected-link")
        try fs.createSymbolicLink(at: link, withDestinationURL: outside)
        board.count += 1; lifetime.observe(); try await lifetime.waitForCleanup()
        precondition(fs.fileExists(atPath: directory.appendingPathComponent("b").path))
        let remainingSentinel = try Data(contentsOf: outside.appendingPathComponent("sentinel"))
        precondition(remainingSentinel == bytes)
        try fs.removeItem(at: link)
        board.count += 1; lifetime.observe(); try await lifetime.waitForCleanup()
        precondition(!fs.fileExists(atPath: directory.path))
    }
    static func slowCleanupAndTermination(_ base: URL) async throws {
        let root = base.appendingPathComponent("slow"), gate = Gate()
        let storage = Storage(root: root, cleanupCheckpoint: { point in
            switch point {
            case .beforeEntryRemoval: try await gate.park()
            case .afterLegacyClaimRegistration, .beforeIdentityCheck: break
            }
        })
        let board = Board(), lifetime = coordinator(storage, board: board)
        lifetime.start(observeAutomatically: false); try await lifetime.ready()
        let publication = try await receive(storage, tag: 50)!
        try await publish(lifetime, storage: storage, board: board, publication: publication)
        board.count += 1; lifetime.observe(); try await wait { await gate.arrived }
        let peer = Storage(root: root)
        var admitted = false
        let admission = Task {
            defer { admitted = true }
            try await peer.beginReceive(id(51), manifest: manifest())
        }
        try await wait { admitted } // A slow recursive delete does not hold the quota lock.
        try await admission.value
        try await peer.finish(id(51))
        let duplicate = Task { try await storage.cleanupCommitted(publication.record) }
        try await wait { await storage.activeCleanupWaiters == 1 }
        let began = ContinuousClock.now
        let application = BeamItApplicationLifetime(lifetime: lifetime)
        var replies = 0
        application.requestTermination { replies += 1 }
        application.requestTermination { replies += 100 } // Same application termination is idempotent.
        try await wait { replies > 0 }
        precondition(replies == 1 && began.duration(to: .now) < .seconds(3))
        precondition(FileManager.default.fileExists(atPath: publication.files[0].path))
        await gate.open(); try await lifetime.waitForCleanup()
        do { _ = try await duplicate.value; preconditionFailure("Joined cancellation result was lost.") }
        catch is CancellationError {}
        precondition(lifetime.diagnostic != nil)
        // Quiesced shutdown uses the same real app adapter and deletes own paths.
        let quiescedStorage = Storage(root: base.appendingPathComponent("quiesced"))
        let quiesced = coordinator(quiescedStorage, board: Board())
        quiesced.start(observeAutomatically: false); try await quiesced.ready()
        let own = try await receive(quiescedStorage, tag: 52)!
        let termination = BeamItApplicationLifetime(lifetime: quiesced)
        var completed = false
        termination.requestTermination { completed = true }
        try await wait { completed }
        precondition(!FileManager.default.fileExists(atPath: own.files[0].path))
        let clock = Clock(), busyStorage = Storage(root: base.appendingPathComponent("busy"))
        let busy = coordinator(busyStorage, board: Board(), clock: clock)
        busy.start(observeAutomatically: false)
        try await wait { busy.recoveryFinished }
        try await busy.ready()
        let pending = try await receive(busyStorage, tag: 53)!
        let owner = Board()
        var cancelled = false
        busy.registerWork(owner, cancel: { cancelled = true }, busy: { true })
        clock.advancing = true
        await busy.shutdown()
        precondition(cancelled && clock.now >= 2)
        precondition(FileManager.default.fileExists(atPath: pending.files[0].path))
        let cancelledRoot = base.appendingPathComponent("cancelled-terminal")
        let cancelledStorage = Storage(root: cancelledRoot)
        _ = try await receive(cancelledStorage, tag: 54, promote: false)
        let terminal = Task {
            withUnsafeCurrentTask { $0?.cancel() }
            precondition(Task.isCancelled)
            try await cancelledStorage.finish(id(54))
        }
        try await terminal.value
        precondition(!FileManager.default.fileExists(atPath:
            cancelledRoot.appendingPathComponent("client/partial/" + id(54)).path))
        precondition(!FileManager.default.fileExists(atPath:
            cancelledRoot.appendingPathComponent(".reservations/" + id(54)).path))
    }
        static func legacyClaimRace(_ base: URL) async throws {
            let fs = FileManager.default
            for injectError in [false, true] {
                let root = base.appendingPathComponent("legacy-claim-" + String(injectError)), gate = Gate()
                var storage: Storage? = Storage(root: root, cleanupCheckpoint: { point in
                    if case .afterLegacyClaimRegistration = point {
                        try await gate.park()
                        if injectError { throw BeamItClipboardFailure.filesystem("cleanup_failed", code: EACCES) }
                    }
                })
                try await storage!.recover()
                let directory = try legacy(root, tag: 80)
                var original = stat()
                precondition(lstat(directory.path, &original) == 0)
                let moved = base.appendingPathComponent("legacy-original-" + String(injectError))
                let attempt = Task { await storage!.cleanupStartup(legacyAllowed: true) }
                try await wait { await gate.arrived } // Locked, typed, identity-bound claim; BEFORE identity validation.
                try fs.moveItem(at: directory, to: moved)
                try fs.createDirectory(at: directory, withIntermediateDirectories: false,
                                       attributes: [.posixPermissions: 0o700])
                let sentinel = directory.appendingPathComponent("replacement")
                try bytes.write(to: sentinel)
                await gate.open()
                let failed = await attempt.value
                precondition(!failed.problems.isEmpty)
                let records = try await storage!.ownedCommittedRecords()
                precondition(records.count == 1 && records[0].identity.device == original.st_dev &&
                             records[0].identity.inode == original.st_ino)
                let ledger = root.appendingPathComponent(".reservations/" + id(80))
                let markers = try fs.contentsOfDirectory(atPath:
                    root.appendingPathComponent(".reservation-records/" + id(80)).path)
                var leaseIdentity = stat()
                precondition(lstat(ledger.path, &leaseIdentity) == 0)
                precondition(markers == ["lease.pin", v2Marker(id(80), kind: "legacy_cleanup",
                    bytes: 0, stage: original, lease: leaseIdentity)].sorted())
                storage = nil // Drop the first owner/claim. The next check must rely on persisted metadata.
                let successor = Storage(root: root)
                let disabled = await successor.cleanupStartup(legacyAllowed: false)
                precondition(!disabled.problems.isEmpty && fs.fileExists(atPath: sentinel.path))
                let enabled = await successor.cleanupStartup(legacyAllowed: true)
                precondition(!enabled.problems.isEmpty && fs.fileExists(atPath: sentinel.path))
                let preserved = try Data(contentsOf: sentinel), source = try Data(contentsOf: moved.appendingPathComponent("b"))
                precondition(preserved == bytes && source == bytes && fs.fileExists(atPath: ledger.path))
            }
            // A synthetic ordinary ledger left by an older implementation must
            // not bypass conservative legacy eligibility either.
            let root = base.appendingPathComponent("old-unbound"), storage = Storage(root: root)
            try await storage.recover()
            let retained = try legacy(root, tag: 81)
            let ledger = root.appendingPathComponent(".reservations/" + id(81))
            try JSONSerialization.data(withJSONObject: ["kind": "partial", "bytes": 0]).write(to: ledger)
            try fs.setAttributes([.posixPermissions: 0o600], ofItemAtPath: ledger.path)
            let rejected = await storage.cleanupStartup(legacyAllowed: false)
            precondition(!rejected.problems.isEmpty && fs.fileExists(atPath: retained.appendingPathComponent("b").path))
        }
        static func shutdownAdmission(_ base: URL) async throws {
            let fs = FileManager.default
            for race in [false, true] {
                let root = base.appendingPathComponent("admission-" + String(race))
                let board = Board(), clock = Clock(), gate = Gate()
                let lifetime = coordinator(Storage(root: root), board: board, clock: clock)
                let peer = Peer()
                let sync = BeamItClipboardSync(connection: peer, lifetime: lifetime,
                                              admissionCheckpoint: race ? { try await gate.park() } : nil)
                // Deliberately never start recovery; successful rejection must not
                // create a root, read NSApp, open the real board, or contact a host.
                if !race { clock.advancing = true; await lifetime.shutdown() }
                let download = Task { try await sync.downloadHostClipboard(manual: true) }
                if race {
                    try await wait { await gate.arrived } // The actual common entry has passed its FIRST acceptance guard.
                    clock.advancing = true
                    await lifetime.shutdown()
                    await gate.open()
                }
                do { try await download.value; preconditionFailure("Shutdown admitted a manual download.") }
                catch is CancellationError {}
                precondition(!lifetime.acceptsTransfers && peer.calls == 0 && !fs.fileExists(atPath: root.path))
            }
        }
        static func automaticObserverAndLaunchCallback(_ base: URL) async throws {
            let fs = FileManager.default, root = base.appendingPathComponent("automatic-observer"), poll = Poll()
            let board = Board(), storage = Storage(root: root)
            let lifetime = Lifetime(storage: storage, changeCount: { board.sequence() }, legacyEligibility: { .alone },
                                    sleep: { try await poll.sleep($0) })
            let application = BeamItApplicationLifetime(lifetime: lifetime)
            // Invoke the actual launch delegate callback, but NOT NSApplication's
            // event loop or real launch/quit. No connection/sync/window is created.
            application.applicationDidFinishLaunching(Notification(name: NSApplication.didFinishLaunchingNotification))
            try await lifetime.ready()
            try await wait { await poll.arrivals == 1 }
            let publication = try await receive(storage, tag: 82)!
            try await publish(lifetime, storage: storage, board: board, publication: publication)
            let reads = board.countReads
            await poll.advance()
            try await wait { await poll.arrivals == 2 }
            try await lifetime.waitForCleanup()
            precondition(board.countReads > reads && fs.fileExists(atPath: publication.files[0].path))
            board.count += 1
            await poll.advance()
            try await wait { await poll.arrivals == 3 } // Observe ran automatically, not through a manual observe() call.
            try await lifetime.waitForCleanup()
            precondition(!fs.fileExists(atPath: publication.files[0].path) && lifetime.registeredIDs.isEmpty)
            await lifetime.shutdown()
            await poll.advance() // Release cancelled fixture timer, preventing a suspended test task leak.
        }
        static func textOnlyReplacement(_ base: URL) async throws {
            let fs = FileManager.default, storage = Storage(root: base.appendingPathComponent("text-only"))
            let board = Board(), lifetime = coordinator(storage, board: board)
            lifetime.start(observeAutomatically: false); try await lifetime.ready()
            let publication = try await receive(storage, tag: 83)!
            try await publish(lifetime, storage: storage, board: board, publication: publication)
            let restored = try await lifetime.publish(nil) {
                board.count += 2
                return .restored(board.count)
            }
            guard case .restored = restored else { preconditionFailure("Text-only rollback lost.") }
            lifetime.observe(); try await lifetime.waitForCleanup()
            precondition(fs.fileExists(atPath: publication.files[0].path) &&
                         lifetime.registeredIDs == Set([publication.record.id]))
            let replaced = try await lifetime.publish(nil) {
                board.count += 1; board.urls = []
                return .success(board.count)
            }
            guard case .success = replaced else { preconditionFailure("Text-only publication lost.") }
            try await lifetime.waitForCleanup()
            precondition(!fs.fileExists(atPath: publication.files[0].path) && lifetime.registeredIDs.isEmpty)
            await lifetime.shutdown()
        }
        actor RemovalGate {
            let gate: Gate
            let index: Int
            var calls = 0
            init(gate: Gate, index: Int) { self.gate = gate; self.index = index }
            func checkpoint(_ point: Storage.CleanupPoint) async throws {
                if case .beforeEntryRemoval = point {
                    calls += 1
                    if calls == index { try await gate.park() }
                }
            }
        }
        static func nestedPreUnlinkReplacement(_ base: URL) async throws {
            let fs = FileManager.default
            for folder in [false, true] {
                let root = base.appendingPathComponent("nested-replacement-" + String(folder)), gate = Gate()
                let removal = RemovalGate(gate: gate, index: folder ? 2 : 1)
                let storage = Storage(root: root, cleanupCheckpoint: { try await removal.checkpoint($0) })
                try await storage.recover()
                let directory = try legacy(root, tag: 84), entry = directory.appendingPathComponent("b")
                if folder {
                    try fs.removeItem(at: entry)
                    try fs.createDirectory(at: entry, withIntermediateDirectories: false)
                    try bytes.write(to: entry.appendingPathComponent("child"))
                }
                let moved = base.appendingPathComponent("nested-original-" + String(folder))
                let cleanup = Task { await storage.cleanupStartup(legacyAllowed: true) }
                try await wait { await gate.arrived } // Original opened/stat'ed; immediately BEFORE exact pre-unlink check.
                try fs.moveItem(at: entry, to: moved)
                let sentinel: URL
                if folder {
                    try fs.createDirectory(at: entry, withIntermediateDirectories: false)
                    sentinel = entry.appendingPathComponent("replacement")
                } else { sentinel = entry }
                try bytes.write(to: sentinel)
                await gate.open()
                let report = await cleanup.value
                precondition(!report.problems.isEmpty && fs.fileExists(atPath: moved.path))
                let retained = try Data(contentsOf: sentinel)
                precondition(retained == bytes && fs.fileExists(atPath: directory.path) &&
                             fs.fileExists(atPath: root.appendingPathComponent(".reservations/" + id(84)).path))
            }
        }
    static func fixedManifest(_ count: Int) -> Bulk.Manifest {
        .init(selectedRoots: 1, totalFileBytes: UInt64(count), nodes: [
            .init(kind: .file, path: "b", length: UInt64(count),
                  sha256: Data(SHA256.hash(data: Data(repeating: 9, count: count))), width: nil, height: nil)
        ])
    }
    static func nestedManifest() -> Bulk.Manifest {
        .init(selectedRoots: 1, totalFileBytes: 3, nodes: [
            .init(kind: .directory, path: "folder", length: nil, sha256: nil, width: nil, height: nil),
            .init(kind: .file, path: "folder/b", length: 3, sha256: Data(SHA256.hash(data: bytes)),
                  width: nil, height: nil)
        ])
    }
    static func treeDirectorySyncOrdering(_ base: URL) async throws {
        let fs = FileManager.default
        for mode in ["--tree-sync-failure", "--tree-sync-crash"] {
            let root = base.appendingPathComponent(String(mode.dropFirst(2)))
            let process = try subprocess(mode, root: root)
            defer { if process.isRunning { process.terminate() } }
            try await wait { !process.isRunning }
            precondition(process.terminationStatus == 0)
            let records = root.appendingPathComponent(".reservation-records/" + id(123))
            let names = try fs.contentsOfDirectory(atPath: records.path)
            precondition(names.contains(where: { $0.hasPrefix("v2.") }) &&
                         fs.fileExists(atPath: root.appendingPathComponent(".reservations/" + id(123)).path))
            let marker = names.first(where: { $0.hasPrefix("v2.") })!
            precondition(!marker.hasPrefix("deleted_"))
            let successor = Storage(root: root)
            try await successor.recover()
            precondition(!fs.fileExists(atPath: root.appendingPathComponent("client/partial/" + id(123)).path))
            precondition(!fs.fileExists(atPath: records.path) &&
                         !fs.fileExists(atPath: root.appendingPathComponent(".reservations/" + id(123)).path))
        }
        let root = base.appendingPathComponent("tree-sync-cancel"), gate = Gate()
        let storage = Storage(root: root, cleanupCheckpoint: { point in
            if case .beforeEntryRemoval = point { try await gate.park() }
        })
        let publication = try await receive(storage, tag: 123)!
        let cleanup = Task { try await storage.cleanupCommitted(publication.record) }
        try await wait { await gate.arrived }
        cleanup.cancel()
        await gate.open()
        do { _ = try await cleanup.value; preconditionFailure("Expected cleanup-task cancellation.") }
        catch is CancellationError {}
        let records = root.appendingPathComponent(".reservation-records/" + id(123))
        let markers = try fs.contentsOfDirectory(atPath: records.path)
        precondition(markers.count == 2 && markers.contains(where: { $0.hasPrefix("v2.") }))
        precondition(fs.fileExists(atPath: root.appendingPathComponent("client/committed/" + id(123) + "/b").path))
        precondition(fs.fileExists(atPath: root.appendingPathComponent(".reservations/" + id(123)).path))
        try await storage.cleanupCommitted(publication.record)
        precondition(!fs.fileExists(atPath: records.path))

        let partialRoot = base.appendingPathComponent("partial-tree-cancel")
        let partialGate = Gate()
        let removal = RemovalGate(gate: partialGate, index: 2)
        let partialStorage = Storage(root: partialRoot,
                                     cleanupCheckpoint: { try await removal.checkpoint($0) })
        let partialPublication = try await receive(partialStorage, tag: 132, twoFiles: true)!
        let partialStage = partialRoot.appendingPathComponent("client/committed/" + id(132))
        let partialRecords = partialRoot.appendingPathComponent(".reservation-records/" + id(132))
        let canonicalLease = partialRoot.appendingPathComponent(".reservations/" + id(132))
        var originalStage = stat(), originalLease = stat()
        precondition(lstat(partialStage.path, &originalStage) == 0 &&
                     lstat(canonicalLease.path, &originalLease) == 0)
        let partialCleanup = Task {
            try await partialStorage.cleanupCommitted(partialPublication.record)
        }
        try await wait { await partialGate.arrived }
        partialCleanup.cancel()
        await partialGate.open()
        do {
            _ = try await partialCleanup.value
            preconditionFailure("Cancellation after a child unlink must stop tree cleanup.")
        } catch is CancellationError {}
        let remainingNames = try fs.contentsOfDirectory(atPath: partialStage.path).sorted()
        let remainingBytes = try Data(contentsOf: partialStage.appendingPathComponent("b"))
        precondition(remainingNames == ["b"] && remainingBytes == bytes)
        var retainedStage = stat(), retainedLease = stat(), retainedPin = stat()
        let pinPath = partialRecords.appendingPathComponent("lease.pin")
        precondition(lstat(partialStage.path, &retainedStage) == 0 &&
                     retainedStage.st_dev == originalStage.st_dev &&
                     retainedStage.st_ino == originalStage.st_ino)
        precondition(lstat(canonicalLease.path, &retainedLease) == 0 &&
                     lstat(pinPath.path, &retainedPin) == 0 &&
                     retainedLease.st_dev == originalLease.st_dev &&
                     retainedLease.st_ino == originalLease.st_ino &&
                     retainedPin.st_dev == originalLease.st_dev &&
                     retainedPin.st_ino == originalLease.st_ino)
        let retainedMarkers = try fs.contentsOfDirectory(atPath: partialRecords.path)
        let expectedMarker = v2Marker(id(132), kind: "partial", bytes: 4,
                                     stage: originalStage, lease: originalLease)
        precondition(retainedMarkers.sorted() == ["lease.pin", expectedMarker].sorted() &&
                     fs.fileExists(atPath: partialRoot.appendingPathComponent(".reservations/" + id(132)).path))
        let retried = try await partialStorage.cleanupCommitted(partialPublication.record)
        precondition(retried &&
                     !fs.fileExists(atPath: partialStage.path) &&
                     !fs.fileExists(atPath: partialRecords.path) &&
                     !fs.fileExists(atPath: canonicalLease.path))
    }
    static func proofBoundaryCancellation(_ base: URL) async throws {
        let fs = FileManager.default
        let cases: [(UInt8, Storage.LedgerPoint)] = [
            (130, .beforeDeletionProofRename),
            (131, .beforeDeletionProofSync)
        ]
        for (tag, boundary) in cases {
            let root = base.appendingPathComponent("proof-boundary-cancel-" + String(tag))
            let storage = Storage(root: root, ledgerCheckpoint: { point in
                if point == boundary { withUnsafeCurrentTask { $0?.cancel() } }
            })
            let publication = try await receive(storage, tag: tag)!
            let stage = root.appendingPathComponent("client/committed/" + id(tag))
            let records = root.appendingPathComponent(".reservation-records/" + id(tag))
            let lease = root.appendingPathComponent(".reservations/" + id(tag))
            var originalStage = stat(), originalLease = stat()
            precondition(lstat(stage.path, &originalStage) == 0 &&
                         lstat(lease.path, &originalLease) == 0)
            let cleanup = Task { try await storage.cleanupCommitted(publication.record) }
            do {
                _ = try await cleanup.value
                preconditionFailure("Cancellation at either proof boundary must stop retirement.")
            } catch is CancellationError {}
            precondition(!fs.fileExists(atPath: stage.path))
            let pin = records.appendingPathComponent("lease.pin")
            var retainedLease = stat(), retainedPin = stat()
            precondition(lstat(lease.path, &retainedLease) == 0 &&
                         lstat(pin.path, &retainedPin) == 0 &&
                         retainedLease.st_dev == originalLease.st_dev &&
                         retainedLease.st_ino == originalLease.st_ino &&
                         retainedPin.st_dev == originalLease.st_dev &&
                         retainedPin.st_ino == originalLease.st_ino)
            let markers = try fs.contentsOfDirectory(atPath: records.path)
            let activeMarker = v2Marker(id(tag), kind: "partial", bytes: 3,
                                        stage: originalStage, lease: originalLease)
            precondition(markers.sorted() == ["lease.pin", activeMarker].sorted() &&
                         fs.fileExists(atPath: lease.path))
        }
    }
    static func v2Marker(_ id: String, kind: String, bytes: UInt64,
                         stage: stat, lease: stat, deleted: Bool = false) -> String {
        (deleted ? "deleted_" : "") + "v2." + id + "." + kind + "." + String(bytes) + "." +
        String(UInt32(bitPattern: stage.st_dev)) + "." + String(stage.st_ino) + "." +
        String(UInt32(bitPattern: lease.st_dev)) + "." + String(lease.st_ino)
    }
    static func interruptedMetadata(_ base: URL) async throws {
        let fs = FileManager.default
        for (index, data) in [Data(), Data("{\"kind\":\"legacy_cleanup\",\"directory_inode\":".utf8)].enumerated() {
            let root = base.appendingPathComponent("torn-old-" + String(index)), storage = Storage(root: root)
            try await storage.recover()
            let unknown = try legacy(root, tag: 90), sentinel = unknown.appendingPathComponent("b")
            try data.write(to: root.appendingPathComponent(".reservations/" + id(90)))
            let abandoned = root.appendingPathComponent("client/partial/" + id(91))
            try fs.createDirectory(at: abandoned, withIntermediateDirectories: true)
            var original = stat()
            precondition(lstat(abandoned.path, &original) == 0)
            try JSONSerialization.data(withJSONObject: ["kind": "partial", "bytes": 3,
                "directory_device": UInt32(bitPattern: original.st_dev), "directory_inode": original.st_ino])
                .write(to: root.appendingPathComponent(".reservations/" + id(91)))
            try fs.setAttributes([.posixPermissions: 0o600],
                                 ofItemAtPath: root.appendingPathComponent(".reservations/" + id(91)).path)
            try await rejects("staging_recovery_incomplete") { try await storage.recover() }
            do { try await storage.recover(legacyAllowed: true) }
            catch let failure as BeamItClipboardFailure {
                precondition(failure.code == "staging_recovery_incomplete")
            }
            precondition(!fs.fileExists(atPath: abandoned.path)) // Other authorized recovery completed despite the quarantined ID.
            let retained = await storage.cleanupStartup(legacyAllowed: true)
            precondition(!retained.problems.isEmpty && fs.fileExists(atPath: sentinel.path))
            // Its untrusted bound prevents root admission even though
            // recovery completed the unrelated identity-bound partial.
            try await rejects("cleanup_ownership_uncertain") {
                try await storage.beginReceive(id(91), manifest: manifest())
            }
            precondition(!fs.fileExists(atPath: root.appendingPathComponent("client/partial/" + id(91)).path))
            let source = try Data(contentsOf: sentinel)
            precondition(source == bytes)
        }
        for code in [EACCES, ENOSPC] {
            let root = base.appendingPathComponent("record-failure-" + String(code))
            let storage = Storage(root: root, ledgerCheckpoint: { point in
                if case .beforeRecordPublication = point {
                    throw BeamItClipboardFailure.filesystem("cleanup_claim_failed", code: code)
                }
            })
            try await storage.recover()
            let received = try legacy(root, tag: 92)
            let failed = await storage.cleanupStartup(legacyAllowed: true)
            precondition(!failed.problems.isEmpty && fs.fileExists(atPath: received.path))
            try await rejects("cleanup_ownership_uncertain") {
                try await storage.beginReceive(id(94), manifest: manifest())
            }
            precondition(!fs.fileExists(atPath: root.appendingPathComponent("client/partial/" + id(94)).path))
            let successor = Storage(root: root)
            try await rejects("staging_recovery_incomplete") { try await successor.recover() }
            let retry = await successor.cleanupStartup(legacyAllowed: true)
            precondition(!retry.problems.isEmpty && fs.fileExists(atPath: received.path))
            try await rejects("cleanup_ownership_uncertain") {
                try await successor.beginReceive(id(93), manifest: manifest())
            }
            precondition(!fs.fileExists(atPath: root.appendingPathComponent("client/partial/" + id(93)).path))
        }
    }
    static func ledgerCrashWindows(_ base: URL) async throws {
        let fs = FileManager.default
        let cases: [(String, Bool, Bool, Bool)] = [
            ("lease", false, false, true), ("pin-link", false, false, true),
            ("directory", false, false, true),
            ("record-directory", false, false, true), ("record", true, false, true),
            ("sync", true, false, false), ("legacy-record", true, true, true),
            ("legacy-directory", false, true, true)
        ]
        for (mode, bound, committed, replace) in cases {
            let root = base.appendingPathComponent("ledger-crash-" + mode)
            let ready = base.appendingPathComponent("ledger-ready-" + mode), release = base.appendingPathComponent("ledger-release-" + mode)
            let ownerReady = base.appendingPathComponent("live-ready-" + mode), ownerRelease = base.appendingPathComponent("live-release-" + mode)
            let owner = try subprocess("--owner", root: root, extra: [ownerReady.path, ownerRelease.path])
            defer { if owner.isRunning { owner.terminate() } }
            try await wait { fs.fileExists(atPath: ownerReady.path) || !owner.isRunning }
            precondition(owner.isRunning && fs.fileExists(atPath: ownerReady.path))
            let crash = try subprocess("--ledger-" + mode, root: root, extra: [ready.path, release.path])
            defer { if crash.isRunning { crash.terminate() } }
            try await wait { fs.fileExists(atPath: ready.path) || !crash.isRunning }
            precondition(crash.isRunning && fs.fileExists(atPath: ready.path))
            let target = root.appendingPathComponent("client/" + (committed ? "committed/" : "partial/") + id(90))
            let original = base.appendingPathComponent("crash-original-" + mode)
            if replace {
                if fs.fileExists(atPath: target.path) {
                    try bytes.write(to: target.appendingPathComponent("original"))
                    try fs.moveItem(at: target, to: original)
                }
                try fs.createDirectory(at: target, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
                try bytes.write(to: target.appendingPathComponent("replacement"))
            }
            try Data([1]).write(to: release)
            try await wait { !crash.isRunning }
            precondition(crash.terminationStatus == 0)
            let successor = Storage(root: root)
            do { try await successor.recover() }
            catch let error as BeamItClipboardFailure { precondition(error.code == "staging_recovery_incomplete") }
            _ = await successor.cleanupStartup(legacyAllowed: false)
            _ = await successor.cleanupStartup(legacyAllowed: true)
            if replace {
                let preserved = try Data(contentsOf: target.appendingPathComponent("replacement"))
                precondition(preserved == bytes)
                if fs.fileExists(atPath: original.path) {
                    let source = try Data(contentsOf: original.appendingPathComponent("original"))
                    precondition(source == bytes)
                }
            } else {
                precondition(bound && !fs.fileExists(atPath: target.path)) // Complete record recovers original, not current replacement.
                precondition(!fs.fileExists(atPath: root.appendingPathComponent(".reservations/" + id(90)).path))
            }
            let protected = root.appendingPathComponent("client/committed/" + id(70) + "/b")
            precondition(fs.fileExists(atPath: protected.path)) // A real live process still holds its kernel lease.
            if ["lease", "pin-link", "directory", "record-directory", "legacy-directory"].contains(mode) {
                try await rejects("cleanup_ownership_uncertain") {
                    try await successor.beginReceive(id(94), manifest: manifest())
                }
                precondition(!fs.fileExists(atPath: root.appendingPathComponent("client/partial/" + id(94)).path))
            } else {
                try await successor.beginReceive(id(94), manifest: manifest())
                try await successor.finish(id(94))
            }
            try Data([1]).write(to: ownerRelease)
            try await wait { !owner.isRunning }
            precondition(owner.terminationStatus == 0)
        }
    }
    static func legacyClaimQuota(_ base: URL) async throws {
        let fs = FileManager.default, root = base.appendingPathComponent("legacy-quota")
        let before = Gate(), deletion = Gate(), removal = RemovalGate(gate: deletion, index: 2)
        let storage = Storage(root: root, cleanupCheckpoint: { point in
            if case .afterLegacyClaimRegistration = point { try await before.park() }
            try await removal.checkpoint(point)
        })
        try await storage.recover()
        let directory = try legacy(root, tag: 95)
        try Data([9]).write(to: directory.appendingPathComponent("a"))
        let live = Storage(root: root)
        try await live.beginReceive(id(96), manifest: manifest()) // Real held lease + payload/scratch promises.
        let filler = root.appendingPathComponent("quota-fixture")
        let fd = Darwin.open(filler.path, O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, 0o600)
        guard fd >= 0 else { throw BeamItClipboardFailure("fixture_open_failed") }
        defer { Darwin.close(fd) }
        precondition(ftruncate(fd, 8_000_000_000 - 2 * 128 * 1024 * 1024 - 9) == 0)
        let cleanup = Task { await storage.cleanupStartup(legacyAllowed: true) }
        try await wait { await before.arrived }
        let peer = Storage(root: root)
        try await rejects("quota_exceeded") { try await peer.beginReceive(id(97), manifest: manifest()) } // One byte over before deletion.
        await before.open()
        try await wait { await deletion.arrived } // a removed; b remains; same typed legacy claim is held.
        precondition(!fs.fileExists(atPath: directory.appendingPathComponent("a").path) &&
                     fs.fileExists(atPath: directory.appendingPathComponent("b").path))
        try await rejects("quota_exceeded") { try await peer.beginReceive(id(98), manifest: fixedManifest(4)) }
        try await peer.beginReceive(id(97), manifest: manifest()) // Exact fit: actual remainder, no legacy scratch/payload double-count.
        precondition(fs.fileExists(atPath: root.appendingPathComponent(".reservations/" + id(96)).path))
        await deletion.open()
        let result = await cleanup.value
        precondition(result.problems.isEmpty && !fs.fileExists(atPath: directory.path) &&
                     !fs.fileExists(atPath: root.appendingPathComponent(".reservation-records/" + id(95)).path))
        try await peer.finish(id(97))
        try await rejects("quota_exceeded") { try await peer.beginReceive(id(98), manifest: fixedManifest(7)) }
        try await peer.beginReceive(id(98), manifest: fixedManifest(6)) // Exact freed-byte accounting after completed cleanup.
        try await peer.finish(id(98)); try await live.finish(id(96))
    }
    static func receiveAcknowledgementThenCleanupFailure(_ base: URL) async throws {
        let fs = FileManager.default, root = base.appendingPathComponent("receive-ack"), fault = Fault()
        await fault.disable()
        let storage = Storage(root: root, cleanupCheckpoint: { try await fault.checkpoint($0) })
        let board = Board(), lifetime = coordinator(storage, board: board)
        lifetime.start(observeAutomatically: false); try await lifetime.ready()
        let peer = Peer(); peer.recordEvents = true
        let sync = BeamItClipboardSync(connection: peer, lifetime: lifetime)
        _ = try await receive(storage, tag: 99, twoFiles: true, promote: false) // Actual binary frames + ordered END.
        var publications = 0, marks = 0
        try await sync.completeReceivedFiles(transferID: id(99), check: {}, publish: { publication in
            publications += 1
            precondition(peer.events.isEmpty && fs.fileExists(atPath: publication.files[0].path))
            let outcome = try await lifetime.publish(publication.record) {
                board.urls = publication.files; board.count += 1
                return .success(board.count)
            }
            guard case .success(let count) = outcome else { preconditionFailure("Fake board publication failed.") }
            return count
        }, didPublish: { marks += 1 })
        precondition(publications == 1 && marks == 1 && peer.events.count == 1)
        precondition(peer.events[0]["type"] as? String == "clipboard_bulk_complete" &&
                     peer.events[0]["transfer_id"] as? String == id(99) &&
                     peer.events[0]["sequence"] as? Int == board.count)
        let acknowledgedEvents = peer.events.map { NSDictionary(dictionary: $0) }
        try await storage.finish(id(99), published: true)
        await fault.set(EACCES)
        board.count += 1; lifetime.observe(); try await lifetime.waitForCleanup()
        precondition(lifetime.diagnostic != nil && peer.events.count == acknowledgedEvents.count &&
                     NSDictionary(dictionary: peer.events[0]).isEqual(acknowledgedEvents[0]))
        precondition(fs.fileExists(atPath: root.appendingPathComponent("client/committed/" + id(99) + "/b").path))
        // The same production completion path cannot ACK before validated END.
        try await storage.beginReceive(id(100), manifest: manifest())
        do {
            try await sync.completeReceivedFiles(transferID: id(100), check: {}, publish: { _ in
                preconditionFailure("Unvalidated END reached board publication.")
            }, didPublish: { preconditionFailure("Unvalidated END was marked published.") })
            preconditionFailure("Missing END completed.")
        } catch let failure as BeamItClipboardFailure { precondition(failure.code == "incomplete_transfer") }
        precondition(peer.events.count == 1)
        try await storage.finish(id(100))
        await fault.disable()
        board.count += 1; lifetime.observe(); try await lifetime.waitForCleanup()
        await lifetime.shutdown()
        sync.stop()
    }
    static func unlinkedLiveLeaseQuota(_ base: URL) async throws {
        let fs = FileManager.default, root = base.appendingPathComponent("unlinked-live-lease")
        let live = Storage(root: root)
        try await live.beginReceive(id(101), manifest: manifest())
        // Hostile metadata replacement seam: unlink only an isolated fixture
        // lease while its real owner still holds the kernel descriptor.
        try fs.removeItem(at: root.appendingPathComponent(".reservations/" + id(101)))
        let filler = root.appendingPathComponent("quota-fixture")
        let fd = Darwin.open(filler.path, O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, 0o600)
        guard fd >= 0 else { throw BeamItClipboardFailure("fixture_open_failed") }
        defer { Darwin.close(fd) }
        precondition(ftruncate(fd, 8_000_000_000 - 2 * 128 * 1024 * 1024 - 5) == 0)
        let peer = Storage(root: root)
        try await rejects("quota_exceeded") { try await peer.beginReceive(id(102), manifest: manifest()) }
        try await peer.beginReceive(id(102), manifest: fixedManifest(2)) // Exact fit with retained missing-lease promises.
        let recovery = Task { try await peer.recover() }
        try await rejects("staging_recovery_incomplete") { try await recovery.value }
        precondition(fs.fileExists(atPath: root.appendingPathComponent("client/partial/" + id(101)).path))
        try await peer.finish(id(102))
        try await rejects("cleanup_identity_changed") { try await live.finish(id(101)) }
    }
    static func renamedMissingLeaseQuota(_ base: URL) async throws {
        let fs = FileManager.default
        let cases = [false, true].flatMap { outgoing in
            [false, true].flatMap { unlinked in [false, true].map { absent in (outgoing, unlinked, absent) } }
        }
        for (outgoing, unlinked, absent) in cases {
            let kind = outgoing ? "outgoing" : "partial"
            let tag = kind + "-" + String(unlinked) + "-" + String(absent)
            let root = base.appendingPathComponent("renamed-lease-" + tag)
            let live = Storage(root: root)
            let ownerID: String
            let fileName: String
            var sourceURL: URL?
            if outgoing {
                let source = base.appendingPathComponent("outgoing-original-" + tag + ".bin")
                try bytes.write(to: source)
                let selection = try await live.inspect(urls: [source], png: nil, width: nil, height: nil)
                let prepared = try await live.prepare(selection)
                precondition(prepared.totalFileBytes == 3)
                ownerID = selection.id; fileName = source.lastPathComponent; sourceURL = source
            } else {
                ownerID = id(103); fileName = "b"
                try await live.beginReceive(ownerID, manifest: manifest())
                let first = try Bulk.encode(kind: .file, transferID: Data(repeating: 103, count: 16),
                                            nodeIndex: 0, offset: 0, payload: Data(bytes.prefix(1)))
                let ended = try await live.accept(ownerID, frame: first)
                precondition(!ended)
            }
            let original = root.appendingPathComponent("client/" + kind + "/" + ownerID)
            let moved = absent ? base.appendingPathComponent("outside-inventory-" + tag) :
                root.appendingPathComponent("client/" + kind + "/renamed-stage")
            var originalIdentity = stat()
            precondition(lstat(original.path, &originalIdentity) == 0)
            let recordDirectory = root.appendingPathComponent(".reservation-records/" + ownerID)
            var leaseIdentity = stat()
            precondition(lstat(recordDirectory.appendingPathComponent("lease.pin").path, &leaseIdentity) == 0)
            if unlinked { try fs.removeItem(at: root.appendingPathComponent(".reservations/" + ownerID)) }
            try fs.moveItem(at: original, to: moved) // Real stage/owner descriptor remains live.
            if outgoing {
                let frame = try await live.senderFrame(ownerID, transferID: Data(repeating: 106, count: 16))
                let header = try Bulk.decode(frame)
                precondition(header.payload == bytes && header.nodeIndex == 0 && header.offset == 0)
            }
            let filler = root.appendingPathComponent("quota-fixture")
            let fd = Darwin.open(filler.path, O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, 0o600)
            guard fd >= 0 else { throw BeamItClipboardFailure("fixture_open_failed") }
            defer { Darwin.close(fd) }
            let peer = Storage(root: root)
            for replacement in [false, true] {
                if replacement {
                    try fs.createDirectory(at: original, withIntermediateDirectories: false,
                                           attributes: [.posixPermissions: 0o700])
                    try Data([77, 78]).write(to: original.appendingPathComponent("sentinel"))
                    var substituted = stat()
                    precondition(lstat(original.path, &substituted) == 0 &&
                                 substituted.st_ino != originalIdentity.st_ino)
                }
                // In-root bytes plus remaining promise total three; outside
                // the inventory the full three-byte promise remains.
                // Replacement bytes never reduce that promise.
                precondition(ftruncate(fd, 8_000_000_000 - 2 * 128 * 1024 * 1024 - 5 -
                                      (replacement ? 2 : 0)) == 0)
                let incoming = id(replacement ? 105 : 104)
                try await rejects("quota_exceeded") { try await peer.beginReceive(incoming, manifest: manifest()) }
                try await peer.beginReceive(incoming, manifest: fixedManifest(2)) // Exact fit, not double-counted spool bytes.
                try await peer.finish(incoming)
                if !replacement {
                    try await rejects(unlinked ? "cleanup_ownership_uncertain" : "cleanup_identity_changed") {
                        try await live.finish(ownerID)
                    }
                    let markers = try fs.contentsOfDirectory(atPath:
                        recordDirectory.path)
                    precondition(markers.sorted() == ["lease.pin", v2Marker(ownerID, kind: kind,
                        bytes: 3, stage: originalIdentity, lease: leaseIdentity)].sorted())
                    precondition(fs.fileExists(atPath: root.appendingPathComponent(".reservations/" + ownerID).path) == !unlinked)
                }
            }
            if !outgoing {
                let tail = try Bulk.encode(kind: .file, transferID: Data(repeating: 103, count: 16),
                                           nodeIndex: 0, offset: 1, payload: Data(bytes.dropFirst()))
                let ended = try await live.accept(ownerID, frame: tail)
                precondition(!ended) // Descriptor-relative receiver still writes the renamed ORIGINAL, never replacement.
            }
            do { try await peer.recover() }
            catch let failure as BeamItClipboardFailure { precondition(failure.code == "staging_recovery_incomplete") }
            try await rejects(unlinked ? "cleanup_ownership_uncertain" : "cleanup_identity_changed") {
                try await live.finish(ownerID)
            }
            let leasePath = root.appendingPathComponent(".reservations/" + ownerID)
            precondition(fs.fileExists(atPath: leasePath.path) == !unlinked)
            let owned = try Data(contentsOf: moved.appendingPathComponent(fileName))
            let substitution = try Data(contentsOf: original.appendingPathComponent("sentinel"))
            precondition(owned == bytes && substitution == Data([77, 78]))
            let markers = try fs.contentsOfDirectory(atPath: recordDirectory.path)
            precondition(markers.sorted() == ["lease.pin", v2Marker(ownerID, kind: kind,
                bytes: 3, stage: originalIdentity, lease: leaseIdentity)].sorted())
            let committed = root.appendingPathComponent("client/committed/" + ownerID)
            try fs.createDirectory(at: committed, withIntermediateDirectories: false)
            try Data([79]).write(to: committed.appendingPathComponent("sentinel"))
            let cleanup = await peer.cleanupStartup(legacyAllowed: true)
            precondition(!cleanup.problems.isEmpty)
            let retained = try Data(contentsOf: committed.appendingPathComponent("sentinel"))
            precondition(retained == Data([79]) && fs.fileExists(atPath: moved.path) &&
                         fs.fileExists(atPath: original.path))
            if let sourceURL {
                let unchanged = try Data(contentsOf: sourceURL)
                precondition(unchanged == bytes)
            }
        }
    }
    nonisolated static func signalOnce(_ url: URL) -> Bool {
        let fd = Darwin.open(url.path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0o600)
        guard fd >= 0 else { return false }
        Darwin.close(fd)
        return true
    }
    static func quotaRootLockContention(_ base: URL) async throws {
        let fs = FileManager.default
        let sameRoot = base.appendingPathComponent("quota-root-same-process")
        let held = base.appendingPathComponent("quota-root-same-held")
        let release = base.appendingPathComponent("quota-root-same-release")
        let attempted = base.appendingPathComponent("quota-root-same-attempted")
        let proceed = base.appendingPathComponent("quota-root-same-proceed")
        let acquired = base.appendingPathComponent("quota-root-same-acquired")
        let first = Storage(root: sameRoot, ledgerCheckpoint: { point in
    if point == .afterQuotaLockAcquired && signalOnce(held) {
        let deadline = ContinuousClock.now + .seconds(6)
        while !FileManager.default.fileExists(atPath: release.path) {
            guard ContinuousClock.now < deadline else {
                throw BeamItClipboardFailure("fixture_barrier_timeout")
            }
            usleep(1000)
        }
    }
        })
        let second = Storage(root: sameRoot, ledgerCheckpoint: { point in
    if point == .beforeQuotaLockAcquire, signalOnce(attempted) {
        let deadline = ContinuousClock.now + .seconds(6)
        while !FileManager.default.fileExists(atPath: proceed.path) {
            guard ContinuousClock.now < deadline else {
                throw BeamItClipboardFailure("fixture_barrier_timeout")
            }
            usleep(1000)
        }
    }
    if point == .afterQuotaLockAcquired { _ = signalOnce(acquired) }
        })
        let firstTask = Task { try await first.beginReceive(id(120), manifest: manifest()) }
        try await wait { fs.fileExists(atPath: held.path) }
        let secondTask = Task { try await second.beginReceive(id(121), manifest: manifest()) }
        try await wait { fs.fileExists(atPath: attempted.path) }
        try Data([1]).write(to: proceed)
        try await Task.sleep(for: .milliseconds(200)) // Bounded observation after holder and contender rendezvous.
        precondition(!fs.fileExists(atPath: acquired.path))
        try Data([1]).write(to: release)
        try await firstTask.value
        try await wait { fs.fileExists(atPath: acquired.path) }
        try await secondTask.value
        try await first.finish(id(120)); try await second.finish(id(121))

        let root = base.appendingPathComponent("quota-root-separate-process")
        let ownerReady = base.appendingPathComponent("quota-root-owner-ready")
        let ownerRelease = base.appendingPathComponent("quota-root-owner-release")
        let owner = try subprocess("--hold-root-quota", root: root,
                           extra: [ownerReady.path, ownerRelease.path])
        defer { if owner.isRunning { owner.terminate() } }
        try await wait { fs.fileExists(atPath: ownerReady.path) || !owner.isRunning }
        precondition(owner.isRunning && fs.fileExists(atPath: ownerReady.path))
        let peerAttempt = base.appendingPathComponent("quota-root-peer-attempt")
        let peerProceed = base.appendingPathComponent("quota-root-peer-proceed")
        let peerAcquired = base.appendingPathComponent("quota-root-peer-acquired")
        let peer = Storage(root: root, ledgerCheckpoint: { point in
    if point == .beforeQuotaLockAcquire, signalOnce(peerAttempt) {
        let deadline = ContinuousClock.now + .seconds(6)
        while !FileManager.default.fileExists(atPath: peerProceed.path) {
            guard ContinuousClock.now < deadline else {
                throw BeamItClipboardFailure("fixture_barrier_timeout")
            }
            usleep(1000)
        }
    }
    if point == .afterQuotaLockAcquired { _ = signalOnce(peerAcquired) }
        })
        let peerTask = Task { try await peer.beginReceive(id(122), manifest: manifest()) }
        try await wait { fs.fileExists(atPath: peerAttempt.path) }
        try Data([1]).write(to: peerProceed)
        try await Task.sleep(for: .milliseconds(200)) // Bounded observation after owner and peer rendezvous.
        precondition(!fs.fileExists(atPath: peerAcquired.path))
        try Data([1]).write(to: ownerRelease)
        try await wait { !owner.isRunning }
        precondition(owner.terminationStatus == 0)
        try await wait { fs.fileExists(atPath: peerAcquired.path) }
        try await peerTask.value
        try await peer.finish(id(122))
        try await peer.recover()
        precondition(!fs.fileExists(atPath: root.appendingPathComponent("client/partial/" + id(120)).path))
    }
    static func promotionCrashWindows(_ base: URL) async throws {
                let fs = FileManager.default
                let cases: [(String, Storage.LedgerPoint, Bool)] = [
                    ("before-rename", .beforePromotionRename, false),
                    ("after-rename", .afterPromotionRename, true),
                    ("before-source-sync", .beforePromotionSourceSync, true),
                    ("after-source-sync", .afterPromotionSourceSync, true),
                    ("before-destination-sync", .beforePromotionDestinationSync, true),
                    ("after-destination-sync", .afterPromotionDestinationSync, true)
                ]
                for (name, _, renamed) in cases {
                    let root = base.appendingPathComponent("promotion-crash-" + name)
                    let process = try subprocess("--promotion-" + name, root: root)
                    defer { if process.isRunning { process.terminate() } }
                    try await wait { !process.isRunning }
                    precondition(process.terminationStatus == 0)
                    let partial = root.appendingPathComponent("client/partial/" + id(124))
                    let committed = root.appendingPathComponent("client/committed/" + id(124))
                    precondition(fs.fileExists(atPath: renamed ? committed.path : partial.path))
                    precondition(!fs.fileExists(atPath: renamed ? partial.path : committed.path))
                    precondition(!fs.fileExists(atPath: base.appendingPathComponent("promotion-published-" + name).path))
                    let successor = Storage(root: root)
                    try await successor.recover()
                    _ = await successor.cleanupStartup(legacyAllowed: false)
                    precondition(!fs.fileExists(atPath: partial.path) && !fs.fileExists(atPath: committed.path))
                    precondition(!fs.fileExists(atPath: root.appendingPathComponent(".reservations/" + id(124)).path) &&
                                 !fs.fileExists(atPath: root.appendingPathComponent(".reservation-records/" + id(124)).path))
                }
            }
    static func oldDeletedMarkerQuarantine(_ base: URL) async throws {
                let fs = FileManager.default, root = base.appendingPathComponent("old-deleted-marker")
                let owner = Storage(root: root), transferID = id(125)
                try await owner.beginReceive(transferID, manifest: manifest())
                let stage = root.appendingPathComponent("client/partial/" + transferID)
                var stageInfo = stat()
                precondition(lstat(stage.path, &stageInfo) == 0)
                let records = root.appendingPathComponent(".reservation-records/" + transferID)
                var leaseInfo = stat()
                precondition(lstat(records.appendingPathComponent("lease.pin").path, &leaseInfo) == 0)
                let versionTwo = v2Marker(transferID, kind: "partial", bytes: 3,
                                          stage: stageInfo, lease: leaseInfo)
                try fs.removeItem(at: records.appendingPathComponent(versionTwo))
                try fs.removeItem(at: records.appendingPathComponent("lease.pin"))
                let oldDeleted = "deleted_partial.3." + String(UInt32(bitPattern: stageInfo.st_dev)) + "." +
                    String(stageInfo.st_ino)
                try Data().write(to: records.appendingPathComponent(oldDeleted))
                try fs.setAttributes([.posixPermissions: 0o600],
                                     ofItemAtPath: records.appendingPathComponent(oldDeleted).path)
                let peer = Storage(root: root)
                try await rejects("staging_recovery_incomplete") { try await peer.recover(legacyAllowed: true) }
                try await rejects("cleanup_ownership_uncertain") {
                    try await peer.beginReceive(id(126), manifest: manifest())
                }
                precondition(fs.fileExists(atPath: stage.path) &&
                             fs.fileExists(atPath: root.appendingPathComponent(".reservations/" + transferID).path) &&
                             fs.fileExists(atPath: records.appendingPathComponent(oldDeleted).path))
                _ = owner // Retain the live owner through the quarantine assertions.
            }
    static func rootLockSubstitution(_ base: URL) async throws {
        let fs = FileManager.default
        let root = base.appendingPathComponent("quota-lock-substitution")
        try await Storage(root: root).recover()
        let lock = root.appendingPathComponent(".quota-lock")
        let original = base.appendingPathComponent("original-quota-lock")
        let replacement = Data([37])
        let storage = Storage(root: root, ledgerCheckpoint: { point in
            if point == .afterQuotaLockAcquired {
                try FileManager.default.moveItem(at: lock, to: original)
                try replacement.write(to: lock)
            }
        })
        try await rejects("cleanup_identity_changed") {
            try await storage.beginReceive(id(114), manifest: manifest())
        }
        let actual = try Data(contentsOf: lock)
        precondition(actual == replacement && fs.fileExists(atPath: original.path))
        precondition(!fs.fileExists(atPath: root.appendingPathComponent(".reservations/" + id(114)).path))
        precondition(!fs.fileExists(atPath: root.appendingPathComponent("client/partial/" + id(114)).path))
    }
    static func bindingCorruption(_ base: URL) async throws {
        let fs = FileManager.default
        for mode in ["missing-pin", "replacement-pin", "conflicting-marker"] {
            let root = base.appendingPathComponent("binding-corruption-" + mode)
            let owner = Storage(root: root), transferID = id(115)
            try await owner.beginReceive(transferID, manifest: manifest())
            let stage = root.appendingPathComponent("client/partial/" + transferID)
            let sentinel = stage.appendingPathComponent("b")
            try bytes.write(to: sentinel)
            var stageIdentity = stat(), leaseIdentity = stat()
            precondition(lstat(stage.path, &stageIdentity) == 0)
            precondition(lstat(root.appendingPathComponent(".reservations/" + transferID).path,
                               &leaseIdentity) == 0)
            let directory = root.appendingPathComponent(".reservation-records/" + transferID)
            let pin = directory.appendingPathComponent("lease.pin")
            if mode == "missing-pin" {
                try fs.removeItem(at: pin)
            } else if mode == "replacement-pin" {
                try fs.removeItem(at: pin)
                try Data().write(to: pin)
                try fs.setAttributes([.posixPermissions: 0o600], ofItemAtPath: pin.path)
            } else {
                let conflict = directory.appendingPathComponent(v2Marker(transferID, kind: "partial",
                    bytes: 4, stage: stageIdentity, lease: leaseIdentity))
                try Data().write(to: conflict)
            }
            let peer = Storage(root: root)
            try await rejects("staging_recovery_incomplete") { try await peer.recover() }
            try await rejects("cleanup_ownership_uncertain") {
                try await peer.beginReceive(id(116), manifest: manifest())
            }
            let retained = try Data(contentsOf: sentinel)
            precondition(retained == bytes &&
                         fs.fileExists(atPath: root.appendingPathComponent(".reservations/" + transferID).path) &&
                         fs.fileExists(atPath: directory.path))
        }
    }
    static func measurementMutation(_ base: URL) async throws {
        let fs = FileManager.default
        for outgoing in [false, true] {
            let kind = outgoing ? "outgoing" : "partial"
            let root = base.appendingPathComponent("measurement-mutation-" + kind), live = Storage(root: root)
            let ownerID: String
            if outgoing {
                let source = base.appendingPathComponent("measurement-source")
                try bytes.write(to: source)
                let selection = try await live.inspect(urls: [source], png: nil, width: nil, height: nil)
                _ = try await live.prepare(selection); ownerID = selection.id
            } else {
                ownerID = id(107)
                try await live.beginReceive(ownerID, manifest: manifest())
            }
            let original = root.appendingPathComponent("client/" + kind + "/" + ownerID)
            let moved = root.appendingPathComponent("client/" + kind + "/measurement-moved")
            try fs.removeItem(at: root.appendingPathComponent(".reservations/" + ownerID))
            let peer = Storage(root: root, ledgerCheckpoint: { point in
                if point == .afterMeasurementUsage && !FileManager.default.fileExists(atPath: moved.path) {
                    // Executed between usage() and exact final parent-entry verification.
                    try FileManager.default.moveItem(at: original, to: moved)
                    try FileManager.default.createDirectory(at: original, withIntermediateDirectories: false)
                    try Data([88]).write(to: original.appendingPathComponent("sentinel"))
                }
            })
            try await peer.beginReceive(id(108), manifest: manifest())
            try await peer.finish(id(108))
            precondition(!fs.fileExists(atPath: root.appendingPathComponent(".reservations/" + id(108)).path))
            let sentinel = try Data(contentsOf: original.appendingPathComponent("sentinel"))
            precondition(sentinel == Data([88]) && fs.fileExists(atPath: moved.path) &&
                         fs.fileExists(atPath: root.appendingPathComponent(".reservation-records/" + ownerID).path))
        }
    }
    static func measurementRaceQuota(_ base: URL) async throws {
        let fs = FileManager.default
        let scratch: Int64 = 128 * 1024 * 1024
        for (index, point) in [Storage.LedgerPoint.afterMeasurementLookup,
                               .afterMeasurementOpen].enumerated() {
            let root = base.appendingPathComponent("measurement-race-" + String(index))
            let owner = Storage(root: root)
            _ = try await receive(owner, tag: 128, promote: true)
            let committed = root.appendingPathComponent("client/committed/" + id(128))
            let measurementPath = root.appendingPathComponent("client/partial/" + id(128))
            let moved = base.appendingPathComponent("measurement-original-" + String(index))
            var boundIdentity = stat()
            precondition(lstat(committed.path, &boundIdentity) == 0)
            try fs.moveItem(at: committed, to: measurementPath)
            var relocatedIdentity = stat()
            precondition(lstat(measurementPath.path, &relocatedIdentity) == 0 &&
                         relocatedIdentity.st_dev == boundIdentity.st_dev &&
                         relocatedIdentity.st_ino == boundIdentity.st_ino)
            try fs.createDirectory(at: committed, withIntermediateDirectories: false,
                                   attributes: [.posixPermissions: 0o700])
            try Data([77]).write(to: committed.appendingPathComponent("sentinel"))
            let filler = root.appendingPathComponent("measurement-quota-fixture")
            let fillerFD = Darwin.open(filler.path, O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, 0o600)
            guard fillerFD >= 0 else { throw BeamItClipboardFailure("fixture_open_failed") }
            defer { Darwin.close(fillerFD) }
            // Actual sentinels total two bytes. Old and new reservations each
            // add three payload bytes plus scratch, so these sizes are exact
            // capacity and one byte over: q - 2*s - 2 - 3 - 3.
            precondition(ftruncate(fillerFD, 8_000_000_000 - 2 * scratch - 7) == 0)
            let raced = base.appendingPathComponent("measurement-raced-" + String(index))
            let peer = Storage(root: root, ledgerCheckpoint: { current in
                guard current == point, signalOnce(raced) else { return }
                try FileManager.default.moveItem(at: measurementPath, to: moved)
                try FileManager.default.createDirectory(at: measurementPath, withIntermediateDirectories: false,
                                                        attributes: [.posixPermissions: 0o700])
                try Data([88]).write(to: measurementPath.appendingPathComponent("sentinel"))
            })
            try await rejects("quota_exceeded") { try await peer.beginReceive(id(129), manifest: manifest()) }
            precondition(fs.fileExists(atPath: raced.path), "The chosen measurement checkpoint must be reached.")
            precondition(fs.fileExists(atPath: moved.appendingPathComponent("b").path))
            var movedIdentity = stat(), committedReplacement = stat(), measuredReplacement = stat()
            precondition(lstat(moved.path, &movedIdentity) == 0 &&
                         movedIdentity.st_dev == boundIdentity.st_dev && movedIdentity.st_ino == boundIdentity.st_ino)
            precondition(lstat(committed.path, &committedReplacement) == 0 &&
                         (committedReplacement.st_dev != boundIdentity.st_dev ||
                          committedReplacement.st_ino != boundIdentity.st_ino))
            precondition(lstat(measurementPath.path, &measuredReplacement) == 0 &&
                         (measuredReplacement.st_dev != boundIdentity.st_dev ||
                          measuredReplacement.st_ino != boundIdentity.st_ino))
            let firstCommittedSentinel = try Data(contentsOf: committed.appendingPathComponent("sentinel"))
            let firstMeasuredSentinel = try Data(contentsOf: measurementPath.appendingPathComponent("sentinel"))
            precondition(firstCommittedSentinel == Data([77]) && firstMeasuredSentinel == Data([88]))
            precondition(ftruncate(fillerFD, 8_000_000_000 - 2 * scratch - 8) == 0)
            try await peer.beginReceive(id(129), manifest: manifest()) // Exact capacity with full uncertain promise.
            try await peer.finish(id(129))
            precondition(fs.fileExists(atPath: root.appendingPathComponent(".reservation-records/" + id(128)).path))
            let originalBytes = try Data(contentsOf: moved.appendingPathComponent("b"))
            let finalCommittedSentinel = try Data(contentsOf: committed.appendingPathComponent("sentinel"))
            let finalMeasuredSentinel = try Data(contentsOf: measurementPath.appendingPathComponent("sentinel"))
            var finalCommittedIdentity = stat(), finalMeasuredIdentity = stat()
            precondition(lstat(committed.path, &finalCommittedIdentity) == 0 &&
                         finalCommittedIdentity.st_dev == committedReplacement.st_dev &&
                         finalCommittedIdentity.st_ino == committedReplacement.st_ino)
            precondition(lstat(measurementPath.path, &finalMeasuredIdentity) == 0 &&
                         finalMeasuredIdentity.st_dev == measuredReplacement.st_dev &&
                         finalMeasuredIdentity.st_ino == measuredReplacement.st_ino)
            precondition(originalBytes == bytes && finalCommittedSentinel == Data([77]) &&
                         finalMeasuredSentinel == Data([88]))
        }
    }
    static func rollbackMetadataPreservation(_ base: URL) async throws {
        let fs = FileManager.default
        let cases = [false, true].flatMap { missingLease in
            [false, true].flatMap { renamed in [false, true].map { early in (missingLease, renamed, early) } }
        }
        for (missingLease, renamed, early) in cases {
            let tag = String(missingLease) + "-" + String(renamed) + "-" + String(early)
            let root = base.appendingPathComponent("rollback-metadata-" + tag)
            let original = root.appendingPathComponent("client/partial/" + id(109))
            let moved = root.appendingPathComponent("client/partial/rollback-original")
            let lease = root.appendingPathComponent(".reservations/" + id(109))
            let oldLease = base.appendingPathComponent("rollback-lease-" + tag)
            let interruption: Storage.LedgerPoint = early ? .afterDirectoryCreation : .afterRecordSync
            let storage = Storage(root: root, ledgerCheckpoint: { point in
                if point == interruption {
                    if missingLease { try FileManager.default.removeItem(at: lease) }
                    else { try FileManager.default.moveItem(at: lease, to: oldLease); try Data().write(to: lease) }
                    if renamed { try FileManager.default.moveItem(at: original, to: moved) }
                    throw BeamItClipboardFailure("fixture_rollback")
                }
            })
            try await rejects("fixture_rollback") { try await storage.beginReceive(id(109), manifest: manifest()) }
            let record = root.appendingPathComponent(".reservation-records/" + id(109))
            let markers = try fs.contentsOfDirectory(atPath: record.path)
            let retained = renamed ? moved : original
            var identity = stat()
            precondition(lstat(retained.path, &identity) == 0)
            if early {
                precondition(markers == ["lease.pin"])
            } else {
                var leaseIdentity = stat()
                let pinPath = record.appendingPathComponent("lease.pin")
                precondition(lstat(pinPath.path, &leaseIdentity) == 0)
                precondition(markers.sorted() == ["lease.pin", v2Marker(id(109), kind: "partial",
                    bytes: 3, stage: identity, lease: leaseIdentity)].sorted())
            }
            precondition(fs.fileExists(atPath: lease.path) == !missingLease)
            if !missingLease {
                let descriptor = Darwin.open(oldLease.path, O_RDWR | O_NOFOLLOW | O_CLOEXEC)
                guard descriptor >= 0 else { throw BeamItClipboardFailure("fixture_open_failed") }
                let result = flock(descriptor, LOCK_EX | LOCK_NB), code = errno
                Darwin.close(descriptor)
                precondition(result != 0 && code == EWOULDBLOCK) // ORIGINAL lease is still kernel-held.
            }
            let filler = root.appendingPathComponent("quota-fixture")
            let descriptor = Darwin.open(filler.path, O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, 0o600)
            guard descriptor >= 0 else { throw BeamItClipboardFailure("fixture_open_failed") }
            defer { Darwin.close(descriptor) }
            precondition(ftruncate(descriptor, 8_000_000_000 - 2 * 128 * 1024 * 1024 - 5) == 0)
            let peer = Storage(root: root)
            if early {
                try await rejects("cleanup_ownership_uncertain") {
                    try await peer.beginReceive(id(112), manifest: manifest())
                }
            } else {
                try await rejects("quota_exceeded") { try await peer.beginReceive(id(112), manifest: manifest()) }
                try await peer.beginReceive(id(113), manifest: fixedManifest(2))
                try await peer.finish(id(113))
            }
            try fs.removeItem(at: filler)
            if !missingLease && !renamed {
                // The original inode remains locked while the canonical name
                // is a different unlocked file. Recovery must inspect the pin,
                // retain the stage and refuse to adopt the replacement.
                let successor = Storage(root: root)
                try await rejects("staging_recovery_incomplete") { try await successor.recover() }
                precondition(fs.fileExists(atPath: original.path) && fs.fileExists(atPath: record.path))
            }
            let committed = try legacy(root, tag: 109)
            let report = await peer.cleanupStartup(legacyAllowed: true)
            let sentinel = try Data(contentsOf: committed.appendingPathComponent("b"))
            precondition(!report.problems.isEmpty && sentinel == bytes && fs.fileExists(atPath: record.path))
        }
    }
    static func successfulRetirementAndCrashes(_ base: URL) async throws {
        let fs = FileManager.default
        for outgoing in [false, true] {
            let root = base.appendingPathComponent("successful-retirement-" + String(outgoing)), storage = Storage(root: root)
            let transferID: String
            if outgoing {
                let source = base.appendingPathComponent("successful-source")
                try bytes.write(to: source)
                let selection = try await storage.inspect(urls: [source], png: nil, width: nil, height: nil)
                _ = try await storage.prepare(selection); transferID = selection.id
            } else {
                transferID = id(110); try await storage.beginReceive(transferID, manifest: manifest())
            }
            try await storage.finish(transferID)
            for path in [".reservations/", ".reservation-records/", "client/partial/", "client/outgoing/"] {
                precondition(!fs.fileExists(atPath: root.appendingPathComponent(path + transferID).path))
            }
        }
        for mode in ["--retire-proof", "--retire-lease"] {
            let root = base.appendingPathComponent(String(mode.dropFirst(2)))
            let process = try subprocess(mode, root: root)
            defer { if process.isRunning { process.terminate() } }
            try await wait { !process.isRunning }
            precondition(process.terminationStatus == 0)
            let records = root.appendingPathComponent(".reservation-records/" + id(111))
            let markers = try fs.contentsOfDirectory(atPath: records.path)
            precondition(markers.count == 1 && markers[0].hasPrefix("deleted_"))
            let replacement = try legacy(root, tag: 111), peer = Storage(root: root)
            _ = await peer.cleanupStartup(legacyAllowed: true)
            let preserved = try Data(contentsOf: replacement.appendingPathComponent("b"))
            precondition(preserved == bytes && fs.fileExists(atPath: records.path))
            try fs.removeItem(at: replacement) // Remove only the injected private substitution.
            try await peer.recover()
            precondition(!fs.fileExists(atPath: records.path) &&
                         !fs.fileExists(atPath: root.appendingPathComponent(".reservations/" + id(111)).path))
        }
        let failures: [(String, Storage.LedgerPoint, Bool)] = [
            ("stage-sync", .beforeStageParentSync, false),
            ("proof-sync", .beforeDeletionProofSync, false),
            ("canonical-sync", .beforeCanonicalLeaseSync, true),
            ("pin-sync", .beforePinRetirementSync, true),
            ("record", .beforeRecordRetirement, true),
            ("record-directory-sync", .beforeRecordDirectorySync, true)
        ]
        for (name, _, proofExpected) in failures {
            let root = base.appendingPathComponent("retirement-failure-" + name)
            let process = try subprocess("--retire-failure-" + name, root: root)
            defer { if process.isRunning { process.terminate() } }
            try await wait { !process.isRunning }
            precondition(process.terminationStatus == 0)
            let peer = Storage(root: root)
            if proofExpected {
                try await peer.recover()
                precondition(!fs.fileExists(atPath: root.appendingPathComponent(".reservation-records/" + id(112)).path))
                precondition(!fs.fileExists(atPath: root.appendingPathComponent(".reservations/" + id(112)).path))
            } else {
                do { try await peer.recover(); preconditionFailure("Missing pre-proof stage must remain uncertain.") }
                catch let failure as BeamItClipboardFailure {
                    precondition(failure.code == "staging_recovery_incomplete")
                }
                let record = root.appendingPathComponent(".reservation-records/" + id(112))
                let lease = root.appendingPathComponent(".reservations/" + id(112))
                precondition(fs.fileExists(atPath: record.path) && fs.fileExists(atPath: lease.path))
            }
        }
    }
    static func originalLeaseClosesBeforeQuotaUnlock(_ base: URL) async throws {
        let fs = FileManager.default
        for (index, point) in [Storage.LedgerPoint.beforePinRetirementSync,
                               .beforeRecordRetirement].enumerated() {
            let root = base.appendingPathComponent("lease-close-order-" + String(index))
            let start = base.appendingPathComponent("lease-close-start-" + String(index))
            let ready = base.appendingPathComponent("lease-close-ready-" + String(index))
            let waiting = base.appendingPathComponent("lease-close-waiting-" + String(index))
            let outcome = base.appendingPathComponent("lease-close-outcome-" + String(index))
            let owner = Storage(root: root, ledgerCheckpoint: { current in
                guard current == point, signalOnce(start) else { return }
                let deadline = ContinuousClock.now + .seconds(6)
                while !fs.fileExists(atPath: waiting.path) {
                    guard ContinuousClock.now < deadline else {
                        throw BeamItClipboardFailure("fixture_barrier_timeout")
                    }
                    usleep(1000)
                }
                throw BeamItClipboardFailure("injected_retirement_failure")
            })
            try await owner.beginReceive(id(123), manifest: manifest())
            let observer = try subprocess("--observe-lease-close", root: root,
                extra: [ready.path, start.path, waiting.path, outcome.path, id(123)])
            defer { if observer.isRunning { observer.terminate() } }
            try await wait { fs.fileExists(atPath: ready.path) || !observer.isRunning }
            precondition(observer.isRunning && fs.fileExists(atPath: ready.path))
            try await rejects("injected_retirement_failure") { try await owner.finish(id(123)) }
            try await wait { !observer.isRunning }
            precondition(observer.terminationStatus == 0)
            let observed = try String(contentsOf: outcome, encoding: .utf8)
            precondition(observed == "released")
            let successor = Storage(root: root)
            try await successor.recover()
        }
    }
    static func childRoot(_ value: String) throws -> URL {
        let url = URL(fileURLWithPath: value)
        guard let resolved = realpath(FileManager.default.temporaryDirectory.path, nil) else {
            throw BeamItClipboardFailure("fixture_root_failed")
        }
        defer { free(resolved) }
        let prefix = String(cString: resolved) + "/beamit-lifetime-"
        guard url.path.hasPrefix(prefix), !url.path.contains("/../") else {
            throw BeamItClipboardFailure("fixture_root_required")
        }
        return url
    }
    static func child(_ arguments: [String]) async throws {
        let root = try childRoot(arguments[2])
        if arguments[1] == "--tree-sync-failure" || arguments[1] == "--tree-sync-crash" {
            let crash = arguments[1] == "--tree-sync-crash"
            let storage = Storage(root: root, ledgerCheckpoint: { point in
                if point == .afterTreeDirectorySync("folder/b"), crash { _exit(0) }
            }, treeDirectorySync: { descriptor, path in
                if path == "folder/b", !crash { return -1 }
                return fsync(descriptor)
            })
            try await storage.beginReceive(id(123), manifest: nestedManifest())
            if crash { preconditionFailure("Tree sync crash checkpoint did not run.") }
            do { try await storage.finish(id(123)); preconditionFailure("Expected tree sync failure.") }
            catch let failure as BeamItClipboardFailure {
                precondition(failure.code == "cleanup_failed")
            }
            _exit(0)
        }
        if arguments[1].hasPrefix("--promotion-") {
            let name = String(arguments[1].dropFirst("--promotion-".count))
            let points: [String: Storage.LedgerPoint] = [
                "before-rename": .beforePromotionRename, "after-rename": .afterPromotionRename,
                "before-source-sync": .beforePromotionSourceSync, "after-source-sync": .afterPromotionSourceSync,
                "before-destination-sync": .beforePromotionDestinationSync,
                "after-destination-sync": .afterPromotionDestinationSync
            ]
            guard let target = points[name] else { throw BeamItClipboardFailure("fixture_arguments") }
            let exitsAtCheckpoint = name.hasPrefix("after-")
            let storage = Storage(root: root, ledgerCheckpoint: { point in
                guard point == target else { return }
                if exitsAtCheckpoint { _exit(0) }
                throw BeamItClipboardFailure("injected_promotion_failure")
            })
            do {
                _ = try await receive(storage, tag: 124, promote: true)
                try Data([1]).write(to: root.deletingLastPathComponent()
                    .appendingPathComponent("promotion-published-" + name))
            } catch let failure as BeamItClipboardFailure {
                precondition(!exitsAtCheckpoint && failure.code == "injected_promotion_failure")
            }
            _exit(0)
        }
        if arguments[1] == "--hold-root-quota" {
            guard arguments.count == 5 else { throw BeamItClipboardFailure("fixture_arguments") }
            let ready = try childRoot(arguments[3]), release = try childRoot(arguments[4])
            let storage = Storage(root: root, ledgerCheckpoint: { point in
                if point == .afterQuotaLockAcquired, signalOnce(ready) {
                    let deadline = ContinuousClock.now + .seconds(6)
                    while !FileManager.default.fileExists(atPath: release.path) {
                        guard ContinuousClock.now < deadline else {
                            throw BeamItClipboardFailure("fixture_barrier_timeout")
                        }
                        usleep(1000)
                    }
                }
            })
            try await storage.beginReceive(id(120), manifest: manifest())
            _exit(0)
        }
        if arguments[1] == "--observe-lease-close" {
            guard arguments.count == 8 else { throw BeamItClipboardFailure("fixture_arguments") }
            let ready = try childRoot(arguments[3]), start = try childRoot(arguments[4])
            let waiting = try childRoot(arguments[5]), outcome = try childRoot(arguments[6])
            let observedID = arguments[7]
            let leasePath = root.appendingPathComponent(".reservations/" + observedID)
            let leaseFD = Darwin.open(leasePath.path, O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)
            guard leaseFD >= 0 else { throw BeamItClipboardFailure("fixture_open_failed") }
            defer { Darwin.close(leaseFD) }
            try Data([1]).write(to: ready)
            try await wait { FileManager.default.fileExists(atPath: start.path) }
            let storage = Storage(root: root, ledgerCheckpoint: { point in
                if point == .beforeQuotaLockAcquire { try Data([1]).write(to: waiting) }
                if point == .afterQuotaLockAcquired {
                    let result = flock(leaseFD, LOCK_EX | LOCK_NB) == 0 ? "released" : "locked"
                    if result == "released" { _ = flock(leaseFD, LOCK_UN) }
                    try Data(result.utf8).write(to: outcome)
                }
            })
            try await storage.beginReceive(id(127), manifest: manifest())
            try await storage.finish(id(127))
            _exit(0)
        }
        if arguments[1].hasPrefix("--retire-failure-") {
            let name = String(arguments[1].dropFirst("--retire-failure-".count))
            let points: [String: Storage.LedgerPoint] = [
                "stage-sync": .beforeStageParentSync, "proof-sync": .beforeDeletionProofSync,
                "canonical-sync": .beforeCanonicalLeaseSync, "pin-sync": .beforePinRetirementSync,
                "record": .beforeRecordRetirement, "record-directory-sync": .beforeRecordDirectorySync
            ]
            guard let point = points[name] else { throw BeamItClipboardFailure("fixture_arguments") }
            let storage = Storage(root: root, ledgerCheckpoint: {
                if $0 == point { throw BeamItClipboardFailure("injected_checkpoint") }
            })
            try await storage.beginReceive(id(112), manifest: manifest())
            do { try await storage.finish(id(112)); preconditionFailure("Expected injected cleanup failure.") }
            catch let failure as BeamItClipboardFailure {
                precondition(failure.code == "injected_checkpoint")
            }
            _exit(0)
        }
        if arguments[1].hasPrefix("--retire-") {
            let point: Storage.LedgerPoint = arguments[1] == "--retire-proof" ? .afterDeletionProof : .afterLeaseRetirement
            let storage = Storage(root: root, ledgerCheckpoint: { if $0 == point { _exit(0) } })
            try await storage.beginReceive(id(111), manifest: manifest())
            try await storage.finish(id(111))
            preconditionFailure("Retirement crash checkpoint did not run.")
        }
        if arguments[1].hasPrefix("--ledger-") {
            guard arguments.count == 5 else { throw BeamItClipboardFailure("fixture_arguments") }
            let mode = String(arguments[1].dropFirst("--ledger-".count))
            let points: [String: Storage.LedgerPoint] = [
                "lease": .afterLeaseCreation, "pin-link": .afterLeasePinLink,
                "directory": .afterDirectoryCreation,
                "record-directory": .afterRecordDirectoryCreation, "record": .afterRecordPublication,
                "sync": .afterRecordSync, "legacy-record": .afterRecordPublication,
                "legacy-directory": .afterRecordDirectoryCreation
            ]
            guard let point = points[mode] else { throw BeamItClipboardFailure("fixture_arguments") }
            let ready = try childRoot(arguments[3]), release = try childRoot(arguments[4])
            let storage = Storage(root: root, ledgerCheckpoint: { current in
                guard current == point else { return }
                try Data([1]).write(to: ready)
                let deadline = ContinuousClock.now + .seconds(6)
                while !FileManager.default.fileExists(atPath: release.path) {
                    guard ContinuousClock.now < deadline else { throw BeamItClipboardFailure("fixture_barrier_timeout") }
                    usleep(1000) // Poll only the explicit private-root release signal; not proof by elapsed sleep.
                }
                _exit(0) // Real process death inside creation/publication with locks still held.
            })
            try await storage.recover()
            if mode.hasPrefix("legacy") {
                _ = try legacy(root, tag: 90)
                _ = await storage.cleanupStartup(legacyAllowed: true)
            } else { try await storage.beginReceive(id(90), manifest: manifest()) }
            preconditionFailure("Ledger crash checkpoint did not run.")
        }
        let storage = Storage(root: root)
        let promoted = arguments[1] != "--crash-partial"
        let publication = try await receive(storage, tag: 70, promote: promoted)
        if arguments[1] == "--crash-published", let publication {
            let board = Board(), lifetime = coordinator(storage, board: board)
            // Start before creating transfer in production; here it sees its own
            // already-held lease and must preserve it.
            lifetime.start(observeAutomatically: false); try await lifetime.ready()
            try await publish(lifetime, storage: storage, board: board, publication: publication)
        }
        if arguments[1] == "--owner" {
            try Data([1]).write(to: childRoot(arguments[3]))
            let release = try childRoot(arguments[4])
            try await wait { FileManager.default.fileExists(atPath: release.path) }
        }
        _exit(0) // Simulated process exit without Swift cleanup/destructors.
    }
    static func subprocess(_ mode: String, root: URL, extra: [String] = []) throws -> Process {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: CommandLine.arguments[0])
        process.arguments = [mode, root.path] + extra
        try process.run()
        return process
    }
    static func crossProcessAndCrashes(_ base: URL) async throws {
        let fs = FileManager.default, root = base.appendingPathComponent("cross-process")
        let ready = base.appendingPathComponent("ready"), release = base.appendingPathComponent("release")
        let owner = try subprocess("--owner", root: root, extra: [ready.path, release.path])
        defer { if owner.isRunning { owner.terminate() } }
        try await wait { fs.fileExists(atPath: ready.path) || !owner.isRunning }
        precondition(owner.isRunning && fs.fileExists(atPath: ready.path))
        let peer = Storage(root: root), board = Board(), lifetime = coordinator(peer, board: board)
        lifetime.start(observeAutomatically: false); try await lifetime.ready()
        let committed = root.appendingPathComponent("client/committed/" + id(70))
        precondition(fs.fileExists(atPath: committed.path) && lifetime.diagnostic != nil)
        await lifetime.shutdown()
        precondition(fs.fileExists(atPath: committed.path))
        try Data([1]).write(to: release)
        try await wait { !owner.isRunning }
        precondition(owner.terminationStatus == 0)
        let successor = coordinator(Storage(root: root), board: Board())
        successor.start(observeAutomatically: false); try await successor.ready()
        precondition(!fs.fileExists(atPath: committed.path))
        for mode in ["--crash-partial", "--crash-promoted", "--crash-published"] {
            let crashRoot = base.appendingPathComponent(String(mode.dropFirst(2)))
            let process = try subprocess(mode, root: crashRoot)
            defer { if process.isRunning { process.terminate() } }
            try await wait { !process.isRunning }
            precondition(process.terminationStatus == 0)
            let recovered = coordinator(Storage(root: crashRoot), board: Board())
            recovered.start(observeAutomatically: false); try await recovered.ready()
            precondition(!fs.fileExists(atPath: crashRoot.appendingPathComponent("client/partial/" + id(70)).path))
            precondition(!fs.fileExists(atPath: crashRoot.appendingPathComponent("client/committed/" + id(70)).path))
        }
    }
    static func main() async throws {
        if CommandLine.arguments.count >= 3 {
            try await child(CommandLine.arguments)
            return
        }
        let fs = FileManager.default
        guard let resolved = realpath(fs.temporaryDirectory.path, nil) else {
            throw BeamItClipboardFailure("fixture_root_failed")
        }
        defer { free(resolved) }
        let base = URL(fileURLWithPath: String(cString: resolved))
            .appendingPathComponent("beamit-lifetime-" + UUID().uuidString)
        try fs.createDirectory(at: base, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        defer {
            do { try fs.removeItem(at: base) }
            catch { preconditionFailure("Private fixture cleanup failed.") }
        }
        try await startupAndReplacement(base)
        try await publicationAndRollback(base)
        try await legacySafety(base)
        try await legacyClaimRace(base)
        try await shutdownAdmission(base)
        try await automaticObserverAndLaunchCallback(base)
        try await textOnlyReplacement(base)
        try await nestedPreUnlinkReplacement(base)
        try await interruptedMetadata(base)
        try await ledgerCrashWindows(base)
        try await legacyClaimQuota(base)
        try await receiveAcknowledgementThenCleanupFailure(base)
        try await unlinkedLiveLeaseQuota(base)
        try await renamedMissingLeaseQuota(base)
        try await quotaRootLockContention(base)
        try await promotionCrashWindows(base)
        try await oldDeletedMarkerQuarantine(base)
        try await treeDirectorySyncOrdering(base)
        try await proofBoundaryCancellation(base)
        try await rootLockSubstitution(base)
        try await measurementMutation(base)
        try await measurementRaceQuota(base)
        try await rollbackMetadataPreservation(base)
        try await successfulRetirementAndCrashes(base)
        try await originalLeaseClosesBeforeQuotaUnlock(base)
        try await bindingCorruption(base)
        try await partialFailureAndQuota(base, error: EACCES, name: "permission")
        try await partialFailureAndQuota(base, error: ENOSPC, name: "disk-full")
        try await hostileReplacement(base)
        try await slowCleanupAndTermination(base)
        try await crossProcessAndCrashes(base)
        print("Private-root lifetime/admission/ledger recovery and production receive-completion ACK checks passed. Full control/WSS/native-board receive and AppKit event loop NOT covered. No user clipboard/staging accessed.")
    }
}

private extension ClipboardLifetimeTests.Fault {
    func set(_ code: Int32) { failure = code }
}
