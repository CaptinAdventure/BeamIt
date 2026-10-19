import AppKit

final class BeamItStreamWindowController: NSWindowController, NSWindowDelegate {
    var infoWindow: NSWindow?
    var parametersWindow: NSWindow?
    let stream: BeamItVideoStream
    let sourceKind: String
    var onClose: (() -> Void)?
    var onError: ((String) -> Void)?
    private var isClosingFromHost = false
    private let frameView: BeamItFrameView
    private let inputHandler: BeamItInputHandler
    private var keyMonitor: Any?
    private var videoPixelSize: NSSize
    private var isShowingInputError = false
    private var attentionRequest: Int?
    private var iconTask: Task<Void, Never>?
    let isPopup: Bool
    /// The most recently focused stream window, the target of "Send Text to Target…".
    private(set) static weak var activeController: BeamItStreamWindowController? {
        didSet {
            BeamItScalingMenu.shared.controller = activeController
        }
    }
    private(set) var allowsScaling = false
    private var windowScale: CGFloat = 1
    private var isApplyingPixelSize = false
    private var isFullscreenTransition = false
    private var resetScaleAfterFullscreen = false
    private var lastEscapeTimestamp: TimeInterval?
    private var isClosing = false
    var typingTask: Task<Void, Never>?

    var isTypingText: Bool { typingTask != nil }

    func setAllowsScaling(_ allowed: Bool) {
        guard !isPopup, let window else { return }
        allowsScaling = allowed
        if window.styleMask.contains(.fullScreen) || isFullscreenTransition {
            if !allowed {
                resetScaleAfterFullscreen = true
                if !isFullscreenTransition { toggleFullscreen() }
            }
            BeamItScalingMenu.shared.controller = self
            return
        }
        if allowed {
            window.styleMask.insert(.resizable)
        } else {
            window.styleMask.remove(.resizable)
            windowScale = 1
        }
        applyPixelSize()
        BeamItScalingMenu.shared.controller = self
        stream.popupCoordinator?.reposition()
    }

    func setWindowScale(_ scale: CGFloat) {
        guard allowsScaling, !isFullscreenTransition else { return }
        windowScale = scale
        if let window, window.styleMask.contains(.fullScreen) {
            toggleFullscreen()
            return
        }
        applyPixelSize()
        stream.popupCoordinator?.reposition()
    }

    func toggleFullscreen() {
        guard !isPopup, !isFullscreenTransition, let window, window.attachedSheet == nil else { return }
        isFullscreenTransition = true
        if !window.styleMask.contains(.fullScreen) {
            window.styleMask.insert(.resizable)
        }
        window.toggleFullScreen(nil)
    }

    private func handleFullscreenEscape(_ event: NSEvent) -> Bool {
        guard window?.styleMask.contains(.fullScreen) == true,
              event.type == .keyDown, event.keyCode == 53, !event.isARepeat else { return false }
        let previous = lastEscapeTimestamp
        lastEscapeTimestamp = event.timestamp
        guard let previous, event.timestamp >= previous, event.timestamp - previous <= 1 else { return false }
        lastEscapeTimestamp = nil
        resetScaleAfterFullscreen = true
        inputHandler.releaseMouseCapture()
        inputHandler.releaseAll()
        if !isFullscreenTransition { toggleFullscreen() }
        return true
    }

    static func captureMouseForActiveStream() {
        guard let controller = activeController, let window = controller.window,
              window.isKeyWindow, window.attachedSheet == nil else {
            NSSound.beep()
            return
        }
        controller.stopTypingText()
        controller.inputHandler.captureMouse(in: controller.frameView)
    }

    func stopTypingText() {
        typingTask?.cancel()
        typingTask = nil
    }

    var title: String {
        if let title = stream.offer.source.title, !title.isEmpty {
            return title
        }
        if stream.offer.source.kind == "desktop" {
            return "Desktop · \((stream.offer.source.capture ?? "").uppercased())"
        }
        return "Stream \(stream.offer.streamID)"
    }

