#pragma once

#include "host/wss/Fields.hpp"
#include "host/wss/AudioSocket.hpp"
#include "host/wss/StreamSocket.hpp"
#include "host/AttentionMonitor.hpp"
#include "host/Clipboard.hpp"
#include "host/ClipboardBulkStorage.hpp"
#include "host/HostServer.hpp"
#include "host/HostControl.hpp"
#include "host/InputInjector.hpp"
#include "host/StreamPipeline.hpp"
#include "host/WindowTracker.hpp"

#include <deque>
#include <cstddef>
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
    asio::thread_pool bulk_worker{1};
    wintls::context tls{wintls::method::system_default};
    HostServerOptions options;
    std::weak_ptr<ControlSession> active;
    std::uint64_t auth_generation{};
    // Windows currently requesting attention (network thread only).
    std::map<HWND, AttentionEvent> attention;
    // Reported top-level windows (network thread only).
    WindowList windows;
    ClipboardMonitor* clipboard{};
    // Optional native facade for deterministic, side-effect-free integration tests.
    std::function<DWORD()> clipboard_sequence{GetClipboardSequenceNumber};
    std::function<void(ClipboardWrite, std::function<void(ClipboardWriteResult)>)> clipboard_publish;
    std::function<void(unsigned, bool, ClipboardMonitor::ReadCallback)> clipboard_read;
    std::filesystem::path bulk_staging_root;
    std::function<Clock::time_point()> bulk_clock{Clock::now};
    // Optional bounded preparation observer for deterministic source-mutation/cancel tests.
    std::function<void(std::uint64_t)> bulk_prepare_observer;
    ClipboardBulkWriteCheck bulk_write_check;
    // Network-thread payload ownership / pending-write deltas, for bounded local
    // diagnostics. Never invoked by the disk worker; empty in normal operation.
    std::function<void(std::ptrdiff_t, int)> bulk_send_activity;
    // Connected stream count and execution state are owned by the network thread.
    std::size_t connected_streams{};
    bool execution_state_active{};
    InputElevation input_elevation{InputElevation::Starting};

    bool stream_connected();
    bool stream_disconnected();
    void change_password(HostCommand& command);
    void publish_status();
};

struct BulkEntry {
    explicit BulkEntry(net::clipboard_bulk::SocketCredentials issued)
        : credentials(std::move(issued)), token(credentials.token) {}
    net::clipboard_bulk::SocketCredentials credentials;
    net::clipboard_bulk::SingleUseToken token;
    net::clipboard_bulk::Manifest manifest;
    std::string source_snapshot;
    DWORD destination_sequence{};
    ClipboardWrite metadata;
    std::unique_ptr<ClipboardBulkReceiver> receiver; // accessed serially on bulk_worker
    std::unique_ptr<ClipboardBulkSpool> spool; // accessed serially on bulk_worker
    std::shared_ptr<const ClipboardBulkSnapshot> source;
    std::atomic<std::uint64_t> prepared_bytes{};
    std::atomic<Clock::duration::rep> prepared_tick{};
    std::shared_ptr<WebSocket> socket;
    std::shared_ptr<std::atomic_bool> allowed{std::make_shared<std::atomic_bool>(true)};
    std::unique_ptr<asio::steady_timer> timer;
    std::unique_ptr<asio::steady_timer> preparation_timer;
    Clock::time_point created{Clock::now()}, progress{created}, reported{}, offered{};
    std::uint64_t received{};
    std::uint32_t completed_nodes{};
    std::uint64_t receiver_bytes{};
    std::uint64_t sending_ceiling{};
    bool ready{}, ended{}, committed{}, publishing{}, terminal{}, download{}, end_in_flight{}, receiver_complete{};
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
    bool connected{};
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
    // input.cursor == "client" (§5.9.11): the pointer is not drawn into the video; its shape is sent instead.
    bool mirror_cursor{};
    std::string cursor_shape;
    // Unanswered set_stream_rate requests by pipeline serial (§5.14).
    std::vector<std::pair<std::uint64_t, RequestId>> rate_requests;
    std::string local_statistics;
    Clock::time_point stats_sampled_at{};

    // Releases held input, stops capture/encoding and closes the stream socket.
    void shut_down(websocket::close_reason reason);
};

struct AudioEntry {
    std::string id;
    std::string token;
    bool token_used{};
    Clock::time_point token_expiry{};
    std::unique_ptr<asio::steady_timer> token_timer;
    media::AudioConfiguration configuration;
    std::shared_ptr<AudioSocket> socket;
    std::map<std::uint32_t, AudioTrack> capture_tracks;
    struct CaptureState {
        std::unique_ptr<AudioCapture> capture;
        RequestId request_id;
        bool ready{};
    };
    std::map<std::uint32_t, CaptureState> captures;
    std::uint32_t next_track_id{net::protocol::microphone_track_id + 1};
};

