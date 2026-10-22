#pragma once

#include "beamit/media/OpusAudio.hpp"
#include "net/AudioProtocol.hpp"

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <map>
#include <optional>
#include <thread>

namespace beamit::host {

inline constexpr std::size_t max_queued_audio_packets = 3;

struct AudioPcmPacket {
    std::uint64_t sequence{};
    std::uint64_t sample_timestamp{};
    bool discontinuity{};
    std::vector<float> samples;
};

enum class AudioSubmitResult { Accepted, NotConnected, QueueFull };

struct AudioTrack {
    std::uint32_t id{};
    enum class Kind { Microphone, Desktop, Application } kind{Kind::Microphone};
    std::uint32_t pid{};
    media::AudioConfiguration configuration;
};

// Thread-safe PCM boundary for future device capture/playback. One endpoint per host instance.
class AudioPipeline;
class AudioEndpoint {
public:
    [[nodiscard]] AudioSubmitResult submit_pcm(std::uint32_t track_id, std::span<const float> samples);
    [[nodiscard]] std::optional<AudioPcmPacket> receive_pcm();
    [[nodiscard]] bool connected() const;

    [[nodiscard]] std::vector<AudioTrack> tracks() const;
    void attach(AudioTrack track, const std::shared_ptr<AudioPipeline>& pipeline);
    void detach(std::uint32_t track_id, const std::shared_ptr<AudioPipeline>& pipeline);

private:
    mutable std::mutex mutex_;
    struct Entry {
        AudioTrack track;
        std::weak_ptr<AudioPipeline> pipeline;
    };
    std::map<std::uint32_t, Entry> tracks_;
};

// Codec work never executes on the WSS thread. All queues are bounded.
class AudioPipeline {
public:
    AudioPipeline(media::AudioConfiguration configuration, std::function<void()> notify_output);
    ~AudioPipeline();
    void stop();
    [[nodiscard]] AudioSubmitResult submit_pcm(std::span<const float> samples);
    [[nodiscard]] bool submit_encoded(net::protocol::AudioPacket packet);
    [[nodiscard]] std::optional<net::protocol::AudioPacket> receive_encoded();
    [[nodiscard]] std::optional<AudioPcmPacket> receive_pcm();
    [[nodiscard]] std::string error() const;
    [[nodiscard]] bool running() const;
    [[nodiscard]] const media::AudioConfiguration& configuration() const { return codec_.configuration(); }

private:
    void run();
    media::OpusAudio codec_;
    std::function<void()> notify_output_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::vector<float>> encode_queue_;
    std::deque<net::protocol::AudioPacket> decode_queue_;
    std::deque<net::protocol::AudioPacket> encoded_;
    std::deque<AudioPcmPacket> decoded_;
    bool stopping_{};
    std::string error_;
    std::uint64_t sequence_{};
    std::jthread worker_;
};

} // namespace beamit::host
