#include "host/HostServer.hpp"
#include "host/ElevatedInput.hpp"

#include <catch2/catch_test_macros.hpp>

#include <limits>

using namespace beamit::host::input;

TEST_CASE("Input helper IPC rejects malformed and noncanonical messages", "[input][input-ipc]") {
    ipc::Request request;
    CHECK(ipc::valid(request));
    request.revision++;
    CHECK_FALSE(ipc::valid(request));
    request = {};
    request.operation = static_cast<ipc::Operation>(100);
    CHECK_FALSE(ipc::valid(request));
    request = {};
    request.window = 1; // Unused fields must be zero.
    CHECK_FALSE(ipc::valid(request));
    request = {};
    request.operation = ipc::Operation::Absolute;
    CHECK_FALSE(ipc::valid(request)); // Zero stream identifier.
    request.stream = 1;
    CHECK(ipc::valid(request));
    request.x = std::numeric_limits<double>::infinity();
    CHECK_FALSE(ipc::valid(request));
    request.x = std::numeric_limits<double>::quiet_NaN();
    CHECK_FALSE(ipc::valid(request));
    request.x = 1'000'001;
    CHECK_FALSE(ipc::valid(request));
    request.x = -1'000'000;
    request.width = 65536;
    CHECK(ipc::valid(request));
    request.width++;
    CHECK_FALSE(ipc::valid(request));

    ipc::Response response;
    CHECK(ipc::valid(response));
    response.result = 99;
    CHECK_FALSE(ipc::valid(response));
    response = {};
    response.modifiers = 128;
    CHECK_FALSE(ipc::valid(response));
    response = {};
    response.present = 2;
    CHECK_FALSE(ipc::valid(response));
    response = {};
    response.diagnostic_present = 1;
    CHECK_FALSE(ipc::valid(response));
    response.diagnostic.injector_pid = GetCurrentProcessId();
    response.diagnostic.requested = 2;
    response.diagnostic.inserted = 1;
    CHECK(ipc::valid(response));
    response.diagnostic.inserted = 3;
    CHECK_FALSE(ipc::valid(response));
}

TEST_CASE("Input helper validates input domains independently of JSON", "[input][input-ipc]") {
    ipc::Request request;
    request.stream = 1;
    request.operation = ipc::Operation::Button;
    for (int button = 0; button <= 4; ++button) {
        request.first = button;
        CHECK(ipc::valid(request));
    }
    request.first = 5;
    CHECK_FALSE(ipc::valid(request));
    request.first = 0;
    request.flags = 2;
    CHECK_FALSE(ipc::valid(request));

    request = {};
    request.stream = 1;
    request.operation = ipc::Operation::Key;
    request.first = key_for_code("KeyA")->scan;
    request.flags = 1;
    CHECK(ipc::valid(request));
    request.first = 0x7f; // Not in the supported KeyboardEvent.code table.
    CHECK_FALSE(ipc::valid(request));
    const auto pause = *key_for_code("Pause");
    request.first = pause.scan;
    request.second = pause.virtual_key;
    CHECK(ipc::valid(request));
    request.flags = 4;
    CHECK_FALSE(ipc::valid(request));

    request = {};
    request.stream = 1;
    request.operation = ipc::Operation::Wheel;
    request.first = -12000;
    request.second = 12000;
    CHECK(ipc::valid(request));
    request.second++;
    CHECK_FALSE(ipc::valid(request));

    request = {};
    request.stream = 1;
    request.operation = ipc::Operation::Text;
    request.length = 1;
    request.text[0] = L'a';
    CHECK(ipc::valid(request));
    request.text[1] = L'x'; // Hidden trailing content is rejected.
    CHECK_FALSE(ipc::valid(request));
    request.text[1] = 0;
    request.text[0] = 0x07;
    CHECK_FALSE(ipc::valid(request));
    request.text[0] = 0xd800; // Unpaired UTF-16 surrogate.
    CHECK_FALSE(ipc::valid(request));
    request.text[0] = 0xd83d;
    request.text[1] = 0xdc4b;
    request.length = 2;
    CHECK(ipc::valid(request));
    request.length = static_cast<std::uint32_t>(max_text_units + 1);
    CHECK_FALSE(ipc::valid(request));
}

TEST_CASE("Helper streams are isolated bounded and revoked without injecting input", "[input][input-ipc]") {
    ipc::Session session;
    CHECK(ipc::valid(session.dispatch({})));
    ipc::Request create;
    create.operation = ipc::Operation::Create;
    create.stream = 1;
    CHECK(ipc::valid(session.dispatch(create)));
    CHECK_THROWS(session.dispatch(create));
    for (std::uint64_t id = 2; id <= ipc::max_streams; ++id) {
        create.stream = id;
        CHECK_NOTHROW(session.dispatch(create));
    }
    create.stream = ipc::max_streams + 1;
    CHECK_THROWS(session.dispatch(create));
    ipc::Request destroy;
    destroy.operation = ipc::Operation::Destroy;
    destroy.stream = 1;
    CHECK_NOTHROW(session.dispatch(destroy));
    CHECK_THROWS(session.dispatch(destroy));
    CHECK_NOTHROW(session.dispatch(create)); // A freed slot can be reused.
    ipc::Request state;
    state.operation = ipc::Operation::Modifiers;
    state.stream = 2;
    CHECK(ipc::valid(session.dispatch(state)));
    state.operation = ipc::Operation::Cursor;
    state.width = 1920;
    state.height = 1080;
    CHECK(ipc::valid(session.dispatch(state)));
    state.stream = 1; // Removed stream cannot query state or inject.
    CHECK_THROWS(session.dispatch(state));
    CHECK_NOTHROW(session.cancel());
    CHECK_NOTHROW(session.cancel());
    CHECK_THROWS(session.dispatch({}));
    CHECK_THROWS(session.dispatch(create));

    ipc::Session graceful;
    ipc::Request shutdown;
    shutdown.operation = ipc::Operation::Shutdown;
    CHECK_NOTHROW(graceful.dispatch(shutdown));
    CHECK_THROWS(graceful.dispatch({}));
}

TEST_CASE("Non elevated mode keeps the existing local input path", "[input][input-ipc]") {
    beamit::host::HostServerOptions options;
    CHECK_FALSE(options.elevate_input);
    CHECK_FALSE(options.elevated_input);
    StreamInput stream(nullptr);
    CHECK(stream.mouse_mode == MouseMode::Absolute);
    CHECK_NOTHROW(stream.modifiers());
    CHECK_NOTHROW(stream.cursor_position(1920, 1080));
    CHECK_NOTHROW(stream.release_all()); // No held input: no SendInput call.
    stream.cancel();
    CHECK(stream.wheel(0, 0) == InputResult::InjectFailed);
    CHECK(stream.move_absolute(0, 0, 1920, 1080) == InputResult::InjectFailed);
    wchar_t name[] = L"BeamItInputHelper.exe";
    wchar_t* argv[]{name};
    CHECK(run_input_helper(1, argv) == 1); // No public unbound/non-elevated helper mode.
}
