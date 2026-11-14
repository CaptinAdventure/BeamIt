#pragma once

#include "ClientProtocol.hpp"
#include "FrameProtocol.hpp"

#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

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

// Bulk callbacks run on the network thread. A binary message is valid only during the callback.
struct BulkCallbacks {
    std::function<void(std::span<const std::byte>)> message;
    std::function<void(const SessionError&)> failed;
    // When supplied, the next network read waits for this serial consumer.
    std::function<void(std::vector<std::byte>, std::function<void(std::expected<void, SessionError>)>)> consume;
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

    // Opens one session-bound bulk socket at a same-authority path such as /bulk/{id}?token=...
    void open_bulk(std::string target, std::chrono::milliseconds token_lifetime, BulkCallbacks callbacks,
                   Completion<void> opened);
    // Queues one binary message. The session bounds queued payload to the bulk protocol limit.
    void write_bulk(std::vector<std::byte> message, Completion<void> written);
    void close_bulk();

    [[nodiscard]] std::optional<ClockEstimate::Sample> clock() const;
    [[nodiscard]] std::optional<Endpoint> endpoint() const;
    [[nodiscard]] std::string certificate_fingerprint() const;

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace beamit::client
