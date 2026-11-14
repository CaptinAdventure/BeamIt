#include "AudioClient.hpp"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>

namespace beamit::client {
namespace {

using Microsoft::WRL::ComPtr;

struct HandleCloser {
    void operator()(void* value) const noexcept {
        if (value) CloseHandle(value);
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

void check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        throw std::runtime_error(std::format("{} failed (HRESULT 0x{:08X})", operation,
                                             static_cast<std::uint32_t>(result)));
    }
}

struct ComApartment {
    ComApartment() { check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "Initialize audio COM apartment"); }
    ~ComApartment() { CoUninitialize(); }
};

WAVEFORMATEX wave_format(const media::AudioConfiguration& configuration) {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = static_cast<WORD>(configuration.channels);
    format.nSamplesPerSec = configuration.sample_rate;
    format.wBitsPerSample = sizeof(float) * 8;
    format.nBlockAlign = format.nChannels * sizeof(float);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    return format;
}

ComPtr<IAudioClient> default_client(EDataFlow flow, const WAVEFORMATEX& format, HANDLE event) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)),
          "Create audio device enumerator");
    ComPtr<IMMDevice> device;
    check(enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device), "Get default audio endpoint");
    ComPtr<IAudioClient> client;
    check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                            reinterpret_cast<void**>(client.GetAddressOf())), "Activate audio client");
    check(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
          AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
              AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
          0, 0, &format, nullptr), "Initialize shared-mode audio");
    check(client->SetEventHandle(event), "Set audio event handle");
    return client;
}

} // namespace

AudioClient::AudioClient(AudioTrackConfiguration microphone, SendPacket send_packet, Error error)
    : microphone_(std::move(microphone)), send_packet_(std::move(send_packet)), error_(std::move(error)),
      encoder_(microphone_.configuration) {
    microphone_.configuration.validate();
    if (!microphone_.track_id || !send_packet_) throw std::invalid_argument("Invalid microphone audio route");
}

AudioClient::~AudioClient() { stop(); }

void AudioClient::start() {
    if (running_.exchange(true)) throw std::logic_error("Audio client is already running");
    reported_error_ = false;
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_event_) {
        running_ = false;
        throw std::runtime_error(std::format("Create audio stop event failed (Win32 {})", GetLastError()));
    }
    try {
        capture_thread_ = std::thread([this] { capture_loop(); });
        render_thread_ = std::thread([this] { render_loop(); });
    } catch (...) {
        stop();
        throw;
    }
}

void AudioClient::add_track(AudioTrackConfiguration track) {
    track.configuration.validate();
    const auto& expected = microphone_.configuration;
    if (!track.track_id || track.configuration.sample_rate != expected.sample_rate ||
        track.configuration.channels != expected.channels ||
        track.configuration.packet_duration != expected.packet_duration) {
        throw std::invalid_argument("The offered audio track format is incompatible with the active audio device.");
    }
    auto decoder = std::make_unique<Track>(track.configuration);
    std::scoped_lock lock(tracks_mutex_);
    if (!tracks_.emplace(track.track_id, std::move(decoder)).second) {
        throw std::invalid_argument("The audio track is already active.");
    }
}

void AudioClient::remove_track(std::uint32_t track_id) {
    std::scoped_lock lock(tracks_mutex_);
    tracks_.erase(track_id);
}

void AudioClient::receive(const net::protocol::AudioPacket& packet,
                          const media::AudioConfiguration& configuration) {
    std::scoped_lock lock(tracks_mutex_);
    const auto track = tracks_.find(packet.track_id);
    if (track == tracks_.end()) return;
    if (configuration.sample_rate != track->second->configuration.sample_rate ||
        configuration.channels != track->second->configuration.channels ||
        configuration.packet_duration != track->second->configuration.packet_duration) {
        throw std::invalid_argument("The host changed an active audio track configuration.");
    }
    auto samples = track->second->decoder.decode(packet.payload, packet.discontinuity);
    auto& queued = track->second->queued;
    if (queued.size() >= 3) queued.pop_front();
    queued.push_back(std::move(samples));
}

void AudioClient::stop() noexcept {
    running_ = false;
    if (stop_event_) SetEvent(static_cast<HANDLE>(stop_event_));
    if (capture_thread_.joinable()) capture_thread_.join();
    if (render_thread_.joinable()) render_thread_.join();
    if (stop_event_) {
        CloseHandle(static_cast<HANDLE>(stop_event_));
        stop_event_ = nullptr;
    }
    std::scoped_lock lock(tracks_mutex_);
    tracks_.clear();
}

void AudioClient::report_error(std::string message) {
    running_ = false;
    if (stop_event_) SetEvent(static_cast<HANDLE>(stop_event_));
    if (reported_error_.exchange(true) || !error_) return;
    error_(std::move(message));
}

