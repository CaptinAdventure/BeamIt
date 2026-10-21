import AppKit

final class BeamItPopups {
    private struct Parent {
        weak var controller: BeamItStreamWindowController?
    }
    private struct Opening {
        let token: UUID
        let parent: String
        let task: Task<Void, Never>
    }
    private weak var connection: BeamItConnection?
    private var windows: [String: BeamItHostWindow] = [:]
    private var parents: [String: Parent] = [:]
    private var opening: [String: Opening] = [:]
    private var popups: [String: BeamItStreamWindowController] = [:]
    private var popupParents: [String: String] = [:]
    private var attempted = Set<String>()
    private var stopped = false

    init(connection: BeamItConnection) { self.connection = connection }

    func register(_ controller: BeamItStreamWindowController) {
        guard controller.sourceKind != "desktop", let hwnd = controller.stream.offer.source.hwnd else { return }
        parents[hwnd] = Parent(controller: controller)
        reconcile()
    }

    func unregister(_ controller: BeamItStreamWindowController) {
        guard let hwnd = controller.stream.offer.source.hwnd,
              parents[hwnd]?.controller === controller else { return }
        parents.removeValue(forKey: hwnd)
        for (popup, entry) in Array(opening) where entry.parent == hwnd { remove(popup) }
        for (popup, parent) in Array(popupParents) where parent == hwnd { remove(popup) }
    }

    func stop() {
        stopped = true
        for hwnd in Array(opening.keys) { remove(hwnd) }
        for hwnd in Array(popups.keys) { remove(hwnd) }
        parents.removeAll()
        windows.removeAll()
    }

    func update(_ window: BeamItHostWindow, closed: Bool) {
        guard !stopped else { return }
        if closed {
            windows.removeValue(forKey: window.hwnd)
            remove(window.hwnd)
            attempted.remove(window.hwnd)
        } else {
            windows[window.hwnd] = window
            if window.role != "popup" || window.state == "minimized" { remove(window.hwnd) }
        }
        reconcile()
    }

    func streamStopped(_ id: String) {
        guard let hwnd = popups.first(where: { $0.value.stream.offer.streamID == id })?.key else { return }
        let controller = popups.removeValue(forKey: hwnd)
        popupParents.removeValue(forKey: hwnd)
        controller?.closeAfterHostStop()
    }

    func reposition() {
        for (hwnd, controller) in popups {
            guard let popup = windows[hwnd], let parentHWND = popupParents[hwnd],
                  let hostParent = windows[parentHWND],
                  let parent = parents[parentHWND]?.controller else { continue }
            position(controller, popup: popup, parent: parent, hostParent: hostParent)
        }
    }

    private func parent(for popup: BeamItHostWindow) -> String? {
        // Prefer an explicit ownership chain, including nested context menus.
        var owner = popup.owner
        var visited = Set<String>()
        while let hwnd = owner, visited.insert(hwnd).inserted {
            if let controller = parents[hwnd]?.controller,
               controller.stream.offer.source.pid == popup.pid { return hwnd }
            owner = windows[hwnd]?.owner
        }
        // Some Chromium popups are unowned. Avoid guessing between multiple streams of one process.
        let matches = parents.filter { $0.value.controller?.stream.offer.source.pid == popup.pid }
        if matches.count == 1 { return matches.first?.key }
        return matches.first(where: { $0.value.controller?.window?.isKeyWindow == true })?.key
    }

    private func reconcile() {
        guard !stopped, let connection else { return }
        reposition()
        for popup in windows.values where popup.role == "popup" && popup.state != "minimized" {
            guard popups[popup.hwnd] == nil, opening[popup.hwnd] == nil, !attempted.contains(popup.hwnd),
                  let parentHWND = parent(for: popup),
                  let parent = parents[parentHWND]?.controller, windows[parentHWND] != nil else { continue }
            attempted.insert(popup.hwnd)
            let token = UUID()
            let task = Task { [weak self, weak parent] in
                guard let self, let parent else { return }
                do {
                    let stream = try await connection.startStream(source: .window(popup.hwnd))
                    guard !Task.isCancelled, !self.stopped,
                          !stream.isClosed,
                          self.opening[popup.hwnd]?.token == token,
                          let current = self.windows[popup.hwnd],
                          let hostParent = self.windows[parentHWND],
                          self.parents[parentHWND]?.controller === parent else {
                        await stream.stop()
                        return
                    }
                    self.opening.removeValue(forKey: popup.hwnd)
                    let controller = BeamItStreamWindowController(stream: stream, isPopup: true)
                    controller.onError = { [weak parent] message in parent?.onError?("Popup: \(message)") }
                    controller.onClose = { [weak self] in
                        self?.popups.removeValue(forKey: popup.hwnd)
                        self?.popupParents.removeValue(forKey: popup.hwnd)
                    }
                    self.popups[popup.hwnd] = controller
                    self.popupParents[popup.hwnd] = parentHWND
                    self.position(controller, popup: current, parent: parent, hostParent: hostParent)
                    if let child = controller.window {
                        parent.window?.addChildWindow(child, ordered: .above)
                    }
                    controller.show()
                } catch {
                    guard self.opening[popup.hwnd]?.token == token else { return }
                    self.opening.removeValue(forKey: popup.hwnd)
                    guard !Task.isCancelled else { return }
                    NSLog("BeamIt popup stream failed: %@", error.localizedDescription)
                    parent.onError?("Could not show a remote popup: \(error.localizedDescription)")
                }
            }
            opening[popup.hwnd] = Opening(token: token, parent: parentHWND, task: task)
        }
    }

    private func position(_ controller: BeamItStreamWindowController, popup: BeamItHostWindow,
                          parent: BeamItStreamWindowController, hostParent: BeamItHostWindow) {
        guard let view = parent.window?.contentView, let window = parent.window,
              let child = controller.window, hostParent.rect.width > 0, hostParent.rect.height > 0 else { return }
        // The video surface is inset when fullscreen needs letterboxing.
        let videoView = view.subviews.first ?? view
        let content = window.convertToScreen(videoView.convert(videoView.bounds, to: nil))
        let sx = content.width / hostParent.rect.width
        let sy = content.height / hostParent.rect.height
        child.setFrame(NSRect(
            x: content.minX + (popup.rect.x - hostParent.rect.x) * sx,
            y: content.maxY - (popup.rect.y - hostParent.rect.y + popup.rect.height) * sy,
            width: popup.rect.width * sx,
            height: popup.rect.height * sy
        ), display: true)
    }

    private func remove(_ hwnd: String) {
        opening.removeValue(forKey: hwnd)?.task.cancel()
        popupParents.removeValue(forKey: hwnd)
        if let controller = popups.removeValue(forKey: hwnd) {
            if let window = controller.window {
                window.parent?.removeChildWindow(window)
            }
            if stopped {
                controller.closeAfterHostStop()
            } else {
                controller.close()
            }
        }
    }
}

final class BeamItPopupPanel: NSPanel {
    override var canBecomeKey: Bool { false }
    override var canBecomeMain: Bool { false }
}
