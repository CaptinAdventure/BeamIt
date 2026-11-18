import AppKit
import Observation
import Darwin

// App lifetime, not connection lifetime. All decision/publication state stays
// on MainActor; descriptors, hashing and deletion stay on the storage actor.
@MainActor @Observable
final class BeamItClipboardLifetime {
    typealias Record = BeamItClipboardStorage.CommittedRecord
    enum WriteOutcome { case success(Int), restored(Int), uncertain(Int) }
    nonisolated enum LegacyEligibility: Sendable, Equatable { case alone, anotherInstance, unavailable }
    static let shared = BeamItClipboardLifetime(
        storage: BeamItClipboardStorage(),
        changeCount: { NSPasteboard.general.changeCount },
        legacyEligibility: {
            await Task.detached(priority: .utility) { enumerateInstances() }.value
        })

    let storage: BeamItClipboardStorage
    private(set) var diagnostic: String?
    private struct Entry {
        let record: Record
        var count: Int?
        var eligible = false
        var uncertain = false
    }
    private struct Work {
        let cancel: @MainActor () -> Void
        let busy: @MainActor () -> Bool
    }
    @ObservationIgnored private var records: [String: Entry] = [:]
    @ObservationIgnored private var work: [ObjectIdentifier: Work] = [:]
    @ObservationIgnored private var currentID: String?
    @ObservationIgnored private var startupTask: Task<Void, Never>?
    @ObservationIgnored private var observerTask: Task<Void, Never>?
    @ObservationIgnored private var cleanupTask: Task<Void, Never>?
    @ObservationIgnored private var publishing = false
    @ObservationIgnored private var accepting = true
    @ObservationIgnored private var shuttingDown = false
    @ObservationIgnored private var lastObserved: Int?
    @ObservationIgnored private let changeCount: @MainActor () -> Int
    @ObservationIgnored private let legacyEligibility: @Sendable () async -> LegacyEligibility
    @ObservationIgnored private let sleep: @Sendable (Duration) async throws -> Void
    @ObservationIgnored private let uptime: @MainActor () -> TimeInterval

    // Test callers must supply an explicit private storage and fake board.
    // Initialization itself performs no disk/clipboard access.
    init(storage: BeamItClipboardStorage, changeCount: @escaping @MainActor () -> Int,
         legacyEligibility: @escaping @Sendable () async -> LegacyEligibility,
         sleep: @escaping @Sendable (Duration) async throws -> Void = { try await Task.sleep(for: $0) },
         uptime: @escaping @MainActor () -> TimeInterval = { ProcessInfo.processInfo.systemUptime }) {
        self.storage = storage; self.changeCount = changeCount
        self.legacyEligibility = legacyEligibility; self.sleep = sleep; self.uptime = uptime
    }

    func start(observeAutomatically: Bool = true) {
        guard startupTask == nil, accepting else { return }
        startupTask = Task(priority: .utility) {
            defer { startupFinished = true }
            await storage.setRecoveryReporter { [weak self] in self?.reportFailure() }
            let eligibility = await legacyEligibility()
            guard !Task.isCancelled else { return }
            do { try await storage.recover(legacyAllowed: eligibility == .alone) }
            catch { reportFailure() }
            guard !Task.isCancelled else { return }
            let report = await storage.cleanupStartup(legacyAllowed: eligibility == .alone)
            if !report.problems.isEmpty { reportFailure() }
            guard !Task.isCancelled else { return }
            do {
                let retained = try await storage.ownedCommittedRecords()
                let baseline = changeCount()
                lastObserved = baseline
                for record in retained {
                    // A failed startup deletion already owns an exact claim.
                    // This count is a retry signal, not clipboard ownership or
                    // evidence that a consumer has completed.
                    records[record.id] = Entry(record: record, count: baseline)
                }
            } catch { reportFailure() }
            guard observeAutomatically, accepting else { return }
            observerTask = Task {
                while !Task.isCancelled {
                    do { try await sleep(.milliseconds(80)) } catch { return }
                    observe()
                }
            }
        }
    }
    func ready() async throws {
        guard accepting else { throw CancellationError() }
        guard let startupTask else { throw BeamItClipboardFailure("staging_not_started") }
        // Poll the bounded state rather than awaiting an uninterruptible child:
        // cancellation of the calling transfer remains effective.
        let deadline = uptime() + 30
        while !startupFinished {
            try Task.checkCancellation()
            guard accepting, uptime() < deadline else { throw BeamItClipboardFailure("staging_busy") }
            try await sleep(.milliseconds(10))
        }
        withExtendedLifetime(startupTask) {}
    }
    @ObservationIgnored private var startupFinished = false

