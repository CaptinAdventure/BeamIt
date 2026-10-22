#include "beamit/media/OpusAudio.hpp"

#include <opus.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace beamit::media {
namespace {

void check_opus(int result, const char* operation) {
    if (result < 0) throw std::runtime_error(std::string(operation) + ": " + opus_strerror(result));
}

} // namespace

void AudioConfiguration::validate() const {
    constexpr std::array rates{8000u, 12000u, 16000u, 24000u, 48000u};
    constexpr std::array durations{2500LL, 5000LL, 10000LL, 20000LL, 40000LL, 60000LL};
    if (std::ranges::find(rates, sample_rate) == rates.end() || (channels != 1 && channels != 2) ||
        std::ranges::find(durations, packet_duration.count()) == durations.end() ||
        (static_cast<std::uint64_t>(sample_rate) * packet_duration.count()) % 1'000'000 != 0) {
        throw std::invalid_argument("Unsupported Opus sample rate, channels or packet duration");
    }
}

std::uint32_t AudioConfiguration::frame_size() const {
    validate();
    return static_cast<std::uint32_t>(sample_rate * static_cast<std::uint64_t>(packet_duration.count()) / 1'000'000);
}

std::size_t AudioConfiguration::sample_count() const { return frame_size() * channels; }

std::string AudioConfiguration::duration_text() const {
    validate();
    auto fraction = std::to_string(1'000'000 + packet_duration.count()).substr(1);
    while (fraction.back() == '0') fraction.pop_back();
    return "PT0." + fraction + "S";
}

struct OpusAudio::Impl {
    std::unique_ptr<OpusEncoder, decltype(&opus_encoder_destroy)> encoder{nullptr, opus_encoder_destroy};
    std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)> decoder{nullptr, opus_decoder_destroy};
};

OpusAudio::OpusAudio(AudioConfiguration configuration)
    : configuration_(configuration), impl_(std::make_unique<Impl>()) {
    configuration_.validate();
    int error{};
    impl_->encoder.reset(opus_encoder_create(configuration.sample_rate, configuration.channels,
                                             OPUS_APPLICATION_VOIP, &error));
    check_opus(error, "Create Opus encoder");
    impl_->decoder.reset(opus_decoder_create(configuration.sample_rate, configuration.channels, &error));
    check_opus(error, "Create Opus decoder");
    check_opus(opus_encoder_ctl(impl_->encoder.get(), OPUS_SET_INBAND_FEC(audio_use_fec)), "Set Opus FEC");
    check_opus(opus_encoder_ctl(impl_->encoder.get(), OPUS_SET_DTX(0)), "Disable Opus DTX");
    check_opus(opus_encoder_ctl(impl_->encoder.get(), OPUS_SET_COMPLEXITY(audio_encoder_complexity)),
               "Set Opus complexity");
}

OpusAudio::~OpusAudio() = default;

std::vector<std::byte> OpusAudio::encode(std::span<const float> pcm) {
    if (pcm.size() != configuration_.sample_count() ||
        !std::ranges::all_of(pcm, [](float value) { return std::isfinite(value) && std::abs(value) <= 1.0f; })) {
        throw std::invalid_argument("Audio PCM must contain one packet of finite, normalized interleaved samples");
    }
    std::vector<std::byte> packet(max_opus_packet_bytes);
    const auto size = opus_encode_float(impl_->encoder.get(), pcm.data(), configuration_.frame_size(),
                                       reinterpret_cast<unsigned char*>(packet.data()),
                                       static_cast<opus_int32>(packet.size()));
    check_opus(size, "Encode Opus");
    packet.resize(size);
    return packet;
}

std::vector<float> OpusAudio::decode(std::span<const std::byte> packet, bool discontinuity) {
    if (packet.empty() || packet.size() > max_opus_packet_bytes) {
        throw std::invalid_argument("Invalid Opus packet size");
    }
    const auto* bytes = reinterpret_cast<const unsigned char*>(packet.data());
    const auto size = static_cast<opus_int32>(packet.size());
    const auto samples = opus_packet_get_nb_samples(bytes, size, configuration_.sample_rate);
    check_opus(samples, "Inspect Opus packet");
    if (samples != configuration_.frame_size()) {
        throw std::invalid_argument("Opus packet does not match the advertised audio configuration");
    }
    if (discontinuity) check_opus(opus_decoder_ctl(impl_->decoder.get(), OPUS_RESET_STATE), "Reset Opus decoder");
    std::vector<float> pcm(configuration_.sample_count());
    const auto decoded = opus_decode_float(impl_->decoder.get(), bytes, size, pcm.data(), configuration_.frame_size(), 0);
    check_opus(decoded, "Decode Opus");
    if (decoded != configuration_.frame_size()) throw std::runtime_error("Unexpected decoded Opus frame size");
    return pcm;
}

} // namespace beamit::media
