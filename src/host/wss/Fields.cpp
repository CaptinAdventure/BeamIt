#include "host/wss/Fields.hpp"

#include "Utf8.hpp"
#include "net/Crypto.hpp"

#include <spdlog/fmt/fmt.h>

#include <charconv>
#include <cmath>

namespace beamit::host::wss {
namespace {

ProtocolError invalid(std::string message) { return {"invalid_request", std::move(message)}; }

ProtocolError range_error(std::string_view name, std::int64_t minimum, std::int64_t maximum) {
    return invalid(fmt::format("{} must be an integer from {} to {}", name, minimum, maximum));
}

} // namespace

std::string hwnd_text(HWND window) { return fmt::format("0x{:X}", reinterpret_cast<std::uintptr_t>(window)); }

std::optional<HWND> parse_hwnd(std::string_view text) {
    int base = 10;
    if (text.starts_with("0x") || text.starts_with("0X")) {
        base = 16;
        text.remove_prefix(2);
    }
    std::uintptr_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (error != std::errc{} || end != text.data() + text.size() || value == 0) return std::nullopt;
    return reinterpret_cast<HWND>(value);
}

std::string window_title(HWND window) {
    std::wstring title(static_cast<std::size_t>(GetWindowTextLengthW(window)) + 1, L'\0');
    title.resize(static_cast<std::size_t>(GetWindowTextW(window, title.data(), static_cast<int>(title.size()))));
    return to_utf8(title);
}

const json::object* optional_object(const json::object& message, std::string_view name) {
    const auto* field = message.if_contains(name);
    if (!field) return nullptr;
    if (!field->is_object()) throw invalid(fmt::format("{} must be an object", name));
    return &field->as_object();
}

std::string_view required_string(const json::object& message, std::string_view name) {
    const auto* field = message.if_contains(name);
    if (!field || !field->is_string()) throw invalid(fmt::format("{} must be a string", name));
    return field->as_string();
}

bool required_bool(const json::object& message, std::string_view name) {
    const auto* field = message.if_contains(name);
    if (!field || !field->is_bool()) throw invalid(fmt::format("{} must be a boolean", name));
    return field->as_bool();
}

std::optional<std::int64_t> optional_integer(const json::object& message, std::string_view name,
                                             std::int64_t minimum, std::int64_t maximum) {
    const auto* field = message.if_contains(name);
    if (!field) return std::nullopt;
    // Boost.JSON stores every integer that fits as int64; uint64 values exceed any accepted range.
    if (!field->is_int64() || field->as_int64() < minimum || field->as_int64() > maximum) {
        throw range_error(name, minimum, maximum);
    }
    return field->as_int64();
}

std::int64_t required_integer(const json::object& message, std::string_view name, std::int64_t minimum,
                              std::int64_t maximum) {
    const auto value = optional_integer(message, name, minimum, maximum);
    if (!value) throw range_error(name, minimum, maximum);
    return *value;
}

std::optional<double> optional_number(const json::object& message, std::string_view name) {
    const auto* field = message.if_contains(name);
    if (!field) return std::nullopt;
    double value{};
    if (field->is_double()) {
        value = field->as_double();
    } else if (field->is_int64()) {
        value = static_cast<double>(field->as_int64());
    } else if (field->is_uint64()) {
        value = static_cast<double>(field->as_uint64());
    } else {
        throw invalid(fmt::format("{} must be a number", name));
    }
    if (!std::isfinite(value) || std::abs(value) > 1e6) throw invalid(fmt::format("{} is out of range", name));
    return value;
}

HWND required_hwnd(const json::object& message, std::string_view name) {
    const auto* field = message.if_contains(name);
    const auto window = field && field->is_string() ? parse_hwnd(field->as_string()) : std::nullopt;
    if (!window) throw invalid(fmt::format("{} must be a window handle string", name));
    return *window;
}

std::optional<std::vector<std::byte>> base64url_field(const json::object& message, std::string_view name) {
    const auto* field = message.if_contains(name);
    if (!field || !field->is_string()) return std::nullopt;
    return beamit::net::base64url_decode(field->as_string());
}

} // namespace beamit::host::wss