// The JSON signaling socket (PROTOCOL.md §3-5). Everything runs on the network thread unless noted.
// Implementation: ControlSession.cpp (lifecycle, authentication, streams), ControlInput.cpp (§5.9),
// ControlDesktop.cpp (windows, icons, clipboard, attention) and ControlStats.cpp (§5.15).
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
    std::shared_ptr<AudioEntry> claim_audio(std::string_view audio_id, std::string_view token);
    [[nodiscard]] bool has_audio(const std::shared_ptr<AudioEntry>& entry) const;
    void attach_audio(const std::shared_ptr<AudioEntry>& entry, std::shared_ptr<WebSocket> socket);
    std::shared_ptr<BulkEntry> claim_bulk(std::string_view id, std::string_view token);
    void attach_bulk(const std::shared_ptr<BulkEntry>& entry, std::shared_ptr<WebSocket> socket);
    void invalidate_bulk();

    // Host events.
    void on_attention(const AttentionEvent& event);
    void on_clipboard_changed(const ClipboardChange& change);
    void send_window_event(const WindowEvent& event);

    const std::string& remote() const { return remote_; }
    HostStatus local_status() const;
    void disconnect_stream(const std::string& id);
    void upgrade_input(const std::shared_ptr<input::ElevatedInput>& helper);
    // Reverts every stream to normal (non-elevated) input after the elevated helper failed.
    void downgrade_input();

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
    void set_stream_rate(const json::object& message, const RequestId& id);
    std::shared_ptr<StreamEntry> find_stream(const json::object& message);
    void start_pipeline(const std::shared_ptr<StreamEntry>& entry);
    void offer(const std::shared_ptr<StreamEntry>& entry, const VideoParameters& video);
    void end_stream(const std::shared_ptr<StreamEntry>& entry, std::string_view reason, std::string_view message = {},
                    const RequestId& id = std::nullopt);
    void release_stream_execution_state(StreamEntry& entry);
    void stop_all_streams(websocket::close_code code);
    void start_audio(const json::object& message, const RequestId& id);
    void stop_audio(const json::object& message, const RequestId& id);
    void start_audio_track(const json::object& message, const RequestId& id);
    void stop_audio_track(const json::object& message, const RequestId& id);
    void end_audio_track(const std::shared_ptr<AudioEntry>& entry, std::uint32_t track_id,
                         std::string_view reason, std::string_view message = {}, const RequestId& id = std::nullopt);
    std::shared_ptr<AudioEntry> find_audio(const json::object& message);
    void end_audio(std::string_view reason, std::string_view message = {}, const RequestId& id = std::nullopt);

    // Input (ControlInput.cpp).
    static input::MouseMode mouse_mode_field(const json::value& value);
    void handle_input(const json::object& message, const RequestId& id);
    void report_input_result(StreamEntry& entry, input::InputResult result, const RequestId& id);
    // Polls the host pointer shape while any stream mirrors it (§5.9.11).
    void schedule_cursor();
    void poll_cursor();

    // Desktop (ControlDesktop.cpp).
    void list_windows(const json::object& message, const RequestId& id);
    void get_window_icon(const json::object& message, const RequestId& id);
    void get_clipboard(const json::object& message, const RequestId& id);
    void set_clipboard(const json::object& message, const RequestId& id);
    void send_attention(const StreamEntry& entry, const AttentionEvent& event);
    void bulk_negotiate(const json::object&, const RequestId&);
    void bulk_begin(const json::object&, const RequestId&);
    void bulk_commit(const json::object&, const RequestId&);
    void bulk_cancel(const json::object&, const RequestId&);
    void bulk_request(const json::object&, const RequestId&);
    void bulk_receiver_complete(const json::object&, const RequestId&);
    void bulk_receiver_progress(const json::object&, const RequestId&);
    void bulk_receiver_failed(const json::object&, const RequestId&);
    void get_bulk_clipboard(unsigned formats, RequestId id, std::string reconfirm_transfer = {});
    void read_clipboard(unsigned formats, bool binary, ClipboardMonitor::ReadCallback done);
    void complete_download();
    void finish_bulk(std::string_view code, bool cancelled = false, std::string_view detail = {}, bool notify = true);
    void publish_bulk();
    void schedule_bulk_timeout();
    static awaitable<void> receive_bulk(std::shared_ptr<ControlSession>, std::shared_ptr<BulkEntry>);
    static awaitable<void> prepare_bulk(std::shared_ptr<ControlSession>, std::shared_ptr<BulkEntry>, RequestId);
    static awaitable<void> publish_bulk_async(std::shared_ptr<ControlSession>, std::shared_ptr<BulkEntry>);
    static awaitable<void> snapshot_bulk(std::shared_ptr<ControlSession>, ClipboardContent,
        std::shared_ptr<std::atomic_bool>, RequestId, std::string reconfirm_transfer);
    static awaitable<void> prepare_download(std::shared_ptr<ControlSession>, std::shared_ptr<BulkEntry>);
    static awaitable<void> send_bulk(std::shared_ptr<ControlSession>, std::shared_ptr<BulkEntry>);
    static awaitable<void> watch_download_socket(std::shared_ptr<ControlSession>, std::shared_ptr<BulkEntry>);

    // Statistics (ControlStats.cpp).
    void schedule_stats();
    void send_stream_stats();

    Server& server_;
    std::shared_ptr<WebSocket> ws_;
    std::string authority_;
    std::string remote_;
    std::vector<std::byte> server_nonce_;
    std::uint64_t auth_generation_{};
    std::deque<Outgoing> outbox_;
    std::map<std::string, std::shared_ptr<StreamEntry>> streams_;
    std::shared_ptr<AudioEntry> audio_;
    bool bulk_negotiated_{};
    std::shared_ptr<BulkEntry> bulk_;
    std::shared_ptr<ClipboardBulkSnapshot> bulk_snapshot_;
    std::string bulk_snapshot_id_;
    std::shared_ptr<std::atomic_bool> snapshot_job_;
    std::shared_ptr<asio::steady_timer> snapshot_timer_;
    std::function<void()> snapshot_abort_;
    std::string last_bulk_id_, last_bulk_source_;
    bool writing_{};
    bool closing_{};
    bool authenticated_{};
    bool auth_failed_{};
    std::unique_ptr<asio::steady_timer> stats_timer_;
    Clock::time_point stats_since_{};
    std::unique_ptr<asio::steady_timer> cursor_timer_;
    bool cursor_polling_{};
};

} // namespace beamit::host::wss
