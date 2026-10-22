#pragma once

#include "net/Protocol.hpp"
#include "beamit/media/OpusAudio.hpp"

#include <optional>
#include <algorithm>
#include <span>
#include <stdexcept>
#include <vector>

namespace beamit::net::protocol {

inline constexpr std::uint8_t message_audio = 2;
inline constexpr std::uint8_t audio_frame_version = 2;
inline constexpr std::uint32_t microphone_track_id = 1;
inline constexpr std::size_t max_audio_capture_tracks = 8;
inline constexpr std::size_t audio_header_size = 32;
inline constexpr std::size_t max_audio_message = audio_header_size + media::max_opus_packet_bytes;

struct AudioPacket {
    std::uint32_t track_id{microphone_track_id};
    std::uint64_t sequence{};
    std::uint64_t sample_timestamp{};
    bool discontinuity{};
    std::vector<std::byte> payload;
};

[[nodiscard]] inline std::vector<std::byte> encode_audio_packet(const AudioPacket& packet,
                                                               const media::AudioConfiguration& configuration) {
    std::vector<std::byte> bytes(audio_header_size + packet.payload.size());
    const auto put = [&](std::size_t offset, std::uint64_t value, std::size_t width) {
        for (std::size_t index = 0; index < width; ++index) {
            bytes[offset + index] = static_cast<std::byte>((value >> (8 * (width - 1 - index))) & 0xff);
        }
    };
    put(0, audio_frame_version, 1);
    put(1, message_audio, 1);
    put(2, audio_header_size, 2);
    put(4, packet.discontinuity ? flag_discontinuity : 0, 4);
    put(8, configuration.frame_size(), 4);
    put(12, packet.track_id, 4);
    put(16, packet.sequence, 8);
    put(24, packet.sample_timestamp, 8);
    std::copy(packet.payload.begin(), packet.payload.end(), bytes.begin() + audio_header_size);
    return bytes;
}

[[nodiscard]] inline AudioPacket decode_audio_packet(std::span<const std::byte> bytes,
                                                     const media::AudioConfiguration& configuration,
                                                     std::optional<std::uint64_t> previous_sequence) {
    if (bytes.size() <= audio_header_size || bytes.size() > max_audio_message) {
        throw std::invalid_argument("Invalid audio message length");
    }
    const auto get = [&](std::size_t offset, std::size_t width) {
        std::uint64_t value{};
        for (std::size_t index = 0; index < width; ++index) {
            value = (value << 8) | std::to_integer<unsigned>(bytes[offset + index]);
        }
        return value;
    };
    if (get(0, 1) != audio_frame_version || get(1, 1) != message_audio || get(2, 2) != audio_header_size ||
        get(8, 4) != configuration.frame_size() || get(12, 4) == 0) {
        throw std::invalid_argument("Invalid audio header or frame size");
    }
    AudioPacket packet{.track_id = static_cast<std::uint32_t>(get(12, 4)),
                       .sequence = get(16, 8), .sample_timestamp = get(24, 8),
                       .discontinuity = (get(4, 4) & flag_discontinuity) != 0,
                       .payload = {bytes.begin() + audio_header_size, bytes.end()}};
    const auto span = configuration.frame_size();
    if (packet.sequence > UINT64_MAX / span || packet.sample_timestamp != packet.sequence * span ||
        (!previous_sequence && !packet.discontinuity) ||
        (previous_sequence && (packet.sequence <= *previous_sequence ||
          (packet.sequence - *previous_sequence != 1 && !packet.discontinuity)))) {
        throw std::invalid_argument("Invalid audio sequence, timestamp or discontinuity");
    }
    return packet;
}

} // namespace beamit::net::protocol
