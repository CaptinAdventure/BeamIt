#include "ClientProtocol.hpp"

#include "net/Protocol.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <limits>

namespace beamit::client {
namespace {

constexpr std::uint32_t max_pbkdf2_iterations = 10'000'000;
constexpr std::size_t max_salt_bytes = 1024;
constexpr std::uint32_t max_dimension = 16384;

bool equals_ignore_case(std::string_view left, std::string_view right) {
    return std::ranges::equal(left, right, [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
}

struct Authority {
    std::string host;
    std::uint16_t port{443};
};

std::expected<Authority, std::string> parse_authority(std::string_view text) {
    if (text.empty()) return std::unexpected("The server address has no host.");
    if (text.find('@') != std::string_view::npos) return std::unexpected("User names in server addresses are not supported.");
    Authority result;
    std::string_view port_text;
    if (text.front() == '[') {
        const auto close = text.find(']');
        if (close == std::string_view::npos) return std::unexpected("Unterminated IPv6 address.");
        result.host = text.substr(1, close - 1);
        const auto rest = text.substr(close + 1);
        if (!rest.empty()) {
            if (rest.front() != ':') return std::unexpected("Invalid text after the IPv6 address.");
            port_text = rest.substr(1);
        }
        if (result.host.empty() || result.host.find_first_not_of("0123456789abcdefABCDEF:.") != std::string::npos) {
            return std::unexpected("Invalid IPv6 address.");
        }
    } else {
        const auto colon = text.find(':');
        if (colon != std::string_view::npos) {
            if (text.find(':', colon + 1) != std::string_view::npos) {
                return std::unexpected("IPv6 addresses must be enclosed in brackets.");
            }
            port_text = text.substr(colon + 1);
        }
        result.host = text.substr(0, colon);
        if (result.host.empty() || std::ranges::any_of(result.host, [](char character) {
                return !std::isalnum(static_cast<unsigned char>(character)) && character != '-' && character != '.' &&
                       character != '_';
            })) {
            return std::unexpected("Invalid host name.");
        }
    }
    if (!port_text.empty() || (text.back() == ':')) {
        unsigned value{};
        const auto [end, error] = std::from_chars(port_text.data(), port_text.data() + port_text.size(), value);
        if (error != std::errc{} || end != port_text.data() + port_text.size() || value == 0 || value > 65535) {
            return std::unexpected("Invalid port number.");
        }
        result.port = static_cast<std::uint16_t>(value);
    }
    return result;
}

std::string make_authority(const std::string& host, std::uint16_t port) {
    return (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" + std::to_string(port);
}

std::optional<std::vector<std::byte>> decode_field(const json::object& object, std::string_view key) {
    const auto text = string_field(object, key);
    if (!text) return std::nullopt;
    return net::base64url_decode(*text);
}

template<class T>
std::optional<T> bounded(const json::object& object, std::string_view key, std::int64_t minimum, std::int64_t maximum) {
    const auto value = integer_field(object, key);
    if (!value || *value < minimum || *value > maximum) return std::nullopt;
    return static_cast<T>(*value);
}

bool valid_identifier(std::string_view text) {
    return text.size() >= 8 && text.size() <= 128 && std::ranges::all_of(text, [](char character) {
        return std::isalnum(static_cast<unsigned char>(character)) || character == '-' || character == '_';
    });
}

} // namespace

bool Endpoint::same_origin(std::string_view other_authority) const {
    const auto other = parse_authority(other_authority);
    return other && other->port == port && equals_ignore_case(other->host, host);
}

std::expected<Endpoint, std::string> parse_server_url(std::string_view url) {
    while (!url.empty() && std::isspace(static_cast<unsigned char>(url.front()))) url.remove_prefix(1);
    while (!url.empty() && std::isspace(static_cast<unsigned char>(url.back()))) url.remove_suffix(1);
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) return std::unexpected("Enter a wss:// or https:// server URL.");
    const auto scheme = url.substr(0, scheme_end);
    if (!equals_ignore_case(scheme, "wss") && !equals_ignore_case(scheme, "https")) {
        return std::unexpected("Only wss:// and https:// server URLs are supported.");
    }
    auto rest = url.substr(scheme_end + 3);
    rest = rest.substr(0, rest.find_first_of("/?#"));
    auto authority = parse_authority(rest);
    if (!authority) return std::unexpected(authority.error());
    return Endpoint{authority->host, authority->port, make_authority(authority->host, authority->port)};
}

std::optional<net::Sha256> parse_fingerprint(std::string_view text) {
    std::string digits;
    for (const char character : text) {
        if (character == ':' || std::isspace(static_cast<unsigned char>(character))) continue;
        if (!std::isxdigit(static_cast<unsigned char>(character))) return std::nullopt;
        digits.push_back(character);
    }
    if (digits.size() != 64) return std::nullopt;
    net::Sha256 result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        unsigned value{};
        std::from_chars(digits.data() + index * 2, digits.data() + index * 2 + 2, value, 16);
        result[index] = static_cast<std::byte>(value);
    }
    return result;
}

std::string format_fingerprint(const net::Sha256& fingerprint) {
    std::string text;
    for (const auto byte : fingerprint) {
        if (!text.empty()) text += ':';
        text += std::format("{:02X}", std::to_integer<unsigned>(byte));
    }
    return text;
}

std::optional<std::int64_t> integer_field(const json::object& object, std::string_view key) {
    const auto* value = object.if_contains(key);
    if (!value) return std::nullopt;
    if (const auto* signed_value = value->if_int64()) return *signed_value;
    if (const auto* unsigned_value = value->if_uint64();
        unsigned_value && *unsigned_value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return static_cast<std::int64_t>(*unsigned_value);
    }
    return std::nullopt;
}

std::optional<std::string_view> string_field(const json::object& object, std::string_view key) {
    const auto* value = object.if_contains(key);
    if (!value || !value->is_string()) return std::nullopt;
    return std::string_view(value->get_string());
}

std::expected<AuthChallenge, std::string> parse_hello(const json::object& hello) {
    const auto* auth = hello.if_contains("auth");
    if (string_field(hello, "type") != "hello" || !auth || !auth->is_object()) {
        return std::unexpected("The host did not send a BeamIt authentication challenge.");
    }
    if (integer_field(hello, "protocol") != net::protocol::version) {
        return std::unexpected("The host uses an unsupported BeamIt protocol version.");
    }
    const auto& fields = auth->get_object();
    const auto iterations = integer_field(fields, "iterations");
    auto salt = decode_field(fields, "salt");
    auto nonce = decode_field(fields, "server_nonce");
    if (string_field(fields, "mechanism") != "pbkdf2-sha256-hmac" || !iterations ||
        *iterations < static_cast<std::int64_t>(net::protocol::pbkdf2_iterations) || *iterations > max_pbkdf2_iterations ||
        !salt || salt->empty() || salt->size() > max_salt_bytes || !nonce || nonce->size() != net::protocol::nonce_bytes) {
        return std::unexpected("The host sent an invalid or weakened authentication challenge.");
    }
    return AuthChallenge{std::move(*salt), std::move(*nonce), static_cast<std::uint32_t>(*iterations)};
}

AuthExchange create_authentication(const AuthChallenge& challenge, std::string_view password) {
    auto client_nonce = net::random_bytes(net::protocol::nonce_bytes);
    auto key = net::pbkdf2_sha256(password, challenge.salt, challenge.iterations);
    const auto client_proof = net::auth_proof(key, "client", challenge.server_nonce, client_nonce);
    AuthExchange result;
    result.expected_server_proof = net::auth_proof(key, "server", challenge.server_nonce, client_nonce);
    SecureZeroMemory(key.data(), key.size());
    result.message = {
        {"type", "authenticate"},
        {"protocol", net::protocol::version},
        {"client_nonce", net::base64url_encode(client_nonce)},
        {"proof", net::base64url_encode(client_proof)},
    };
    return result;
}

std::expected<VideoParameters, std::string> parse_video(const json::value& value) {
    const auto* object = value.if_object();
    if (!object) return std::unexpected("Video parameters are missing.");
    VideoParameters video;
    const auto generation = bounded<std::uint32_t>(*object, "generation", 1, std::numeric_limits<std::uint32_t>::max());
    const auto width = bounded<std::uint32_t>(*object, "width", 1, max_dimension);
    const auto height = bounded<std::uint32_t>(*object, "height", 1, max_dimension);
    const auto codec = string_field(*object, "codec");
    const auto format = string_field(*object, "format");
    if (!generation || !width || !height || !codec || (*codec != "h264" && *codec != "h265") ||
        (format && *format != "annexb")) {
        return std::unexpected("The host sent invalid video parameters.");
    }
    video.generation = *generation;
    video.width = *width;
    video.height = *height;
    video.codec = *codec;
    video.codec_string = string_field(*object, "codec_string").value_or("");
    video.chroma_format = string_field(*object, "chroma_format").value_or("");
    video.rate_control = string_field(*object, "rate_control").value_or("");
    video.encoder = string_field(*object, "encoder").value_or("");
    video.bit_depth = bounded<std::uint32_t>(*object, "bit_depth", 8, 16).value_or(8);
    if (object->contains("frame_rate")) {
        video.frame_rate = bounded<std::uint32_t>(*object, "frame_rate", 1, 240);
        if (!video.frame_rate) return std::unexpected("The host sent an invalid frame rate.");
    }
    if (object->contains("bitrate_kbps")) {
        video.bitrate_kbps = bounded<std::uint32_t>(*object, "bitrate_kbps", 1, 1'000'000);
        if (!video.bitrate_kbps) return std::unexpected("The host sent an invalid bitrate.");
    }
    if (object->contains("max_bitrate_kbps")) {
        video.max_bitrate_kbps = bounded<std::uint32_t>(*object, "max_bitrate_kbps", 1, 4'000'000);
        if (!video.max_bitrate_kbps) return std::unexpected("The host sent an invalid peak bitrate.");
    }
    return video;
}

std::expected<StreamOffer, std::string> parse_stream_offer(const json::object& offer, const Endpoint& control) {
    const auto stream_id = string_field(offer, "stream_id");
    const auto url = string_field(offer, "url");
    const auto lifetime = integer_field(offer, "token_expires_in_ms");
    const auto* source = offer.if_contains("source");
    const auto* video = offer.if_contains("video");
    if (string_field(offer, "type") != "stream_offer" || !stream_id || !valid_identifier(*stream_id) || !url ||
        !lifetime || *lifetime <= 0 || !source || !source->is_object() || !video) {
        return std::unexpected("The host sent an invalid stream offer.");
    }
    constexpr std::string_view scheme = "wss://";
    if (url->size() <= scheme.size() || !equals_ignore_case(url->substr(0, scheme.size()), scheme)) {
        return std::unexpected("The stream URL is not a secure WebSocket URL.");
    }
    const auto rest = url->substr(scheme.size());
    const auto path_start = rest.find('/');
    if (path_start == std::string_view::npos || !control.same_origin(rest.substr(0, path_start))) {
        return std::unexpected("The stream URL does not match the connected host.");
    }
    const auto target = rest.substr(path_start);
    const auto expected_path = "/streams/" + std::string(*stream_id);
    if (!target.starts_with(expected_path) || target.size() <= expected_path.size() ||
        target[expected_path.size()] != '?' || target.find('#') != std::string_view::npos) {
        return std::unexpected("The stream URL has an unexpected path.");
    }
    auto parameters = parse_video(*video);
    if (!parameters) return std::unexpected(parameters.error());

    StreamOffer result;
    result.stream_id = *stream_id;
    result.target = target;
    result.token_lifetime = std::chrono::milliseconds(*lifetime);
    result.video = std::move(*parameters);
    const auto& source_object = source->get_object();
    result.source.kind = string_field(source_object, "kind").value_or("");
    result.source.hwnd = string_field(source_object, "hwnd").value_or("");
    result.source.pid = integer_field(source_object, "pid");
    result.source.title = string_field(source_object, "title").value_or("");
    result.source.capture = string_field(source_object, "capture").value_or("");
    if (const auto* dpi = source_object.if_contains("dpi"); dpi && dpi->is_object()) {
        result.source.dpi_x = bounded<std::uint32_t>(dpi->get_object(), "x", 1, 10'000);
        result.source.dpi_y = bounded<std::uint32_t>(dpi->get_object(), "y", 1, 10'000);
    }
    result.mouse_mode = "absolute";
    result.cursor = "video";
    if (const auto* input = offer.if_contains("input"); input && input->is_object()) {
        if (const auto mode = string_field(input->get_object(), "mouse_mode")) result.mouse_mode = *mode;
        if (string_field(input->get_object(), "cursor") == "client") result.cursor = "client";
    }
    return result;
}

void ClockEstimate::record(std::uint64_t host_us, std::chrono::steady_clock::time_point sent,
                           std::chrono::steady_clock::time_point received) {
    if (received < sent) return;
    using std::chrono::duration;
    const auto sent_us = duration<double, std::micro>(sent.time_since_epoch()).count();
    const auto received_us = duration<double, std::micro>(received.time_since_epoch()).count();
    std::erase_if(samples_, [&](const Sample& sample) { return received - sample.received > std::chrono::seconds(30); });
    samples_.push_back({static_cast<double>(host_us) - (sent_us + received_us) / 2, received_us - sent_us, received});
}

std::optional<ClockEstimate::Sample> ClockEstimate::best(std::chrono::steady_clock::time_point now) const {
    std::optional<Sample> result;
    for (const auto& sample : samples_) {
        if (now < sample.received || now - sample.received > std::chrono::seconds(30)) continue;
        if (!result || sample.rtt_us < result->rtt_us) result = sample;
    }
    return result;
}

} // namespace beamit::client
