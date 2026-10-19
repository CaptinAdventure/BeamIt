import AppKit
import AVFoundation
import CoreMedia
import CoreVideo

final class BeamItFrameView: NSView {
    var diagnostics: BeamItStreamDiagnostics?
    private var pendingSince: TimeInterval = 0
    private var pendingCaptureUS: UInt64 = 0
    var inputHandler: BeamItInputHandler?
    private var trackingArea: NSTrackingArea?

    override var acceptsFirstResponder: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let trackingArea {
            removeTrackingArea(trackingArea)
        }
        let area = NSTrackingArea(
            rect: .zero,
            options: [.mouseMoved, .activeAlways, .inVisibleRect],
            owner: self,
            userInfo: nil
        )
        addTrackingArea(area)
        trackingArea = area
    }

    override func mouseMoved(with event: NSEvent) { inputHandler?.mouseMoved(event, in: self) }
    override func mouseDragged(with event: NSEvent) { inputHandler?.mouseMoved(event, in: self) }
    override func rightMouseDragged(with event: NSEvent) { inputHandler?.mouseMoved(event, in: self) }
    override func otherMouseDragged(with event: NSEvent) { inputHandler?.mouseMoved(event, in: self) }
    override func mouseDown(with event: NSEvent) { inputHandler?.mouseButton(event, in: self, down: true) }
    override func mouseUp(with event: NSEvent) { inputHandler?.mouseButton(event, in: self, down: false) }
    override func rightMouseDown(with event: NSEvent) { inputHandler?.mouseButton(event, in: self, down: true) }
    override func rightMouseUp(with event: NSEvent) { inputHandler?.mouseButton(event, in: self, down: false) }
    override func otherMouseDown(with event: NSEvent) { inputHandler?.mouseButton(event, in: self, down: true) }
    override func otherMouseUp(with event: NSEvent) { inputHandler?.mouseButton(event, in: self, down: false) }
    override func scrollWheel(with event: NSEvent) { inputHandler?.scrollWheel(event, in: self) }
    private let frameLock = NSLock()
    private var pendingPixelBuffer: CVPixelBuffer?
    private var presentationScheduled = false
    private var formatDescription: CMVideoFormatDescription?

    override var wantsUpdateLayer: Bool { true }

    override func makeBackingLayer() -> CALayer {
        let layer = AVSampleBufferDisplayLayer()
        layer.backgroundColor = NSColor.black.cgColor
        layer.videoGravity = .resize
        return layer
    }

    func display(_ pixelBuffer: CVPixelBuffer, captureUS: UInt64) {
        frameLock.lock()
        if pendingPixelBuffer != nil { diagnostics?.coalescedFrame() }
        pendingSince = ProcessInfo.processInfo.systemUptime
        pendingCaptureUS = captureUS
        pendingPixelBuffer = pixelBuffer
        let shouldSchedule = !presentationScheduled
        presentationScheduled = true
        frameLock.unlock()

        if shouldSchedule {
            DispatchQueue.main.async { [weak self] in
                self?.presentLatestFrame()
            }
        }
    }

    private func presentLatestFrame() {
        frameLock.lock()
        let pixelBuffer = pendingPixelBuffer
        let queuedSince = pendingSince
        let captureUS = pendingCaptureUS
        pendingPixelBuffer = nil
        presentationScheduled = false
        frameLock.unlock()
        guard let pixelBuffer else { return }

        if let current = formatDescription,
           !CMVideoFormatDescriptionMatchesImageBuffer(current, imageBuffer: pixelBuffer) {
            formatDescription = nil
        }
        if formatDescription == nil,
           CMVideoFormatDescriptionCreateForImageBuffer(
               allocator: kCFAllocatorDefault,
               imageBuffer: pixelBuffer,
               formatDescriptionOut: &formatDescription
           ) != noErr {
            return
        }
        guard let formatDescription else { return }
        var timing = CMSampleTimingInfo(
            duration: .invalid,
            presentationTimeStamp: CMClockGetTime(CMClockGetHostTimeClock()),
            decodeTimeStamp: .invalid
        )
        var sampleBuffer: CMSampleBuffer?
        guard CMSampleBufferCreateReadyWithImageBuffer(
            allocator: kCFAllocatorDefault,
            imageBuffer: pixelBuffer,
            formatDescription: formatDescription,
            sampleTiming: &timing,
            sampleBufferOut: &sampleBuffer
        ) == noErr, let sampleBuffer else { return }
        let displayLayer = layer as? AVSampleBufferDisplayLayer
        displayLayer?.sampleBufferRenderer.enqueue(sampleBuffer)
        if displayLayer != nil {
            let now = ProcessInfo.processInfo.systemUptime
            diagnostics?.queued(milliseconds: (now - queuedSince) * 1000, captureUS: captureUS, now: now)
        }
    }
}
