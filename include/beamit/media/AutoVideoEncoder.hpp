#pragma once

#include "beamit/media/Encoder.hpp"

#include <memory>

namespace beamit::media {

class AutoVideoEncoder final : public Encoder {
public:
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
    // Selects NVENC 4:4:4 when available; false only for a non-capability failure (error_ set).
    bool try_select_nvenc(const CapturedFrame& frame);
    // Submits through D3D12 restricted to the chroma range. No value: no candidate in range is supported (never
    // returned for the last resort, which stays selected even when it fails).
    std::optional<EncoderSubmitResult> try_select_d3d12(const CapturedFrame& frame, unsigned minimum_chroma_idc,
                                                        unsigned maximum_chroma_idc, bool last_resort);

    EncoderOptions options_{};
    std::unique_ptr<Encoder> selected_;
    HRESULT error_{S_OK};
    bool configured_{};
    bool running_{};
};

} // namespace beamit::media
