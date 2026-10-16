#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/json.hpp>
#include <wintls.hpp>
#include <wintls/beast.hpp>

#include "host/HostServer.hpp"

#include "ApplicationWindows.hpp"
#include "FrameTimer.hpp"
#include "HtmlAssets.hpp"
#include "Utf8.hpp"
#include "host/AttentionMonitor.hpp"
#include "host/InputInjector.hpp"
#include "host/StreamPipeline.hpp"
#include "media/BitstreamInfo.hpp"
#include "net/Protocol.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <csignal>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

namespace beamit::host {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace json = boost::json;
namespace protocol = beamit::net::protocol;
using tcp = asio::ip::tcp;
using TlsStream = wintls::stream<tcp::socket>;
using WebSocket = websocket::stream<TlsStream>;
using asio::awaitable;
using asio::use_awaitable;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

std::string bytes_text(std::span<const std::byte> bytes) { return beamit::net::base64url_encode(bytes); }

json::object video_json(const VideoParameters& video) {
    return {
        {"generation", video.generation},
        {"codec", video.codec == media::VideoCodec::H264 ? "h264" : "h265"},
        {"codec_string", video.codec_string},
        {"format", "annexb"},
        {"width", video.width},
        {"height", video.height},
        {"chroma_format", media::chroma_format_name(video.chroma_format_idc)},
        {"bit_depth", video.bit_depth},
        {"frame_rate", video.frame_rate},
        {"bitrate_kbps", video.bitrate_kbps},
        {"key_frame_interval", 60},
        {"b_frames", 0},
        {"encoder", video.encoder},
    };
}

std::string hwnd_text(HWND window) {
    return fmt::format("0x{:X}", reinterpret_cast<std::uintptr_t>(window));
}

std::optional<HWND> parse_hwnd(std::string_view text) {
    int base = 10;
    if (text.starts_with("0x") || text.starts_with("0X")) {
        base = 16;
        text.remove_prefix(2);
    }
    std::uintptr_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (error != std::errc{} || end != text.data() + text.size() || value == 0) return std::nullopt;
    return reinterpret_cast<HWND>(value);
}

std::string window_title(HWND window) {
    std::wstring title(static_cast<std::size_t>(GetWindowTextLengthW(window)) + 1, L'\0');
    title.resize(static_cast<std::size_t>(GetWindowTextW(window, title.data(), static_cast<int>(title.size()))));
    return to_utf8(title);
}

bool valid_authority(std::string_view authority) {
    if (authority.empty() || authority.size() > 255) return false;
    for (const char character : authority) {
        if (!std::isalnum(static_cast<unsigned char>(character)) && character != '.' && character != '-' &&
            character != ':' && character != '[' && character != ']') {
            return false;
        }
    }
    return true;
}

struct ProtocolError {
    std::string code;
    std::string message;
};

struct Server;
struct ControlSession;
struct StreamEntry;

struct QueuedFrame {
    std::shared_ptr<const EncodedFrame> frame;
    bool discontinuity{};
};

// One binary stream socket with a tiny bounded queue; overflow skips to the next key frame.
struct StreamSocket : std::enable_shared_from_this<StreamSocket> {
    static constexpr std::size_t max_queued = 2;

    StreamSocket(std::shared_ptr<WebSocket> socket, std::string id) : ws(std::move(socket)), stream_id(std::move(id)) {}

    std::shared_ptr<WebSocket> ws;
    std::string stream_id;
    std::deque<QueuedFrame> queue;
    std::function<void()> on_disconnected;
    std::optional<websocket::close_reason> pending_close;
    bool writing{};
    bool closing{};
    bool drop_until_key{};
    std::uint64_t dropped{};

    // Returns false when the encoder must produce a key frame to recover.
    bool deliver(std::shared_ptr<const EncodedFrame> frame) {
        if (closing) return true;
        bool discontinuity = false;
        bool recovered = true;
        if (!drop_until_key && queue.size() >= max_queued) {
            dropped += queue.size();
            queue.clear();
            drop_until_key = true;
            recovered = frame->packet.key_frame;
            spdlog::warn("Stream {}: client is not keeping up; dropping frames until the next key frame", stream_id);
        }
        if (drop_until_key) {
            if (!frame->packet.key_frame) {
                ++dropped;
                return recovered;
            }
            drop_until_key = false;
            discontinuity = true;
        }
        queue.push_back({std::move(frame), discontinuity});
        if (!writing) {
            writing = true;
            asio::co_spawn(ws->get_executor(), write_loop(shared_from_this()), asio::detached);
        }
        return recovered;
    }

    static awaitable<void> write_loop(std::shared_ptr<StreamSocket> self) {
        try {
            while (!self->queue.empty()) {
                auto item = std::move(self->queue.front());
                self->queue.pop_front();
                const auto& packet = item.frame->packet;
                std::uint32_t flags = 0;
                if (packet.key_frame) flags |= protocol::flag_key_frame;
                if (item.discontinuity) flags |= protocol::flag_discontinuity;
                const auto header = protocol::encode_frame_header({
                    .flags = flags,
                    .generation = item.frame->generation,
                    .sequence = item.frame->sequence,
                    .capture_time_us = protocol::host_time_us(packet.captured_at),
                });
                const std::array<asio::const_buffer, 2> buffers{
                    asio::buffer(header), asio::buffer(packet.data.data(), packet.data.size())};
                co_await self->ws->async_write(buffers, use_awaitable);
            }
            if (self->pending_close) {
                auto reason = *self->pending_close;
                self->pending_close.reset();
                co_await self->ws->async_close(reason, use_awaitable);
            }
        } catch (const std::exception& error) {
            spdlog::debug("Stream {} write ended: {}", self->stream_id, error.what());
            beast::error_code ignored;
            beast::get_lowest_layer(*self->ws).close(ignored);
        }
        self->writing = false;
    }

