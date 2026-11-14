#include "ClientSession.hpp"
#include "ClipboardManager.hpp"

// Boost.Asio/Beast must be included before boost-wintls.
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <wintls.hpp>
#include <wintls/beast.hpp>

#include "net/Protocol.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#include <fstream>
#include <winioctl.h>

using namespace beamit;
using namespace beamit::client;
using namespace std::chrono_literals;

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using WebSocket = websocket::stream<wintls::stream<tcp::socket>>;
using asio::awaitable;
using asio::use_awaitable;

constexpr char stream_id[] = "TestStream000001";
constexpr char stream_token[] = "single-use-token";
constexpr char bulk_target[] =
    "/bulk/AAAAAAAAAAAAAAAAAAAAAA?token=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

// Ephemeral self-signed loopback certificate. boost-wintls requires a CryptoAPI (not CNG) key container.
class TestCertificate {
public:
    TestCertificate() {
        const auto suffix = net::base64url_encode(net::random_bytes(9));
        name_ = L"BeamItClientTest-" + std::wstring(suffix.begin(), suffix.end());
        REQUIRE(CryptAcquireContextW(&provider_, name_.c_str(), MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES,
                                     CRYPT_NEWKEYSET | CRYPT_SILENT));
        HCRYPTKEY key{};
        REQUIRE(CryptGenKey(provider_, AT_KEYEXCHANGE, (2048U << 16), &key));
        CryptDestroyKey(key);
        std::vector<BYTE> encoded(256);
        DWORD size = static_cast<DWORD>(encoded.size());
        REQUIRE(CertStrToNameW(X509_ASN_ENCODING, L"CN=localhost", CERT_X500_NAME_STR, nullptr, encoded.data(), &size,
                               nullptr));
        CERT_NAME_BLOB subject{size, encoded.data()};
        CRYPT_KEY_PROV_INFO provider_info{};
        provider_info.pwszContainerName = name_.data();
        provider_info.pwszProvName = const_cast<wchar_t*>(MS_ENH_RSA_AES_PROV_W);
        provider_info.dwProvType = PROV_RSA_AES;
        provider_info.dwKeySpec = AT_KEYEXCHANGE;
        CRYPT_ALGORITHM_IDENTIFIER algorithm{const_cast<char*>(szOID_RSA_SHA256RSA), {}};
        certificate.reset(CertCreateSelfSignCertificate(provider_, &subject, 0, &provider_info, &algorithm, nullptr,
                                                        nullptr, nullptr));
        REQUIRE(certificate);
        fingerprint = format_fingerprint(net::sha256(
            {reinterpret_cast<const std::byte*>(certificate->pbCertEncoded), certificate->cbCertEncoded}));
    }
    ~TestCertificate() {
        certificate.reset();
        if (provider_) CryptReleaseContext(provider_, 0);
        HCRYPTPROV removed{};
        CryptAcquireContextW(&removed, name_.c_str(), MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, CRYPT_DELETEKEYSET);
    }
    TestCertificate(const TestCertificate&) = delete;
    TestCertificate& operator=(const TestCertificate&) = delete;

    wintls::cert_context_ptr certificate;
    std::string fingerprint;

private:
    std::wstring name_;
    HCRYPTPROV provider_{};
};

// Serializes writes on one WebSocket; Beast allows only one outstanding write.
class Writer : public std::enable_shared_from_this<Writer> {
public:
    explicit Writer(std::shared_ptr<WebSocket> socket) : socket_(std::move(socket)) {}
    void text(const json::object& message) { push({json::serialize(message), true}); }
    void binary(std::vector<std::byte> bytes) { push({std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()), false}); }
    std::shared_ptr<WebSocket> socket() const { return socket_; }

private:
    void push(std::pair<std::string, bool> message) {
        queue_.push_back(std::move(message));
        if (queue_.size() == 1) next();
    }
    void next() {
        socket_->text(queue_.front().second);
        socket_->async_write(asio::buffer(queue_.front().first), [self = shared_from_this()](beast::error_code error, std::size_t) {
            self->queue_.pop_front();
            if (!error && !self->queue_.empty()) self->next();
        });
    }
    std::shared_ptr<WebSocket> socket_;
    std::deque<std::pair<std::string, bool>> queue_;
};

std::vector<std::byte> frame(std::uint32_t generation, std::uint64_t sequence, bool key_frame) {
    const auto header = net::protocol::encode_frame_header(
        {key_frame ? net::protocol::flag_key_frame : 0U, generation, sequence, 1000 + sequence});
    std::vector<std::byte> bytes(header.begin(), header.end());
    for (unsigned value : {0U, 0U, 0U, 1U, key_frame ? 0x26U : 0x02U, 0x01U}) bytes.push_back(static_cast<std::byte>(value));
    return bytes;
}

class MockHost {
public:
    std::string password = "correct horse battery staple";
    bool forge_server_proof{};
    std::atomic<int> upgrades{};
    std::atomic<int> key_frame_requests{};
    std::atomic<bool> unexpected_headers{};
    std::function<std::vector<json::object>(const json::object&)> clipboard_handler;

    MockHost() : acceptor_(io_, {asio::ip::address_v4::loopback(), 0}) {
        tls_.use_certificate(certificate_.certificate.get());
        port = acceptor_.local_endpoint().port();
        asio::co_spawn(io_, accept_loop(), asio::detached);
        thread_ = std::thread([this] { io_.run(); });
    }
    ~MockHost() {
        run([this] {
            beast::error_code ignored;
            acceptor_.close(ignored);
            for (auto& socket : sockets_) {
                beast::get_lowest_layer(*socket).close(ignored);
            }
        });
        io_.stop();
        thread_.join();
    }

    std::string fingerprint() const { return certificate_.fingerprint; }
    std::string url() const { return "wss://127.0.0.1:" + std::to_string(port); }

    template<class Function>
    void run(Function function) {
        std::promise<void> done;
        asio::post(io_, [&] {
            function();
            done.set_value();
        });
        done.get_future().wait();
    }

    void push(json::object message) {
        run([&] { if (control_) control_->text(message); });
    }
    void send_frame(std::vector<std::byte> bytes) {
        run([&] { if (stream_) stream_->binary(std::move(bytes)); });
    }
    void send_bulk(std::vector<std::byte> bytes) {
        run([&] { if (bulk_) bulk_->binary(std::move(bytes)); });
    }
    void close_control(unsigned short code) {
        run([&] {
            if (!control_) return;
            control_->socket()->async_close(websocket::close_reason(code), [](beast::error_code) {});
        });
    }
    void close_bulk() {
        run([&] {
            if (bulk_) bulk_->socket()->async_close(websocket::close_reason(websocket::close_code::normal),
                [](beast::error_code) {});
        });
    }
    bool stream_connected() {
        bool result{};
        run([&] { result = stream_ != nullptr; });
        return result;
    }
    bool bulk_connected() {
        bool result{};
        run([&] { result = bulk_ != nullptr; });
        return result;
    }
    std::vector<std::vector<std::byte>> bulk_messages() {
        std::vector<std::vector<std::byte>> result;
        run([&] { result = bulk_messages_; });
        return result;
    }

