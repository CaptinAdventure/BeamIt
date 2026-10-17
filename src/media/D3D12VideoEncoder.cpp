#include "beamit/media/D3D12VideoEncoder.hpp"
#include "CodecHeaders.hpp"
#include "EncodeSequence.hpp"
#include "EncodeFormat.hpp"

#include <spdlog/spdlog.h>

#include <d3d11_4.h>
#include <d3d12.h>
#include <d3d12video.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace beamit::media {
namespace {

using Microsoft::WRL::ComPtr;

D3D12_RESOURCE_DESC buffer_desc(UINT64 size) {
    return D3D12_RESOURCE_DESC{
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Alignment = 0,
        .Width = size,
        .Height = 1,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_UNKNOWN,
        .SampleDesc = {.Count = 1, .Quality = 0},
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        .Flags = D3D12_RESOURCE_FLAG_NONE,
    };
}

HRESULT create_buffer(
    ID3D12Device* device,
    UINT64 size,
    D3D12_HEAP_TYPE heap_type,
    D3D12_RESOURCE_STATES initial_state,
    D3D12_RESOURCE_FLAGS flags,
    ID3D12Resource** resource) {
    const D3D12_HEAP_PROPERTIES heap_properties{
        .Type = heap_type,
        .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
        .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
        .CreationNodeMask = 1,
        .VisibleNodeMask = 1,
    };
    auto desc = buffer_desc(size);
    desc.Flags = flags;
    return device->CreateCommittedResource(
        &heap_properties,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        initial_state,
        nullptr,
        IID_PPV_ARGS(resource));
}

UINT64 align_up(UINT64 size, UINT alignment) {
    if (alignment == 0) {
        return 0;
    }
    const UINT64 remainder = size % alignment;
    if (remainder == 0) {
        return size;
    }
    const UINT64 padding = alignment - remainder;
    return size > (std::numeric_limits<UINT64>::max)() - padding ? 0 : size + padding;
}

void wait_for_fence(ID3D12Fence* fence, UINT64 value) noexcept {
    if (fence == nullptr || fence->GetCompletedValue() >= value) {
        return;
    }
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event == nullptr) {
        while (fence->GetCompletedValue() < value) {
            Sleep(1);
        }
        return;
    }
    if (SUCCEEDED(fence->SetEventOnCompletion(value, event))) {
        WaitForSingleObject(event, INFINITE);
    } else {
        while (fence->GetCompletedValue() < value) {
            Sleep(1);
        }
    }
    CloseHandle(event);
}

} // namespace

struct D3D12VideoEncoder::Impl {
    EncoderOptions options{};
    detail::EncodeFormat format{detail::default_encode_format};
    UINT format_planes{2};
    const char* initialization_stage{"device initialization"};
    HRESULT error{S_OK};
    bool configured{false};
    bool running{false};
    bool initialized{false};
    bool pending{false};
    bool readback_queued{false};
    bool pending_key_frame{true};
    bool key_frame_requested{false};
    // The driver accepts rate-control changes within a sequence; the next EncodeFrame carries the change.
    bool rate_control_reconfigurable{false};
    bool rate_control_changed{false};
    std::uint64_t frame_number{0};
    std::uint64_t shared_fence_value{0};
    std::uint64_t encode_fence_value{0};
    std::uint64_t copy_fence_value{0};
    UINT64 bitstream_capacity{0};
    UINT64 metadata_capacity{0};
    std::vector<std::byte> parameter_sets;
    std::chrono::steady_clock::time_point pending_timestamp{};
    std::optional<CapturedFrame> pending_frame;

    HANDLE shared_input_handle{nullptr};
    HANDLE shared_fence_handle{nullptr};

    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12VideoDevice3> video_device;
    ComPtr<ID3D12CommandQueue> direct_queue;
    ComPtr<ID3D12CommandQueue> encode_queue;
    ComPtr<ID3D12Fence> shared_fence;
    ComPtr<ID3D12Fence> encode_fence;
    ComPtr<ID3D12Fence> copy_fence;

    ComPtr<ID3D12Resource> input_texture;
    ComPtr<ID3D12Resource> reconstructed_textures[2];
    UINT reconstructed_subresources[2]{};
    ComPtr<ID3D12Resource> bitstream;
    ComPtr<ID3D12Resource> encode_metadata;
    ComPtr<ID3D12Resource> resolved_metadata;
    ComPtr<ID3D12Resource> bitstream_readback;
    ComPtr<ID3D12Resource> metadata_readback;
    ComPtr<ID3D12VideoEncoder> encoder;
    ComPtr<ID3D12VideoEncoderHeap> encoder_heap;

    ComPtr<ID3D12CommandAllocator> direct_allocator;
    ComPtr<ID3D12GraphicsCommandList> direct_list;
    ComPtr<ID3D12CommandAllocator> encode_allocator;
    ComPtr<ID3D12VideoEncodeCommandList2> encode_list;

    // Restarting at a GOP boundary keeps POC, idr_pic_id and reference slots consistent.
    void apply_key_frame_request() noexcept {
        if (!key_frame_requested) return;
        key_frame_requested = false;
        if (frame_number % detail::gop_length != 0) {
            frame_number = (frame_number / detail::gop_length + 1) * detail::gop_length;
        }
    }

    void clear() noexcept {
        if (shared_fence_value != 0 && shared_fence) {
            wait_for_fence(shared_fence.Get(), shared_fence_value);
        }
        if (copy_fence_value != 0 && copy_fence) {
            wait_for_fence(copy_fence.Get(), copy_fence_value);
        }
        if (encode_fence_value != 0 && encode_fence) {
            wait_for_fence(encode_fence.Get(), encode_fence_value);
        }
        running = false;
        initialized = false;
        pending = false;
        pending_frame.reset();
        readback_queued = false;
        frame_number = 0;
        key_frame_requested = false;
        rate_control_reconfigurable = false;
        rate_control_changed = false;
        parameter_sets.clear();
        shared_fence_value = 0;
        encode_fence_value = 0;
        copy_fence_value = 0;
        encode_list.Reset();
        encode_allocator.Reset();
        direct_list.Reset();
        direct_allocator.Reset();
        encoder_heap.Reset();
        encoder.Reset();
        metadata_readback.Reset();
        bitstream_readback.Reset();
        resolved_metadata.Reset();
        encode_metadata.Reset();
        bitstream.Reset();
        input_texture.Reset();
        for (auto& texture : reconstructed_textures) texture.Reset();
        reconstructed_subresources[0] = reconstructed_subresources[1] = 0;
        copy_fence.Reset();
        encode_fence.Reset();
        shared_fence.Reset();
        encode_queue.Reset();
        direct_queue.Reset();
        video_device.Reset();
        device.Reset();
        if (shared_input_handle != nullptr) {
            CloseHandle(shared_input_handle);
            shared_input_handle = nullptr;
        }
        if (shared_fence_handle != nullptr) {
            CloseHandle(shared_fence_handle);
            shared_fence_handle = nullptr;
        }
    }

