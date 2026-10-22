#include "media/CodecHeaders.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace {

using namespace beamit::media;

std::vector<std::vector<std::byte>> nal_units(std::span<const std::byte> stream) {
    std::vector<std::vector<std::byte>> units;
    for (std::size_t i = 0; i < stream.size(); ++i) {
        if (i + 4 <= stream.size() && stream[i] == std::byte{0} &&
            stream[i + 1] == std::byte{0} && stream[i + 2] == std::byte{0} &&
            stream[i + 3] == std::byte{1}) {
            units.emplace_back();
            i += 3;
        } else {
            REQUIRE_FALSE(units.empty());
            units.back().push_back(stream[i]);
        }
    }
    return units;
}

class BitReader {
public:
    explicit BitReader(std::span<const std::byte> nal, std::size_t header_size) {
        unsigned zeroes = 0;
        for (const auto byte : nal.subspan(header_size)) {
            if (zeroes == 2 && byte == std::byte{3}) {
                zeroes = 0;
                continue;
            }
            bytes_.push_back(byte);
            zeroes = byte == std::byte{0} ? zeroes + 1 : 0;
        }
    }

    std::uint64_t bits(unsigned count) {
        REQUIRE(position_ + count <= bytes_.size() * 8);
        std::uint64_t value = 0;
        for (unsigned i = 0; i < count; ++i, ++position_) {
            value = (value << 1) |
                ((std::to_integer<unsigned>(bytes_[position_ / 8]) >> (7 - position_ % 8)) & 1U);
        }
        return value;
    }

    std::uint32_t ue() {
        unsigned zeroes = 0;
        while (bits(1) == 0) {
            REQUIRE(++zeroes < 32);
        }
        return static_cast<std::uint32_t>((std::uint64_t{1} << zeroes) - 1 + bits(zeroes));
    }

    void end() {
        REQUIRE(bits(1) == 1);
        while (position_ < bytes_.size() * 8) {
            REQUIRE(bits(1) == 0);
        }
    }

private:
    std::vector<std::byte> bytes_;
    std::size_t position_{0};
};

void read_hevc_profile(
    BitReader& reader,
    const detail::EncodeFormat& format = detail::default_encode_format) {
    REQUIRE(reader.bits(2) == 0);
    REQUIRE(reader.bits(1) == 0);
    const bool range_extension = format.chroma_idc != 1;
    REQUIRE(reader.bits(5) == (range_extension ? 4 : 1));
    REQUIRE(reader.bits(32) == (range_extension ? 0x08000000 : 0x40000000));
    REQUIRE(reader.bits(4) == 0xb);
    if (range_extension) {
        REQUIRE(reader.bits(2) == 3); // max 12-bit, max 10-bit
        REQUIRE(reader.bits(1) == (format.bit_depth == 8 ? 1 : 0));
        REQUIRE(reader.bits(1) == (format.chroma_idc <= 2 ? 1 : 0));
        REQUIRE(reader.bits(4) == 0); // not 4:2:0/monochrome/intra-only/single-picture
        REQUIRE(reader.bits(1) == 1); // lower bit rate
        REQUIRE(reader.bits(35) == 0);
    } else {
        REQUIRE(reader.bits(44) == 0);
    }
    REQUIRE(reader.bits(8) == 120);
}

EncoderOptions options(VideoCodec codec) {
    return {1920, 1080, 30, 12000, codec};
}

} // namespace

TEST_CASE("HEVC sampling candidates prefer 4:4:4 then 4:2:2 then 4:2:0", "[encoder][sampling]") {
    unsigned previous = 3;
    for (const auto& format : detail::hevc_formats) {
        REQUIRE(format.chroma_idc <= previous);
        previous = format.chroma_idc;
    }
    REQUIRE(detail::hevc_formats.front().chroma_idc == 3);
    REQUIRE(detail::hevc_formats[2].chroma_idc == 2);
    REQUIRE(detail::hevc_formats.back().texture == DXGI_FORMAT_NV12);
}

