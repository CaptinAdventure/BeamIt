#pragma once

#include "host/wss/Fields.hpp"
#include "host/wss/StreamSocket.hpp"
#include "host/AttentionMonitor.hpp"
#include "host/Clipboard.hpp"
#include "host/HostServer.hpp"
#include "host/InputInjector.hpp"
#include "host/StreamPipeline.hpp"
#include "host/WindowTracker.hpp"

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace beamit::host::wss {

struct ControlSession;

// Host-wide state shared by the network thread and the monitors that post to it.
struct Server {
    explicit Server(HostServerOptions host_options) : options(std::move(host_options)) {}

    asio::io_context io{1};
    // Icon extraction can block on hung windows and disk; it runs here, never on the network thread. Declared after
    // `io` so that it is joined before the io_context it posts replies to is destroyed.
    asio::thread_pool icon_worker{1};
    wintls::context tls{wintls::method::system_default};
    HostServerOptions options;
    std::weak_ptr<ControlSession> active;
    // Windows currently requesting attention (network thread only).
    std::map<HWND, AttentionEvent> attention;
    // Reported top-level windows (network thread only).
    WindowList windows;
    ClipboardMonitor* clipboard{};
};

// One stream of a control session, from `start_stream` until it stops.
struct StreamEntry {
    std::string id;
    std::string token;
    RequestId request_id;
    json::object source;
    VideoParameters video;
    bool ready{};
    bool token_used{};
    Clock::time_point token_expiry{};
    std::unique_ptr<StreamPipeline> pipeline;
    std::shared_ptr<StreamSocket> socket;
    std::unique_ptr<asio::steady_timer> token_timer;
    std::unique_ptr<input::StreamInput> input;
    // Process of a window/application stream; attention events of any of its windows are reported. Zero for desktop
    // streams, which report every window.
    DWORD pid{};
    bool focus_error_sent{};
    bool input_error_sent{};

    // Releases held input, stops capture/encoding and closes the stream socket.
    void shut_down(websocket::close_reason reason);
};

// The JSON signaling socket (PROTOCOL.md §3-5). Everything runs on the network thread unless noted.
// Implementation: ControlSession.cpp (lifecycle, authentication, streams), ControlInput.cpp (§5.9) and
// ControlDesktop.cpp (windows, icons, clipboard, attention).
struct ControlSession : std::enable_shared_from_this<ControlSession> {
    ControlSession(Server& host, std::shared_ptr<WebSocket> socket, std::string url_authority, std::string peer);

    // Sends `hello`, then reads and handles messages until the socket closes.
    static awaitable<void> run(std::shared_ptr<ControlSession> self);
    void close(websocket::close_code code, std::string_view reason);

    void send(json::object message);
    void send_serialized(std::string message);
    void send_error(const RequestId& id, std::string_view code, std::string_view message,
                    std::string_view stream_id = {});

    // Stream socket upgrades: validates and consumes a token, then attaches the socket.
    std::shared_ptr<StreamEntry> claim(std::string_view stream_id, std::string_view token);
    [[nodiscard]] bool has_stream(const std::string& stream_id) const { return streams_.contains(stream_id); }
    void attach(const std::shared_ptr<StreamEntry>& entry, std::shared_ptr<WebSocket> socket);

    // Host events.
    void on_attention(const AttentionEvent& event);
    void on_clipboard_changed(const ClipboardChange& change);
    void send_window_event(const WindowEvent& event);

    const std::string& remote() const { return remote_; }

private:
    using Handler = void (ControlSession::*)(const json::object& message, const RequestId& id);
    using Outgoing = std::variant<std::string, websocket::close_reason>;

    // Core (ControlSession.cpp).
    void handle(const std::string& text);
    void reject_unauthenticated(std::string_view code, std::string_view message);
    void authenticate(const json::object& message);
    void reply(const RequestId& id, json::object message);
    // Thread-safe: a callback that adds `id`, serializes on the calling thread and sends from the network thread if
    // the session still exists.
    std::function<void(json::object)> deferred_reply(RequestId id);
    void pump();
    static awaitable<void> write_loop(std::shared_ptr<ControlSession> self);

    void ping(const json::object& message, const RequestId& id);
    void start_stream(const json::object& message, const RequestId& id);
    void stop_stream(const json::object& message, const RequestId& id);
    void request_key_frame(const json::object& message, const RequestId& id);
    std::shared_ptr<StreamEntry> find_stream(const json::object& message);
    void start_pipeline(const std::shared_ptr<StreamEntry>& entry);
    void offer(const std::shared_ptr<StreamEntry>& entry, const VideoParameters& video);
    void end_stream(const std::shared_ptr<StreamEntry>& entry, std::string_view reason, std::string_view message = {},
                    const RequestId& id = std::nullopt);
    void stop_all_streams(websocket::close_code code);

    // Input (ControlInput.cpp).
    static input::MouseMode mouse_mode_field(const json::value& value);
    void handle_input(const json::object& message, const RequestId& id);
    void report_input_result(StreamEntry& entry, input::InputResult result, const RequestId& id);

    // Desktop (ControlDesktop.cpp).
    void list_windows(const json::object& message, const RequestId& id);
    void get_window_icon(const json::object& message, const RequestId& id);
    void get_clipboard(const json::object& message, const RequestId& id);
    void set_clipboard(const json::object& message, const RequestId& id);
    void send_attention(const StreamEntry& entry, const AttentionEvent& event);

    Server& server_;
    std::shared_ptr<WebSocket> ws_;
    std::string authority_;
    std::string remote_;
    std::vector<std::byte> server_nonce_;
    std::deque<Outgoing> outbox_;
    std::map<std::string, std::shared_ptr<StreamEntry>> streams_;
    bool writing_{};
    bool closing_{};
    bool authenticated_{};
    bool auth_failed_{};
};

} // namespace beamit::host::wss