    HRESULT initialize(ID3D11Texture2D* capture_texture);
    HRESULT initialize_format(ID3D11Texture2D* capture_texture);
    HRESULT convert(const CapturedFrame& frame);
    HRESULT encode();
    HRESULT queue_readback();
};

D3D12VideoEncoder::D3D12VideoEncoder() : impl_(std::make_unique<Impl>()) {}

D3D12VideoEncoder::~D3D12VideoEncoder() {
    stop();
}

std::wstring_view D3D12VideoEncoder::backend_name() const noexcept {
    return L"Direct3D 12 Video Encode";
}

bool D3D12VideoEncoder::configure(const EncoderOptions& options) {
    stop();
    if (options.width == 0 || options.height == 0 || options.frame_rate == 0 ||
        options.target_bitrate_kbps == 0 || (options.width & 1U) != 0 ||
        (options.height & 1U) != 0 ||
        (options.codec != VideoCodec::H264 && options.codec != VideoCodec::H265)) {
        impl_->error = E_INVALIDARG;
        impl_->configured = false;
        return false;
    }

    impl_->options = options;
    impl_->configured = true;
    impl_->error = S_OK;
    return true;
}

bool D3D12VideoEncoder::start() {
    if (impl_->running) {
        return true;
    }
    if (!impl_->configured) {
        impl_->error = E_UNEXPECTED;
        return false;
    }

    impl_->running = true;
    impl_->error = S_OK;
    return true;
}

EncoderSubmitResult D3D12VideoEncoder::try_submit(const CapturedFrame& frame) {
    if (!impl_->running || !frame.texture) {
        impl_->error = E_UNEXPECTED;
        return EncoderSubmitResult::Failed;
    }
    if (impl_->pending) {
        impl_->error = S_OK;
        return EncoderSubmitResult::WouldBlock;
    }
    const bool needs_initialization = !impl_->initialized;
    if (needs_initialization) {
        impl_->error = impl_->initialize(frame.texture.Get());
        if (FAILED(impl_->error)) {
            impl_->clear();
            return EncoderSubmitResult::Failed;
        }
    }

    impl_->apply_key_frame_request();
    impl_->pending_frame = frame;
    impl_->error = needs_initialization ? S_OK : impl_->convert(frame);
    if (SUCCEEDED(impl_->error)) impl_->error = impl_->encode();
    if (FAILED(impl_->error)) {
        const HRESULT error = impl_->error;
        impl_->clear();
        impl_->error = error;
        return EncoderSubmitResult::Failed;
    }

    impl_->pending = true;
    impl_->readback_queued = false;
    impl_->pending_timestamp = frame.captured_at;
    impl_->pending_key_frame = detail::picture_order(impl_->frame_number).idr;
    ++impl_->frame_number;
    impl_->error = S_OK;
    return EncoderSubmitResult::Accepted;
}

EncoderSubmitResult D3D12VideoEncoder::try_repeat_frame() {
    if (!impl_->running || !impl_->initialized || impl_->frame_number == 0) {
        impl_->error = E_UNEXPECTED;
        return EncoderSubmitResult::Failed;
    }
    if (impl_->pending) {
        impl_->error = S_OK;
        return EncoderSubmitResult::WouldBlock;
    }
    impl_->apply_key_frame_request();
    impl_->error = impl_->encode();
    if (FAILED(impl_->error)) {
        const auto error = impl_->error;
        impl_->clear();
        impl_->error = error;
        return EncoderSubmitResult::Failed;
    }
    impl_->pending = true;
    impl_->readback_queued = false;
    impl_->pending_key_frame = detail::picture_order(impl_->frame_number).idr;
    ++impl_->frame_number;
    return EncoderSubmitResult::Accepted;
}

void D3D12VideoEncoder::request_key_frame() noexcept {
    impl_->key_frame_requested = true;
}

bool D3D12VideoEncoder::try_update_bitrate(std::uint32_t bitrate_kbps) {
    if (!impl_->configured || FAILED(impl_->error) || !bitrate_kbps) return false;
    if (impl_->initialized && !impl_->rate_control_reconfigurable) return false;
    // Before deferred initialization the encoder is simply created with the new bitrate; the parameter sets carry no
    // HRD bitrate, so they stay valid either way.
    if (impl_->initialized && impl_->options.target_bitrate_kbps != bitrate_kbps) impl_->rate_control_changed = true;
    impl_->options.target_bitrate_kbps = bitrate_kbps;
    return true;
}

std::optional<EncodedPacket> D3D12VideoEncoder::try_receive() {
    if (!impl_->pending) {
        return std::nullopt;
    }

    const HRESULT device_status = impl_->device->GetDeviceRemovedReason();
    if (FAILED(device_status)) {
        impl_->error = device_status;
        return std::nullopt;
    }

    if (impl_->encode_fence->GetCompletedValue() < impl_->encode_fence_value) {
        impl_->error = S_OK;
        return std::nullopt;
    }

    if (!impl_->readback_queued) {
        impl_->error = impl_->queue_readback();
        if (FAILED(impl_->error)) {
            return std::nullopt;
        }
        impl_->readback_queued = true;
        return std::nullopt;
    }

    if (impl_->copy_fence->GetCompletedValue() < impl_->copy_fence_value) {
        impl_->error = S_OK;
        return std::nullopt;
    }

    D3D12_RANGE read_range{0, static_cast<SIZE_T>(impl_->metadata_capacity)};
    void* metadata_bytes = nullptr;
    HRESULT result = impl_->metadata_readback->Map(0, &read_range, &metadata_bytes);
    if (FAILED(result)) {
        impl_->error = result;
        return std::nullopt;
    }

    D3D12_VIDEO_ENCODER_OUTPUT_METADATA metadata{};
    std::memcpy(&metadata, metadata_bytes, sizeof(metadata));
    D3D12_VIDEO_ENCODER_FRAME_SUBREGION_METADATA subregion{};
    std::memcpy(
        &subregion,
        static_cast<const std::byte*>(metadata_bytes) + sizeof(metadata),
        sizeof(subregion));
    const D3D12_RANGE no_write{0, 0};
    impl_->metadata_readback->Unmap(0, &no_write);

    if (metadata.EncodeErrorFlags != D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_NO_ERROR ||
        metadata.EncodedBitstreamWrittenBytesCount == 0 ||
        metadata.EncodedBitstreamWrittenBytesCount > impl_->bitstream_capacity ||
        metadata.WrittenSubregionsCount != 1 ||
        subregion.bSize > metadata.EncodedBitstreamWrittenBytesCount ||
        subregion.bStartOffset >= subregion.bSize) {
        impl_->error = E_FAIL;
        impl_->pending = false;
        impl_->pending_frame.reset();
        return std::nullopt;
    }

    const auto byte_count = static_cast<SIZE_T>(subregion.bSize - subregion.bStartOffset);
    const auto offset = static_cast<SIZE_T>(subregion.bStartOffset);
    D3D12_RANGE bitstream_range{offset, offset + byte_count};
    void* bitstream_bytes = nullptr;
    result = impl_->bitstream_readback->Map(0, &bitstream_range, &bitstream_bytes);
    if (FAILED(result)) {
        impl_->error = result;
        return std::nullopt;
    }

    EncodedPacket packet{};
    packet.data = {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}};
    if (impl_->options.codec == VideoCodec::H264) {
        packet.data.push_back(std::byte{9});
    } else {
        packet.data.push_back(std::byte{70});
        packet.data.push_back(std::byte{1});
    }
    packet.data.push_back(impl_->pending_key_frame ? std::byte{0x10} : std::byte{0x30});
    if (impl_->pending_key_frame) {
        packet.data.insert(packet.data.end(), impl_->parameter_sets.begin(), impl_->parameter_sets.end());
    }
    const auto* payload = static_cast<const std::byte*>(bitstream_bytes) + offset;
    packet.data.insert(packet.data.end(), payload, payload + byte_count);
    packet.captured_at = impl_->pending_timestamp;
    packet.codec = impl_->options.codec;
    packet.key_frame = impl_->pending_key_frame;
    impl_->bitstream_readback->Unmap(0, &no_write);

    impl_->pending = false;
    impl_->pending_frame.reset();
    impl_->readback_queued = false;
    impl_->error = S_OK;
    return packet;
}

