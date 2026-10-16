#include "CodecHeaders.hpp"
#include "EncodeSequence.hpp"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <stdexcept>

namespace beamit::media::detail {
namespace {

class BitWriter {
public:
    void bits(std::uint64_t value, unsigned count) {
        for (unsigned i = count; i != 0; --i) {
            if (used_ == 0) {
                bytes_.push_back(std::byte{0});
            }
            bytes_.back() |= static_cast<std::byte>(((value >> (i - 1)) & 1U) << (7 - used_));
            used_ = (used_ + 1) % 8;
        }
    }

    void flag(bool value) { bits(value ? 1 : 0, 1); }

    void ue(std::uint32_t value) {
        const std::uint64_t code = static_cast<std::uint64_t>(value) + 1;
        unsigned width = 0;
        for (auto remaining = code; remaining != 0; remaining >>= 1) {
            ++width;
        }
        bits(0, width - 1);
        bits(code, width);
    }

    void finish() {
        flag(true); // rbsp_stop_one_bit
        if (used_ != 0) {
            bits(0, 8 - used_);
        }
    }

    [[nodiscard]] std::span<const std::byte> bytes() const { return bytes_; }

private:
    std::vector<std::byte> bytes_;
    unsigned used_{0};
};

void append_nal(
    std::vector<std::byte>& output,
    std::initializer_list<unsigned> header,
    BitWriter rbsp) {
    rbsp.finish();
    output.insert(output.end(), {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}});
    for (const auto byte : header) {
        output.push_back(static_cast<std::byte>(byte));
    }
    unsigned zeroes = 0;
    for (const auto byte : rbsp.bytes()) {
        if (zeroes == 2 && byte <= std::byte{3}) {
            output.push_back(std::byte{3}); // emulation_prevention_three_byte
            zeroes = 0;
        }
        output.push_back(byte);
        zeroes = byte == std::byte{0} ? zeroes + 1 : 0;
    }
}

std::vector<std::byte> h264_headers(
    const EncoderOptions& options,
    D3D12_VIDEO_ENCODER_LEVELS_H264 level) {
    constexpr std::array<unsigned, 20> levels{
        10, 11, 11, 12, 13, 20, 21, 22, 30, 31, 32, 40, 41, 42, 50, 51, 52, 60, 61, 62};
    if (static_cast<unsigned>(level) >= levels.size()) {
        throw std::invalid_argument("Unknown H.264 level");
    }
    const auto width_mbs = (options.width + 15) / 16;
    const auto height_mbs = (options.height + 15) / 16;
    const bool cropped = width_mbs * 16 != options.width || height_mbs * 16 != options.height;

    BitWriter sps;
    sps.bits(77, 8); // Main profile
    sps.bits(level == D3D12_VIDEO_ENCODER_LEVELS_H264_1b ? 0x10 : 0, 8);
    sps.bits(levels[static_cast<unsigned>(level)], 8);
    sps.ue(0); // seq_parameter_set_id
    sps.ue(order_bits_minus4); // log2_max_frame_num_minus4
    sps.ue(0); // pic_order_cnt_type
    sps.ue(order_bits_minus4); // log2_max_pic_order_cnt_lsb_minus4
    sps.ue(1); // max_num_ref_frames
    sps.flag(false); // gaps_in_frame_num_value_allowed_flag
    sps.ue(width_mbs - 1);
    sps.ue(height_mbs - 1);
    sps.flag(true); // frame_mbs_only_flag (progressive)
    sps.flag(false); // direct_8x8_inference_flag
    sps.flag(cropped);
    if (cropped) {
        sps.ue(0);
        sps.ue((width_mbs * 16 - options.width) / 2);
        sps.ue(0);
        sps.ue((height_mbs * 16 - options.height) / 2);
    }
    sps.flag(true); // vui_parameters_present_flag
    sps.flag(false); // aspect_ratio_info_present_flag
    sps.flag(false); // overscan_info_present_flag
    sps.flag(false); // video_signal_type_present_flag
    sps.flag(false); // chroma_loc_info_present_flag
    sps.flag(true); // timing_info_present_flag
    sps.bits(1, 32); // num_units_in_tick
    sps.bits(static_cast<std::uint64_t>(options.frame_rate) * 2, 32);
    sps.flag(true); // fixed_frame_rate_flag
    sps.flag(false); // nal_hrd_parameters_present_flag
    sps.flag(false); // vcl_hrd_parameters_present_flag
    sps.flag(false); // pic_struct_present_flag
    sps.flag(true); // bitstream_restriction_flag
    sps.flag(true); // motion_vectors_over_pic_boundaries_flag
    sps.ue(2); // max_bytes_per_pic_denom
    sps.ue(1); // max_bits_per_mb_denom
    sps.ue(16); // log2_max_mv_length_horizontal
    sps.ue(16); // log2_max_mv_length_vertical
    sps.ue(0); // max_num_reorder_frames
    sps.ue(1); // max_dec_frame_buffering

    BitWriter pps;
    pps.ue(0); // pic_parameter_set_id
    pps.ue(0); // seq_parameter_set_id
    pps.flag(false); // entropy_coding_mode_flag: CAVLC
    pps.flag(false); // bottom_field_pic_order_in_frame_present_flag
    pps.ue(0); // num_slice_groups_minus1
    pps.ue(0); // num_ref_idx_l0_default_active_minus1
    pps.ue(0); // num_ref_idx_l1_default_active_minus1
    pps.flag(false); // weighted_pred_flag
    pps.bits(0, 2); // weighted_bipred_idc
    pps.ue(0); // pic_init_qp_minus26, signed zero
    pps.ue(0); // pic_init_qs_minus26
    pps.ue(0); // chroma_qp_index_offset
    pps.flag(true); // deblocking_filter_control_present_flag
    pps.flag(false); // constrained_intra_pred_flag
    pps.flag(false); // redundant_pic_cnt_present_flag

    std::vector<std::byte> output;
    append_nal(output, {0x67}, sps);
    append_nal(output, {0x68}, pps);
    return output;
}

void hevc_profile_tier_level(
    BitWriter& writer,
    const D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC& level,
    const EncodeFormat& format) {
    constexpr std::array<unsigned, 13> levels{
        30, 60, 63, 90, 93, 120, 123, 150, 153, 156, 180, 183, 186};
    if (static_cast<unsigned>(level.Level) >= levels.size()) {
        throw std::invalid_argument("Unknown HEVC level");
    }
    writer.bits(0, 2); // general_profile_space
    writer.flag(level.Tier == D3D12_VIDEO_ENCODER_TIER_HEVC_HIGH);
    const bool range_extension = format.chroma_idc != 1;
    const unsigned profile_idc = range_extension ? 4 : 1;
    writer.bits(profile_idc, 5);
    writer.bits(1U << (31 - profile_idc), 32);
    writer.bits(0xb, 4); // progressive, non-packed, frame-only
    if (range_extension) {
        writer.flag(true); // max_12bit_constraint_flag
        writer.flag(true); // max_10bit_constraint_flag
        writer.flag(format.bit_depth == 8); // max_8bit_constraint_flag
        writer.flag(format.chroma_idc <= 2); // max_422chroma_constraint_flag
        writer.flag(false); // max_420chroma_constraint_flag
        writer.flag(false); // max_monochrome_constraint_flag
        writer.flag(false); // intra_constraint_flag
        writer.flag(false); // one_picture_only_constraint_flag
        writer.flag(true); // lower_bit_rate_constraint_flag
        writer.bits(0, 35); // reserved and inbld flag
    } else {
        writer.bits(0, 44);
    }
    writer.bits(levels[static_cast<unsigned>(level.Level)], 8);
}

std::vector<std::byte> hevc_headers(
    const EncoderOptions& options,
    const D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC& level,
    UINT block_size,
    const D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC& config,
    const EncodeFormat& format) {
    if (block_size == 0 || block_size > 64 || (block_size & (block_size - 1)) != 0) {
        throw std::invalid_argument("Invalid HEVC resolution block size");
    }
    const auto width = (options.width + block_size - 1) / block_size * block_size;
    const auto height = (options.height + block_size - 1) / block_size * block_size;
    const bool cropped = width != options.width || height != options.height;

    BitWriter vps;
    vps.bits(0, 4); // vps_video_parameter_set_id
    vps.flag(true); // vps_base_layer_internal_flag
    vps.flag(true); // vps_base_layer_available_flag
    vps.bits(0, 6); // vps_max_layers_minus1
    vps.bits(0, 3); // vps_max_sub_layers_minus1
    vps.flag(true); // vps_temporal_id_nesting_flag
    vps.bits(0xffff, 16);
    hevc_profile_tier_level(vps, level, format);
    vps.flag(false); // vps_sub_layer_ordering_info_present_flag
    vps.ue(1); // vps_max_dec_pic_buffering_minus1: current plus previous picture
    vps.ue(0); // vps_max_num_reorder_pics
    vps.ue(1); // vps_max_latency_increase_plus1
    vps.bits(0, 6); // vps_max_layer_id
    vps.ue(0); // vps_num_layer_sets_minus1
    vps.flag(false); // vps_timing_info_present_flag
    vps.flag(false); // vps_extension_flag

    BitWriter sps;
    sps.bits(0, 4); // sps_video_parameter_set_id
    sps.bits(0, 3); // sps_max_sub_layers_minus1
    sps.flag(true); // sps_temporal_id_nesting_flag
    hevc_profile_tier_level(sps, level, format);
    sps.ue(0); // sps_seq_parameter_set_id
    sps.ue(format.chroma_idc);
    if (format.chroma_idc == 3) sps.flag(false); // separate_colour_plane_flag
    sps.ue(width);
    sps.ue(height);
    sps.flag(cropped);
    if (cropped) {
        sps.ue(0);
        sps.ue((width - options.width) / (format.chroma_idc == 3 ? 1 : 2));
        sps.ue(0);
        sps.ue((height - options.height) / (format.chroma_idc == 1 ? 2 : 1));
    }
    sps.ue(format.bit_depth - 8);
    sps.ue(format.bit_depth - 8);
    sps.ue(order_bits_minus4); // log2_max_pic_order_cnt_lsb_minus4
    sps.flag(false); // sps_sub_layer_ordering_info_present_flag
    sps.ue(1); // sps_max_dec_pic_buffering_minus1
    sps.ue(0); // sps_max_num_reorder_pics
    sps.ue(1); // sps_max_latency_increase_plus1
    const auto min_cu = static_cast<unsigned>(config.MinLumaCodingUnitSize) + 3;
    const auto max_cu = static_cast<unsigned>(config.MaxLumaCodingUnitSize) + 3;
    const auto min_tu = static_cast<unsigned>(config.MinLumaTransformUnitSize) + 2;
    const auto max_tu = static_cast<unsigned>(config.MaxLumaTransformUnitSize) + 2;
    sps.ue(min_cu - 3);
    sps.ue(max_cu - min_cu);
    sps.ue(min_tu - 2);
    sps.ue(max_tu - min_tu);
    sps.ue(config.max_transform_hierarchy_depth_inter);
    sps.ue(config.max_transform_hierarchy_depth_intra);
    sps.flag(false); // scaling_list_enabled_flag
    sps.flag((config.ConfigurationFlags &
              D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_FLAG_USE_ASYMETRIC_MOTION_PARTITION) != 0);
    sps.flag(false); // sample_adaptive_offset_enabled_flag
    sps.flag(false); // pcm_enabled_flag
    sps.ue(0); // num_short_term_ref_pic_sets
    sps.flag(false); // long_term_ref_pics_present_flag
    sps.flag(false); // sps_temporal_mvp_enabled_flag
    sps.flag(false); // strong_intra_smoothing_enabled_flag
    sps.flag(true); // vui_parameters_present_flag
    sps.flag(false); // aspect_ratio_info_present_flag
    sps.flag(false); // overscan_info_present_flag
    sps.flag(false); // video_signal_type_present_flag
    sps.flag(false); // chroma_loc_info_present_flag
    sps.flag(false); // neutral_chroma_indication_flag
    sps.flag(false); // field_seq_flag
    sps.flag(false); // frame_field_info_present_flag
    sps.flag(false); // default_display_window_flag
    sps.flag(true); // vui_timing_info_present_flag
    sps.bits(1, 32); // vui_num_units_in_tick
    sps.bits(options.frame_rate, 32); // vui_time_scale
    sps.flag(false); // vui_poc_proportional_to_timing_flag
    sps.flag(false); // vui_hrd_parameters_present_flag
    sps.flag(false); // bitstream_restriction_flag
    sps.flag(false); // sps_extension_present_flag

    BitWriter pps;
    pps.ue(0); // pps_pic_parameter_set_id
    pps.ue(0); // pps_seq_parameter_set_id
    pps.flag(false); // dependent_slice_segments_enabled_flag
    pps.flag(false); // output_flag_present_flag
    pps.bits(0, 3); // num_extra_slice_header_bits
    pps.flag(false); // sign_data_hiding_enabled_flag
    pps.flag(true); // cabac_init_present_flag
    pps.ue(0); // num_ref_idx_l0_default_active_minus1
    pps.ue(0); // num_ref_idx_l1_default_active_minus1
    pps.ue(0); // init_qp_minus26, signed zero
    pps.flag(false); // constrained_intra_pred_flag
    pps.flag(false); // transform_skip_enabled_flag
    pps.flag(true); // cu_qp_delta_enabled_flag
    pps.ue(0); // diff_cu_qp_delta_depth
    pps.ue(0); // pps_cb_qp_offset
    pps.ue(0); // pps_cr_qp_offset
    pps.flag(true); // pps_slice_chroma_qp_offsets_present_flag
    pps.flag(false); // weighted_pred_flag
    pps.flag(false); // weighted_bipred_flag
    pps.flag(false); // transquant_bypass_enabled_flag
    pps.flag(false); // tiles_enabled_flag
    pps.flag(false); // entropy_coding_sync_enabled_flag
    pps.flag(true); // pps_loop_filter_across_slices_enabled_flag
    pps.flag(true); // deblocking_filter_control_present_flag
    pps.flag(false); // deblocking_filter_override_enabled_flag
    pps.flag(false); // pps_deblocking_filter_disabled_flag
    pps.ue(0); // pps_beta_offset_div2
    pps.ue(0); // pps_tc_offset_div2
    pps.flag(false); // pps_scaling_list_data_present_flag
    pps.flag(false); // lists_modification_present_flag
    pps.ue(0); // log2_parallel_merge_level_minus2
    pps.flag(false); // slice_segment_header_extension_present_flag
    pps.flag(false); // pps_extension_present_flag

    std::vector<std::byte> output;
    append_nal(output, {32 << 1, 1}, vps);
    append_nal(output, {33 << 1, 1}, sps);
    append_nal(output, {34 << 1, 1}, pps);
    return output;
}

} // namespace

std::vector<std::byte> make_parameter_sets(
    const EncoderOptions& options,
    const D3D12_VIDEO_ENCODER_LEVEL_SETTING& level,
    UINT subregion_block_size,
    const D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC* hevc_config,
    const EncodeFormat& format) {
    if (options.width == 0 || options.height == 0 || options.width > 65536 ||
        options.height > 65536 || (options.width & 1U) != 0 ||
        (options.height & 1U) != 0 || options.frame_rate == 0 ||
        options.frame_rate > 1000) {
        throw std::invalid_argument("Invalid parameter-set dimensions or frame rate");
    }
    if (options.codec == VideoCodec::H264 && level.DataSize == sizeof(D3D12_VIDEO_ENCODER_LEVELS_H264) &&
        level.pH264LevelSetting != nullptr) {
        return h264_headers(options, *level.pH264LevelSetting);
    }
    if (options.codec == VideoCodec::H265 &&
        level.DataSize == sizeof(D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC) &&
        level.pHEVCLevelSetting != nullptr && hevc_config != nullptr) {
        return hevc_headers(options, *level.pHEVCLevelSetting, subregion_block_size, *hevc_config, format);
    }
    throw std::invalid_argument("Missing codec level");
}

} // namespace beamit::media::detail
