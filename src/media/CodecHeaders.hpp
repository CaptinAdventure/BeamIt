#pragma once

#include "beamit/media/Encoder.hpp"
#include "EncodeFormat.hpp"

#include <d3d12video.h>

#include <vector>

namespace beamit::media::detail {

// Matches the encoder's negotiated profile/sampling and single-layer I/P configuration.
[[nodiscard]] std::vector<std::byte> make_parameter_sets(
    const EncoderOptions& options,
    const D3D12_VIDEO_ENCODER_LEVEL_SETTING& level,
    UINT subregion_block_size,
    const D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC* hevc_config = nullptr,
    const EncodeFormat& format = default_encode_format);

} // namespace beamit::media::detail
