#include "DecoderProbe.hpp"

#include <windows.h>
#include <d3d10_1.h>
#include <d3d10.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace beamit::client {
namespace {

using Microsoft::WRL::ComPtr;

std::wstring widen(std::string_view value) {
    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (size == 0 && !value.empty()) throw std::runtime_error("Could not decode probe error text");
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (size != 0 && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), size) == 0) throw std::runtime_error("Could not decode probe error text");
    return result;
}

void check(HRESULT result, std::string_view operation) {
    if (SUCCEEDED(result)) return;
    std::ostringstream message;
    message << operation << " failed (HRESULT 0x" << std::hex << std::uppercase
            << static_cast<std::uint32_t>(result) << ')';
    throw std::runtime_error(message.str());
}

class Platform {
public:
    Platform() { check(MFStartup(MF_VERSION, MFSTARTUP_FULL), "MFStartup"); }
    ~Platform() {
        if (FAILED(MFShutdown())) OutputDebugStringW(L"BeamIt decoder probe: MFShutdown failed\n");
    }
    Platform(Platform const&) = delete;
    Platform& operator=(Platform const&) = delete;
};

struct Activations {
    IMFActivate** values{};
    UINT32 count{};
    ~Activations() {
        for (UINT32 index = 0; index < count; ++index) values[index]->Release();
        CoTaskMemFree(values);
    }
    Activations() = default;
    Activations(Activations const&) = delete;
    Activations& operator=(Activations const&) = delete;
};

struct Candidate {
    ComPtr<IMFActivate> activation;
    std::wstring name;
    bool hardware{};
};

std::vector<Candidate> candidates(bool hevc) {
    MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264};
    std::vector<Candidate> result;
    for (bool hardware : {true, false}) {
        Activations entries;
        const auto flags = MFT_ENUM_FLAG_SORTANDFILTER |
            (hardware ? MFT_ENUM_FLAG_HARDWARE : MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_LOCALMFT);
        check(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, flags, &input, nullptr, &entries.values, &entries.count),
            "MFTEnumEx");
        for (UINT32 index = 0; index < entries.count; ++index) {
            wchar_t* name{};
            UINT32 length{};
            check(entries.values[index]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &length),
                "Get decoder friendly name");
            std::wstring text;
            try {
                text.assign(name, length);
            } catch (...) {
                CoTaskMemFree(name);
                throw;
            }
            CoTaskMemFree(name);
            result.push_back({ComPtr<IMFActivate>(entries.values[index]), std::move(text), hardware});
        }
    }
    return result;
}

class Activation {
public:
    explicit Activation(IMFActivate* activation) : activation_(activation) {
        check(activation_->ActivateObject(IID_PPV_ARGS(transform.GetAddressOf())), "Activate decoder");
    }
    ~Activation() {
        const auto result = activation_->ShutdownObject();
        if (FAILED(result)) OutputDebugStringW(L"BeamIt decoder probe: ShutdownObject failed\n");
    }
    ComPtr<IMFTransform> transform;
private:
    ComPtr<IMFActivate> activation_;
};

struct Device {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IMFDXGIDeviceManager> manager;

    Device() {
        constexpr D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &device, nullptr, &context), "Create decoder D3D11 device");
        ComPtr<ID3D10Multithread> multithread;
        check(device.As(&multithread), "Get D3D multithread protection");
        multithread->SetMultithreadProtected(TRUE);
        UINT token{};
        check(MFCreateDXGIDeviceManager(&token, &manager), "Create DXGI device manager");
        check(manager->ResetDevice(device.Get(), token), "Set DXGI device");
    }
};

ComPtr<IMFMediaType> negotiate_output(IMFTransform* transform) {
    for (DWORD index = 0;; ++index) {
        ComPtr<IMFMediaType> type;
        const auto result = transform->GetOutputAvailableType(0, index, &type);
        if (result == MF_E_NO_MORE_TYPES) throw std::runtime_error("Decoder has no accepted output media type");
        check(result, "Get decoder output type");
        const auto set = transform->SetOutputType(0, type.Get(), 0);
        if (set == MF_E_INVALIDMEDIATYPE) continue;
        check(set, "Set decoder output type");
        return type;
    }
}

