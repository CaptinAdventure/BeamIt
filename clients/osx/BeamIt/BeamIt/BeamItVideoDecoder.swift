import CoreMedia
import CoreVideo
import Foundation
import VideoToolbox

final class BeamItVideoDecoder {
    let generation: Int

    private let codec: String
    private let width: Int
    private let height: Int
    private let hdrChroma: String?
    private let av1: AV1Config?
    private let onError: (Error) -> Void
    private var formatDescription: CMVideoFormatDescription?
    private var session: VTDecompressionSession?
    private let diagnostics: BeamItStreamDiagnostics
    private let timingLock = NSLock()
    private var decodeStarts: [Int64: (start: TimeInterval, captureUS: UInt64)] = [:]
    private var nextDecodeID: Int64 = 0

    init(codec: String, width: Int, height: Int, generation: Int, hdrChroma: String? = nil, codecString: String? = nil, chroma: String? = nil, diagnostics: BeamItStreamDiagnostics, onError: @escaping (Error) -> Void) throws {
        self.diagnostics = diagnostics
        self.codec = codec
        self.width = width
        self.height = height
        self.generation = generation
        self.hdrChroma = hdrChroma
        if codec == "av1" {
            guard let config = AV1Config(codecString: codecString, chroma: chroma) else { throw DecoderError.missingParameterSets }
            av1 = config
        } else {
            av1 = nil
        }
        self.onError = onError
    }

    deinit {
        if let session {
            VTDecompressionSessionInvalidate(session)
        }
    }

    func decode(_ accessUnit: Data, timestamp: UInt64, output: @escaping (CVPixelBuffer, UInt64) -> Void) throws {
        if codec == "av1" {
            try decodeAV1(accessUnit, timestamp: timestamp, output: output)
            return
        }
        let nalUnits = Self.splitNALUnits(accessUnit)
        guard !nalUnits.isEmpty else {
            throw DecoderError.invalidAccessUnit
        }
        if session == nil {
            let parameterSets = Self.parameterSets(in: nalUnits, codec: codec)
            try createSession(parameterSets: parameterSets, output: output)
        }

        let mediaNALUnits = nalUnits.filter { !Self.isNonPictureNAL($0, codec: codec) }
        let sampleData = Self.lengthPrefixed(mediaNALUnits)
        guard !sampleData.isEmpty else { throw DecoderError.invalidAccessUnit }
        try submit(sampleData, timestamp: timestamp)
    }

    /// AV1 temporal units are low-overhead OBU streams (PROTOCOL.md); the sequence header seeds the `av1C` atom.
    private func decodeAV1(_ temporalUnit: Data, timestamp: UInt64, output: @escaping (CVPixelBuffer, UInt64) -> Void) throws {
        guard let av1 else { throw DecoderError.missingParameterSets }
        let obus = AV1Config.splitOBUs([UInt8](temporalUnit))
        guard !obus.isEmpty else { throw DecoderError.invalidAccessUnit }
        if session == nil {
            guard let header = obus.first(where: { $0.type == 1 }) else { throw DecoderError.missingParameterSets }
            var description: CMVideoFormatDescription?
            let status = CMVideoFormatDescriptionCreate(
                allocator: kCFAllocatorDefault,
                codecType: kCMVideoCodecType_AV1,
                width: Int32(width),
                height: Int32(height),
                extensions: [
                    kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms: ["av1C": av1.record(sequenceHeader: header.bytes)]
                ] as CFDictionary,
                formatDescriptionOut: &description
            )
            guard status == noErr, let description else { throw DecoderError.videoToolbox(status) }
            try createSession(description: description, output: output)
        }
        // Temporal delimiters are implied by sample boundaries in ISO BMFF.
        let sample = obus.filter { $0.type != 2 }.reduce(into: Data()) { $0.append(contentsOf: $1.bytes) }
        guard !sample.isEmpty else { throw DecoderError.invalidAccessUnit }
        try submit(sample, timestamp: timestamp)
    }

    private func createSession(parameterSets: [[UInt8]], output: @escaping (CVPixelBuffer, UInt64) -> Void) throws {
        let requiredParameterSets = codec == "h264" ? 2 : 3
        guard parameterSets.count >= requiredParameterSets else {
            throw DecoderError.missingParameterSets
        }
        var description: CMVideoFormatDescription?
        let formatStatus = Self.makeFormatDescription(codec: codec, parameterSets: parameterSets, output: &description)
        guard formatStatus == noErr, let description else {
            throw DecoderError.videoToolbox(formatStatus)
        }
        try createSession(description: description, output: output)
    }

