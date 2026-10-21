import Foundation

struct BeamItApplication: Identifiable {
    let pid: Int
    let title: String
    let imageName: String
    let windows: [BeamItHostWindow]

    var id: Int { pid }
}

struct BeamItVideo: Decodable {
    let generation: Int
    let codec: String
    let width: Int
    let height: Int
    let codec_string: String?
    let chroma_format: String?
    let bit_depth: Int?
    var frame_rate: Int?
    var bitrate_kbps: Int?
    var max_bitrate_kbps: Int?
    let rate_control: String?
    let encoder: String?
}

struct BeamItStreamOffer: Decodable {
    let streamID: String
    let url: URL
    let tokenExpiresInMS: Int
    let source: StreamSource
    let video: BeamItVideo

    enum CodingKeys: String, CodingKey {
        case streamID = "stream_id"
        case url
        case tokenExpiresInMS = "token_expires_in_ms"
        case source
        case video
    }

    struct StreamSource: Decodable {
        let kind: String
        let hwnd: String?
        let pid: Int?
        let title: String?
        let capture: String?
    }
}

enum BeamItSource: Hashable {
    case desktopDXGI
    case desktopWGC
    case application(Int)
    case window(String)

    var title: String {
        switch self {
        case .desktopDXGI:
            return "Desktop · DXGI"
        case .desktopWGC:
            return "Desktop · WGC"
        case .application(let pid):
            return "Application · PID \(pid)"
        case .window(let hwnd):
            return "Window · \(hwnd)"
        }
    }
}
