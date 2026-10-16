#pragma once

#include "beamit/capture/Capturer.hpp"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace beamit::capture {

class DxgiCapturer final : public Capturer {
public:
    DxgiCapturer() = default;
    ~DxgiCapturer() override;

    DxgiCapturer(const DxgiCapturer&) = delete;
    DxgiCapturer& operator=(const DxgiCapturer&) = delete;

    [[nodiscard]] std::wstring_view backend_name() const noexcept override {
        return L"DXGI Desktop Duplication";
    }

    [[nodiscard]] bool supports_target(const CaptureTarget& target) const noexcept override {
        return target.kind == CaptureTargetKind::Desktop;
    }

    bool configure(const CaptureTarget& target, const CaptureOptions& options) override;
    bool start() override;
    [[nodiscard]] std::optional<media::CapturedFrame> try_acquire_frame() override;
    void stop() noexcept override;
    [[nodiscard]] bool is_running() const noexcept override;
    // Returns S_OK after a successful operation or a no-frame timeout.
    [[nodiscard]] HRESULT last_error() const noexcept override;

private:
    bool initialize_duplication();
    void set_error(HRESULT error) noexcept;

    CaptureTarget target_{};
    CaptureOptions options_{};
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;
    HRESULT last_error_{S_OK};
    bool configured_{false};
    bool running_{false};
    struct FrameLease;
    std::shared_ptr<FrameLease> frame_lease_;
};

} // namespace beamit::capture
