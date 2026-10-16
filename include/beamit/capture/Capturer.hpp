#pragma once

#include "beamit/media/VideoFrame.hpp"

#include <windows.h>

#include <optional>
#include <string_view>

namespace beamit::capture {

enum class CaptureTargetKind {
    Desktop,
    Window,
};

struct CaptureTarget {
    CaptureTargetKind kind{CaptureTargetKind::Desktop};
    HMONITOR monitor{nullptr};
    HWND window{nullptr};
};

struct CaptureOptions {
    bool capture_cursor{true};
};

class Capturer {
public:
    virtual ~Capturer() = default;

    [[nodiscard]] virtual std::wstring_view backend_name() const noexcept = 0;
    [[nodiscard]] virtual bool supports_target(const CaptureTarget& target) const noexcept = 0;
    virtual bool configure(const CaptureTarget& target, const CaptureOptions& options) = 0;
    virtual bool start() = 0;
    // Returns no value when no new frame is available; capture failures must be surfaced.
    [[nodiscard]] virtual std::optional<media::CapturedFrame> try_acquire_frame() = 0;
    virtual void stop() noexcept = 0;
    [[nodiscard]] virtual bool is_running() const noexcept = 0;
    [[nodiscard]] virtual HRESULT last_error() const noexcept = 0;
};

} // namespace beamit::capture
