#include "FrameProtocol.hpp"

#include "net/Protocol.hpp"

#include <limits>
#include <stdexcept>

namespace beamit::client {
namespace {

std::uint64_t read_be(std::span<const std::byte> bytes, std::size_t offset, std::size_t width) {
    std::uint64_t value{};
    for (std::size_t index = 0; index < width; ++index) {
        value = (value << 8) | std::to_integer<unsigned char>(bytes[offset + index]);
    }
    return value;
}

} // namespace

ParsedFrame parse_frame(std::span<const std::byte> message) {
    namespace protocol = net::protocol;
    if (message.size() < protocol::frame_header_size) {
        throw std::invalid_argument("Truncated video frame header");
    }
    if (std::to_integer<unsigned>(message[0]) != protocol::frame_version) {
        throw std::invalid_argument("Unsupported video frame version");
    }
    const auto header_size = static_cast<std::size_t>(read_be(message, 2, 2));
    if (header_size < protocol::frame_header_size || header_size > message.size()) {
        throw std::invalid_argument("Invalid video frame header length");
    }
    if (std::to_integer<unsigned>(message[1]) != protocol::message_video) return {};
    if (header_size == message.size()) throw std::invalid_argument("Empty video access unit");
    const auto generation = static_cast<std::uint32_t>(read_be(message, 8, 4));
    if (generation == 0) throw std::invalid_argument("Video generation must be positive");
    const auto flags = static_cast<std::uint32_t>(read_be(message, 4, 4));
    return {FrameKind::Video, {
        generation,
        read_be(message, 16, 8),
        read_be(message, 24, 8),
        (flags & protocol::flag_key_frame) != 0,
        (flags & protocol::flag_discontinuity) != 0,
        message.subspan(header_size)
    }};
}

FrameGate::FrameGate(std::uint32_t generation) : generation_(generation) {
    if (generation == 0) throw std::invalid_argument("Generation must be positive");
}

void FrameGate::reconfigure(std::uint32_t generation) {
    if (generation <= generation_) throw std::invalid_argument("Generation must advance");
    generation_ = generation;
    reset();
}

void FrameGate::reset() {
    has_sequence_ = false;
    waiting_for_key_frame_ = true;
    key_frame_requested_ = false;
}

FrameAction FrameGate::accept(const VideoFrame& frame) {
    if (frame.generation < generation_) return FrameAction::Ignore;
    if (frame.generation > generation_) return FrameAction::AwaitConfiguration;
    if (has_sequence_ && frame.sequence <= last_sequence_) return FrameAction::Ignore;
    const bool gap = has_sequence_ &&
        (last_sequence_ == std::numeric_limits<std::uint64_t>::max() || frame.sequence != last_sequence_ + 1);
    last_sequence_ = frame.sequence;
    has_sequence_ = true;
    if (gap || frame.discontinuity) waiting_for_key_frame_ = true;
    if (frame.key_frame) {
        waiting_for_key_frame_ = false;
        key_frame_requested_ = false;
        return FrameAction::Decode;
    }
    if (!waiting_for_key_frame_) return FrameAction::Decode;
    if (key_frame_requested_) return FrameAction::Ignore;
    key_frame_requested_ = true;
    return FrameAction::RequestKeyFrame;
}

} // namespace beamit::client