    unsigned short port{};

private:
    awaitable<void> accept_loop() {
        for (;;) {
            beast::error_code error;
            auto socket = co_await acceptor_.async_accept(asio::redirect_error(use_awaitable, error));
            if (error) co_return;
            asio::co_spawn(io_, serve(std::move(socket)), asio::detached);
        }
    }

    awaitable<json::object> read(WebSocket& socket) {
        beast::flat_buffer buffer;
        co_await socket.async_read(buffer, use_awaitable);
        co_return json::parse(beast::buffers_to_string(buffer.data())).as_object();
    }

    awaitable<void> serve(tcp::socket raw) {
        auto socket = std::make_shared<WebSocket>(std::move(raw), tls_);
        sockets_.push_back(socket);
        try {
            co_await socket->next_layer().async_handshake(wintls::handshake_type::server, use_awaitable);
            beast::flat_buffer buffer;
            http::request<http::string_body> request;
            co_await http::async_read(socket->next_layer(), buffer, request, use_awaitable);
            ++upgrades;
            const std::string target(request.target());
            if (request[http::field::host] != "127.0.0.1:" + std::to_string(port) ||
                request.find(http::field::origin) != request.end()) {
                unexpected_headers = true;
            }
            co_await socket->async_accept(request, use_awaitable);
            if (target == "/control") {
                co_await control_session(socket);
            } else if (target == std::string("/streams/") + stream_id + "?token=" + stream_token) {
                stream_ = std::make_shared<Writer>(socket);
                beast::flat_buffer ignored;
                co_await socket->async_read(ignored, use_awaitable);
            } else if (target == bulk_target) {
                bulk_ = std::make_shared<Writer>(socket);
                co_await bulk_session(socket);
            }
        } catch (...) {
        }
    }

    awaitable<void> bulk_session(std::shared_ptr<WebSocket> socket) {
        for (;;) {
            beast::flat_buffer buffer;
            co_await socket->async_read(buffer, use_awaitable);
            if (!socket->got_binary()) co_return;
            const auto* begin = static_cast<const std::byte*>(buffer.data().data());
            bulk_messages_.emplace_back(begin, begin + buffer.size());
        }
    }

    awaitable<void> control_session(std::shared_ptr<WebSocket> socket) {
        const auto salt = net::random_bytes(16);
        const auto nonce = net::random_bytes(32);
        auto writer = std::make_shared<Writer>(socket);
        writer->text({{"type", "hello"}, {"protocol", 1}, {"server", "BeamIt"},
            {"auth", json::object{{"mechanism", "pbkdf2-sha256-hmac"}, {"salt", net::base64url_encode(salt)},
                                  {"iterations", net::protocol::pbkdf2_iterations},
                                  {"server_nonce", net::base64url_encode(nonce)}}}});
        const auto authenticate = co_await read(*socket);
        const auto key = net::pbkdf2_sha256(password, salt, net::protocol::pbkdf2_iterations);
        const auto client_nonce = *net::base64url_decode(std::string_view(authenticate.at("client_nonce").as_string()));
        const auto expected = net::base64url_encode(net::auth_proof(key, "client", nonce, client_nonce));
        if (authenticate.at("proof").as_string() != expected) {
            writer->text({{"type", "error"}, {"code", "auth_failed"}, {"message", "Authentication failed"}});
            co_return;
        }
        auto proof = net::auth_proof(key, "server", nonce, client_nonce);
        if (forge_server_proof) proof[0] ^= std::byte{1};
        writer->text({{"type", "authenticated"}, {"server_proof", net::base64url_encode(proof)}, {"host_time_us", 1}});
        control_ = writer;
        for (;;) {
            const auto message = co_await read(*socket);
            const auto type = message.at("type").as_string();
            if (clipboard_handler && (std::string_view(type).starts_with("clipboard_") || type == "get_clipboard" || type == "set_clipboard")) {
                for (auto& response : clipboard_handler(message)) writer->text(response);
                continue;
            }
            json::object reply{{"id", message.contains("id") ? message.at("id") : nullptr}};
            if (type == "ping") {
                reply["type"] = "pong";
                reply["host_time_us"] = 123456789;
            } else if (type == "list_windows") {
                reply["type"] = "windows";
                reply["windows"] = json::array{};
            } else if (type == "start_stream") {
                reply["type"] = "stream_offer";
                reply["stream_id"] = stream_id;
                reply["url"] = "wss://127.0.0.1:" + std::to_string(port) + "/streams/" + stream_id + "?token=" + stream_token;
                reply["token_expires_in_ms"] = 10000;
                reply["source"] = json::object{{"kind", "desktop"}, {"capture", "dxgi"}};
                reply["video"] = json::object{{"generation", 1}, {"codec", "h265"}, {"width", 64}, {"height", 64}};
            } else if (type == "request_key_frame") {
                ++key_frame_requests;
                continue;
            } else {
                reply["type"] = "error";
                reply["code"] = "unknown_type";
                reply["message"] = "Unknown type";
            }
            writer->text(reply);
        }
    }

    TestCertificate certificate_;
    wintls::context tls_{wintls::method::system_default};
    asio::io_context io_;
    tcp::acceptor acceptor_;
    std::vector<std::shared_ptr<WebSocket>> sockets_;
    std::shared_ptr<Writer> control_;
    std::shared_ptr<Writer> stream_;
    std::shared_ptr<Writer> bulk_;
    std::vector<std::vector<std::byte>> bulk_messages_;
    std::thread thread_;
};

// Runs session callbacks on the test thread, as the UI dispatcher does in the app.
class Inbox {
public:
    Session::Dispatcher dispatcher() {
        return [this](std::function<void()> function) {
            std::scoped_lock lock(mutex_);
            queue_.push_back(std::move(function));
            ready_.notify_one();
        };
    }

    bool wait_pending(std::chrono::milliseconds timeout = 5s) {
        std::unique_lock lock(mutex_);
        return ready_.wait_for(lock, timeout, [&] { return !queue_.empty(); });
    }

    template<class Predicate>
    bool pump_until(Predicate predicate, std::chrono::milliseconds timeout = 20s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!predicate()) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::unique_lock lock(mutex_);
            if (!ready_.wait_for(lock, 10ms, [&] { return !queue_.empty(); })) continue;
            auto function = std::move(queue_.front());
            queue_.pop_front();
            lock.unlock();
            function();
        }
        return true;
    }

private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> queue_;
};

struct Connected {
    std::optional<std::expected<void, SessionError>> result;
    std::vector<json::object> messages;
    std::optional<SessionError> disconnected;
    std::function<void(const json::object&)> message_hook;
    std::function<void()> disconnected_hook;
};

std::unique_ptr<Session> connect(Inbox& inbox, Connected& state, ConnectOptions options) {
    Session::Events events;
    events.message = [&state](const json::object& message) {
        state.messages.push_back(message);
        if (state.message_hook) state.message_hook(message);
    };
    events.disconnected = [&state](const SessionError& error) {
        state.disconnected = error;
        if (state.disconnected_hook) state.disconnected_hook();
    };
    auto session = std::make_unique<Session>(inbox.dispatcher(), std::move(events));
    session->connect(std::move(options), [&state](std::expected<void, SessionError> result) { state.result = result; });
    REQUIRE(inbox.pump_until([&] { return state.result.has_value(); }));
    return session;
}

} // namespace

