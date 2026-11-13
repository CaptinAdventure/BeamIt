#include "VideoPipeline.hpp"

#include <windows.h>
#include <strmif.h>
#include <codecapi.h>
#include <d3d11_4.h>
#include <dxgi1_3.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <format>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

namespace beamit::client {
namespace {

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

constexpr std::size_t max_queued_frames = 8;

struct HResultError : std::runtime_error {
    HResultError(HRESULT code_, std::string_view operation)
        : std::runtime_error(std::format("{} failed (HRESULT 0x{:08X})", operation, static_cast<std::uint32_t>(code_))),
          code(code_) {}
    HRESULT code;
};

void check(HRESULT result, std::string_view operation) {
    if (FAILED(result)) throw HResultError(result, operation);
}

std::string narrow(std::wstring_view text) {
    if (text.empty()) return {};
    const auto size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(std::max(size, 0)), '\0');
    if (size > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    }
    return result;
}

struct OutputFormat {
    GUID subtype;
    DXGI_FORMAT format;
    const char* name;
};

const OutputFormat output_formats[]{
    {MFVideoFormat_NV12, DXGI_FORMAT_NV12, "NV12"},
    {MFVideoFormat_P010, DXGI_FORMAT_P010, "P010"},
    {MFVideoFormat_AYUV, DXGI_FORMAT_AYUV, "AYUV"},
    {MFVideoFormat_Y410, DXGI_FORMAT_Y410, "Y410"},
    {MFVideoFormat_Y416, DXGI_FORMAT_Y416, "Y416"},
    {MFVideoFormat_YUY2, DXGI_FORMAT_YUY2, "YUY2"},
    {MFVideoFormat_Y210, DXGI_FORMAT_Y210, "Y210"},
    {MFVideoFormat_P016, DXGI_FORMAT_P016, "P016"},
    {MFVideoFormat_ARGB32, DXGI_FORMAT_B8G8R8A8_UNORM, "BGRA"},
    {MFVideoFormat_RGB32, DXGI_FORMAT_B8G8R8X8_UNORM, "BGRX"},
};

struct Frame {
    std::uint32_t generation{};
    std::uint64_t capture_time_us{};
    bool key_frame{};
    std::vector<std::byte> payload;
};

struct Picture {
    ComPtr<IMFSample> sample; // Keeps the decoder surface from being reused while presented.
    ComPtr<ID3D11Texture2D> texture;
    UINT subresource{};
    std::uint64_t capture_time_us{};
};

class Decoder {
public:
    Decoder(IMFActivate* activation, std::wstring name, IMFDXGIDeviceManager* manager, const VideoParameters& video,
            const std::function<bool(DXGI_FORMAT)>& supported)
        : activation_(activation), name_(std::move(name)) {
        check(activation_->ActivateObject(IID_PPV_ARGS(&transform_)), "Activate decoder");
        try {
            ComPtr<IMFAttributes> attributes;
            check(transform_->GetAttributes(&attributes), "Get decoder attributes");
            UINT32 d3d_aware{};
            if (SUCCEEDED(attributes->GetUINT32(MF_SA_D3D11_AWARE, &d3d_aware)) && d3d_aware) {
                check(transform_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(manager)),
                      "Attach decoder D3D11 device");
                gpu_ = true;
            }
            (void)attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
            if (ComPtr<ICodecAPI> codec; SUCCEEDED(transform_.As(&codec))) {
                VARIANT value{};
                value.vt = VT_UI4;
                value.ulVal = TRUE;
                (void)codec->SetValue(&CODECAPI_AVLowLatencyMode, &value);
            }
            ComPtr<IMFMediaType> input;
            check(MFCreateMediaType(&input), "Create decoder input type");
            check(input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Set input major type");
            check(input->SetGUID(MF_MT_SUBTYPE, video.codec == "h265" ? MFVideoFormat_HEVC : MFVideoFormat_H264),
                  "Set input codec");
            check(input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "Set progressive input");
            check(MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, video.width, video.height), "Set input size");
            if (video.frame_rate) {
                check(MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, *video.frame_rate, 1), "Set input frame rate");
            }
            check(transform_->SetInputType(0, input.Get(), 0), "Set decoder input type");
            negotiate(supported);
            check(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "Begin decoder streaming");
            check(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "Start decoder stream");
        } catch (...) {
            transform_.Reset();
            (void)activation_->ShutdownObject();
            throw;
        }
    }

    ~Decoder() {
        if (transform_) {
            (void)transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
            transform_.Reset();
        }
        (void)activation_->ShutdownObject();
    }
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    // Returns the decoded picture, or nothing if the decoder needs more input.
    std::optional<Picture> decode(const Frame& frame, ID3D11Device* device, ID3D11DeviceContext* context) {
        ComPtr<IMFMediaBuffer> buffer;
        check(MFCreateMemoryBuffer(static_cast<DWORD>(frame.payload.size()), &buffer), "Create input buffer");
        BYTE* destination{};
        check(buffer->Lock(&destination, nullptr, nullptr), "Lock input buffer");
        std::memcpy(destination, frame.payload.data(), frame.payload.size());
        check(buffer->Unlock(), "Unlock input buffer");
        check(buffer->SetCurrentLength(static_cast<DWORD>(frame.payload.size())), "Set input length");
        ComPtr<IMFSample> sample;
        check(MFCreateSample(&sample), "Create input sample");
        check(sample->AddBuffer(buffer.Get()), "Attach input buffer");
        check(sample->SetSampleTime(static_cast<LONGLONG>(frame.capture_time_us * 10)), "Set sample time");
        if (frame.key_frame) check(sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE), "Mark key frame");
        check(transform_->ProcessInput(0, sample.Get(), 0), "Decode input");

        for (int attempt = 0; attempt < 4; ++attempt) {
            MFT_OUTPUT_STREAM_INFO info{};
            check(transform_->GetOutputStreamInfo(0, &info), "Get output requirements");
            ComPtr<IMFSample> provided;
            if ((info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) == 0) {
                check(MFCreateSample(&provided), "Create output sample");
                ComPtr<IMFMediaBuffer> memory;
                check(MFCreateAlignedMemoryBuffer(info.cbSize, info.cbAlignment ? info.cbAlignment - 1 : 0, &memory),
                      "Allocate output buffer");
                check(provided->AddBuffer(memory.Get()), "Attach output buffer");
            }
            MFT_OUTPUT_DATA_BUFFER output{};
            output.pSample = provided.Get();
            DWORD status{};
            const auto result = transform_->ProcessOutput(0, 1, &output, &status);
            ComPtr<IMFCollection> events;
            events.Attach(output.pEvents);
            ComPtr<IMFSample> decoded;
            if (output.pSample && output.pSample != provided.Get()) decoded.Attach(output.pSample);
            else decoded = provided;
            if (result == MF_E_TRANSFORM_NEED_MORE_INPUT) return std::nullopt;
            if (result == MF_E_TRANSFORM_STREAM_CHANGE) {
                negotiate(supported_);
                continue;
            }
            check(result, "Decode output");
            return picture(decoded.Get(), frame.capture_time_us, device, context);
        }
        throw std::runtime_error("The decoder kept changing its output format");
    }

    [[nodiscard]] const std::wstring& name() const { return name_; }
    [[nodiscard]] bool gpu() const { return gpu_; }
    [[nodiscard]] const OutputFormat& format() const { return *format_; }
    [[nodiscard]] UINT32 coded_width() const { return coded_width_; }
    [[nodiscard]] UINT32 coded_height() const { return coded_height_; }