    void close(websocket::close_reason reason) {
        if (closing) return;
        closing = true;
        queue.clear();
        pending_close = reason;
        if (!writing) {
            writing = true;
            asio::co_spawn(ws->get_executor(), write_loop(shared_from_this()), asio::detached);
        }
    }

    static awaitable<void> read_loop(std::shared_ptr<StreamSocket> self) {
        beast::flat_buffer buffer;
        try {
            while (true) {
                buffer.clear();
                co_await self->ws->async_read(buffer, use_awaitable);
                self->close({websocket::close_code::bad_payload, "clients must not send data on stream sockets"});
            }
        } catch (const std::exception& error) {
            spdlog::debug("Stream {} socket closed: {}", self->stream_id, error.what());
        }
        if (!self->closing) {
            self->closing = true;
            if (self->on_disconnected) self->on_disconnected();
        }
    }
};

struct StreamEntry {
    std::string id;
    std::string token;
    std::optional<json::value> request_id;
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
};

struct Server {
    explicit Server(HostServerOptions host_options) : options(std::move(host_options)) {}

    asio::io_context io{1};
    wintls::context tls{wintls::method::system_default};
    HostServerOptions options;
    std::weak_ptr<ControlSession> active;
    // Windows currently requesting attention (network thread only).
    std::map<HWND, AttentionEvent> attention;
};

using Outgoing = std::variant<std::string, websocket::close_reason>;

struct ControlSession : std::enable_shared_from_this<ControlSession> {
    ControlSession(Server& host, std::shared_ptr<WebSocket> socket, std::string url_authority, std::string peer)
        : server(host), ws(std::move(socket)), authority(std::move(url_authority)), remote(std::move(peer)),
          server_nonce(beamit::net::random_bytes(protocol::nonce_bytes)) {}

    Server& server;
    std::shared_ptr<WebSocket> ws;
    std::string authority;
    std::string remote;
    std::vector<std::byte> server_nonce;
    std::deque<Outgoing> outbox;
    std::map<std::string, std::shared_ptr<StreamEntry>> streams;
    bool writing{};
    bool closing{};
    bool authenticated{};
    bool auth_failed{};

    void send(json::object message) {
        if (closing) return;
        outbox.emplace_back(json::serialize(message));
        pump();
    }

    void send_error(const std::optional<json::value>& id, std::string_view code, std::string_view message,
                    std::string_view stream_id = {}) {
        json::object error{{"type", "error"}, {"code", code}, {"message", message}};
        if (id) error["id"] = *id;
        if (!stream_id.empty()) error["stream_id"] = stream_id;
        send(std::move(error));
    }

    void close(websocket::close_code code, std::string_view reason) {
        if (closing) return;
        closing = true;
        stop_all_streams(websocket::close_code::going_away);
        outbox.emplace_back(websocket::close_reason{code, reason});
        pump();
    }

    void pump() {
        if (writing || outbox.empty()) return;
        writing = true;
        asio::co_spawn(ws->get_executor(), write_loop(shared_from_this()), asio::detached);
    }

    static awaitable<void> write_loop(std::shared_ptr<ControlSession> self) {
        try {
            while (!self->outbox.empty()) {
                auto item = std::move(self->outbox.front());
                self->outbox.pop_front();
                if (auto* text = std::get_if<std::string>(&item)) {
                    self->ws->text(true);
                    co_await self->ws->async_write(asio::buffer(*text), use_awaitable);
                } else {
                    co_await self->ws->async_close(std::get<websocket::close_reason>(item), use_awaitable);
                    self->outbox.clear();
                }
            }
        } catch (const std::exception& error) {
            spdlog::debug("Control write ended: {}", error.what());
            self->outbox.clear();
            beast::error_code ignored;
            beast::get_lowest_layer(*self->ws).close(ignored);
        }
        self->writing = false;
    }

    void stop_all_streams(websocket::close_code code) {
        auto entries = std::move(streams);
        streams.clear();
        for (auto& [id, entry] : entries) {
            if (entry->token_timer) entry->token_timer->cancel();
            entry->input->release_all();
            entry->pipeline->stop();
            if (entry->socket) entry->socket->close({code, "signaling session ended"});
        }
    }

    void end_stream(const std::shared_ptr<StreamEntry>& entry, std::string_view reason,
                    std::string_view message = {}, const std::optional<json::value>& id = std::nullopt) {
        if (streams.erase(entry->id) == 0) return;
        if (entry->token_timer) entry->token_timer->cancel();
        entry->input->release_all();
        entry->pipeline->stop();
        if (entry->socket) entry->socket->close({websocket::close_code::normal, reason});
        json::object stopped{{"type", "stream_stopped"}, {"stream_id", entry->id}, {"reason", reason}};
        if (id) stopped["id"] = *id;
        if (!message.empty()) stopped["message"] = message;
        send(std::move(stopped));
        spdlog::info("Stream {} stopped: {}{}{}", entry->id, reason, message.empty() ? "" : " - ", message);
    }

    static awaitable<void> run(std::shared_ptr<ControlSession> self) {
        const auto& salt = self->server.options.auth_salt;
        self->send({
            {"type", "hello"},
            {"protocol", protocol::version},
            {"server", "BeamIt"},
            {"auth", {
                {"mechanism", "pbkdf2-sha256-hmac"},
                {"salt", bytes_text(salt)},
                {"iterations", self->server.options.auth_iterations},
                {"server_nonce", bytes_text(self->server_nonce)},
            }},
        });
        auto auth_timer = std::make_shared<asio::steady_timer>(self->ws->get_executor(), protocol::auth_timeout);
        auth_timer->async_wait([weak = std::weak_ptr(self)](beast::error_code error) {
            auto session = weak.lock();
            if (error || !session || session->authenticated) return;
            spdlog::info("Control client {} did not authenticate in time", session->remote);
            beast::error_code ignored;
            beast::get_lowest_layer(*session->ws).close(ignored);
        });

        beast::flat_buffer buffer;
        try {
            while (true) {
                buffer.clear();
                co_await self->ws->async_read(buffer, use_awaitable);
                if (self->closing || self->auth_failed) continue;
                if (!self->ws->got_text()) {
                    self->close(websocket::close_code::bad_payload, "binary messages are not allowed");
                    continue;
                }
                self->handle(beast::buffers_to_string(buffer.data()));
            }
        } catch (const std::exception& error) {
            spdlog::debug("Control socket {} closed: {}", self->remote, error.what());
        }
        auth_timer->cancel();
        self->closing = true;
        self->stop_all_streams(websocket::close_code::going_away);
        if (self->server.active.lock() == self) self->server.active.reset();
        if (self->authenticated) spdlog::info("Control client {} disconnected", self->remote);
    }

