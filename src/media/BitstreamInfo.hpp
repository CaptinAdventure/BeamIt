#pragma once

#include "beamit/media/VideoFrame.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace beamit::media {

// Decoder-facing parameters read from the SPS of an Annex B key frame.
struct BitstreamInfo {
    // RFC 6381 / WebCodecs codec string, e.g. "avc1.F4001F" or "hvc1.4.10.L120.90".
    std::string codec_string;
    std::uint32_t chroma_format_idc{1};
    std::uint32_t bit_depth_luma{8};
    std::uint32_t bit_depth_chroma{8};
};

[[nodiscard]] std::optional<BitstreamInfo> parse_bitstream_info(
    std::span<const std::byte> annex_b, VideoCodec codec);

[[nodiscard]] const char* chroma_format_name(std::uint32_t chroma_format_idc) noexcept;

} // namespace beamit::media
