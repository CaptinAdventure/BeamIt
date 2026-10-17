#include "beamit/media/NvencVideoEncoder.hpp"
#include "NvencApi.hpp"

#include <d3d11_4.h>
#include <nvEncodeAPI.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace beamit::media {

struct NvencVideoEncoder::Impl {
    EncoderOptions options{};
    HRESULT error{S_OK};
    bool configured{};
    bool running{};
    bool pending{};
    bool has_image{};
    bool event_registered{};
    bool locked{};
    std::uint64_t copy_value{};
    std::uint64_t sequence{};
    bool key_frame_requested{};
    HMODULE module{};
    HANDLE completion{};
    HANDLE copy_completion{};
    NV_ENCODE_API_FUNCTION_LIST api{};
    // Session parameters, kept for in-place reconfiguration; init.encodeConfig points to config.
    NV_ENC_CONFIG config{};
    NV_ENC_INITIALIZE_PARAMS init{};
    void* session{};
    NV_ENC_REGISTERED_PTR registered{};
    NV_ENC_INPUT_PTR mapped{};
    NV_ENC_OUTPUT_PTR output{};
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> input;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context4;
    Microsoft::WRL::ComPtr<ID3D11Fence> copy_fence;
    CapturedFrame source;
    std::chrono::steady_clock::time_point timestamp{};

    bool fail(HRESULT result, const char* operation) {
        error = result;
        spdlog::error("NVENC {} failed (HRESULT 0x{:08X})",
                      operation, static_cast<std::uint32_t>(result));
        return false;
    }

    bool check(NVENCSTATUS status, const char* operation) {
        if (status == NV_ENC_SUCCESS) return true;
        switch (status) {
        case NV_ENC_ERR_NO_ENCODE_DEVICE:
        case NV_ENC_ERR_UNSUPPORTED_DEVICE:
        case NV_ENC_ERR_UNSUPPORTED_PARAM:
        case NV_ENC_ERR_INVALID_VERSION:
            error = DXGI_ERROR_UNSUPPORTED;
            break;
        default:
            error = E_FAIL;
            break;
        }
        const char* detail = session && api.nvEncGetLastErrorString
            ? api.nvEncGetLastErrorString(session) : nullptr;
        spdlog::error("NVENC {} failed (status {}): {}", operation, static_cast<int>(status),
                      detail ? detail : "no driver detail");
        return false;
    }

    void clear() noexcept {
        if (copy_fence && copy_fence->GetCompletedValue() < copy_value) {
            context->Flush();
            const auto result = copy_fence->SetEventOnCompletion(copy_value, copy_completion);
            if (FAILED(result)) {
                fail(result, "set copy completion during shutdown");
            } else if (WaitForSingleObject(copy_completion, INFINITE) != WAIT_OBJECT_0) {
                fail(HRESULT_FROM_WIN32(GetLastError()), "wait for input copy during shutdown");
            }
        }
        bool completed = true;
        if (pending && completion) {
            const auto waited = WaitForSingleObject(completion, 5000);
            if (waited != WAIT_OBJECT_0) {
                fail(waited == WAIT_FAILED ? HRESULT_FROM_WIN32(GetLastError()) : HRESULT_FROM_WIN32(WAIT_TIMEOUT),
                     "wait for outstanding frame during shutdown");
                completed = false;
            }
        }
        if (session) {
            if (completed) {
                if (locked) check(api.nvEncUnlockBitstream(session, output), "unlock during shutdown");
                if (mapped) check(api.nvEncUnmapInputResource(session, mapped), "unmap during shutdown");
                if (registered) check(api.nvEncUnregisterResource(session, registered), "unregister input");
                if (output) check(api.nvEncDestroyBitstreamBuffer(session, output), "destroy output");
            }
            if (event_registered && completed) {
                NV_ENC_EVENT_PARAMS event{};
                event.version = NV_ENC_EVENT_PARAMS_VER;
                event.completionEvent = completion;
                check(api.nvEncUnregisterAsyncEvent(session, &event), "unregister event");
            }
            check(api.nvEncDestroyEncoder(session), "destroy session");
        }
        session = nullptr;
        registered = nullptr;
        mapped = nullptr;
        output = nullptr;
        locked = event_registered = pending = has_image = running = false;
        source = {};
        input.Reset();
        copy_fence.Reset();
        context4.Reset();
        context.Reset();
        device.Reset();
        if (completion) CloseHandle(completion);
        completion = nullptr;
        if (copy_completion) CloseHandle(copy_completion);
        copy_completion = nullptr;
        if (module) FreeLibrary(module);
        module = nullptr;
        api = {};
        sequence = 0;
        key_frame_requested = false;
        copy_value = 0;
    }

