#include "host/AudioCapture.hpp"
#include "ThreadPriority.hpp"

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <wrl.h>
#include <wrl/implements.h>

#include <spdlog/spdlog.h>

#include <thread>
#include <stdexcept>

namespace beamit::host {
namespace {

using Microsoft::WRL::ComPtr;

void check(HRESULT result, const char* operation) {
    if (FAILED(result)) throw std::runtime_error(fmt::format("{} (HRESULT 0x{:08X})", operation,
                                                            static_cast<std::uint32_t>(result)));
}

struct CloseHandleDeleter {
    void operator()(void* handle) const noexcept { CloseHandle(handle); }
};
using UniqueHandle = std::unique_ptr<void, CloseHandleDeleter>;

UniqueHandle event_handle(bool manual_reset = false) {
    UniqueHandle event(CreateEventW(nullptr, manual_reset, FALSE, nullptr));
    if (!event) check(HRESULT_FROM_WIN32(GetLastError()), "Create audio event");
    return event;
}

class ActivationCompletion final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
    IActivateAudioInterfaceCompletionHandler, Microsoft::WRL::FtmBase> {
public:
    UniqueHandle completed{event_handle()};
    HRESULT result{E_PENDING};
    ComPtr<IUnknown> audio;

    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) noexcept override {
        const auto query = operation->GetActivateResult(&result, &audio);
        if (FAILED(query)) result = query;
        SetEvent(completed.get());
        return S_OK;
    }
};

struct ComApartment {
    ComApartment() { check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "Initialize audio COM apartment"); }
    ~ComApartment() { CoUninitialize(); }
};

class WasapiAudioCapture final : public AudioCapture {
public:
    WasapiAudioCapture(AudioTrack track, std::shared_ptr<AudioEndpoint> endpoint, AudioCaptureCallbacks callbacks)
        : stop_event_(event_handle(true)),
          worker_([this, track, endpoint = std::move(endpoint), callbacks = std::move(callbacks)] {
              try {
                  run(track, endpoint, callbacks);
              } catch (const std::exception& error) {
                  if (WaitForSingleObject(stop_event_.get(), 0) != WAIT_OBJECT_0) {
                      spdlog::error("Audio track {} capture failed: {}", track.id, error.what());
                      callbacks.failed(error.what());
                  }
              }
          }) {}

    ~WasapiAudioCapture() override { stop(); }

