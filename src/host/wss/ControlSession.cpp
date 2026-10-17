#include "host/wss/ControlSession.hpp"

#include "ApplicationWindows.hpp"
#include "media/BitstreamInfo.hpp"
#include "net/Crypto.hpp"

#include <shellscalingapi.h>
#include <spdlog/spdlog.h>

#include <map>

namespace beamit::host::wss {
namespace {

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

// The capture target and backend of a `start_stream` source (PROTOCOL.md §5.3), plus its description for the offer.
struct StreamSource {
    capture::CaptureTarget target{};
    CaptureBackend backend{};
    DWORD pid{};
    json::object description;
};

StreamSource parse_source(const json::object& message) {
    const auto* source_field = message.if_contains("source");
    if (!source_field || !source_field->is_object() || !source_field->as_object().contains("kind") ||
        !source_field->as_object().at("kind").is_string()) {
        throw ProtocolError{"invalid_request", "source.kind is required"};
    }
    const auto& source = source_field->as_object();
    const std::string_view kind = source.at("kind").as_string();
    StreamSource result{.description = {{"kind", kind}}};
    // Windows need WGC; whole-desktop streams default to DXGI Desktop Duplication.
    result.backend = kind == "desktop" ? CaptureBackend::Dxgi : CaptureBackend::Wgc;
    if (const auto* capture_field = source.if_contains("capture")) {
        if (capture_field->is_string() && capture_field->as_string() == "wgc") {
            result.backend = CaptureBackend::Wgc;
        } else if (capture_field->is_string() && capture_field->as_string() == "dxgi" && kind == "desktop") {
            result.backend = CaptureBackend::Dxgi;
        } else {
            throw ProtocolError{"invalid_request",
                                "source.capture must be \"dxgi\" or \"wgc\" (window sources: \"wgc\" only)"};
        }
    }
    if (kind == "window" || kind == "application") {
        HWND window{};
        if (kind == "window") {
            window = required_hwnd(source, "hwnd");
        } else {
            const auto pid = static_cast<DWORD>(required_integer(source, "pid", 1, 0xFFFFFFFFLL));
            try {
                window = select_application_window(pid);
            } catch (const std::exception&) {
                throw ProtocolError{"not_found", "no visible window for that pid"};
            }
        }
        if (!IsWindow(window) || GetAncestor(window, GA_ROOT) != window) {
            throw ProtocolError{"not_found", "not a valid top-level window"};
        }
        // Minimized windows are restored by the pipeline before capture starts (StreamPipeline::restore_window).
        if (!IsWindowVisible(window)) {
            throw ProtocolError{"capture_failed", "the window must be visible"};
        }
        GetWindowThreadProcessId(window, &result.pid);
        result.target.kind = capture::CaptureTargetKind::Window;
        result.target.window = window;
        result.description["hwnd"] = hwnd_text(window);
        result.description["pid"] = result.pid;
        result.description["title"] = window_title(window);
    } else if (kind == "desktop") {
        result.target.kind = capture::CaptureTargetKind::Desktop;
    } else {
        throw ProtocolError{"invalid_request", "source.kind must be window, application or desktop"};
    }

    const auto monitor = result.target.window ? MonitorFromWindow(result.target.window, MONITOR_DEFAULTTONEAREST)
                                              : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    UINT dpi_x{};
    UINT dpi_y{};
    const auto dpi_result = GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y);
    if (FAILED(dpi_result) || dpi_x == 0 || dpi_y == 0) {
        throw ProtocolError{"capture_failed", fmt::format("could not query target display DPI (HRESULT 0x{:08X})",
                                                         static_cast<std::uint32_t>(dpi_result))};
    }
    result.description["dpi"] = json::object{{"x", dpi_x}, {"y", dpi_y}};
    result.description["capture"] = result.backend == CaptureBackend::Dxgi ? "dxgi" : "wgc";
    return result;
}

media::EncoderOptions parse_video(const json::object& message, const HostServerOptions& defaults) {
    media::EncoderOptions options{.frame_rate = defaults.frame_rate,
                                  .target_bitrate_kbps = defaults.bitrate_kbps,
                                  .codec = defaults.codec};
    const auto* video = optional_object(message, "video");
    if (!video) return options;
    if (const auto* codec = video->if_contains("codec")) {
        if (codec->is_string() && codec->as_string() == "h264") {
            options.codec = media::VideoCodec::H264;
        } else if (codec->is_string() && codec->as_string() == "h265") {
            options.codec = media::VideoCodec::H265;
        } else {
            throw ProtocolError{"invalid_request", "video.codec must be h264 or h265"};
        }
    }
    options.frame_rate =
        static_cast<std::uint32_t>(optional_integer(*video, "frame_rate", 1, 240).value_or(options.frame_rate));
    options.target_bitrate_kbps = static_cast<std::uint32_t>(
        optional_integer(*video, "bitrate_kbps", 1, 1'000'000).value_or(options.target_bitrate_kbps));
    return options;
}

} // namespace