    void handle(const std::string& text) {
        beast::error_code parse_error;
        auto value = json::parse(text, parse_error);
        if (parse_error || !value.is_object() || !value.as_object().contains("type") ||
            !value.as_object().at("type").is_string()) {
            if (!authenticated) return reject_unauthenticated("invalid_request", "expected an authenticate message");
            return send_error(std::nullopt, "invalid_request", "messages must be JSON objects with a string type");
        }
        auto& message = value.as_object();
        const std::string_view type = message.at("type").as_string();
        std::optional<json::value> id;
        if (const auto* field = message.if_contains("id")) {
            constexpr std::int64_t limit = 1LL << 53;
            if (field->is_string() || (field->is_int64() && field->as_int64() > -limit && field->as_int64() < limit) ||
                (field->is_uint64() && field->as_uint64() < static_cast<std::uint64_t>(limit))) {
                id = *field;
            } else {
                return send_error(std::nullopt, "invalid_request", "id must be a string or a safe integer");
            }
        }
        if (!authenticated) {
            if (type != "authenticate") return reject_unauthenticated("not_authenticated", "authenticate first");
            return authenticate(message);
        }
        try {
            if (type == "ping") {
                json::object pong{{"type", "pong"}, {"host_time_us", protocol::host_time_us(Clock::now())}};
                if (id) pong["id"] = *id;
                send(std::move(pong));
            } else if (type == "list_windows") {
                list_windows(id);
            } else if (type == "start_stream") {
                start_stream(message, id);
            } else if (type == "stop_stream") {
                end_stream(find_stream(message), "requested", {}, id);
            } else if (type == "request_key_frame") {
                find_stream(message)->pipeline->request_key_frame();
            } else if (handle_input(type, message, id)) {
            } else if (type == "authenticate") {
                throw ProtocolError{"invalid_request", "already authenticated"};
            } else {
                throw ProtocolError{"unknown_type", fmt::format("unknown message type '{}'", type)};
            }
        } catch (const ProtocolError& error) {
            send_error(id, error.code, error.message);
        } catch (const std::exception& error) {
            spdlog::error("Request '{}' failed: {}", type, error.what());
            send_error(id, "internal_error", error.what());
        }
    }

    void reject_unauthenticated(std::string_view code, std::string_view message) {
        send_error(std::nullopt, code, message);
        close(websocket::close_code::policy_error, message);
    }

    static std::optional<std::vector<std::byte>> decoded_field(const json::object& message, std::string_view name) {
        const auto* field = message.if_contains(name);
        if (!field || !field->is_string()) return std::nullopt;
        return beamit::net::base64url_decode(field->as_string());
    }

    void authenticate(const json::object& message) {
        const auto* version = message.if_contains("protocol");
        if (!version || !version->is_int64() || version->as_int64() != protocol::version) {
            send_error(std::nullopt, "protocol_version", "this host supports protocol version 1");
            close(websocket::close_code::protocol_error, "unsupported protocol version");
            return;
        }
        const auto client_nonce = decoded_field(message, "client_nonce");
        const auto proof = decoded_field(message, "proof");
        bool valid = client_nonce && proof && client_nonce->size() >= 16 && client_nonce->size() <= 64;
        if (valid) {
            const auto expected = beamit::net::auth_proof(server.options.auth_key, "client", server_nonce,
                                                          *client_nonce);
            valid = beamit::net::constant_time_equal(expected, *proof);
        }
        if (!valid) {
            spdlog::warn("Authentication failed from {}", remote);
            auth_failed = true;
            send_error(std::nullopt, "auth_failed", "authentication failed");
            auto delay = std::make_shared<asio::steady_timer>(ws->get_executor(), 1s);
            delay->async_wait([self = shared_from_this(), delay](beast::error_code) {
                self->close(websocket::close_code::policy_error, "authentication failed");
            });
            return;
        }
        authenticated = true;
        if (auto previous = server.active.lock(); previous && previous.get() != this) {
            spdlog::info("Control client {} supersedes {}", remote, previous->remote);
            previous->close(static_cast<websocket::close_code>(protocol::close_superseded),
                            "superseded by a newer session");
        }
        server.active = shared_from_this();
        const auto server_proof = beamit::net::auth_proof(server.options.auth_key, "server", server_nonce,
                                                          *client_nonce);
        send({{"type", "authenticated"}, {"server_proof", bytes_text(server_proof)},
              {"host_time_us", protocol::host_time_us(Clock::now())}});
        spdlog::info("Control client {} authenticated", remote);
    }

    void list_windows(const std::optional<json::value>& id) {
        json::array windows;
        for (const auto& window : application_windows()) {
            windows.push_back({{"hwnd", hwnd_text(window.hwnd)}, {"pid", window.pid},
                               {"title", to_utf8(window.title)}, {"primary", window.primary_candidate}});
        }
        json::object reply{{"type", "windows"}, {"windows", std::move(windows)}};
        if (id) reply["id"] = *id;
        send(std::move(reply));
    }