TEST_CASE("HEVC range-extension headers signal selected sampling and precision", "[encoder][bitstream][sampling]") {
    D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC level{
        D3D12_VIDEO_ENCODER_LEVELS_HEVC_4, D3D12_VIDEO_ENCODER_TIER_HEVC_MAIN};
    const D3D12_VIDEO_ENCODER_LEVEL_SETTING setting{
        .DataSize = sizeof(level), .pHEVCLevelSetting = &level};
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC config{
        .MinLumaCodingUnitSize = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE_8x8,
        .MaxLumaCodingUnitSize = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE_64x64,
        .MinLumaTransformUnitSize = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE_4x4,
        .MaxLumaTransformUnitSize = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE_32x32,
    };
    for (const auto& format : detail::hevc_formats) {
        INFO(format.name);
        auto requested = options(VideoCodec::H265);
        requested.width = 1918;
        requested.height = 1078;
        const auto units = nal_units(detail::make_parameter_sets(requested, setting, 64, &config, format));
        BitReader vps(units[0], 2);
        REQUIRE(vps.bits(32) == 0x0c01ffff);
        read_hevc_profile(vps, format);
        BitReader sps(units[1], 2);
        REQUIRE(sps.bits(8) == 1);
        read_hevc_profile(sps, format);
        REQUIRE(sps.ue() == 0);
        REQUIRE(sps.ue() == format.chroma_idc);
        if (format.chroma_idc == 3) REQUIRE(sps.bits(1) == 0);
        REQUIRE(sps.ue() == 1920);
        REQUIRE(sps.ue() == 1088);
        REQUIRE(sps.bits(1) == 1);
        REQUIRE(sps.ue() == 0);
        REQUIRE(sps.ue() == (format.chroma_idc == 3 ? 2 : 1));
        REQUIRE(sps.ue() == 0);
        REQUIRE(sps.ue() == (format.chroma_idc == 1 ? 5 : 10));
        REQUIRE(sps.ue() == format.bit_depth - 8);
        REQUIRE(sps.ue() == format.bit_depth - 8);
    }
}

TEST_CASE("H.264 parameter sets describe Main profile, crop and frame rate", "[encoder][bitstream]") {
    auto level = D3D12_VIDEO_ENCODER_LEVELS_H264_4;
    const D3D12_VIDEO_ENCODER_LEVEL_SETTING setting{
        .DataSize = sizeof(level), .pH264LevelSetting = &level};
    const auto units = nal_units(detail::make_parameter_sets(options(VideoCodec::H264), setting, 16));
    REQUIRE(units.size() == 2);
    REQUIRE(units[0][0] == std::byte{0x67});
    REQUIRE(units[1][0] == std::byte{0x68});
    BitReader sps(units[0], 1);
    REQUIRE(sps.bits(8) == 77);
    REQUIRE(sps.bits(8) == 0);
    REQUIRE(sps.bits(8) == 40);
    REQUIRE(sps.ue() == 0); // SPS id
    REQUIRE(sps.ue() == 9); // thirteen-bit frame_num
    REQUIRE(sps.ue() == 0); // POC type
    REQUIRE(sps.ue() == 9); // thirteen-bit POC
    REQUIRE(sps.ue() == 1); // max_num_ref_frames
    REQUIRE(sps.bits(1) == 0); // gaps
    REQUIRE(sps.ue() == 119);
    REQUIRE(sps.ue() == 67);
    REQUIRE(sps.bits(1) == 1); // progressive
    REQUIRE(sps.bits(1) == 0); // direct_8x8_inference
    REQUIRE(sps.bits(1) == 1); // cropping
    REQUIRE(sps.ue() == 0);
    REQUIRE(sps.ue() == 0);
    REQUIRE(sps.ue() == 0);
    REQUIRE(sps.ue() == 4); // 1088 -> 1080
    REQUIRE(sps.bits(1) == 1); // VUI
    REQUIRE(sps.bits(4) == 0);
    REQUIRE(sps.bits(1) == 1); // timing
    REQUIRE(sps.bits(32) == 1);
    REQUIRE(sps.bits(32) == 60);
    REQUIRE(sps.bits(1) == 1); // fixed frame rate
    REQUIRE(sps.bits(3) == 0);
    REQUIRE(sps.bits(2) == 3); // bitstream restrictions and motion vectors over boundaries
    REQUIRE(sps.ue() == 2);
    REQUIRE(sps.ue() == 1);
    REQUIRE(sps.ue() == 16);
    REQUIRE(sps.ue() == 16);
    REQUIRE(sps.ue() == 0); // no picture reordering
    REQUIRE(sps.ue() == 1); // one reference
    sps.end();

    BitReader pps(units[1], 1);
    REQUIRE(pps.ue() == 0);
    REQUIRE(pps.ue() == 0);
    REQUIRE(pps.bits(2) == 0);
    for (int i = 0; i < 3; ++i) {
        REQUIRE(pps.ue() == 0);
    }
    REQUIRE(pps.bits(3) == 0);
    for (int i = 0; i < 3; ++i) {
        REQUIRE(pps.ue() == 0);
    }
    REQUIRE(pps.bits(1) == 1); // deblocking control
    REQUIRE(pps.bits(2) == 0);
    pps.end();
}