void StreamEntry::shut_down(websocket::close_reason reason) {
    if (token_timer) token_timer->cancel();
    input->release_all();
    pipeline->stop();
    if (socket) socket->close(std::move(reason));
}

ControlSession::ControlSession(Server& host, std::shared_ptr<WebSocket> socket, std::string url_authority,
                               std::string peer)
    : server_(host), ws_(std::move(socket)), authority_(std::move(url_authority)), remote_(std::move(peer)),
      server_nonce_(beamit::net::random_bytes(protocol::nonce_bytes)) {}

// --- Outgoing messages ---------------------------------------------------------------------------------------------

void ControlSession::send(json::object message) { send_serialized(json::serialize(message)); }

void ControlSession::send_serialized(std::string message) {
    if (closing_) return;
    outbox_.emplace_back(std::move(message));
    pump();
}

void ControlSession::reply(const RequestId& id, json::object message) {
    if (id) message["id"] = *id;
    send(std::move(message));
}

std::function<void(json::object)> ControlSession::deferred_reply(RequestId id) {
    return [executor = ws_->get_executor(), weak_self = weak_from_this(), id = std::move(id)](json::object message) {
        if (id) message["id"] = *id;
        asio::post(executor, [weak_self, text = json::serialize(message)]() mutable {
            if (auto self = weak_self.lock()) self->send_serialized(std::move(text));
        });
    };
}

void ControlSession::send_error(const RequestId& id, std::string_view code, std::string_view message,
                                std::string_view stream_id) {
    json::object error{{"type", "error"}, {"code", code}, {"message", message}};
    if (!stream_id.empty()) error["stream_id"] = stream_id;
    reply(id, std::move(error));
}

void ControlSession::close(websocket::close_code code, std::string_view reason) {
    if (closing_) return;
    closing_ = true;
    stop_all_streams(websocket::close_code::going_away);
    outbox_.emplace_back(websocket::close_reason{code, reason});
    pump();
}

void ControlSession::pump() {
    if (writing_ || outbox_.empty()) return;
    writing_ = true;
    asio::co_spawn(ws_->get_executor(), write_loop(shared_from_this()), asio::detached);
}

awaitable<void> ControlSession::write_loop(std::shared_ptr<ControlSession> self) {
    try {
        while (!self->outbox_.empty()) {
            auto item = std::move(self->outbox_.front());
            self->outbox_.pop_front();
            if (auto* text = std::get_if<std::string>(&item)) {
                self->ws_->text(true);
                co_await self->ws_->async_write(asio::buffer(*text), use_awaitable);
            } else {
                co_await self->ws_->async_close(std::get<websocket::close_reason>(item), use_awaitable);
                self->outbox_.clear();
            }
        }
    } catch (const std::exception& error) {
        spdlog::debug("Control write ended: {}", error.what());
        self->outbox_.clear();
        beast::error_code ignored;
        beast::get_lowest_layer(*self->ws_).close(ignored);
    }
    self->writing_ = false;
}

// --- Session lifecycle and dispatch --------------------------------------------------------------------------------

awaitable<void> ControlSession::run(std::shared_ptr<ControlSession> self) {
    const auto& options = self->server_.options;
    self->send({
        {"type", "hello"},
        {"protocol", protocol::version},
        {"server", "BeamIt"},
        {"auth", {
            {"mechanism", "pbkdf2-sha256-hmac"},
            {"salt", bytes_text(options.auth_salt)},
            {"iterations", options.auth_iterations},
            {"server_nonce", bytes_text(self->server_nonce_)},
        }},
    });
    auto auth_timer = std::make_shared<asio::steady_timer>(self->ws_->get_executor(), protocol::auth_timeout);
    auth_timer->async_wait([weak = std::weak_ptr(self)](beast::error_code error) {
        auto session = weak.lock();
        if (error || !session || session->authenticated_) return;
        spdlog::info("Control client {} did not authenticate in time", session->remote_);
        beast::error_code ignored;
        beast::get_lowest_layer(*session->ws_).close(ignored);
    });

    beast::flat_buffer buffer;
    try {
        while (true) {
            buffer.clear();
            co_await self->ws_->async_read(buffer, use_awaitable);
            if (self->closing_ || self->auth_failed_) continue;
            if (!self->ws_->got_text()) {
                self->close(websocket::close_code::bad_payload, "binary messages are not allowed");
                continue;
            }
            self->handle(beast::buffers_to_string(buffer.data()));
        }
    } catch (const std::exception& error) {
        spdlog::debug("Control socket {} closed: {}", self->remote_, error.what());
    }
    auth_timer->cancel();
    self->closing_ = true;
    self->stop_all_streams(websocket::close_code::going_away);
    if (self->server_.active.lock() == self) self->server_.active.reset();
    if (self->authenticated_) spdlog::info("Control client {} disconnected", self->remote_);
}