void D3D12VideoEncoder::stop() noexcept {
    if (impl_) {
        impl_->clear();
    }
}

bool D3D12VideoEncoder::is_running() const noexcept {
    return impl_->running;
}

HRESULT D3D12VideoEncoder::last_error() const noexcept {
    return impl_->error;
}

HRESULT D3D12VideoEncoder::Impl::initialize(ID3D11Texture2D* capture_texture) {
    for (const auto& candidate : detail::hevc_formats) {
        // The D3D12 H.264 API exposes no 4:4:4/4:2:2 profiles; never change the requested codec.
        if (options.codec == VideoCodec::H264 && candidate.texture != DXGI_FORMAT_NV12) continue;
        format = candidate;
        initialization_stage = "device initialization";
        HRESULT result = initialize_format(capture_texture);
        if (SUCCEEDED(result)) {
            initialization_stage = "D3D11 conversion/shared-resource interop";
            CapturedFrame frame{.texture = capture_texture};
            result = convert(frame);
        }
        if (SUCCEEDED(result)) {
            running = true;
            spdlog::info("Encoder sampling: {} {}-bit {} ({})",
                         format.sampling, format.bit_depth, format.name,
                         options.codec == VideoCodec::H264 ? "H.264" : "HEVC");
            return S_OK;
        }
        spdlog::warn("Encoder candidate {} {}-bit {} failed at {} (HRESULT 0x{:08X})",
                     format.sampling, format.bit_depth, format.name, initialization_stage,
                     static_cast<std::uint32_t>(result));
        clear();
        // Device removal, allocation failure and other operational errors are not capability fallback.
        if (result != E_INVALIDARG && result != E_NOTIMPL && result != E_NOINTERFACE &&
            result != DXGI_ERROR_UNSUPPORTED && result != HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)) {
            return result;
        }
    }
    return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
}