TEST_CASE("Sessions pin the certificate, authenticate and route replies and events", "[client-session]") {
    MockHost host;
    Inbox inbox;
    Connected state;
    auto session = connect(inbox, state, {host.url(), host.password, host.fingerprint()});
    REQUIRE(*state.result);
    CHECK(session->certificate_fingerprint() == host.fingerprint());
    CHECK(session->endpoint()->authority == "127.0.0.1:" + std::to_string(host.port));

    std::optional<std::expected<json::object, SessionError>> reply;
    session->request("list_windows", {}, "windows", [&](auto result) { reply = std::move(result); });
    REQUIRE(inbox.pump_until([&] { return reply.has_value(); }));
    REQUIRE(*reply);
    CHECK((*reply)->at("windows").as_array().empty());

    reply.reset();
    session->request("bogus", {}, "never", [&](auto result) { reply = std::move(result); });
    REQUIRE(inbox.pump_until([&] { return reply.has_value(); }));
    REQUIRE_FALSE(*reply);
    CHECK(reply->error().code == "unknown_type");

    host.push({{"type", "window_opened"}, {"window", json::object{{"hwnd", "0x10"}}}});
    REQUIRE(inbox.pump_until([&] { return !state.messages.empty(); }));
    CHECK(state.messages.front().at("type") == "window_opened");
    REQUIRE(inbox.pump_until([&] { return session->clock().has_value(); }, 5s));

    host.close_control(net::protocol::close_superseded);
    REQUIRE(inbox.pump_until([&] { return state.disconnected.has_value(); }));
    CHECK(state.disconnected->code == "superseded");
}

TEST_CASE("A mismatched certificate fails before the WebSocket upgrade", "[client-session]") {
    MockHost host;
    Inbox inbox;
    Connected state;
    auto wrong = host.fingerprint();
    wrong[0] = wrong[0] == '0' ? '1' : '0';
    auto session = connect(inbox, state, {host.url(), host.password, wrong});
    REQUIRE_FALSE(*state.result);
    CHECK(state.result->error().code == "certificate_mismatch");
    CHECK(state.result->error().message.find(host.fingerprint()) != std::string::npos);
    CHECK(host.upgrades == 0);

    Connected missing;
    session = connect(inbox, missing, {host.url(), host.password, ""});
    CHECK(missing.result->error().code == "invalid_fingerprint");

    Connected insecure;
    ConnectOptions options{host.url(), host.password, ""};
    options.trust_any_certificate = true;
    session = connect(inbox, insecure, options);
    CHECK(*insecure.result);
}

TEST_CASE("Password and server proof failures are reported", "[client-session]") {
    MockHost host;
    Inbox inbox;
    Connected rejected;
    auto session = connect(inbox, rejected, {host.url(), "wrong password", host.fingerprint()});
    REQUIRE_FALSE(*rejected.result);
    CHECK(rejected.result->error().code == "auth_failed");
    CHECK(rejected.result->error().message.find("wrong password") == std::string::npos);

    host.forge_server_proof = true;
    Connected forged;
    session = connect(inbox, forged, {host.url(), host.password, host.fingerprint()});
    REQUIRE_FALSE(*forged.result);
    CHECK(forged.result->error().code == "server_proof_mismatch");
}

TEST_CASE("Stream sockets are pinned and gate frames by generation and key frame", "[client-session]") {
    MockHost host;
    Inbox inbox;
    Connected state;
    auto session = connect(inbox, state, {host.url(), host.password, host.fingerprint()});
    REQUIRE(*state.result);

    std::optional<std::expected<json::object, SessionError>> reply;
    session->request("start_stream", {{"source", json::object{{"kind", "desktop"}}}, {"video", json::object{{"codec", "h265"}}}},
                     "stream_offer", [&](auto result) { reply = std::move(result); });
    REQUIRE(inbox.pump_until([&] { return reply.has_value(); }));
    REQUIRE(*reply);
    const auto offer = parse_stream_offer(**reply, *session->endpoint());
    REQUIRE(offer);

    // Stream callbacks run on the network thread.
    std::mutex mutex;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> decoded;
    std::vector<std::size_t> payload_sizes;
    std::vector<std::uint32_t> generations;
    StreamCallbacks callbacks;
    callbacks.configured = [&](const VideoParameters& video) {
        std::scoped_lock lock(mutex);
        generations.push_back(video.generation);
    };
    callbacks.frame = [&](const VideoFrame& frame) {
        std::scoped_lock lock(mutex);
        payload_sizes.push_back(frame.payload.size());
        decoded.emplace_back(frame.generation, frame.sequence);
    };
    std::optional<std::expected<void, SessionError>> opened;
    session->open_stream(*offer, callbacks, [&](auto result) { opened = result; });
    REQUIRE(inbox.pump_until([&] { return opened.has_value(); }));
    REQUIRE(*opened);
    REQUIRE(inbox.pump_until([&] { return host.stream_connected(); }, 5s));

    const auto decoded_count = [&] {
        std::scoped_lock lock(mutex);
        return decoded.size();
    };
    host.send_frame(frame(1, 0, false));
    REQUIRE(inbox.pump_until([&] { return host.key_frame_requests == 1; }, 5s));
    host.send_frame(frame(1, 1, true));
    host.send_frame(frame(1, 2, false));
    REQUIRE(inbox.pump_until([&] { return decoded_count() == 2; }, 5s));

    // Generation 2 arrives before its stream_config and is held until configured.
    host.send_frame(frame(2, 0, true));
    host.send_frame(frame(2, 1, false));
    host.push({{"type", "stream_config"}, {"stream_id", stream_id},
               {"video", json::object{{"generation", 2}, {"codec", "h265"}, {"width", 32}, {"height", 32}}}});
    REQUIRE(inbox.pump_until([&] { return decoded_count() == 4; }, 5s));
    REQUIRE(inbox.pump_until([&] { return !state.messages.empty(); }, 5s));
    {
        std::scoped_lock lock(mutex);
        CHECK(generations == std::vector<std::uint32_t>{1, 2});
        CHECK(decoded == std::vector<std::pair<std::uint32_t, std::uint64_t>>{{1, 1}, {1, 2}, {2, 0}, {2, 1}});
        CHECK(payload_sizes == std::vector<std::size_t>(4, 6));
    }
    CHECK(host.key_frame_requests == 1);
    CHECK(host.upgrades == 2);
    CHECK_FALSE(host.unexpected_headers);

    session->close_stream(stream_id);
    session.reset();
}

