import AppKit
import Observation

/// Slowly wanders the host pointer between random points on the most recently focused stream while no stream window
/// has keyboard focus, so the host does not consider the session idle.
@MainActor
@Observable
final class BeamItAntiIdle {
    static let shared = BeamItAntiIdle()
    static let defaultsKey = "autoMoveMouseWhenUnfocused"

    var isEnabled = UserDefaults.standard.bool(forKey: defaultsKey) {
        didSet {
            UserDefaults.standard.set(isEnabled, forKey: Self.defaultsKey)
            update()
        }
    }

    @ObservationIgnored private weak var target: BeamItStreamWindowController?
    @ObservationIgnored private var timer: Timer?
    @ObservationIgnored private var segmentStart: TimeInterval = 0
    @ObservationIgnored private var from = CGPoint.zero
    @ObservationIgnored private var to = CGPoint.zero
    @ObservationIgnored private var generator = SystemRandomNumberGenerator()
    @ObservationIgnored private var observers: [NSObjectProtocol] = []

    private init() {
        let center = NotificationCenter.default
        for name in [NSWindow.didBecomeKeyNotification, NSWindow.didResignKeyNotification,
                     NSApplication.didBecomeActiveNotification, NSApplication.didResignActiveNotification,
                     NSWindow.willCloseNotification] {
            observers.append(center.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                // Key status settles after the notification when focus moves between windows.
                DispatchQueue.main.async { MainActor.assumeIsolated { self?.update() } }
            })
        }
    }

    /// Called when a top-level stream window becomes key.
    func streamDidBecomeKey(_ controller: BeamItStreamWindowController) {
        guard !controller.isPopup else { return }
        target = controller
        update()
    }

    static let segmentDuration: TimeInterval = 5
    /// Wander area diameter as a fraction of the shorter video side.
    nonisolated static let areaFraction = 0.2

    /// Uniformly random point in the unit disk.
    nonisolated static func randomOffset<G: RandomNumberGenerator>(using generator: inout G) -> CGPoint {
        let radius = sqrt(Double.random(in: 0...1, using: &generator))
        let angle = Double.random(in: 0..<(2 * Double.pi), using: &generator)
        return CGPoint(x: radius * cos(angle), y: radius * sin(angle))
    }

    /// Smoothly eased point between two unit-disk offsets at `progress` (0…1).
    nonisolated static func offset(from: CGPoint, to: CGPoint, progress: Double) -> CGPoint {
        let t = min(max(progress, 0), 1)
        let eased = t * t * (3 - 2 * t)
        return CGPoint(x: from.x + (to.x - from.x) * eased, y: from.y + (to.y - from.y) * eased)
    }

    /// Video-pixel position for a unit-disk offset: centred, within a circle whose diameter is
    /// `areaFraction` of the shorter video side.
    nonisolated static func position(offset: CGPoint, width: Int, height: Int) -> CGPoint {
        let radius = Double(min(width, height)) * areaFraction / 2
        return CGPoint(x: Double(width) / 2 + offset.x * radius, y: Double(height) / 2 + offset.y * radius)
    }

    private var streamHasFocus: Bool {
        NSApp.isActive && NSApp.keyWindow?.windowController is BeamItStreamWindowController
    }

    private func update() {
        let shouldRun = isEnabled && target?.window?.isVisible == true && !streamHasFocus
        if shouldRun, timer == nil {
            from = .zero
            to = Self.randomOffset(using: &generator)
            segmentStart = ProcessInfo.processInfo.systemUptime
            let timer = Timer(timeInterval: 1.0 / 30, repeats: true) { [weak self] _ in
                MainActor.assumeIsolated { self?.tick() }
            }
            RunLoop.main.add(timer, forMode: .common)
            self.timer = timer
        } else if !shouldRun, let timer {
            timer.invalidate()
            self.timer = nil
            target?.antiIdleStopped()
        }
    }

    private func tick() {
        guard let target, target.window?.isVisible == true, !streamHasFocus else {
            update()
            return
        }
        let now = ProcessInfo.processInfo.systemUptime
        if now - segmentStart >= Self.segmentDuration {
            from = to
            to = Self.randomOffset(using: &generator)
            segmentStart = now
        }
        target.antiIdleMove(offset: Self.offset(from: from, to: to, progress: (now - segmentStart) / Self.segmentDuration))
    }
}