    private func createSession(description: CMVideoFormatDescription, output: @escaping (CVPixelBuffer, UInt64) -> Void) throws {
        formatDescription = description

        var callback = VTDecompressionOutputCallbackRecord(
            decompressionOutputCallback: { refCon, _, status, _, imageBuffer, presentationTimeStamp, _ in
                guard let refCon else { return }
                let decoder = Unmanaged<BeamItVideoDecoder>.fromOpaque(refCon).takeUnretainedValue()
                decoder.timingLock.lock()
                let start = decoder.decodeStarts.removeValue(forKey: presentationTimeStamp.value)
                decoder.timingLock.unlock()
                guard status == noErr, let imageBuffer else {
                    decoder.onError(DecoderError.videoToolbox(status))
                    return
                }
                if let start {
                    decoder.diagnostics.decodedFrame(imageBuffer, milliseconds: (ProcessInfo.processInfo.systemUptime - start.start) * 1000)
                    decoder.output?(imageBuffer, start.captureUS)
                }
            },
            decompressionOutputRefCon: Unmanaged.passUnretained(self).toOpaque()
        )
        self.output = output
        // HDR keeps VideoToolbox's native 10-bit YCbCr output so PQ/HLG code values reach the display untouched.
        let pixelFormat: OSType = switch hdrChroma {
        case "4:4:4": kCVPixelFormatType_444YpCbCr10BiPlanarVideoRange
        case "4:2:2": kCVPixelFormatType_422YpCbCr10BiPlanarVideoRange
        case .some: kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange
        case nil: kCVPixelFormatType_32BGRA
        }
        let attributes: [NSString: Any] = [
            kCVPixelBufferPixelFormatTypeKey: pixelFormat,
            kCVPixelBufferWidthKey: width,
            kCVPixelBufferHeightKey: height,
            kCVPixelBufferIOSurfacePropertiesKey: [:] as [String: Any]
        ]
        let status = withUnsafeMutablePointer(to: &callback) { callbackPointer in
            VTDecompressionSessionCreate(
                allocator: kCFAllocatorDefault,
                formatDescription: description,
                decoderSpecification: nil,
                imageBufferAttributes: attributes as CFDictionary,
                outputCallback: callbackPointer,
                decompressionSessionOut: &session
            )
        }
        guard status == noErr else { throw DecoderError.videoToolbox(status) }
        if let session {
            let realtimeStatus = VTSessionSetProperty(
                session,
                key: kVTDecompressionPropertyKey_RealTime,
                value: kCFBooleanTrue
            )
            guard realtimeStatus == noErr else { throw DecoderError.videoToolbox(realtimeStatus) }
        }
    }

    private var output: ((CVPixelBuffer, UInt64) -> Void)?

    private func submit(_ data: Data, timestamp: UInt64) throws {
        guard let session, let formatDescription else { throw DecoderError.decoderUnavailable }
        // Idle-repeat pictures can share a host capture timestamp; use a unique local token
        // for callback timing instead of conflating those separate decode submissions.
        let decodeID = nextDecodeID
        nextDecodeID &+= 1
        var blockBuffer: CMBlockBuffer?
        let createBlockStatus = CMBlockBufferCreateWithMemoryBlock(
            allocator: kCFAllocatorDefault,
            memoryBlock: nil,
            blockLength: data.count,
            blockAllocator: kCFAllocatorDefault,
            customBlockSource: nil,
            offsetToData: 0,
            dataLength: data.count,
            flags: 0,
            blockBufferOut: &blockBuffer
        )
        guard createBlockStatus == kCMBlockBufferNoErr, let blockBuffer else {
            throw DecoderError.videoToolbox(createBlockStatus)
        }
        let copyStatus = data.withUnsafeBytes {
            CMBlockBufferReplaceDataBytes(
                with: $0.baseAddress!,
                blockBuffer: blockBuffer,
                offsetIntoDestination: 0,
                dataLength: data.count
            )
        }
        guard copyStatus == kCMBlockBufferNoErr else { throw DecoderError.videoToolbox(copyStatus) }

        var timing = CMSampleTimingInfo(
            duration: .invalid,
            presentationTimeStamp: CMTime(value: decodeID, timescale: 1_000_000),
            decodeTimeStamp: .invalid
        )
        var sampleSize = data.count
        var sampleBuffer: CMSampleBuffer?
        let sampleStatus = CMSampleBufferCreateReady(
            allocator: kCFAllocatorDefault,
            dataBuffer: blockBuffer,
            formatDescription: formatDescription,
            sampleCount: 1,
            sampleTimingEntryCount: 1,
            sampleTimingArray: &timing,
            sampleSizeEntryCount: 1,
            sampleSizeArray: &sampleSize,
            sampleBufferOut: &sampleBuffer
        )
        guard sampleStatus == noErr, let sampleBuffer else { throw DecoderError.videoToolbox(sampleStatus) }
        var infoFlags = VTDecodeInfoFlags()
        timingLock.lock()
        decodeStarts[decodeID] = (ProcessInfo.processInfo.systemUptime, timestamp)
        timingLock.unlock()
        let decodeStatus = VTDecompressionSessionDecodeFrame(
            session,
            sampleBuffer: sampleBuffer,
            flags: [._EnableAsynchronousDecompression],
            frameRefcon: nil,
            infoFlagsOut: &infoFlags
        )
        if decodeStatus != noErr || infoFlags.contains(.frameDropped) {
            timingLock.lock()
            decodeStarts.removeValue(forKey: decodeID)
            timingLock.unlock()
        }
        guard decodeStatus == noErr else { throw DecoderError.videoToolbox(decodeStatus) }
    }

