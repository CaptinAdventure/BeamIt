import Foundation

@main
struct AudioProtocolTests {
    static func main() throws {
        let configuration = BeamItOpusConfiguration(
            sampleRate: 48_000,
            channels: 2,
            frameSize: 480,
            packetDuration: "PT0.01S",
            useFec: false,
            application: "VOIP"
        )
        try configuration.validate()
        precondition(configuration.sampleCount == 960)
        let offerData = Data(
            """
            {
              "audio_id": "fP8Yq4mA2sL9",
              "url": "wss://host:17389/audio/fP8Yq4mA2sL9?token=abc",
              "token_expires_in_ms": 10000,
              "audio_version": 2,
              "hostToClient": { "tracks": [] },
              "clientToHost": {
                "track_id": 1,
                "source": { "kind": "microphone" },
                "codec": "opus",
                "opusAudioConfiguration": {
                  "sampleRate": 48000, "channels": 2, "frameSize": 480,
                  "packetDuration": "PT0.01S", "useFec": false, "application": "VOIP"
                }
              }
            }
            """.utf8
        )
        let offer = try JSONDecoder().decode(BeamItAudioOffer.self, from: offerData)
        precondition(offer.audioID == "fP8Yq4mA2sL9")
        precondition(offer.microphone.trackID == 1)
        precondition(offer.microphone.opus == configuration)

        let encoder = try BeamItOpusEncoder(configuration: configuration)
        let decoder = try BeamItOpusDecoder(configuration: configuration)
        let pcm = (0..<configuration.sampleCount).map { index in
            Float(sin(Double(index) * 2 * .pi * 440 / Double(configuration.sampleRate)) * 0.2)
        }
        let payload = try encoder.encode(pcm)
        let decoded = try decoder.decode(payload, discontinuity: true)
        precondition(decoded.count == configuration.sampleCount)

        let packet = try BeamItAudioPacket.encode(
            payload: payload,
            trackID: 7,
            sequence: 12,
            frameSize: UInt32(configuration.frameSize),
            discontinuity: true
        )
        let frame = try BeamItAudioPacket.decode(packet)
        precondition(frame.trackID == 7)
        precondition(frame.sequence == 12)
        precondition(frame.sampleTimestamp == 5_760)
        precondition(frame.discontinuity)
        precondition(frame.payload == payload)

        let start = ContinuousClock.now
        var queue = BeamItAudioSendQueue(configuration: configuration)
        for sequence in 0..<10 {
            queue.append(try BeamItAudioPacket.encode(
                payload: payload, trackID: 1, sequence: UInt64(sequence),
                frameSize: UInt32(configuration.frameSize), discontinuity: sequence == 0
            ))
        }
        let first = try BeamItAudioPacket.decode(queue.take(at: start)!)
        precondition(first.sequence == 7)
        precondition(first.discontinuity)
        precondition(queue.take(at: start) == nil)
        precondition(queue.take(at: start + .milliseconds(9)) == nil)
        let second = try BeamItAudioPacket.decode(queue.take(at: start + .milliseconds(10))!)
        precondition(second.sequence == 8)
        precondition(queue.take(at: start + .milliseconds(10)) == nil)
        // A delayed sender must not catch up by draining multiple packets at once.
        let delayed = try BeamItAudioPacket.decode(queue.take(at: start + .seconds(1))!)
        precondition(delayed.sequence == 9)
        queue.append(packet)
        precondition(queue.take(at: start + .seconds(1)) == nil)
        precondition(queue.take(at: start + .seconds(1) + .milliseconds(10)) == packet)
        precondition(queue.isEmpty)
        precondition(queue.take(at: start + .seconds(2)) == nil)
        queue.append(packet)
        precondition(queue.take(at: start + .seconds(2)) == packet)

        let longerConfiguration = BeamItOpusConfiguration(
            sampleRate: 48_000,
            channels: 2,
            frameSize: 960,
            packetDuration: "PT0.02S",
            useFec: false,
            application: "VOIP"
        )
        try longerConfiguration.validate()
        var longerQueue = BeamItAudioSendQueue(configuration: longerConfiguration)
        longerQueue.append(packet)
        longerQueue.append(packet)
        precondition(longerQueue.take(at: start) == packet)
        precondition(longerQueue.take(at: start + .milliseconds(10)) == nil)
        precondition(longerQueue.take(at: start + .milliseconds(20)) == packet)

        var malformed = packet
        malformed[0] = 1
        do {
            _ = try BeamItAudioPacket.decode(malformed)
            preconditionFailure("Version 1 packets must be rejected.")
        } catch BeamItAudioError.invalidPacket {}

        let unsupported = BeamItOpusConfiguration(
            sampleRate: 44_100,
            channels: 2,
            frameSize: 441,
            packetDuration: "PT0.01S",
            useFec: false,
            application: "VOIP"
        )
        do {
            try unsupported.validate()
            preconditionFailure("Unsupported Opus sample rates must be rejected.")
        } catch BeamItAudioError.unsupportedConfiguration {}

        print("Audio configuration, Opus, v2 framing, and bounded send pacing checks passed.")
    }
}
