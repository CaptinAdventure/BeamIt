#pragma once

#include "beamit/media/Encoder.hpp"

#include <memory>

namespace beamit::media {

class D3D12VideoEncoder final : public Encoder {
public:
    D3D12VideoEncoder();
    ~D3D12VideoEncoder() override;

    D3D12VideoEncoder(const D3D12VideoEncoder&) = delete;
    D3D12VideoEncoder& operator=(const D3D12VideoEncoder&) = delete;

    [[nodiscard]] std::wstring_view backend_name() const noexcept override;
    bool configure(const EncoderOptions& options) override;
    bool start() override;
    EncoderSubmitResult try_submit(const CapturedFrame& frame) override;
    EncoderSubmitResult try_repeat_frame() override;
    void request_key_frame() noexcept override;
    bool try_update_bitrate(std::uint32_t bitrate_kbps, std::uint32_t max_bitrate_kbps) override;
    [[nodiscard]] RateControlMode rate_control() const noexcept override;
    [[nodiscard]] std::optional<EncodedPacket> try_receive() override;
    void stop() noexcept override;
    [[nodiscard]] bool is_running() const noexcept override;
    [[nodiscard]] HRESULT last_error() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace beamit::media
