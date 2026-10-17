#pragma once

#include "host/wss/WebSocket.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace beamit::host::wss {

// The optional `id` of a client request, echoed in its reply (PROTOCOL.md §3).
using RequestId = std::optional<json::value>;

// A request error reported as {"type":"error","code":...}; the socket stays open.
struct ProtocolError {
    std::string code;
    std::string message;
};

// Window handles travel as "0x..." strings.
[[nodiscard]] std::string hwnd_text(HWND window);
[[nodiscard]] std::optional<HWND> parse_hwnd(std::string_view text);
[[nodiscard]] std::string window_title(HWND window);

// Field accessors for request objects. Missing optional fields yield nullptr/nullopt; present fields of the wrong
// type or range, and missing required fields, throw ProtocolError{"invalid_request"}.
[[nodiscard]] const json::object* optional_object(const json::object& message, std::string_view name);
[[nodiscard]] std::string_view required_string(const json::object& message, std::string_view name);
[[nodiscard]] bool required_bool(const json::object& message, std::string_view name);
[[nodiscard]] std::optional<std::int64_t> optional_integer(const json::object& message, std::string_view name,
                                                           std::int64_t minimum, std::int64_t maximum);
[[nodiscard]] std::int64_t required_integer(const json::object& message, std::string_view name, std::int64_t minimum,
                                            std::int64_t maximum);
// A finite number within +/-1e6.
[[nodiscard]] std::optional<double> optional_number(const json::object& message, std::string_view name);
[[nodiscard]] HWND required_hwnd(const json::object& message, std::string_view name);
// A base64url string field, or nullopt when missing or not decodable.
[[nodiscard]] std::optional<std::vector<std::byte>> base64url_field(const json::object& message,
                                                                    std::string_view name);

} // namespace beamit::host::wss
