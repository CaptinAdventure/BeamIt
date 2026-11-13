import AppKit
import CoreVideo

@main
struct ScreenshotTests {
    static func main() throws {
        var created: CVPixelBuffer?
        precondition(CVPixelBufferCreate(
            kCFAllocatorDefault, 16, 8, kCVPixelFormatType_32BGRA, nil, &created
        ) == kCVReturnSuccess)
        let buffer = created!
        CVBufferSetAttachment(
            buffer, kCVImageBufferCGColorSpaceKey, CGColorSpace(name: CGColorSpace.sRGB)!,
            .shouldPropagate
        )
        precondition(CVPixelBufferLockBaseAddress(buffer, []) == kCVReturnSuccess)
        let pixels = CVPixelBufferGetBaseAddress(buffer)!.assumingMemoryBound(to: UInt8.self)
        let stride = CVPixelBufferGetBytesPerRow(buffer)
        for y in 0..<8 {
            for x in 0..<16 {
                let offset = y * stride + x * 4
                pixels[offset] = 0
                pixels[offset + 1] = 0
                pixels[offset + 2] = y < 4 ? 255 : 0
                pixels[offset + 3] = 255
            }
        }
        CVPixelBufferUnlockBaseAddress(buffer, [])
        let png = try BeamItScreenshot.png(from: buffer)
        precondition(Array(png.prefix(8)) == [137, 80, 78, 71, 13, 10, 26, 10])
        let decoded = NSBitmapImageRep(data: png)!
        precondition(decoded.pixelsWide == 16 && decoded.pixelsHigh == 8)
        var top = [Int](repeating: 0, count: decoded.samplesPerPixel)
        var bottom = top
        decoded.getPixel(&top, atX: 0, y: 0)
        decoded.getPixel(&bottom, atX: 0, y: 7)
        precondition(top[0] > 230 && top[1] < 25, "Unexpected top pixels: \(top)")
        precondition(bottom[0] < 25, "Unexpected bottom pixels: \(bottom)")
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString + ".png")
        try png.write(to: url, options: .atomic)
        defer { try? FileManager.default.removeItem(at: url) }
        let saved = try Data(contentsOf: url)
        precondition(saved == png)
        print("PNG signature, original dimensions, orientation, colors, and file export checks passed.")
    }
}
