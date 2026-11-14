#include "ClientSession.hpp"

// Boost.Asio/Beast must be included before boost-wintls.
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <wintls.hpp>
#include <wintls/beast.hpp>

#include "net/Protocol.hpp"
#include "net/ClipboardBulk.hpp"

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace beamit::client {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using TlsStream = wintls::stream<tcp::socket>;
using WebSocket = websocket::stream<TlsStream>;
using asio::awaitable;
using asio::use_awaitable;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// Clipboard replies may need up to six bytes per content byte after JSON escaping (PROTOCOL.md §5.11).
constexpr std::size_t control_receive_limit = 64ULL * 1024 * 1024;
constexpr std::size_t stream_receive_limit = 32ULL * 1024 * 1024;
constexpr std::size_t bulk_receive_limit = 40 + 256 * 1024;
constexpr std::size_t bulk_queued_limit = 1024 * 1024;
constexpr std::size_t max_queued_bytes = 64ULL * 1024 * 1024;
constexpr std::size_t max_held_frames = 16;
constexpr std::size_t max_held_bytes = 64ULL * 1024 * 1024;
constexpr auto request_timeout = 10s;
constexpr char user_agent[] = "BeamIt-Windows/1";

struct Failure : std::runtime_error {
    Failure(std::string code, std::string message) : std::runtime_error(message), error{std::move(code), std::move(message)} {}
    SessionError error;
};

SessionError describe(std::exception_ptr exception, bool timed_out) {
    if (timed_out) return {"timeout", "The host did not respond in time."};
    try {
        std::rethrow_exception(exception);
    } catch (const Failure& failure) {
        return failure.error;
    } catch (const boost::system::system_error& error) {
        const auto code = error.code();
        if (code == asio::error::connection_refused) {
            return {"connection_refused", "The host refused the connection. Check that BeamIt runs on this port."};
        }
        if (code == asio::error::host_not_found || code == asio::error::host_not_found_try_again) {
            return {"host_not_found", "The host name could not be resolved."};
        }
        if (code == websocket::error::closed) return {"disconnected", "The host closed the connection."};
        // boost-wintls reports SSPI SECURITY_STATUS values (0x8009xxxx) in the system category.
        if (code.category() == boost::system::system_category() &&
            (static_cast<std::uint32_t>(code.value()) & 0xFFFF0000U) == 0x80090000U) {
            return {"tls_failed", "The secure connection failed: " + code.message()};
        }
        return {"network_error", "Network error: " + code.message()};
    } catch (const std::invalid_argument& error) {
        return {"protocol_error", std::string("The host sent invalid data: ") + error.what()};
    } catch (const std::exception& error) {
        return {"internal_error", error.what()};
    }
}

// Closes sockets when a deadline expires; never touches them after destruction.
class Deadline {
public:
    Deadline(asio::io_context& io, Clock::duration duration, std::function<void()> expire)
        : shared_(std::make_shared<Shared>(io)) {
        shared_->expire = std::move(expire);
        shared_->timer.expires_after(duration);
        shared_->timer.async_wait([shared = shared_](beast::error_code error) {
            if (error || !shared->expire) return;
            shared->expired = true;
            std::exchange(shared->expire, nullptr)();
        });
    }
    ~Deadline() {
        shared_->expire = nullptr;
        shared_->timer.cancel();
    }
    Deadline(const Deadline&) = delete;
    Deadline& operator=(const Deadline&) = delete;
    [[nodiscard]] bool expired() const { return shared_->expired; }

private:
    struct Shared {
        explicit Shared(asio::io_context& io) : timer(io) {}
        asio::steady_timer timer;
        std::function<void()> expire;
        bool expired{};
    };
    std::shared_ptr<Shared> shared_;
};

void close_transport(WebSocket& socket) {
    beast::error_code ignored;
    beast::get_lowest_layer(socket).shutdown(tcp::socket::shutdown_both, ignored);
    beast::get_lowest_layer(socket).close(ignored);
}

bool is_ip_literal(const std::string& host) {
    beast::error_code error;
    (void)asio::ip::make_address(host, error);
    return !error;
}

bool valid_bulk_target(std::string_view target) {
    if (!target.starts_with("/bulk/") || target.find("://") != std::string_view::npos ||
        target.find('#') != std::string_view::npos) return false;
    const auto query = target.find('?');
    if (query == std::string_view::npos || target.substr(query, 7) != "?token=" ||
        target.find('?', query + 1) != std::string_view::npos) return false;
    const auto id = target.substr(6, query - 6);
    const auto token = target.substr(query + 7);
    const auto valid_token = [](std::string_view value, std::size_t length) {
        return value.size() == length && std::ranges::all_of(value, [](char character) {
            return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
                   (character >= '0' && character <= '9') || character == '-' || character == '_';
        });
    };
    return valid_token(id, 22) && valid_token(token, 43);
}

net::Sha256 peer_fingerprint(TlsStream& stream) {
    const CERT_CONTEXT* certificate{};
    if (QueryContextAttributesW(stream.native_handle(), SECPKG_ATTR_REMOTE_CERT_CONTEXT, &certificate) != SEC_E_OK ||
        !certificate) {
        throw Failure("certificate_unavailable", "The host did not present a TLS certificate.");
    }
    const std::unique_ptr<const CERT_CONTEXT, decltype(&CertFreeCertificateContext)> owner(
        certificate, CertFreeCertificateContext);
    return net::sha256({reinterpret_cast<const std::byte*>(certificate->pbCertEncoded), certificate->cbCertEncoded});
}

struct PinPolicy {
    std::optional<net::Sha256> expected;
    bool trust_any{};
};

// TCP, TLS, certificate pin and WebSocket upgrade. The pin is checked before any HTTP byte is sent.
awaitable<net::Sha256> establish(WebSocket& socket, tcp::resolver& resolver, const Endpoint& endpoint,
                                 const PinPolicy& pin, const std::string& target, std::size_t message_limit) {
    const auto results = co_await resolver.async_resolve(endpoint.host, std::to_string(endpoint.port), use_awaitable);
    co_await asio::async_connect(beast::get_lowest_layer(socket), results, use_awaitable);
    beast::get_lowest_layer(socket).set_option(tcp::no_delay(true));
    if (!is_ip_literal(endpoint.host)) socket.next_layer().set_server_hostname(endpoint.host);
    co_await socket.next_layer().async_handshake(wintls::handshake_type::client, use_awaitable);
    const auto presented = peer_fingerprint(socket.next_layer());
    if (!pin.trust_any && (!pin.expected || !net::constant_time_equal(presented, *pin.expected))) {
        close_transport(socket);
        throw Failure("certificate_mismatch",
            "The host certificate does not match the pinned fingerprint. Presented SHA-256: " +
            format_fingerprint(presented));
    }
    socket.read_message_max(message_limit);
    socket.set_option(websocket::stream_base::decorator([](websocket::request_type& request) {
        request.set(beast::http::field::user_agent, user_agent);
    }));
    // Idle liveness uses signaling pings (State::clock_loop); a Beast idle timer would outlive closed sockets.
    socket.set_option(websocket::stream_base::timeout{15s, websocket::stream_base::none(), false});
    co_await socket.async_handshake(endpoint.authority, target, use_awaitable);
    co_return presented;
}

awaitable<json::object> read_json(WebSocket& socket, std::size_t limit) {
    beast::flat_buffer buffer;
    co_await socket.async_read(buffer, use_awaitable);
    if (!socket.got_text()) throw Failure("protocol_error", "The host sent a binary signaling message.");
    const auto size = buffer.size();
    const std::string_view text(static_cast<const char*>(buffer.data().data()), size);
    beast::error_code error;
    auto value = json::parse(text, error);
    if (error || !value.is_object() || !string_field(value.get_object(), "type")) {
        throw Failure("protocol_error", "The host sent an invalid signaling message.");
    }
    if (size > limit && string_field(value.get_object(), "type") != "clipboard") {
        throw Failure("protocol_error", "The host sent an oversized signaling message.");
    }
    co_return std::move(value.get_object());
}

SessionError close_error(WebSocket& socket) {
    const auto& reason = socket.reason();
    if (reason.code == net::protocol::close_superseded) {
        return {"superseded", "Another client connected to this host instance, so this session was closed."};
    }
    if (reason.code == websocket::close_code::policy_error) {
        return {"policy", reason.reason.empty() ? std::string("The host closed the session.") : std::string(reason.reason)};
    }
    return {"disconnected", "The connection to the host was lost."};
}

} // namespace

