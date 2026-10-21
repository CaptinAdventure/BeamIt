import AppKit
import Observation

enum BeamItApplicationMode: String, CaseIterable {
    case allWindows
    case mainWindow

    var title: String {
        switch self {
        case .allWindows: "All Windows"
        case .mainWindow: "Main Window Only"
        }
    }
}

/// Owns the session's top-level stream windows. Applications connected with "All Windows" also
/// get a stream for each of their main/owned/tool windows, including ones opened later.
@MainActor
@Observable
final class BeamItStreamManager {
    static let shared = BeamItStreamManager()

    private(set) var windows: [String: BeamItStreamWindowController] = [:]
    private(set) var isStarting = false
    @ObservationIgnored var onError: ((String) -> Void)?
    @ObservationIgnored var onAllClosed: (() -> Void)?

    private(set) weak var connection: BeamItConnection?
    /// Processes connected in "All Windows" mode.
    @ObservationIgnored private var followed = Set<Int>()
    /// Windows already streamed or attempted for followed processes; cleared when the host
    /// closes/hides the window, so one the user closed is not reopened automatically.
    @ObservationIgnored private var handled = Set<String>()

    func attach(_ connection: BeamItConnection) {
        reset()
        self.connection = connection
        connection.onStreamStopped = { [weak self] streamID, reason in
            guard let self else { return }
            self.windows.removeValue(forKey: streamID)?.closeAfterHostStop()
            if let reason, !["requested", "source_closed"].contains(reason) {
                self.onError?("Stream stopped: \(reason).")
            }
            self.streamRemoved()
        }
        connection.hostWindows.onChange = { [weak self] window, closed in
            guard let self else { return }
            if closed { self.handled.remove(window.hwnd) }
            if self.followed.contains(window.pid) { self.reconcile() }
        }
    }

    /// Closes all stream windows locally (the connection is being torn down).
    func reset() {
        let controllers = Array(windows.values)
        windows.removeAll()
        followed.removeAll()
        handled.removeAll()
        connection?.hostWindows.onChange = nil
        connection = nil
        for controller in controllers { controller.closeAfterHostStop() }
    }

    /// Starts a stream chosen in the source picker, which is hidden on success.
    func startFromPicker(_ source: BeamItSource, mode: BeamItApplicationMode) {
        guard !isStarting else { return }
        isStarting = true
        Task {
            defer { isStarting = false }
            guard await start(source, activate: true) != nil else { return }
            BeamItMainWindow.hide()
            if case .application(let pid) = source, mode == .allWindows {
                followed.insert(pid)
                reconcile()
            }
        }
    }

    /// Windows of the focused stream's process that are not streamed yet.
    var connectableWindows: [BeamItHostWindow] {
        guard let controller = BeamItScalingMenu.shared.controller, controller.sourceKind != "desktop",
              let pid = controller.stream.offer.source.pid, let connection else { return [] }
        let streamed = Set(windows.values.compactMap(\.stream.offer.source.hwnd))
        return connection.hostWindows.windows.values
            .filter { $0.pid == pid && BeamItHostWindowList.applicationRoles.contains($0.role) && !streamed.contains($0.hwnd) }
            .sorted { $0.menuTitle.localizedStandardCompare($1.menuTitle) == .orderedAscending }
    }

    func connectWindow(_ hwnd: String) {
        Task { await start(.window(hwnd), activate: true) }
    }

    private func reconcile() {
        guard let connection else { return }
        let streamed = Set(windows.values.compactMap(\.stream.offer.source.hwnd))
        for window in BeamItHostWindowList.windowsToFollow(Array(connection.hostWindows.windows.values), followed: followed,
                                          excluded: handled.union(streamed)) {
            handled.insert(window.hwnd)
            Task { await start(.window(window.hwnd), activate: false) }
        }
    }

    private func start(_ source: BeamItSource, activate: Bool) async -> BeamItStreamWindowController? {
        guard let connection else { return nil }
        do {
            let stream = try await connection.startStream(source: source)
            guard self.connection === connection, !stream.isClosed else {
                await stream.stop()
                return nil
            }
            let controller = BeamItStreamWindowController(stream: stream)
            controller.onError = { [weak self] message in self?.onError?(message) }
            let streamID = stream.offer.streamID
            controller.onClose = { [weak self] in
                DispatchQueue.main.async {
                    guard let self, self.windows.removeValue(forKey: streamID) != nil else { return }
                    self.streamRemoved()
                }
            }
            windows[streamID] = controller
            if let hwnd = stream.offer.source.hwnd {
                handled.insert(hwnd)
                if case .window = source { place(controller, hwnd: hwnd) }
            }
            controller.show(activate: activate)
            return controller
        } catch {
            if case .window = source {
                onError?("Could not connect window: \(error.localizedDescription)")
            } else {
                onError?(error.localizedDescription)
            }
            return nil
        }
    }

    /// Positions a secondary window like on the host, relative to another stream of its process.
    private func place(_ controller: BeamItStreamWindowController, hwnd: String) {
        guard let connection, let window = controller.window, let host = connection.hostWindows.window(hwnd),
              let anchor = windows.values.first(where: {
                  $0 !== controller && $0.stream.offer.source.pid == host.pid && $0.window?.isVisible == true
              }),
              let anchorHWND = anchor.stream.offer.source.hwnd,
              let anchorHost = connection.hostWindows.window(anchorHWND),
              let anchorWindow = anchor.window, let screen = anchorWindow.screen else { return }
        let scale = screen.backingScaleFactor
        var frame = window.frame
        frame.origin.x = anchorWindow.frame.minX + (host.rect.x - anchorHost.rect.x) / scale
        frame.origin.y = anchorWindow.frame.maxY - (host.rect.y - anchorHost.rect.y) / scale - frame.height
        let visible = screen.visibleFrame
        frame.origin.x = min(max(frame.minX, visible.minX), max(visible.minX, visible.maxX - frame.width))
        frame.origin.y = min(max(frame.minY, visible.minY), max(visible.minY, visible.maxY - frame.height))
        window.setFrame(frame, display: false)
    }

    private func streamRemoved() {
        let streamedPIDs = Set(windows.values.compactMap(\.stream.offer.source.pid))
        followed.formIntersection(streamedPIDs)
        if windows.isEmpty {
            handled.removeAll()
            onAllClosed?()
        }
    }
}
