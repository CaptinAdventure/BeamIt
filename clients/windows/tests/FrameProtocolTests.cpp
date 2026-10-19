#include "FrameProtocol.hpp"
#include "net/Protocol.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

template<class Function>
void rejects(Function function, std::string_view message) {
    try {
        function();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error(std::string(message));
}

std::vector<std::byte> packet() {
    const auto header = beamit::net::protocol::encode_frame_header({3, 7, 1234, 9'876'543});
    std::vector<std::byte> bytes(header.begin(), header.end());
    for (unsigned value : {0, 0, 0, 1, 0x65, 0x88}) bytes.push_back(static_cast<std::byte>(value));
    return bytes;
}

void parsing() {
    using namespace beamit::client;
    auto bytes = packet();
    const auto frame = parse_frame(bytes);
    require(frame.kind == FrameKind::Video, "video type");
    require(frame.video.generation == 7 && frame.video.sequence == 1234, "network byte order");
    require(frame.video.capture_time_us == 9'876'543, "capture timestamp");
    require(frame.video.key_frame && frame.video.discontinuity, "flags");
    require(frame.video.payload.size() == 6 && frame.video.payload.data() == bytes.data() + 32, "borrowed payload");

    for (std::size_t size = 0; size < 32; ++size) {
        rejects([&] { (void)parse_frame(std::span(bytes).first(size)); }, "truncated header accepted");
    }
    bytes[0] = std::byte{2};
    rejects([&] { (void)parse_frame(bytes); }, "unknown version accepted");
    bytes = packet();
    bytes[3] = std::byte{31};
    rejects([&] { (void)parse_frame(bytes); }, "short header accepted");
    bytes[3] = std::byte{255};
    rejects([&] { (void)parse_frame(bytes); }, "out of bounds header accepted");
    bytes = packet();
    bytes.resize(32);
    rejects([&] { (void)parse_frame(bytes); }, "empty access unit accepted");
    bytes = packet();
    bytes[1] = std::byte{99};
    require(parse_frame(bytes).kind == FrameKind::Unknown, "unknown message not ignored");
    bytes = packet();
    bytes[11] = std::byte{0};
    rejects([&] { (void)parse_frame(bytes); }, "zero generation accepted");
    bytes = packet();
    bytes.insert(bytes.begin() + 32, 4, std::byte{0});
    bytes[3] = std::byte{36};
    require(parse_frame(bytes).video.payload.size() == 6, "extended header");
    bytes[4] |= std::byte{0x80};
    require(parse_frame(bytes).video.payload.size() == 6, "unknown flags");
}

void gating() {
    using namespace beamit::client;
    rejects([] { FrameGate gate(0); }, "zero generation accepted");
    FrameGate gate(7);
    VideoFrame frame{7, 10, 1, false, false, {}};
    require(gate.accept(frame) == FrameAction::RequestKeyFrame, "initial P decoded");
    ++frame.sequence;
    require(gate.accept(frame) == FrameAction::Ignore, "repeated keyframe requests");
    frame.key_frame = true;
    ++frame.sequence;
    require(gate.accept(frame) == FrameAction::Decode, "IDR rejected");
    require(gate.accept(frame) == FrameAction::Ignore, "duplicate decoded");
    frame.key_frame = false;
    ++frame.sequence;
    require(gate.accept(frame) == FrameAction::Decode, "consecutive P rejected");
    frame.sequence += 2;
    require(gate.accept(frame) == FrameAction::RequestKeyFrame, "reference gap ignored");
    ++frame.sequence;
    require(gate.accept(frame) == FrameAction::Ignore, "P decoded after gap");
    frame.key_frame = true;
    ++frame.sequence;
    require(gate.accept(frame) == FrameAction::Decode, "recovery IDR rejected");
    frame.key_frame = false;
    frame.discontinuity = true;
    ++frame.sequence;
    require(gate.accept(frame) == FrameAction::RequestKeyFrame, "discontinuity ignored");
    frame.generation = 8;
    require(gate.accept(frame) == FrameAction::AwaitConfiguration, "future generation did not await configuration");
    frame.generation = 6;
    require(gate.accept(frame) == FrameAction::Ignore, "stale generation decoded");
    rejects([&] { gate.reconfigure(7); }, "same generation reconfigured");
    gate.reconfigure(8);
    frame = {8, 0, 1, true, false, {}};
    require(gate.accept(frame) == FrameAction::Decode, "new generation with reset sequence rejected");
    gate.reset();
    frame.key_frame = false;
    require(gate.accept(frame) == FrameAction::RequestKeyFrame, "decoder reset did not gate");
}

} // namespace

int main() {
    try {
        parsing();
        gating();
        std::cout << "Windows client frame protocol checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