TEST_CASE("Bulk sockets reuse the pinned authority and bound binary writes", "[client-session][clipboard-bulk]") {
    MockHost host;
    Inbox inbox;
    Connected state;
    auto session = connect(inbox, state, {host.url(), host.password, host.fingerprint()});
    REQUIRE(*state.result);

    std::mutex mutex;
    std::vector<std::vector<std::byte>> received;
    BulkCallbacks callbacks;
    callbacks.message = [&](std::span<const std::byte> bytes) {
        std::scoped_lock lock(mutex);
        received.emplace_back(bytes.begin(), bytes.end());
    };
    std::optional<std::expected<void, SessionError>> opened;
    session->open_bulk(bulk_target, 10s, callbacks, [&](auto result) { opened = std::move(result); });
    REQUIRE(inbox.pump_until([&] { return opened.has_value(); }));
    REQUIRE(*opened);
    REQUIRE(inbox.pump_until([&] { return host.bulk_connected(); }, 5s));

    const std::vector<std::byte> incoming(40, std::byte{0x41});
    host.send_bulk(incoming);
    REQUIRE(inbox.pump_until([&] {
        std::scoped_lock lock(mutex);
        return received.size() == 1;
    }, 5s));
    {
        std::scoped_lock lock(mutex);
        CHECK(received.front() == incoming);
    }

    const std::vector<std::byte> outgoing(40, std::byte{0x42});
    std::optional<std::expected<void, SessionError>> written;
    session->write_bulk(outgoing, [&](auto result) { written = std::move(result); });
    REQUIRE(inbox.pump_until([&] { return written.has_value(); }));
    REQUIRE(*written);
    REQUIRE(inbox.pump_until([&] { return host.bulk_messages().size() == 1; }, 5s));
    CHECK(host.bulk_messages().front() == outgoing);
    CHECK_FALSE(host.unexpected_headers);

    session->close_bulk();
    session.reset();
}

TEST_CASE("Bulk socket rejects malformed transfer IDs and tokens before opening WSS",
          "[client-session][clipboard-bulk]") {
    MockHost host;
    Inbox inbox;
    Connected state;
    auto session = connect(inbox, state, {host.url(), host.password, host.fingerprint()});
    REQUIRE(*state.result);
    std::optional<std::expected<void, SessionError>> opened;
    session->open_bulk("/bulk/short?token=invalid", 10s, {},
                       [&](auto result) { opened = std::move(result); });
    REQUIRE(inbox.pump_until([&] { return opened.has_value(); }));
    REQUIRE_FALSE(*opened);
    CHECK(opened->error().code == "invalid_offer");
    CHECK(host.upgrades == 1); // only the authenticated control socket
}

TEST_CASE("Bulk receive awaits storage drain while control requests remain responsive",
          "[client-session][clipboard-bulk]") {
    MockHost host;
    Inbox inbox;
    Connected state;
    auto session = connect(inbox, state, {host.url(), host.password, host.fingerprint()});
    REQUIRE(*state.result);
    std::mutex mutex;
    unsigned consumed{};
    std::function<void(std::expected<void, SessionError>)> pending;
    BulkCallbacks callbacks;
    callbacks.consume = [&](std::vector<std::byte>, auto done) {
        std::scoped_lock lock(mutex);
        ++consumed;
        pending = std::move(done);
    };
    std::optional<std::expected<void, SessionError>> opened;
    session->open_bulk(bulk_target, 10s, std::move(callbacks), [&](auto result) { opened = std::move(result); });
    REQUIRE(inbox.pump_until([&] { return opened.has_value(); }));
    REQUIRE(*opened);
    REQUIRE(inbox.pump_until([&] { return host.bulk_connected(); }));
    host.send_bulk(std::vector<std::byte>(40));
    host.send_bulk(std::vector<std::byte>(40));
    REQUIRE(inbox.pump_until([&] { std::scoped_lock lock(mutex); return consumed == 1; }));
    bool replied{};
    session->request("list_windows", {}, "windows", [&](auto result) { replied = bool(result); });
    REQUIRE(inbox.pump_until([&] { return replied; }));
    // Keep draining UI/control callbacks with the first disk completion withheld.
    inbox.pump_until([] { return false; }, 50ms);
    std::function<void(std::expected<void, SessionError>)> drain;
    {
        std::scoped_lock lock(mutex);
        CHECK(consumed == 1);
        drain = std::move(pending);
    }
    drain({});
    REQUIRE(inbox.pump_until([&] { std::scoped_lock lock(mutex); return consumed == 2; }));
    {
        std::scoped_lock lock(mutex);
        drain = std::move(pending);
    }
    drain({});
    session->close_bulk();
}

namespace {
namespace bulk = net::clipboard_bulk;
struct ClipboardTemp {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("BeamItManager-" + net::base64url_encode(net::random_bytes(12)));
    ClipboardTemp() { std::filesystem::create_directories(path); }
    ~ClipboardTemp() { std::error_code error; std::filesystem::remove_all(path, error); }
};
const std::string manager_png =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9Wl6VwsAAAAASUVORK5CYII=";
enum class ManagerCase { Upload, Download, DeclineUpload, DeclineDownload, Five, Six, ExactBytes,
    Cancel, Focus, Source, Destination, Reconfirm, LegacyUpload, LegacyDownload, InvalidCaps, NativeFailure,
    Disconnect, Corrupt, EarlyComplete, SourceReconfirm, UploadQuota, DownloadQuota, Excluded,
    WrongAuthority, Manual, Disabled, PngUpload, PngDownload, NoProgress, AbsoluteTimeout, TextUpload, TextDownload,
    StaleFastPaste, HostExcluded, SourceBusy, UploadRate, DownloadRate, ClosedAfterEnd, SourceAfterEnd, CacheInvalidated,
    FatalFallback, MalformedFallback, StaleHostPaste };

void manager_sparse_file(const std::filesystem::path& path, std::uint64_t size) {
    std::filesystem::create_directories(path.parent_path());
    const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    REQUIRE(file != INVALID_HANDLE_VALUE);
    DWORD returned{};
    const bool sparse = DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr);
    CloseHandle(file);
    REQUIRE(sparse);
    std::filesystem::resize_file(path, size);
}

