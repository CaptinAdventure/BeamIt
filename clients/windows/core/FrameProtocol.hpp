#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace beamit::client {

struct VideoFrame {
    std::uint32_t generation{};
    std::uint64_t sequence{};
    std::uint64_t capture_time_us{};
    bool key_frame{};
    bool discontinuity{};
    std::span<const std::byte> payload;
};

enum class FrameKind { Video, Unknown };

struct ParsedFrame {
    FrameKind kind{FrameKind::Unknown};
    VideoFrame video;
};

// The caller owns the message for the entire lifetime of the returned payload.
[[nodiscard]] ParsedFrame parse_frame(std::span<const std::byte> message);

enum class FrameAction { Decode, Ignore, RequestKeyFrame, AwaitConfiguration };

class FrameGate {
public:
    explicit FrameGate(std::uint32_t generation);
    void reconfigure(std::uint32_t generation);
    [[nodiscard]] FrameAction accept(const VideoFrame& frame);
    void reset();

private:
    std::uint32_t generation_;
    std::uint64_t last_sequence_{};
    bool has_sequence_{};
    bool waiting_for_key_frame_{true};
    bool key_frame_requested_{};
};

} // namespace beamit::client
