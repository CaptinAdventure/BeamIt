#include "host/AudioEndpoint.hpp"
#include "ThreadPriority.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace beamit::host {

AudioSubmitResult AudioEndpoint::submit_pcm(std::uint32_t track_id, std::span<const float> samples, bool discontinuity) {
    std::lock_guard lock(mutex_);
    const auto found = tracks_.find(track_id);
    if (found == tracks_.end()) return AudioSubmitResult::NotConnected;
    if (found->second.track.kind == AudioTrack::Kind::Microphone) {
        throw std::invalid_argument("Cannot submit host PCM to the client microphone track");
    }
    const auto pipeline = found->second.pipeline.lock();
    return pipeline ? pipeline->submit_pcm(samples, discontinuity) : AudioSubmitResult::NotConnected;
}

std::optional<AudioPcmPacket> AudioEndpoint::receive_pcm() {
    std::lock_guard lock(mutex_);
    const auto found = tracks_.find(net::protocol::microphone_track_id);
    if (found == tracks_.end()) return std::nullopt;
    const auto pipeline = found->second.pipeline.lock();
    return pipeline ? pipeline->receive_pcm() : std::nullopt;
}

bool AudioEndpoint::connected() const {
    std::lock_guard lock(mutex_);
    const auto found = tracks_.find(net::protocol::microphone_track_id);
    if (found == tracks_.end()) return false;
    const auto pipeline = found->second.pipeline.lock();
    return pipeline && pipeline->running();
}

std::vector<AudioTrack> AudioEndpoint::tracks() const {
    std::lock_guard lock(mutex_);
    std::vector<AudioTrack> result;
    for (const auto& [id, entry] : tracks_) {
        if (auto pipeline = entry.pipeline.lock(); pipeline && pipeline->running()) result.push_back(entry.track);
    }
    return result;
}

void AudioEndpoint::attach(AudioTrack track, const std::shared_ptr<AudioPipeline>& pipeline) {
    std::lock_guard lock(mutex_);
    if (!pipeline || track.id == 0 ||
        ((track.id == net::protocol::microphone_track_id) != (track.kind == AudioTrack::Kind::Microphone))) {
        throw std::invalid_argument("Invalid audio track direction or ID");
    }
    if (const auto found = tracks_.find(track.id); found != tracks_.end() && !found->second.pipeline.expired()) {
        throw std::logic_error("Audio track already attached");
    }
    tracks_[track.id] = {track, pipeline};
}

void AudioEndpoint::detach(std::uint32_t track_id, const std::shared_ptr<AudioPipeline>& pipeline) {
    std::lock_guard lock(mutex_);
    const auto found = tracks_.find(track_id);
    if (found != tracks_.end() && found->second.pipeline.lock() == pipeline) tracks_.erase(found);
}

AudioPipeline::AudioPipeline(media::AudioConfiguration configuration, std::function<void()> notify_output)
    : codec_(configuration), notify_output_(std::move(notify_output)), worker_([this] { run(); }) {}

AudioPipeline::~AudioPipeline() { stop(); }

void AudioPipeline::stop() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_one();
    if (worker_.joinable()) worker_.join();
    std::lock_guard lock(mutex_);
    encode_queue_.clear();
    decode_queue_.clear();
    encoded_.clear();
    decoded_.clear();
}

bool AudioPipeline::running() const {
    std::lock_guard lock(mutex_);
    return !stopping_;
}

AudioSubmitResult AudioPipeline::submit_pcm(std::span<const float> samples, bool discontinuity) {
    if (samples.size() != configuration().sample_count() ||
        !std::ranges::all_of(samples, [](float value) { return std::isfinite(value) && std::abs(value) <= 1.0f; })) {
        throw std::invalid_argument("PCM submission must contain exactly one normalized audio packet");
    }
    std::lock_guard lock(mutex_);
    if (stopping_) return AudioSubmitResult::NotConnected;
    if (encode_queue_.size() >= max_queued_audio_packets) return AudioSubmitResult::QueueFull;
    encode_queue_.push_back({{samples.begin(), samples.end()}, discontinuity});
    wake_.notify_one();
    return AudioSubmitResult::Accepted;
}

bool AudioPipeline::submit_encoded(net::protocol::AudioPacket packet) {
    std::lock_guard lock(mutex_);
    if (stopping_ || decode_queue_.size() >= max_queued_audio_packets) return false;
    decode_queue_.push_back(std::move(packet));
    wake_.notify_one();
    return true;
}

std::optional<net::protocol::AudioPacket> AudioPipeline::receive_encoded() {
    std::lock_guard lock(mutex_);
    if (encoded_.empty()) return std::nullopt;
    auto packet = std::move(encoded_.front());
    encoded_.pop_front();
    return packet;
}

std::optional<AudioPcmPacket> AudioPipeline::receive_pcm() {
    std::lock_guard lock(mutex_);
    if (decoded_.empty()) return std::nullopt;
    auto packet = std::move(decoded_.front());
    decoded_.pop_front();
    return packet;
}

std::string AudioPipeline::error() const {
    std::lock_guard lock(mutex_);
    return error_;
}

void AudioPipeline::run() {
    try {
        const MmcssTaskGuard mmcss(set_realtime_priority_with_handle());
        if (!mmcss.registered()) spdlog::warn("Audio MMCSS registration failed; using thread-priority fallback");
        while (true) {
            EncodeInput pcm;
            std::optional<net::protocol::AudioPacket> incoming;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [&] { return stopping_ || !encode_queue_.empty() || !decode_queue_.empty(); });
                if (stopping_) return;
                if (!encode_queue_.empty()) {
                    pcm = std::move(encode_queue_.front());
                    encode_queue_.pop_front();
                }
                if (!decode_queue_.empty()) {
                    incoming = std::move(decode_queue_.front());
                    decode_queue_.pop_front();
                }
            }
            if (!pcm.samples.empty()) {
                net::protocol::AudioPacket packet{.sequence = sequence_,
                    .sample_timestamp = sequence_ * configuration().frame_size(),
                    .discontinuity = sequence_ == 0 || pcm.discontinuity,
                    .payload = codec_.encode(pcm.samples)};
                ++sequence_;
                bool notify{};
                {
                    std::lock_guard lock(mutex_);
                    notify = encoded_.empty();
                    if (encoded_.size() >= max_queued_audio_packets) {
                        encoded_.pop_front();
                        spdlog::warn("Audio output queue full; dropping oldest packet");
                        if (!encoded_.empty()) encoded_.front().discontinuity = true;
                    }
                    encoded_.push_back(std::move(packet));
                }
                if (notify) notify_output_();
            }
            if (incoming) {
                AudioPcmPacket packet{incoming->sequence, incoming->sample_timestamp, incoming->discontinuity,
                                      codec_.decode(incoming->payload, incoming->discontinuity)};
                std::lock_guard lock(mutex_);
                if (decoded_.size() >= max_queued_audio_packets) {
                    decoded_.pop_front();
                    spdlog::warn("Audio PCM queue full; dropping oldest packet");
                    if (!decoded_.empty()) decoded_.front().discontinuity = true;
                }
                decoded_.push_back(std::move(packet));
            }
        }
    } catch (const std::exception& error) {
        {
            std::lock_guard lock(mutex_);
            error_ = error.what();
            stopping_ = true;
        }
        spdlog::error("Audio codec failed: {}", error.what());
        notify_output_();
    }
}

} // namespace beamit::host