struct Session::State {
    enum class Phase { Idle, Connecting, Authenticated, Closed };

    struct Pending {
        std::string expected;
        std::function<void(std::expected<json::object, SessionError>)> done; // Runs on the network thread.
        std::unique_ptr<asio::steady_timer> timer;
        Clock::time_point sent;
    };

    struct Stream {
        Stream(asio::io_context& io, wintls::context& tls, const StreamOffer& offer, StreamCallbacks callbacks)
            : socket(io, tls), resolver(io), id(offer.stream_id), target(offer.target), lifetime(offer.token_lifetime),
              video(offer.video), gate(offer.video.generation), callbacks(std::move(callbacks)) {}
        WebSocket socket;
        tcp::resolver resolver;
        std::string id;
        std::string target;
        std::chrono::milliseconds lifetime;
        VideoParameters video;
        FrameGate gate;
        StreamCallbacks callbacks;
        std::deque<std::vector<std::byte>> held;
        std::size_t held_bytes{};
        bool held_overflow{};
        bool closing{};
    };

    struct Bulk {
        struct Write {
            std::vector<std::byte> bytes;
            std::function<void(std::expected<void, SessionError>)> done;
        };
        Bulk(asio::io_context& io, wintls::context& tls, std::string target,
             std::chrono::milliseconds lifetime, BulkCallbacks callbacks)
            : socket(io, tls), resolver(io), target(std::move(target)), lifetime(lifetime),
              callbacks(std::move(callbacks)) {}
        WebSocket socket;
        tcp::resolver resolver;
        std::string target;
        std::chrono::milliseconds lifetime;
        BulkCallbacks callbacks;
        std::deque<Write> writes;
        std::size_t queued_bytes{};
        bool writing{};
        bool closing{};
    };

