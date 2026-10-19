import AppKit
import CoreVideo
import SwiftUI

final class BeamItStreamDiagnostics {
    private let clock: BeamItClockEstimate
    private var captureToEnqueueMS: Double?
    private var latencySampleTime: TimeInterval?

    init(clock: BeamItClockEstimate = BeamItClockEstimate()) { self.clock = clock }
    private let lock = NSLock()
    private var bytes = 0
    private var received = 0
    private var decoded = 0
    private var enqueued = 0
    private var coalesced = 0
    private var decodeMS: Double?
    private var queueMS: Double?
    private var pixels = "Waiting for decoded frame"
    private var color = "Not reported"
    private var epoch = ProcessInfo.processInfo.systemUptime
    private var previous = (bytes: 0, received: 0, decoded: 0, enqueued: 0)
    private var rates = "Waiting for frames"
    private var hostStats: BeamItHostStreamStats?
    private var hostTime: UInt64 = 0
    private var hostReportReceived: TimeInterval = 0

    func updateHostStats(_ stats: BeamItHostStreamStats, hostTime: UInt64) {
        lock.lock()
        defer { lock.unlock() }
        hostStats = stats
        self.hostTime = hostTime
        hostReportReceived = ProcessInfo.processInfo.systemUptime
    }

    func receivedPacket(bytes: Int) {
        lock.lock()
        defer { lock.unlock() }
        self.bytes += bytes
        received += 1
    }

    func decodedFrame(_ image: CVPixelBuffer, milliseconds: Double) {
        lock.lock()
        defer { lock.unlock() }
        decoded += 1
        decodeMS = milliseconds
        let format = CVPixelBufferGetPixelFormatType(image)
        let fourCC = String(bytes: [
            UInt8((format >> 24) & 255), UInt8((format >> 16) & 255),
            UInt8((format >> 8) & 255), UInt8(format & 255)
        ], encoding: .ascii) ?? String(format)
        pixels = "\(CVPixelBufferGetWidth(image)) × \(CVPixelBufferGetHeight(image)), \(fourCC)"
        func attachment(_ key: CFString) -> String {
            guard let value = CVBufferCopyAttachment(image, key, nil) else { return "not reported" }
            return String(describing: value)
        }
        color = "Primaries: \(attachment(kCVImageBufferColorPrimariesKey))\nTransfer: \(attachment(kCVImageBufferTransferFunctionKey))\nYCbCr matrix: \(attachment(kCVImageBufferYCbCrMatrixKey))"
    }

    func queued(milliseconds: Double, captureUS: UInt64? = nil,
                now: TimeInterval = ProcessInfo.processInfo.systemUptime) {
        lock.lock()
        defer { lock.unlock() }
        enqueued += 1
        queueMS = milliseconds
        if let captureUS, let estimate = clock.best(now: now) {
            captureToEnqueueMS = (now * 1_000_000 - Double(captureUS) + estimate.offsetUS) / 1000
            latencySampleTime = now
        } else {
            captureToEnqueueMS = nil
            latencySampleTime = nil
        }
    }

    func coalescedFrame() {
        lock.lock()
        coalesced += 1
        lock.unlock()
    }

    func rows(video: BeamItVideo, streamID: String) -> [(String, String)] {
        lock.lock()
        defer { lock.unlock() }
        let now = ProcessInfo.processInfo.systemUptime
        let elapsed = now - epoch
        if elapsed >= 1 {
            rates = String(format: "%.2f Mbit/s · %.1f received / %.1f decoded / %.1f enqueued FPS",
                           Double(bytes - previous.bytes) * 8 / elapsed / 1_000_000,
                           Double(received - previous.received) / elapsed,
                           Double(decoded - previous.decoded) / elapsed,
                           Double(enqueued - previous.enqueued) / elapsed)
            previous = (bytes, received, decoded, enqueued)
            epoch = now
        }
        func ms(_ value: Double?) -> String { value.map { String(format: "%.2f ms (latest)", $0) } ?? "Waiting for frame" }
        let estimate = clock.best(now: now)
        let latency = estimate != nil ? captureToEnqueueMS : nil
        let localRows: [(String, String)] = [
            ("Stream", streamID), ("Generation", "\(video.generation)"),
            ("Codec", video.codec_string ?? video.codec),
            ("Video size", "\(video.width) × \(video.height) pixels"),
            ("Chroma / bit depth", "\(video.chroma_format ?? "Unknown") / \(video.bit_depth.map(String.init) ?? "Unknown") bit"),
            ("Host encoder", video.encoder ?? "Not reported"),
            ("Rate control / effective peak", "\(video.rate_control ?? "Not reported") / \(video.max_bitrate_kbps.map(String.init) ?? "?") kbit/s"),
            ("Requested rate", "\(video.frame_rate.map(String.init) ?? "?") FPS · \(video.bitrate_kbps.map(String.init) ?? "?") kbit/s"),
            ("Measured rate", rates),
            ("Decoded output", pixels), ("Output color metadata", color),
            ("Decode submission → callback", ms(decodeMS)),
            ("Decode callback → display enqueue", ms(queueMS)),
            ("Frames coalesced before enqueue", "\(coalesced)"),
            ("Estimated capture → enqueue", latency.map { String(format: "%.2f ms (not display completion)", $0) } ?? "Waiting for clock sample and frame"),
            ("Latency sample age", latency != nil ? String(format: "%.1f s", now - (latencySampleTime ?? now)) : "Unavailable"),
            ("Clock RTT / nominal uncertainty", estimate.map { String(format: "%.2f ms RTT / ±%.2f ms (assumes symmetric path)", $0.rttUS / 1000, $0.rttUS / 2000) } ?? "No fresh clock sample"),
            ("Clock sample age", estimate.map { String(format: "%.1f s (expires at 30 s)", now - $0.received) } ?? "Unavailable"),
            ("Actual presentation lateness", "Unavailable — enqueue is not display completion"),
            ("Capture → display latency", "Unavailable — actual display completion is not measured")
        ]
        return localRows + (hostStats?.rows(hostTime: hostTime, age: now - hostReportReceived)
                            ?? [("Host statistics", "Waiting for first 10-second report")])
    }
}

private struct BeamItStreamInfoView: View {
    let stream: BeamItVideoStream

    var body: some View {
        TimelineView(.periodic(from: .now, by: 1)) { _ in
            ScrollView {
                Grid(alignment: .topLeading, horizontalSpacing: 20, verticalSpacing: 10) {
                    ForEach(stream.diagnostics.rows(video: stream.video, streamID: stream.offer.streamID), id: \.0) { row in
                        GridRow {
                            Text(row.0).foregroundStyle(.secondary)
                            Text(row.1).textSelection(.enabled)
                        }
                    }
                }
                .font(.system(size: 12, design: .monospaced))
                .padding(20)
            }
        }
        .frame(minWidth: 680, minHeight: 440)
    }
}

extension BeamItStreamWindowController {
    static func showActiveStreamInfo() {
        guard let controller = activeController, !controller.stream.isClosed else {
            NSSound.beep()
            return
        }
        if let window = controller.infoWindow {
            window.makeKeyAndOrderFront(nil)
            return
        }
        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 780, height: 530),
            styleMask: [.titled, .closable, .miniaturizable, .resizable],
            backing: .buffered, defer: false
        )
        window.title = "Stream info — \(controller.title)"
        window.isReleasedWhenClosed = false
        window.contentView = NSHostingView(rootView: BeamItStreamInfoView(stream: controller.stream))
        window.center()
        controller.infoWindow = window
        window.makeKeyAndOrderFront(nil)
    }
}