void ControlSession::handle(const std::string& text) {
    beast::error_code parse_error;
    auto value = json::parse(text, parse_error);
    if (parse_error || !value.is_object() || !value.as_object().contains("type") ||
        !value.as_object().at("type").is_string()) {
        if (!authenticated_) return reject_unauthenticated("invalid_request", "expected an authenticate message");
        return send_error(std::nullopt, "invalid_request", "messages must be JSON objects with a string type");
    }
    auto& message = value.as_object();
    const std::string_view type = message.at("type").as_string();
    RequestId id;
    if (const auto* field = message.if_contains("id")) {
        constexpr std::int64_t limit = 1LL << 53;
        if (field->is_string() || (field->is_int64() && field->as_int64() > -limit && field->as_int64() < limit) ||
            (field->is_uint64() && field->as_uint64() < static_cast<std::uint64_t>(limit))) {
            id = *field;
        } else {
            return send_error(std::nullopt, "invalid_request", "id must be a string or a safe integer");
        }
    }
    if (!authenticated_) {
        if (type != "authenticate") return reject_unauthenticated("not_authenticated", "authenticate first");
        return authenticate(message);
    }

    static const std::map<std::string_view, Handler> handlers{
        {"ping", &ControlSession::ping},
        {"list_windows", &ControlSession::list_windows},
        {"get_window_icon", &ControlSession::get_window_icon},
        {"get_clipboard", &ControlSession::get_clipboard},
        {"set_clipboard", &ControlSession::set_clipboard},
        {"start_stream", &ControlSession::start_stream},
        {"stop_stream", &ControlSession::stop_stream},
        {"request_key_frame", &ControlSession::request_key_frame},
        {"set_stream_rate", &ControlSession::set_stream_rate},
        // §5.9 input
        {"set_mouse_mode", &ControlSession::handle_input},
        {"mouse_move", &ControlSession::handle_input},
        {"mouse_button", &ControlSession::handle_input},
        {"mouse_wheel", &ControlSession::handle_input},
        {"key", &ControlSession::handle_input},
        {"text", &ControlSession::handle_input},
        {"release_all", &ControlSession::handle_input},
        {"get_input_state", &ControlSession::handle_input},
    };
    try {
        if (type == "authenticate") throw ProtocolError{"invalid_request", "already authenticated"};
        const auto handler = handlers.find(type);
        if (handler == handlers.end()) {
            throw ProtocolError{"unknown_type", fmt::format("unknown message type '{}'", type)};
        }
        (this->*handler->second)(message, id);
    } catch (const ProtocolError& error) {
        send_error(id, error.code, error.message);
    } catch (const std::exception& error) {
        spdlog::error("Request '{}' failed: {}", type, error.what());
        send_error(id, "internal_error", error.what());
    }
}

void ControlSession::reject_unauthenticated(std::string_view code, std::string_view message) {
    send_error(std::nullopt, code, message);
    close(websocket::close_code::policy_error, message);
}

