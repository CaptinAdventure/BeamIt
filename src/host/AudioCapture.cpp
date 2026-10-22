#include "host/AudioCapture.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace beamit::host {

AudioPacketizer::AudioPacketizer(media::AudioConfiguration configuration, Submit submit)
    : configuration_(configuration), submit_(std::move(submit)), packet_(configuration.sample_count()) {}

void AudioPacketizer::reset_if_discontinuous(bool discontinuity) {
    if (!discontinuity) return;
    filled_ = 0;
    discontinuity_ = true;
}

void AudioPacketizer::flush() {
    if (filled_ != packet_.size()) return;
    const auto result = submit_(packet_, discontinuity_);
    filled_ = 0;
    if (result == AudioSubmitResult::NotConnected) throw std::runtime_error("Audio capture track detached");
    discontinuity_ = result == AudioSubmitResult::QueueFull;
    if (discontinuity_) spdlog::warn("Audio capture encode queue full; dropping packet");
}

void AudioPacketizer::append(std::span<const float> samples, bool discontinuity) {
    if (samples.size() % configuration_.channels != 0) throw std::invalid_argument("Unaligned audio capture chunk");
    reset_if_discontinuous(discontinuity);
    for (const auto sample : samples) {
        if (!std::isfinite(sample)) throw std::runtime_error("Non-finite WASAPI capture sample");
        packet_[filled_++] = std::clamp(sample, -1.0f, 1.0f);
        flush();
    }
}

void AudioPacketizer::silence(std::size_t frames, bool discontinuity) {
    reset_if_discontinuous(discontinuity);
    while (frames) {
        const auto count = std::min(frames, (packet_.size() - filled_) / configuration_.channels);
        const auto samples = count * configuration_.channels;
        std::fill_n(packet_.begin() + filled_, samples, 0.0f);
        filled_ += samples;
        frames -= count;
        flush();
    }
}

} // namespace beamit::host
