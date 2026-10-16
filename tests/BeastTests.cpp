#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string_view>

TEST_CASE("Boost Beast parses an HTTP WebSocket upgrade request", "[dependencies][beast]") {
    namespace http = boost::beast::http;
    http::request_parser<http::empty_body> parser;
    boost::system::error_code error;
    constexpr std::string_view request =
        "GET /stream HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    parser.put(boost::asio::buffer(request.data(), request.size()), error);
    REQUIRE_FALSE(error);
    REQUIRE(parser.is_done());
    REQUIRE(parser.get().target() == "/stream");
    REQUIRE(boost::beast::websocket::is_upgrade(parser.get()));
}