    State(Dispatcher dispatcher_, Events events_)
        : dispatcher(std::move(dispatcher_)), events(std::move(events_)), clock_timer(io) {
        tls.verify_server_certificate(false);
        thread = std::thread([this] { io.run(); });
    }

    template<class Function>
    void post_ui(Function&& function) {
        dispatcher([alive = alive, function = std::forward<Function>(function)]() mutable {
            if (alive->load()) function();
        });
    }

    template<class T>
    void complete(Completion<T> done, std::expected<T, SessionError> result) {
        if (!done) return;
        post_ui([done = std::move(done), result = std::move(result)]() mutable { done(std::move(result)); });
    }

    awaitable<void> run_connect(ConnectOptions options) {
        auto endpoint_value = parse_server_url(options.server_url);
        if (!endpoint_value) throw Failure("invalid_url", endpoint_value.error());
        PinPolicy pin{std::nullopt, options.trust_any_certificate};
        if (!pin.trust_any) {
            pin.expected = parse_fingerprint(options.fingerprint);
            if (!pin.expected) {
                throw Failure("invalid_fingerprint",
                    "Enter the host certificate's SHA-256 fingerprint (64 hexadecimal digits) from the host log.");
            }
        }
        {
            std::scoped_lock lock(mutex);
            endpoint = *endpoint_value;
            pin_policy = pin;
        }
        Deadline deadline(io, options.timeout, [this] {
            connect_resolver.cancel();
            close_transport(*control);
        });
        try {
            const auto presented = co_await establish(
                *control, connect_resolver, *endpoint_value, pin, "/control", net::protocol::max_signaling_message);
            require_connecting();
            {
                std::scoped_lock lock(mutex);
                fingerprint = format_fingerprint(presented);
            }
            const auto hello = co_await read_json(*control, net::protocol::max_signaling_message);
            require_connecting();
            const auto challenge = parse_hello(hello);
            if (!challenge) throw Failure("invalid_challenge", challenge.error());
            auto exchange = create_authentication(*challenge, options.password);
            SecureZeroMemory(options.password.data(), options.password.size());
            control->text(true);
            co_await control->async_write(asio::buffer(json::serialize(exchange.message)), use_awaitable);
            const auto reply = co_await read_json(*control, net::protocol::max_signaling_message);
            require_connecting();
            if (string_field(reply, "type") == "error") {
                const auto code = std::string(string_field(reply, "code").value_or("auth_failed"));
                throw Failure(code, code == "auth_failed" ? std::string("The host rejected the password.")
                    : std::string(string_field(reply, "message").value_or("The host rejected authentication.")));
            }
            const auto proof = string_field(reply, "server_proof");
            const auto decoded = proof ? net::base64url_decode(*proof) : std::nullopt;
            if (string_field(reply, "type") != "authenticated" || !decoded ||
                !net::constant_time_equal(*decoded, exchange.expected_server_proof)) {
                throw Failure("server_proof_mismatch",
                    "The host could not prove that it knows the password. The connection was closed.");
            }
        } catch (...) {
            SecureZeroMemory(options.password.data(), options.password.size());
            if (deadline.expired()) throw Failure("timeout", "The host did not respond in time.");
            throw;
        }
        require_connecting();
        control->read_message_max(control_receive_limit);
        phase = Phase::Authenticated;
        asio::co_spawn(io, read_loop(), asio::detached);
        asio::co_spawn(io, clock_loop(), asio::detached);
    }

