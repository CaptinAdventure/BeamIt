import CoreMedia
import CoreVideo
import Foundation
import VideoToolbox

final class BeamItVideoDecoder {
    let generation: Int

    private let codec: String
    private let width: Int
    private let height: Int
    private let onError: (Error) -> Void
    private var formatDescription: CMVideoFormatDescription?
    private var session: VTDecompressionSession?
    private let diagnostics: BeamItStreamDiagnostics
    private let timingLock = NSLock()
    private var decodeStarts: [Int64: (start: TimeInterval, captureUS: UInt64)] = [:]
    private var nextDecodeID: Int64 = 0

    init(codec: String, width: Int, height: Int, generation: Int, diagnostics: BeamItStreamDiagnostics, onError: @escaping (Error) -> Void) throws {
        self.diagnostics = diagnostics
        self.codec = codec
        self.width = width
        self.height = height
        self.generation = generation
        self.onError = onError
    }

    deinit {
        if let session {
            VTDecompressionSessionInvalidate(session)
        }
    }

    func decode(_ accessUnit: Data, timestamp: UInt64, output: @escaping (CVPixelBuffer, UInt64) -> Void) throws {
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
        let attributes: [NSString: Any] = [
            kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_32BGRA,
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
