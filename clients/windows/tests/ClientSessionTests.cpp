#include "ClientSession.hpp"

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
    void close_control(unsigned short code) {
        run([&] {
            if (!control_) return;
            control_->socket()->async_close(websocket::close_reason(code), [](beast::error_code) {});
        });
    }
    bool stream_connected() {
        bool result{};
        run([&] { result = stream_ != nullptr; });
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
            }
        } catch (...) {
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
};

std::unique_ptr<Session> connect(Inbox& inbox, Connected& state, ConnectOptions options) {
    Session::Events events;
    events.message = [&state](const json::object& message) { state.messages.push_back(message); };
    events.disconnected = [&state](const SessionError& error) { state.disconnected = error; };
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