    void stop() noexcept override {
        SetEvent(stop_event_.get());
        if (worker_.joinable()) worker_.join();
    }

private:
    void run(const AudioTrack& track, const std::shared_ptr<AudioEndpoint>& endpoint,
             const AudioCaptureCallbacks& callbacks) {
        const ComApartment apartment;
        const MmcssTaskGuard mmcss(set_realtime_priority_with_handle());
        if (!mmcss.registered()) spdlog::warn("Audio capture MMCSS registration failed; using priority fallback");
        track.configuration.validate();
        if (track.kind == AudioTrack::Kind::Microphone) throw std::invalid_argument("Cannot loopback a microphone track");
        if (track.kind == AudioTrack::Kind::Application) {
            UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, track.pid));
            if (!process) check(HRESULT_FROM_WIN32(GetLastError()), "Open audio source process");
            if (WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT) {
                throw std::runtime_error("Audio source process has exited");
            }
        }
        AUDIOCLIENT_ACTIVATION_PARAMS activation{};
        activation.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        activation.ProcessLoopbackParams.TargetProcessId =
            track.kind == AudioTrack::Kind::Desktop ? GetCurrentProcessId() : track.pid;
        activation.ProcessLoopbackParams.ProcessLoopbackMode = track.kind == AudioTrack::Kind::Desktop
            ? PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE : PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
        PROPVARIANT parameters{};
        parameters.vt = VT_BLOB;
        parameters.blob.cbSize = sizeof(activation);
        parameters.blob.pBlobData = reinterpret_cast<BYTE*>(&activation);
        const auto completion = Microsoft::WRL::Make<ActivationCompletion>();
        if (!completion) throw std::bad_alloc();
        ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
        check(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
                                         &parameters, completion.Get(), &operation), "Activate process loopback");
        const HANDLE activation_events[]{stop_event_.get(), completion->completed.get()};
        const auto activated = WaitForMultipleObjects(2, activation_events, FALSE, 10'000);
        if (activated == WAIT_OBJECT_0) return;
        if (activated == WAIT_TIMEOUT) throw std::runtime_error("WASAPI process-loopback activation timed out");
        if (activated != WAIT_OBJECT_0 + 1) check(HRESULT_FROM_WIN32(GetLastError()), "Wait for audio activation");
        check(completion->result, "Activate WASAPI process loopback");
        ComPtr<IAudioClient> client;
        check(completion->audio.As(&client), "Query WASAPI audio client");

        // Process loopback accepts an explicit format. The audio engine performs device conversion/resampling.
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels = static_cast<WORD>(track.configuration.channels);
        format.nSamplesPerSec = track.configuration.sample_rate;
        format.wBitsPerSample = sizeof(float) * 8;
        format.nBlockAlign = format.nChannels * sizeof(float);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        check(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            0, 0, &format, nullptr), "Initialize WASAPI process loopback");
        auto samples_ready = event_handle();
        check(client->SetEventHandle(samples_ready.get()), "Set WASAPI sample event");
        ComPtr<IAudioCaptureClient> capture;
        check(client->GetService(IID_PPV_ARGS(&capture)), "Get WASAPI capture service");
        AudioPacketizer packetizer(track.configuration, [&](std::span<const float> pcm, bool discontinuity) {
            return endpoint->submit_pcm(track.id, pcm, discontinuity);
        });
        check(client->Start(), "Start WASAPI process loopback");
        struct StopClient {
            IAudioClient* client;
            ~StopClient() {
                const auto result = client->Stop();
                if (FAILED(result)) spdlog::warn("Stop WASAPI capture failed (HRESULT 0x{:08X})",
                                               static_cast<std::uint32_t>(result));
            }
        } stop_client{client.Get()};
        callbacks.started();
        const HANDLE capture_events[]{stop_event_.get(), samples_ready.get()};
        while (true) {
            const auto ready = WaitForMultipleObjects(2, capture_events, FALSE, INFINITE);
            if (ready == WAIT_OBJECT_0) return;
            if (ready != WAIT_OBJECT_0 + 1) check(HRESULT_FROM_WIN32(GetLastError()), "Wait for WASAPI sample");
            while (WaitForSingleObject(stop_event_.get(), 0) == WAIT_TIMEOUT) {
                UINT32 frames{};
                check(capture->GetNextPacketSize(&frames), "Query WASAPI packet size");
                if (!frames) break;
                BYTE* data{};
                DWORD flags{};
                check(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr), "Acquire WASAPI packet");
                struct ReleaseBuffer {
                    IAudioCaptureClient* capture;
                    UINT32 frames;
                    bool released{};
                    ~ReleaseBuffer() {
                        if (!released) {
                            const auto result = capture->ReleaseBuffer(frames);
                            if (FAILED(result)) spdlog::warn("Release WASAPI packet failed (HRESULT 0x{:08X})",
                                                           static_cast<std::uint32_t>(result));
                        }
                    }
                } release{capture.Get(), frames};
                const bool discontinuity = (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    packetizer.silence(frames, discontinuity);
                } else {
                    if (!data) throw std::runtime_error("WASAPI supplied a null non-silent buffer");
                    packetizer.append({reinterpret_cast<const float*>(data),
                                       static_cast<std::size_t>(frames) * format.nChannels}, discontinuity);
                }
                const auto result = capture->ReleaseBuffer(frames);
                release.released = true;
                check(result, "Release WASAPI packet");
            }
        }
    }

    UniqueHandle stop_event_;
    std::jthread worker_;
};

} // namespace

std::unique_ptr<AudioCapture> make_wasapi_audio_capture(
    AudioTrack track, std::shared_ptr<AudioEndpoint> endpoint, AudioCaptureCallbacks callbacks) {
    return std::make_unique<WasapiAudioCapture>(track, std::move(endpoint), std::move(callbacks));
}

} // namespace beamit::host