    void require_connecting() const {
        if (phase != Phase::Connecting) throw Failure("disconnected", "The connection attempt was cancelled.");
    }

    awaitable<void> read_loop() {
        try {
            while (phase == Phase::Authenticated) {
                auto message = co_await read_json(*control, net::protocol::max_authenticated_signaling_message);
                handle(std::move(message));
            }
        } catch (...) {
            if (phase != Phase::Authenticated) co_return;
            auto error = describe(std::current_exception(), false);
            if (error.code == "disconnected") error = close_error(*control);
            end_session(std::move(error), true);
        }
    }

    awaitable<void> clock_loop() {
        for (unsigned count = 0; phase == Phase::Authenticated; ++count) {
            const auto sent = Clock::now();
            send_request("ping", {}, "pong", [this, sent](std::expected<json::object, SessionError> reply) {
                if (!reply && reply.error().code == "timeout") {
                    end_session({"timeout", "The host stopped responding. The session was closed."}, true);
                    return;
                }
                const auto host = reply ? integer_field(*reply, "host_time_us") : std::nullopt;
                if (!host || *host < 0) return;
                std::scoped_lock lock(mutex);
                clock_estimate.record(static_cast<std::uint64_t>(*host), sent, Clock::now());
            });
            clock_timer.expires_after(count < 5 ? Clock::duration(200ms) : Clock::duration(5s));
            beast::error_code error;
            co_await clock_timer.async_wait(asio::redirect_error(use_awaitable, error));
            if (error) co_return;
        }
    }

    void handle(json::object message) {
        const auto type = std::string(*string_field(message, "type"));
        if (const auto id = integer_field(message, "id")) {
            if (auto node = pending.extract(*id)) {
                auto& request = node.mapped();
                request.timer->cancel();
                if (type == "error") {
                    auto code = std::string(string_field(message, "code").value_or("error"));
                    if (request.expected == "clipboard_bulk_capabilities" && code == "unknown_type") {
                        const auto* fatal = message.if_contains("fatal");
                        if (!fatal || !fatal->is_bool() || fatal->as_bool()) code = "invalid_reply";
                    }
                    request.done(std::unexpected(SessionError{
                        std::move(code),
                        std::string(string_field(message, "message").value_or("The host rejected the request."))}));
                } else if (type != request.expected) {
                    request.done(std::unexpected(SessionError{"unexpected_reply", "The host sent an unexpected reply: " + type}));
                } else {
                    request.done(std::move(message));
                }
                return;
            }
        }
        const auto stream_id = string_field(message, "stream_id");
        if (type == "stream_config" && stream_id) {
            if (const auto* video = message.if_contains("video")) {
                if (auto parameters = parse_video(*video); parameters) configure_stream(std::string(*stream_id), *parameters);
            }
        } else if (type == "stream_stopped" && stream_id) {
            drop_stream(std::string(*stream_id));
        }
        if (events.message) {
            post_ui([this, shared = std::make_shared<json::object>(std::move(message))] { events.message(*shared); });
        }
    }

    void send_request(std::string type, json::object fields, std::string expected,
                      std::function<void(std::expected<json::object, SessionError>)> done) {
        if (phase != Phase::Authenticated) {
            done(std::unexpected(SessionError{"not_connected", "Not connected to a host."}));
            return;
        }
        const auto id = next_id++;
        fields["type"] = type;
        fields["id"] = id;
        auto text = json::serialize(fields);
        if (text.size() > net::protocol::max_authenticated_signaling_message) {
            done(std::unexpected(SessionError{"too_large", "The message exceeds the 16 MiB signaling limit. Nothing was sent."}));
            return;
        }
        Pending request{std::move(expected), std::move(done), std::make_unique<asio::steady_timer>(io, request_timeout), Clock::now()};
        request.timer->async_wait([this, id](beast::error_code error) {
            if (error) return;
            if (auto node = pending.extract(id)) {
                node.mapped().done(std::unexpected(SessionError{"timeout", "The host did not answer the request in time."}));
            }
        });
        pending.emplace(id, std::move(request));
        enqueue(std::move(text));
    }

