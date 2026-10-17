// Periodic `stream_stats` reports (PROTOCOL.md §5.15).
#include "host/wss/ControlSession.hpp"

#include <cmath>

namespace beamit::host::wss {
namespace {

double milliseconds(Clock::duration value) {
    return std::round(std::chrono::duration<double, std::milli>(value).count() * 1000.0) / 1000.0;
}

// {avg, max} in milliseconds, or null without samples.
json::value duration_json(const DurationStats& stats) {
    if (stats.count == 0) return nullptr;
    return json::object{{"avg", milliseconds(stats.total / static_cast<Clock::duration::rep>(stats.count))},
                        {"max", milliseconds(stats.max)}};
}

std::uint64_t kbps(std::uint64_t bytes, Clock::duration elapsed) {
    const auto seconds = std::chrono::duration<double>(elapsed).count();
    return seconds > 0 ? static_cast<std::uint64_t>(std::llround(static_cast<double>(bytes) * 8 / 1000 / seconds)) : 0;
}

} // namespace

void ControlSession::schedule_stats() {
    if (closing_) return;
    if (!stats_timer_) {
        stats_since_ = Clock::now();
        stats_timer_ = std::make_unique<asio::steady_timer>(ws_->get_executor());
    }
    stats_timer_->expires_after(protocol::stats_interval);
    stats_timer_->async_wait([weak = weak_from_this()](beast::error_code error) {
        auto self = weak.lock();
        if (error || !self || self->closing_) return;
        self->send_stream_stats();
        self->schedule_stats();
    });
}

void ControlSession::send_stream_stats() {
    const auto now = Clock::now();
    json::array streams;
    for (const auto& [id, stream] : streams_) {
        if (!stream->ready) continue;
        const auto pipeline = stream->pipeline->take_stats();
        const auto network = stream->socket ? stream->socket->take_stats() : NetworkStats{};
        const auto elapsed_s = std::chrono::duration<double>(pipeline.elapsed).count();
        streams.push_back(json::object{
            {"stream_id", id},
            {"connected", static_cast<bool>(stream->socket)},
            {"generation", stream->video.generation},
            {"width", stream->video.width},
            {"height", stream->video.height},
            {"frame_rate", stream->video.frame_rate},
            {"bitrate_kbps", stream->video.bitrate_kbps},
            {"interval_ms", milliseconds(pipeline.elapsed)},
            {"frames_captured", pipeline.frames_captured},
            {"frames_encoded", pipeline.frames_encoded},
            {"key_frames", pipeline.key_frames},
            {"frames_sent", network.frames_sent},
            {"frames_dropped_encoder", pipeline.frames_skipped},
            {"frames_dropped_network", network.frames_dropped},
            {"network_recoveries", network.recoveries},
            {"queued_frames", stream->socket ? stream->socket->queued() : 0},
            {"encoded_kbps", kbps(pipeline.encoded_bytes, pipeline.elapsed)},
            {"sent_kbps", kbps(network.bytes_sent, pipeline.elapsed)},
            {"encoder_load", elapsed_s > 0 ? std::round(std::chrono::duration<double>(pipeline.busy).count() /
                                                        elapsed_s * 1000.0) / 1000.0
                                           : 0.0},
            {"capture_ms", duration_json(pipeline.capture)},
            {"encode_ms", duration_json(pipeline.encode)},
            {"send_ms", duration_json(network.send)},
            {"write_ms", duration_json(network.write)},
            {"host_latency_ms", duration_json(network.host_latency)},
        });
    }
    send({{"type", "stream_stats"},
          {"host_time_us", protocol::host_time_us(now)},
          {"interval_ms", milliseconds(now - std::exchange(stats_since_, now))},
          {"streams", std::move(streams)}});
}

} // namespace beamit::host::wss
