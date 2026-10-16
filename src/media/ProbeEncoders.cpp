#include "ProbeEncoders.hpp"
#include "EncodeFormat.hpp"
#include "EncodeSequence.hpp"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <spdlog/spdlog.h>

#include <stdexcept>
#include <string>

namespace {

using Microsoft::WRL::ComPtr;

void check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        throw std::runtime_error(fmt::format("{} failed (HRESULT 0x{:08X})",
                                            operation, static_cast<std::uint32_t>(result)));
    }
}

void report(ID3D12VideoDevice3* device, bool h264,
            const beamit::media::detail::EncodeFormat& format,
            std::uint32_t fps, std::uint32_t bitrate) {
    using namespace beamit::media::detail;
    const auto codec = h264 ? D3D12_VIDEO_ENCODER_CODEC_H264 : D3D12_VIDEO_ENCODER_CODEC_HEVC;
    auto h264_profile = D3D12_VIDEO_ENCODER_PROFILE_H264_MAIN;
    auto hevc_profile = format.profile;
    const char* name = h264 ? "H.264 Main" :
        format.profile == D3D12_VIDEO_ENCODER_PROFILE_HEVC_MAIN ? "HEVC Main" :
        format.profile == D3D12_VIDEO_ENCODER_PROFILE_HEVC_MAIN10 ? "HEVC Main10" :
        format.profile == D3D12_VIDEO_ENCODER_PROFILE_HEVC_MAIN_444 ? "HEVC Main444" :
        format.profile == D3D12_VIDEO_ENCODER_PROFILE_HEVC_MAIN10_444 ? "HEVC Main10_444" :
        "HEVC Main10_422";
    const auto failure = [&](const char* stage, HRESULT result) {
        spdlog::info("  {} {} {}-bit {}: {} (HRESULT 0x{:08X})", name, format.sampling,
                     format.bit_depth, format.name, stage, static_cast<std::uint32_t>(result));
    };
    D3D12_VIDEO_ENCODER_PROFILE_DESC profile{};
    if (h264) {
        profile.DataSize = sizeof(h264_profile);
        profile.pH264Profile = &h264_profile;
    } else {
        profile.DataSize = sizeof(hevc_profile);
        profile.pHEVCProfile = &hevc_profile;
    }
    D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC codec_support{0, codec, FALSE};
    HRESULT result = device->CheckFeatureSupport(
        D3D12_FEATURE_VIDEO_ENCODER_CODEC, &codec_support, sizeof(codec_support));
    if (FAILED(result) || !codec_support.IsSupported) {
        failure("codec unsupported/query failed", result);
        return;
    }
    D3D12_VIDEO_ENCODER_LEVELS_H264 min_h264{}, max_h264{}, selected_h264{};
    D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC min_hevc{}, max_hevc{}, selected_hevc{};
    const auto level = [&](auto& h, auto& v) {
        D3D12_VIDEO_ENCODER_LEVEL_SETTING setting{};
        if (h264) { setting.DataSize = sizeof(h); setting.pH264LevelSetting = &h; }
        else { setting.DataSize = sizeof(v); setting.pHEVCLevelSetting = &v; }
        return setting;
    };
    D3D12_FEATURE_DATA_VIDEO_ENCODER_PROFILE_LEVEL levels{
        .NodeIndex = 0, .Codec = codec, .Profile = profile,
        .MinSupportedLevel = level(min_h264, min_hevc),
        .MaxSupportedLevel = level(max_h264, max_hevc),
    };
    result = device->CheckFeatureSupport(
        D3D12_FEATURE_VIDEO_ENCODER_PROFILE_LEVEL, &levels, sizeof(levels));
    if (FAILED(result) || !levels.IsSupported) {
        failure("profile unsupported/query failed", result);
        return;
    }
    D3D12_FEATURE_DATA_VIDEO_ENCODER_INPUT_FORMAT input{
        .NodeIndex = 0, .Codec = codec, .Profile = profile, .Format = format.texture,
    };
    result = device->CheckFeatureSupport(D3D12_FEATURE_VIDEO_ENCODER_INPUT_FORMAT, &input, sizeof(input));
    if (FAILED(result) || !input.IsSupported) {
        failure("profile supported; input format unsupported/query failed", result);
        return;
    }
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264 h264_config{};
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC hevc_config{
        .MinLumaCodingUnitSize = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE_8x8,
        .MaxLumaCodingUnitSize = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE_64x64,
        .MinLumaTransformUnitSize = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE_4x4,
        .MaxLumaTransformUnitSize = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE_32x32,
    };
    if (!h264) {
        bool found = false;
        for (const auto cu : {D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE_64x64,
                              D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE_32x32}) {
            for (UCHAR depth = 0; depth <= 3 && !found; ++depth) {
                D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC limits{
                    .MinLumaCodingUnitSize = hevc_config.MinLumaCodingUnitSize,
                    .MaxLumaCodingUnitSize = cu,
                    .MinLumaTransformUnitSize = hevc_config.MinLumaTransformUnitSize,
                    .MaxLumaTransformUnitSize = hevc_config.MaxLumaTransformUnitSize,
                    .max_transform_hierarchy_depth_inter = depth,
                    .max_transform_hierarchy_depth_intra = depth,
                };
                D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT config_support{
                    .NodeIndex = 0, .Codec = codec, .Profile = profile,
                };
                config_support.CodecSupportLimits.DataSize = sizeof(limits);
                config_support.CodecSupportLimits.pHEVCSupport = &limits;
                result = device->CheckFeatureSupport(
                    D3D12_FEATURE_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT,
                    &config_support, sizeof(config_support));
                if (FAILED(result)) { failure("configuration query failed", result); return; }
                if (!config_support.IsSupported) continue;
                found = true;
                hevc_config.MaxLumaCodingUnitSize = cu;
                hevc_config.max_transform_hierarchy_depth_inter = depth;
                hevc_config.max_transform_hierarchy_depth_intra = depth;
                if (limits.SupportFlags &
                    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC_FLAG_ASYMETRIC_MOTION_PARTITION_REQUIRED) {
                    hevc_config.ConfigurationFlags =
                        D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_FLAG_USE_ASYMETRIC_MOTION_PARTITION;
                }
            }
            if (found) break;
        }
        if (!found) { failure("input supported; current coding-unit configurations unsupported", S_OK); return; }
    }
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION config{};
    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE gop{};
    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE_H264 h264_gop{
        gop_length, 1, 0, order_bits_minus4, order_bits_minus4};
    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE_HEVC hevc_gop{
        gop_length, 1, order_bits_minus4};
    if (h264) {
        config.DataSize = sizeof(h264_config); config.pH264Config = &h264_config;
        gop.DataSize = sizeof(h264_gop); gop.pH264GroupOfPictures = &h264_gop;
    } else {
        config.DataSize = sizeof(hevc_config); config.pHEVCConfig = &hevc_config;
        gop.DataSize = sizeof(hevc_gop); gop.pHEVCGroupOfPictures = &hevc_gop;
    }
    D3D12_VIDEO_ENCODER_RATE_CONTROL_CBR cbr{};
    cbr.MaxQP = 51;
    cbr.TargetBitRate = static_cast<UINT64>(bitrate) * 1000;
    D3D12_VIDEO_ENCODER_RATE_CONTROL rate{};
    rate.Mode = D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CBR;
    rate.ConfigParams.DataSize = sizeof(cbr);
    rate.ConfigParams.pConfiguration_CBR = &cbr;
    rate.TargetFrameRate = {fps, 1};
    D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC resolution{1920, 1080};
    auto suggested_h264 = h264_profile;
    auto suggested_hevc = hevc_profile;
    auto suggested_profile = profile;
    if (h264) suggested_profile.pH264Profile = &suggested_h264;
    else suggested_profile.pHEVCProfile = &suggested_hevc;
    D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLUTION_SUPPORT_LIMITS limits{};
    D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT support{
        .NodeIndex = 0, .Codec = codec, .InputFormat = format.texture,
        .CodecConfiguration = config, .CodecGopSequence = gop, .RateControl = rate,
        .IntraRefresh = D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_NONE,
        .SubregionFrameEncoding = D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME,
        .ResolutionsListCount = 1, .pResolutionList = &resolution,
        .MaxReferenceFramesInDPB = 1, .SuggestedProfile = suggested_profile,
        .SuggestedLevel = level(selected_h264, selected_hevc), .pResolutionDependentSupport = &limits,
    };
    result = device->CheckFeatureSupport(D3D12_FEATURE_VIDEO_ENCODER_SUPPORT, &support, sizeof(support));
    const bool supported = SUCCEEDED(result) &&
        support.ValidationFlags == D3D12_VIDEO_ENCODER_VALIDATION_FLAG_NONE &&
        (support.SupportFlags & D3D12_VIDEO_ENCODER_SUPPORT_FLAG_GENERAL_SUPPORT_OK) &&
        suggested_h264 == h264_profile && suggested_hevc == hevc_profile;
    spdlog::info("  {} {} {}-bit {}: profile/input supported; 1080p I/P {} "
                 "(validation 0x{:X}, HRESULT 0x{:08X})",
                 name, format.sampling, format.bit_depth, format.name,
                 supported ? "SUPPORTED" : "UNSUPPORTED/query failed",
                 static_cast<unsigned>(support.ValidationFlags), static_cast<std::uint32_t>(result));
}

} // namespace

