#pragma once

#include "host/AudioEndpoint.hpp"
#include "host/wss/WebSocket.hpp"

namespace beamit::host::wss {

[[nodiscard]] json::object audio_configuration_json(const media::AudioConfiguration& configuration);

class AudioSocket : public std::enable_shared_from_this<AudioSocket> {
public:
    AudioSocket(std::shared_ptr<WebSocket> socket, std::shared_ptr<AudioEndpoint> endpoint,
                media::AudioConfiguration configuration);
    void start();
    void close(websocket::close_reason reason);
    void add_track(const AudioTrack& track);
    void remove_track(std::uint32_t track_id);
    std::function<void(std::string, std::string)> on_ended;

private:
    void output_ready();
    void fail(std::string reason, std::string message);
    void pump();
    static awaitable<void> write_loop(std::shared_ptr<AudioSocket> self);
    static awaitable<void> read_loop(std::shared_ptr<AudioSocket> self);

    std::shared_ptr<WebSocket> ws_;
    std::shared_ptr<AudioEndpoint> endpoint_;
    media::AudioConfiguration configuration_;
    struct TrackState {
        AudioTrack description;
        std::shared_ptr<AudioPipeline> pipeline;
        std::deque<net::protocol::AudioPacket> outgoing;
    };
    std::map<std::uint32_t, TrackState> tracks_;
    std::uint32_t last_written_track_{};
    std::optional<std::uint64_t> received_sequence_;
    std::optional<websocket::close_reason> close_reason_;
    bool closing_{};
    bool writing_{};
};

} // namespace beamit::host::wss
