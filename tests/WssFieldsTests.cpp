#include "host/wss/Fields.hpp"

#include <catch2/catch_test_macros.hpp>

#include <limits>

using namespace beamit::host::wss;

namespace {

json::object parse(std::string_view text) { return json::parse(text).as_object(); }

template <typename Function>
std::string error_code(Function&& function) {
    try {
        function();
    } catch (const ProtocolError& error) {
        return error.code;
    }
    return {};
}

} // namespace

TEST_CASE("Window handles round-trip through hex strings", "[wss]") {
    const auto window = reinterpret_cast<HWND>(std::uintptr_t{0x1A2B});
    CHECK(hwnd_text(window) == "0x1A2B");
    CHECK(parse_hwnd("0x1A2B") == window);
    CHECK(parse_hwnd("0X1a2b") == window);
    CHECK(parse_hwnd("6699") == window);
    CHECK_FALSE(parse_hwnd("0x0"));
    CHECK_FALSE(parse_hwnd(""));
    CHECK_FALSE(parse_hwnd("0x"));
    CHECK_FALSE(parse_hwnd("0x12zz"));
    CHECK_FALSE(parse_hwnd("-5"));
}

TEST_CASE("Request fields validate type and range", "[wss]") {
    const auto message = parse(R"({"s":"text","b":true,"i":42,"big":18446744073709551615,"d":1.5,
                                   "huge":2e6,"o":{},"h":"0x10","bad":"zz"})");

    CHECK(required_string(message, "s") == "text");
    CHECK(error_code([&] { (void)required_string(message, "i"); }) == "invalid_request");
    CHECK(error_code([&] { (void)required_string(message, "missing"); }) == "invalid_request");

    CHECK(required_bool(message, "b"));
    CHECK(error_code([&] { (void)required_bool(message, "s"); }) == "invalid_request");

    CHECK(optional_integer(message, "i", 0, 100) == 42);
    CHECK_FALSE(optional_integer(message, "missing", 0, 100));
    CHECK(error_code([&] { (void)optional_integer(message, "i", 0, 41); }) == "invalid_request");
    CHECK(error_code([&] { (void)optional_integer(message, "d", 0, 100); }) == "invalid_request");
    CHECK(error_code([&] {
        (void)optional_integer(message, "big", 0, std::numeric_limits<std::int64_t>::max());
    }) == "invalid_request");
    CHECK(required_integer(message, "i", 42, 42) == 42);
    CHECK(error_code([&] { (void)required_integer(message, "missing", 0, 1); }) == "invalid_request");

    CHECK(optional_number(message, "d") == 1.5);
    CHECK(optional_number(message, "i") == 42.0);
    CHECK_FALSE(optional_number(message, "missing"));
    CHECK(error_code([&] { (void)optional_number(message, "big"); }) == "invalid_request");
    CHECK(error_code([&] { (void)optional_number(message, "huge"); }) == "invalid_request");
    CHECK(error_code([&] { (void)optional_number(message, "s"); }) == "invalid_request");

    CHECK(optional_object(message, "o") != nullptr);
    CHECK(optional_object(message, "missing") == nullptr);
    CHECK(error_code([&] { (void)optional_object(message, "s"); }) == "invalid_request");

    CHECK(required_hwnd(message, "h") == reinterpret_cast<HWND>(std::uintptr_t{0x10}));
    CHECK(error_code([&] { (void)required_hwnd(message, "bad"); }) == "invalid_request");
    CHECK(error_code([&] { (void)required_hwnd(message, "i"); }) == "invalid_request");
}
