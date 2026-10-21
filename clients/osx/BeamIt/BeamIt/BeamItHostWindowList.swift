import Foundation
import Observation

/// Live set of host top-level windows (PROTOCOL.md §5.12).
/// Observers are only notified when listing fields change, not on every move/resize.
@MainActor
@Observable
final class BeamItHostWindowList {
    /// Roles connected by "All Windows"; transient `popup` windows are handled by BeamItPopups.
    nonisolated static let applicationRoles: Set<String> = ["main", "owned", "tool"]

    @ObservationIgnored private var storage: [String: BeamItHostWindow] = [:]
    private var revision = 0
    @ObservationIgnored var onChange: ((BeamItHostWindow, Bool) -> Void)?

    var windows: [String: BeamItHostWindow] {
        _ = revision
        return storage
    }

    /// Current state without registering an observation dependency (for placement).
    func window(_ hwnd: String) -> BeamItHostWindow? { storage[hwnd] }

    func update(_ window: BeamItHostWindow, closed: Bool) {
        if closed {
            if storage.removeValue(forKey: window.hwnd) != nil { revision += 1 }
        } else {
            let old = storage.updateValue(window, forKey: window.hwnd)
            if old.map({ !Self.sameListing($0, window) }) ?? true { revision += 1 }
        }
        onChange?(window, closed)
    }

    /// Groups capturable (non-popup) windows by process, main windows first.
    var applications: [BeamItApplication] {
        Dictionary(grouping: windows.values.filter { $0.role != "popup" }, by: \.pid)
            .map { pid, appWindows in
                let sorted = appWindows.sorted {
                    if ($0.role == "main") != ($1.role == "main") { return $0.role == "main" }
                    return ($0.title ?? "").localizedStandardCompare($1.title ?? "") == .orderedAscending
                }
                let imageName = sorted.lazy.compactMap(\.image_name).first { !$0.isEmpty } ?? ""
                let title = sorted.lazy.compactMap(\.title).first { !$0.isEmpty }
                    ?? (imageName.isEmpty ? "Application" : imageName)
                return BeamItApplication(pid: pid, title: title, imageName: imageName, windows: sorted)
            }
            .sorted { $0.title.localizedStandardCompare($1.title) == .orderedAscending }
    }

    private static func sameListing(_ a: BeamItHostWindow, _ b: BeamItHostWindow) -> Bool {
        a.pid == b.pid && a.role == b.role && a.state == b.state && a.title == b.title
            && a.image_name == b.image_name && a.owner == b.owner
    }

    /// Windows of followed processes that should get a stream now. Minimized windows wait until
    /// restored, because starting a stream would restore and activate them on the host.
    nonisolated static func windowsToFollow(_ windows: [BeamItHostWindow], followed: Set<Int>,
                                            excluded: Set<String>) -> [BeamItHostWindow] {
        windows.filter {
            followed.contains($0.pid) && applicationRoles.contains($0.role)
                && $0.state != "minimized" && !excluded.contains($0.hwnd)
        }
    }
}
