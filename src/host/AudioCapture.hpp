#pragma once

#include "host/AudioEndpoint.hpp"

namespace beamit::host {

struct AudioCaptureCallbacks {
    std::function<void()> started;
    std::function<void(std::string)> failed;
};

class AudioCapture {
public:
    virtual ~AudioCapture() = default;
    virtual void stop() noexcept = 0;
};

[[nodiscard]] std::unique_ptr<AudioCapture> make_wasapi_audio_capture(
    AudioTrack track, std::shared_ptr<AudioEndpoint> endpoint, AudioCaptureCallbacks callbacks);

// Splits arbitrary WASAPI chunks into exactly one configured Opus packet at a time.
class AudioPacketizer {
public:
    using Submit = std::function<AudioSubmitResult(std::span<const float>, bool)>;
    AudioPacketizer(media::AudioConfiguration configuration, Submit submit);
    void append(std::span<const float> samples, bool discontinuity = false);
    void silence(std::size_t frames, bool discontinuity = false);

private:
    void reset_if_discontinuous(bool discontinuity);
    void flush();
    media::AudioConfiguration configuration_;
    Submit submit_;
    std::vector<float> packet_;
    std::size_t filled_{};
    bool discontinuity_{true};
};

} // namespace beamit::host
