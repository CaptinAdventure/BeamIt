import AppKit
import SwiftUI

@MainActor
enum BeamItMainWindow {
    static weak var window: NSWindow?

    static func show() {
        guard let window else {
            NSSound.beep()
            return
        }
        window.makeKeyAndOrderFront(nil)
        NSApp.activate()
    }

    static func hide() {
        window?.orderOut(nil)
    }
}

struct BeamItMainWindowAccessor: NSViewRepresentable {
    func makeNSView(context: Context) -> WindowTrackingView {
        WindowTrackingView()
    }

    func updateNSView(_ nsView: WindowTrackingView, context: Context) {}
}

final class WindowTrackingView: NSView {
    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        let attachedWindow = window
        DispatchQueue.main.async {
            if let attachedWindow {
                BeamItMainWindow.window = attachedWindow
            }
        }
    }
}