private:
    void negotiate(const std::function<bool(DXGI_FORMAT)>& supported) {
        supported_ = supported;
        for (DWORD index = 0;; ++index) {
            ComPtr<IMFMediaType> type;
            const auto result = transform_->GetOutputAvailableType(0, index, &type);
            if (result == MF_E_NO_MORE_TYPES) throw std::runtime_error("The decoder offers no presentable output format");
            check(result, "Get decoder output type");
            GUID subtype{};
            check(type->GetGUID(MF_MT_SUBTYPE, &subtype), "Get output subtype");
            const auto match = std::ranges::find_if(output_formats, [&](const OutputFormat& f) { return f.subtype == subtype; });
            if (match == std::end(output_formats) || !supported(match->format)) continue;
            if (FAILED(transform_->SetOutputType(0, type.Get(), 0))) continue;
            check(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &coded_width_, &coded_height_), "Get output size");
            format_ = &*match;
            return;
        }
    }

    Picture picture(IMFSample* sample, std::uint64_t capture_time_us, ID3D11Device* device, ID3D11DeviceContext* context) {
        if (!sample) throw std::runtime_error("The decoder returned no picture");
        ComPtr<IMFMediaBuffer> buffer;
        check(sample->GetBufferByIndex(0, &buffer), "Get decoded buffer");
        Picture result{sample, {}, 0, capture_time_us};
        if (ComPtr<IMFDXGIBuffer> gpu; SUCCEEDED(buffer.As(&gpu))) {
            check(gpu->GetResource(IID_PPV_ARGS(&result.texture)), "Get decoded texture");
            check(gpu->GetSubresourceIndex(&result.subresource), "Get decoded subresource");
            return result;
        }
        // Software output: upload into a texture the video processor can read.
        if (!upload_) {
            const D3D11_TEXTURE2D_DESC description{coded_width_, coded_height_, 1, 1, format_->format, {1, 0},
                                                   D3D11_USAGE_DEFAULT, D3D11_BIND_DECODER, 0, 0};
            check(device->CreateTexture2D(&description, nullptr, &upload_), "Create upload texture");
        }
        ComPtr<IMF2DBuffer> planes;
        BYTE* data{};
        LONG pitch{};
        if (SUCCEEDED(buffer.As(&planes)) && SUCCEEDED(planes->Lock2D(&data, &pitch))) {
            context->UpdateSubresource(upload_.Get(), 0, nullptr, data, static_cast<UINT>(pitch), 0);
            (void)planes->Unlock2D();
        } else {
            DWORD length{};
            check(buffer->Lock(&data, nullptr, &length), "Lock decoded buffer");
            const auto row = length / (format_->format == DXGI_FORMAT_NV12 ? coded_height_ * 3 / 2 : coded_height_);
            context->UpdateSubresource(upload_.Get(), 0, nullptr, data, row, 0);
            (void)buffer->Unlock();
        }
        result.texture = upload_;
        return result;
    }

    ComPtr<IMFActivate> activation_;
    std::wstring name_;
    ComPtr<IMFTransform> transform_;
    std::function<bool(DXGI_FORMAT)> supported_;
    const OutputFormat* format_{};
    UINT32 coded_width_{};
    UINT32 coded_height_{};
    bool gpu_{};
    ComPtr<ID3D11Texture2D> upload_;
};