    std::shared_ptr<StreamEntry> find_stream(const json::object& message) {
        const auto* field = message.if_contains("stream_id");
        if (!field || !field->is_string()) throw ProtocolError{"invalid_request", "stream_id is required"};
        const auto found = streams.find(std::string(field->as_string()));
        if (found == streams.end()) throw ProtocolError{"not_found", "unknown stream_id"};
        return found->second;
    }

    static std::uint32_t optional_uint(const json::object& video, std::string_view name, std::uint32_t fallback,
                                       std::uint32_t minimum, std::uint32_t maximum) {
        const auto* field = video.if_contains(name);
        if (!field) return fallback;
        if (!field->is_int64() && !field->is_uint64()) {
            throw ProtocolError{"invalid_request", fmt::format("{} must be an integer", name)};
        }
        const auto value = field->is_int64() ? field->as_int64() : static_cast<std::int64_t>(field->as_uint64());
        if (value < minimum || value > maximum) {
            throw ProtocolError{"invalid_request", fmt::format("{} must be {}-{}", name, minimum, maximum)};
        }
        return static_cast<std::uint32_t>(value);
    }

    void start_stream(const json::object& message, const std::optional<json::value>& id) {
        if (streams.size() >= protocol::max_streams_per_session) {
            throw ProtocolError{"limit_reached", "too many concurrent streams"};
        }
        const auto* source_field = message.if_contains("source");
        if (!source_field || !source_field->is_object() || !source_field->as_object().contains("kind") ||
            !source_field->as_object().at("kind").is_string()) {
            throw ProtocolError{"invalid_request", "source.kind is required"};
        }
        const auto& source = source_field->as_object();
        const std::string_view kind = source.at("kind").as_string();
        capture::CaptureTarget target{};
        DWORD stream_pid{};
        json::object source_out{{"kind", kind}};
        if (kind == "window" || kind == "application") {
            HWND window{};
            if (kind == "window") {
                const auto* hwnd = source.if_contains("hwnd");
                const auto parsed = hwnd && hwnd->is_string() ? parse_hwnd(hwnd->as_string()) : std::nullopt;
                if (!parsed) throw ProtocolError{"invalid_request", "source.hwnd must be a window handle string"};
                window = *parsed;
            } else {
                const auto* pid = source.if_contains("pid");
                if (!pid || !pid->is_int64() || pid->as_int64() <= 0 || pid->as_int64() > 0xFFFFFFFFLL) {
                    throw ProtocolError{"invalid_request", "source.pid must be a positive integer"};
                }
                try {
                    window = select_application_window(static_cast<DWORD>(pid->as_int64()));
                } catch (const std::exception&) {
                    throw ProtocolError{"not_found", "no visible window for that pid"};
                }
            }
            if (!IsWindow(window) || GetAncestor(window, GA_ROOT) != window) {
                throw ProtocolError{"not_found", "not a valid top-level window"};
            }
            if (!IsWindowVisible(window) || IsIconic(window)) {
                throw ProtocolError{"capture_failed", "the window must be visible and not minimized"};
            }
            DWORD pid{};
            GetWindowThreadProcessId(window, &pid);
            target.kind = capture::CaptureTargetKind::Window;
            target.window = window;
            source_out["hwnd"] = hwnd_text(window);
            source_out["pid"] = pid;
            stream_pid = pid;
            source_out["title"] = window_title(window);
        } else if (kind == "desktop") {
            target.kind = capture::CaptureTargetKind::Desktop;
        } else {
            throw ProtocolError{"invalid_request", "source.kind must be window, application or desktop"};
        }

        const auto& defaults = server.options;
        media::EncoderOptions options{.frame_rate = defaults.frame_rate,
                                      .target_bitrate_kbps = defaults.bitrate_kbps,
                                      .codec = defaults.codec};
        if (const auto* video_field = message.if_contains("video")) {
            if (!video_field->is_object()) throw ProtocolError{"invalid_request", "video must be an object"};
            const auto& video = video_field->as_object();
            if (const auto* codec = video.if_contains("codec")) {
                if (codec->is_string() && codec->as_string() == "h264") {
                    options.codec = media::VideoCodec::H264;
                } else if (codec->is_string() && codec->as_string() == "h265") {
                    options.codec = media::VideoCodec::H265;
                } else {
                    throw ProtocolError{"invalid_request", "video.codec must be h264 or h265"};
                }
            }
            options.frame_rate = optional_uint(video, "frame_rate", options.frame_rate, 1, 240);
            options.target_bitrate_kbps =
                optional_uint(video, "bitrate_kbps", options.target_bitrate_kbps, 1, 1'000'000);
        }

        auto mouse_mode = input::MouseMode::Absolute;
        if (const auto* input_field = message.if_contains("input")) {
            if (!input_field->is_object()) throw ProtocolError{"invalid_request", "input must be an object"};
            if (const auto* mode = input_field->as_object().if_contains("mouse_mode")) {
                mouse_mode = parse_mode(*mode);
            }
        }

        std::unique_ptr<capture::Capturer> capturer;
        std::unique_ptr<media::Encoder> encoder;
        try {
            capturer = defaults.make_capturer(target.kind);
        } catch (const std::exception& error) {
            throw ProtocolError{"capture_failed", error.what()};
        }
        try {
            encoder = defaults.make_encoder(options.codec);
        } catch (const std::exception& error) {
            throw ProtocolError{"encoder_failed", error.what()};
        }

        auto entry = std::make_shared<StreamEntry>();
        entry->id = bytes_text(beamit::net::random_bytes(16));
        entry->token = bytes_text(beamit::net::random_bytes(32));
        entry->request_id = id;
        entry->source = std::move(source_out);
        entry->pipeline = std::make_unique<StreamPipeline>(std::move(capturer), std::move(encoder), target, options);
        entry->input = std::make_unique<input::StreamInput>(target.window);
        entry->input->mouse_mode = mouse_mode;
        entry->pid = stream_pid;
        streams.emplace(entry->id, entry);
        spdlog::info("Starting stream {} for {}", entry->id, json::serialize(entry->source));

        // Pipeline callbacks run on its thread; hop to the network thread and drop stale events.
        const auto executor = ws->get_executor();
        const auto on_network = [executor, weak_self = weak_from_this(), weak_entry = std::weak_ptr(entry)](
                                    auto&& function) {
            asio::post(executor, [weak_self, weak_entry, function = std::forward<decltype(function)>(function)] {
                auto self = weak_self.lock();
                auto stream = weak_entry.lock();
                if (self && stream && self->streams.contains(stream->id)) function(*self, stream);
            });
        };
        entry->pipeline->start({
            .ready = [on_network](const VideoParameters& video) {
                on_network([video](ControlSession& self, const std::shared_ptr<StreamEntry>& stream) {
                    self.offer(stream, video);
                });
            },
            .start_failed = [on_network](std::string code, std::string text) {
                on_network([code, text](ControlSession& self, const std::shared_ptr<StreamEntry>& stream) {
                    self.streams.erase(stream->id);
                    stream->input->release_all();
                    stream->pipeline->stop();
                    self.send_error(stream->request_id, code, text);
                });
            },
            .reconfigured = [on_network](const VideoParameters& video) {
                on_network([video](ControlSession& self, const std::shared_ptr<StreamEntry>& stream) {
                    stream->video = video;
                    self.send({{"type", "stream_config"}, {"stream_id", stream->id}, {"video", video_json(video)}});
                });
            },
            .frame = [on_network](std::shared_ptr<const EncodedFrame> frame) {
                on_network([frame = std::move(frame)](ControlSession&, const std::shared_ptr<StreamEntry>& stream) {
                    if (stream->socket && !stream->socket->deliver(frame)) stream->pipeline->request_key_frame();
                });
            },
            .stopped = [on_network](std::string reason, std::string text) {
                on_network([reason, text](ControlSession& self, const std::shared_ptr<StreamEntry>& stream) {
                    self.end_stream(stream, reason, text);
                });
            },
        });
    }

