#pragma once

#include "ClientProtocol.hpp"
#include "FrameProtocol.hpp"

#include <dxgi1_2.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace beamit::client {

struct PipelineStats {
    std::uint64_t received{};
    std::uint64_t decoded{};
    std::uint64_t presented{};
    std::uint64_t coalesced{};      // Decoded pictures replaced by a newer one before presentation.
    std::uint64_t dropped{};        // Compressed pictures discarded while waiting for a key frame.
    std::string decoder;            // Media Foundation transform name, empty until the first key frame.
    std::string output;             // "GPU surface (adapter)" or "system memory upload".
    std::string output_format;
    std::uint32_t generation{};
    std::uint32_t width{};
    std::uint32_t height{};
    double decode_ms_average{};
    double decode_ms_max{};
    std::uint64_t last_capture_time_us{};
    std::chrono::steady_clock::time_point last_present; // Present call (enqueue), not display completion.
};

// Per-stream Media Foundation decode and D3D11 presentation on a dedicated thread.
// Pictures are decoded in order; only decoded pictures are coalesced before presentation.
class VideoPipeline {
public:
    struct Callbacks {
        // The stream cannot be decoded (e.g. unsupported profile or lost device). Pipeline thread.
        std::function<void(const std::string& message)> failed;
        // Decoding must resume from a key frame. Pipeline thread.
        std::function<void()> request_key_frame;
    };

    explicit VideoPipeline(Callbacks callbacks);
    ~VideoPipeline();
    VideoPipeline(const VideoPipeline&) = delete;
    VideoPipeline& operator=(const VideoPipeline&) = delete;

    // Composition swap chain for a WinUI SwapChainPanel.
    [[nodiscard]] IDXGISwapChain1* swap_chain() const;

    void configure(const VideoParameters& video);
    void submit(const VideoFrame& frame);
    // Physical pixels of the presentation area; the picture is letterboxed to keep its aspect ratio.
    void resize(unsigned width, unsigned height);
    // Encodes the most recent decoded picture as PNG at its video size; empty if none.
    [[nodiscard]] std::vector<std::byte> screenshot_png();
    [[nodiscard]] PipelineStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace beamit::client
