#include "host/HostControl.hpp"
#include "host/wss/ControlSession.hpp"
#include "net/Protocol.hpp"

#include <catch2/catch_test_macros.hpp>

#include <thread>

using namespace beamit;
using namespace beamit::host;

TEST_CASE("Generated connection passwords are 36 URL-safe random characters", "[host-control]") {
    const auto first = generate_connection_password();
    const auto second = generate_connection_password();
    CHECK(first.size() == 36);
    CHECK(second.size() == 36);
    CHECK(first != second);
    CHECK(first.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") ==
          std::string::npos);
    const auto decoded = net::base64url_decode(first);
    REQUIRE(decoded);
    CHECK(decoded->size() == 27);
}

TEST_CASE("Tray commands and status cross threads without sharing mutable state", "[host-control]") {
    HostControl control;
    std::thread ui([&] {
        control.submit({.kind = HostCommand::Kind::DisconnectStream, .stream_id = "stream-a"});
        control.submit({.kind = HostCommand::Kind::Exit});
    });
    ui.join();
    const auto commands = control.take_commands();
    REQUIRE(commands.size() == 2);
    CHECK(commands[0].kind == HostCommand::Kind::DisconnectStream);
    CHECK(commands[0].stream_id == "stream-a");
    CHECK(commands[1].kind == HostCommand::Kind::Exit);
    CHECK(control.take_commands().empty());

    const auto sampled_at = std::chrono::steady_clock::now();
    control.publish({.client = "192.168.1.2", .streams = {{"stream-b", "Teams", "Frames: encoded 60", sampled_at}},
                     .listening = true, .input_elevation = InputElevation::NotElevated});
    auto snapshot = control.status();
    REQUIRE(snapshot.streams.size() == 1);
    CHECK(snapshot.client == "192.168.1.2");
    CHECK(snapshot.streams[0].description == "Teams");
    CHECK(snapshot.streams[0].statistics == "Frames: encoded 60");
    CHECK(snapshot.streams[0].sampled_at == sampled_at);
    CHECK(snapshot.listening);
    CHECK(snapshot.input_elevation == InputElevation::NotElevated);
    snapshot.streams.clear();
    CHECK(control.status().streams.size() == 1);
    control.publish({});
    CHECK(control.status().client.empty());
    CHECK(control.status().streams.empty());
}

TEST_CASE("Host status preserves input elevation without a connected client", "[host-control]") {
    wss::Server server{HostServerOptions{}};
    server.options.control = std::make_shared<HostControl>();
    for (const auto elevation : {InputElevation::Elevated, InputElevation::NotElevated, InputElevation::Unknown}) {
        server.input_elevation = elevation;
        server.publish_status();
        const auto status = server.options.control->status();
        CHECK(status.listening);
        CHECK(status.input_elevation == elevation);
        CHECK(status.client.empty());
        CHECK(status.streams.empty());
    }
}

TEST_CASE("Input elevation reports are delivered once without replacing host status", "[host-control]") {
    HostControl control;
    control.publish({.listening = true, .input_elevation = InputElevation::NotElevated});
    std::thread worker([&] {
        control.notify({false, "UAC cancelled; previous input handler remains active"});
    });
    worker.join();
    auto notices = control.take_notices();
    REQUIRE(notices.size() == 1);
    CHECK_FALSE(notices.front().success);
    CHECK(notices.front().message.find("previous input handler") != std::string::npos);
    CHECK(control.take_notices().empty());
    CHECK(control.status().listening);
    CHECK(control.status().input_elevation == InputElevation::NotElevated);
    control.notify({true, "Input elevated"});
    notices = control.take_notices();
    REQUIRE(notices.size() == 1);
    CHECK(notices.front().success);
}

TEST_CASE("Local password changes replace the verifier and invalidate pending challenges", "[host-control]") {
    wss::Server server{HostServerOptions{}};
    server.options.auth_iterations = net::protocol::pbkdf2_iterations;
    HostCommand command{.kind = HostCommand::Kind::ChangePassword};
    command.auth_salt = net::random_bytes(16);
    command.auth_key = net::pbkdf2_sha256("new connection password", command.auth_salt, 1);
    const auto salt = command.auth_salt;
    const auto key = command.auth_key;
    server.change_password(command);
    CHECK(server.options.auth_key == key);
    CHECK(server.options.auth_salt == salt);
    CHECK(server.auth_generation == 1);
    command.auth_salt = net::random_bytes(16);
    command.disconnect_client = true;
    server.change_password(command);
    CHECK(server.auth_generation == 2);
    CHECK(server.options.auth_iterations == net::protocol::pbkdf2_iterations);
}