    void offer(const std::shared_ptr<StreamEntry>& entry, const VideoParameters& video) {
        entry->video = video;
        entry->ready = true;
        entry->token_expiry = Clock::now() + protocol::token_lifetime;
        entry->token_timer = std::make_unique<asio::steady_timer>(ws->get_executor(), protocol::token_lifetime);
        entry->token_timer->async_wait(
            [weak_self = weak_from_this(), weak_entry = std::weak_ptr(entry)](beast::error_code error) {
                auto self = weak_self.lock();
                auto stream = weak_entry.lock();
                if (error || !self || !stream || stream->token_used) return;
                self->end_stream(stream, "token_expired");
            });
        json::object reply{
            {"type", "stream_offer"},
            {"stream_id", entry->id},
            {"url", fmt::format("wss://{}/streams/{}?token={}", authority, entry->id, entry->token)},
            {"token_expires_in_ms", std::chrono::milliseconds(protocol::token_lifetime).count()},
            {"source", entry->source},
            {"video", video_json(video)},
            {"input", {{"mouse_mode", input::mouse_mode_name(entry->input->mouse_mode)}}},
        };
        if (entry->request_id) reply["id"] = *entry->request_id;
        send(std::move(reply));
        for (const auto& [window, event] : server.attention) send_attention(*entry, event);
    }

    void on_attention(const AttentionEvent& event) {
        for (const auto& [id, entry] : streams) {
            if (entry->ready) send_attention(*entry, event);
        }
    }

    void send_attention(const StreamEntry& entry, const AttentionEvent& event) {
        if (entry.pid != 0 && entry.pid != event.pid) return;
        json::object window{{"hwnd", hwnd_text(event.window)}, {"pid", event.pid}};
        if (IsWindow(event.window)) window["title"] = window_title(event.window);
        send({{"type", "attention"}, {"stream_id", entry.id}, {"state", event.requested ? "set" : "clear"},
              {"window", std::move(window)}});
    }

    static input::MouseMode parse_mode(const json::value& value) {
        const auto mode = value.is_string() ? input::parse_mouse_mode(value.as_string()) : std::nullopt;
        if (!mode) throw ProtocolError{"invalid_request", "mouse_mode must be absolute or relative"};
        return *mode;
    }

    static std::optional<double> number(const json::object& message, std::string_view name) {
        const auto* field = message.if_contains(name);
        if (!field) return std::nullopt;
        double value{};
        if (field->is_double()) {
            value = field->as_double();
        } else if (field->is_int64()) {
            value = static_cast<double>(field->as_int64());
        } else if (field->is_uint64()) {
            value = static_cast<double>(field->as_uint64());
        } else {
            throw ProtocolError{"invalid_request", fmt::format("{} must be a number", name)};
        }
        if (!std::isfinite(value) || std::abs(value) > 1e6) {
            throw ProtocolError{"invalid_request", fmt::format("{} is out of range", name)};
        }
        return value;
    }

    static int wheel_units(const json::object& message, std::string_view name) {
        const auto* field = message.if_contains(name);
        if (!field) return 0;
        if (!field->is_int64() || std::abs(field->as_int64()) > protocol::max_wheel_units) {
            throw ProtocolError{"invalid_request",
                                fmt::format("{} must be an integer within +/-{}", name, protocol::max_wheel_units)};
        }
        return static_cast<int>(field->as_int64());
    }

    static bool required_bool(const json::object& message, std::string_view name) {
        const auto* field = message.if_contains(name);
        if (!field || !field->is_bool()) throw ProtocolError{"invalid_request", fmt::format("{} must be a boolean", name)};
        return field->as_bool();
    }