    private static func makeFormatDescription(
        codec: String,
        parameterSets: [[UInt8]],
        output: inout CMVideoFormatDescription?
    ) -> OSStatus {
        var pointers: [UnsafePointer<UInt8>] = []
        let sizes = parameterSets.map(\.count)

        func withPointers<T>(_ index: Int, _ body: () -> T) -> T {
            guard index < parameterSets.count else { return body() }
            return parameterSets[index].withUnsafeBufferPointer { buffer in
                pointers.append(buffer.baseAddress!)
                defer { pointers.removeLast() }
                return withPointers(index + 1, body)
            }
        }

        return withPointers(0) {
            pointers.withUnsafeBufferPointer { pointerBuffer in
                sizes.withUnsafeBufferPointer { sizeBuffer in
                    if codec == "h264" {
                        return CMVideoFormatDescriptionCreateFromH264ParameterSets(
                            allocator: kCFAllocatorDefault,
                            parameterSetCount: parameterSets.count,
                            parameterSetPointers: pointerBuffer.baseAddress!,
                            parameterSetSizes: sizeBuffer.baseAddress!,
                            nalUnitHeaderLength: 4,
                            formatDescriptionOut: &output
                        )
                    }
                    return CMVideoFormatDescriptionCreateFromHEVCParameterSets(
                        allocator: kCFAllocatorDefault,
                        parameterSetCount: parameterSets.count,
                        parameterSetPointers: pointerBuffer.baseAddress!,
                        parameterSetSizes: sizeBuffer.baseAddress!,
                        nalUnitHeaderLength: 4,
                        extensions: nil,
                        formatDescriptionOut: &output
                    )
                }
            }
        }
    }

    private static func splitNALUnits(_ data: Data) -> [[UInt8]] {
        let bytes = [UInt8](data)
        var starts: [(offset: Int, length: Int)] = []
        var index = 0
        while index + 3 < bytes.count {
            if bytes[index] == 0, bytes[index + 1] == 0, bytes[index + 2] == 1 {
                starts.append((index, 3))
                index += 3
            } else if index + 4 <= bytes.count,
                      bytes[index] == 0, bytes[index + 1] == 0, bytes[index + 2] == 0, bytes[index + 3] == 1 {
                starts.append((index, 4))
                index += 4
            } else {
                index += 1
            }
        }

        return starts.enumerated().compactMap { position, start in
            let begin = start.offset + start.length
            let end = position + 1 < starts.count ? starts[position + 1].offset : bytes.count
            guard begin < end else { return nil }
            return Array(bytes[begin..<end])
        }
    }

    private static func parameterSets(in nalUnits: [[UInt8]], codec: String) -> [[UInt8]] {
        if codec == "h264" {
            let sps = nalUnits.first { $0[0] & 0x1F == 7 }
            let pps = nalUnits.first { $0[0] & 0x1F == 8 }
            return [sps, pps].compactMap { $0 }
        }
        let vps = nalUnits.first { ($0[0] >> 1) & 0x3F == 32 }
        let sps = nalUnits.first { ($0[0] >> 1) & 0x3F == 33 }
        let pps = nalUnits.first { ($0[0] >> 1) & 0x3F == 34 }
        return [vps, sps, pps].compactMap { $0 }
    }

