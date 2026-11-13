#pragma once

#include "ClientProtocol.hpp"
#include "FrameProtocol.hpp"

#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace beamit::client {

struct SessionError {
    std::string code;    // Host error code (PROTOCOL.md §3.1) or a local code such as "timeout".
    std::string message; // User-facing text; never contains secrets or stream tokens.
};

struct ConnectOptions {
    std::string server_url;
    std::string password;
    std::string fingerprint; // Host certificate SHA-256; required unless trust_any_certificate is set.
    bool trust_any_certificate{};
    std::chrono::milliseconds timeout{std::chrono::seconds(15)};
};

// Stream callbacks run on the network thread so decoding is not delayed by the UI thread.
struct StreamCallbacks {
    // A new generation was configured; decoding must restart with this parameter set.
    std::function<void(const VideoParameters&)> configured;
    // The payload is valid only during the call.
    std::function<void(const VideoFrame&)> frame;
    // The stream socket ended unexpectedly. Not called after close_stream or session teardown.
    std::function<void(const SessionError&)> failed;
};

struct AudioCallbacks {
    // Audio packets run on the network thread; implementations must enqueue quickly.
    std::function<void(const net::protocol::AudioPacket&, const media::AudioConfiguration&)> packet;
    std::function<void(const SessionError&)> failed;
};

// One authenticated signaling session plus its stream sockets (PROTOCOL.md §2-§7).
// Request completions and events run through the dispatcher; none run after destruction.
class Session {
public:
    using Dispatcher = std::function<void(std::function<void()>)>;
    template<class T>
    using Completion = std::function<void(std::expected<T, SessionError>)>;

    struct Events {
        // Unsolicited host messages, e.g. window_opened, stream_config, attention, stream_stopped.
        std::function<void(const json::object&)> message;
        // The authenticated session ended without disconnect() being called.
        std::function<void(const SessionError&)> disconnected;
    };

    Session(Dispatcher dispatcher, Events events);
    // Closes all sockets and joins the network thread. Must not run on the network thread.
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void connect(ConnectOptions options, Completion<void> done);
    void request(std::string type, json::object fields, std::string expected_reply, Completion<json::object> done);
    // Fire-and-forget message, e.g. input (PROTOCOL.md §5.9). Ignored unless authenticated.
    void send(json::object message);
    // Graceful and idempotent; suppresses the disconnected event and fails pending requests.
    void disconnect();

    void open_stream(const StreamOffer& offer, StreamCallbacks callbacks, Completion<void> opened);
    void close_stream(const std::string& stream_id);
    // Drops pictures until the next key frame and asks the host for one, e.g. after a decoder error.
    void restart_stream_decoding(const std::string& stream_id);

    void open_audio(const AudioOffer& offer, AudioCallbacks callbacks, Completion<void> opened);
    void configure_audio_track(AudioTrackConfiguration track);
    void remove_audio_track(std::uint32_t track_id);
    void send_audio_packet(std::vector<std::byte> packet);
    void close_audio();

    [[nodiscard]] std::optional<ClockEstimate::Sample> clock() const;
    [[nodiscard]] std::optional<Endpoint> endpoint() const;
    [[nodiscard]] std::string certificate_fingerprint() const;

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace beamit::client