    static std::string_view required_string(const json::object& message, std::string_view name) {
        const auto* field = message.if_contains(name);
        if (!field || !field->is_string()) throw ProtocolError{"invalid_request", fmt::format("{} must be a string", name)};
        return field->as_string();
    }

    // Handles the §5.9 input messages; returns false for other types. Success is silent.
    bool handle_input(std::string_view type, const json::object& message, const std::optional<json::value>& id) {
        static constexpr std::array types{"set_mouse_mode", "mouse_move",  "mouse_button",   "mouse_wheel",
                                          "key",            "text",        "release_all",    "get_input_state"};
        if (std::ranges::find(types, type) == types.end()) return false;
        const auto entry = find_stream(message);
        auto& state = *entry->input;
        if (type == "set_mouse_mode") {
            state.mouse_mode = parse_mode(message.contains("mode") ? message.at("mode") : json::value{});
            json::object reply{{"type", "mouse_mode"}, {"stream_id", entry->id},
                               {"mode", input::mouse_mode_name(state.mouse_mode)}};
            if (id) reply["id"] = *id;
            send(std::move(reply));
            return true;
        }
        if (type == "get_input_state") {
            if (!entry->ready) throw ProtocolError{"not_ready", "the stream has not been offered yet"};
            const auto position = state.cursor_position(entry->video.width, entry->video.height)
                                      .value_or(input::ScreenPoint{-1, -1});
            const auto modifiers = input::modifier_state();
            json::object reply{
                {"type", "input_state"},
                {"stream_id", entry->id},
                {"x", position.x},
                {"y", position.y},
                {"modifiers", {{"shift", modifiers.shift}, {"control", modifiers.control}, {"alt", modifiers.alt},
                               {"meta", modifiers.meta}, {"caps_lock", modifiers.caps_lock},
                               {"num_lock", modifiers.num_lock}, {"scroll_lock", modifiers.scroll_lock}}},
                {"mouse_mode", input::mouse_mode_name(state.mouse_mode)},
            };
            if (id) reply["id"] = *id;
            send(std::move(reply));
            return true;
        }
        if (type == "release_all") {
            state.release_all();
            return true;
        }
        if (!entry->socket) throw ProtocolError{"not_ready", "open the stream socket before sending input"};

        auto result = input::InputResult::Ok;
        if (type == "mouse_move") {
            const auto absolute = state.mouse_mode == input::MouseMode::Absolute;
            const auto first = number(message, absolute ? "x" : "dx");
            const auto second = number(message, absolute ? "y" : "dy");
            if (!first || !second || message.contains(absolute ? "dx" : "x") || message.contains(absolute ? "dy" : "y")) {
                throw ProtocolError{"invalid_request", absolute ? "absolute mouse_move needs x and y only"
                                                                : "relative mouse_move needs dx and dy only"};
            }
            result = absolute ? state.move_absolute(*first, *second, entry->video.width, entry->video.height)
                              : state.move_relative(*first, *second, entry->video.width, entry->video.height);
        } else if (type == "mouse_button") {
            const auto button = input::parse_mouse_button(required_string(message, "button"));
            if (!button) throw ProtocolError{"invalid_request", "button must be left, right, middle, back or forward"};
            result = state.button(*button, required_bool(message, "down"));
        } else if (type == "mouse_wheel") {
            result = state.wheel(wheel_units(message, "dx"), wheel_units(message, "dy"));
        } else if (type == "key") {
            const auto code = required_string(message, "code");
            const auto key = input::key_for_code(code);
            if (!key) throw ProtocolError{"invalid_request", fmt::format("unsupported key code '{}'", code)};
            result = state.key(*key, required_bool(message, "down"));
        } else {
            const auto text = input::text_to_utf16(required_string(message, "text"));
            if (!text) {
                throw ProtocolError{"invalid_request",
                                    "text must be valid UTF-8, at most 1024 UTF-16 units, without control characters"};
            }
            result = state.text(*text);
        }
        report_input_result(*entry, result, id);
        return true;
    }

    // Reports focus/injection failures once until the next success so a stream of moves cannot flood the socket.
    void report_input_result(StreamEntry& entry, input::InputResult result, const std::optional<json::value>& id) {
        switch (result) {
        case input::InputResult::Ok:
            entry.focus_error_sent = false;
            entry.input_error_sent = false;
            return;
        case input::InputResult::FocusFailed:
            if (std::exchange(entry.focus_error_sent, true)) return;
            send_error(id, "focus_failed", "the target window could not be brought to the foreground", entry.id);
            return;
        case input::InputResult::InjectFailed:
        case input::InputResult::TargetGone:
            if (std::exchange(entry.input_error_sent, true)) return;
            send_error(id, "input_failed",
                       result == input::InputResult::TargetGone
                           ? "the target window no longer exists"
                           : "Windows rejected the input (secure desktop or elevated target?)",
                       entry.id);
            return;
        }
    }

    // Validates and consumes a stream token; returns the entry when the upgrade may proceed.
    std::shared_ptr<StreamEntry> claim(std::string_view stream_id, std::string_view token) {
        if (!authenticated || closing) return nullptr;
        const auto found = streams.find(std::string(stream_id));
        if (found == streams.end()) return nullptr;
        auto& entry = found->second;
        const auto expected = std::as_bytes(std::span(entry->token));
        const auto offered = std::as_bytes(std::span(token));
        if (!entry->ready || entry->token_used || Clock::now() > entry->token_expiry ||
            !beamit::net::constant_time_equal(expected, offered)) {
            return nullptr;
        }
        entry->token_used = true;
        if (entry->token_timer) entry->token_timer->cancel();
        return entry;
    }