void probe_encoders(std::uint32_t fps, std::uint32_t bitrate) {
    using namespace beamit::media::detail;
    ComPtr<IDXGIFactory1> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create DXGI factory");
    spdlog::info("D3D12 adapter probe: 1920x1080, {} FPS, {} kbps, GOP 60, one reference, no B-frames",
                 fps, bitrate);
    spdlog::info("Capability queries only: not a live encode, conversion or cross-adapter transfer test");
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        const auto enumeration = factory->EnumAdapters1(index, &adapter);
        if (enumeration == DXGI_ERROR_NOT_FOUND) break;
        check(enumeration, "Enumerate adapters");
        DXGI_ADAPTER_DESC1 desc{};
        check(adapter->GetDesc1(&desc), "Read adapter description");
        const int length = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nullptr, 0, nullptr, nullptr);
        if (!length) check(HRESULT_FROM_WIN32(GetLastError()), "Convert adapter name");
        std::string name(length, '\0');
        if (!WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name.data(), length, nullptr, nullptr)) {
            check(HRESULT_FROM_WIN32(GetLastError()), "Convert adapter name");
        }
        name.pop_back();
        spdlog::info("Adapter {}: {} vendor 0x{:04X}, device 0x{:04X}, LUID {:08X}:{:08X}{}",
                     index, name, desc.VendorId, desc.DeviceId,
                     static_cast<UINT>(desc.AdapterLuid.HighPart), desc.AdapterLuid.LowPart,
                     desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE ? " [software; skipped]" : "");
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        ComPtr<ID3D12Device> device;
        auto result = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        ComPtr<ID3D12VideoDevice3> video;
        if (SUCCEEDED(result)) result = device.As(&video);
        if (FAILED(result)) {
            spdlog::warn("  D3D12 video device unavailable (HRESULT 0x{:08X})",
                         static_cast<std::uint32_t>(result));
            continue;
        }
        report(video.Get(), true, default_encode_format, fps, bitrate);
        for (const auto& format : hevc_formats) report(video.Get(), false, format, fps, bitrate);
        const EncodeFormat main10{
            D3D12_VIDEO_ENCODER_PROFILE_HEVC_MAIN10, DXGI_FORMAT_P010, 1, 10, "4:2:0", "P010"};
        report(video.Get(), false, main10, fps, bitrate);
    }
}