    void enqueue(std::string text) {
        if (!control || phase != Phase::Authenticated) return;
        queued_bytes += text.size();
        if (queued_bytes > max_queued_bytes) {
            end_session({"overloaded", "The connection to the host is not draining. The session was closed."}, true);
            return;
        }
        writes.push_back(std::move(text));
        if (!writing) write_next();
    }

    void write_next() {
        if (writes.empty() || phase != Phase::Authenticated) {
            writing = false;
            return;
        }
        writing = true;
        control->text(true);
        control->async_write(asio::buffer(writes.front()), [this](beast::error_code error, std::size_t) {
            queued_bytes -= writes.front().size();
            writes.pop_front();
            if (error) {
                // The read loop reports the connection failure.
                writing = false;
                return;
            }
            write_next();
        });
    }

    void fail_pending(const SessionError& error) {
        auto requests = std::move(pending);
        pending.clear();
        for (auto& [id, request] : requests) {
            request.timer->cancel();
            request.done(std::unexpected(error));
        }
    }

    void end_session(SessionError error, bool notify) {
        if (phase == Phase::Closed) return;
        phase = Phase::Closed;
        clock_timer.cancel();
        connect_resolver.cancel();
        fail_pending(error);
        for (auto& [id, stream] : std::exchange(streams, {})) {
            stream->closing = true;
            stream->resolver.cancel();
            close_transport(stream->socket);
        }
        close_bulk_state();
        if (control) close_transport(*control);
        if (notify && events.disconnected) {
            post_ui([this, error = std::move(error)] { events.disconnected(error); });
        }
    }

    awaitable<void> run_bulk(std::shared_ptr<Bulk> current, Completion<void> opened) {
        Endpoint target_endpoint;
        PinPolicy pin;
        {
            std::scoped_lock lock(mutex);
            target_endpoint = *endpoint;
            pin = pin_policy;
        }
        try {
            {
                Deadline deadline(io, std::min<Clock::duration>(current->lifetime, 15s), [&] {
                    current->resolver.cancel();
                    close_transport(current->socket);
                });
                try {
                    (void)co_await establish(current->socket, current->resolver, target_endpoint, pin,
                                             current->target, bulk_receive_limit);
                } catch (...) {
                    if (deadline.expired()) throw Failure("timeout", "The bulk socket did not open in time.");
                    throw;
                }
            }
            complete<void>(std::exchange(opened, nullptr), {});
            beast::flat_buffer buffer;
            while (!current->closing) {
                buffer.clear();
                co_await current->socket.async_read(buffer, use_awaitable);
                if (current->closing) break;
                if (!current->socket.got_binary()) {
                    throw Failure("protocol_error", "The host sent a non-binary bulk message.");
                }
                if (current->callbacks.consume) {
                    std::vector<std::byte> bytes(buffer.size());
                    asio::buffer_copy(asio::buffer(bytes), buffer.data());
                    const auto consumed = co_await asio::async_initiate<decltype(use_awaitable),
                        void(std::expected<void, SessionError>)>(
                        [this, current, bytes = std::move(bytes)](auto handler) mutable {
                            auto completion = std::make_shared<decltype(handler)>(std::move(handler));
                            current->callbacks.consume(std::move(bytes),
                                [executor = io.get_executor(), completion](auto result) mutable {
                                    asio::post(executor, [completion, result = std::move(result)]() mutable {
                                        (*completion)(std::move(result));
                                    });
                                });
                        }, use_awaitable);
                    if (!consumed) throw Failure(consumed.error().code, consumed.error().message);
                } else if (current->callbacks.message) {
                    const auto bytes = std::span(
                        static_cast<const std::byte*>(buffer.data().data()), buffer.size());
                    current->callbacks.message(bytes);
                }
            }
        } catch (...) {
            auto error = describe(std::current_exception(), false);
            if (current->closing) {
                if (opened) complete<void>(std::move(opened), std::unexpected(SessionError{
                    "cancelled", "The bulk socket was closed before it opened."}));
                co_return;
            }
            current->closing = true;
            close_transport(current->socket);
            if (bulk == current) bulk.reset();
            if (opened) complete<void>(std::move(opened), std::unexpected(error));
            else if (current->callbacks.failed) current->callbacks.failed(error);
        }
    }