ComPtr<IMFSample> input_sample(std::span<const std::byte> bytes) {
    ComPtr<IMFSample> sample;
    check(MFCreateSample(&sample), "Create input sample");
    ComPtr<IMFMediaBuffer> buffer;
    check(MFCreateMemoryBuffer(static_cast<DWORD>(bytes.size()), &buffer), "Create input buffer");
    BYTE* destination{};
    check(buffer->Lock(&destination, nullptr, nullptr), "Lock input buffer");
    std::memcpy(destination, bytes.data(), bytes.size());
    check(buffer->Unlock(), "Unlock input buffer");
    check(buffer->SetCurrentLength(static_cast<DWORD>(bytes.size())), "Set input length");
    check(sample->AddBuffer(buffer.Get()), "Attach input buffer");
    check(sample->SetSampleTime(0), "Set sample time");
    check(sample->SetSampleDuration(166667), "Set sample duration");
    check(sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE), "Mark IDR sample");
    return sample;
}

struct OutputAllocation {
    ComPtr<IMFVideoSampleAllocatorEx> allocator;
    ComPtr<IMFSample> sample;
};

OutputAllocation output_sample(IMFTransform* transform, IMFMediaType* type, IMFDXGIDeviceManager* manager) {
    MFT_OUTPUT_STREAM_INFO info{};
    check(transform->GetOutputStreamInfo(0, &info), "Get output allocation requirements");
    if (info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) return {};
    OutputAllocation allocation;
    if (manager) {
        check(MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&allocation.allocator)), "Create GPU sample allocator");
        check(allocation.allocator->SetDirectXManager(manager), "Set GPU sample allocator device");
        check(allocation.allocator->InitializeSampleAllocatorEx(1, 2, nullptr, type), "Initialize GPU samples");
        check(allocation.allocator->AllocateSample(&allocation.sample), "Allocate GPU output sample");
    } else {
        if (info.cbSize == 0) throw std::runtime_error("Decoder did not specify an output buffer size");
        check(MFCreateSample(&allocation.sample), "Create output sample");
        ComPtr<IMFMediaBuffer> buffer;
        check(MFCreateAlignedMemoryBuffer(info.cbSize, info.cbAlignment ? info.cbAlignment - 1 : 0, &buffer),
            "Allocate output buffer");
        check(allocation.sample->AddBuffer(buffer.Get()), "Attach output buffer");
    }
    return allocation;
}

