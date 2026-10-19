import Foundation

final class BeamItClockEstimate {
    struct Sample {
        let offsetUS: Double
        let rttUS: Double
        let received: TimeInterval
    }
    private let lock = NSLock()
    private var samples: [Sample] = []

    func record(hostUS: UInt64, sent: TimeInterval, received: TimeInterval) {
        guard received >= sent else { return }
        let sample = Sample(
            offsetUS: Double(hostUS) - (sent + received) * 500_000,
            rttUS: (received - sent) * 1_000_000, received: received
        )
        lock.lock()
        defer { lock.unlock() }
        samples.removeAll { received - $0.received > 30 }
        samples.append(sample)
    }

    func best(now: TimeInterval = ProcessInfo.processInfo.systemUptime) -> Sample? {
        lock.lock()
        defer { lock.unlock() }
        return samples.filter { now >= $0.received && now - $0.received <= 30 }
            .min { $0.rttUS < $1.rttUS }
    }
}