    void write_bulk_next(const std::shared_ptr<Bulk>& current) {
        if (current->closing || current->writes.empty()) {
            current->writing = false;
            return;
        }
        current->writing = true;
        current->socket.binary(true);
        current->socket.async_write(asio::buffer(current->writes.front().bytes),
            [this, current](beast::error_code error, std::size_t) {
                auto write = std::move(current->writes.front());
                current->writes.pop_front();
                current->queued_bytes -= write.bytes.size();
                if (error || current->closing) {
                    const bool cancelled = current->closing;
                    const SessionError failure{cancelled ? "cancelled" : "network_error",
                                               cancelled ? "The bulk transfer was cancelled."
                                                         : "The bulk transfer connection failed."};
                    complete<void>(std::move(write.done), std::unexpected(failure));
                    current->closing = true;
                    close_transport(current->socket);
                    if (bulk == current) bulk.reset();
                    for (auto& queued : current->writes) {
                        current->queued_bytes -= queued.bytes.size();
                        complete<void>(std::move(queued.done), std::unexpected(failure));
                    }
                    current->writes.clear();
                    current->queued_bytes = 0;
                    current->writing = false;
                    if (current->callbacks.failed) current->callbacks.failed(failure);
                    return;
                }
                complete<void>(std::move(write.done), {});
                write_bulk_next(current);
            });
    }

    void close_bulk_state() {
        if (!bulk) return;
        auto current = std::exchange(bulk, {});
        current->closing = true;
        current->resolver.cancel();
        close_transport(current->socket);
        const SessionError failure{"cancelled", "The bulk transfer was cancelled."};
        const auto first_queued = current->writing ? std::next(current->writes.begin()) : current->writes.begin();
        for (auto iterator = first_queued; iterator != current->writes.end(); ++iterator) {
            current->queued_bytes -= iterator->bytes.size();
            complete<void>(std::move(iterator->done), std::unexpected(failure));
        }
        current->writes.erase(first_queued, current->writes.end());
        if (!current->writing) current->queued_bytes = 0;
    }

    // Stream sockets (PROTOCOL.md §6-§7). All stream state is owned by the network thread.

    awaitable<void> run_stream(std::shared_ptr<Stream> stream, Completion<void> opened) {
        Endpoint target_endpoint;
        PinPolicy pin;
        {
            std::scoped_lock lock(mutex);
            target_endpoint = *endpoint;
            pin = pin_policy;
        }
        try {
            {
                Deadline deadline(io, std::min<Clock::duration>(stream->lifetime, 15s), [&] {
                    stream->resolver.cancel();
                    close_transport(stream->socket);
                });
                try {
                    (void)co_await establish(stream->socket, stream->resolver, target_endpoint, pin, stream->target,
                                             stream_receive_limit);
                } catch (...) {
                    if (deadline.expired()) throw Failure("timeout", "The stream socket did not open in time.");
                    throw;
                }
            }
            complete<void>(std::exchange(opened, nullptr), {});
            if (stream->callbacks.configured) stream->callbacks.configured(stream->video);
            beast::flat_buffer buffer;
            while (!stream->closing) {
                buffer.clear();
                co_await stream->socket.async_read(buffer, use_awaitable);
                if (stream->closing) break;
                if (!stream->socket.got_binary()) throw Failure("protocol_error", "The host sent a non-binary video message.");
                const std::span<const std::byte> bytes(static_cast<const std::byte*>(buffer.data().data()), buffer.size());
                accept_frame(*stream, bytes);
            }
        } catch (...) {
            if (stream->closing) co_return;
            auto error = describe(std::current_exception(), false);
            stream->closing = true;
            close_transport(stream->socket);
            if (const auto found = streams.find(stream->id); found != streams.end() && found->second == stream) {
                streams.erase(found);
            }
            if (opened) {
                complete<void>(std::move(opened), std::unexpected(error));
            } else if (stream->callbacks.failed) {
                stream->callbacks.failed(error);
            }
        }
    }

    void accept_frame(Stream& stream, std::span<const std::byte> bytes) {
        const auto parsed = parse_frame(bytes);
        if (parsed.kind != FrameKind::Video) return;
        switch (stream.gate.accept(parsed.video)) {
        case FrameAction::Decode:
            if (stream.callbacks.frame) stream.callbacks.frame(parsed.video);
            break;
        case FrameAction::RequestKeyFrame:
            request_key_frame(stream.id);
            break;
        case FrameAction::AwaitConfiguration:
            if (stream.held.size() < max_held_frames && stream.held_bytes + bytes.size() <= max_held_bytes) {
                stream.held.emplace_back(bytes.begin(), bytes.end());
                stream.held_bytes += bytes.size();
            } else {
                stream.held_overflow = true;
            }
            break;
        case FrameAction::Ignore:
            break;
        }
    }

