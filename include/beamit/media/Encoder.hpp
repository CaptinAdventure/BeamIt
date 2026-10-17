#pragma once

#include "beamit/media/VideoFrame.hpp"

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
};

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
    // Changes the target bitrate of the current sequence in place: no new parameter sets, no IDR. Returns false,
    // leaving the encoder usable, when the backend cannot; the caller then reconfigures. Call from the encoding
    // thread after the previous picture was received.
    virtual bool try_update_bitrate(std::uint32_t bitrate_kbps) = 0;
    // Returns no value while output is pending; check last_error() to distinguish failures.
    [[nodiscard]] virtual std::optional<EncodedPacket> try_receive() = 0;
    virtual void stop() noexcept = 0;
    [[nodiscard]] virtual bool is_running() const noexcept = 0;
    [[nodiscard]] virtual HRESULT last_error() const noexcept = 0;
};

} // namespace beamit::media