    init(stream: BeamItVideoStream, isPopup: Bool = false) {
        self.isPopup = isPopup
        self.stream = stream
        sourceKind = stream.offer.source.kind
        videoPixelSize = NSSize(width: stream.offer.video.width, height: stream.offer.video.height)
        let scale = NSScreen.main?.backingScaleFactor ?? 1
        let pointSize = NSSize(width: videoPixelSize.width / scale, height: videoPixelSize.height / scale)
        frameView = BeamItFrameView(frame: NSRect(origin: .zero, size: pointSize))
        frameView.diagnostics = stream.diagnostics
        inputHandler = BeamItInputHandler(stream: stream)
        frameView.inputHandler = inputHandler
        let window: NSWindow
        if isPopup {
            let panel = BeamItPopupPanel(
                contentRect: frameView.frame, styleMask: [.borderless, .nonactivatingPanel],
                backing: .buffered, defer: false
            )
            panel.hidesOnDeactivate = true
            panel.becomesKeyOnlyIfNeeded = true
            window = panel
        } else {
            window = NSWindow(
                contentRect: frameView.frame, styleMask: [.titled, .closable, .miniaturizable],
                backing: .buffered, defer: false
            )
        }
        window.isReleasedWhenClosed = false
        window.isOpaque = true
        window.backgroundColor = .black
        let surface = BeamItStreamSurface(frame: frameView.frame)
        surface.videoSize = videoPixelSize
        surface.addSubview(frameView)
        window.contentView = surface
        if !isPopup { window.collectionBehavior.insert(.fullScreenPrimary) }
        window.center()
        super.init(window: window)
        window.title = title
        inputHandler.onMouseCaptureChanged = { [weak self] captured in
            guard let self else { return }
            self.lastEscapeTimestamp = nil
            self.window?.title = captured
                ? "\(self.title) — Control+Option+G to release mouse"
                : self.title
        }
        window.delegate = self
        window.initialFirstResponder = frameView
        // Key events are taken from a local monitor because AppKit never delivers keyUp while ⌘ is
        // held, and the main menu would otherwise consume ⌘-shortcuts meant for the host.
        keyMonitor = NSEvent.addLocalMonitorForEvents(matching: [.keyDown, .keyUp, .flagsChanged]) { [weak self] event in
            guard let self, let window = self.window, event.window === window, window.isKeyWindow,
                  window.attachedSheet == nil else {
                return event
            }
            if self.handleFullscreenEscape(event) { return nil }
            return self.inputHandler.handleKeyEvent(event) ? nil : event
        }
        applyPixelSize()
        stream.onFrame = { [weak self] pixelBuffer, captureUS in
            self?.frameView.display(pixelBuffer, captureUS: captureUS)
        }
        stream.onVideoConfig = { [weak self] video in
            DispatchQueue.main.async {
                guard let self else { return }
                self.videoPixelSize = NSSize(width: video.width, height: video.height)
                self.applyPixelSize()
                self.stream.popupCoordinator?.reposition()
            }
        }
        stream.onError = { [weak self] error in
            DispatchQueue.main.async {
                self?.onError?(error.localizedDescription)
                self?.closeAfterHostStop()
            }
            NSLog("BeamIt stream error: %@", error.localizedDescription)
        }
        stream.onInputError = { [weak self] message in
            self?.handleInputError(message)
        }
        stream.onAttention = { [weak self] in
            self?.updateDockAttention()
        }
        iconTask = Task { [weak self, stream] in
            guard !isPopup else { return }
            do {
                guard let image = try await stream.windowIcon() else { return }
                try Task.checkCancellation()
                guard let self, let window = self.window else { return }
                window.miniwindowImage = image
                let accessory = NSTitlebarAccessoryViewController()
                accessory.layoutAttribute = .left
                let container = NSView(frame: NSRect(x: 0, y: 0, width: 24, height: 22))
                let icon = NSImageView(frame: NSRect(x: 4, y: 3, width: 16, height: 16))
                icon.image = image
                icon.imageScaling = .scaleProportionallyUpOrDown
                icon.setAccessibilityLabel("\(self.title) icon")
                container.addSubview(icon)
                accessory.view = container
                window.addTitlebarAccessoryViewController(accessory)
            } catch is CancellationError {
                // Closing the stream cancels icon loading.
            } catch {
                NSLog("BeamIt window icon unavailable: %@", error.localizedDescription)
            }
        }
    }

