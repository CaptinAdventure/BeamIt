import AppKit
import CoreVideo

// Standalone model doubles keep diagnostics checks independent of a live host.
struct BeamItVideo {
    let generation = 2
    let codec = "h264"
    let width = 1920
    let height = 1080
    let codec_string: String? = "avc1.F4001F"
    let chroma_format: String? = "4:4:4"
    let bit_depth: Int? = 8
    let frame_rate: Int? = 60
    let bitrate_kbps: Int? = 48000
    let max_bitrate_kbps: Int? = 96000
    let rate_control: String? = "vbr"
    let encoder: String? = "NVENC"
}
final class BeamItVideoStream {
    let diagnostics = BeamItStreamDiagnostics()
    let video = BeamItVideo()
    let offer = (streamID: "test", unused: false)
    let isClosed = false
}
final class BeamItStreamWindowController {
    static weak var activeController: BeamItStreamWindowController?
    let stream = BeamItVideoStream()
    let title = "Test"
    var infoWindow: NSWindow?
}

@main
struct StreamInfoTests {
    static func main() throws {
        let clock = BeamItClockEstimate()
        clock.record(hostUS: 15_010_000, sent: 10, received: 10.02)
        precondition(abs(clock.best(now: 10.02)!.offsetUS - 5_000_000) < 0.001)
        clock.record(hostUS: 17_010_000, sent: 11, received: 11.1)
        precondition(abs(clock.best(now: 11.1)!.rttUS - 20_000) < 0.001)
        precondition(clock.best(now: 42) == nil, "stale samples must expire")
        let negative = BeamItClockEstimate()
        negative.record(hostUS: 5_010_000, sent: 10, received: 10.02)
        precondition(abs(negative.best(now: 10.02)!.offsetUS + 5_000_000) < 0.001)
        let liveClock = BeamItClockEstimate()
        let now = ProcessInfo.processInfo.systemUptime
        liveClock.record(hostUS: UInt64((now + 4.99) * 1_000_000), sent: now - 0.02, received: now)
        let latencyDiagnostics = BeamItStreamDiagnostics(clock: liveClock)
        latencyDiagnostics.queued(milliseconds: 1, captureUS: UInt64((now + 5 - 0.025) * 1_000_000),
                                  now: now)
        let latencyRows = Dictionary(uniqueKeysWithValues: latencyDiagnostics.rows(video: BeamItVideo(), streamID: "latency"))
        precondition(latencyRows["Estimated capture → enqueue"]?.hasPrefix("25.00 ms") == true)
        let diagnostics = BeamItStreamDiagnostics()
        func rows() -> [String: String] {
            Dictionary(uniqueKeysWithValues: diagnostics.rows(video: BeamItVideo(), streamID: "test"))
        }
        precondition(rows()["Chroma / bit depth"] == "4:4:4 / 8 bit")
        precondition(rows()["Rate control / effective peak"] == "vbr / 96000 kbit/s")
        precondition(rows()["Decode submission → callback"] == "Waiting for frame")
        var buffer: CVPixelBuffer?
        precondition(CVPixelBufferCreate(kCFAllocatorDefault, 64, 32, kCVPixelFormatType_32BGRA,
                                         nil, &buffer) == kCVReturnSuccess)
        guard let buffer else { fatalError("Could not allocate test buffer") }
        CVBufferSetAttachment(buffer, kCVImageBufferColorPrimariesKey,
                              kCVImageBufferColorPrimaries_ITU_R_709_2, .shouldPropagate)
        diagnostics.receivedPacket(bytes: 125_000)
        diagnostics.decodedFrame(buffer, milliseconds: 2.5)
        diagnostics.queued(milliseconds: 1.25)
        diagnostics.coalescedFrame()
        precondition(rows()["Decoded output"] == "64 × 32, BGRA")
        precondition(rows()["Decode submission → callback"] == "2.50 ms (latest)")
        precondition(rows()["Decode callback → display enqueue"] == "1.25 ms (latest)")
        precondition(rows()["Frames coalesced before enqueue"] == "1")
        precondition(rows()["Output color metadata"]?.contains("709") == true)
        precondition(rows()["Actual presentation lateness"]?.hasPrefix("Unavailable") == true)
        precondition(rows()["Host statistics"]?.contains("Waiting") == true)
        let report = try JSONDecoder().decode(BeamItHostStatsReport.self, from: Data(#"""
        {"host_time_us":123456789,"interval_ms":10000.4,"streams":[{
          "stream_id":"test","connected":true,"generation":2,"width":1920,"height":1080,
          "frame_rate":60,"bitrate_kbps":20000,"interval_ms":10000.4,
          "frames_captured":567,"frames_encoded":599,"key_frames":10,"frames_sent":599,
          "frames_dropped_encoder":15,"frames_dropped_network":0,"network_recoveries":0,
          "queued_frames":0,"encoded_kbps":12148,"sent_kbps":12163,"encoder_load":0.79,
          "capture_ms":null,"encode_ms":{"avg":12.71,"max":25.01},
          "send_ms":{"avg":0.85,"max":11.46},"write_ms":{"avg":0.75,"max":11.36},
          "host_latency_ms":{"avg":14.1,"max":30.9}
        }]}
        """#.utf8))
        diagnostics.updateHostStats(report.streams[0], hostTime: report.host_time_us)
        precondition(rows()["Host encoder load"] == "79.0%")
        precondition(rows()["Host skipped encoder ticks"] == "15")
        precondition(rows()["Host capture → submission"] == "No samples")
        precondition(rows()["Host encode"] == "12.71 avg / 25.01 max ms")
        precondition(rows()["Host clock at report"]?.contains("123456789") == true)
        let empty = try JSONDecoder().decode(BeamItHostStatsReport.self, from:
            Data(#"{"host_time_us":1,"interval_ms":10000,"streams":[]}"#.utf8))
        precondition(empty.streams.isEmpty)
        Thread.sleep(forTimeInterval: 1.05)
        precondition(rows()["Measured rate"]?.contains("Mbit/s") == true)
        Thread.sleep(forTimeInterval: 1.05)
        precondition(rows()["Measured rate"]?.hasPrefix("0.00 Mbit/s") == true)
        print("Stream diagnostic metadata, timing and idle rate checks passed.")
    }
}