    void configure_stream(const std::string& id, const VideoParameters& video) {
        const auto found = streams.find(id);
        if (found == streams.end()) return;
        auto& stream = *found->second;
        if (video.generation <= stream.video.generation || video.codec != stream.video.codec) return;
        stream.video = video;
        stream.gate.reconfigure(video.generation);
        if (stream.callbacks.configured) stream.callbacks.configured(stream.video);
        auto held = std::exchange(stream.held, {});
        stream.held_bytes = 0;
        for (const auto& bytes : held) {
            if (stream.closing) return;
            const auto parsed = parse_frame(bytes);
            if (parsed.video.generation == video.generation) accept_frame(stream, bytes);
            else if (parsed.video.generation > video.generation) {
                stream.held.push_back(bytes);
                stream.held_bytes += bytes.size();
            }
        }
        if (std::exchange(stream.held_overflow, false)) {
            stream.gate.reset();
            request_key_frame(id);
        }
    }

    void request_key_frame(const std::string& id) {
        enqueue(json::serialize(json::object{{"type", "request_key_frame"}, {"stream_id", id}}));
    }

    void drop_stream(const std::string& id) {
        const auto found = streams.find(id);
        if (found == streams.end()) return;
        found->second->closing = true;
        close_transport(found->second->socket);
        streams.erase(found);
    }

    // Declaration order matters: the io_context outlives every socket and timer that uses it.
    Dispatcher dispatcher;
    Events events;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
    wintls::context tls{wintls::method::system_default};
    asio::io_context io{1};
    asio::executor_work_guard<asio::io_context::executor_type> work{io.get_executor()};
    tcp::resolver connect_resolver{io};
    std::unique_ptr<WebSocket> control;
    asio::steady_timer clock_timer;
    std::unordered_map<std::int64_t, Pending> pending;
    std::map<std::string, std::shared_ptr<Stream>> streams;
    std::shared_ptr<Bulk> bulk;
    std::deque<std::string> writes;
    std::size_t queued_bytes{};
    bool writing{};
    std::int64_t next_id{1};
    Phase phase{Phase::Idle};

    mutable std::mutex mutex; // Guards the values below, which the UI thread may read.
    std::optional<Endpoint> endpoint;
    PinPolicy pin_policy;
    std::string fingerprint;
    ClockEstimate clock_estimate;

    std::thread thread;
};

Session::Session(Dispatcher dispatcher, Events events)
    : state_(std::make_shared<State>(std::move(dispatcher), std::move(events))) {}

Session::~Session() {
    auto* state = state_.get();
    state->alive->store(false);
    asio::post(state->io, [state] { state->end_session({"disconnected", "Disconnected."}, false); });
    state->work.reset();
    if (state->thread.joinable()) state->thread.join();
}

void Session::connect(ConnectOptions options, Completion<void> done) {
    auto* state = state_.get();
    asio::post(state->io, [state, options = std::move(options), done = std::move(done)]() mutable {
        if (state->phase != State::Phase::Idle) {
            SecureZeroMemory(options.password.data(), options.password.size());
            state->complete<void>(std::move(done), std::unexpected(SessionError{"busy", "This session was already used."}));
            return;
        }
        state->phase = State::Phase::Connecting;
        state->control = std::make_unique<WebSocket>(state->io, state->tls);
        asio::co_spawn(state->io, state->run_connect(std::move(options)),
            [state, done = std::move(done)](std::exception_ptr exception) mutable {
                if (!exception) {
                    state->complete<void>(std::move(done), {});
                    return;
                }
                auto error = describe(exception, false);
                state->end_session(error, false);
                state->complete<void>(std::move(done), std::unexpected(std::move(error)));
            });
    });
}

void Session::request(std::string type, json::object fields, std::string expected_reply, Completion<json::object> done) {
    auto* state = state_.get();
    asio::post(state->io, [state, type = std::move(type), fields = std::move(fields),
                           expected = std::move(expected_reply), done = std::move(done)]() mutable {
        state->send_request(std::move(type), std::move(fields), std::move(expected),
            [state, done = std::move(done)](std::expected<json::object, SessionError> result) mutable {
                state->complete<json::object>(std::move(done), std::move(result));
            });
    });
}

void Session::send(json::object message) {
    auto* state = state_.get();
    asio::post(state->io, [state, message = std::move(message)] {
        auto text = json::serialize(message);
        if (text.size() <= net::protocol::max_authenticated_signaling_message) state->enqueue(std::move(text));
    });
}

void Session::disconnect() {
    auto* state = state_.get();
    asio::post(state->io, [state] { state->end_session({"disconnected", "Disconnected."}, false); });
}

