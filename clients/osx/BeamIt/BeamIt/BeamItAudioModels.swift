import Foundation

struct BeamItOpusConfiguration: Decodable, Equatable {
    let sampleRate: Int
    let channels: Int
    let frameSize: Int
    let packetDuration: String
    let useFec: Bool
    let application: String

    var sampleCount: Int { frameSize * channels }

    func validate() throws {
        let duration: Double?
        if let expression = try? NSRegularExpression(
            pattern: #"^PT(?:([0-9]+(?:\.[0-9]+)?)H)?(?:([0-9]+(?:\.[0-9]+)?)M)?(?:([0-9]+(?:\.[0-9]+)?)S)?$"#
        ), let match = expression.firstMatch(
            in: packetDuration,
            range: NSRange(packetDuration.startIndex..., in: packetDuration)
        ) {
            func number(_ index: Int) -> Double {
                guard let range = Range(match.range(at: index), in: packetDuration) else { return 0 }
                return Double(packetDuration[range]) ?? 0
            }
            let seconds = number(1) * 3_600 + number(2) * 60 + number(3)
            duration = seconds > 0 ? seconds : nil
        } else {
            duration = nil
        }
        let supportedDurationsUS: Set<Int> = [2_500, 5_000, 10_000, 20_000, 40_000, 60_000]
        guard [8_000, 12_000, 16_000, 24_000, 48_000].contains(sampleRate),
              (1...2).contains(channels),
              (1...(sampleRate * 60 / 1_000)).contains(frameSize),
              let duration,
              supportedDurationsUS.contains(Int((duration * 1_000_000).rounded())),
              abs(Double(frameSize) / Double(sampleRate) - duration) < 0.000_001,
              application == "VOIP"
        else {
            throw BeamItAudioError.unsupportedConfiguration
        }
    }
}

struct BeamItAudioOffer: Decodable {
    let audioID: String
    let url: URL
    let tokenExpiresInMS: Int
    let audioVersion: Int
    let microphone: BeamItAudioTrackConfiguration

    enum CodingKeys: String, CodingKey {
        case audioID = "audio_id"
        case url
        case tokenExpiresInMS = "token_expires_in_ms"
        case audioVersion = "audio_version"
        case clientToHost
    }

    enum ClientToHostKeys: String, CodingKey {
        case trackID = "track_id"
        case codec
        case source
        case opusAudioConfiguration
    }

    init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        audioID = try values.decode(String.self, forKey: .audioID)
        url = try values.decode(URL.self, forKey: .url)
        tokenExpiresInMS = try values.decode(Int.self, forKey: .tokenExpiresInMS)
        audioVersion = try values.decode(Int.self, forKey: .audioVersion)
        let microphone = try values.nestedContainer(keyedBy: ClientToHostKeys.self, forKey: .clientToHost)
        let source = try microphone.nestedContainer(keyedBy: AudioSourceKeys.self, forKey: .source)
        guard try source.decode(String.self, forKey: .kind) == "microphone",
              try microphone.decode(String.self, forKey: .codec) == "opus" else {
            throw BeamItAudioError.unsupportedConfiguration
        }
        self.microphone = BeamItAudioTrackConfiguration(
            trackID: try microphone.decode(Int.self, forKey: .trackID),
            codec: try microphone.decode(String.self, forKey: .codec),
            opus: try microphone.decode(BeamItOpusConfiguration.self, forKey: .opusAudioConfiguration)
        )
    }

    private enum AudioSourceKeys: String, CodingKey {
        case kind
    }
}

struct BeamItAudioTrackConfiguration: Decodable, Equatable {
    let trackID: Int
    let codec: String
    let opus: BeamItOpusConfiguration

    enum CodingKeys: String, CodingKey {
        case trackID = "track_id"
        case codec
        case opus = "opusAudioConfiguration"
    }
}

enum BeamItAudioSource: Hashable {
    case desktop
    case application(pid: Int, title: String)

    var fields: [String: Any] {
        switch self {
        case .desktop:
            ["kind": "desktop"]
        case .application(let pid, _):
            ["kind": "application", "pid": pid]
        }
    }

    var title: String {
        switch self {
        case .desktop:
            "Desktop audio"
        case .application(_, let title):
            title
        }
    }

    var pid: Int? {
        if case .application(let pid, _) = self { return pid }
        return nil
    }
}

enum BeamItAudioError: LocalizedError {
    case unsupportedConfiguration
    case invalidPacket
    case microphonePermissionDenied
    case codecFailure(String)
    case deviceFailure(String)

    var errorDescription: String? {
        switch self {
        case .unsupportedConfiguration:
            "The host offered an unsupported Opus audio configuration."
        case .invalidPacket:
            "The host sent an invalid audio packet."
        case .microphonePermissionDenied:
            "Microphone access is required for two-way audio. Allow it in System Settings > Privacy & Security > Microphone."
        case .codecFailure(let message):
            "Opus audio failed: \(message)"
        case .deviceFailure(let message):
            "Audio device failed: \(message)"
        }
    }
}
