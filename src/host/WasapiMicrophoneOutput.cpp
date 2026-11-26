#include "host/WasapiMicrophoneOutput.hpp"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <optional>
#include <stdexcept>
#include <utility>

namespace beamit::host {
namespace {

using Microsoft::WRL::ComPtr;

// PKEY_Device_FriendlyName, defined here to avoid INITGUID in this translation unit.
constexpr PROPERTYKEY device_friendly_name{
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
constexpr REFERENCE_TIME render_buffer_100ns = 1'000'000; // 100 ms
// Jitter cushion written ahead of the first packet and after each underrun.
constexpr UINT32 cushion_frames = BEAMIT_MIC_FRAMES_PER_PACKET * 2;
// Packets that would queue more than this are dropped, bounding microphone latency.
constexpr UINT32 max_queued_frames = BEAMIT_MIC_FRAMES_PER_PACKET * 6;

std::string narrow(std::wstring_view text) {
    const auto size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                          nullptr, nullptr);
    std::string result(static_cast<std::size_t>(std::max(size, 0)), '\0');
    if (size > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr,
                            nullptr);
    }
    return result;
}

std::wstring widen(std::string_view text) {
    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                          nullptr, 0);
    if (size <= 0 && !text.empty()) throw std::invalid_argument("--microphone-output is not valid UTF-8");
    std::wstring result(static_cast<std::size_t>(std::max(size, 0)), L'\0');
    if (size > 0) {
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(),
                            size);
    }
    return result;
}

std::wstring lower(std::wstring_view text) {
    std::wstring result(text);
    std::ranges::transform(result, result.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return result;
}

void check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        throw std::runtime_error(fmt::format("{} (HRESULT 0x{:08X})", operation, static_cast<std::uint32_t>(result)));
    }
}

std::string join_names(const std::vector<std::wstring>& names) {
    std::string result;
    for (const auto& name : names) result += (result.empty() ? "" : ", ") + narrow(name);
    return result.empty() ? "none" : result;
}

struct RenderEndpoint {
    ComPtr<IMMDevice> device;
    std::wstring name;
};

std::vector<RenderEndpoint> render_endpoints() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)),
          "Create audio device enumerator");
    ComPtr<IMMDeviceCollection> collection;
    check(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection), "Enumerate render endpoints");
    UINT count{};
    check(collection->GetCount(&count), "Count render endpoints");
    std::vector<RenderEndpoint> result;
    for (UINT index = 0; index < count; ++index) {
        RenderEndpoint endpoint;
        ComPtr<IPropertyStore> properties;
        if (FAILED(collection->Item(index, &endpoint.device)) ||
            FAILED(endpoint.device->OpenPropertyStore(STGM_READ, &properties))) {
            continue;
        }
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(properties->GetValue(device_friendly_name, &value)) && value.vt == VT_LPWSTR) {
            endpoint.name = value.pwszVal;
        }
        PropVariantClear(&value);
        if (!endpoint.name.empty()) result.push_back(std::move(endpoint));
    }
    return result;
}

struct ComApartment {
    ComApartment() { check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "Initialize audio COM apartment"); }
    ~ComApartment() { CoUninitialize(); }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
};

class WasapiMicrophoneDevice final : public BeamItMicrophoneDevice {
public:
    explicit WasapiMicrophoneDevice(std::wstring endpoint_name) : endpoint_name_(std::move(endpoint_name)) {}
    ~WasapiMicrophoneDevice() override { release(); }