void AudioClient::capture_loop() {
    try {
        const ComApartment apartment;
        UniqueHandle ready(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (!ready) check(HRESULT_FROM_WIN32(GetLastError()), "Create microphone event");
        const auto format = wave_format(microphone_.configuration);
        auto client = default_client(eCapture, format, static_cast<HANDLE>(ready.get()));
        ComPtr<IAudioCaptureClient> capture;
        check(client->GetService(IID_PPV_ARGS(&capture)), "Get microphone capture service");
        check(client->Start(), "Start microphone capture");
        std::vector<float> pending;
        pending.reserve(microphone_.configuration.sample_count() * 2);
        bool discontinuity = true;
        const HANDLE events[]{static_cast<HANDLE>(stop_event_), static_cast<HANDLE>(ready.get())};
        while (!stopping()) {
            const auto result = WaitForMultipleObjects(2, events, FALSE, INFINITE);
            if (result == WAIT_OBJECT_0) break;
            if (result != WAIT_OBJECT_0 + 1) check(HRESULT_FROM_WIN32(GetLastError()), "Wait for microphone samples");
            while (!stopping()) {
                UINT32 frames{};
                check(capture->GetNextPacketSize(&frames), "Query microphone packet size");
                if (!frames) break;
                BYTE* data{};
                DWORD flags{};
                check(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr), "Acquire microphone packet");
                const bool lost = (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
                if (lost) {
                    pending.clear();
                    discontinuity = true;
                }
                const auto sample_count = static_cast<std::size_t>(frames) * microphone_.configuration.channels;
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    pending.insert(pending.end(), sample_count, 0.0f);
                } else {
                    if (!data) throw std::runtime_error("WASAPI returned a null microphone buffer.");
                    const auto* samples = reinterpret_cast<const float*>(data);
                    for (std::size_t index = 0; index < sample_count; ++index) {
                        if (!std::isfinite(samples[index])) throw std::runtime_error("Microphone PCM is not finite.");
                        pending.push_back(std::clamp(samples[index], -1.0f, 1.0f));
                    }
                }
                check(capture->ReleaseBuffer(frames), "Release microphone packet");

                const auto packet_samples = microphone_.configuration.sample_count();
                while (pending.size() >= packet_samples && !stopping()) {
                    auto encoded = encoder_.encode(std::span(pending).first(packet_samples));
                    if (sequence_ > UINT64_MAX / microphone_.configuration.frame_size()) {
                        throw std::runtime_error("Microphone audio sequence overflowed.");
                    }
                    net::protocol::AudioPacket packet{
                        .track_id = microphone_.track_id,
                        .sequence = sequence_,
                        .sample_timestamp = sequence_ * microphone_.configuration.frame_size(),
                        .discontinuity = discontinuity,
                        .payload = std::move(encoded),
                    };
                    ++sequence_;
                    discontinuity = false;
                    pending.erase(pending.begin(), pending.begin() + packet_samples);
                    send_packet_(net::protocol::encode_audio_packet(packet, microphone_.configuration));
                }
            }
        }
        const auto stopped = client->Stop();
        if (FAILED(stopped)) check(stopped, "Stop microphone capture");
    } catch (const std::exception& error) {
        if (!stopping()) report_error(error.what());
    }
}

void AudioClient::render_loop() {
    try {
        const ComApartment apartment;
        UniqueHandle ready(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (!ready) check(HRESULT_FROM_WIN32(GetLastError()), "Create playback event");
        const auto format = wave_format(microphone_.configuration);
        auto client = default_client(eRender, format, static_cast<HANDLE>(ready.get()));
        ComPtr<IAudioRenderClient> render;
        check(client->GetService(IID_PPV_ARGS(&render)), "Get audio render service");
        UINT32 buffer_frames{};
        check(client->GetBufferSize(&buffer_frames), "Get audio render buffer size");
        check(client->Start(), "Start audio playback");
        const HANDLE events[]{static_cast<HANDLE>(stop_event_), static_cast<HANDLE>(ready.get())};
        while (!stopping()) {
            const auto result = WaitForMultipleObjects(2, events, FALSE, INFINITE);
            if (result == WAIT_OBJECT_0) break;
            if (result != WAIT_OBJECT_0 + 1) check(HRESULT_FROM_WIN32(GetLastError()), "Wait for playback buffer");
            UINT32 padding{};
            check(client->GetCurrentPadding(&padding), "Query audio render padding");
            const auto frames = buffer_frames - padding;
            if (!frames) continue;
            BYTE* bytes{};
            check(render->GetBuffer(frames, &bytes), "Acquire audio render buffer");
            auto* output = reinterpret_cast<float*>(bytes);
            const auto channels = microphone_.configuration.channels;
            std::fill_n(output, static_cast<std::size_t>(frames) * channels, 0.0f);
            {
                std::scoped_lock lock(tracks_mutex_);
                for (auto& [id, track] : tracks_) {
                    for (UINT32 frame = 0; frame < frames; ++frame) {
                        if (!track->current || track->offset >= track->configuration.frame_size()) {
                            track->current.reset();
                            track->offset = 0;
                            if (track->queued.empty()) break;
                            track->current = std::move(track->queued.front());
                            track->queued.pop_front();
                        }
                        for (unsigned channel = 0; channel < channels; ++channel) {
                            output[static_cast<std::size_t>(frame) * channels + channel] +=
                                (*track->current)[track->offset * channels + channel];
                        }
                        ++track->offset;
                    }
                }
            }
            const auto samples = static_cast<std::size_t>(frames) * channels;
            for (std::size_t index = 0; index < samples; ++index) {
                output[index] = std::clamp(output[index], -1.0f, 1.0f);
            }
            check(render->ReleaseBuffer(frames, 0), "Release audio render buffer");
        }
        const auto stopped = client->Stop();
        if (FAILED(stopped)) check(stopped, "Stop audio playback");
    } catch (const std::exception& error) {
        if (!stopping()) report_error(error.what());
    }
}

} // namespace beamit::client
