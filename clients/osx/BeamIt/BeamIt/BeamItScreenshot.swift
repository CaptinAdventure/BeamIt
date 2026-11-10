import AppKit
import CoreImage
import CoreVideo

enum BeamItScreenshot {
    static func png(from pixelBuffer: CVPixelBuffer) throws -> Data {
        let image = CIImage(cvPixelBuffer: pixelBuffer)
        let context = CIContext(options: [.cacheIntermediates: false])
        guard let colorSpace = CGColorSpace(name: CGColorSpace.sRGB),
              let data = context.pngRepresentation(of: image, format: .RGBA8, colorSpace: colorSpace) else {
            throw CocoaError(.fileWriteUnknown)
        }
        return data
    }
}