    void attach(const std::shared_ptr<StreamEntry>& entry, std::shared_ptr<WebSocket> socket) {
        auto stream_socket = std::make_shared<StreamSocket>(std::move(socket), entry->id);
        stream_socket->on_disconnected = [weak_self = weak_from_this(), weak_entry = std::weak_ptr(entry)] {
            auto self = weak_self.lock();
            auto stream = weak_entry.lock();
            if (self && stream) self->end_stream(stream, "disconnected");
        };
        entry->socket = stream_socket;
        asio::co_spawn(stream_socket->ws->get_executor(), StreamSocket::read_loop(stream_socket), asio::detached);
        entry->pipeline->activate();
        spdlog::info("Stream {} connected", entry->id);
    }
};

void configure_websocket(WebSocket& ws, std::size_t max_message) {
    ws.set_option(websocket::stream_base::timeout{
        .handshake_timeout = 10s, .idle_timeout = 30s, .keep_alive_pings = true});
    ws.set_option(websocket::stream_base::decorator([](websocket::response_type& response) {
        response.set(http::field::server, "BeamIt");
    }));
    ws.read_message_max(max_message);
    ws.auto_fragment(false);
}

// CSP source for an inline block: 'sha256-<standard padded base64>' of the exact text.
std::string csp_hash(std::string_view text) {
    auto encoded = beamit::net::base64url_encode(beamit::net::sha256(std::as_bytes(std::span(text))));
    std::ranges::replace(encoded, '-', '+');
    std::ranges::replace(encoded, '_', '/');
    encoded.append((4 - encoded.size() % 4) % 4, '=');
    return "'sha256-" + encoded + "'";
}

// Allows exactly the embedded page's inline <script>/<style> blocks and style="" attributes, nothing else inline.
std::string make_content_security_policy(std::string_view page) {
    std::string scripts;
    std::string styles;
    std::string style_attributes;
    const auto collect = [&](std::string_view open, std::string_view close, std::string& out) {
        for (std::size_t at = page.find(open); at != std::string_view::npos; at = page.find(open, at + 1)) {
            const auto tag_end = page.find('>', at);
            const auto end = page.find(close, tag_end);
            if (tag_end == std::string_view::npos || end == std::string_view::npos) break;
            if (page.substr(at, tag_end - at).find(" src=") != std::string_view::npos) continue;
            out += ' ' + csp_hash(page.substr(tag_end + 1, end - tag_end - 1));
        }
    };
    collect("<script", "</script>", scripts);
    collect("<style", "</style>", styles);
    for (std::size_t at = page.find(" style=\""); at != std::string_view::npos; at = page.find(" style=\"", at + 1)) {
        const auto begin = at + 8;
        const auto end = page.find('"', begin);
        if (end == std::string_view::npos) break;
        style_attributes += ' ' + csp_hash(page.substr(begin, end - begin));
    }
    std::string policy = "default-src 'none'; script-src 'self'" + scripts + "; style-src 'self'" + styles;
    if (!style_attributes.empty()) policy += " 'unsafe-hashes'" + style_attributes;
    policy += "; connect-src 'self'; img-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'";
    return policy;
}

const std::string& content_security_policy() {
    static const std::string policy = make_content_security_policy(html_assets::index);
    return policy;
}

http::response<http::string_body> make_response(http::status status, unsigned version, bool keep_alive,
                                                std::string_view content_type, std::string body) {
    http::response<http::string_body> response{status, version};
    response.set(http::field::server, "BeamIt");
    response.set(http::field::content_type, content_type);
    response.set(http::field::cache_control, "no-store");
    response.set("X-Content-Type-Options", "nosniff");
    response.set("Referrer-Policy", "no-referrer");
    response.set("Content-Security-Policy", content_security_policy());
    response.keep_alive(keep_alive);
    response.body() = std::move(body);
    response.prepare_payload();
    return response;
}

awaitable<void> write_status(WebSocket& ws, const http::request<http::empty_body>& request, http::status status) {
    auto response = make_response(status, request.version(), false, "text/plain; charset=utf-8",
                                  std::string(http::obsolete_reason(status)) + "\n");
    co_await http::async_write(ws.next_layer(), response, use_awaitable);
}

awaitable<void> upgrade(Server& server, std::shared_ptr<WebSocket> ws, http::request<http::empty_body> request,
                        std::string remote) {
    const std::string target(request.target());
    const std::string host(request[http::field::host]);
    const std::string authority = valid_authority(host) ? host : fmt::format("localhost:{}", server.options.port);
    if (const auto origin = request[http::field::origin]; !origin.empty() && origin != "https://" + host) {
        spdlog::warn("Rejected WebSocket from {} with foreign origin {}", remote, std::string(origin));
        co_await write_status(*ws, request, http::status::forbidden);
        co_return;
    }
    if (target == "/control") {
        configure_websocket(*ws, protocol::max_signaling_message);
        co_await ws->async_accept(request, use_awaitable);
        spdlog::info("Control socket opened from {}", remote);
        co_await ControlSession::run(std::make_shared<ControlSession>(server, ws, authority, remote));
        co_return;
    }
    constexpr std::string_view prefix = "/streams/";
    const auto query = target.find("?token=");
    if (target.starts_with(prefix) && query != std::string::npos) {
        const std::string_view stream_id = std::string_view(target).substr(prefix.size(), query - prefix.size());
        const std::string_view token = std::string_view(target).substr(query + 7);
        auto session = server.active.lock();
        auto entry = session ? session->claim(stream_id, token) : nullptr;
        if (!entry) {
            spdlog::warn("Rejected stream socket from {}: invalid, used or expired token", remote);
            co_await write_status(*ws, request, http::status::forbidden);
            co_return;
        }
        configure_websocket(*ws, 1024);
        ws->binary(true);
        co_await ws->async_accept(request, use_awaitable);
        if (!session->streams.contains(entry->id)) {
            co_await ws->async_close({websocket::close_code::normal, "stream stopped"}, use_awaitable);
            co_return;
        }
        session->attach(entry, ws);
        co_return;
    }
    co_await write_status(*ws, request, http::status::not_found);
}

awaitable<void> serve_connection(Server& server, tcp::socket socket) {
    beast::error_code endpoint_error;
    const auto endpoint = socket.remote_endpoint(endpoint_error);
    const std::string remote = endpoint_error ? "unknown" : fmt::format("{}:{}", endpoint.address().to_string(),
                                                                        endpoint.port());
    socket.set_option(tcp::no_delay(true));
    auto ws = std::make_shared<WebSocket>(std::move(socket), server.tls);
    asio::steady_timer deadline(ws->get_executor());
    const auto arm = [&](Clock::duration timeout) {
        deadline.expires_after(timeout);
        deadline.async_wait([weak = std::weak_ptr(ws)](beast::error_code error) {
            auto stream = weak.lock();
            if (error || !stream) return;
            beast::error_code ignored;
            beast::get_lowest_layer(*stream).close(ignored);
        });
    };
    try {
        arm(10s);
        co_await ws->next_layer().async_handshake(wintls::handshake_type::server, use_awaitable);
        beast::flat_buffer buffer;
        while (true) {
            arm(30s);
            http::request_parser<http::empty_body> parser;
            parser.header_limit(8 * 1024);
            co_await http::async_read(ws->next_layer(), buffer, parser, use_awaitable);
            deadline.cancel();
            auto request = parser.release();
            if (websocket::is_upgrade(request)) {
                co_await upgrade(server, ws, std::move(request), remote);
                co_return;
            }
            const bool head = request.method() == http::verb::head;
            http::response<http::string_body> response;
            if (request.method() != http::verb::get && !head) {
                response = make_response(http::status::method_not_allowed, request.version(), false,
                                         "text/plain; charset=utf-8", "Method Not Allowed\n");
            } else if (request.target() == "/") {
                response = make_response(http::status::ok, request.version(), request.keep_alive(),
                                         "text/html; charset=utf-8", std::string(html_assets::index));
            } else if (request.target() == "/beamit.js") {
                response = make_response(http::status::ok, request.version(), request.keep_alive(),
                                         "text/javascript; charset=utf-8", std::string(html_assets::library));
            } else {
                response = make_response(http::status::not_found, request.version(), request.keep_alive(),
                                         "text/plain; charset=utf-8", "Not Found\n");
            }
            if (head) {
                const auto length = response.body().size();
                response.body().clear();
                response.content_length(length);
            }
            co_await http::async_write(ws->next_layer(), response, use_awaitable);
            if (!response.keep_alive()) break;
        }
        deadline.cancel();
        co_await ws->next_layer().async_shutdown(use_awaitable);
    } catch (const std::exception& error) {
        spdlog::debug("Connection {} ended: {}", remote, error.what());
    }
}

awaitable<void> accept_loop(Server& server, tcp::acceptor& acceptor) {
    while (acceptor.is_open()) {
        auto [error, socket] = co_await acceptor.async_accept(asio::as_tuple(use_awaitable));
        if (error) {
            if (error == asio::error::operation_aborted) co_return;
            spdlog::warn("Accept failed: {}", error.message());
            continue;
        }
        asio::co_spawn(acceptor.get_executor(), serve_connection(server, std::move(socket)), asio::detached);
    }
}

} // namespace

