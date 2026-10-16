#pragma once

#include "beamit/media/Encoder.hpp"

#include <memory>

namespace beamit::media {

class NvencVideoEncoder final : public Encoder {
public:
    NvencVideoEncoder();
    ~NvencVideoEncoder() override;
    NvencVideoEncoder(const NvencVideoEncoder&) = delete;
    NvencVideoEncoder& operator=(const NvencVideoEncoder&) = delete;

    [[nodiscard]] std::wstring_view backend_name() const noexcept override;
    bool configure(const EncoderOptions& options) override;
    bool start() override;
    // Initialize against the capture device without submitting a picture.
    bool prepare(const CapturedFrame& frame);
    EncoderSubmitResult try_submit(const CapturedFrame& frame) override;
    EncoderSubmitResult try_repeat_frame() override;
    void request_key_frame() noexcept override;
    [[nodiscard]] std::optional<EncodedPacket> try_receive() override;
    void stop() noexcept override;
    [[nodiscard]] bool is_running() const noexcept override;
    [[nodiscard]] HRESULT last_error() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace beamit::media
