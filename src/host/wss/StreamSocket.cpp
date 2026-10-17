#include "host/wss/StreamSocket.hpp"

#include <spdlog/spdlog.h>

#include <array>

namespace beamit::host::wss {

bool StreamSocket::deliver(std::shared_ptr<const EncodedFrame> frame) {
    if (closing_) return true;
    bool discontinuity = false;
    bool recovered = true;
    if (!drop_until_key_ && queue_.size() >= max_queued) {
        stats_.frames_dropped += queue_.size();
        ++stats_.recoveries;
        queue_.clear();
        drop_until_key_ = true;
        recovered = frame->packet.key_frame;
        spdlog::warn("Stream {}: client is not keeping up; dropping frames until the next key frame", stream_id);
    }
    if (drop_until_key_) {
        if (!frame->packet.key_frame) {
            ++stats_.frames_dropped;
            return recovered;
        }
        drop_until_key_ = false;
        discontinuity = true;
    }
    queue_.push_back({std::move(frame), discontinuity});
    start_writing();
    return recovered;
}

void StreamSocket::close(websocket::close_reason reason) {
    if (closing_) return;
    closing_ = true;
    queue_.clear();
    pending_close_ = reason;
    start_writing();
}

void StreamSocket::start_writing() {
    if (writing_) return;
    writing_ = true;
    asio::co_spawn(ws->get_executor(), write_loop(shared_from_this()), asio::detached);
}

awaitable<void> StreamSocket::write_loop(std::shared_ptr<StreamSocket> self) {
    try {
        while (!self->queue_.empty()) {
            auto item = std::move(self->queue_.front());
            self->queue_.pop_front();
            const auto& packet = item.frame->packet;
            std::uint32_t flags = 0;
            if (packet.key_frame) flags |= protocol::flag_key_frame;
            if (item.discontinuity) flags |= protocol::flag_discontinuity;
            const auto header = protocol::encode_frame_header({
                .flags = flags,
                .generation = item.frame->generation,
                .sequence = item.frame->sequence,
                .capture_time_us = protocol::host_time_us(packet.captured_at),
            });
            const std::array<asio::const_buffer, 2> buffers{
                asio::buffer(header), asio::buffer(packet.data.data(), packet.data.size())};
            const auto write_start = Clock::now();
            co_await self->ws->async_write(buffers, use_awaitable);
            const auto written = Clock::now();
            auto& stats = self->stats_;
            ++stats.frames_sent;
            stats.bytes_sent += header.size() + packet.data.size();
            stats.write.add(written - write_start);
            stats.send.add(written - item.frame->encoded_at);
            stats.host_latency.add(written - packet.captured_at);
        }
        if (self->pending_close_) {
            auto reason = *self->pending_close_;
            self->pending_close_.reset();
            co_await self->ws->async_close(reason, use_awaitable);
        }
    } catch (const std::exception& error) {
        spdlog::debug("Stream {} write ended: {}", self->stream_id, error.what());
        beast::error_code ignored;
        beast::get_lowest_layer(*self->ws).close(ignored);
    }
    self->writing_ = false;
}

awaitable<void> StreamSocket::read_loop(std::shared_ptr<StreamSocket> self) {
    beast::flat_buffer buffer;
    try {
        while (true) {
            buffer.clear();
            co_await self->ws->async_read(buffer, use_awaitable);
            self->close({websocket::close_code::bad_payload, "clients must not send data on stream sockets"});
        }
    } catch (const std::exception& error) {
        spdlog::debug("Stream {} socket closed: {}", self->stream_id, error.what());
    }
    if (!self->closing_) {
        self->closing_ = true;
        if (self->on_disconnected) self->on_disconnected();
    }
}

} // namespace beamit::host::wss
