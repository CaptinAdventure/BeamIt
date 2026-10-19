import Foundation

struct BeamItHostStatsReport: Decodable {
    let host_time_us: UInt64
    let interval_ms: Double
    let streams: [BeamItHostStreamStats]
}

struct BeamItHostStreamStats: Decodable {
    struct DurationStats: Decodable {
        let avg: Double
        let max: Double
    }

    let stream_id: String
    let connected: Bool
    let generation: Int
    let width: Int
    let height: Int
    let frame_rate: Int
    let bitrate_kbps: Int
    let interval_ms: Double
    let frames_captured: UInt64
    let frames_encoded: UInt64
    let key_frames: UInt64
    let frames_sent: UInt64
    let frames_dropped_encoder: UInt64
    let frames_dropped_network: UInt64
    let network_recoveries: UInt64
    let queued_frames: UInt64
    let encoded_kbps: Double
    let sent_kbps: Double
    let encoder_load: Double
    let capture_ms: DurationStats?
    let encode_ms: DurationStats?
    let send_ms: DurationStats?
    let write_ms: DurationStats?
    let host_latency_ms: DurationStats?

    func rows(hostTime: UInt64, age: Double) -> [(String, String)] {
        func duration(_ value: DurationStats?) -> String {
            guard let value else { return "No samples" }
            return String(format: "%.2f avg / %.2f max ms", value.avg, value.max)
        }
        return [
            ("Host report age", String(format: "%.1f s (reports every 10 s)", age)),
            ("Host clock at report", "\(hostTime) µs (host monotonic clock)"),
            ("Host reporting interval", String(format: "%.2f s", interval_ms / 1000)),
            ("Host socket", connected ? "Connected" : "Not connected"),
            ("Host reported video", "\(width) × \(height) · generation \(generation)"),
            ("Host reported targets", "\(frame_rate) FPS · \(bitrate_kbps) kbit/s"),
            ("Host actual rates", String(format: "%.0f encoded / %.0f sent kbit/s", encoded_kbps, sent_kbps)),
            ("Host encoder load", String(format: "%.1f%%", encoder_load * 100)),
            ("Host frames (interval)", "\(frames_captured) fresh / \(frames_encoded) encoded / \(frames_sent) sent / \(key_frames) key"),
            ("Host skipped encoder ticks", "\(frames_dropped_encoder)"),
            ("Host network drops / recoveries", "\(frames_dropped_network) / \(network_recoveries)"),
            ("Host queued frames", "\(queued_frames)"),
            ("Host capture → submission", duration(capture_ms)),
            ("Host encode", duration(encode_ms)),
            ("Host encode → write completion", duration(send_ms)),
            ("Host socket write", duration(write_ms)),
            ("Host capture → write completion", duration(host_latency_ms))
        ]
    }
}
