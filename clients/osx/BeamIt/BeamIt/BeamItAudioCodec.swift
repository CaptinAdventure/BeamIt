import Foundation
import Opus

final class BeamItOpusEncoder {
    private let encoder: OpaquePointer
    let configuration: BeamItOpusConfiguration

    init(configuration: BeamItOpusConfiguration) throws {
        try configuration.validate()
        var error: Int32 = 0
        guard let createdEncoder = opus_encoder_create(
            Int32(configuration.sampleRate),
            Int32(configuration.channels),
            OPUS_APPLICATION_VOIP,
            &error
        ), error == OPUS_OK else {
            throw BeamItAudioError.codecFailure(String(cString: opus_strerror(error)))
        }
        encoder = createdEncoder
        self.configuration = configuration
        for result in [
            beamit_opus_set_complexity(createdEncoder, 5),
            beamit_opus_set_fec(createdEncoder, configuration.useFec ? 1 : 0),
            beamit_opus_set_dtx(createdEncoder, 0)
        ] where result != OPUS_OK {
            opus_encoder_destroy(createdEncoder)
            throw BeamItAudioError.codecFailure(String(cString: opus_strerror(result)))
        }
    }

    deinit { opus_encoder_destroy(encoder) }

    func encode(_ pcm: [Float]) throws -> Data {
        guard pcm.count == configuration.sampleCount else { throw BeamItAudioError.invalidPacket }
        let normalizedPCM = pcm.map { value in
            value.isFinite ? min(max(value, -1), 1) : 0
        }
        var packet = [UInt8](repeating: 0, count: 1_275)
        let size = normalizedPCM.withUnsafeBufferPointer { samples in
            packet.withUnsafeMutableBufferPointer { bytes in
                guard let sampleBase = samples.baseAddress, let byteBase = bytes.baseAddress else {
                    return Int32(OPUS_BAD_ARG)
                }
                return opus_encode_float(
                    encoder,
                    sampleBase,
                    Int32(configuration.frameSize),
                    byteBase,
                    Int32(bytes.count)
                )
            }
        }
        guard size > 0 else {
            throw BeamItAudioError.codecFailure(String(cString: opus_strerror(size)))
        }
        return Data(packet.prefix(Int(size)))
    }
}

final class BeamItOpusDecoder {
    private let decoder: OpaquePointer
    let configuration: BeamItOpusConfiguration

    init(configuration: BeamItOpusConfiguration) throws {
        try configuration.validate()
        var error: Int32 = 0
        guard let decoder = opus_decoder_create(
            Int32(configuration.sampleRate),
            Int32(configuration.channels),
            &error
        ), error == OPUS_OK else {
            throw BeamItAudioError.codecFailure(String(cString: opus_strerror(error)))
        }
        self.decoder = decoder
        self.configuration = configuration
    }

    deinit { opus_decoder_destroy(decoder) }

    func decode(_ packet: Data, discontinuity: Bool) throws -> [Float] {
        guard !packet.isEmpty, packet.count <= 1_275 else { throw BeamItAudioError.invalidPacket }
        let packetFrames = packet.withUnsafeBytes { bytes -> Int32 in
            guard let baseAddress = bytes.baseAddress else { return Int32(OPUS_BAD_ARG) }
            return opus_packet_get_nb_samples(
                baseAddress.assumingMemoryBound(to: UInt8.self),
                Int32(packet.count),
                Int32(configuration.sampleRate)
            )
        }
        guard packetFrames == configuration.frameSize else {
            throw BeamItAudioError.codecFailure("The Opus packet does not match its advertised duration.")
        }
        if discontinuity, beamit_opus_reset_decoder(decoder) != OPUS_OK {
            throw BeamItAudioError.codecFailure("Could not reset the decoder.")
        }
        var pcm = [Float](repeating: 0, count: configuration.sampleCount)
        let decoded = packet.withUnsafeBytes { bytes in
            pcm.withUnsafeMutableBufferPointer { samples in
                guard let sampleBase = samples.baseAddress,
                      let byteBase = bytes.baseAddress?.assumingMemoryBound(to: UInt8.self) else {
                    return Int32(OPUS_BAD_ARG)
                }
                return opus_decode_float(
                    decoder,
                    byteBase,
                    Int32(packet.count),
                    sampleBase,
                    Int32(configuration.frameSize),
                    0
                )
            }
        }
        guard decoded == configuration.frameSize else {
            throw BeamItAudioError.codecFailure(decoded < 0
                ? String(cString: opus_strerror(decoded))
                : "The host packet does not match its advertised duration.")
        }
        return pcm
    }
}