    void acquire() override {
        apartment_.emplace();
        try {
            auto endpoints = render_endpoints();
            std::vector<std::wstring> names;
            for (const auto& endpoint : endpoints) names.push_back(endpoint.name);
            std::size_t index{};
            try {
                index = endpoint_name_.empty() ? match_vb_cable_endpoint(names)
                                               : match_render_endpoint(names, endpoint_name_);
            } catch (const std::exception& error) {
                throw std::runtime_error(fmt::format("{} (active playback devices: {})", error.what(),
                                                     join_names(names)));
            }
            name_ = narrow(endpoints[index].name);
            warn_if_default(*endpoints[index].device.Get());
            check(endpoints[index].device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_),
                  "Activate microphone render endpoint");
            WAVEFORMATEX format{
                .wFormatTag = WAVE_FORMAT_PCM,
                .nChannels = BEAMIT_MIC_CHANNELS,
                .nSamplesPerSec = BEAMIT_MIC_SAMPLE_RATE_HZ,
                .nAvgBytesPerSec = BEAMIT_MIC_SAMPLE_RATE_HZ * BEAMIT_MIC_BYTES_PER_FRAME,
                .nBlockAlign = static_cast<WORD>(BEAMIT_MIC_BYTES_PER_FRAME),
                .wBitsPerSample = BEAMIT_MIC_BITS_PER_SAMPLE,
                .cbSize = 0,
            };
            // The audio engine converts to the endpoint mix format when it differs from 48 kHz PCM16 stereo.
            check(client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                      AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                      render_buffer_100ns, 0, &format, nullptr),
                  "Initialize microphone render endpoint");
            check(client_->GetBufferSize(&buffer_frames_), "Query microphone render buffer");
            check(client_->GetService(IID_PPV_ARGS(&render_)), "Open microphone render client");
            spdlog::info("Client microphone audio plays into \"{}\" ({} frame buffer)", name_, buffer_frames_);
        } catch (...) {
            release();
            throw;
        }
    }

    void write(const BeamItMicPacket& packet) override {
        if (!render_) throw std::runtime_error("Microphone render endpoint is not acquired");
        UINT32 padding{};
        check(client_->GetCurrentPadding(&padding), fmt::format("Query \"{}\" playback", name_).c_str());
        const UINT32 frames = packet.header.frame_count;
        if (padding == 0) {
            write_frames(nullptr, std::min(cushion_frames, buffer_frames_ - frames));
            padding = cushion_frames;
        }
        if (padding + frames > std::min(max_queued_frames, buffer_frames_)) {
            if (++dropped_ % 100 == 1) {
                spdlog::warn("\"{}\" playback is behind; dropped {} microphone packet(s)", name_, dropped_);
            }
            return;
        }
        write_frames(packet.samples.data(), frames);
        if (!started_) {
            check(client_->Start(), fmt::format("Start \"{}\" playback", name_).c_str());
            started_ = true;
        }
    }

    void release() noexcept override {
        if (client_ && started_) client_->Stop();
        started_ = false;
        render_.Reset();
        client_.Reset();
        apartment_.reset();
    }

private:
    void warn_if_default(IMMDevice& device) const {
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> default_device;
        LPWSTR selected_id{};
        LPWSTR default_id{};
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       IID_PPV_ARGS(&enumerator))) &&
            SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &default_device)) &&
            SUCCEEDED(device.GetId(&selected_id)) && SUCCEEDED(default_device->GetId(&default_id)) &&
            std::wstring_view(selected_id) == default_id) {
            spdlog::warn("\"{}\" is the default playback device: host audio is also played into the microphone "
                         "cable, so remote listeners may hear echoes. Choose real speakers as the default device.",
                         name_);
        }
        CoTaskMemFree(selected_id);
        CoTaskMemFree(default_id);
    }

    void write_frames(const std::int16_t* samples, UINT32 frames) {
        if (frames == 0) return;
        BYTE* buffer{};
        check(render_->GetBuffer(frames, &buffer), fmt::format("Fill \"{}\" playback", name_).c_str());
        if (samples) {
            std::memcpy(buffer, samples, frames * BEAMIT_MIC_CHANNELS * sizeof(std::int16_t));
            check(render_->ReleaseBuffer(frames, 0), "Submit microphone playback");
        } else {
            check(render_->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT), "Submit microphone silence");
        }
    }

    std::wstring endpoint_name_;
    std::optional<ComApartment> apartment_;
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioRenderClient> render_;
    std::string name_;
    UINT32 buffer_frames_{};
    std::uint64_t dropped_{};
    bool started_{};
};

class FallbackMicrophoneDevice final : public BeamItMicrophoneDevice {
public:
    FallbackMicrophoneDevice(BeamItMicrophoneDeviceFactory primary, BeamItMicrophoneDeviceFactory fallback)
        : primary_(std::move(primary)), fallback_(std::move(fallback)) {}