std::wstring decode(Candidate const& candidate, bool hevc, unsigned width, unsigned height,
                    std::span<const std::byte> bytes) {
    Activation active(candidate.activation.Get());
    const auto transform = active.transform.Get();
    ComPtr<IMFAttributes> attributes;
    check(transform->GetAttributes(&attributes), "Get decoder attributes");
    UINT32 asynchronous{};
    const auto async_status = attributes->GetUINT32(MF_TRANSFORM_ASYNC, &asynchronous);
    if (async_status != MF_E_ATTRIBUTENOTFOUND) check(async_status, "Query asynchronous mode");
    if (asynchronous) check(attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE), "Unlock async decoder");
    UINT32 d3d_aware{};
    const auto d3d_status = attributes->GetUINT32(MF_SA_D3D11_AWARE, &d3d_aware);
    if (d3d_status != MF_E_ATTRIBUTENOTFOUND) check(d3d_status, "Query D3D11 awareness");
    std::unique_ptr<Device> device;
    if (d3d_aware) {
        device = std::make_unique<Device>();
        check(transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
            reinterpret_cast<ULONG_PTR>(device->manager.Get())), "Attach decoder D3D11 device");
    }
    // The probe reports rejected low-latency configuration rather than pretending it was accepted.
    check(attributes->SetUINT32(MF_LOW_LATENCY, TRUE), "Request low-latency decoding");
    ComPtr<IMFMediaType> input;
    check(MFCreateMediaType(&input), "Create decoder input type");
    check(input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Set input major type");
    check(input->SetGUID(MF_MT_SUBTYPE, hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264), "Set input codec");
    check(input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "Set progressive input");
    check(MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, width, height), "Set input dimensions");
    check(MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, 60, 1), "Set input frame rate");
    check(transform->SetInputType(0, input.Get(), 0), "Set decoder input type");
    auto type = negotiate_output(transform);
    check(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "Begin decoder streaming");
    check(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "Start decoder stream");
    auto input_frame = input_sample(bytes);
    ComPtr<IMFMediaEventGenerator> events;
    if (asynchronous) check(active.transform.As(&events), "Get async decoder events");

    bool submitted{};
    bool draining{};
    bool may_output = !asynchronous;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (asynchronous && !may_output) {
            ComPtr<IMFMediaEvent> event;
            const auto status = events->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
            if (status == MF_E_NO_EVENTS_AVAILABLE) { Sleep(1); continue; }
            check(status, "Get decoder event");
            HRESULT event_status{};
            check(event->GetStatus(&event_status), "Get decoder event status");
            check(event_status, "Decoder event");
            MediaEventType event_type{};
            check(event->GetType(&event_type), "Get decoder event type");
            if (event_type == METransformNeedInput && !submitted) {
                check(transform->ProcessInput(0, input_frame.Get(), 0), "Submit IDR");
                submitted = true;
            } else if (event_type == METransformHaveOutput) {
                may_output = true;
            } else if (event_type == METransformDrainComplete) {
                throw std::runtime_error("Decoder drained without producing a picture");
            }
        } else if (!asynchronous && !submitted) {
            check(transform->ProcessInput(0, input_frame.Get(), 0), "Submit IDR");
            submitted = true;
        }
        if (submitted && !draining) {
            check(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0), "End decoder input");
            check(transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0), "Drain decoder");
            draining = true;
        }
        if (!may_output) continue;
        auto allocated = output_sample(transform, type.Get(), device ? device->manager.Get() : nullptr);
        MFT_OUTPUT_DATA_BUFFER output{};
        output.pSample = allocated.sample.Get();
        DWORD flags{};
        const auto status = transform->ProcessOutput(0, 1, &output, &flags);
        ComPtr<IMFCollection> output_events;
        output_events.Attach(output.pEvents);
        ComPtr<IMFSample> decoded;
        if (output.pSample == allocated.sample.Get()) decoded = allocated.sample;
        else decoded.Attach(output.pSample);
        if (status == MF_E_TRANSFORM_STREAM_CHANGE) {
            type = negotiate_output(transform);
            // The same output event authorizes retrying after type renegotiation.
            if (asynchronous) may_output = true;
            continue;
        }
        may_output = !asynchronous;
        if (status == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (!asynchronous) throw std::runtime_error("Complete IDR required: decoder needs more input after drain");
            continue;
        }
        check(status, "Decode IDR output");
        if (!decoded) throw std::runtime_error("Decoder returned no output sample");
        ComPtr<IMFMediaBuffer> buffer;
        check(decoded->GetBufferByIndex(0, &buffer), "Get decoded buffer");
        ComPtr<IMFDXGIBuffer> gpu;
        const bool gpu_output = SUCCEEDED(buffer.As(&gpu));
        UINT32 output_width{}, output_height{};
        check(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &output_width, &output_height), "Get output size");
        if (output_width == 0 || output_height == 0) throw std::runtime_error("Decoder output has invalid dimensions");
        return L"decoded " + std::to_wstring(output_width) + L"x" + std::to_wstring(output_height) +
            (gpu_output ? L" to a DXGI surface" : L" to system memory") +
            (asynchronous ? L" (asynchronous MFT)" : L" (synchronous MFT)");
    }
    throw std::runtime_error("Decoder did not produce a picture within the probe deadline");
}

} // namespace

std::wstring decoder_inventory_report() {
    Platform platform;
    std::wstring report = L"Enumeration is not proof of profile support or successful decoding.\n";
    for (bool hevc : {false, true}) {
        report += hevc ? L"\nHEVC:\n" : L"\nH.264:\n";
        const auto entries = candidates(hevc);
        if (entries.empty()) report += L"  No installed decoder candidates.\n";
        for (auto const& entry : entries) {
            report += (entry.hardware ? L"  Hardware: " : L"  Installed software: ") + entry.name + L"\n";
        }
    }
    return report;
}

DecoderProbeResult probe_decoder(bool hevc, unsigned width, unsigned height, std::span<const std::byte> access_unit) {
    if (width == 0 || height == 0 || width > 16384 || height > 16384 ||
        static_cast<std::uint64_t>(width) * height > 64ULL * 1024 * 1024 ||
        access_unit.empty() || access_unit.size() > 16ULL * 1024 * 1024) {
        throw std::invalid_argument("Probe requires valid dimensions and one IDR access unit up to 16 MiB");
    }
    Platform platform;
    DecoderProbeResult result;
    for (auto const& entry : candidates(hevc)) {
        result.report += (entry.hardware ? L"Hardware: " : L"Installed software: ") + entry.name + L": ";
        try {
            result.report += decode(entry, hevc, width, height, access_unit) + L"\n";
            result.decoded = true;
            break;
        } catch (const std::exception& error) {
            result.report += widen(error.what()) + L"\n";
        }
    }
    if (!result.decoded) result.report += L"No installed Media Foundation candidate decoded this fixture. No codec/profile fallback was attempted.\n";
    return result;
}

} // namespace beamit::client
