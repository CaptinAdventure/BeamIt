#pragma once

#include <d3d12video.h>

#include <array>

namespace beamit::media::detail {

struct EncodeFormat {
    D3D12_VIDEO_ENCODER_PROFILE_HEVC profile;
    DXGI_FORMAT texture;
    unsigned chroma_idc;
    unsigned bit_depth;
    const char* sampling;
    const char* name;
};

inline constexpr std::array hevc_formats{
    EncodeFormat{D3D12_VIDEO_ENCODER_PROFILE_HEVC_MAIN_444, DXGI_FORMAT_AYUV, 3, 8, "4:4:4", "AYUV"},
    EncodeFormat{D3D12_VIDEO_ENCODER_PROFILE_HEVC_MAIN10_444, DXGI_FORMAT_Y410, 3, 10, "4:4:4", "Y410"},
    EncodeFormat{D3D12_VIDEO_ENCODER_PROFILE_HEVC_MAIN10_422, DXGI_FORMAT_Y210, 2, 10, "4:2:2", "Y210"},
    EncodeFormat{D3D12_VIDEO_ENCODER_PROFILE_HEVC_MAIN, DXGI_FORMAT_NV12, 1, 8, "4:2:0", "NV12"},
};

inline constexpr auto default_encode_format = hevc_formats.back();

} // namespace beamit::media::detail