    private static func isNonPictureNAL(_ nal: [UInt8], codec: String) -> Bool {
        if codec == "h264" {
            return [7, 8, 9].contains(Int(nal[0] & 0x1F))
        }
        return [32, 33, 34, 35].contains(Int((nal[0] >> 1) & 0x3F))
    }

    private static func lengthPrefixed(_ nalUnits: [[UInt8]]) -> Data {
        var data = Data()
        for nal in nalUnits {
            var length = UInt32(nal.count).bigEndian
            withUnsafeBytes(of: &length) { data.append(contentsOf: $0) }
            data.append(contentsOf: nal)
        }
        return data
    }

    private enum DecoderError: LocalizedError {
        case invalidAccessUnit
        case missingParameterSets
        case decoderUnavailable
        case videoToolbox(OSStatus)

        var errorDescription: String? {
            switch self {
            case .invalidAccessUnit:
                return "The host sent an invalid or empty encoded access unit."
            case .missingParameterSets:
                return "The stream's key frame did not include the codec parameter sets required by VideoToolbox."
            case .decoderUnavailable:
                return "The VideoToolbox decoder is not ready."
            case .videoToolbox(let status):
                return "VideoToolbox could not decode this stream (status \(status))."
            }
        }
    }
}

/// AV1CodecConfigurationRecord fields taken from the host's RFC 6381 `av01.P.LLT.DD[.M.CCC...]` string.
struct AV1Config {
    let profile: UInt8
    let level: UInt8
    let highTier: Bool
    let bitDepth: Int
    let monochrome: Bool
    let subsamplingX: Bool
    let subsamplingY: Bool

    init?(codecString: String?, chroma: String?) {
        guard let parts = codecString?.split(separator: ".").map(String.init),
              parts.count >= 4, parts[0] == "av01",
              let profile = UInt8(parts[1]), profile <= 2,
              parts[2].count == 3, let level = UInt8(parts[2].prefix(2)),
              let bitDepth = Int(parts[3]), [8, 10, 12].contains(bitDepth)
        else { return nil }
        self.profile = profile
        self.level = level
        highTier = parts[2].hasSuffix("H")
        self.bitDepth = bitDepth
        monochrome = chroma == "4:0:0"
        subsamplingX = chroma == "4:2:0" || chroma == "4:2:2" || chroma == nil
        subsamplingY = chroma == "4:2:0" || chroma == nil
    }

    func record(sequenceHeader: [UInt8]) -> Data {
        var bytes: [UInt8] = [
            0x81,
            profile << 5 | level,
            (highTier ? 0x80 : 0) | (bitDepth > 8 ? 0x40 : 0) | (bitDepth == 12 ? 0x20 : 0)
                | (monochrome ? 0x10 : 0) | (subsamplingX ? 0x08 : 0) | (subsamplingY ? 0x04 : 0),
            0
        ]
        bytes.append(contentsOf: sequenceHeader)
        return Data(bytes)
    }

    struct OBU {
        let type: Int
        let bytes: [UInt8]
    }

    /// Splits a low-overhead OBU stream; every OBU must carry `obu_has_size_field`.
    static func splitOBUs(_ data: [UInt8]) -> [OBU] {
        var obus: [OBU] = []
        var index = 0
        while index < data.count {
            let header = data[index]
            guard header & 0x80 == 0, header & 0x02 != 0 else { return [] }
            var cursor = index + 1 + Int((header >> 2) & 1)
            var size = 0
            var shift = 0
            while true {
                guard cursor < data.count, shift < 56 else { return [] }
                let byte = data[cursor]
                cursor += 1
                size |= Int(byte & 0x7F) << shift
                shift += 7
                if byte & 0x80 == 0 { break }
            }
            guard cursor + size <= data.count else { return [] }
            obus.append(OBU(type: Int((header >> 3) & 0x0F), bytes: Array(data[index..<cursor + size])))
            index = cursor + size
        }
        return obus
    }

    /// VideoToolbox AV1 decoding is hardware-only on Apple silicon with an AV1 block (M3/A17 Pro and later).
    static var isHardwareDecodeSupported: Bool { VTIsHardwareDecodeSupported(kCMVideoCodecType_AV1) }
}
