import AppKit
import CoreVideo

// Standalone doubles let the production controller exercise restoration without a host or
// switching the user's desktop into a fullscreen Space.
final class BeamItVideoStream {
    let offer: BeamItStreamOffer
    var diagnostics = 0
    var popupCoordinator: TestPopups?
    var attentionWindows = Set<String>()
    var onFrame: ((CVPixelBuffer, UInt64) -> Void)?
    var onVideoConfig: ((BeamItVideo) -> Void)?
    var onError: ((Error) -> Void)?
    var onInputError: ((String) -> Void)?
    var onAttention: (() -> Void)?
    var isClosed = false
    var video: BeamItVideo { offer.video }

    init() throws {
        let json = """
        {"stream_id":"test","url":"wss://localhost/test","token_expires_in_ms":10000,
         "source":{"kind":"desktop"},"video":{"generation":1,"codec":"h264","width":640,"height":360}}
        """
        offer = try JSONDecoder().decode(BeamItStreamOffer.self, from: Data(json.utf8))
    }
    func windowIcon() async throws -> NSImage? { nil }
    func handleStopped() {}
    func stop() async {}
}

final class TestPopups {
    func register(_ controller: BeamItStreamWindowController) {}
    func unregister(_ controller: BeamItStreamWindowController) {}
    func reposition() {}
}

final class BeamItInputHandler {
    var onMouseCaptureChanged: ((Bool) -> Void)?
    init(stream: BeamItVideoStream) {}
    func captureMouse(in view: NSView) {}
    func releaseMouseCapture() {}
    func releaseAll() {}
    func handleKeyEvent(_ event: NSEvent) -> Bool { true }
    func sendSyntheticPosition(_ point: CGPoint) {}
    func forgetPosition() {}
}

final class BeamItFrameView: NSView {
    var diagnostics = 0
    var inputHandler: BeamItInputHandler?
    func display(_ pixelBuffer: CVPixelBuffer, captureUS: UInt64) {}
}

final class BeamItPopupPanel: NSPanel {}

@main
struct FullscreenTests {
    @MainActor
    static func main() async throws {
        _ = NSApplication.shared
        for allowsScaling in [false, true] {
            let controller = BeamItStreamWindowController(stream: try BeamItVideoStream())
            let window = controller.window!
            controller.setAllowsScaling(allowsScaling)
            if allowsScaling { controller.setWindowScale(2) }
            let originalSize = window.contentRect(forFrameRect: window.frame).size
            controller.windowWillEnterFullScreen(Notification(name: NSWindow.willEnterFullScreenNotification))
            window.styleMask = .borderless
            window.setFrame(NSRect(x: 0, y: 0, width: 1280, height: 800), display: false)
            controller.windowDidResize(Notification(name: NSWindow.didResizeNotification))
            controller.windowDidExitFullScreen(Notification(name: NSWindow.didExitFullScreenNotification))
            // AppKit's final restoration can change the frame after the delegate callback.
            window.setFrame(NSRect(x: 0, y: 0, width: 1000, height: 700), display: false)
            controller.windowDidResize(Notification(name: NSWindow.didResizeNotification))
            try await Task.sleep(for: .milliseconds(50))
            precondition(window.styleMask.contains(.titled), "exit must restore the title bar")
            precondition(window.styleMask.contains(.closable), "exit must restore the close control")
            precondition(window.styleMask.contains(.miniaturizable))
            precondition(window.styleMask.contains(.resizable) == allowsScaling)
            precondition(window.contentRect(forFrameRect: window.frame).size == originalSize,
                         "transition resize events must not replace the saved windowed scale")
            controller.close()
        }
        print("Fullscreen window restoration checks passed")
    }
}
