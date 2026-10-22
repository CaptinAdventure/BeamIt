#pragma once

#include "beamit/media/VideoFrame.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>

namespace beamit::media {

struct EncoderOptions {
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t frame_rate{0};
    std::uint32_t target_bitrate_kbps{0};
    VideoCodec codec{VideoCodec::H264};
    // VBR peak; 0 selects default_peak_factor x target. Values below the target are raised to it.
    std::uint32_t max_bitrate_kbps{0};
};

inline constexpr std::uint32_t default_peak_factor = 2;
inline constexpr std::uint32_t max_bitrate_limit_kbps = 4'000'000;
inline constexpr std::uint32_t key_frame_interval_seconds = 10;

[[nodiscard]] constexpr std::uint32_t key_frame_interval(std::uint32_t frame_rate) noexcept {
    return frame_rate * key_frame_interval_seconds;
}

[[nodiscard]] constexpr std::uint32_t peak_bitrate_kbps(std::uint32_t target_kbps, std::uint32_t max_kbps) noexcept {
    const auto peak = max_kbps ? std::uint64_t{max_kbps} : std::uint64_t{target_kbps} * default_peak_factor;
    return static_cast<std::uint32_t>(std::max<std::uint64_t>(target_kbps, std::min<std::uint64_t>(peak, max_bitrate_limit_kbps)));
}

[[nodiscard]] constexpr std::uint32_t peak_bitrate_kbps(const EncoderOptions& options) noexcept {
    return peak_bitrate_kbps(options.target_bitrate_kbps, options.max_bitrate_kbps);
}

// VBR: average target with a peak and a one-frame (at the target) VBV. CBR is used only when a driver lacks VBR.
enum class RateControlMode {
    Vbr,
    Cbr,
};

[[nodiscard]] constexpr const char* rate_control_name(RateControlMode mode) noexcept {
    return mode == RateControlMode::Cbr ? "cbr" : "vbr";
}

enum class EncoderSubmitResult {
    Accepted,
    WouldBlock,
    Failed,
};

class Encoder {
public:
    virtual ~Encoder() = default;

    [[nodiscard]] virtual std::wstring_view backend_name() const noexcept = 0;
    virtual bool configure(const EncoderOptions& options) = 0;
    virtual bool start() = 0;
    // WouldBlock leaves ownership with the caller; drain output and retry. Failed requires last_error().
    virtual EncoderSubmitResult try_submit(const CapturedFrame& frame) = 0;
    // Encode the last accepted image again as a new picture, without retaining its capture surface.
    virtual EncoderSubmitResult try_repeat_frame() = 0;
    // Makes the next submitted picture an IDR with parameter sets; call from the encoding thread.
    virtual void request_key_frame() noexcept = 0;
    // Changes the target and peak (EncoderOptions::max_bitrate_kbps semantics) bitrates of the current sequence in
    // place: no new parameter sets, no IDR. Returns false, leaving the encoder usable, when the backend cannot; the
    // caller then reconfigures. Call from the encoding thread after the previous picture was received.
    virtual bool try_update_bitrate(std::uint32_t bitrate_kbps, std::uint32_t max_bitrate_kbps) = 0;
    // The rate control in use; meaningful once the first picture was accepted.
    [[nodiscard]] virtual RateControlMode rate_control() const noexcept = 0;
    // Returns no value while output is pending; check last_error() to distinguish failures.
    [[nodiscard]] virtual std::optional<EncodedPacket> try_receive() = 0;
    virtual void stop() noexcept = 0;
    [[nodiscard]] virtual bool is_running() const noexcept = 0;
    [[nodiscard]] virtual HRESULT last_error() const noexcept = 0;
};

} // namespace beamit::media