void ControlSession::authenticate(const json::object& message) {
    const auto* version = message.if_contains("protocol");
    if (!version || !version->is_int64() || version->as_int64() != protocol::version) {
        send_error(std::nullopt, "protocol_version", "this host supports protocol version 1");
        close(websocket::close_code::protocol_error, "unsupported protocol version");
        return;
    }
    const auto& key = server_.options.auth_key;
    const auto client_nonce = base64url_field(message, "client_nonce");
    const auto proof = base64url_field(message, "proof");
    bool valid = client_nonce && proof && client_nonce->size() >= 16 && client_nonce->size() <= 64;
    if (valid) {
        const auto expected = beamit::net::auth_proof(key, "client", server_nonce_, *client_nonce);
        valid = beamit::net::constant_time_equal(expected, *proof);
    }
    if (!valid) {
        spdlog::warn("Authentication failed from {}", remote_);
        auth_failed_ = true;
        send_error(std::nullopt, "auth_failed", "authentication failed");
        auto delay = std::make_shared<asio::steady_timer>(ws_->get_executor(), 1s);
        delay->async_wait([self = shared_from_this(), delay](beast::error_code) {
            self->close(websocket::close_code::policy_error, "authentication failed");
        });
        return;
    }
    authenticated_ = true;
    if (auto previous = server_.active.lock(); previous && previous.get() != this) {
        spdlog::info("Control client {} supersedes {}", remote_, previous->remote_);
        previous->close(static_cast<websocket::close_code>(protocol::close_superseded),
                        "superseded by a newer session");
    }
    server_.active = shared_from_this();
    ws_->read_message_max(protocol::max_authenticated_signaling_message);
    const auto server_proof = beamit::net::auth_proof(key, "server", server_nonce_, *client_nonce);
    send({{"type", "authenticated"}, {"server_proof", bytes_text(server_proof)},
          {"host_time_us", protocol::host_time_us(Clock::now())}});
    spdlog::info("Control client {} authenticated", remote_);
    for (const auto& window : server_.windows.windows()) {
        send_window_event({.kind = WindowEvent::Kind::Opened, .window = window});
    }
}

void ControlSession::ping(const json::object&, const RequestId& id) {
    reply(id, {{"type", "pong"}, {"host_time_us", protocol::host_time_us(Clock::now())}});
}

// --- Streams -------------------------------------------------------------------------------------------------------

std::shared_ptr<StreamEntry> ControlSession::find_stream(const json::object& message) {
    const auto* field = message.if_contains("stream_id");
    if (!field || !field->is_string()) throw ProtocolError{"invalid_request", "stream_id is required"};
    const auto found = streams_.find(std::string(field->as_string()));
    if (found == streams_.end()) throw ProtocolError{"not_found", "unknown stream_id"};
    return found->second;
}

void ControlSession::start_stream(const json::object& message, const RequestId& id) {
    if (streams_.size() >= protocol::max_streams_per_session) {
        throw ProtocolError{"limit_reached", "too many concurrent streams"};
    }
    auto source = parse_source(message);
    const auto options = parse_video(message, server_.options);
    auto mouse_mode = input::MouseMode::Absolute;
    if (const auto* input_field = optional_object(message, "input")) {
        if (const auto* mode = input_field->if_contains("mouse_mode")) mouse_mode = mouse_mode_field(*mode);
    }

    std::unique_ptr<capture::Capturer> capturer;
    std::unique_ptr<media::Encoder> encoder;
    try {
        capturer = server_.options.make_capturer(source.backend);
    } catch (const std::exception& error) {
        throw ProtocolError{"capture_failed", error.what()};
    }
    try {
        encoder = server_.options.make_encoder(options.codec);
    } catch (const std::exception& error) {
        throw ProtocolError{"encoder_failed", error.what()};
    }

    auto entry = std::make_shared<StreamEntry>();
    entry->id = bytes_text(beamit::net::random_bytes(16));
    entry->token = bytes_text(beamit::net::random_bytes(32));
    entry->request_id = id;
    entry->source = std::move(source.description);
    entry->pipeline =
        std::make_unique<StreamPipeline>(std::move(capturer), std::move(encoder), source.target, options);
    entry->input = std::make_unique<input::StreamInput>(source.target.window);
    entry->input->mouse_mode = mouse_mode;
    entry->pid = source.pid;
    streams_.emplace(entry->id, entry);
    spdlog::info("Starting stream {} for {}", entry->id, json::serialize(entry->source));
    start_pipeline(entry);
}

