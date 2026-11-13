import CoreVideo
import Foundation

struct BeamItApplication: Identifiable {
    let pid: Int
    let title: String
    let imageName: String
    let windows: [BeamItHostWindow]

    var id: Int { pid }
}

struct BeamItVideoColor: Decodable {
    let primaries: String?
    let transfer: String?
    let matrix: String?
    let range: String?

    /// The host currently always streams limited-range BT.709/sRGB SDR (PROTOCOL.md §5.5).
    static let sdr = BeamItVideoColor(primaries: "bt709", transfer: "srgb", matrix: "bt709", range: "limited")

    func tag(_ buffer: CVPixelBuffer) {
        let primariesValue: CFString = primaries == "bt2020" ? kCVImageBufferColorPrimaries_ITU_R_2020 : kCVImageBufferColorPrimaries_ITU_R_709_2
        let transferValue: CFString = switch transfer {
        case "pq": kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ
        case "hlg": kCVImageBufferTransferFunction_ITU_R_2100_HLG
        case "bt709": kCVImageBufferTransferFunction_ITU_R_709_2
        default: kCVImageBufferTransferFunction_sRGB
        }
        let matrixValue: CFString = matrix == "bt2020" ? kCVImageBufferYCbCrMatrix_ITU_R_2020 : kCVImageBufferYCbCrMatrix_ITU_R_709_2
        CVBufferSetAttachment(buffer, kCVImageBufferColorPrimariesKey, primariesValue, .shouldPropagate)
        CVBufferSetAttachment(buffer, kCVImageBufferTransferFunctionKey, transferValue, .shouldPropagate)
        CVBufferSetAttachment(buffer, kCVImageBufferYCbCrMatrixKey, matrixValue, .shouldPropagate)
    }
}

struct BeamItVideo: Decodable {
    let color: BeamItVideoColor?
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
    let input: Input?

    /// `input.cursor` is only present when the host supports pointer-shape mirroring (§5.9.11).
    struct Input: Decodable {
        let mouse_mode: String?
        let cursor: String?
    }

    var hostSendsCursorShape: Bool { input?.cursor == "client" }

    enum CodingKeys: String, CodingKey {
        case streamID = "stream_id"
        case url
        case tokenExpiresInMS = "token_expires_in_ms"
        case source
        case video
        case input
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
