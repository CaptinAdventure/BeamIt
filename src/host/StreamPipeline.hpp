#pragma once

#include "beamit/capture/Capturer.hpp"
#include "beamit/media/Encoder.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace beamit::host {

using Clock = std::chrono::steady_clock;

struct VideoParameters {
    std::uint32_t generation{};
    media::VideoCodec codec{media::VideoCodec::H264};
    std::string codec_string;
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t chroma_format_idc{1};
    std::uint32_t bit_depth{8};
    std::uint32_t frame_rate{};
    std::uint32_t bitrate_kbps{};
    // Effective VBR peak.
    std::uint32_t max_bitrate_kbps{};
    media::RateControlMode rate_control{media::RateControlMode::Vbr};
    std::string encoder;
};

struct EncodedFrame {
    media::EncodedPacket packet;
    std::uint32_t generation{};
    std::uint64_t sequence{};
    // When the encoder returned the packet.
    Clock::time_point encoded_at{};
};

// Running average and maximum of a duration.
struct DurationStats {
    std::uint64_t count{};
    Clock::duration total{};
    Clock::duration max{};

    void add(Clock::duration value) noexcept {
        if (value < Clock::duration::zero()) value = Clock::duration::zero();
        ++count;
        total += value;
        max = std::max(max, value);
    }
};

// Pipeline counters since the previous StreamPipeline::take_stats (PROTOCOL.md §5.15).
struct PipelineStats {
    Clock::duration elapsed{};
    std::uint64_t frames_captured{};  // Encoded pictures with new source content.
    std::uint64_t frames_encoded{};   // Including repeats of an unchanged source.
    std::uint64_t key_frames{};
    std::uint64_t frames_skipped{};   // Frame-rate ticks missed because capture/encoding was too slow.
    std::uint64_t encoded_bytes{};
    DurationStats capture;            // Source presentation -> encoder submission (new content only).
    DurationStats encode;             // Encoder submission -> packet available.
    Clock::duration busy{};           // Time spent capturing, encoding and reconfiguring.
};

// How a rate change was applied (PROTOCOL.md §5.14 `stream_rate.applied`).
enum class RateChangeResult {
    Unchanged,     // The requested values were already in effect.
    InPlace,       // New bitrate within the current sequence: same generation, no IDR.
    Reconfigured,  // Encoder restarted: new generation, IDR and `reconfigured` before this report.
};

// All callbacks run on the pipeline thread.
struct PipelineCallbacks {
    std::function<void(const VideoParameters&)> ready;
    // code is a PROTOCOL.md error code: capture_failed or encoder_failed.
    std::function<void(std::string code, std::string message)> start_failed;
    std::function<void(const VideoParameters&)> reconfigured;
    // Every rate change request up to and including `serial` has been applied.
    std::function<void(const VideoParameters&, std::uint64_t serial, RateChangeResult)> rate_changed;
    std::function<void(std::shared_ptr<const EncodedFrame>)> frame;
    // reason is a PROTOCOL.md stream_stopped reason: source_closed or error.
    std::function<void(std::string reason, std::string message)> stopped;
};

// Owns one capture + encode loop on a dedicated thread. The first frame is encoded during
// startup to learn exact decoder parameters; frames are delivered only after activate().
class StreamPipeline {
public:
    StreamPipeline(std::unique_ptr<capture::Capturer> capturer, std::unique_ptr<media::Encoder> encoder,
                   capture::CaptureTarget target, media::EncoderOptions options);
    ~StreamPipeline();
    StreamPipeline(const StreamPipeline&) = delete;
    StreamPipeline& operator=(const StreamPipeline&) = delete;

    void start(PipelineCallbacks callbacks);
    void activate() noexcept;
    void request_key_frame() noexcept;
    // Queues a bitrate, peak (0 = automatic) and/or frame-rate change, applied once the stream is active; later
    // requests override the values of pending ones. Returns the serial reported by `rate_changed`.
    std::uint64_t request_rate_change(std::optional<std::uint32_t> bitrate_kbps,
                                      std::optional<std::uint32_t> max_bitrate_kbps,
                                      std::optional<std::uint32_t> frame_rate);
    // Thread-safe: returns and resets the counters.
    PipelineStats take_stats();
    // Joins the thread; no callbacks run after this returns.
    void stop() noexcept;

private:
    void run() noexcept;
    void loop();

    std::unique_ptr<capture::Capturer> capturer_;
    std::unique_ptr<media::Encoder> encoder_;
    capture::CaptureTarget target_;
    media::EncoderOptions options_;
    PipelineCallbacks callbacks_;
    std::atomic<bool> stop_requested_{};
    std::atomic<bool> active_{};
    std::atomic<bool> key_frame_requested_{};
    struct RateChange {
        std::optional<std::uint32_t> bitrate_kbps;
        std::optional<std::uint32_t> max_bitrate_kbps;
        std::optional<std::uint32_t> frame_rate;
        std::uint64_t serial{};
    };
    std::mutex rate_mutex_;
    RateChange rate_change_;
    std::uint64_t rate_serial_{};
    std::optional<RateChange> take_rate_change();
    std::mutex stats_mutex_;
    PipelineStats stats_;
    Clock::time_point stats_since_{Clock::now()};
    std::thread thread_;
};

} // namespace beamit::host