void ControlSession::start_pipeline(const std::shared_ptr<StreamEntry>& entry) {
    // Pipeline callbacks run on its thread; hop to the network thread and drop events of streams that have ended.
    const auto on_network = [executor = ws_->get_executor(), weak_self = weak_from_this(),
                             weak_entry = std::weak_ptr(entry)](auto&& function) {
        asio::post(executor, [weak_self, weak_entry, function = std::forward<decltype(function)>(function)] {
            auto self = weak_self.lock();
            auto stream = weak_entry.lock();
            if (self && stream && self->streams_.contains(stream->id)) function(*self, stream);
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
                self.streams_.erase(stream->id);
                stream->shut_down({websocket::close_code::normal, "stream failed"});
                self.send_error(stream->request_id, code, text);
            });
        },
        .reconfigured = [on_network](const VideoParameters& video) {
            on_network([video](ControlSession& self, const std::shared_ptr<StreamEntry>& stream) {
                stream->video = video;
                self.send({{"type", "stream_config"}, {"stream_id", stream->id}, {"video", video_json(video)}});
            });
        },
        .rate_changed = [on_network](const VideoParameters& video, std::uint64_t serial, RateChangeResult result) {
            on_network([video, serial, result](ControlSession& self, const std::shared_ptr<StreamEntry>& stream) {
                stream->video = video;
                const char* applied = result == RateChangeResult::InPlace        ? "in_place"
                                      : result == RateChangeResult::Reconfigured ? "reconfigured"
                                                                                 : "unchanged";
                // Merged requests are applied together, so every request up to the serial is answered.
                std::erase_if(stream->rate_requests, [&](const auto& request) {
                    if (request.first > serial) return false;
                    self.reply(request.second, {{"type", "stream_rate"}, {"stream_id", stream->id},
                                                {"applied", applied}, {"generation", video.generation},
                                                {"frame_rate", video.frame_rate},
                                                {"bitrate_kbps", video.bitrate_kbps}});
                    return true;
                });
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

void ControlSession::offer(const std::shared_ptr<StreamEntry>& entry, const VideoParameters& video) {
    entry->video = video;
    entry->ready = true;
    entry->token_expiry = Clock::now() + protocol::token_lifetime;
    entry->token_timer = std::make_unique<asio::steady_timer>(ws_->get_executor(), protocol::token_lifetime);
    entry->token_timer->async_wait(
        [weak_self = weak_from_this(), weak_entry = std::weak_ptr(entry)](beast::error_code error) {
            auto self = weak_self.lock();
            auto stream = weak_entry.lock();
            if (error || !self || !stream || stream->token_used) return;
            self->end_stream(stream, "token_expired");
        });
    reply(entry->request_id, {
        {"type", "stream_offer"},
        {"stream_id", entry->id},
        {"url", fmt::format("wss://{}/streams/{}?token={}", authority_, entry->id, entry->token)},
        {"token_expires_in_ms", std::chrono::milliseconds(protocol::token_lifetime).count()},
        {"source", entry->source},
        {"video", video_json(video)},
        {"input", {{"mouse_mode", input::mouse_mode_name(entry->input->mouse_mode)}}},
    });
    for (const auto& [window, event] : server_.attention) send_attention(*entry, event);
}

void ControlSession::stop_stream(const json::object& message, const RequestId& id) {
    end_stream(find_stream(message), "requested", {}, id);
}

void ControlSession::request_key_frame(const json::object& message, const RequestId&) {
    find_stream(message)->pipeline->request_key_frame();
}

void ControlSession::set_stream_rate(const json::object& message, const RequestId& id) {
    const auto stream = find_stream(message);
    const auto bitrate = optional_integer(message, "bitrate_kbps", 1, 1'000'000);
    const auto frame_rate = optional_integer(message, "frame_rate", 1, 240);
    if (!bitrate && !frame_rate) {
        throw ProtocolError{"invalid_request", "set_stream_rate needs bitrate_kbps or frame_rate"};
    }
    const auto to_u32 = [](std::optional<std::int64_t> value) -> std::optional<std::uint32_t> {
        if (!value) return std::nullopt;
        return static_cast<std::uint32_t>(*value);
    };
    const auto serial = stream->pipeline->request_rate_change(to_u32(bitrate), to_u32(frame_rate));
    stream->rate_requests.emplace_back(serial, id);
}

void ControlSession::end_stream(const std::shared_ptr<StreamEntry>& entry, std::string_view reason,
                                std::string_view message, const RequestId& id) {
    if (streams_.erase(entry->id) == 0) return;
    entry->shut_down({websocket::close_code::normal, reason});
    json::object stopped{{"type", "stream_stopped"}, {"stream_id", entry->id}, {"reason", reason}};
    if (!message.empty()) stopped["message"] = message;
    reply(id, std::move(stopped));
    spdlog::info("Stream {} stopped: {}{}{}", entry->id, reason, message.empty() ? "" : " - ", message);
}

void ControlSession::stop_all_streams(websocket::close_code code) {
    for (auto& [id, entry] : std::exchange(streams_, {})) entry->shut_down({code, "signaling session ended"});
}

std::shared_ptr<StreamEntry> ControlSession::claim(std::string_view stream_id, std::string_view token) {
    if (!authenticated_ || closing_) return nullptr;
    const auto found = streams_.find(std::string(stream_id));
    if (found == streams_.end()) return nullptr;
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

void ControlSession::attach(const std::shared_ptr<StreamEntry>& entry, std::shared_ptr<WebSocket> socket) {
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

} // namespace beamit::host::wss
