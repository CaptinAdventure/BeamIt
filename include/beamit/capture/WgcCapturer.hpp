#pragma once

#include "beamit/capture/Capturer.hpp"

#include <memory>

namespace beamit::capture {

class WgcCapturer final : public Capturer {
public:
    WgcCapturer();
    ~WgcCapturer() override;

    WgcCapturer(const WgcCapturer&) = delete;
    WgcCapturer& operator=(const WgcCapturer&) = delete;

    [[nodiscard]] std::wstring_view backend_name() const noexcept override;
    [[nodiscard]] bool supports_target(const CaptureTarget& target) const noexcept override;
    bool configure(const CaptureTarget& target, const CaptureOptions& options) override;
    bool start() override;
    [[nodiscard]] std::optional<media::CapturedFrame> try_acquire_frame() override;
    void stop() noexcept override;
    [[nodiscard]] bool is_running() const noexcept override;
    [[nodiscard]] HRESULT last_error() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace beamit::capture