HRESULT D3D12VideoEncoder::Impl::initialize_format(ID3D11Texture2D* capture_texture) {
    if (capture_texture == nullptr) {
        return E_INVALIDARG;
    }

    D3D11_TEXTURE2D_DESC captured_desc{};
    capture_texture->GetDesc(&captured_desc);
    if (captured_desc.Width != options.width || captured_desc.Height != options.height ||
        captured_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        return E_INVALIDARG;
    }

    ComPtr<ID3D11Device> capture_device;
    capture_texture->GetDevice(&capture_device);
    ComPtr<IDXGIDevice> dxgi_capture_device;
    HRESULT result = capture_device.As(&dxgi_capture_device);
    if (FAILED(result)) {
        return result;
    }

    ComPtr<IDXGIAdapter> adapter;
    result = dxgi_capture_device->GetAdapter(&adapter);
    if (FAILED(result)) {
        return result;
    }
    result = D3D12CreateDevice(
        adapter.Get(),
        D3D_FEATURE_LEVEL_11_0,
        IID_PPV_ARGS(&device));
    if (FAILED(result)) {
        return result;
    }

    result = device.As(&video_device);
    if (FAILED(result)) {
        return result;
    }

    const D3D12_COMMAND_QUEUE_DESC direct_queue_desc{
        .Type = D3D12_COMMAND_LIST_TYPE_DIRECT,
        .Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL,
        .Flags = D3D12_COMMAND_QUEUE_FLAG_NONE,
        .NodeMask = 0,
    };
    result = device->CreateCommandQueue(&direct_queue_desc, IID_PPV_ARGS(&direct_queue));
    if (FAILED(result)) {
        return result;
    }

    auto encode_queue_desc = direct_queue_desc;
    encode_queue_desc.Type = D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE;
    result = device->CreateCommandQueue(&encode_queue_desc, IID_PPV_ARGS(&encode_queue));
    if (FAILED(result)) {
        return result;
    }

    const D3D12_VIDEO_ENCODER_CODEC codec =
        options.codec == VideoCodec::H264 ? D3D12_VIDEO_ENCODER_CODEC_H264
                                          : D3D12_VIDEO_ENCODER_CODEC_HEVC;
    D3D12_VIDEO_ENCODER_PROFILE_H264 h264_profile =
        D3D12_VIDEO_ENCODER_PROFILE_H264_MAIN;
    D3D12_VIDEO_ENCODER_PROFILE_HEVC hevc_profile = format.profile;
    D3D12_VIDEO_ENCODER_PROFILE_DESC profile{};
    profile.DataSize = options.codec == VideoCodec::H264
                           ? sizeof(h264_profile)
                           : sizeof(hevc_profile);
    if (options.codec == VideoCodec::H264) {
        profile.pH264Profile = &h264_profile;
    } else {
        profile.pHEVCProfile = &hevc_profile;
    }

    D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC codec_support{0, codec, FALSE};
    result = video_device->CheckFeatureSupport(
        D3D12_FEATURE_VIDEO_ENCODER_CODEC,
        &codec_support,
        sizeof(codec_support));
    if (FAILED(result) || !codec_support.IsSupported) {
        return FAILED(result) ? result : HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }

    D3D12_FEATURE_DATA_VIDEO_ENCODER_INPUT_FORMAT input_support{
        .NodeIndex = 0,
        .Codec = codec,
        .Profile = profile,
        .Format = format.texture,
        .IsSupported = FALSE,
    };
    initialization_stage = "codec/profile/input-format support";
    result = video_device->CheckFeatureSupport(
        D3D12_FEATURE_VIDEO_ENCODER_INPUT_FORMAT,
        &input_support,
        sizeof(input_support));
    if (FAILED(result) || !input_support.IsSupported) {
        return FAILED(result) ? result : HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }

    D3D12_VIDEO_ENCODER_LEVELS_H264 h264_min_level{}, h264_max_level{}, h264_level{};
    D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC hevc_min_level{}, hevc_max_level{}, hevc_level{};
    const auto level_setting = [&](D3D12_VIDEO_ENCODER_LEVELS_H264& h264,
                                   D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC& hevc) {
        D3D12_VIDEO_ENCODER_LEVEL_SETTING setting{};
        if (options.codec == VideoCodec::H264) {
            setting.DataSize = sizeof(h264);
            setting.pH264LevelSetting = &h264;
        } else {
            setting.DataSize = sizeof(hevc);
            setting.pHEVCLevelSetting = &hevc;
        }
        return setting;
    };
    D3D12_FEATURE_DATA_VIDEO_ENCODER_PROFILE_LEVEL profile_level{
        .NodeIndex = 0,
        .Codec = codec,
        .Profile = profile,
        .IsSupported = FALSE,
        .MinSupportedLevel = level_setting(h264_min_level, hevc_min_level),
        .MaxSupportedLevel = level_setting(h264_max_level, hevc_max_level),
    };
    initialization_stage = "profile/level support";
    result = video_device->CheckFeatureSupport(
        D3D12_FEATURE_VIDEO_ENCODER_PROFILE_LEVEL,
        &profile_level,
        sizeof(profile_level));
    if (FAILED(result) || !profile_level.IsSupported) {
        return FAILED(result) ? result : HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }

    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264 h264_config{};
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC hevc_config{
        .ConfigurationFlags = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_FLAG_NONE,
        .MinLumaCodingUnitSize = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE_8x8,
        .MaxLumaCodingUnitSize = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE_64x64,
        .MinLumaTransformUnitSize = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE_4x4,
        .MaxLumaTransformUnitSize = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE_32x32,
        .max_transform_hierarchy_depth_inter = 0,
        .max_transform_hierarchy_depth_intra = 0,
    };
    if (options.codec == VideoCodec::H265) {
        initialization_stage = "HEVC coding-unit configuration support";
        bool found = false;
        for (const auto max_cu : {D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE_64x64,
                                  D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE_32x32}) {
            for (UCHAR depth = 0; depth <= 3 && !found; ++depth) {
                D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC limits{
                    .MinLumaCodingUnitSize = hevc_config.MinLumaCodingUnitSize,
                    .MaxLumaCodingUnitSize = max_cu,
                    .MinLumaTransformUnitSize = hevc_config.MinLumaTransformUnitSize,
                    .MaxLumaTransformUnitSize = hevc_config.MaxLumaTransformUnitSize,
                    .max_transform_hierarchy_depth_inter = depth,
                    .max_transform_hierarchy_depth_intra = depth,
                };
                D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT configuration_support{
                    .NodeIndex = 0,
                    .Codec = codec,
                    .Profile = profile,
                };
                configuration_support.CodecSupportLimits.DataSize = sizeof(limits);
                configuration_support.CodecSupportLimits.pHEVCSupport = &limits;
                result = video_device->CheckFeatureSupport(
                    D3D12_FEATURE_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT,
                    &configuration_support, sizeof(configuration_support));
                if (FAILED(result)) {
                    return result;
                }
                if (configuration_support.IsSupported) {
                    hevc_config.MaxLumaCodingUnitSize = max_cu;
                    hevc_config.max_transform_hierarchy_depth_inter = depth;
                    hevc_config.max_transform_hierarchy_depth_intra = depth;
                    if ((limits.SupportFlags &
                         D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC_FLAG_ASYMETRIC_MOTION_PARTITION_REQUIRED) != 0) {
                        hevc_config.ConfigurationFlags =
                            D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_FLAG_USE_ASYMETRIC_MOTION_PARTITION;
                    }
                    found = true;
                }
            }
            if (found) {
                break;
            }
        }
        if (!found) {
            return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        }
    }
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION codec_config{};
    codec_config.DataSize = options.codec == VideoCodec::H264
                                ? sizeof(h264_config)
                                : sizeof(hevc_config);
    if (options.codec == VideoCodec::H264) {
        codec_config.pH264Config = &h264_config;
    } else {
        codec_config.pHEVCConfig = &hevc_config;
    }

    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE_H264 h264_gop{
        .GOPLength = detail::gop_length,
        .PPicturePeriod = 1,
        .pic_order_cnt_type = 0,
        .log2_max_frame_num_minus4 = detail::order_bits_minus4,
        .log2_max_pic_order_cnt_lsb_minus4 = detail::order_bits_minus4,
    };
    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE_HEVC hevc_gop{
        .GOPLength = detail::gop_length,
        .PPicturePeriod = 1,
        .log2_max_pic_order_cnt_lsb_minus4 = detail::order_bits_minus4,
    };
    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE gop{};
    gop.DataSize = options.codec == VideoCodec::H264 ? sizeof(h264_gop) : sizeof(hevc_gop);
    if (options.codec == VideoCodec::H264) {
        gop.pH264GroupOfPictures = &h264_gop;
    } else {
        gop.pHEVCGroupOfPictures = &hevc_gop;
    }

    D3D12_VIDEO_ENCODER_RATE_CONTROL_CBR cbr{
        .InitialQP = 0,
        .MinQP = 0,
        .MaxQP = 51,
        .MaxFrameBitSize = 0,
        .TargetBitRate = static_cast<UINT64>(options.target_bitrate_kbps) * 1000,
        .VBVCapacity = 0,
        .InitialVBVFullness = 0,
    };
    D3D12_VIDEO_ENCODER_RATE_CONTROL rate_control{};
    rate_control.Mode = D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CBR;
    rate_control.ConfigParams.DataSize = sizeof(cbr);
    rate_control.ConfigParams.pConfiguration_CBR = &cbr;
    rate_control.TargetFrameRate = DXGI_RATIONAL{options.frame_rate, 1};

    const D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC resolution{
        options.width,
        options.height,
    };
    auto suggested_h264_profile = h264_profile;
    auto suggested_hevc_profile = hevc_profile;
    auto suggested_profile = profile;
    if (options.codec == VideoCodec::H264) {
        suggested_profile.pH264Profile = &suggested_h264_profile;
    } else {
        suggested_profile.pHEVCProfile = &suggested_hevc_profile;
    }
    D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLUTION_SUPPORT_LIMITS resolution_support{};
    D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT support{
        .NodeIndex = 0,
        .Codec = codec,
        .InputFormat = format.texture,
        .CodecConfiguration = codec_config,
        .CodecGopSequence = gop,
        .RateControl = rate_control,
        .IntraRefresh = D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_NONE,
        .SubregionFrameEncoding = D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME,
        .ResolutionsListCount = 1,
        .pResolutionList = &resolution,
        .MaxReferenceFramesInDPB = 1,
        .SuggestedProfile = suggested_profile,
        .SuggestedLevel = level_setting(h264_level, hevc_level),
        .pResolutionDependentSupport = &resolution_support,
    };
    initialization_stage = "I/P encode configuration support";
    result = video_device->CheckFeatureSupport(
        D3D12_FEATURE_VIDEO_ENCODER_SUPPORT,
        &support,
        sizeof(support));
    if (FAILED(result)) {
        return result;
    }
    if (support.ValidationFlags != D3D12_VIDEO_ENCODER_VALIDATION_FLAG_NONE ||
        (support.SupportFlags & D3D12_VIDEO_ENCODER_SUPPORT_FLAG_GENERAL_SUPPORT_OK) == 0 ||
        suggested_h264_profile != h264_profile || suggested_hevc_profile != hevc_profile) {
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }
    rate_control_reconfigurable =
        (support.SupportFlags & D3D12_VIDEO_ENCODER_SUPPORT_FLAG_RATE_CONTROL_RECONFIGURATION_AVAILABLE) != 0;
    try {
        parameter_sets = detail::make_parameter_sets(
            options, support.SuggestedLevel, resolution_support.SubregionBlockPixelsSize, &hevc_config, format);
    } catch (const std::invalid_argument&) {
        return E_INVALIDARG;
    }

    D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOURCE_REQUIREMENTS requirements{
        .NodeIndex = 0,
        .Codec = codec,
        .Profile = profile,
        .InputFormat = format.texture,
        .PictureTargetResolution = resolution,
        .IsSupported = FALSE,
    };
    initialization_stage = "encoder resource requirements";
    result = video_device->CheckFeatureSupport(
        D3D12_FEATURE_VIDEO_ENCODER_RESOURCE_REQUIREMENTS,
        &requirements,
        sizeof(requirements));
    if (FAILED(result) || !requirements.IsSupported) {
        return FAILED(result) ? result : HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }

    const D3D12_VIDEO_ENCODER_DESC encoder_desc{
        .NodeMask = 0,
        .Flags = D3D12_VIDEO_ENCODER_FLAG_NONE,
        .EncodeCodec = codec,
        .EncodeProfile = profile,
        .InputFormat = format.texture,
        .CodecConfiguration = codec_config,
        .MaxMotionEstimationPrecision =
            D3D12_VIDEO_ENCODER_MOTION_ESTIMATION_PRECISION_MODE_MAXIMUM,
    };
    initialization_stage = "encoder creation";
    result = video_device->CreateVideoEncoder(
        &encoder_desc,
        IID_PPV_ARGS(&encoder));
    if (FAILED(result)) {
        return result;
    }

    const D3D12_VIDEO_ENCODER_HEAP_DESC heap_desc{
        .NodeMask = 0,
        .Flags = D3D12_VIDEO_ENCODER_HEAP_FLAG_NONE,
        .EncodeCodec = codec,
        .EncodeProfile = profile,
        .EncodeLevel = support.SuggestedLevel,
        .ResolutionsListCount = 1,
        .pResolutionList = &resolution,
    };
    initialization_stage = "encoder heap creation";
    result = video_device->CreateVideoEncoderHeap(
        &heap_desc,
        IID_PPV_ARGS(&encoder_heap));
    if (FAILED(result)) {
        return result;
    }

    const D3D12_HEAP_PROPERTIES default_heap{
        .Type = D3D12_HEAP_TYPE_DEFAULT,
        .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
        .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
        .CreationNodeMask = 1,
        .VisibleNodeMask = 1,
    };
    D3D12_RESOURCE_DESC input_desc{
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Alignment = 0,
        .Width = options.width,
        .Height = options.height,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = format.texture,
        .SampleDesc = {.Count = 1, .Quality = 0},
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                 D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS,
    };
    initialization_stage = "input/reconstruction resource allocation";
    result = device->CreateCommittedResource(
        &default_heap,
        D3D12_HEAP_FLAG_SHARED,
        &input_desc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&input_texture));
    if (FAILED(result)) {
        return result;
    }
    D3D12_FEATURE_DATA_FORMAT_INFO format_info{format.texture, 0};
    result = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &format_info, sizeof(format_info));
    if (FAILED(result)) return result;
    format_planes = format_info.PlaneCount;
    if (format_planes == 0) return E_FAIL;
    auto reconstructed_desc = input_desc;
    const UINT reconstruction_alignment = resolution_support.SubregionBlockPixelsSize;
    if (reconstruction_alignment == 0) {
        return E_FAIL;
    }
    reconstructed_desc.Width =
        (options.width + reconstruction_alignment - 1) / reconstruction_alignment * reconstruction_alignment;
    reconstructed_desc.Height =
        (options.height + reconstruction_alignment - 1) / reconstruction_alignment * reconstruction_alignment;
    reconstructed_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    const bool texture_array =
        (support.SupportFlags &
         D3D12_VIDEO_ENCODER_SUPPORT_FLAG_RECONSTRUCTED_FRAMES_REQUIRE_TEXTURE_ARRAYS) != 0;
    reconstructed_desc.DepthOrArraySize = texture_array ? 2 : 1;
    result = device->CreateCommittedResource(
        &default_heap, D3D12_HEAP_FLAG_NONE, &reconstructed_desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&reconstructed_textures[0]));
    if (FAILED(result)) {
        return result;
    }
    if (texture_array) {
        reconstructed_textures[1] = reconstructed_textures[0];
        reconstructed_subresources[1] = 1;
    } else {
        result = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &reconstructed_desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&reconstructed_textures[1]));
        if (FAILED(result)) return result;
    }
    result = device->CreateSharedHandle(
        input_texture.Get(),
        nullptr,
        GENERIC_ALL,
        nullptr,
        &shared_input_handle);
    if (FAILED(result)) {
        return result;
    }

    initialization_stage = "fences, buffers and command lists";
    result = device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&shared_fence));
    if (FAILED(result)) {
        return result;
    }
    result = device->CreateSharedHandle(
        shared_fence.Get(),
        nullptr,
        GENERIC_ALL,
        nullptr,
        &shared_fence_handle);
    if (FAILED(result)) {
        return result;
    }

    const UINT64 pixels = static_cast<UINT64>(options.width) * options.height;
    if (pixels > std::numeric_limits<UINT64>::max() / 4) {
        return E_INVALIDARG;
    }
    bitstream_capacity =
        align_up(pixels * 4, requirements.CompressedBitstreamBufferAccessAlignment);
    metadata_capacity = align_up(
        std::max<UINT64>(
            requirements.MaxEncoderOutputMetadataBufferSize,
            sizeof(D3D12_VIDEO_ENCODER_OUTPUT_METADATA) +
                sizeof(D3D12_VIDEO_ENCODER_FRAME_SUBREGION_METADATA)),
        requirements.EncoderMetadataBufferAccessAlignment);
    if (bitstream_capacity == 0 || metadata_capacity == 0) {
        return E_FAIL;
    }

    result = create_buffer(
        device.Get(),
        bitstream_capacity,
        D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_FLAG_NONE,
        &bitstream);
    if (FAILED(result)) {
        return result;
    }
    result = create_buffer(
        device.Get(),
        metadata_capacity,
        D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_FLAG_NONE,
        &encode_metadata);
    if (FAILED(result)) {
        return result;
    }
    result = create_buffer(
        device.Get(),
        metadata_capacity,
        D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_FLAG_NONE,
        &resolved_metadata);
    if (FAILED(result)) {
        return result;
    }
    result = create_buffer(
        device.Get(),
        bitstream_capacity,
        D3D12_HEAP_TYPE_READBACK,
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_FLAG_NONE,
        &bitstream_readback);
    if (FAILED(result)) {
        return result;
    }
    result = create_buffer(
        device.Get(),
        metadata_capacity,
        D3D12_HEAP_TYPE_READBACK,
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_FLAG_NONE,
        &metadata_readback);
    if (FAILED(result)) {
        return result;
    }

    result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&encode_fence));
    if (FAILED(result)) {
        return result;
    }
    result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copy_fence));
    if (FAILED(result)) {
        return result;
    }

    result = device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&direct_allocator));
    if (FAILED(result)) {
        return result;
    }
    result = device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        direct_allocator.Get(),
        nullptr,
        IID_PPV_ARGS(&direct_list));
    if (FAILED(result)) {
        return result;
    }
    result = direct_list->Close();
    if (FAILED(result)) {
        return result;
    }

    result = device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE,
        IID_PPV_ARGS(&encode_allocator));
    if (FAILED(result)) {
        return result;
    }
    ComPtr<ID3D12VideoEncodeCommandList2> encode_list2;
    result = device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE,
        encode_allocator.Get(),
        nullptr,
        IID_PPV_ARGS(&encode_list2));
    if (FAILED(result)) {
        return result;
    }
    encode_list = std::move(encode_list2);
    result = encode_list->Close();
    if (FAILED(result)) {
        return result;
    }

    initialized = true;
    return S_OK;
}

