#include "host/wss/ControlSession.hpp"
#include "host/wss/HttpFrontend.hpp"

#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <set>

using namespace beamit;
using namespace beamit::host;
using namespace beamit::host::wss;
using namespace std::chrono_literals;

namespace {

class TestAudioCapture final : public AudioCapture {
public:
    void stop() noexcept override {}
};

std::map<std::uint32_t, AudioCaptureCallbacks> capture_callbacks;

awaitable<std::shared_ptr<WebSocket>> connect_tls(Server& server, wintls::context& tls, unsigned short port) {
    auto socket = std::make_shared<WebSocket>(server.io, tls);
    co_await beast::get_lowest_layer(*socket).async_connect(
        tcp::endpoint(asio::ip::address_v4::loopback(), port), use_awaitable);
    co_await socket->next_layer().async_handshake(wintls::handshake_type::client, use_awaitable);
    co_return socket;
}

awaitable<void> send_json(WebSocket& socket, json::object message) {
    const auto text = json::serialize(message);
    socket.text(true);
    co_await socket.async_write(asio::buffer(text), use_awaitable);
}

awaitable<json::object> read_json(WebSocket& socket) {
    while (true) {
        beast::flat_buffer buffer;
        co_await socket.async_read(buffer, use_awaitable);
        REQUIRE(socket.got_text());
        auto message = json::parse(beast::buffers_to_string(buffer.data())).as_object();
        if (message.at("type") != "stream_stats") co_return message;
    }
}

awaitable<void> reject_upgrade(Server& server, wintls::context& tls, unsigned short port, std::string path,
                               std::string origin = {}) {
    auto socket = co_await connect_tls(server, tls, port);
    if (!origin.empty()) {
        socket->set_option(websocket::stream_base::decorator([origin](websocket::request_type& request) {
            request.set(http::field::origin, origin);
        }));
    }
    websocket::response_type response;
    beast::error_code error;
    co_await socket->async_handshake(response, "localhost", path, asio::redirect_error(use_awaitable, error));
    CHECK(error == websocket::error::upgrade_declined);
    CHECK(response.result() == http::status::forbidden);
}

awaitable<std::shared_ptr<WebSocket>> authenticate_client(Server& server, wintls::context& tls, unsigned short port) {
    auto control = co_await connect_tls(server, tls, port);
    co_await control->async_handshake("localhost", "/control", use_awaitable);
    const auto hello = co_await read_json(*control);
    const auto nonce = net::base64url_decode(
        std::string_view(hello.at("auth").as_object().at("server_nonce").as_string()));
    REQUIRE(nonce);
    const auto client_nonce = net::random_bytes(net::protocol::nonce_bytes);
    const auto proof = net::auth_proof(server.options.auth_key, "client", *nonce, client_nonce);
    co_await send_json(*control, {{"type", "authenticate"}, {"protocol", net::protocol::version},
        {"client_nonce", net::base64url_encode(client_nonce)}, {"proof", net::base64url_encode(proof)}});
    const auto authenticated = co_await read_json(*control);
    REQUIRE(authenticated.at("type") == "authenticated");
    CHECK(authenticated.at("server_proof").as_string() ==
        net::base64url_encode(net::auth_proof(server.options.auth_key, "server", *nonce, client_nonce)));
    co_return control;
}

awaitable<void> pause(asio::io_context& io) {
    asio::steady_timer timer(io, 1ms);
    co_await timer.async_wait(use_awaitable);
}

std::string offer_path(const json::object& offer) {
    const std::string url(offer.at("url").as_string());
    return url.substr(url.find('/', 6));
}

awaitable<void> exercise_audio(Server& server, unsigned short port) {
    wintls::context tls(wintls::method::system_default);
    // Loopback-only test client; production clients must validate/pin the host certificate.
    tls.verify_server_certificate(false);
    co_await reject_upgrade(server, tls, port, "/audio/unknown?token=invalid");
    auto unauthenticated = co_await connect_tls(server, tls, port);
    co_await unauthenticated->async_handshake("localhost", "/control", use_awaitable);
    (void)co_await read_json(*unauthenticated);
    co_await send_json(*unauthenticated, {{"type", "start_audio"}});
    CHECK((co_await read_json(*unauthenticated)).at("code") == "not_authenticated");
    beast::error_code ignored;
    co_await unauthenticated->async_close(websocket::close_code::normal, asio::redirect_error(use_awaitable, ignored));

    auto control = co_await authenticate_client(server, tls, port);
    co_await send_json(*control, {{"type", "start_audio"}, {"id", 1}});
    auto offer = co_await read_json(*control);
    REQUIRE(offer.at("type") == "audio_offer");
    CHECK(offer.at("id") == 1);
    CHECK(offer.at("audio_version") == 2);
    CHECK(offer.at("hostToClient").as_object().at("tracks").as_array().empty());
    CHECK(offer.at("clientToHost").as_object().at("track_id") == protocol::microphone_track_id);
    const auto& opus = offer.at("clientToHost").as_object().at("opusAudioConfiguration").as_object();
    CHECK(opus.at("frameSize") == 480);
    CHECK(opus.at("packetDuration") == "PT0.01S");
    auto path = offer_path(offer);
    co_await reject_upgrade(server, tls, port, path + "wrong");
    co_await reject_upgrade(server, tls, port, path, "https://foreign.invalid");
    co_await send_json(*control, {{"type", "start_audio"}, {"id", 2}});
    CHECK((co_await read_json(*control)).at("code") == "limit_reached");
    co_await send_json(*control, {{"type", "start_audio_track"}, {"audio_id", offer.at("audio_id")},
        {"source", json::object{{"kind", "desktop"}}}});
    CHECK((co_await read_json(*control)).at("code") == "not_ready");
    auto audio = co_await connect_tls(server, tls, port);
    co_await audio->async_handshake("localhost", path, use_awaitable);
    co_await reject_upgrade(server, tls, port, path);
    while (!server.options.audio->connected()) co_await pause(server.io);
    const media::AudioConfiguration configuration;
    std::vector<float> pcm(configuration.sample_count(), 0.1f);
    const auto audio_id = offer.at("audio_id");
    co_await send_json(*control, {{"type", "start_audio_track"}, {"audio_id", audio_id},
        {"source", json::object{{"kind", "application"}, {"pid", 1234}}}});
    auto track_reply = co_await read_json(*control);
    REQUIRE(track_reply.at("type") == "audio_track_started");
    const auto track_a = static_cast<std::uint32_t>(track_reply.at("track").as_object().at("track_id").as_int64());
    CHECK(track_reply.at("track").as_object().at("source").as_object().at("pid") == 1234);
    CHECK(track_reply.at("track").as_object().at("opusAudioConfiguration") ==
          offer.at("clientToHost").as_object().at("opusAudioConfiguration"));
    co_await send_json(*control, {{"type", "start_audio_track"}, {"audio_id", audio_id},
        {"source", json::object{{"kind", "application"}, {"pid", 5678}}}});
    track_reply = co_await read_json(*control);
    REQUIRE(track_reply.at("type") == "audio_track_started");
    const auto track_b = static_cast<std::uint32_t>(track_reply.at("track").as_object().at("track_id").as_int64());
    CHECK(track_b != track_a);
    CHECK(server.options.audio->tracks().size() == 3);
    co_await send_json(*control, {{"type", "start_audio_track"}, {"audio_id", audio_id},
        {"source", json::object{{"kind", "application"}, {"pid", 1234}}}});
    CHECK((co_await read_json(*control)).at("code") == "invalid_request");
    co_await send_json(*control, {{"type", "start_audio_track"}, {"audio_id", audio_id},
        {"source", json::object{{"kind", "desktop"}}}});
    CHECK((co_await read_json(*control)).at("code") == "invalid_request");
    REQUIRE(server.options.audio->submit_pcm(track_a, pcm) == AudioSubmitResult::Accepted);
    REQUIRE(server.options.audio->submit_pcm(track_b, pcm) == AudioSubmitResult::Accepted);
    beast::flat_buffer buffer;
    media::OpusAudio client_codec;
    std::set<std::uint32_t> received_tracks;
    for (unsigned index = 0; index < 2; ++index) {
        buffer.clear();
        co_await audio->async_read(buffer, use_awaitable);
        REQUIRE(audio->got_binary());
        const auto data = buffer.cdata();
        const auto packet = net::protocol::decode_audio_packet(
            {static_cast<const std::byte*>(data.data()), data.size()}, configuration, std::nullopt);
        CHECK(packet.sequence == 0);
        CHECK(client_codec.decode(packet.payload, true).size() == pcm.size());
        received_tracks.insert(packet.track_id);
    }
    CHECK(received_tracks == std::set<std::uint32_t>{track_a, track_b});
    co_await send_json(*control, {{"type", "start_audio_track"}, {"id", 77}, {"audio_id", audio_id},
        {"source", json::object{{"kind", "application"}, {"pid", 7777}}}});
    const auto initialization_failure = co_await read_json(*control);
    CHECK(initialization_failure.at("type") == "audio_track_stopped");
    CHECK(initialization_failure.at("id") == 77);
    CHECK(initialization_failure.at("reason") == "capture_failed");
    CHECK(server.options.audio->connected());

    // Stop while activation is pending; a late readiness callback cannot resurrect the removed track.
    co_await send_json(*control, {{"type", "start_audio_track"}, {"id", 78}, {"audio_id", audio_id},
        {"source", json::object{{"kind", "application"}, {"pid", 8888}}}});
    co_await send_json(*control, {{"type", "ping"}});
    CHECK((co_await read_json(*control)).at("type") == "pong");
    const auto routes = server.options.audio->tracks();
    const auto pending = std::ranges::find_if(routes, [](const auto& track) { return track.pid == 8888; });
    REQUIRE(pending != routes.end());
    co_await send_json(*control, {{"type", "stop_audio_track"}, {"id", 79}, {"audio_id", audio_id},
        {"track_id", pending->id}});
    const auto cancelled_start = co_await read_json(*control);
    const auto cancelled_stop = co_await read_json(*control);
    CHECK(cancelled_start.at("id") == 78);
    CHECK(cancelled_stop.at("id") == 79);
    capture_callbacks.at(pending->id).started();
    co_await send_json(*control, {{"type", "ping"}});
    CHECK((co_await read_json(*control)).at("type") == "pong");
    // A capture error removes only that track, while another encoder and the microphone remain active.
    co_await send_json(*control, {{"type", "start_audio_track"}, {"audio_id", audio_id},
        {"source", json::object{{"kind", "application"}, {"pid", 6789}}}});
    track_reply = co_await read_json(*control);
    const auto failing_track =
        static_cast<std::uint32_t>(track_reply.at("track").as_object().at("track_id").as_int64());
    capture_callbacks.at(failing_track).failed("Simulated WASAPI device failure");
    const auto failed_track = co_await read_json(*control);
    CHECK(failed_track.at("type") == "audio_track_stopped");
    CHECK(failed_track.at("track_id") == failing_track);
    CHECK(failed_track.at("reason") == "capture_failed");
    CHECK(server.options.audio->connected());
    CHECK(server.options.audio->submit_pcm(failing_track, pcm) == AudioSubmitResult::NotConnected);
    audio->binary(true);
    const auto incoming = net::protocol::encode_audio_packet(
        {.discontinuity = true, .payload = client_codec.encode(pcm)}, configuration);
    co_await audio->async_write(asio::buffer(incoming), use_awaitable);
    std::optional<AudioPcmPacket> decoded;
    while (!(decoded = server.options.audio->receive_pcm())) co_await pause(server.io);
    CHECK(decoded->samples.size() == pcm.size());
    CHECK(decoded->sequence == 0);

    // Removing capture tracks does not replace the microphone or the socket.
    co_await send_json(*control, {{"type", "stop_audio_track"}, {"audio_id", audio_id}, {"track_id", track_a}});
    CHECK((co_await read_json(*control)).at("type") == "audio_track_stopped");
    CHECK(server.options.audio->submit_pcm(track_a, pcm) == AudioSubmitResult::NotConnected);
    CHECK(server.options.audio->connected());
    REQUIRE(server.options.audio->submit_pcm(track_b, pcm) == AudioSubmitResult::Accepted);
    buffer.clear();
    co_await audio->async_read(buffer, use_awaitable);
    const auto surviving_data = buffer.cdata();
    const auto surviving_packet = net::protocol::decode_audio_packet(
        {static_cast<const std::byte*>(surviving_data.data()), surviving_data.size()}, configuration, 0);
    CHECK(surviving_packet.track_id == track_b);
    CHECK(surviving_packet.sequence == 1);
    co_await send_json(*control, {{"type", "stop_audio_track"}, {"audio_id", audio_id}, {"track_id", track_b}});
    CHECK((co_await read_json(*control)).at("type") == "audio_track_stopped");
    co_await send_json(*control, {{"type", "stop_audio_track"}, {"audio_id", audio_id},
        {"track_id", protocol::microphone_track_id}});
    CHECK((co_await read_json(*control)).at("code") == "invalid_request");
    co_await send_json(*control, {{"type", "start_audio_track"}, {"audio_id", audio_id},
        {"source", json::object{{"kind", "desktop"}}}});
    track_reply = co_await read_json(*control);
    REQUIRE(track_reply.at("type") == "audio_track_started");
    const auto desktop = static_cast<std::uint32_t>(track_reply.at("track").as_object().at("track_id").as_int64());
    CHECK(desktop > track_b);
    CHECK(track_reply.at("track").as_object().at("source").as_object().at("exclude_host_process_tree") == true);
    co_await send_json(*control, {{"type", "start_audio_track"}, {"audio_id", audio_id},
        {"source", json::object{{"kind", "application"}, {"pid", 1234}}}});
    CHECK((co_await read_json(*control)).at("code") == "invalid_request");
    REQUIRE(server.options.audio->submit_pcm(desktop, pcm) == AudioSubmitResult::Accepted);
    buffer.clear();
    co_await audio->async_read(buffer, use_awaitable);
    const auto desktop_data = buffer.cdata();
    const auto desktop_packet = net::protocol::decode_audio_packet(
        {static_cast<const std::byte*>(desktop_data.data()), desktop_data.size()}, configuration, std::nullopt);
    CHECK(desktop_packet.track_id == desktop);
    CHECK(desktop_packet.sequence == 0);
    const auto next_microphone = net::protocol::encode_audio_packet(
        {.sequence = 1, .sample_timestamp = configuration.frame_size(), .payload = client_codec.encode(pcm)},
        configuration);
    co_await audio->async_write(asio::buffer(next_microphone), use_awaitable);
    while (!(decoded = server.options.audio->receive_pcm())) co_await pause(server.io);
    CHECK(decoded->sequence == 1);
    co_await send_json(*control, {{"type", "stop_audio_track"}, {"audio_id", audio_id}, {"track_id", desktop}});
    CHECK((co_await read_json(*control)).at("type") == "audio_track_stopped");
    for (unsigned index = 0; index < protocol::max_audio_capture_tracks; ++index) {
        co_await send_json(*control, {{"type", "start_audio_track"}, {"audio_id", audio_id},
            {"source", json::object{{"kind", "application"}, {"pid", 10'000 + index}}}});
        CHECK((co_await read_json(*control)).at("type") == "audio_track_started");
    }
    co_await send_json(*control, {{"type", "start_audio_track"}, {"audio_id", audio_id},
        {"source", json::object{{"kind", "application"}, {"pid", 99'999}}}});
    CHECK((co_await read_json(*control)).at("code") == "limit_reached");

    // A client cannot inject packets into a host capture track.
    const auto invalid = net::protocol::encode_audio_packet(
        {.track_id = desktop, .sequence = 2, .sample_timestamp = configuration.frame_size() * 2,
         .payload = client_codec.encode(pcm)}, configuration);
    co_await audio->async_write(asio::buffer(invalid), use_awaitable);
    auto stopped = co_await read_json(*control);
    CHECK(stopped.at("type") == "audio_stopped");
    CHECK(stopped.at("reason") == "invalid_packet");
    CHECK_FALSE(server.options.audio->connected());
    CHECK(server.options.audio->tracks().empty());
    co_await audio->async_close(websocket::close_code::normal, asio::redirect_error(use_awaitable, ignored));
    co_await send_json(*control, {{"type", "ping"}, {"id", 3}});
    CHECK((co_await read_json(*control)).at("type") == "pong");

    // Explicit stop invalidates a pending token.
    co_await send_json(*control, {{"type", "start_audio"}});
    offer = co_await read_json(*control);
    co_await send_json(*control, {{"type", "stop_audio"}, {"id", 4}, {"audio_id", offer.at("audio_id")}});
    stopped = co_await read_json(*control);
    CHECK(stopped.at("id") == 4);
    CHECK(stopped.at("reason") == "requested");
    co_await reject_upgrade(server, tls, port, offer_path(offer));

    // A valid envelope with malformed Opus fails explicitly on the worker.
    co_await send_json(*control, {{"type", "start_audio"}});
    offer = co_await read_json(*control);
    audio = co_await connect_tls(server, tls, port);
    co_await audio->async_handshake("localhost", offer_path(offer), use_awaitable);
    audio->binary(true);
    const auto corrupt = net::protocol::encode_audio_packet(
        {.discontinuity = true, .payload = {std::byte{0xff}}}, configuration);
    co_await audio->async_write(asio::buffer(corrupt), use_awaitable);
    stopped = co_await read_json(*control);
    CHECK(stopped.at("reason") == "codec_failed");
    CHECK_FALSE(server.options.audio->connected());
    co_await audio->async_close(websocket::close_code::normal, asio::redirect_error(use_awaitable, ignored));

    // Pending offers expire even when no audio socket is opened.
    co_await send_json(*control, {{"type", "start_audio"}});
    offer = co_await read_json(*control);
    stopped = co_await read_json(*control);
    CHECK(stopped.at("type") == "audio_stopped");
    CHECK(stopped.at("reason") == "token_expired");
    co_await reject_upgrade(server, tls, port, offer_path(offer));

    // A newer authenticated session tears down active audio.
    co_await send_json(*control, {{"type", "start_audio"}});
    offer = co_await read_json(*control);
    audio = co_await connect_tls(server, tls, port);
    co_await audio->async_handshake("localhost", offer_path(offer), use_awaitable);
    while (!server.options.audio->connected()) co_await pause(server.io);
    auto replacement = co_await authenticate_client(server, tls, port);
    CHECK_FALSE(server.options.audio->connected());
    co_await reject_upgrade(server, tls, port, offer_path(offer));
    co_await audio->async_close(websocket::close_code::normal, asio::redirect_error(use_awaitable, ignored));
    co_await control->async_close(websocket::close_code::normal, asio::redirect_error(use_awaitable, ignored));
    co_await replacement->async_close(websocket::close_code::normal, use_awaitable);
}

} // namespace

TEST_CASE("Audio WSS authenticates and exchanges Opus in both directions", "[.audio-wss]") {
    HostServerOptions options;
    try {
        options.identity = installed_identity();
    } catch (const MissingTlsCertificate&) {
        SKIP("Run Install-BeamItCert.ps1 before the local TLS integration test");
    }
    options.auth_salt = net::random_bytes(16);
    options.auth_iterations = 1;
    capture_callbacks.clear();
    options.make_audio_capture = [](AudioTrack track, std::shared_ptr<AudioEndpoint>, AudioCaptureCallbacks callbacks) {
        capture_callbacks[track.id] = callbacks;
        if (track.pid == 7777) callbacks.failed("Simulated activation failure");
        else if (track.pid != 8888) callbacks.started();
        return std::make_unique<TestAudioCapture>();
    };
    Server server(std::move(options));
    server.tls.use_certificate(server.options.identity.certificate.get());
    tcp::acceptor acceptor(server.io, {asio::ip::address_v4::loopback(), 0});
    asio::co_spawn(server.io, accept_loop(server, acceptor), asio::detached);
    bool completed{};
    bool timed_out{};
    std::exception_ptr failure;
    asio::steady_timer timeout(server.io, 20s);
    timeout.async_wait([&](beast::error_code error) {
        if (error) return;
        timed_out = true;
        if (auto session = server.active.lock()) session->close(websocket::close_code::going_away, "test timeout");
        server.io.stop();
    });
    asio::co_spawn(server.io, exercise_audio(server, acceptor.local_endpoint().port()), [&](std::exception_ptr error) {
        failure = error;
        completed = true;
        if (auto session = server.active.lock()) session->close(websocket::close_code::going_away, "test ended");
        timeout.cancel();
        server.io.stop();
    });
    server.io.run();
    REQUIRE_FALSE(timed_out);
    REQUIRE(completed);
    if (failure) std::rethrow_exception(failure);
}