    private func cancelDockAttention() {
        if let attentionRequest {
            NSApp.cancelUserAttentionRequest(attentionRequest)
            self.attentionRequest = nil
        }
    }

    private func updateDockAttention() {
        guard !isPopup else { return }
        guard !stream.attentionWindows.isEmpty else {
            cancelDockAttention()
            return
        }
        guard let window, window.isVisible,
              !(NSApp.isActive && window.isKeyWindow), attentionRequest == nil else { return }
        attentionRequest = NSApp.requestUserAttention(.informationalRequest)
    }

    private func handleInputError(_ message: String) {
        stopTypingText()
        inputHandler.releaseMouseCapture()
        inputHandler.releaseAll()
        onError?(message)
        guard !isShowingInputError, let window, window.attachedSheet == nil else { return }
        isShowingInputError = true
        let alert = NSAlert()
        alert.messageText = "Remote input was rejected"
        alert.informativeText = "\(message)\n\nText sending has stopped; some characters may not have arrived. On the Windows host, activate the target window and select its text field, then retry. Elevated applications require an elevated host; input is unavailable on the lock or UAC screen."
        alert.addButton(withTitle: "OK")
        alert.beginSheetModal(for: window) { [weak self] _ in
            self?.isShowingInputError = false
        }
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    func show() {
        if isPopup {
            window?.orderFront(nil)
        } else {
            showWindow(nil)
            window?.makeKeyAndOrderFront(nil)
            stream.popupCoordinator?.register(self)
            updateDockAttention()
        }
    }

    func closeAfterHostStop() {
        isClosingFromHost = true
        stream.handleStopped()
        close()
    }

    /// 1x maps one video pixel to one device pixel, including on Retina displays.
    private func applyPixelSize() {
        guard let window, !isApplyingPixelSize else { return }
        isApplyingPixelSize = true
        defer { isApplyingPixelSize = false }
        let scale = window.backingScaleFactor > 0 ? window.backingScaleFactor : 1
        if let surface = window.contentView as? BeamItStreamSurface {
            surface.videoSize = videoPixelSize
            surface.needsLayout = true
            surface.layoutSubtreeIfNeeded()
        }
        frameView.layer?.contentsScale = scale
        guard !window.styleMask.contains(.fullScreen), !isFullscreenTransition else { return }
        let size = NSSize(
            width: videoPixelSize.width / scale * windowScale,
            height: videoPixelSize.height / scale * windowScale
        )
        if allowsScaling {
            window.contentAspectRatio = videoPixelSize
        }
        guard window.contentRect(forFrameRect: window.frame).size != size else { return }
        let oldFrame = window.frame
        var newFrame = window.frameRect(forContentRect: NSRect(origin: .zero, size: size))
        newFrame.origin = NSPoint(x: oldFrame.minX, y: oldFrame.maxY - newFrame.height)
        window.setFrame(newFrame, display: true)
        window.contentView?.layoutSubtreeIfNeeded()
    }

    func windowDidResize(_ notification: Notification) {
        window?.contentView?.layoutSubtreeIfNeeded()
        if allowsScaling, !isApplyingPixelSize, !isFullscreenTransition, let window,
           !window.styleMask.contains(.fullScreen), videoPixelSize.width > 0 {
            let size = window.contentRect(forFrameRect: window.frame).size
            windowScale = size.width * window.backingScaleFactor / videoPixelSize.width
        }
        if !isPopup { stream.popupCoordinator?.reposition() }
    }

    func windowWillEnterFullScreen(_ notification: Notification) {
        isFullscreenTransition = true
        lastEscapeTimestamp = nil
        window?.contentResizeIncrements = NSSize(width: 1, height: 1)
    }

    func windowDidEnterFullScreen(_ notification: Notification) {
        isFullscreenTransition = false
        applyPixelSize()
        stream.popupCoordinator?.reposition()
        if resetScaleAfterFullscreen { toggleFullscreen() }
    }

    func windowWillExitFullScreen(_ notification: Notification) {
        isFullscreenTransition = true
        lastEscapeTimestamp = nil
        inputHandler.releaseMouseCapture()
    }

    func windowDidExitFullScreen(_ notification: Notification) {
        restoreWindowedPresentation()
    }

    private func restoreWindowedPresentation() {
        // AppKit still restores its saved style/frame after the did-exit delegate callback.
        // Keep resize tracking suspended until that restoration has finished.
        isFullscreenTransition = true
        DispatchQueue.main.async { [weak self] in
            guard let self, !self.isClosing, let window = self.window,
                  !window.styleMask.contains(.fullScreen) else { return }
            if self.resetScaleAfterFullscreen {
                self.windowScale = 1
                self.resetScaleAfterFullscreen = false
            }
            var style: NSWindow.StyleMask = [.titled, .closable, .miniaturizable]
            if self.allowsScaling { style.insert(.resizable) }
            window.styleMask = style
            window.titleVisibility = .visible
            window.titlebarAppearsTransparent = false
            window.contentResizeIncrements = NSSize(width: 1, height: 1)
            self.isFullscreenTransition = false
            self.applyPixelSize()
            self.stream.popupCoordinator?.reposition()
        }
    }

    func windowDidFailToEnterFullScreen(_ window: NSWindow) {
        resetScaleAfterFullscreen = false
        restoreWindowedPresentation()
        NSLog("BeamIt could not enter fullscreen.")
    }

    func windowDidFailToExitFullScreen(_ window: NSWindow) {
        isFullscreenTransition = false
        resetScaleAfterFullscreen = false
        NSLog("BeamIt could not exit fullscreen.")
    }

    func windowDidChangeBackingProperties(_ notification: Notification) {
        applyPixelSize()
        stream.popupCoordinator?.reposition()
    }

    func windowDidChangeScreen(_ notification: Notification) {
        applyPixelSize()
        stream.popupCoordinator?.reposition()
    }

    func windowDidMove(_ notification: Notification) {
        if !isPopup { stream.popupCoordinator?.reposition() }
    }

    func windowDidBecomeKey(_ notification: Notification) {
        Self.activeController = self
        BeamItAntiIdle.shared.streamDidBecomeKey(self)
        cancelDockAttention()
    }

    func antiIdleMove(offset: CGPoint) {
        guard window?.attachedSheet == nil, !stream.isClosed else { return }
        let video = stream.video
        guard video.width > 0, video.height > 0 else { return }
        inputHandler.sendSyntheticPosition(
            BeamItAntiIdle.position(offset: offset, width: video.width, height: video.height)
        )
    }

    func antiIdleStopped() {
        inputHandler.forgetPosition()
    }

    func windowDidResignKey(_ notification: Notification) {
        inputHandler.releaseMouseCapture()
        inputHandler.releaseAll()
    }

    func windowWillClose(_ notification: Notification) {
        isClosing = true
        inputHandler.releaseMouseCapture()
        inputHandler.releaseAll()
        if let parametersWindow {
            window?.endSheet(parametersWindow)
            parametersWindow.close()
            self.parametersWindow = nil
        }
        infoWindow?.close()
        infoWindow = nil
        if !isPopup { stream.popupCoordinator?.unregister(self) }
        iconTask?.cancel()
        iconTask = nil
        cancelDockAttention()
        stopTypingText()
        if Self.activeController === self {
            Self.activeController = nil
        }
        if let keyMonitor {
            NSEvent.removeMonitor(keyMonitor)
            self.keyMonitor = nil
        }
        frameView.inputHandler = nil
        stream.onFrame = nil
        if !isClosingFromHost {
            Task { await stream.stop() }
        }
        onClose?()
    }
}