std::vector<std::pair<ComPtr<IMFActivate>, std::wstring>> decoder_candidates(bool hevc) {
    MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264};
    IMFActivate** values{};
    UINT32 count{};
    // Synchronous transforms: the Microsoft decoders use DXVA on the supplied D3D11 device.
    check(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                    &input, nullptr, &values, &count), "Enumerate decoders");
    std::vector<std::pair<ComPtr<IMFActivate>, std::wstring>> result;
    for (UINT32 index = 0; index < count; ++index) {
        ComPtr<IMFActivate> activation;
        activation.Attach(values[index]);
        wchar_t* name{};
        UINT32 length{};
        std::wstring text = L"Unnamed decoder";
        if (SUCCEEDED(activation->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &length))) {
            text.assign(name, length);
            CoTaskMemFree(name);
        }
        result.emplace_back(std::move(activation), std::move(text));
    }
    CoTaskMemFree(values);
    return result;
}

} // namespace

struct VideoPipeline::Impl {
    explicit Impl(Callbacks callbacks_) : callbacks(std::move(callbacks_)) {
        check(MFStartup(MF_VERSION, MFSTARTUP_LITE), "MFStartup");
        constexpr D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels,
                                static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &device, nullptr, &context),
              "Create D3D11 device");
        ComPtr<ID3D11Multithread> multithread;
        check(context.As(&multithread), "Get multithread protection");
        multithread->SetMultithreadProtected(TRUE);
        check(device.As(&video_device), "Get video device");
        check(context.As(&video_context), "Get video context");
        UINT token{};
        check(MFCreateDXGIDeviceManager(&token, &manager), "Create DXGI device manager");
        check(manager->ResetDevice(device.Get(), token), "Set DXGI device");

        ComPtr<IDXGIDevice> dxgi_device;
        check(device.As(&dxgi_device), "Get DXGI device");
        ComPtr<IDXGIAdapter> adapter;
        check(dxgi_device->GetAdapter(&adapter), "Get adapter");
        DXGI_ADAPTER_DESC adapter_description{};
        check(adapter->GetDesc(&adapter_description), "Describe adapter");
        adapter_name = narrow(adapter_description.Description);
        ComPtr<IDXGIFactory2> factory;
        check(adapter->GetParent(IID_PPV_ARGS(&factory)), "Get DXGI factory");
        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Width = width = 64;
        description.Height = height = 64;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2;
        description.Scaling = DXGI_SCALING_STRETCH;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        description.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        check(factory->CreateSwapChainForComposition(device.Get(), &description, nullptr, &swap), "Create swap chain");
        ComPtr<IDXGISwapChain2> swap2;
        check(swap.As(&swap2), "Get waitable swap chain");
        check(swap2->SetMaximumFrameLatency(1), "Set frame latency");
        waitable = swap2->GetFrameLatencyWaitableObject();
        clear();
        thread = std::thread([this] { run(); });
    }

    ~Impl() {
        {
            std::scoped_lock lock(mutex);
            stopping = true;
        }
        wake.notify_all();
        thread.join();
        if (waitable) CloseHandle(waitable);
        picture.reset();
        input_views.clear();
        decoder.reset();
        (void)MFShutdown();
    }

    void run() {
        (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        try {
            loop();
        } catch (const std::exception& error) {
            fail(std::string("Video pipeline failed: ") + error.what());
        }
        decoder.reset();
        CoUninitialize();
    }

    void loop() {
        for (;;) {
            std::optional<Frame> frame;
            std::optional<std::pair<unsigned, unsigned>> size;
            std::optional<VideoParameters> parameters;
            std::vector<std::function<void()>> work;
            {
                std::unique_lock lock(mutex);
                const auto ready = [&] { return stopping || !queue.empty() || pending_size || pending_parameters || !tasks.empty(); };
                if (present_pending) wake.wait_for(lock, std::chrono::milliseconds(1), ready);
                else wake.wait(lock, ready);
                if (stopping) return;
                if (pending_parameters) parameters = std::exchange(pending_parameters, std::nullopt);
                if (pending_size) size = std::exchange(pending_size, std::nullopt);
                work = std::exchange(tasks, {});
                if (!queue.empty() && !parameters) {
                    frame = std::move(queue.front());
                    queue.pop_front();
                }
            }
            for (auto& task : work) task();
            if (parameters) reconfigure(*parameters);
            if (size) apply_size(size->first, size->second);
            if (frame && !failed) decode(std::move(*frame));
            if (present_pending && WaitForSingleObjectEx(waitable, 0, FALSE) == WAIT_OBJECT_0) present();
        }
    }

    void reconfigure(const VideoParameters& parameters) {
        std::scoped_lock lock(stats_mutex);
        if (video && parameters.generation <= video->generation) return;
        const bool restart = !video || parameters.codec != video->codec || parameters.width != video->width ||
                             parameters.height != video->height;
        video = parameters;
        current.generation = parameters.generation;
        current.width = parameters.width;
        current.height = parameters.height;
        if (restart) {
            input_views.clear();
            decoder.reset();
        }
        waiting_for_key = true;
    }

    void decode(Frame frame) {
        if (!video || frame.generation != video->generation) return;
        if (waiting_for_key && !frame.key_frame) {
            std::scoped_lock lock(stats_mutex);
            ++current.dropped;
            return;
        }
        const auto started = Clock::now();
        try {
            if (!decoder) {
                // Candidate selection decodes this key frame with the decoder that accepts it.
                create_decoder(frame);
                waiting_for_key = false;
                consecutive_errors = 0;
                return;
            }
            auto output = decoder->decode(frame, device.Get(), context.Get());
            waiting_for_key = false;
            consecutive_errors = 0;
            const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
            std::scoped_lock lock(stats_mutex);
            current.decode_ms_average = current.decoded == 0 ? elapsed : current.decode_ms_average * 0.95 + elapsed * 0.05;
            current.decode_ms_max = std::max(current.decode_ms_max, elapsed);
            if (output) {
                ++current.decoded;
                if (present_pending) ++current.coalesced;
                picture = std::move(*output);
                current.last_capture_time_us = picture->capture_time_us;
                present_pending = true;
            }
        } catch (const std::exception& error) {
            if (failed) return;
            // A device loss or repeated failures on key frames means the stream cannot be decoded here.
            const auto* hresult = dynamic_cast<const HResultError*>(&error);
            const bool device_lost = hresult && (hresult->code == DXGI_ERROR_DEVICE_REMOVED ||
                                                 hresult->code == DXGI_ERROR_DEVICE_RESET);
            if (device_lost || !decoder || ++consecutive_errors >= 3) {
                fail(describe_failure(error.what(), device_lost));
                return;
            }
            OutputDebugStringA((std::string("BeamIt decoder error, requesting key frame: ") + error.what() + "\n").c_str());
            input_views.clear();
            decoder.reset();
            waiting_for_key = true;
            if (callbacks.request_key_frame) callbacks.request_key_frame();
        }
    }

    void create_decoder(const Frame& first) {
        const auto supported = [this](DXGI_FORMAT format) {
            UINT support{};
            return enumerator_for(video->width, video->height) &&
                   SUCCEEDED(enumerator->CheckVideoProcessorFormat(format, &support)) &&
                   (support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) != 0;
        };
        std::string rejected;
        for (auto& [activation, name] : decoder_candidates(video->codec == "h265")) {
            try {
                auto candidate = std::make_unique<Decoder>(activation.Get(), name, manager.Get(), *video, supported);
                auto output = candidate->decode(first, device.Get(), context.Get());
                decoder = std::move(candidate);
                {
                    std::scoped_lock lock(stats_mutex);
                    current.decoder = narrow(decoder->name());
                    current.output = decoder->gpu() ? "GPU surface (D3D11 on " + adapter_name + ")" : "system memory upload";
                    current.output_format = decoder->format().name;
                }
                if (output) {
                    std::scoped_lock lock(stats_mutex);
                    ++current.decoded;
                    picture = std::move(*output);
                    current.last_capture_time_us = picture->capture_time_us;
                    present_pending = true;
                }
                return;
            } catch (const std::exception& error) {
                rejected += "\n" + narrow(name) + ": " + error.what();
            }
        }
        throw std::runtime_error("No installed Media Foundation decoder accepted the stream." + rejected);
    }

    std::string describe_failure(const std::string& reason, bool device_lost) const {
        if (device_lost) return "The GPU device was lost while decoding: " + reason;
        return std::format("This stream cannot be decoded on this computer: {} {} {}-bit ({}). {}",
                           video->codec == "h265" ? "HEVC" : "H.264",
                           video->chroma_format.empty() ? std::string("4:2:0") : video->chroma_format, video->bit_depth,
                           video->codec_string.empty() ? std::string("unknown profile") : video->codec_string, reason);
    }

    void fail(std::string message) {
        failed = true;
        {
            std::scoped_lock lock(mutex);
            queue.clear();
        }
        if (callbacks.failed) callbacks.failed(message);
    }

    bool enumerator_for(UINT input_width, UINT input_height) {
        if (enumerator && enumerator_input == std::pair{input_width, input_height} &&
            enumerator_output == std::pair{width, height}) {
            return true;
        }
        input_views.clear();
        processor.Reset();
        enumerator.Reset();
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputFrameRate = {60, 1};
        content.InputWidth = input_width;
        content.InputHeight = input_height;
        content.OutputFrameRate = {60, 1};
        content.OutputWidth = width;
        content.OutputHeight = height;
        content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        if (FAILED(video_device->CreateVideoProcessorEnumerator(&content, &enumerator))) return false;
        check(video_device->CreateVideoProcessor(enumerator.Get(), 0, &processor), "Create video processor");
        enumerator_input = {input_width, input_height};
        enumerator_output = {width, height};
        output_view.Reset();
        return true;
    }

    void apply_size(unsigned new_width, unsigned new_height) {
        new_width = std::clamp(new_width, 1U, 16384U);
        new_height = std::clamp(new_height, 1U, 16384U);
        if (new_width == width && new_height == height) return;
        output_view.Reset();
        context->Flush();
        check(swap->ResizeBuffers(0, new_width, new_height, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT),
              "Resize swap chain");
        width = new_width;
        height = new_height;
        if (picture) present_pending = true;
        else clear();
    }

    void clear() {
        ComPtr<ID3D11Texture2D> back;
        check(swap->GetBuffer(0, IID_PPV_ARGS(&back)), "Get back buffer");
        ComPtr<ID3D11RenderTargetView> target;
        check(device->CreateRenderTargetView(back.Get(), nullptr, &target), "Create render target");
        constexpr float black[4]{0, 0, 0, 1};
        context->ClearRenderTargetView(target.Get(), black);
        check(swap->Present(0, 0), "Present");
    }

    ID3D11VideoProcessorInputView* input_view(ID3D11Texture2D* texture, UINT subresource) {
        const auto key = std::pair{texture, subresource};
        if (const auto found = input_views.find(key); found != input_views.end()) return found->second.Get();
        if (input_views.size() > 64) input_views.clear();
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC description{};
        description.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        description.Texture2D.ArraySlice = subresource;
        ComPtr<ID3D11VideoProcessorInputView> view;
        check(video_device->CreateVideoProcessorInputView(texture, enumerator.Get(), &description, &view),
              "Create video input view");
        return input_views.emplace(key, std::move(view)).first->second.Get();
    }

    RECT letterbox(UINT target_width, UINT target_height) const {
        const auto scale = std::min(static_cast<double>(target_width) / video->width,
                                    static_cast<double>(target_height) / video->height);
        const auto w = static_cast<LONG>(std::lround(video->width * scale));
        const auto h = static_cast<LONG>(std::lround(video->height * scale));
        const auto x = (static_cast<LONG>(target_width) - w) / 2;
        const auto y = (static_cast<LONG>(target_height) - h) / 2;
        return {x, y, x + w, y + h};
    }

    void blit(ID3D11VideoProcessorOutputView* target, UINT target_width, UINT target_height) {
        const RECT source{0, 0, static_cast<LONG>(std::min(video->width, decoder ? decoder->coded_width() : video->width)),
                          static_cast<LONG>(std::min(video->height, decoder ? decoder->coded_height() : video->height))};
        const RECT destination = letterbox(target_width, target_height);
        const RECT full{0, 0, static_cast<LONG>(target_width), static_cast<LONG>(target_height)};
        const D3D11_VIDEO_COLOR black{.RGBA = {0, 0, 0, 1}};
        video_context->VideoProcessorSetOutputBackgroundColor(processor.Get(), FALSE, &black);
        video_context->VideoProcessorSetOutputTargetRect(processor.Get(), TRUE, &full);
        video_context->VideoProcessorSetStreamFrameFormat(processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        video_context->VideoProcessorSetStreamSourceRect(processor.Get(), 0, TRUE, &source);
        video_context->VideoProcessorSetStreamDestRect(processor.Get(), 0, TRUE, &destination);
        video_context->VideoProcessorSetStreamAutoProcessingMode(processor.Get(), 0, FALSE);
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = input_view(picture->texture.Get(), picture->subresource);
        check(video_context->VideoProcessorBlt(processor.Get(), target, 0, 1, &stream), "Video processor blit");
    }

    void present() {
        present_pending = false;
        if (!picture || !video || !decoder) return;
        if (!enumerator_for(decoder->coded_width(), decoder->coded_height())) {
            fail("The GPU video processor cannot present this stream.");
            return;
        }
        if (!output_view) {
            ComPtr<ID3D11Texture2D> back;
            check(swap->GetBuffer(0, IID_PPV_ARGS(&back)), "Get back buffer");
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC description{};
            description.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            check(video_device->CreateVideoProcessorOutputView(back.Get(), enumerator.Get(), &description, &output_view),
                  "Create video output view");
        }
        blit(output_view.Get(), width, height);
        const auto result = swap->Present(0, 0);
        if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET) {
            fail("The GPU device was lost while presenting video.");
            return;
        }
        check(result, "Present");
        std::scoped_lock lock(stats_mutex);
        ++current.presented;
        current.last_present = Clock::now();
    }

    std::vector<std::byte> capture_png() {
        if (!picture || !video || !decoder || !enumerator_for(decoder->coded_width(), decoder->coded_height())) return {};
        const D3D11_TEXTURE2D_DESC description{video->width, video->height, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0},
                                               D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
        ComPtr<ID3D11Texture2D> target;
        check(device->CreateTexture2D(&description, nullptr, &target), "Create screenshot texture");
        // The processor enumerator is sized for the swap chain; a separate one matches the video size.
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputFrameRate = content.OutputFrameRate = {60, 1};
        content.InputWidth = decoder->coded_width();
        content.InputHeight = decoder->coded_height();
        content.OutputWidth = video->width;
        content.OutputHeight = video->height;
        content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        ComPtr<ID3D11VideoProcessorEnumerator> capture_enumerator;
        check(video_device->CreateVideoProcessorEnumerator(&content, &capture_enumerator), "Create screenshot enumerator");
        ComPtr<ID3D11VideoProcessor> capture_processor;
        check(video_device->CreateVideoProcessor(capture_enumerator.Get(), 0, &capture_processor), "Create screenshot processor");
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_description{};
        output_description.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11VideoProcessorOutputView> view;
        check(video_device->CreateVideoProcessorOutputView(target.Get(), capture_enumerator.Get(), &output_description, &view),
              "Create screenshot view");
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_description{};
        input_description.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        input_description.Texture2D.ArraySlice = picture->subresource;
        ComPtr<ID3D11VideoProcessorInputView> input;
        check(video_device->CreateVideoProcessorInputView(picture->texture.Get(), capture_enumerator.Get(), &input_description, &input),
              "Create screenshot input");
        const RECT source{0, 0, static_cast<LONG>(video->width), static_cast<LONG>(video->height)};
        video_context->VideoProcessorSetStreamSourceRect(capture_processor.Get(), 0, TRUE, &source);
        video_context->VideoProcessorSetStreamDestRect(capture_processor.Get(), 0, TRUE, &source);
        video_context->VideoProcessorSetOutputTargetRect(capture_processor.Get(), TRUE, &source);
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = input.Get();
        check(video_context->VideoProcessorBlt(capture_processor.Get(), view.Get(), 0, 1, &stream), "Screenshot blit");

        auto staging_description = description;
        staging_description.Usage = D3D11_USAGE_STAGING;
        staging_description.BindFlags = 0;
        staging_description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        check(device->CreateTexture2D(&staging_description, nullptr, &staging), "Create screenshot staging");
        context->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map screenshot");
        std::vector<BYTE> pixels(static_cast<std::size_t>(video->width) * video->height * 4);
        for (UINT row = 0; row < video->height; ++row) {
            std::memcpy(pixels.data() + static_cast<std::size_t>(row) * video->width * 4,
                        static_cast<const BYTE*>(mapped.pData) + static_cast<std::size_t>(row) * mapped.RowPitch,
                        static_cast<std::size_t>(video->width) * 4);
        }
        context->Unmap(staging.Get(), 0);
        for (std::size_t index = 3; index < pixels.size(); index += 4) pixels[index] = 0xFF;

        ComPtr<IWICImagingFactory> factory;
        check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "Create WIC");
        ComPtr<IStream> stream_out;
        check(CreateStreamOnHGlobal(nullptr, TRUE, &stream_out), "Create PNG stream");
        ComPtr<IWICBitmapEncoder> encoder;
        check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "Create PNG encoder");
        check(encoder->Initialize(stream_out.Get(), WICBitmapEncoderNoCache), "Initialize PNG encoder");
        ComPtr<IWICBitmapFrameEncode> frame;
        check(encoder->CreateNewFrame(&frame, nullptr), "Create PNG frame");
        check(frame->Initialize(nullptr), "Initialize PNG frame");
        check(frame->SetSize(video->width, video->height), "Set PNG size");
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        check(frame->SetPixelFormat(&format), "Set PNG format");
        check(frame->WritePixels(video->height, video->width * 4, static_cast<UINT>(pixels.size()), pixels.data()), "Write PNG");
        check(frame->Commit(), "Commit PNG frame");
        check(encoder->Commit(), "Commit PNG");
        STATSTG statistics{};
        check(stream_out->Stat(&statistics, STATFLAG_NONAME), "Measure PNG");
        std::vector<std::byte> png(static_cast<std::size_t>(statistics.cbSize.QuadPart));
        const LARGE_INTEGER start{};
        check(stream_out->Seek(start, STREAM_SEEK_SET, nullptr), "Rewind PNG");
        ULONG read{};
        check(stream_out->Read(png.data(), static_cast<ULONG>(png.size()), &read), "Read PNG");
        png.resize(read);
        return png;
    }

    Callbacks callbacks;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VideoDevice> video_device;
    ComPtr<ID3D11VideoContext> video_context;
    ComPtr<IMFDXGIDeviceManager> manager;
    ComPtr<IDXGISwapChain1> swap;
    HANDLE waitable{};
    std::string adapter_name;
    UINT width{};
    UINT height{};

    // Pipeline thread state.
    std::optional<VideoParameters> video;
    std::unique_ptr<Decoder> decoder;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    ComPtr<ID3D11VideoProcessor> processor;
    ComPtr<ID3D11VideoProcessorOutputView> output_view;
    std::map<std::pair<ID3D11Texture2D*, UINT>, ComPtr<ID3D11VideoProcessorInputView>> input_views;
    std::pair<UINT, UINT> enumerator_input{};
    std::pair<UINT, UINT> enumerator_output{};
    std::optional<Picture> picture;
    bool present_pending{};
    bool waiting_for_key{true};
    bool failed{};
    int consecutive_errors{};

    // Shared with producers.
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Frame> queue;
    std::optional<std::pair<unsigned, unsigned>> pending_size;
    std::optional<VideoParameters> pending_parameters;
    std::vector<std::function<void()>> tasks;
    bool stopping{};

    mutable std::mutex stats_mutex;
    PipelineStats current;

    std::thread thread;
};

