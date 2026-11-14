#pragma once

#include "ClientProtocol.hpp"

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace beamit::client {

class AudioClient {
public:
    using SendPacket = std::function<void(std::vector<std::byte>)>;
    using Error = std::function<void(std::string)>;

    AudioClient(AudioTrackConfiguration microphone, SendPacket send_packet, Error error);
    ~AudioClient();
    AudioClient(const AudioClient&) = delete;
    AudioClient& operator=(const AudioClient&) = delete;

    void start();
    void add_track(AudioTrackConfiguration track);
    void remove_track(std::uint32_t track_id);
    void receive(const net::protocol::AudioPacket& packet, const media::AudioConfiguration& configuration);
    void stop() noexcept;

private:
    struct Track {
        explicit Track(media::AudioConfiguration configuration) : configuration(configuration), decoder(configuration) {}
        media::AudioConfiguration configuration;
        media::OpusAudio decoder;
        std::deque<std::vector<float>> queued;
        std::optional<std::vector<float>> current;
        std::size_t offset{};
    };

    void capture_loop();
    void render_loop();
    void report_error(std::string message);
    [[nodiscard]] bool stopping() const noexcept { return !running_.load(); }

    AudioTrackConfiguration microphone_;
    SendPacket send_packet_;
    Error error_;
    media::OpusAudio encoder_;
    std::mutex tracks_mutex_;
    std::map<std::uint32_t, std::unique_ptr<Track>> tracks_;
    std::atomic<bool> running_{};
    std::atomic<bool> reported_error_{};
    void* stop_event_{};
    std::thread capture_thread_;
    std::thread render_thread_;
    std::uint64_t sequence_{};
};

} // namespace beamit::client