    bool initialize(const CapturedFrame& frame) {
        frame.texture->GetDevice(&device);
        device->GetImmediateContext(&context);
        Microsoft::WRL::ComPtr<ID3D11Device5> device5;
        auto result = device.As(&device5);
        if (FAILED(result)) return fail(result, "query D3D11 device fence support");
        result = context.As(&context4);
        if (FAILED(result)) return fail(result, "query D3D11 context fence support");
        result = device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&copy_fence));
        if (FAILED(result)) return fail(result, "create input copy fence");
        copy_completion = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!copy_completion) return fail(HRESULT_FROM_WIN32(GetLastError()), "create copy fence event");
        result = load_nvenc_api(module, api);
        if (FAILED(result)) return fail(result, "load driver API");
        if (!api.nvEncOpenEncodeSessionEx || !api.nvEncDestroyEncoder ||
            !api.nvEncGetEncodeCaps || !api.nvEncGetEncodeProfileGUIDCount ||
            !api.nvEncGetEncodeProfileGUIDs || !api.nvEncGetInputFormatCount ||
            !api.nvEncGetInputFormats || !api.nvEncGetEncodePresetConfigEx ||
            !api.nvEncInitializeEncoder || !api.nvEncCreateBitstreamBuffer ||
            !api.nvEncDestroyBitstreamBuffer || !api.nvEncRegisterResource ||
            !api.nvEncUnregisterResource || !api.nvEncMapInputResource ||
            !api.nvEncUnmapInputResource || !api.nvEncRegisterAsyncEvent ||
            !api.nvEncUnregisterAsyncEvent || !api.nvEncEncodePicture ||
            !api.nvEncLockBitstream || !api.nvEncUnlockBitstream) {
            return fail(E_NOINTERFACE, "incomplete driver function table");
        }
        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{};
        open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
        open.apiVersion = NVENCAPI_VERSION;
        open.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
        open.device = device.Get();
        if (!check(api.nvEncOpenEncodeSessionEx(&open, &session), "open D3D11 session")) return false;
        for (const auto capability : {NV_ENC_CAPS_SUPPORT_YUV444_ENCODE, NV_ENC_CAPS_ASYNC_ENCODE_SUPPORT}) {
            NV_ENC_CAPS_PARAM caps{};
            caps.version = NV_ENC_CAPS_PARAM_VER;
            caps.capsToQuery = capability;
            int supported{};
            if (!check(api.nvEncGetEncodeCaps(session, NV_ENC_CODEC_H264_GUID, &caps, &supported),
                       "query H.264 capability")) return false;
            if (!supported) return fail(DXGI_ERROR_UNSUPPORTED, "H.264 4:4:4 or async encoding unsupported");
        }
        std::uint32_t count{}, written{};
        if (!check(api.nvEncGetEncodeProfileGUIDCount(session, NV_ENC_CODEC_H264_GUID, &count),
                   "count H.264 profiles")) return false;
        std::vector<GUID> profiles(count);
        if (!count) return fail(DXGI_ERROR_UNSUPPORTED, "no H.264 profiles");
        if (!check(api.nvEncGetEncodeProfileGUIDs(session, NV_ENC_CODEC_H264_GUID,
                   profiles.data(), count, &written), "query H.264 profiles")) return false;
        if (written > count) return fail(E_UNEXPECTED, "invalid profile count");
        profiles.resize(written);
        if (std::ranges::none_of(profiles, [](const GUID& guid) {
            return IsEqualGUID(guid, NV_ENC_H264_PROFILE_HIGH_444_GUID);
        })) return fail(DXGI_ERROR_UNSUPPORTED, "High 4:4:4 Predictive profile unavailable");
        if (!check(api.nvEncGetInputFormatCount(session, NV_ENC_CODEC_H264_GUID, &count),
                   "count input formats")) return false;
        std::vector<NV_ENC_BUFFER_FORMAT> formats(count);
        if (!count) return fail(DXGI_ERROR_UNSUPPORTED, "no H.264 input formats");
        if (!check(api.nvEncGetInputFormats(session, NV_ENC_CODEC_H264_GUID,
                   formats.data(), count, &written), "query input formats")) return false;
        if (written > count) return fail(E_UNEXPECTED, "invalid input format count");
        formats.resize(written);
        if (std::ranges::find(formats, NV_ENC_BUFFER_FORMAT_ARGB) == formats.end()) {
            return fail(DXGI_ERROR_UNSUPPORTED, "GPU BGRA/ARGB input unsupported");
        }
        NV_ENC_PRESET_CONFIG preset{};
        preset.version = NV_ENC_PRESET_CONFIG_VER;
        preset.presetCfg.version = NV_ENC_CONFIG_VER;
        if (!check(api.nvEncGetEncodePresetConfigEx(session, NV_ENC_CODEC_H264_GUID,
                   NV_ENC_PRESET_P1_GUID, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset),
                   "query ultra-low-latency preset")) return false;
        config = preset.presetCfg;
        config.profileGUID = NV_ENC_H264_PROFILE_HIGH_444_GUID;
        config.gopLength = 60;
        config.frameIntervalP = 1;
        config.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;
        config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
        set_bitrate(config, options.target_bitrate_kbps);
        config.rcParams.enableLookahead = 0;
        config.rcParams.lookaheadDepth = 0;
        config.rcParams.enableTemporalAQ = 0;
        config.rcParams.zeroReorderDelay = 1;
        config.rcParams.multiPass = NV_ENC_MULTI_PASS_DISABLED;
        auto& h264 = config.encodeCodecConfig.h264Config;
        h264.chromaFormatIDC = 3;
        h264.idrPeriod = config.gopLength;
        h264.maxNumRefFrames = 1;
        h264.repeatSPSPPS = 1;
        h264.outputAUD = 1;
        init = {};
        init.version = NV_ENC_INITIALIZE_PARAMS_VER;
        init.encodeGUID = NV_ENC_CODEC_H264_GUID;
        init.presetGUID = NV_ENC_PRESET_P1_GUID;
        init.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
        init.encodeWidth = init.darWidth = options.width;
        init.encodeHeight = init.darHeight = options.height;
        init.frameRateNum = options.frame_rate;
        init.frameRateDen = 1;
        init.enablePTD = 1;
        init.enableEncodeAsync = 1;
        init.encodeConfig = &config;
        const auto initialized = api.nvEncInitializeEncoder(session, &init);
        if (!check(initialized, "initialize H.264 High444")) {
            if (initialized == NV_ENC_ERR_INVALID_PARAM) error = DXGI_ERROR_UNSUPPORTED;
            return false;
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = options.width;
        desc.Height = options.height;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        result = device->CreateTexture2D(&desc, nullptr, &input);
        if (FAILED(result)) return fail(result, "create owned BGRA input");
        NV_ENC_REGISTER_RESOURCE resource{};
        resource.version = NV_ENC_REGISTER_RESOURCE_VER;
        resource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        resource.resourceToRegister = input.Get();
        resource.width = options.width;
        resource.height = options.height;
        resource.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
        resource.bufferUsage = NV_ENC_INPUT_IMAGE;
        if (!check(api.nvEncRegisterResource(session, &resource), "register D3D11 input")) return false;
        registered = resource.registeredResource;
        NV_ENC_CREATE_BITSTREAM_BUFFER buffer{};
        buffer.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        if (!check(api.nvEncCreateBitstreamBuffer(session, &buffer), "create bitstream buffer")) return false;
        output = buffer.bitstreamBuffer;
        completion = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!completion) return fail(HRESULT_FROM_WIN32(GetLastError()), "create completion event");
        NV_ENC_EVENT_PARAMS event{};
        event.version = NV_ENC_EVENT_PARAMS_VER;
        event.completionEvent = completion;
        if (!check(api.nvEncRegisterAsyncEvent(session, &event), "register completion event")) return false;
        event_registered = true;
        spdlog::info("NVENC D3D11: H.264 High 4:4:4 Predictive, 8-bit 4:4:4; GPU BGRA input; "
                     "CBR, GOP 60 I/P, no B-frames/lookahead, one-frame VBV");
        return true;
    }

    // CBR with a one-frame VBV.
    void set_bitrate(NV_ENC_CONFIG& target, std::uint32_t kbps) const {
        target.rcParams.averageBitRate = kbps * 1000;
        target.rcParams.maxBitRate = target.rcParams.averageBitRate;
        target.rcParams.vbvBufferSize = std::max(1u, target.rcParams.averageBitRate / options.frame_rate);
        target.rcParams.vbvInitialDelay = target.rcParams.vbvBufferSize;
    }

    // Applies a new rate-control target to the open session without an IDR or new SPS/PPS.
    bool reconfigure_bitrate(std::uint32_t kbps) {
        if (!api.nvEncReconfigureEncoder || pending) return false;
        auto updated = config;
        set_bitrate(updated, kbps);
        NV_ENC_RECONFIGURE_PARAMS reconfigure{};
        reconfigure.version = NV_ENC_RECONFIGURE_PARAMS_VER;
        reconfigure.reInitEncodeParams = init;
        reconfigure.reInitEncodeParams.encodeConfig = &updated;
        reconfigure.resetEncoder = 0;
        reconfigure.forceIDR = 0;
        const auto status = api.nvEncReconfigureEncoder(session, &reconfigure);
        if (status != NV_ENC_SUCCESS) {
            // Not fatal: the caller falls back to a full reconfiguration with a new session.
            const char* detail = api.nvEncGetLastErrorString ? api.nvEncGetLastErrorString(session) : nullptr;
            spdlog::warn("NVENC in-place bitrate change rejected (status {}): {}", static_cast<int>(status),
                         detail ? detail : "no driver detail");
            return false;
        }
        config = updated;
        return true;
    }

    EncoderSubmitResult submit() {
        if (!ResetEvent(completion)) {
            fail(HRESULT_FROM_WIN32(GetLastError()), "reset completion event");
            return EncoderSubmitResult::Failed;
        }
        NV_ENC_MAP_INPUT_RESOURCE map{};
        map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
        map.registeredResource = registered;
        if (!check(api.nvEncMapInputResource(session, &map), "map D3D11 input")) {
            return EncoderSubmitResult::Failed;
        }
        mapped = map.mappedResource;
        NV_ENC_PIC_PARAMS pic{};
        pic.version = NV_ENC_PIC_PARAMS_VER;
        pic.inputBuffer = mapped;
        pic.bufferFmt = map.mappedBufferFmt;
        pic.inputWidth = options.width;
        pic.inputHeight = options.height;
        pic.outputBitstream = output;
        pic.completionEvent = completion;
        pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        pic.inputTimeStamp = sequence;
        pic.frameIdx = static_cast<std::uint32_t>(sequence);
        if (key_frame_requested) {
            pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
        }
        if (!check(api.nvEncEncodePicture(session, &pic), "submit picture")) {
            return EncoderSubmitResult::Failed;
        }
        ++sequence;
        key_frame_requested = false;
        pending = true;
        return EncoderSubmitResult::Accepted;
    }
};