VideoPipeline::VideoPipeline(Callbacks callbacks) : impl_(std::make_unique<Impl>(std::move(callbacks))) {}

VideoPipeline::~VideoPipeline() = default;

IDXGISwapChain1* VideoPipeline::swap_chain() const { return impl_->swap.Get(); }

void VideoPipeline::configure(const VideoParameters& video) {
    {
        std::scoped_lock lock(impl_->mutex);
        impl_->pending_parameters = video;
        // Older-generation pictures can no longer be decoded with the new parameter set.
        std::erase_if(impl_->queue, [&](const Frame& frame) { return frame.generation < video.generation; });
    }
    impl_->wake.notify_one();
}

void VideoPipeline::submit(const VideoFrame& frame) {
    bool overflow{};
    {
        std::scoped_lock lock(impl_->mutex);
        if (impl_->queue.size() >= max_queued_frames) {
            // Dropping a reference picture breaks prediction: discard everything and resume at a key frame.
            impl_->queue.clear();
            overflow = true;
        }
        impl_->queue.push_back({frame.generation, frame.capture_time_us, frame.key_frame,
                                {frame.payload.begin(), frame.payload.end()}});
        if (overflow && !frame.key_frame) {
            impl_->queue.clear();
            impl_->tasks.push_back([impl = impl_.get()] { impl->waiting_for_key = true; });
        }
    }
    {
        std::scoped_lock lock(impl_->stats_mutex);
        ++impl_->current.received;
        if (overflow) ++impl_->current.dropped;
    }
    impl_->wake.notify_one();
    if (overflow && !frame.key_frame && impl_->callbacks.request_key_frame) impl_->callbacks.request_key_frame();
}

void VideoPipeline::resize(unsigned width, unsigned height) {
    {
        std::scoped_lock lock(impl_->mutex);
        impl_->pending_size = std::pair{width, height};
    }
    impl_->wake.notify_one();
}

std::vector<std::byte> VideoPipeline::screenshot_png() {
    auto promise = std::make_shared<std::promise<std::vector<std::byte>>>();
    auto result = promise->get_future();
    {
        std::scoped_lock lock(impl_->mutex);
        if (impl_->stopping) return {};
        impl_->tasks.push_back([impl = impl_.get(), promise] {
            try {
                promise->set_value(impl->capture_png());
            } catch (...) {
                promise->set_exception(std::current_exception());
            }
        });
    }
    impl_->wake.notify_one();
    return result.get();
}

PipelineStats VideoPipeline::stats() const {
    std::scoped_lock lock(impl_->stats_mutex);
    return impl_->current;
}

} // namespace beamit::client
