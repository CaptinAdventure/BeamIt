#pragma once

#include "net/Crypto.hpp"

#include <boost/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace beamit::client {

namespace json = boost::json;

// Host endpoint parsed from a wss:// or https:// server URL (PROTOCOL.md §1).
struct Endpoint {
    std::string host;      // Without IPv6 brackets.
    std::uint16_t port{};
    std::string authority; // host[:port] as sent in the Host header; IPv6 literals are bracketed.

    [[nodiscard]] bool same_origin(std::string_view other_authority) const;
};

[[nodiscard]] std::expected<Endpoint, std::string> parse_server_url(std::string_view url);

// Accepts 64 hex digits with optional ':' or whitespace separators.
[[nodiscard]] std::optional<net::Sha256> parse_fingerprint(std::string_view text);
[[nodiscard]] std::string format_fingerprint(const net::Sha256& fingerprint);

struct AuthChallenge {
    std::vector<std::byte> salt;
    std::vector<std::byte> server_nonce;
    std::uint32_t iterations{};
};

[[nodiscard]] std::expected<AuthChallenge, std::string> parse_hello(const json::object& hello);

struct AuthExchange {
    json::object message;
    net::Sha256 expected_server_proof{};
};

// Derives the key, builds `authenticate` (PROTOCOL.md §4.2) and clears the derived key.
[[nodiscard]] AuthExchange create_authentication(const AuthChallenge& challenge, std::string_view password);

[[nodiscard]] std::optional<std::int64_t> integer_field(const json::object& object, std::string_view key);
[[nodiscard]] std::optional<std::string_view> string_field(const json::object& object, std::string_view key);

struct VideoParameters {
    std::uint32_t generation{};
    std::string codec;
    std::string codec_string;
    std::uint32_t width{};
    std::uint32_t height{};
    std::string chroma_format;
    std::uint32_t bit_depth{};
    std::optional<std::uint32_t> frame_rate;
    std::optional<std::uint32_t> bitrate_kbps;
    std::optional<std::uint32_t> max_bitrate_kbps;
    std::string rate_control;
    std::string encoder;
};

[[nodiscard]] std::expected<VideoParameters, std::string> parse_video(const json::value& value);

struct StreamSource {
    std::string kind;
    std::string hwnd;
    std::optional<std::int64_t> pid;
    std::string title;
    std::string capture;
    std::optional<std::uint32_t> dpi_x;
    std::optional<std::uint32_t> dpi_y;
};

struct StreamOffer {
    std::string stream_id;
    std::string target; // Path and query of the stream socket; contains the single-use token.
    std::chrono::milliseconds token_lifetime{};
    StreamSource source;
    VideoParameters video;
    std::string mouse_mode;
    // "client" when the host sends `cursor` shapes instead of drawing the pointer into the video (§5.9.11).
    std::string cursor;
};

// Rejects offers whose URL is not wss:// on the control endpoint's authority (PROTOCOL.md §5.4).
[[nodiscard]] std::expected<StreamOffer, std::string> parse_stream_offer(const json::object& offer,
                                                                         const Endpoint& control);

// RTT-filtered host clock offset from ping/pong samples (PROTOCOL.md §5.15).
class ClockEstimate {
public:
    struct Sample {
        double offset_us{};
        double rtt_us{};
        std::chrono::steady_clock::time_point received;
    };

    void record(std::uint64_t host_us, std::chrono::steady_clock::time_point sent,
                std::chrono::steady_clock::time_point received);
    [[nodiscard]] std::optional<Sample> best(std::chrono::steady_clock::time_point now) const;

private:
    std::vector<Sample> samples_;
};

} // namespace beamit::client
