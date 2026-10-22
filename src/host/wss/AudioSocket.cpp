#include "host/wss/AudioSocket.hpp"

#include <spdlog/spdlog.h>
#include <algorithm>

namespace beamit::host::wss {

json::object audio_configuration_json(const media::AudioConfiguration& configuration) {
    configuration.validate();
    return {{"codec", "opus"}, {"opusAudioConfiguration", json::object{
        {"sampleRate", configuration.sample_rate}, {"channels", configuration.channels},
        {"frameSize", configuration.frame_size()}, {"packetDuration", configuration.duration_text()},
        {"useFec", media::audio_use_fec}, {"application", media::audio_application}}}};
}

AudioSocket::AudioSocket(std::shared_ptr<WebSocket> socket, std::shared_ptr<AudioEndpoint> endpoint,
                         media::AudioConfiguration configuration)
    : ws_(std::move(socket)), endpoint_(std::move(endpoint)), configuration_(configuration) {}

void AudioSocket::start() {
    add_track({.id = protocol::microphone_track_id, .configuration = configuration_});
    asio::co_spawn(ws_->get_executor(), read_loop(shared_from_this()), asio::detached);
}

void AudioSocket::add_track(const AudioTrack& track) {
    if (closing_ || tracks_.contains(track.id)) throw std::logic_error("Audio track is closed or already exists");
    auto pipeline = std::make_shared<AudioPipeline>(track.configuration,
        [executor = ws_->get_executor(), weak = weak_from_this()] {
            asio::post(executor, [weak] {
                if (auto self = weak.lock()) self->output_ready();
            });
        });
    tracks_.emplace(track.id, TrackState{track, pipeline, {}});
    try {
        endpoint_->attach(track, pipeline);
    } catch (...) {
        tracks_.erase(track.id);
        throw;
    }
}

void AudioSocket::remove_track(std::uint32_t track_id) {
    const auto found = tracks_.find(track_id);
    if (found == tracks_.end()) return;
    endpoint_->detach(track_id, found->second.pipeline);
    found->second.pipeline->stop();
    tracks_.erase(found);
}

void AudioSocket::close(websocket::close_reason reason) {
    if (closing_) return;
    closing_ = true;
    while (!tracks_.empty()) remove_track(tracks_.begin()->first);
    close_reason_ = std::move(reason);
    pump();
}

void AudioSocket::fail(std::string reason, std::string message) {
    if (closing_) return;
    spdlog::warn("Audio socket ended: {} - {}", reason, message);
    close({reason == "invalid_packet" ? websocket::close_code::bad_payload : websocket::close_code::internal_error,
           reason});
    if (on_ended) on_ended(std::move(reason), std::move(message));
}

void AudioSocket::output_ready() {
    if (closing_) return;
    for (auto& [id, track] : tracks_) {
        if (auto error = track.pipeline->error(); !error.empty()) return fail("codec_failed", std::move(error));
        while (auto packet = track.pipeline->receive_encoded()) {
            packet->track_id = id;
            if (track.outgoing.size() >= max_queued_audio_packets) {
                track.outgoing.pop_front();
                track.outgoing.front().discontinuity = true;
                spdlog::warn("Audio track {} network queue full; dropping oldest packet", id);
            }
            track.outgoing.push_back(std::move(*packet));
        }
    }
    pump();
}

void AudioSocket::pump() {
    const bool pending = std::ranges::any_of(tracks_, [](const auto& item) { return !item.second.outgoing.empty(); });
    if (writing_ || (!pending && !close_reason_)) return;
    writing_ = true;
    asio::co_spawn(ws_->get_executor(), write_loop(shared_from_this()), asio::detached);
}

awaitable<void> AudioSocket::write_loop(std::shared_ptr<AudioSocket> self) {
    try {
        while (true) {
            // Round-robin scheduling prevents a busy capture source starving the other tracks.
            auto found = self->tracks_.upper_bound(self->last_written_track_);
            if (found == self->tracks_.end()) found = self->tracks_.begin();
            const auto count = self->tracks_.size();
            std::size_t scanned{};
            while (scanned < count && found->second.outgoing.empty()) {
                ++scanned;
                if (++found == self->tracks_.end()) found = self->tracks_.begin();
            }
            if (scanned == count) break;
            auto packet = std::move(found->second.outgoing.front());
            found->second.outgoing.pop_front();
            self->last_written_track_ = found->first;
            const auto bytes = protocol::encode_audio_packet(packet, found->second.description.configuration);
            co_await self->ws_->async_write(asio::buffer(bytes), use_awaitable);
        }
        if (self->close_reason_) {
            const auto reason = *self->close_reason_;
            self->close_reason_.reset();
            co_await self->ws_->async_close(reason, use_awaitable);
        }
    } catch (const std::exception& error) {
        self->fail("disconnected", error.what());
        beast::error_code ignored;
        beast::get_lowest_layer(*self->ws_).close(ignored);
    }
    self->writing_ = false;
}

awaitable<void> AudioSocket::read_loop(std::shared_ptr<AudioSocket> self) {
    beast::flat_buffer buffer;
    try {
        while (!self->closing_) {
            buffer.clear();
            co_await self->ws_->async_read(buffer, use_awaitable);
            if (self->closing_) break;
            if (!self->ws_->got_binary()) {
                self->fail("invalid_packet", "Audio socket accepts binary messages only");
                break;
            }
            const auto data = buffer.cdata();
            net::protocol::AudioPacket packet;
            try {
                packet = protocol::decode_audio_packet(
                    {static_cast<const std::byte*>(data.data()), data.size()},
                    self->configuration_, self->received_sequence_);
            } catch (const std::invalid_argument& error) {
                self->fail("invalid_packet", error.what());
                break;
            }
            if (packet.track_id != protocol::microphone_track_id) {
                self->fail("invalid_packet", "Clients may send only on the offered microphone track");
                break;
            }
            self->received_sequence_ = packet.sequence;
            if (!self->tracks_.at(protocol::microphone_track_id).pipeline->submit_encoded(std::move(packet))) {
                self->fail("overloaded", "Audio decode queue full or codec stopped");
                break;
            }
        }
    } catch (const std::exception& error) {
        self->fail("disconnected", error.what());
    }
}

} // namespace beamit::host::wss
