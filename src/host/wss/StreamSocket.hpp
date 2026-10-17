#pragma once

#include "host/wss/WebSocket.hpp"
#include "host/StreamPipeline.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace beamit::host::wss {

// One binary stream socket (PROTOCOL.md §6-7) with a tiny bounded queue; overflow skips to the next key frame.
// Network thread only.
struct StreamSocket : std::enable_shared_from_this<StreamSocket> {
    static constexpr std::size_t max_queued = 2;

    StreamSocket(std::shared_ptr<WebSocket> socket, std::string id) : ws(std::move(socket)), stream_id(std::move(id)) {}

    // Queues a frame; returns false when the encoder must produce a key frame to recover.
    bool deliver(std::shared_ptr<const EncodedFrame> frame);
    // Drops queued frames and closes the socket after any write in progress.
    void close(websocket::close_reason reason);
    // Reads until the client disconnects (clients must not send data), then calls `on_disconnected`.
    static awaitable<void> read_loop(std::shared_ptr<StreamSocket> self);

    std::shared_ptr<WebSocket> ws;
    std::string stream_id;
    std::function<void()> on_disconnected;

private:
    struct QueuedFrame {
        std::shared_ptr<const EncodedFrame> frame;
        bool discontinuity{};
    };

    void start_writing();
    static awaitable<void> write_loop(std::shared_ptr<StreamSocket> self);

    std::deque<QueuedFrame> queue_;
    std::optional<websocket::close_reason> pending_close_;
    bool writing_{};
    bool closing_{};
    bool drop_until_key_{};
    std::uint64_t dropped_{};
};

} // namespace beamit::host::wss