HRESULT D3D12VideoEncoder::Impl::convert(const CapturedFrame& frame) {
    D3D11_TEXTURE2D_DESC source_desc{};
    frame.texture->GetDesc(&source_desc);
    if (source_desc.Width != options.width || source_desc.Height != options.height ||
        source_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        return E_INVALIDARG;
    }

    ComPtr<ID3D11Device> d3d11_device;
    frame.texture->GetDevice(&d3d11_device);
    ComPtr<ID3D11Device1> d3d11_device1;
    ComPtr<ID3D11Device5> d3d11_device5;
    HRESULT result = d3d11_device.As(&d3d11_device1);
    if (FAILED(result)) {
        return result;
    }
    result = d3d11_device.As(&d3d11_device5);
    if (FAILED(result)) {
        return result;
    }

    ComPtr<ID3D11DeviceContext> base_context;
    d3d11_device->GetImmediateContext(&base_context);
    ComPtr<ID3D11VideoDevice> video_device11;
    ComPtr<ID3D11VideoContext> video_context11;
    ComPtr<ID3D11DeviceContext4> context4;
    result = d3d11_device.As(&video_device11);
    if (FAILED(result)) {
        return result;
    }
    result = base_context.As(&video_context11);
    if (FAILED(result)) {
        return result;
    }
    result = base_context.As(&context4);
    if (FAILED(result)) {
        return result;
    }

    const D3D11_VIDEO_PROCESSOR_CONTENT_DESC content_desc{
        .InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE,
        .InputFrameRate = {options.frame_rate, 1},
        .InputWidth = options.width,
        .InputHeight = options.height,
        .OutputFrameRate = {options.frame_rate, 1},
        .OutputWidth = options.width,
        .OutputHeight = options.height,
        .Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL,
    };
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    result = video_device11->CreateVideoProcessorEnumerator(&content_desc, &enumerator);
    if (FAILED(result)) {
        return result;
    }

    UINT format_support = 0;
    result = enumerator->CheckVideoProcessorFormat(
        DXGI_FORMAT_B8G8R8A8_UNORM,
        &format_support);
    if (FAILED(result) ||
        (format_support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0) {
        return FAILED(result) ? result : HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }
    result = enumerator->CheckVideoProcessorFormat(format.texture, &format_support);
    if (FAILED(result) ||
        (format_support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0) {
        return FAILED(result) ? result : HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }

    ComPtr<ID3D11Texture2D> shared_input;
    result = d3d11_device1->OpenSharedResource1(
        shared_input_handle,
        IID_PPV_ARGS(&shared_input));
    if (FAILED(result)) {
        return result;
    }
    ComPtr<ID3D11Fence> d3d11_shared_fence;
    result = d3d11_device5->OpenSharedFence(shared_fence_handle, IID_PPV_ARGS(&d3d11_shared_fence));
    if (FAILED(result)) {
        return result;
    }

    ComPtr<ID3D11VideoProcessor> processor;
    result = video_device11->CreateVideoProcessor(enumerator.Get(), 0, &processor);
    if (FAILED(result)) {
        return result;
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_view_desc{};
    input_view_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11VideoProcessorInputView> input_view;
    result = video_device11->CreateVideoProcessorInputView(
        frame.texture.Get(),
        enumerator.Get(),
        &input_view_desc,
        &input_view);
    if (FAILED(result)) {
        return result;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_view_desc{};
    output_view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11VideoProcessorOutputView> output_view;
    result = video_device11->CreateVideoProcessorOutputView(
        shared_input.Get(),
        enumerator.Get(),
        &output_view_desc,
        &output_view);
    if (FAILED(result)) {
        return result;
    }

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = input_view.Get();
    result = video_context11->VideoProcessorBlt(
        processor.Get(),
        output_view.Get(),
        0,
        1,
        &stream);
    if (FAILED(result)) {
        return result;
    }

    const auto next_fence_value = shared_fence_value + 1;
    result = context4->Signal(d3d11_shared_fence.Get(), next_fence_value);
    if (FAILED(result)) {
        return result;
    }
    shared_fence_value = next_fence_value;
    base_context->Flush();
    return S_OK;
}

HRESULT D3D12VideoEncoder::Impl::encode() {
    const auto order = detail::picture_order(frame_number);
    HRESULT result = encode_queue->Wait(shared_fence.Get(), shared_fence_value);
    if (FAILED(result)) {
        return result;
    }

    result = encode_allocator->Reset();
    if (FAILED(result)) {
        return result;
    }
    result = encode_list->Reset(encode_allocator.Get());
    if (FAILED(result)) {
        return result;
    }
    D3D12_RESOURCE_BARRIER encode_barriers[4]{};
    ID3D12Resource* encode_resources[]{
        input_texture.Get(), bitstream.Get(), encode_metadata.Get(), resolved_metadata.Get()};
    for (unsigned i = 0; i < std::size(encode_barriers); ++i) {
        encode_barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        encode_barriers[i].Transition = {
            .pResource = encode_resources[i],
            .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            .StateBefore = D3D12_RESOURCE_STATE_COMMON,
            .StateAfter = i == 0 ? D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ
                                : D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE,
        };
    }
    encode_list->ResourceBarrier(static_cast<UINT>(std::size(encode_barriers)), encode_barriers);
    std::vector<D3D12_RESOURCE_BARRIER> reference_barriers;
    for (unsigned slot = 0; slot < 2; ++slot) {
        if (order.idr && slot != order.output_slot) continue;
        const auto& texture = reconstructed_textures[slot];
        const auto array_size = texture->GetDesc().DepthOrArraySize;
        for (UINT plane = 0; plane < format_planes; ++plane) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition = {
                .pResource = texture.Get(),
                .Subresource = reconstructed_subresources[slot] + plane * array_size,
                .StateBefore = D3D12_RESOURCE_STATE_COMMON,
                .StateAfter = slot == order.output_slot ? D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE
                                                       : D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ,
            };
            reference_barriers.push_back(barrier);
        }
    }
    encode_list->ResourceBarrier(static_cast<UINT>(reference_barriers.size()), reference_barriers.data());

    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_DESC picture_control{};
    picture_control.Flags = D3D12_VIDEO_ENCODER_PICTURE_CONTROL_FLAG_USED_AS_REFERENCE_PICTURE;
    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_H264 h264_picture{};
    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_HEVC hevc_picture{};
    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA picture_data{};
    UINT reference_index = 0;
    ID3D12Resource* reference_texture = reconstructed_textures[order.reference_slot].Get();
    UINT reference_subresource = reconstructed_subresources[order.reference_slot];
    D3D12_VIDEO_ENCODER_REFERENCE_PICTURE_DESCRIPTOR_H264 h264_reference{
        .ReconstructedPictureResourceIndex = 0,
        .PictureOrderCountNumber = order.idr ? 0 : (order.index - 1) * 2,
        .FrameDecodingOrderNumber = order.idr ? 0 : order.index - 1,
    };
    D3D12_VIDEO_ENCODER_REFERENCE_PICTURE_DESCRIPTOR_HEVC hevc_reference{
        .ReconstructedPictureResourceIndex = 0,
        .IsRefUsedByCurrentPic = TRUE,
        .PictureOrderCountNumber = order.idr ? 0 : order.index - 1,
    };
    if (!order.idr) {
        picture_control.ReferenceFrames = {1, &reference_texture, &reference_subresource};
    }
    if (options.codec == VideoCodec::H264) {
        h264_picture.FrameType = order.idr ? D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_IDR_FRAME
                                         : D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_P_FRAME;
        h264_picture.idr_pic_id = static_cast<UINT>((frame_number / detail::gop_length) & 0xffffU);
        h264_picture.PictureOrderCountNumber = order.index * 2;
        h264_picture.FrameDecodingOrderNumber = order.index;
        if (!order.idr) {
            h264_picture.List0ReferenceFramesCount = 1;
            h264_picture.pList0ReferenceFrames = &reference_index;
            h264_picture.ReferenceFramesReconPictureDescriptorsCount = 1;
            h264_picture.pReferenceFramesReconPictureDescriptors = &h264_reference;
        }
        picture_data.DataSize = sizeof(h264_picture);
        picture_data.pH264PicData = &h264_picture;
    } else {
        hevc_picture.FrameType = order.idr ? D3D12_VIDEO_ENCODER_FRAME_TYPE_HEVC_IDR_FRAME
                                         : D3D12_VIDEO_ENCODER_FRAME_TYPE_HEVC_P_FRAME;
        hevc_picture.PictureOrderCountNumber = order.index;
        if (!order.idr) {
            hevc_picture.List0ReferenceFramesCount = 1;
            hevc_picture.pList0ReferenceFrames = &reference_index;
            hevc_picture.ReferenceFramesReconPictureDescriptorsCount = 1;
            hevc_picture.pReferenceFramesReconPictureDescriptors = &hevc_reference;
        }
        picture_data.DataSize = sizeof(hevc_picture);
        picture_data.pHEVCPicData = &hevc_picture;
    }
    picture_control.PictureControlCodecData = picture_data;

    D3D12_VIDEO_ENCODER_SEQUENCE_CONTROL_DESC sequence{};
    sequence.RateControl.Mode = D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CBR;
    sequence.RateControl.ConfigParams.DataSize = sizeof(D3D12_VIDEO_ENCODER_RATE_CONTROL_CBR);
    D3D12_VIDEO_ENCODER_RATE_CONTROL_CBR cbr{
        .InitialQP = 0,
        .MinQP = 0,
        .MaxQP = 51,
        .MaxFrameBitSize = 0,
        .TargetBitRate = static_cast<UINT64>(options.target_bitrate_kbps) * 1000,
        .VBVCapacity = 0,
        .InitialVBVFullness = 0,
    };
    sequence.RateControl.ConfigParams.pConfiguration_CBR = &cbr;
    sequence.RateControl.TargetFrameRate = DXGI_RATIONAL{options.frame_rate, 1};
    sequence.PictureTargetResolution = {options.width, options.height};
    sequence.SelectedLayoutMode = D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME;
    if (std::exchange(rate_control_changed, false)) {
        sequence.Flags |= D3D12_VIDEO_ENCODER_SEQUENCE_CONTROL_FLAG_RATE_CONTROL_CHANGE;
    }
    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE_H264 h264_gop{
        .GOPLength = detail::gop_length,
        .PPicturePeriod = 1,
        .pic_order_cnt_type = 0,
        .log2_max_frame_num_minus4 = detail::order_bits_minus4,
        .log2_max_pic_order_cnt_lsb_minus4 = detail::order_bits_minus4,
    };
    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE_HEVC hevc_gop{
        .GOPLength = detail::gop_length,
        .PPicturePeriod = 1,
        .log2_max_pic_order_cnt_lsb_minus4 = detail::order_bits_minus4,
    };
    if (options.codec == VideoCodec::H264) {
        sequence.CodecGopSequence.DataSize = sizeof(h264_gop);
        sequence.CodecGopSequence.pH264GroupOfPictures = &h264_gop;
    } else {
        sequence.CodecGopSequence.DataSize = sizeof(hevc_gop);
        sequence.CodecGopSequence.pHEVCGroupOfPictures = &hevc_gop;
    }

    D3D12_VIDEO_ENCODER_ENCODEFRAME_INPUT_ARGUMENTS input_arguments{};
    input_arguments.SequenceControlDesc = sequence;
    input_arguments.PictureControlDesc = picture_control;
    input_arguments.pInputFrame = input_texture.Get();

    D3D12_VIDEO_ENCODER_ENCODEFRAME_OUTPUT_ARGUMENTS output_arguments{};
    output_arguments.Bitstream.pBuffer = bitstream.Get();
    output_arguments.EncoderOutputMetadata.pBuffer = encode_metadata.Get();
    output_arguments.ReconstructedPicture.pReconstructedPicture =
        reconstructed_textures[order.output_slot].Get();
    output_arguments.ReconstructedPicture.ReconstructedPictureSubresource =
        reconstructed_subresources[order.output_slot];
    encode_list->EncodeFrame(encoder.Get(), encoder_heap.Get(), &input_arguments, &output_arguments);

    D3D12_RESOURCE_BARRIER metadata_barrier{
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
        .Transition = {
            .pResource = encode_metadata.Get(),
            .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            .StateBefore = D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE,
            .StateAfter = D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ,
        },
    };
    encode_list->ResourceBarrier(1, &metadata_barrier);
    for (auto& barrier : reference_barriers) {
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    }
    encode_list->ResourceBarrier(static_cast<UINT>(reference_barriers.size()), reference_barriers.data());

    D3D12_VIDEO_ENCODER_PROFILE_DESC profile{};
    D3D12_VIDEO_ENCODER_PROFILE_H264 h264_profile = D3D12_VIDEO_ENCODER_PROFILE_H264_MAIN;
    D3D12_VIDEO_ENCODER_PROFILE_HEVC hevc_profile = format.profile;
    profile.DataSize = options.codec == VideoCodec::H264 ? sizeof(h264_profile) : sizeof(hevc_profile);
    if (options.codec == VideoCodec::H264) {
        profile.pH264Profile = &h264_profile;
    } else {
        profile.pHEVCProfile = &hevc_profile;
    }
    const D3D12_VIDEO_ENCODER_RESOLVE_METADATA_INPUT_ARGUMENTS resolve_input{
        .EncoderCodec = options.codec == VideoCodec::H264 ? D3D12_VIDEO_ENCODER_CODEC_H264
                                                          : D3D12_VIDEO_ENCODER_CODEC_HEVC,
        .EncoderProfile = profile,
        .EncoderInputFormat = format.texture,
        .EncodedPictureEffectiveResolution = {options.width, options.height},
        .HWLayoutMetadata = {.pBuffer = encode_metadata.Get(), .Offset = 0},
    };
    D3D12_VIDEO_ENCODER_RESOLVE_METADATA_OUTPUT_ARGUMENTS resolve_output{
        .ResolvedLayoutMetadata = {.pBuffer = resolved_metadata.Get(), .Offset = 0},
    };
    encode_list->ResolveEncoderOutputMetadata(&resolve_input, &resolve_output);
    std::swap(metadata_barrier.Transition.StateBefore, metadata_barrier.Transition.StateAfter);
    encode_list->ResourceBarrier(1, &metadata_barrier);
    for (auto& barrier : encode_barriers) {
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    }
    encode_list->ResourceBarrier(static_cast<UINT>(std::size(encode_barriers)), encode_barriers);
    result = encode_list->Close();
    if (FAILED(result)) {
        return result;
    }
    ID3D12CommandList* encode_lists[]{encode_list.Get()};
    encode_queue->ExecuteCommandLists(1, encode_lists);
    ++encode_fence_value;
    return encode_queue->Signal(encode_fence.Get(), encode_fence_value);
}

HRESULT D3D12VideoEncoder::Impl::queue_readback() {
    HRESULT result = direct_queue->Wait(encode_fence.Get(), encode_fence_value);
    if (FAILED(result)) {
        return result;
    }
    result = direct_allocator->Reset();
    if (FAILED(result)) {
        return result;
    }
    result = direct_list->Reset(direct_allocator.Get(), nullptr);
    if (FAILED(result)) {
        return result;
    }

    const D3D12_RESOURCE_BARRIER barriers[]{
        {
            .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
            .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
            .Transition = {
                .pResource = bitstream.Get(),
                .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                .StateBefore = D3D12_RESOURCE_STATE_COMMON,
                .StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE,
            },
        },
        {
            .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
            .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
            .Transition = {
                .pResource = resolved_metadata.Get(),
                .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                .StateBefore = D3D12_RESOURCE_STATE_COMMON,
                .StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE,
            },
        },
    };
    direct_list->ResourceBarrier(static_cast<UINT>(std::size(barriers)), barriers);
    direct_list->CopyBufferRegion(bitstream_readback.Get(), 0, bitstream.Get(), 0, bitstream_capacity);
    direct_list->CopyBufferRegion(
        metadata_readback.Get(),
        0,
        resolved_metadata.Get(),
        0,
        metadata_capacity);
    const D3D12_RESOURCE_BARRIER restore[]{
        {
            .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
            .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
            .Transition = {
                .pResource = bitstream.Get(),
                .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                .StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE,
                .StateAfter = D3D12_RESOURCE_STATE_COMMON,
            },
        },
        {
            .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
            .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
            .Transition = {
                .pResource = resolved_metadata.Get(),
                .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                .StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE,
                .StateAfter = D3D12_RESOURCE_STATE_COMMON,
            },
        },
    };
    direct_list->ResourceBarrier(static_cast<UINT>(std::size(restore)), restore);
    result = direct_list->Close();
    if (FAILED(result)) {
        return result;
    }
    ID3D12CommandList* lists[]{direct_list.Get()};
    direct_queue->ExecuteCommandLists(1, lists);
    ++copy_fence_value;
    return direct_queue->Signal(copy_fence.Get(), copy_fence_value);
}

} // namespace beamit::media