json::object manager_capabilities() {
    return {{"type", "clipboard_bulk_capabilities"}, {"version", 1},
        {"max_file_bytes", bulk::max_file_bytes}, {"max_roots", bulk::max_roots}, {"max_nodes", bulk::max_nodes},
        {"max_chunk_bytes", bulk::max_chunk_size}, {"max_manifest_bytes", bulk::max_manifest_bytes},
        {"max_png_bytes", bulk::max_png_bytes}, {"max_png_dimension", bulk::max_png_dimension},
        {"max_png_pixels", bulk::max_png_pixels}, {"max_component_bytes", 255}, {"max_path_bytes", 4096},
        {"staging_quota_bytes", 8'000'000'000ULL}, {"scratch_bytes", 128 * 1024 * 1024}};
}
} // namespace

TEST_CASE("Windows clipboard manager uses native facades and authenticated pinned WSS in both directions",
          "[clipboard-manager]") {
    auto scenario = ManagerCase::Upload;
    SECTION("upload sends immutable folder PNG text HTML and waits for publication before paste") {}
    SECTION("download publishes all verified formats before receiver acknowledgement") { scenario = ManagerCase::Download; }
    SECTION("declined upload sends no payload or begin") { scenario = ManagerCase::DeclineUpload; }
    SECTION("declined download sends no bulk request") { scenario = ManagerCase::DeclineDownload; }
    SECTION("five selected roots are automatic") { scenario = ManagerCase::Five; }
    SECTION("six selected roots require confirmation") { scenario = ManagerCase::Six; }
    SECTION("exactly five hundred million bytes requires confirmation before spooling") { scenario = ManagerCase::ExactBytes; }
    SECTION("cancel suppresses pending paste") { scenario = ManagerCase::Cancel; }
    SECTION("focus changes invalidate pending paste") { scenario = ManagerCase::Focus; }
    SECTION("source clipboard changes suppress pending paste") { scenario = ManagerCase::Source; }
    SECTION("destination changes before native commit prevent success") { scenario = ManagerCase::Destination; }
    SECTION("host reconfirmation obtains a new confirmation decision") { scenario = ManagerCase::Reconfirm; }
    SECTION("only unknown-type negotiates legacy upload") { scenario = ManagerCase::LegacyUpload; }
    SECTION("legacy text HTML PNG downloads remain supported") { scenario = ManagerCase::LegacyDownload; }
    SECTION("invalid capabilities fail rather than falling back") { scenario = ManagerCase::InvalidCaps; }
    SECTION("native publication failure never acknowledges receipt") { scenario = ManagerCase::NativeFailure; }
    SECTION("disconnect cancels outgoing work and suppresses paste") { scenario = ManagerCase::Disconnect; }
    SECTION("corrupt binary data never reaches native publication") { scenario = ManagerCase::Corrupt; }
    SECTION("early receiver completion cannot release paste") { scenario = ManagerCase::EarlyComplete; }
    SECTION("changed source selection requires a fresh confirmation") { scenario = ManagerCase::SourceReconfirm; }
    SECTION("upload quota fails before offering or sending payload") { scenario = ManagerCase::UploadQuota; }
    SECTION("download quota fails before opening bulk") { scenario = ManagerCase::DownloadQuota; }
    SECTION("excluded local content is never offered") { scenario = ManagerCase::Excluded; }
    SECTION("a cross-authority offer cannot open a bulk socket") { scenario = ManagerCase::WrongAuthority; }
    SECTION("manual upload works while automatic synchronization is disabled") { scenario = ManagerCase::Manual; }
    SECTION("disabled synchronization preserves ordinary paste without a transfer") { scenario = ManagerCase::Disabled; }
    SECTION("PNG-only upload uses binary frames") { scenario = ManagerCase::PngUpload; }
    SECTION("PNG-only download publishes no file references") { scenario = ManagerCase::PngDownload; }
    SECTION("no-progress timeout cancels without forwarding paste") { scenario = ManagerCase::NoProgress; }
    SECTION("absolute timeout cancels despite recent sender progress") { scenario = ManagerCase::AbsoluteTimeout; }
    SECTION("negotiated text HTML upload uses the existing short control request") { scenario = ManagerCase::TextUpload; }
    SECTION("negotiated text HTML download does not open a bulk socket") { scenario = ManagerCase::TextDownload; }
    SECTION("already uploaded paste rechecks clipboard revision at UI delivery") { scenario = ManagerCase::StaleFastPaste; }
    SECTION("host exclusion cancels pending paste without fetching content") { scenario = ManagerCase::HostExcluded; }
    SECTION("unreadable source is an explicit failure before payload") { scenario = ManagerCase::SourceBusy; }
    SECTION("outgoing payload is paced beyond the initial burst") { scenario = ManagerCase::UploadRate; }
    SECTION("incoming payload is paced beyond the initial burst") { scenario = ManagerCase::DownloadRate; }
    SECTION("bulk close after END still waits for the control publication acknowledgement") { scenario = ManagerCase::ClosedAfterEnd; }
    SECTION("source file mutation after END cannot release paste") { scenario = ManagerCase::SourceAfterEnd; }
    SECTION("host clipboard change invalidates a previously acknowledged upload") { scenario = ManagerCase::CacheInvalidated; }
    SECTION("fatal unknown-type cannot enable legacy fallback") { scenario = ManagerCase::FatalFallback; }
    SECTION("malformed unknown-type cannot enable legacy fallback") { scenario = ManagerCase::MalformedFallback; }
    SECTION("a known host clipboard change invalidates a queued fast-path paste") { scenario = ManagerCase::StaleHostPaste; }
    const bool incoming = scenario == ManagerCase::Download || scenario == ManagerCase::DeclineDownload ||
        scenario == ManagerCase::Destination || scenario == ManagerCase::Reconfirm ||
        scenario == ManagerCase::LegacyDownload || scenario == ManagerCase::NativeFailure || scenario == ManagerCase::Corrupt ||
        scenario == ManagerCase::DownloadQuota || scenario == ManagerCase::PngDownload || scenario == ManagerCase::TextDownload ||
        scenario == ManagerCase::DownloadRate;
    const bool legacy = scenario == ManagerCase::LegacyUpload || scenario == ManagerCase::LegacyDownload;
    const bool png_only = scenario == ManagerCase::PngUpload || scenario == ManagerCase::PngDownload;
    const bool text_only = scenario == ManagerCase::TextUpload || scenario == ManagerCase::TextDownload;
    const bool paced = scenario == ManagerCase::UploadRate || scenario == ManagerCase::DownloadRate;
    ClipboardTemp staging, source;
    const auto selected = source.path / L"selection";
    std::filesystem::create_directories(selected / L"empty");
    std::ofstream(selected / L"item.txt") << "abc";
    std::vector<std::byte> file_data{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
    if (paced) {
        file_data.resize(3 * 1024 * 1024);
        std::filesystem::resize_file(selected / L"item.txt", file_data.size());
    }
    struct SourceLock {
        HANDLE file{INVALID_HANDLE_VALUE};
        ~SourceLock() { if (file != INVALID_HANDLE_VALUE) CloseHandle(file); }
    } source_lock;
    if (scenario == ManagerCase::SourceBusy) {
        source_lock.file = CreateFileW((selected / L"item.txt").c_str(), GENERIC_READ, 0,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        REQUIRE(source_lock.file != INVALID_HANDLE_VALUE);
    }
    if (scenario == ManagerCase::ExactBytes) manager_sparse_file(selected / L"item.txt", 500'000'000);
    auto png = *net::base64_decode(manager_png);
    bulk::Manifest remote{.selected_roots = 1, .nodes = {
        {.kind = bulk::Node::Kind::Directory, .path = "selection"},
        {.kind = bulk::Node::Kind::File, .path = "selection/item.txt", .size = file_data.size(),
         .sha256 = net::sha256(file_data)},
        {.kind = bulk::Node::Kind::Directory, .path = "selection/empty"},
        {.kind = bulk::Node::Kind::Png, .path = "clipboard.png", .size = png.size(),
         .sha256 = net::sha256(png), .width = 1, .height = 1}}};
    if (png_only) {
        remote.nodes.erase(remote.nodes.begin(), remote.nodes.end() - 1);
        remote.selected_roots = 0;
    }
    if (text_only) { remote.nodes.clear(); remote.selected_roots = 0; }
    const auto snapshot = bulk::issue_socket_credentials().transfer_id_text;
    const auto next_snapshot = bulk::issue_socket_credentials().transfer_id_text;
    const std::string transfer = "AAAAAAAAAAAAAAAAAAAAAA";
    const json::array roots = png_only || text_only ? json::array{} :
        json::array{json::object{{"path", "selection"}, {"identity", net::base64url_encode(net::Sha256{})}}};
    auto preliminary_manifest = bulk::manifest_json(remote);
    for (auto& node : preliminary_manifest.at("nodes").as_array()) node.as_object().erase("sha256");
    preliminary_manifest["roots"] = roots;
    MockHost host;
    Inbox inbox;
    Connected connected;
    auto session = connect(inbox, connected, {host.url(), host.password, host.fingerprint()});
    REQUIRE(*connected.result);
    std::atomic<int> begins{}, requests{}, commits{}, acknowledgements{}, legacy_writes{}, cancellations{}, failures{}, snapshots{};
    json::object begin_message;
    host.run([&] {
        host.clipboard_handler = [&](const json::object& message) -> std::vector<json::object> {
            const auto type = std::string(message.at("type").as_string());
            const auto id = message.if_contains("id") ? message.at("id") : json::value{};
            if (type == "clipboard_bulk_negotiate") {
                if (scenario == ManagerCase::FatalFallback)
                    return {{{"id", id}, {"type", "error"}, {"code", "unknown_type"}, {"fatal", true}}};
                if (scenario == ManagerCase::MalformedFallback)
                    return {{{"id", id}, {"type", "error"}, {"code", "unknown_type"}}};
                if (legacy) return {{{"id", id}, {"type", "error"}, {"code", "unknown_type"}, {"fatal", false}, {"message", "old host"}}};
                auto result = manager_capabilities(); result["id"] = id;
                if (scenario == ManagerCase::InvalidCaps) result["version"] = 9;
                return {result};
            }
            if (type == "get_clipboard") {
                ++snapshots;
                json::object result{{"id", id}, {"type", "clipboard"}, {"sequence", 99}, {"text", "remote text"},
                    {"html", "<b>remote</b>"}, {"omitted", json::array{}}};
                if (png_only) { result.erase("text"); result.erase("html"); }
                if (legacy) result["image"] = {{"mime_type", "image/png"}, {"data", manager_png}};
                else {
                    result["snapshot_id"] = snapshot;
                    result["manifest"] = preliminary_manifest;
                }
                return {result};
            }
            if (type == "set_clipboard") {
                ++legacy_writes;
                begin_message = message;
                return {{{"type", "clipboard_set"}, {"id", id}, {"sequence", 100}}};
            }
            if (type == "clipboard_bulk_begin") {
                ++begins; begin_message = message;
                json::object offer{{"id", id}, {"type", "clipboard_bulk_offer"}, {"direction", "client_to_host"},
                    {"transfer_id", transfer}, {"source_snapshot_id", message.at("source_snapshot_id")},
                    {"destination_sequence", 99}, {"manifest", message.at("manifest")},
                    {"url", (scenario == ManagerCase::WrongAuthority ? "wss://other.invalid:443" : host.url()) + bulk_target},
                    {"expires_in_ms", 10000}};
                if (scenario == ManagerCase::EarlyComplete) return {offer,
                    {{"type", "clipboard_bulk_complete"}, {"transfer_id", transfer}, {"sequence", 100}}};
                return {offer};
            }
            if (type == "clipboard_bulk_request") {
                const auto count = ++requests;
                json::object reply{{"id", id}, {"type", "clipboard_bulk_progress"}, {"transfer_id", transfer},
                    {"phase", "preparing"}, {"source_snapshot_id", message.at("snapshot_id")},
                    {"completed_bytes", 0}, {"total_bytes", bulk::payload_bytes(remote)},
                    {"completed_nodes", 0}, {"total_nodes", remote.nodes.size()}};
                if (scenario == ManagerCase::Reconfirm && count == 1) {
                    return {reply, {{"type", "clipboard_bulk_reconfirm"}, {"transfer_id", transfer}, {"sequence", 100},
                        {"snapshot_id", next_snapshot}, {"manifest", preliminary_manifest},
                        {"text", "remote text"}, {"html", "<b>remote</b>"}}};
                }
                return {reply, {{"type", "clipboard_bulk_offer"}, {"direction", "host_to_client"},
                    {"transfer_id", transfer}, {"source_snapshot_id", message.at("snapshot_id")}, {"source_sequence", 99},
                    {"roots", roots}, {"manifest", bulk::manifest_json(remote)},
                    {"url", host.url() + bulk_target}, {"expires_in_ms", 10000}}};
            }
            if (type == "clipboard_bulk_commit") {
                ++commits;
                return {{{"id", id}, {"type", "clipboard_bulk_committed"}, {"transfer_id", transfer}}};
            }
            if (type == "clipboard_bulk_complete") ++acknowledgements;
            if (type == "clipboard_bulk_cancel") ++cancellations;
            if (type == "clipboard_bulk_failed") ++failures;
            return {};
        };
    });
    std::atomic<DWORD> sequence{42};
    std::atomic<unsigned> clock_calls{}, clock_seconds{};
    unsigned confirmations{}, publications{};
    bool ready{}, completed{}, paste_called{}, paste_result{};
    std::string error;
    std::function<void()> pending_publication;
    std::filesystem::path published_path;
    std::atomic_bool requested_formats_valid{true};
    ClipboardManager::Native native;
    native.staging_root = staging.path;
    native.sequence = [&] { return sequence.load(); };
    native.clock = [&] {
        const auto call = clock_calls.fetch_add(1);
        const auto seconds = clock_seconds.load();
        return std::chrono::steady_clock::time_point(std::chrono::seconds(
            scenario == ManagerCase::AbsoluteTimeout && call && !seconds ? 1799u : seconds));
    };
    native.read = [&](unsigned formats, bool binary, auto done) {
        host::ClipboardContent content{.sequence = sequence.load(), .text = "local text", .html = "<b>local</b>",
            .image = host::ClipboardImage{.png = png, .width = 1, .height = 1}};
        if (binary) content.files.push_back(selected);
        if (png_only) { content.files.clear(); content.text.reset(); content.html.reset(); }
        if (text_only) { content.files.clear(); content.image.reset(); }
        if (scenario == ManagerCase::Excluded) content.excluded = true;
        if (scenario == ManagerCase::Five || scenario == ManagerCase::Six) {
            content.files.clear();
            for (int index = 0; index < (scenario == ManagerCase::Five ? 5 : 6); ++index) {
                const auto path = source.path / ("root" + std::to_string(index));
                std::filesystem::create_directories(path);
                content.files.push_back(path);
            }
        }
        requested_formats_valid.store(bool(formats & host::clipboard_files) == binary);
        done({.content = std::move(content)});
    };
    native.write = [&](host::ClipboardWrite write, auto done) {
        // Marshal the injected native operation onto the fake UI dispatcher so
        // assertions and deferred publication are observed on the test thread.
        inbox.dispatcher()([&, write = std::move(write), done = std::move(done)]() mutable {
            if (png_only) { CHECK_FALSE(write.text); CHECK_FALSE(write.html); }
            else { CHECK(write.text == "remote text"); CHECK(write.html == "<b>remote</b>"); }
            if (!legacy && !text_only) {
                REQUIRE(write.files.size() == (png_only ? 0u : 1u));
                if (!png_only) {
                    published_path = write.files[0];
                    CHECK(std::filesystem::is_directory(write.files[0] / L"empty"));
                    std::ifstream input(write.files[0] / L"item.txt");
                    CHECK(std::string(std::istreambuf_iterator<char>(input), {}) ==
                        std::string(reinterpret_cast<const char*>(file_data.data()), file_data.size()));
                }
                CHECK(write.png == png);
            } else if (legacy) CHECK(write.image == manager_png);
            else { CHECK_FALSE(write.image); CHECK_FALSE(write.png); CHECK(write.files.empty()); }
            pending_publication = [&, write = std::move(write), done = std::move(done)]() mutable {
                if (sequence.load() != *write.expected_sequence || !write.publication_allowed->load()) {
                    done({.error_code = "stale_clipboard", .error = "injected stale destination"});
                } else if (scenario == ManagerCase::NativeFailure) {
                    done({.error_code = "clipboard_unavailable", .error = "injected native failure"});
                } else { ++publications; sequence.store(43); done({.sequence = 43}); }
            };
        });
    };
    ClipboardManager::Callbacks callbacks;
    callbacks.confirm = [&](auto count, auto bytes, auto decide) {
        ++confirmations;
        if (scenario == ManagerCase::SourceReconfirm && confirmations == 2) completed = false;
        if (scenario == ManagerCase::Six) { CHECK(count == 6); CHECK(bytes == 0); }
        if (scenario == ManagerCase::ExactBytes) { CHECK(count == 1); CHECK(bytes == 500'000'000); }
        if (scenario == ManagerCase::SourceReconfirm) {
            CHECK(count == 1);
            CHECK(bytes == (confirmations == 1 ? 3u : 4u));
            if (confirmations == 1) std::ofstream(selected / L"item.txt", std::ios::app).put('d');
            decide(confirmations == 1);
        } else if (scenario == ManagerCase::Reconfirm) decide(confirmations == 1);
        else decide(false);
    };
    callbacks.status = [&](auto status) {
        if (status.phase == "ready") ready = true;
        if (!status.busy && (status.phase == "complete" || status.phase == "cancelled" || status.phase == "failed")) {
            completed = true; error = status.error;
        }
    };
    auto manager = std::make_unique<ClipboardManager>(*session, inbox.dispatcher(), std::move(callbacks), std::move(native));
    connected.message_hook = [&](const auto& event) { if (manager) manager->event(event); };
    connected.disconnected_hook = [&] { if (manager) manager->cancel(); };
    manager->context({.enabled = scenario != ManagerCase::Disabled && scenario != ManagerCase::Manual, .active = true});
    manager->start();
    REQUIRE(inbox.pump_until([&] { return ready || error == "invalid_capabilities" || error == "invalid_reply"; }));
    completed = false;
    if (scenario == ManagerCase::DeclineUpload || scenario == ManagerCase::DeclineDownload ||
        scenario == ManagerCase::Reconfirm || scenario == ManagerCase::SourceReconfirm) manager->policy({0, 0});
    if (scenario == ManagerCase::UploadQuota || scenario == ManagerCase::DownloadQuota)
        manager_sparse_file(staging.path / L"host" / L"committed" / L"retained", 8'000'000'000 - 128 * 1024 * 1024);
    if (scenario == ManagerCase::InvalidCaps || scenario == ManagerCase::FatalFallback || scenario == ManagerCase::MalformedFallback) {
        CHECK_FALSE(ready);
        CHECK(error == (scenario == ManagerCase::InvalidCaps ? "invalid_capabilities" : "invalid_reply"));
        CHECK(begins == 0);
        CHECK(legacy_writes == 0);
    } else {
        const auto started = std::chrono::steady_clock::now();
        if (scenario == ManagerCase::Manual) manager->upload();
        else if (incoming) manager->download();
        else manager->paste([&](bool result) { paste_called = true; paste_result = result; });
        if (scenario == ManagerCase::Disabled) {
            REQUIRE(inbox.pump_until([&] { return paste_called; }));
            CHECK(paste_result);
            CHECK(begins == 0);
            CHECK(host.bulk_messages().empty());
        } else if (scenario == ManagerCase::UploadQuota || scenario == ManagerCase::DownloadQuota ||
            scenario == ManagerCase::Excluded || scenario == ManagerCase::WrongAuthority || scenario == ManagerCase::SourceBusy) {
            REQUIRE(inbox.pump_until([&] { return completed; }));
            CHECK_FALSE(paste_result);
            CHECK(publications == 0);
            CHECK(commits == 0);
            CHECK(host.upgrades == 1); // control only; no payload connection
            if (scenario == ManagerCase::UploadQuota || scenario == ManagerCase::DownloadQuota) CHECK(error == "quota_exceeded");
            if (scenario == ManagerCase::SourceBusy) CHECK(error == "source_unavailable");
        } else if (scenario == ManagerCase::DeclineUpload || scenario == ManagerCase::DeclineDownload ||
            scenario == ManagerCase::Six || scenario == ManagerCase::ExactBytes || scenario == ManagerCase::Reconfirm ||
            scenario == ManagerCase::SourceReconfirm) {
            REQUIRE(inbox.pump_until([&] { return completed &&
                (scenario != ManagerCase::SourceReconfirm || confirmations == 2); }));
            CHECK(confirmations == (scenario == ManagerCase::Reconfirm || scenario == ManagerCase::SourceReconfirm ? 2u : 1u));
            CHECK(begins == 0);
            CHECK(requests == (scenario == ManagerCase::Reconfirm ? 1 : 0));
            CHECK(publications == 0);
        } else if (legacy || text_only) {
            if (incoming) {
                REQUIRE(inbox.pump_until([&] { return bool(pending_publication); }));
                auto publish = std::move(pending_publication); publish();
            }
            REQUIRE(inbox.pump_until([&] { return completed && (incoming || paste_called); }));
            CHECK((incoming ? publications == 1 : paste_result));
            CHECK(begins == 0);
            if (!incoming) {
                host.run([&] {
                    CHECK(begin_message.at("text") == "local text");
                    CHECK(begin_message.at("html") == "<b>local</b>");
                    if (legacy) CHECK(std::string(begin_message.at("image").as_object().at("data").as_string()) == manager_png);
                    else CHECK_FALSE(begin_message.contains("image"));
                });
            }
        } else if (incoming) {
            REQUIRE(inbox.pump_until([&] { return host.bulk_connected(); }));
            const std::array<std::byte, 16> raw{};
            for (std::uint32_t node = 0; node < remote.nodes.size(); ++node) {
                if (remote.nodes[node].kind == bulk::Node::Kind::Directory) continue;
                auto bytes = remote.nodes[node].kind == bulk::Node::Kind::Png ? png :
                    file_data;
                if (scenario == ManagerCase::Corrupt && node == 1) bytes[0] = std::byte{'x'};
                for (std::size_t offset = 0; offset < bytes.size(); offset += bulk::max_chunk_size) {
                    const auto length = std::min(bulk::max_chunk_size, bytes.size() - offset);
                    const auto header = bulk::encode_header({
                        .kind = remote.nodes[node].kind == bulk::Node::Kind::Png ? bulk::FrameKind::Png : bulk::FrameKind::File,
                        .transfer_id = raw, .node_index = node, .offset = offset,
                        .payload_size = static_cast<std::uint32_t>(length)});
                    std::vector<std::byte> frame(header.begin(), header.end());
                    frame.insert(frame.end(), bytes.begin() + offset, bytes.begin() + offset + length);
                    host.send_bulk(std::move(frame));
                }
                if (scenario == ManagerCase::Corrupt) break;
            }
            if (scenario != ManagerCase::Corrupt) {
                const auto end = bulk::encode_header({.kind = bulk::FrameKind::End, .transfer_id = raw,
                    .node_index = UINT32_MAX, .offset = bulk::payload_bytes(remote)});
                host.send_bulk({end.begin(), end.end()});
                REQUIRE(inbox.pump_until([&] { return bool(pending_publication); }));
                CHECK(acknowledgements == 0);
                CHECK_FALSE(completed);
                if (scenario == ManagerCase::Destination) sequence.store(50);
                auto publish = std::move(pending_publication); publish();
            }
            REQUIRE(inbox.pump_until([&] { return completed; }));
            const bool success = scenario == ManagerCase::Download || scenario == ManagerCase::PngDownload ||
                scenario == ManagerCase::DownloadRate;
            CHECK(publications == (success ? 1u : 0u));
            REQUIRE(inbox.pump_until([&] { return success ? acknowledgements.load() == 1 :
                cancellations.load() + failures.load() != 0; }));
            if (!success) CHECK(acknowledgements == 0);
        } else {
            if (scenario == ManagerCase::EarlyComplete) {
                REQUIRE(inbox.pump_until([&] { return begins.load() == 1; }));
            } else {
                REQUIRE(inbox.pump_until([&] { return commits.load() == 1; }));
                CHECK_FALSE(paste_called);
                REQUIRE(inbox.pump_until([&] {
                    const auto messages = host.bulk_messages();
                    return !messages.empty() && bulk::decode_header(messages.back())->kind == bulk::FrameKind::End;
                }));
                host.run([&] {
                    if (png_only) { CHECK_FALSE(begin_message.contains("text")); CHECK_FALSE(begin_message.contains("html")); }
                    else { CHECK(begin_message.at("text") == "local text"); CHECK(begin_message.at("html") == "<b>local</b>"); }
                    const auto parsed = bulk::parse_manifest(begin_message.at("manifest"), json::serialize(begin_message.at("manifest")).size());
                    REQUIRE(parsed);
                    CHECK(parsed->selected_roots == (scenario == ManagerCase::Five ? 5u : png_only ? 0u : 1u));
                });
                if (scenario == ManagerCase::Cancel) manager->cancel();
                if (scenario == ManagerCase::Focus) manager->context({.enabled = true, .active = false});
                if (scenario == ManagerCase::Source) sequence.store(50);
                if (scenario == ManagerCase::SourceAfterEnd) std::ofstream(selected / L"item.txt") << "changed";
                if (scenario == ManagerCase::ClosedAfterEnd) {
                    host.close_bulk();
                    // Drain the independent transport-close callback before control completion.
                    const auto until = std::chrono::steady_clock::now() + 100ms;
                    inbox.pump_until([&] { return std::chrono::steady_clock::now() >= until; }, 1s);
                    CHECK_FALSE(paste_called);
                }
                if (scenario == ManagerCase::Disconnect) {
                    host.close_control(net::protocol::close_superseded);
                    REQUIRE(inbox.pump_until([&] { return connected.disconnected.has_value(); }));
                }
                if (scenario == ManagerCase::NoProgress) clock_seconds.store(31);
                if (scenario == ManagerCase::AbsoluteTimeout) clock_seconds.store(1801);
                if (scenario == ManagerCase::HostExcluded)
                    host.push({{"type", "clipboard_changed"}, {"sequence", 101}, {"excluded", true}, {"formats", json::array{}}});
                if (scenario != ManagerCase::Cancel && scenario != ManagerCase::Focus && scenario != ManagerCase::Disconnect &&
                    scenario != ManagerCase::NoProgress && scenario != ManagerCase::AbsoluteTimeout &&
                    scenario != ManagerCase::HostExcluded) {
                    host.push({{"type", "clipboard_bulk_complete"}, {"transfer_id", transfer}, {"sequence", 100}});
                }
            }
            REQUIRE(inbox.pump_until([&] { return completed && (scenario == ManagerCase::Manual || paste_called); }));
            const bool success = scenario == ManagerCase::Upload || scenario == ManagerCase::Five ||
                scenario == ManagerCase::PngUpload || scenario == ManagerCase::StaleFastPaste || scenario == ManagerCase::UploadRate ||
                scenario == ManagerCase::ClosedAfterEnd || scenario == ManagerCase::CacheInvalidated ||
                scenario == ManagerCase::StaleHostPaste;
            CHECK(paste_result == success);
            CHECK(publications == 0);
            CHECK(confirmations == 0);
            if (scenario == ManagerCase::NoProgress || scenario == ManagerCase::AbsoluteTimeout) CHECK(error == "timeout");
            if (scenario == ManagerCase::Manual) CHECK(error.empty());
            if (scenario == ManagerCase::HostExcluded) { CHECK(error == "excluded"); CHECK(snapshots == 0); }
            if (scenario == ManagerCase::StaleFastPaste || scenario == ManagerCase::StaleHostPaste) {
                bool called{}, forwarded{true};
                manager->paste([&](bool result) { called = true; forwarded = result; });
                REQUIRE(inbox.wait_pending());
                // UI callback is queued but has not run. Inject the already-delivered
                // session event at the real manager boundary, before UI delivery.
                if (scenario == ManagerCase::StaleFastPaste) sequence.store(50);
                else manager->event({{"type", "clipboard_changed"}, {"excluded", true}, {"sequence", 101}});
                REQUIRE(inbox.pump_until([&] { return called; }));
                CHECK_FALSE(forwarded);
                CHECK(begins == 1);
            }
            if (scenario == ManagerCase::CacheInvalidated) {
                host.push({{"type", "clipboard_changed"}, {"sequence", 101}, {"excluded", true}, {"formats", json::array{}}});
                REQUIRE(inbox.pump_until([&] {
                    return std::ranges::any_of(connected.messages, [](const auto& event) {
                        return event.at("type") == "clipboard_changed";
                    });
                }));
                bool called{}, forwarded{true};
                manager->paste([&](bool result) { called = true; forwarded = result; });
                REQUIRE(inbox.pump_until([&] { return begins.load() == 2; }));
                CHECK_FALSE(called); // Must upload anew, never release using the old cache.
                manager->cancel();
                REQUIRE(inbox.pump_until([&] { return called; }));
                CHECK_FALSE(forwarded);
            }
        }
        // A 3 MiB payload needs at least 250 ms beyond a 1 MiB burst at 8 MiB/s.
        // Allow scheduling/timer measurement tolerance; this is not a LAN latency claim.
        if (paced) CHECK(std::chrono::steady_clock::now() - started >= 240ms);
    }
    manager.reset();
    session.reset();
    const auto outgoing = staging.path / L"client" / L"outgoing";
    const auto partial = staging.path / L"client" / L"partial";
    CHECK((!std::filesystem::exists(outgoing) || std::filesystem::is_empty(outgoing)));
    CHECK((!std::filesystem::exists(partial) || std::filesystem::is_empty(partial)));
    CHECK(requested_formats_valid.load());
    if (publications && !published_path.empty()) CHECK(std::filesystem::exists(published_path / L"item.txt"));
}