    func registerWork(_ owner: AnyObject, cancel: @escaping @MainActor () -> Void,
                      busy: @escaping @MainActor () -> Bool) {
        guard accepting else { cancel(); return }
        work[ObjectIdentifier(owner)] = Work(cancel: cancel, busy: busy)
    }
    func unregisterWork(_ owner: AnyObject) { work.removeValue(forKey: ObjectIdentifier(owner)) }

    // The monitor reads only the sequence signal, never board types/data/URLs.
    // Publication's intermediate clear/write/rollback counts are protected.
    func observe() {
        guard accepting, !publishing, startupFinished else { return }
        let count = changeCount()
        guard count != lastObserved else { return }
        lastObserved = count
        for id in Array(records.keys) {
            guard var entry = records[id], !entry.uncertain, let own = entry.count, count != own else { continue }
            entry.eligible = true
            records[id] = entry
        }
        scheduleCleanup()
    }
    private func scheduleCleanup() {
        guard !publishing, cleanupTask == nil, accepting,
              records.values.contains(where: { $0.eligible && !$0.uncertain }) else { return }
        cleanupTask = Task(priority: .utility) {
            let eligible = records.values.filter { $0.eligible && !$0.uncertain }.map(\.record)
            for record in eligible {
                guard !Task.isCancelled, !publishing else { break }
                await retire(record)
            }
            cleanupTask = nil
        }
    }
    private func retire(_ record: Record) async {
        do {
            if try await storage.cleanupCommitted(record) {
                records.removeValue(forKey: record.id)
                if currentID == record.id { currentID = nil }
            } else { records[record.id]?.eligible = false; reportFailure() }
        } catch { records[record.id]?.eligible = false; reportFailure() }
    }
    // A writer may throw only before mutating the board. After a native write
    // begins it returns success/restored/uncertain, including rollback outcome.
    func publish(_ record: Record?, write: @MainActor () throws -> WriteOutcome) async throws -> WriteOutcome {
        try await ready()
        let deadline = uptime() + 30
        while publishing || cleanupTask != nil {
            try Task.checkCancellation()
            guard accepting, uptime() < deadline else { throw BeamItClipboardFailure("staging_busy") }
            try await sleep(.milliseconds(10))
        }
        try Task.checkCancellation()
        guard accepting else { throw CancellationError() }
        publishing = true
        let previous = currentID
        // Storage already holds the committed lease. This exact record is
        // registered before write can expose even the first native file URL.
        if let record { records[record.id] = Entry(record: record, count: nil) }
        defer { publishing = false; scheduleCleanup() }
        let outcome: WriteOutcome
        do { outcome = try write() }
        catch {
            if let record { records.removeValue(forKey: record.id) }
            throw error // No native write began; caller disposes unpublished work.
        }
        switch outcome {
        case .success(let count):
            lastObserved = count
            for id in Array(records.keys) where id != record?.id {
                if records[id]?.uncertain == false { records[id]?.eligible = true }
            }
            if let record { records[record.id] = Entry(record: record, count: count) }
            currentID = record?.id
        case .restored(let count):
            lastObserved = count
            if let record { records.removeValue(forKey: record.id) }
            if let previous, records[previous]?.uncertain == false {
                records[previous]?.count = count
                records[previous]?.eligible = false
            }
            currentID = previous
        case .uncertain(let count):
            lastObserved = count
            for id in [previous, record?.id].compactMap({ $0 }) {
                records[id]?.uncertain = true
                records[id]?.eligible = false
            }
            reportFailure()
        }
        return outcome
    }
    // Read-only lifecycle test seams; no native/file operations are bypassed.
    var isPublishing: Bool { publishing }
    var recoveryFinished: Bool { startupFinished }
    var acceptsTransfers: Bool { accepting }
    var isCleaning: Bool { cleanupTask != nil }
    var registeredIDs: Set<String> { Set(records.keys) }
    func clipboardSequence() -> Int { changeCount() }
    func waitForCleanup() async throws {
        let deadline = uptime() + 30
        while cleanupTask != nil {
            try Task.checkCancellation()
            guard uptime() < deadline else { throw BeamItClipboardFailure("staging_busy") }
            try await sleep(.milliseconds(10))
        }
    }
    func reportFailure() {
        diagnostic = "Clipboard staging cleanup incomplete. Residual files remain quota-accounted; see the macOS staging recovery instructions."
        NSLog("BeamIt clipboard staging cleanup incomplete; residual data remains quota-accounted.")
    }
    // The application delegate awaits this bounded controller, not an
    // unawaited willTerminate task. Expiry cancels but does not join blocked I/O.
    func shutdown() async {
        guard !shuttingDown else { return }
        shuttingDown = true; accepting = false
        observerTask?.cancel(); startupTask?.cancel()
        for item in work.values { item.cancel() }
        let deadline = uptime() + 2
        while publishing || !startupFinished || work.values.contains(where: { $0.busy() }) || cleanupTask != nil {
            if uptime() >= deadline {
                cleanupTask?.cancel()
                reportFailure()
                return
            }
            do { try await sleep(.milliseconds(10)) } catch { reportFailure(); return }
        }
        cleanupTask = Task(priority: .utility) {
            do {
                // Include quiesced promotion/rollback failures which never
                // reached a successful clipboard record.
                let owned = try await storage.ownedCommittedRecords()
                for record in owned {
                    if Task.isCancelled { break }
                    await retire(record)
                }
            } catch {
                reportFailure()
            }
            cleanupTask = nil
        }
        while cleanupTask != nil && uptime() < deadline {
            do { try await sleep(.milliseconds(10)) } catch { break }
        }
        if cleanupTask != nil { cleanupTask?.cancel(); reportFailure() }
    }
    // proc_name failures are uncertainty, not permission to remove legacy
    // directories. A pre-revision binary starting after this snapshot cannot
    // be excluded atomically; the user documentation states that limitation.
    private nonisolated static func enumerateInstances() -> LegacyEligibility {
        let count = proc_listallpids(nil, 0)
        guard count > 0, count < 1_000_000 else { return .unavailable }
        var pids = [pid_t](repeating: 0, count: Int(count) + 128)
        let actual = pids.withUnsafeMutableBytes { proc_listallpids($0.baseAddress, Int32($0.count)) }
        guard actual > 0, Int(actual) < pids.count else { return .unavailable }
        let ownName = ProcessInfo.processInfo.processName
        for pid in pids.prefix(Int(actual)) where pid > 0 && pid != getpid() {
            var name = [CChar](repeating: 0, count: 1024)
            errno = 0
            let length = name.withUnsafeMutableBytes { proc_name(pid, $0.baseAddress, UInt32($0.count)) }
            if length <= 0 {
                if errno == ESRCH { continue }
                return .unavailable
            }
            let text = name.withUnsafeBufferPointer { String(validatingCString: $0.baseAddress!) }
            guard let text else { return .unavailable }
            if text == ownName || text == "BeamIt" { return .anotherInstance }
        }
        return .alone
    }
}
