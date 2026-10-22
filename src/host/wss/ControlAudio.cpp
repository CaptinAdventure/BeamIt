#include "host/wss/ControlSession.hpp"

#include "net/Crypto.hpp"

#include <spdlog/spdlog.h>
#include <utility>

namespace beamit::host::wss {
namespace {

json::object track_json(const AudioTrack& track) {
    auto result = audio_configuration_json(track.configuration);
    result["track_id"] = track.id;
    if (track.kind == AudioTrack::Kind::Desktop) {
        result["source"] = json::object{{"kind", "desktop"}, {"exclude_host_process_tree", true}};
    } else if (track.kind == AudioTrack::Kind::Application) {
        result["source"] = json::object{{"kind", "application"}, {"pid", track.pid}, {"include_process_tree", true}};
    } else {
        result["source"] = json::object{{"kind", "microphone"}};
    }
    return result;
}

} // namespace

void ControlSession::start_audio(const json::object&, const RequestId& id) {
    if (audio_) throw ProtocolError{"limit_reached", "Only one audio socket per authenticated session is allowed"};
    if (!server_.options.audio) throw ProtocolError{"audio_failed", "Host audio endpoint is not configured"};
    auto entry = std::make_shared<AudioEntry>();
    entry->id = net::base64url_encode(net::random_bytes(16));
    entry->token = net::base64url_encode(net::random_bytes(32));
    entry->token_expiry = Clock::now() + protocol::token_lifetime;
    entry->token_timer = std::make_unique<asio::steady_timer>(ws_->get_executor(), entry->token_expiry);
    entry->token_timer->async_wait([weak = weak_from_this(), weak_entry = std::weak_ptr(entry)](beast::error_code error) {
        const auto self = weak.lock();
        const auto audio = weak_entry.lock();
        if (!error && self && audio && self->audio_ == audio) self->end_audio("token_expired");
    });
    const auto microphone = track_json({.id = protocol::microphone_track_id, .configuration = entry->configuration});
    audio_ = entry;
    reply(id, {{"type", "audio_offer"}, {"audio_id", entry->id},
        {"url", fmt::format("wss://{}/audio/{}?token={}", authority_, entry->id, entry->token)},
        {"token_expires_in_ms", std::chrono::duration_cast<std::chrono::milliseconds>(protocol::token_lifetime).count()},
        {"audio_version", protocol::audio_frame_version},
        {"hostToClient", json::object{{"tracks", json::array{}}}}, {"clientToHost", microphone}});
}

std::shared_ptr<AudioEntry> ControlSession::find_audio(const json::object& message) {
    const auto requested = required_string(message, "audio_id");
    if (!audio_ || audio_->id != requested) throw ProtocolError{"not_found", "Unknown audio_id"};
    return audio_;
}

void ControlSession::stop_audio(const json::object& message, const RequestId& id) {
    (void)find_audio(message);
    end_audio("requested", {}, id);
}

void ControlSession::start_audio_track(const json::object& message, const RequestId& id) {
    const auto entry = find_audio(message);
    if (!entry->socket) throw ProtocolError{"not_ready", "Open the audio socket before adding capture tracks"};
    if (entry->capture_tracks.size() >= protocol::max_audio_capture_tracks ||
        entry->next_track_id == UINT32_MAX) {
        throw ProtocolError{"limit_reached", "Audio capture track limit reached"};
    }
    const auto* source = optional_object(message, "source");
    if (!source) throw ProtocolError{"invalid_request", "source is required"};
    const auto kind = required_string(*source, "kind");
    AudioTrack track{.id = entry->next_track_id, .configuration = entry->configuration};
    if (kind == "desktop") {
        track.kind = AudioTrack::Kind::Desktop;
    } else if (kind == "application") {
        track.kind = AudioTrack::Kind::Application;
        track.pid = static_cast<std::uint32_t>(required_integer(*source, "pid", 1, UINT32_MAX));
        if (track.pid == GetCurrentProcessId()) {
            throw ProtocolError{"invalid_request", "BeamIt itself cannot be an application audio source"};
        }
    } else {
        throw ProtocolError{"invalid_request", "Audio source.kind must be desktop or application"};
    }
    for (const auto& [track_id, current] : entry->capture_tracks) {
        if (current.kind == AudioTrack::Kind::Desktop || track.kind == AudioTrack::Kind::Desktop) {
            throw ProtocolError{"invalid_request", "Desktop audio cannot be combined with other capture tracks"};
        }
        if (current.pid == track.pid) throw ProtocolError{"invalid_request", "Application audio is already selected"};
    }
    entry->capture_tracks.emplace(track.id, track);
    try {
        entry->socket->add_track(track);
    } catch (...) {
        entry->capture_tracks.erase(track.id);
        throw;
    }
    ++entry->next_track_id;
    reply(id, {{"type", "audio_track_started"}, {"audio_id", entry->id}, {"track", track_json(track)}});
}

void ControlSession::stop_audio_track(const json::object& message, const RequestId& id) {
    const auto entry = find_audio(message);
    const auto track_id = static_cast<std::uint32_t>(required_integer(message, "track_id", 1, UINT32_MAX));
    if (track_id == protocol::microphone_track_id) {
        throw ProtocolError{"invalid_request", "The microphone route lasts until stop_audio"};
    }
    if (entry->capture_tracks.erase(track_id) == 0) throw ProtocolError{"not_found", "Unknown capture track"};
    entry->socket->remove_track(track_id);
    reply(id, {{"type", "audio_track_stopped"}, {"audio_id", entry->id}, {"track_id", track_id},
               {"reason", "requested"}});
}

bool ControlSession::has_audio(const std::shared_ptr<AudioEntry>& entry) const {
    return authenticated_ && !closing_ && audio_ == entry;
}

std::shared_ptr<AudioEntry> ControlSession::claim_audio(std::string_view audio_id, std::string_view token) {
    if (!audio_ || !has_audio(audio_) || audio_->id != audio_id || audio_->token_used ||
        Clock::now() >= audio_->token_expiry ||
        !net::constant_time_equal(std::as_bytes(std::span(audio_->token)), std::as_bytes(std::span(token)))) {
        return nullptr;
    }
    audio_->token_used = true;
    // Keep expiry armed until the upgrade completes, not just until the token is claimed.
    return audio_;
}

void ControlSession::attach_audio(const std::shared_ptr<AudioEntry>& entry, std::shared_ptr<WebSocket> socket) {
    try {
        entry->socket = std::make_shared<AudioSocket>(std::move(socket), server_.options.audio, entry->configuration);
        entry->socket->on_ended = [weak = weak_from_this(), weak_entry = std::weak_ptr(entry)](
                                      std::string reason, std::string message) {
            if (auto self = weak.lock(); self && self->audio_ == weak_entry.lock()) {
                self->end_audio(reason, message);
            }
        };
        entry->socket->start();
        entry->token_timer->cancel();
        spdlog::info("Audio connected");
    } catch (const std::exception& error) {
        spdlog::error("Audio initialization failed: {}", error.what());
        end_audio("codec_failed", error.what());
    }
}

void ControlSession::end_audio(std::string_view reason, std::string_view message, const RequestId& id) {
    auto entry = std::exchange(audio_, {});
    if (!entry) return;
    entry->token_timer->cancel();
    if (entry->socket) entry->socket->close({websocket::close_code::normal, reason});
    json::object stopped{{"type", "audio_stopped"}, {"audio_id", entry->id}, {"reason", reason}};
    if (!message.empty()) stopped["message"] = message;
    reply(id, std::move(stopped));
    spdlog::info("Audio stopped: {}", reason);
}

} // namespace beamit::host::wss