TEST_CASE("HEVC parameter sets describe matching VPS, SPS and PPS", "[encoder][bitstream]") {
    D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC level{
        D3D12_VIDEO_ENCODER_LEVELS_HEVC_4, D3D12_VIDEO_ENCODER_TIER_HEVC_MAIN};
    const D3D12_VIDEO_ENCODER_LEVEL_SETTING setting{
        .DataSize = sizeof(level), .pHEVCLevelSetting = &level};
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC config{
        .MinLumaCodingUnitSize = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE_8x8,
        .MaxLumaCodingUnitSize = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE_64x64,
        .MinLumaTransformUnitSize = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE_4x4,
        .MaxLumaTransformUnitSize = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE_32x32,
    };
    SECTION("64x64 coding units") {}
    SECTION("Negotiated 32x32 coding units and asymmetric motion partition") {
        config.MaxLumaCodingUnitSize = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE_32x32;
        config.max_transform_hierarchy_depth_inter = 2;
        config.max_transform_hierarchy_depth_intra = 2;
        config.ConfigurationFlags =
            D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_FLAG_USE_ASYMETRIC_MOTION_PARTITION;
    }
    const auto units = nal_units(detail::make_parameter_sets(options(VideoCodec::H265), setting, 64, &config));
    REQUIRE(units.size() == 3);
    for (unsigned i = 0; i < 3; ++i) {
        REQUIRE(units[i][0] == static_cast<std::byte>((32 + i) << 1));
        REQUIRE(units[i][1] == std::byte{1});
    }
    BitReader vps(units[0], 2);
    REQUIRE(vps.bits(4) == 0);
    REQUIRE(vps.bits(2) == 3); // base layer internal/available
    REQUIRE(vps.bits(9) == 0);
    REQUIRE(vps.bits(1) == 1);
    REQUIRE(vps.bits(16) == 0xffff);
    read_hevc_profile(vps);
    REQUIRE(vps.bits(1) == 0);
    REQUIRE(vps.ue() == 1); // current plus reference picture
    REQUIRE(vps.ue() == 0);
    REQUIRE(vps.ue() == 1);
    REQUIRE(vps.bits(6) == 0);
    REQUIRE(vps.ue() == 0);
    REQUIRE(vps.bits(2) == 0);
    vps.end();

    BitReader sps(units[1], 2);
    REQUIRE(sps.bits(7) == 0);
    REQUIRE(sps.bits(1) == 1);
    read_hevc_profile(sps);
    REQUIRE(sps.ue() == 0);
    REQUIRE(sps.ue() == 1);
    REQUIRE(sps.ue() == 1920);
    REQUIRE(sps.ue() == 1088);
    REQUIRE(sps.bits(1) == 1);
    REQUIRE(sps.ue() == 0);
    REQUIRE(sps.ue() == 0);
    REQUIRE(sps.ue() == 0);
    REQUIRE(sps.ue() == 4);
    for (int i = 0; i < 2; ++i) {
        REQUIRE(sps.ue() == 0);
    }
    REQUIRE(sps.ue() == 9); // thirteen-bit POC
    REQUIRE(sps.bits(1) == 0);
    REQUIRE(sps.ue() == 1); // current plus reference picture
    REQUIRE(sps.ue() == 0);
    REQUIRE(sps.ue() == 1);
    REQUIRE(sps.ue() == 0);
    REQUIRE(sps.ue() == static_cast<unsigned>(config.MaxLumaCodingUnitSize));
    REQUIRE(sps.ue() == 0);
    REQUIRE(sps.ue() == 3);
    REQUIRE(sps.ue() == config.max_transform_hierarchy_depth_inter);
    REQUIRE(sps.ue() == config.max_transform_hierarchy_depth_intra);
    REQUIRE(sps.bits(1) == 0); // scaling list
    REQUIRE(sps.bits(1) == (config.ConfigurationFlags != 0 ? 1 : 0)); // AMP
    REQUIRE(sps.bits(2) == 0);
    REQUIRE(sps.ue() == 0);
    REQUIRE(sps.bits(3) == 0);
    REQUIRE(sps.bits(1) == 1); // VUI
    REQUIRE(sps.bits(8) == 0);
    REQUIRE(sps.bits(1) == 1);
    REQUIRE(sps.bits(32) == 1);
    REQUIRE(sps.bits(32) == 30);
    REQUIRE(sps.bits(4) == 0);
    sps.end();

    BitReader pps(units[2], 2);
    REQUIRE(pps.ue() == 0);
    REQUIRE(pps.ue() == 0);
    REQUIRE(pps.bits(6) == 0);
    REQUIRE(pps.bits(1) == 1); // cabac_init_present
    for (int i = 0; i < 3; ++i) {
        REQUIRE(pps.ue() == 0);
    }
    REQUIRE(pps.bits(2) == 0);
    REQUIRE(pps.bits(1) == 1); // cu_qp_delta
    for (int i = 0; i < 3; ++i) {
        REQUIRE(pps.ue() == 0);
    }
    REQUIRE(pps.bits(1) == 1); // slice chroma offsets
    REQUIRE(pps.bits(5) == 0);
    REQUIRE(pps.bits(2) == 3); // loop filter across slices, deblocking control
    REQUIRE(pps.bits(2) == 0);
    REQUIRE(pps.ue() == 0);
    REQUIRE(pps.ue() == 0);
    REQUIRE(pps.bits(2) == 0);
    REQUIRE(pps.ue() == 0);
    REQUIRE(pps.bits(2) == 0);
    pps.end();
}

TEST_CASE("Parameter sets reject missing levels and invalid dimensions", "[encoder][bitstream]") {
    REQUIRE_THROWS_AS(detail::make_parameter_sets(options(VideoCodec::H264), {}, 16), std::invalid_argument);
    auto level = D3D12_VIDEO_ENCODER_LEVELS_H264_4;
    const D3D12_VIDEO_ENCODER_LEVEL_SETTING setting{
        .DataSize = sizeof(level), .pH264LevelSetting = &level};
    auto invalid = options(VideoCodec::H264);
    invalid.width = 1919;
    REQUIRE_THROWS_AS(detail::make_parameter_sets(invalid, setting, 16), std::invalid_argument);
}