void Session::open_stream(const StreamOffer& offer, StreamCallbacks callbacks, Completion<void> opened) {
    auto* state = state_.get();
    asio::post(state->io, [state, offer, callbacks = std::move(callbacks), opened = std::move(opened)]() mutable {
        if (state->phase != State::Phase::Authenticated) {
            state->complete<void>(std::move(opened), std::unexpected(SessionError{"not_connected", "Not connected to a host."}));
            return;
        }
        if (state->streams.contains(offer.stream_id) ||
            state->streams.size() >= net::protocol::max_streams_per_session) {
            state->complete<void>(std::move(opened), std::unexpected(SessionError{"limit_reached", "Too many streams are open."}));
            return;
        }
        auto stream = std::make_shared<State::Stream>(state->io, state->tls, offer, std::move(callbacks));
        state->streams.emplace(offer.stream_id, stream);
        asio::co_spawn(state->io, state->run_stream(stream, std::move(opened)), asio::detached);
    });
}

void Session::close_stream(const std::string& stream_id) {
    auto* state = state_.get();
    asio::post(state->io, [state, stream_id] { state->drop_stream(stream_id); });
}

void Session::restart_stream_decoding(const std::string& stream_id) {
    auto* state = state_.get();
    asio::post(state->io, [state, stream_id] {
        const auto found = state->streams.find(stream_id);
        if (found == state->streams.end()) return;
        found->second->gate.reset();
        state->request_key_frame(stream_id);
    });
}

void Session::open_bulk(std::string target, std::chrono::milliseconds token_lifetime,
                        BulkCallbacks callbacks, Completion<void> opened) {
    auto* state = state_.get();
    asio::post(state->io, [state, target = std::move(target), token_lifetime,
                           callbacks = std::move(callbacks), opened = std::move(opened)]() mutable {
        if (state->phase != State::Phase::Authenticated) {
            state->complete<void>(std::move(opened),
                std::unexpected(SessionError{"not_connected", "Not connected to a host."}));
            return;
        }
        if (state->bulk) {
            state->complete<void>(std::move(opened),
                std::unexpected(SessionError{"limit_reached", "A bulk transfer is already active."}));
            return;
        }
        if (!valid_bulk_target(target) || token_lifetime <= 0ms || token_lifetime > 10s) {
            state->complete<void>(std::move(opened),
                std::unexpected(SessionError{"invalid_offer", "The host sent an invalid bulk socket offer."}));
            return;
        }
        auto bulk = std::make_shared<State::Bulk>(state->io, state->tls, std::move(target),
                                                   token_lifetime, std::move(callbacks));
        state->bulk = bulk;
        asio::co_spawn(state->io, state->run_bulk(bulk, std::move(opened)), asio::detached);
    });
}

void Session::write_bulk(std::vector<std::byte> message, Completion<void> written) {
    auto* state = state_.get();
    asio::post(state->io, [state, message = std::move(message), written = std::move(written)]() mutable {
        if (state->phase != State::Phase::Authenticated || !state->bulk || state->bulk->closing) {
            state->complete<void>(std::move(written),
                std::unexpected(SessionError{"not_connected", "The bulk transfer socket is not open."}));
            return;
        }
        auto& current = *state->bulk;
        if (message.size() < net::clipboard_bulk::frame_header_size ||
            message.size() > bulk_receive_limit ||
            current.queued_bytes > bulk_queued_limit - message.size()) {
            state->complete<void>(std::move(written),
                std::unexpected(SessionError{"overloaded", "The bulk transfer queue limit was reached."}));
            return;
        }
        current.queued_bytes += message.size();
        current.writes.push_back({std::move(message),
            [state, written = std::move(written)](std::expected<void, SessionError> result) mutable {
                state->complete<void>(std::move(written), std::move(result));
            }});
        if (!current.writing) state->write_bulk_next(state->bulk);
    });
}

void Session::close_bulk() {
    auto* state = state_.get();
    asio::post(state->io, [state] { state->close_bulk_state(); });
}

std::optional<ClockEstimate::Sample> Session::clock() const {
    std::scoped_lock lock(state_->mutex);
    return state_->clock_estimate.best(Clock::now());
}

std::optional<Endpoint> Session::endpoint() const {
    std::scoped_lock lock(state_->mutex);
    return state_->endpoint;
}

std::string Session::certificate_fingerprint() const {
    std::scoped_lock lock(state_->mutex);
    return state_->fingerprint;
}

} // namespace beamit::client
