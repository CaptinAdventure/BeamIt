#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace beamit::media {

inline constexpr std::uint32_t audio_sample_rate_hz = 48'000;
inline constexpr auto audio_packet_duration = std::chrono::milliseconds(10);
inline constexpr unsigned audio_channels = 2;
inline constexpr bool audio_use_fec = false;
inline constexpr std::string_view audio_application = "VOIP";
inline constexpr int audio_encoder_complexity = 5;
inline constexpr std::size_t max_opus_packet_bytes = 1275;

struct AudioConfiguration {
    std::uint32_t sample_rate{audio_sample_rate_hz};
    std::chrono::microseconds packet_duration{audio_packet_duration};
    unsigned channels{audio_channels};

    void validate() const;
    [[nodiscard]] std::uint32_t frame_size() const;
    [[nodiscard]] std::size_t sample_count() const;
    [[nodiscard]] std::string duration_text() const;
};

// One encoder and decoder, owned and used by one audio worker.
class OpusAudio {
public:
    explicit OpusAudio(AudioConfiguration configuration = {});
    ~OpusAudio();
    OpusAudio(const OpusAudio&) = delete;
    OpusAudio& operator=(const OpusAudio&) = delete;

    [[nodiscard]] std::vector<std::byte> encode(std::span<const float> interleaved);
    [[nodiscard]] std::vector<float> decode(std::span<const std::byte> packet, bool discontinuity = false);
    [[nodiscard]] const AudioConfiguration& configuration() const noexcept { return configuration_; }

private:
    struct Impl;
    AudioConfiguration configuration_;
    std::unique_ptr<Impl> impl_;
};

} // namespace beamit::media
