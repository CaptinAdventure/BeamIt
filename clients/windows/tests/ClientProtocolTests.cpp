#include "ClientProtocol.hpp"

#include "net/Protocol.hpp"

#include <catch2/catch_test_macros.hpp>

#include <thread>

using namespace beamit;
using namespace beamit::client;

TEST_CASE("Server URLs require a secure scheme and normalize the authority", "[client-protocol]") {
    auto endpoint = parse_server_url("  wss://Host.example:8443/ignored?x#y ");
    REQUIRE(endpoint);
    CHECK(endpoint->host == "Host.example");
    CHECK(endpoint->port == 8443);
    CHECK(endpoint->authority == "Host.example:8443");

    endpoint = parse_server_url("https://10.0.0.5");
    REQUIRE(endpoint);
    CHECK(endpoint->port == 443);
    CHECK(endpoint->authority == "10.0.0.5:443");

    endpoint = parse_server_url("wss://[fe80::1]:9000");
    REQUIRE(endpoint);
    CHECK(endpoint->host == "fe80::1");
    CHECK(endpoint->authority == "[fe80::1]:9000");

    for (const auto* invalid : {"ws://host:1", "http://host", "host:8443", "wss://", "wss://user@host:1",
                                "wss://host:0", "wss://host:65536", "wss://host:", "wss://fe80::1:9000",
                                "wss://ho st:1", "wss://[fe80::1"}) {
        INFO(invalid);
        CHECK_FALSE(parse_server_url(invalid));
    }
    CHECK(parse_server_url("wss://host:1")->same_origin("HOST:1"));
    CHECK_FALSE(parse_server_url("wss://host:1")->same_origin("host:2"));
    CHECK_FALSE(parse_server_url("wss://host:1")->same_origin("other:1"));
}

TEST_CASE("Fingerprints accept the host log format", "[client-protocol]") {
    const std::string logged =
        "00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:aa:bb:cc:dd:ee:ff";
    const auto parsed = parse_fingerprint(logged);
    REQUIRE(parsed);
    CHECK(std::to_integer<unsigned>((*parsed)[10]) == 0xAA);
    CHECK(format_fingerprint(*parsed) ==
          "00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF");
    CHECK(parse_fingerprint(" 00112233445566778899aabbccddeeff\n00112233445566778899AABBCCDDEEFF "));
    CHECK_FALSE(parse_fingerprint(logged.substr(3)));
    CHECK_FALSE(parse_fingerprint(logged + ":00"));
    CHECK_FALSE(parse_fingerprint("zz" + logged.substr(2)));
    CHECK_FALSE(parse_fingerprint(""));
}

namespace {

json::object hello(std::int64_t iterations = net::protocol::pbkdf2_iterations) {
    return {
        {"type", "hello"},
        {"protocol", 1},
        {"server", "BeamIt"},
        {"auth", json::object{
            {"mechanism", "pbkdf2-sha256-hmac"},
            {"salt", net::base64url_encode(net::random_bytes(16))},
            {"iterations", iterations},
            {"server_nonce", net::base64url_encode(net::random_bytes(32))},
        }},
    };
}

} // namespace

TEST_CASE("The hello challenge is validated and proofs match PROTOCOL.md section 4.2", "[client-protocol]") {
    const auto message = hello();
    const auto challenge = parse_hello(message);
    REQUIRE(challenge);
    const auto exchange = create_authentication(*challenge, "pässword");
    CHECK(exchange.message.at("type") == "authenticate");
    CHECK(exchange.message.at("protocol") == 1);
    const auto client_nonce = net::base64url_decode(std::string_view(exchange.message.at("client_nonce").as_string()));
    REQUIRE(client_nonce);
    CHECK(client_nonce->size() == 32);
    const auto key = net::pbkdf2_sha256("pässword", challenge->salt, challenge->iterations);
    CHECK(exchange.message.at("proof").as_string() ==
          net::base64url_encode(net::auth_proof(key, "client", challenge->server_nonce, *client_nonce)));
    const auto server = net::auth_proof(key, "server", challenge->server_nonce, *client_nonce);
    CHECK(net::constant_time_equal(server, exchange.expected_server_proof));

    CHECK_FALSE(parse_hello(hello(1)));
    CHECK_FALSE(parse_hello(hello(100'000'000)));
    auto wrong = hello();
    wrong["protocol"] = 2;
    CHECK_FALSE(parse_hello(wrong));
    wrong = hello();
    wrong["auth"].as_object()["mechanism"] = "plain";
    CHECK_FALSE(parse_hello(wrong));
    wrong = hello();
    wrong["auth"].as_object()["server_nonce"] = net::base64url_encode(net::random_bytes(16));
    CHECK_FALSE(parse_hello(wrong));
    wrong = hello();
    wrong["auth"].as_object()["salt"] = "not base64!";
    CHECK_FALSE(parse_hello(wrong));
}