    void acquire() override {
        std::string primary_error;
        try {
            device_ = primary_();
            if (!device_) throw std::runtime_error("no device");
            device_->acquire();
            return;
        } catch (const std::exception& error) {
            device_.reset();
            primary_error = error.what();
        }
        try {
            device_ = fallback_();
            if (!device_) throw std::runtime_error("no device");
            device_->acquire();
            spdlog::info("Using the fallback microphone output: {}", primary_error);
        } catch (const std::exception& error) {
            device_.reset();
            throw std::runtime_error(fmt::format("{}; {}", primary_error, error.what()));
        }
    }

    void write(const BeamItMicPacket& packet) override {
        if (!device_) throw std::runtime_error("Microphone output is not acquired");
        device_->write(packet);
    }

    void release() noexcept override {
        if (device_) device_->release();
        device_.reset();
    }

private:
    BeamItMicrophoneDeviceFactory primary_;
    BeamItMicrophoneDeviceFactory fallback_;
    std::unique_ptr<BeamItMicrophoneDevice> device_;
};

} // namespace

std::size_t match_render_endpoint(const std::vector<std::wstring>& names, std::wstring_view wanted) {
    const auto key = lower(wanted);
    if (key.empty()) throw std::invalid_argument("Microphone playback device name is empty");
    std::optional<std::size_t> exact;
    std::vector<std::size_t> partial;
    for (std::size_t index = 0; index < names.size(); ++index) {
        const auto name = lower(names[index]);
        if (name == key) {
            if (exact) throw std::runtime_error(fmt::format("Several playback devices are named \"{}\"",
                                                            narrow(wanted)));
            exact = index;
        } else if (name.contains(key)) {
            partial.push_back(index);
        }
    }
    if (exact) return *exact;
    if (partial.size() == 1) return partial.front();
    if (partial.empty()) throw std::runtime_error(fmt::format("No playback device matches \"{}\"", narrow(wanted)));
    throw std::runtime_error(fmt::format("Several playback devices match \"{}\"; use a longer name", narrow(wanted)));
}

std::size_t match_vb_cable_endpoint(const std::vector<std::wstring>& names) {
    try {
        return match_render_endpoint(names, vb_cable_render_endpoint);
    } catch (const std::exception&) {
    }
    std::vector<std::size_t> found;
    for (std::size_t index = 0; index < names.size(); ++index) {
        const auto name = lower(names[index]);
        if (name.contains(lower(vb_cable_device_description)) && !name.contains(L"16ch") && !name.contains(L"16 ch")) {
            found.push_back(index);
        }
    }
    if (found.size() == 1) return found.front();
    throw std::runtime_error(found.empty() ? "VB-Audio Virtual Cable is not installed"
                                           : "Several VB-Audio Virtual Cable playback devices exist; pass "
                                             "--microphone-output with the playback device name");
}

std::vector<std::wstring> render_endpoint_names() {
    ComApartment apartment;
    std::vector<std::wstring> result;
    for (auto& endpoint : render_endpoints()) result.push_back(std::move(endpoint.name));
    return result;
}

std::unique_ptr<BeamItMicrophoneDevice> make_wasapi_microphone_device(std::wstring endpoint_name) {
    return std::make_unique<WasapiMicrophoneDevice>(std::move(endpoint_name));
}

BeamItMicrophoneDeviceFactory make_fallback_microphone_device(BeamItMicrophoneDeviceFactory primary,
                                                              BeamItMicrophoneDeviceFactory fallback) {
    return [primary = std::move(primary), fallback = std::move(fallback)] {
        return std::make_unique<FallbackMicrophoneDevice>(primary, fallback);
    };
}

BeamItMicrophoneDeviceFactory microphone_device_factory(std::string_view selection) {
    if (selection == "beamit") return make_beamit_microphone_device;
    if (selection == "vb-cable") return [] { return make_wasapi_microphone_device({}); };
    if (selection == "auto") {
        return make_fallback_microphone_device(make_beamit_microphone_device, [] {
            return make_wasapi_microphone_device({});
        });
    }
    if (selection.empty()) throw std::invalid_argument("--microphone-output is empty");
    return [name = widen(selection)] { return make_wasapi_microphone_device(name); };
}

} // namespace beamit::host
