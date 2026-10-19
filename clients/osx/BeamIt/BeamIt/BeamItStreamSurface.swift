import AppKit

final class BeamItStreamSurface: NSView {
    var videoSize = NSSize(width: 1, height: 1)

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
        layer?.backgroundColor = NSColor.black.cgColor
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    override func layout() {
        super.layout()
        guard videoSize.width > 0, videoSize.height > 0 else { return }
        let scale = min(bounds.width / videoSize.width, bounds.height / videoSize.height)
        let size = NSSize(width: videoSize.width * scale, height: videoSize.height * scale)
        subviews.first?.frame = NSRect(
            x: bounds.midX - size.width / 2, y: bounds.midY - size.height / 2,
            width: size.width, height: size.height
        )
    }
}