namespace {

json::object offer(std::string url) {
    return {
        {"type", "stream_offer"},
        {"id", 3},
        {"stream_id", "Q2hOc3kV2bTqjG6dAVu0Sg"},
        {"url", std::move(url)},
        {"token_expires_in_ms", 10000},
        {"source", json::object{{"kind", "window"}, {"hwnd", "0x1A2B3C"}, {"pid", 4242}, {"title", "Teams"},
                                {"capture", "wgc"}, {"dpi", json::object{{"x", 144}, {"y", 144}}}}},
        {"video", json::object{{"generation", 1}, {"codec", "h265"}, {"codec_string", "hev1.4.10.L120.90"},
                               {"format", "annexb"}, {"width", 1920}, {"height", 1080}, {"chroma_format", "4:4:4"},
                               {"bit_depth", 8}, {"frame_rate", 60}, {"bitrate_kbps", 12000},
                               {"max_bitrate_kbps", 24000}, {"rate_control", "vbr"}}},
        {"input", json::object{{"mouse_mode", "absolute"}}},
    };
}

} // namespace

TEST_CASE("Stream offers must stay on the control origin", "[client-protocol]") {
    const auto control = *parse_server_url("wss://host.lan:8443");
    const auto parsed = parse_stream_offer(offer("wss://HOST.lan:8443/streams/Q2hOc3kV2bTqjG6dAVu0Sg?token=abc"), control);
    REQUIRE(parsed);
    CHECK(parsed->target == "/streams/Q2hOc3kV2bTqjG6dAVu0Sg?token=abc");
    CHECK(parsed->token_lifetime == std::chrono::milliseconds(10000));
    CHECK(parsed->video.codec == "h265");
    CHECK(parsed->video.chroma_format == "4:4:4");
    CHECK(parsed->video.max_bitrate_kbps == 24000U);
    CHECK(parsed->source.pid == 4242);
    CHECK(parsed->source.dpi_x == 144U);
    CHECK(parsed->mouse_mode == "absolute");
    CHECK(parsed->cursor == "video");
    auto mirrored = offer("wss://host.lan:8443/streams/Q2hOc3kV2bTqjG6dAVu0Sg?token=abc");
    mirrored["input"].as_object()["cursor"] = "client";
    CHECK(parse_stream_offer(mirrored, control)->cursor == "client");

    for (const auto* url : {"ws://host.lan:8443/streams/Q2hOc3kV2bTqjG6dAVu0Sg?token=abc",
                            "wss://evil.lan:8443/streams/Q2hOc3kV2bTqjG6dAVu0Sg?token=abc",
                            "wss://host.lan:9443/streams/Q2hOc3kV2bTqjG6dAVu0Sg?token=abc",
                            "wss://host.lan:8443/streams/other0000000000?token=abc",
                            "wss://host.lan:8443/streams/Q2hOc3kV2bTqjG6dAVu0Sg",
                            "wss://host.lan:8443/control?token=abc"}) {
        INFO(url);
        CHECK_FALSE(parse_stream_offer(offer(url), control));
    }
    auto invalid = offer("wss://host.lan:8443/streams/Q2hOc3kV2bTqjG6dAVu0Sg?token=abc");
    invalid["video"].as_object()["codec"] = "av1";
    CHECK_FALSE(parse_stream_offer(invalid, control));
    invalid = offer("wss://host.lan:8443/streams/Q2hOc3kV2bTqjG6dAVu0Sg?token=abc");
    invalid["token_expires_in_ms"] = 0;
    CHECK_FALSE(parse_stream_offer(invalid, control));
    invalid = offer("wss://host.lan:8443/streams/a/b?token=abc");
    invalid["stream_id"] = "a/b";
    CHECK_FALSE(parse_stream_offer(invalid, control));
}

TEST_CASE("Clock estimates prefer the lowest round trip within 30 seconds", "[client-protocol]") {
    using namespace std::chrono_literals;
    ClockEstimate clock;
    const auto start = std::chrono::steady_clock::time_point(100s);
    clock.record(5'000'000, start, start + 10ms);
    clock.record(5'000'000, start + 1s, start + 1s + 2ms);
    const auto best = clock.best(start + 2s);
    REQUIRE(best);
    CHECK(best->rtt_us == 2000.0);
    CHECK(best->offset_us == 5'000'000.0 - (101'000'000.0 + 101'002'000.0) / 2);
    CHECK_FALSE(clock.best(start + 40s));
    clock.record(1, start + 3s, start + 2s);
    CHECK(clock.best(start + 3s)->rtt_us == 2000.0);
}