enum BeamItAudioPacket {
    static let headerLength = 32
    static let maxPacketLength = headerLength + 1_275
    static let discontinuityFlag: UInt32 = 1 << 1

    static func encode(payload: Data, trackID: UInt32, sequence: UInt64, frameSize: UInt32,
                       discontinuity: Bool) throws -> Data {
        guard !payload.isEmpty, payload.count <= 1_275, trackID != 0, frameSize > 0,
              sequence <= UInt64.max / UInt64(frameSize) else {
            throw BeamItAudioError.invalidPacket
        }
        var packet = Data(capacity: headerLength + payload.count)
        packet.append(contentsOf: [2, 2, 0, UInt8(headerLength)])
        packet.appendUInt32BE(discontinuity ? discontinuityFlag : 0)
        packet.appendUInt32BE(frameSize)
        packet.appendUInt32BE(trackID)
        packet.appendUInt64BE(sequence)
        packet.appendUInt64BE(sequence * UInt64(frameSize))
        packet.append(payload)
        return packet
    }

    static func decode(_ packet: Data) throws -> Decoded {
        guard (headerLength + 1)...maxPacketLength ~= packet.count,
              packet[0] == 2, packet[1] == 2,
              packet.readUInt16BE(at: 2) == headerLength else {
            throw BeamItAudioError.invalidPacket
        }
        return Decoded(
            flags: packet.readUInt32BE(at: 4),
            frameSize: packet.readUInt32BE(at: 8),
            trackID: packet.readUInt32BE(at: 12),
            sequence: packet.readUInt64BE(at: 16),
            sampleTimestamp: packet.readUInt64BE(at: 24),
            payload: packet.subdata(in: headerLength..<packet.count)
        )
    }

    struct Decoded {
        let flags: UInt32
        let frameSize: UInt32
        let trackID: UInt32
        let sequence: UInt64
        let sampleTimestamp: UInt64
        let payload: Data
        var discontinuity: Bool { flags & discontinuityFlag != 0 }
    }
}

struct BeamItAudioSendQueue {
    private let packetDuration: Duration
    private var packets: [Data] = []
    private(set) var nextSendTime: ContinuousClock.Instant?

    init(configuration: BeamItOpusConfiguration) {
        packetDuration = .seconds(Double(configuration.frameSize) / Double(configuration.sampleRate))
    }

    var isEmpty: Bool { packets.isEmpty }

    mutating func append(_ packet: Data) {
        if packets.count >= 3 {
            packets.removeFirst()
            packets[0][7] |= UInt8(BeamItAudioPacket.discontinuityFlag)
        }
        packets.append(packet)
    }

    mutating func take(at now: ContinuousClock.Instant) -> Data? {
        guard !packets.isEmpty, nextSendTime.map({ now >= $0 }) ?? true else { return nil }
        // Rebase after a stall instead of sending a catch-up burst to the host.
        nextSendTime = now + packetDuration
        return packets.removeFirst()
    }
}

private extension Data {
    mutating func appendUInt32BE(_ value: UInt32) {
        append(UInt8(truncatingIfNeeded: value >> 24))
        append(UInt8(truncatingIfNeeded: value >> 16))
        append(UInt8(truncatingIfNeeded: value >> 8))
        append(UInt8(truncatingIfNeeded: value))
    }

    mutating func appendUInt64BE(_ value: UInt64) {
        appendUInt32BE(UInt32(truncatingIfNeeded: value >> 32))
        appendUInt32BE(UInt32(truncatingIfNeeded: value))
    }

    func readUInt16BE(at offset: Int) -> UInt16 {
        UInt16(self[offset]) << 8 | UInt16(self[offset + 1])
    }

    func readUInt32BE(at offset: Int) -> UInt32 {
        UInt32(self[offset]) << 24 | UInt32(self[offset + 1]) << 16 |
        UInt32(self[offset + 2]) << 8 | UInt32(self[offset + 3])
    }

    func readUInt64BE(at offset: Int) -> UInt64 {
        UInt64(readUInt32BE(at: offset)) << 32 | UInt64(readUInt32BE(at: offset + 4))
    }
}
