import AppKit

// Standalone doubles exercise the production coordinator without a Windows host or visible windows.
enum BeamItSource { case window(String) }
struct TestOffer {
    struct Source {
        let hwnd: String?
        let pid: Int?
    }
    let streamID: String
    let source: Source
}
final class BeamItVideoStream {
    let offer: TestOffer
    var isClosed = false
    init(_ hwnd: String, pid: Int = 42) {
        offer = TestOffer(streamID: UUID().uuidString, source: .init(hwnd: hwnd, pid: pid))
    }
    func stop() async { isClosed = true }
}
final class BeamItConnection {
    var requested: [String] = []
    var streams: [BeamItVideoStream] = []
    var delay = Duration.zero
    func startStream(source: BeamItSource) async throws -> BeamItVideoStream {
        guard case .window(let hwnd) = source else { fatalError() }
        requested.append(hwnd)
        // Deliberately complete even after cancellation, as a host offer may already be in flight.
        try? await Task.sleep(for: delay)
        let stream = BeamItVideoStream(hwnd)
        streams.append(stream)
        return stream
    }
}
final class BeamItStreamWindowController {
    static var created: [BeamItStreamWindowController] = []
    let stream: BeamItVideoStream
    let sourceKind = "application"
    let window: NSWindow?
    var onError: ((String) -> Void)?
    var onClose: (() -> Void)?
    var shown = false
    init(stream: BeamItVideoStream, isPopup: Bool = false) {
        self.stream = stream
        window = NSWindow(contentRect: NSRect(x: 100, y: 200, width: isPopup ? 100 : 800,
                                              height: isPopup ? 80 : 600),
                          styleMask: .borderless, backing: .buffered, defer: false)
        window?.isReleasedWhenClosed = false
        Self.created.append(self)
    }
    func show() { shown = true }
    func close() { stream.isClosed = true; onClose?() }
    func closeAfterHostStop() { close() }
}

@main
struct PopupTests {
    static func main() async throws {
        _ = NSApplication.shared
        let connection = BeamItConnection()
        let coordinator = BeamItPopups(connection: connection)
        defer {
            coordinator.stop()
            for controller in BeamItStreamWindowController.created { controller.window?.close() }
        }
        let parent = BeamItStreamWindowController(stream: BeamItVideoStream("main"))
        func host(_ hwnd: String, role: String = "popup", owner: String? = nil,
                  pid: Int = 42, x: Double = 300, y: Double = 300) -> BeamItHostWindow {
            BeamItHostWindow(hwnd: hwnd, pid: pid, role: role, owner: owner,
                             rect: .init(x: x, y: y, width: role == "main" ? 1600 : 200,
                                         height: role == "main" ? 1200 : 160),
                             state: "normal")
        }
        func settle() async throws { try await Task.sleep(for: .milliseconds(40)) }
        coordinator.update(host("main", role: "main", x: 100, y: 100), closed: false)
        coordinator.register(parent)
        coordinator.update(host("unrelated", pid: 99), closed: false)
        coordinator.update(host("dialog", role: "owned", owner: "main"), closed: false)
        coordinator.update(host("menu", owner: "main"), closed: false)
        try await settle()
        precondition(connection.requested == ["menu"], "only related popup roles may stream")
        let menu = BeamItStreamWindowController.created.last!
        precondition(menu.shown && menu.window?.parent === parent.window)
        precondition(menu.window?.frame.origin == NSPoint(x: 200, y: 620), "Retina offsets invert Windows Y")
        coordinator.update(host("menu", owner: "main", x: 500, y: 500), closed: false)
        precondition(menu.window?.frame.origin == NSPoint(x: 300, y: 520))
        precondition(connection.requested.count == 1, "changed events must not duplicate streams")
        coordinator.update(host("submenu", owner: "menu"), closed: false)
        try await settle()
        precondition(connection.requested.contains("submenu"), "nested ownership must resolve")
        coordinator.update(host("menu"), closed: true)
        precondition(menu.stream.isClosed)
        coordinator.update(host("menu", owner: "main"), closed: false)
        try await settle()
        precondition(connection.requested.filter { $0 == "menu" }.count == 2, "reopened handles may stream again")
        coordinator.update(host("unowned"), closed: false)
        try await settle()
        precondition(connection.requested.contains("unowned"), "single-process fallback must support Chromium")

        connection.delay = .milliseconds(150)
        coordinator.update(host("short-lived", owner: "main"), closed: false)
        try await settle()
        coordinator.update(host("short-lived"), closed: true)
        try await settle()
        precondition(connection.streams.filter { $0.offer.source.hwnd == "short-lived" }.allSatisfy(\.isClosed),
                     "late offers must be stopped rather than displayed")
        coordinator.unregister(parent)
        precondition(connection.streams.allSatisfy(\.isClosed), "closing parent must close popup streams")
        coordinator.register(parent)
        let other = BeamItStreamWindowController(stream: BeamItVideoStream("other"))
        coordinator.update(host("other", role: "main"), closed: false)
        coordinator.register(other)
        let requests = connection.requested.count
        coordinator.update(host("ambiguous"), closed: false)
        try await settle()
        precondition(connection.requested.count == requests, "ambiguous unowned popups must not be guessed")
        coordinator.stop()
        print("Popup ownership, placement and lifecycle checks passed.")
    }
}