void run_host_server(HostServerOptions options) {
    // Pipeline threads join the MTA and exit with each stream. Without a process-wide MTA reference, COM tears the
    // MTA down after the last stream and unloads WinRT DLLs that C++/WinRT's factory cache still points into.
    CO_MTA_USAGE_COOKIE mta_cookie{};
    check_hresult(CoIncrementMTAUsage(&mta_cookie), "Keeping the COM multithreaded apartment alive");
    // Input mapping uses physical pixels for window rectangles, the cursor and the virtual desktop.
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        spdlog::warn("Could not enable per-monitor DPI awareness (error {}); remote input may be offset on scaled "
                     "displays", GetLastError());
    }
    Server server(std::move(options));
    server.tls.use_certificate(server.options.identity.certificate.get());
    const tcp::endpoint endpoint{asio::ip::make_address(server.options.bind_address), server.options.port};
    tcp::acceptor acceptor(server.io);
    acceptor.open(endpoint.protocol());
    // Prevent other processes from binding the same port (Windows SO_REUSEADDR allows hijacking).
    acceptor.set_option(asio::detail::socket_option::boolean<SOL_SOCKET, SO_EXCLUSIVEADDRUSE>(true));
    acceptor.bind(endpoint);
    acceptor.listen();
    spdlog::info("TLS: {}; SHA-256 fingerprint {}", server.options.identity.description,
                 server.options.identity.sha256_fingerprint);
    spdlog::info("Serving https://{}:{}/ (signaling wss://<host>:<port>/control); press Ctrl+C to stop",
                 server.options.bind_address, server.options.port);

    // Taskbar flash notifications arrive on the monitor's thread; track and forward them on the network thread.
    AttentionMonitor attention_monitor([&server](const AttentionEvent& event) {
        asio::post(server.io, [&server, event] {
            spdlog::debug("Attention {} for window {} (pid {})", event.requested ? "set" : "clear",
                          hwnd_text(event.window), event.pid);
            if (event.requested) {
                server.attention[event.window] = event;
            } else {
                server.attention.erase(event.window);
            }
            if (auto session = server.active.lock()) session->on_attention(event);
        });
    });

    asio::signal_set signals(server.io, SIGINT, SIGTERM);
    signals.async_wait([&](beast::error_code error, int) {
        if (error) return;
        spdlog::info("Shutting down");
        beast::error_code ignored;
        acceptor.close(ignored);
        // Join every pipeline before the io_context stops accepting their posted callbacks.
        if (auto session = server.active.lock()) session->close(websocket::close_code::going_away, "host shutting down");
        server.io.stop();
    });
    asio::co_spawn(server.io, accept_loop(server, acceptor), asio::detached);
    server.io.run();
}

} // namespace beamit::host