NvencVideoEncoder::NvencVideoEncoder() : impl_(std::make_unique<Impl>()) {}
NvencVideoEncoder::~NvencVideoEncoder() { impl_->clear(); }
std::wstring_view NvencVideoEncoder::backend_name() const noexcept { return L"NVENC DirectX 11"; }

bool NvencVideoEncoder::configure(const EncoderOptions& options) {
    impl_->clear();
    impl_->configured = false;
    impl_->error = S_OK;
    if (!options.width || !options.height || !options.frame_rate || !options.target_bitrate_kbps ||
        options.target_bitrate_kbps > std::numeric_limits<std::uint32_t>::max() / 1000 ||
        options.codec != VideoCodec::H264) {
        return impl_->fail(E_INVALIDARG, "configuration (H.264 only; dimensions, FPS and bitrate must be nonzero)");
    }
    impl_->options = options;
    impl_->configured = true;
    return true;
}

bool NvencVideoEncoder::start() {
    if (!impl_->configured || FAILED(impl_->error)) return impl_->fail(E_UNEXPECTED, "start before configuration");
    impl_->running = true;
    return true;
}

bool NvencVideoEncoder::prepare(const CapturedFrame& frame) {
    if (!impl_->running || FAILED(impl_->error)) {
        impl_->fail(E_UNEXPECTED, "submit while stopped or failed");
        return false;
    }
    if (!frame.texture) {
        impl_->fail(E_INVALIDARG, "null capture texture");
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    frame.texture->GetDesc(&desc);
    if (desc.Width != impl_->options.width || desc.Height != impl_->options.height ||
        desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM || desc.SampleDesc.Count != 1 ||
        desc.MipLevels != 1 || desc.ArraySize != 1) {
        impl_->fail(E_INVALIDARG, "input must be a matching single-sample BGRA8 texture");
        return false;
    }
    if (!impl_->session && !impl_->initialize(frame)) return false;
    Microsoft::WRL::ComPtr<ID3D11Device> source_device;
    frame.texture->GetDevice(&source_device);
    if (source_device.Get() != impl_->device.Get()) {
        impl_->fail(E_INVALIDARG, "capture device changed; reconfigure required");
        return false;
    }
    return true;
}

EncoderSubmitResult NvencVideoEncoder::try_submit(const CapturedFrame& frame) {
    if (impl_->pending && impl_->running && SUCCEEDED(impl_->error)) return EncoderSubmitResult::WouldBlock;
    if (!prepare(frame)) return EncoderSubmitResult::Failed;
    impl_->source = frame;
    impl_->timestamp = frame.captured_at;
    impl_->context->CopyResource(impl_->input.Get(), frame.texture.Get());
    const auto copied = impl_->context4->Signal(impl_->copy_fence.Get(), impl_->copy_value + 1);
    if (FAILED(copied)) {
        impl_->fail(copied, "signal input copy");
        return EncoderSubmitResult::Failed;
    }
    ++impl_->copy_value;
    impl_->has_image = true;
    return impl_->submit();
}

void NvencVideoEncoder::request_key_frame() noexcept {
    impl_->key_frame_requested = true;
}

bool NvencVideoEncoder::try_update_bitrate(std::uint32_t bitrate_kbps) {
    if (!impl_->configured || FAILED(impl_->error) || !bitrate_kbps ||
        bitrate_kbps > std::numeric_limits<std::uint32_t>::max() / 1000) {
        return false;
    }
    // Before deferred initialization the session is simply opened with the new bitrate.
    if (impl_->session && !impl_->reconfigure_bitrate(bitrate_kbps)) return false;
    impl_->options.target_bitrate_kbps = bitrate_kbps;
    return true;
}

EncoderSubmitResult NvencVideoEncoder::try_repeat_frame() {
    if (!impl_->running || FAILED(impl_->error) || !impl_->has_image) {
        impl_->fail(E_UNEXPECTED, "repeat without a running encoder and previous image");
        return EncoderSubmitResult::Failed;
    }
    if (impl_->pending) return EncoderSubmitResult::WouldBlock;
    return impl_->submit();
}

std::optional<EncodedPacket> NvencVideoEncoder::try_receive() {
    if (!impl_->pending || FAILED(impl_->error)) return std::nullopt;
    const auto waited = WaitForSingleObject(impl_->completion, 0);
    if (waited == WAIT_TIMEOUT) return std::nullopt;
    if (waited != WAIT_OBJECT_0) {
        impl_->fail(HRESULT_FROM_WIN32(GetLastError()), "poll completion");
        return std::nullopt;
    }
    NV_ENC_LOCK_BITSTREAM lock{};
    lock.version = NV_ENC_LOCK_BITSTREAM_VER;
    lock.outputBitstream = impl_->output;
    lock.doNotWait = 1;
    if (!impl_->check(impl_->api.nvEncLockBitstream(impl_->session, &lock), "lock output")) return std::nullopt;
    impl_->locked = true;
    if (!lock.bitstreamSizeInBytes || !lock.bitstreamBufferPtr) {
        impl_->fail(E_UNEXPECTED, "empty encoded picture");
        return std::nullopt;
    }
    EncodedPacket packet;
    packet.codec = VideoCodec::H264;
    packet.captured_at = impl_->timestamp;
    packet.key_frame = lock.pictureType == NV_ENC_PIC_TYPE_IDR;
    packet.data.resize(lock.bitstreamSizeInBytes);
    std::memcpy(packet.data.data(), lock.bitstreamBufferPtr, packet.data.size());
    if (!impl_->check(impl_->api.nvEncUnlockBitstream(impl_->session, impl_->output), "unlock output")) {
        return std::nullopt;
    }
    impl_->locked = false;
    if (!impl_->check(impl_->api.nvEncUnmapInputResource(impl_->session, impl_->mapped), "unmap input")) {
        return std::nullopt;
    }
    impl_->mapped = nullptr;
    impl_->pending = false;
    impl_->source = {};
    return packet;
}

void NvencVideoEncoder::stop() noexcept { impl_->clear(); }
bool NvencVideoEncoder::is_running() const noexcept { return impl_->running; }
HRESULT NvencVideoEncoder::last_error() const noexcept { return impl_->error; }

} // namespace beamit::media
