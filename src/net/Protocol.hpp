#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace beamit::net::protocol {

inline constexpr int version = 1;
inline constexpr std::uint32_t pbkdf2_iterations = 600'000;
inline constexpr std::size_t nonce_bytes = 32;
inline constexpr std::size_t max_signaling_message = 64 * 1024;
inline constexpr std::size_t max_streams_per_session = 8;
// Largest wheel movement accepted in one mouse_wheel message (120 units = one notch).
inline constexpr std::int64_t max_wheel_units = 120 * 100;
inline constexpr std::chrono::seconds auth_timeout{10};
inline constexpr std::chrono::seconds token_lifetime{10};
inline constexpr unsigned short close_superseded = 4001;

inline constexpr std::uint8_t frame_version = 1;
inline constexpr std::uint8_t message_video = 1;
inline constexpr std::uint32_t flag_key_frame = 1U << 0;
inline constexpr std::uint32_t flag_discontinuity = 1U << 1;
inline constexpr std::size_t frame_header_size = 32;

struct FrameHeader {
    std::uint32_t flags{};
    std::uint32_t generation{};
    std::uint64_t sequence{};
    std::uint64_t capture_time_us{};
};

// Encodes PROTOCOL.md §7 in network byte order.
[[nodiscard]] constexpr std::array<std::byte, frame_header_size> encode_frame_header(const FrameHeader& header) noexcept {
    std::array<std::byte, frame_header_size> bytes{};
    const auto put = [&](std::size_t offset, std::uint64_t value, std::size_t width) {
        for (std::size_t index = 0; index < width; ++index) {
            bytes[offset + index] = static_cast<std::byte>((value >> (8 * (width - 1 - index))) & 0xFF);
        }
    };
    put(0, frame_version, 1);
    put(1, message_video, 1);
    put(2, frame_header_size, 2);
    put(4, header.flags, 4);
    put(8, header.generation, 4);
    put(12, 0, 4);
    put(16, header.sequence, 8);
    put(24, header.capture_time_us, 8);
    return bytes;
}

// Host clock shared by pong.host_time_us and frame capture_time_us.
[[nodiscard]] inline std::uint64_t host_time_us(std::chrono::steady_clock::time_point time) noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(time.time_since_epoch()).count());
}

} // namespace beamit::net::protocol
