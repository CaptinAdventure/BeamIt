#pragma once

#include "host/AudioEndpoint.hpp"

#include <winsock2.h>

#include "BeamItMicIoctl.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace beamit::host {

inline constexpr std::size_t max_microphone_output_packets = BEAMIT_MIC_QUEUE_PACKETS;

struct BeamItMicPacket {
    BeamItMicPcmHeader header{};
    std::vector<std::int16_t> samples;
};

[[nodiscard]] std::int16_t microphone_sample_to_pcm16(float sample) noexcept;
[[nodiscard]] BeamItMicPacket make_beamit_mic_packet(const AudioPcmPacket& packet);

class BeamItMicrophoneDevice {
public:
    virtual ~BeamItMicrophoneDevice() = default;
    virtual void acquire() = 0;
    virtual void write(const BeamItMicPacket& packet) = 0;
    virtual void release() noexcept = 0;
};

using BeamItMicrophoneDeviceFactory = std::function<std::unique_ptr<BeamItMicrophoneDevice>()>;

[[nodiscard]] std::unique_ptr<BeamItMicrophoneDevice> make_beamit_microphone_device();

class BeamItMicrophoneOutput {
public:
    using ErrorCallback = std::function<void(std::string)>;
    using ReadyCallback = std::function<void()>;

    BeamItMicrophoneOutput(media::AudioConfiguration configuration, ErrorCallback on_error,
                           BeamItMicrophoneDeviceFactory device_factory = make_beamit_microphone_device,
                           ReadyCallback on_ready = {});
    ~BeamItMicrophoneOutput();
    BeamItMicrophoneOutput(const BeamItMicrophoneOutput&) = delete;
    BeamItMicrophoneOutput& operator=(const BeamItMicrophoneOutput&) = delete;

    void submit(const AudioPcmPacket& packet);
    void stop();
    [[nodiscard]] std::string error() const;

private:
    void run();
    void report_error(std::string message);

    ErrorCallback on_error_;
    ReadyCallback on_ready_;
    BeamItMicrophoneDeviceFactory device_factory_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<BeamItMicPacket> queue_;
    std::string error_;
    bool stopping_{};
    std::thread worker_;
};

} // namespace beamit::host
