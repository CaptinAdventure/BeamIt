#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <vector>

namespace beamit::media {

enum class VideoCodec {
    H264,
    H265,
};

struct CapturedFrame {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    std::chrono::steady_clock::time_point captured_at{};
    // Keeps borrowed backend surfaces valid until the consumer finishes GPU use.
    std::shared_ptr<void> lifetime;
};

struct EncodedPacket {
    std::vector<std::byte> data;
    std::chrono::steady_clock::time_point captured_at{};
    VideoCodec codec{VideoCodec::H264};
    bool key_frame{false};
};

} // namespace beamit::media
