#include "../../shared/PcmQueue.h"
#include "../../shared/CaptureClock.h"

#include <cstdio>
#include <vector>

namespace {
unsigned failures{};
unsigned checks{};

void check(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

BeamItMicPcmHeader packet(UINT64 sequence, UINT32 flags = 0) {
    return {BEAMIT_MIC_DRIVER_VERSION, flags, sequence,
            sequence * BEAMIT_MIC_FRAMES_PER_PACKET, BEAMIT_MIC_FRAMES_PER_PACKET, 0};
}
} // namespace

int self_tests() {
    using beamit::mic::PcmQueue;
    PcmQueue queue;
    std::vector<INT16> input(BEAMIT_MIC_FRAMES_PER_PACKET * BEAMIT_MIC_CHANNELS);
    std::vector<INT16> output(input.size() * BEAMIT_MIC_QUEUE_PACKETS, -1);
    for (UINT32 index = 0; index < input.size(); ++index) input[index] = static_cast<INT16>(index + 1);
    check(!queue.valid(packet(0)), "first packet requires discontinuity");
    const auto first = packet(0, BeamItMicPcmDiscontinuity);
    check(queue.valid(first), "initial packet");
    check(queue.write(first, input.data()) == 0, "initial write fits");
    check(queue.queued() == BEAMIT_MIC_FRAMES_PER_PACKET, "queue frames");
    check(!queue.valid(first), "replay rejected, including discontinuity");
    check(!queue.valid(packet(2)), "gap requires discontinuity");
    check(queue.valid(packet(2, BeamItMicPcmDiscontinuity)), "explicit discontinuous gap");

    const auto half = BEAMIT_MIC_FRAMES_PER_PACKET / 2;
    check(queue.read(output.data(), half) == 0, "partial read");
    for (UINT32 index = 0; index < half * BEAMIT_MIC_CHANNELS; ++index)
        check(output[index] == input[index], "partial PCM matches");
    check(queue.read(output.data(), BEAMIT_MIC_FRAMES_PER_PACKET) == half, "underrun frame count");
    for (UINT32 index = 0; index < half * BEAMIT_MIC_CHANNELS; ++index) {
        check(output[index] == input[index + half * BEAMIT_MIC_CHANNELS], "remaining PCM matches");
        check(output[index + half * BEAMIT_MIC_CHANNELS] == 0, "underrun supplies zero");
    }
    for (UINT64 index = 1; index <= BEAMIT_MIC_QUEUE_PACKETS; ++index) {
        const auto next = packet(index);
        check(queue.valid(next), "continuous next packet");
        check(queue.write(next, input.data()) == 0, "bounded queue fills");
    }
    std::vector<INT16> newest(input.size(), -1234);
    check(queue.write(packet(4), newest.data()) == BEAMIT_MIC_FRAMES_PER_PACKET, "overflow drops oldest frames");
    check(queue.queued() == PcmQueue::capacity_frames, "overflow stays bounded");
    queue.read(output.data(), PcmQueue::capacity_frames);
    for (UINT32 index = 0; index < input.size(); ++index)
        check(output[output.size() - input.size() + index] == newest[index], "newest packet retained");

    queue.write(packet(5), input.data());
    queue.write(packet(7, BeamItMicPcmDiscontinuity), newest.data());
    check(queue.queued() == BEAMIT_MIC_FRAMES_PER_PACKET, "discontinuity clears queued samples");
    queue.reset();
    check(queue.queued() == 0, "lease reset clears queue");
    check(!queue.valid(packet(8)), "reset requires discontinuity");
    check(queue.read(output.data(), PcmQueue::capacity_frames) == PcmQueue::capacity_frames, "empty read is silence");
    for (auto value : output) check(value == 0, "no stale audio after reset");
    auto invalid = first;
    invalid.version = 0;
    check(!queue.valid(invalid), "bad version");
    invalid = first;
    invalid.flags |= 2;
    check(!queue.valid(invalid), "unknown flag");
    invalid = first;
    invalid.reserved = 1;
    check(!queue.valid(invalid), "reserved field");
    invalid = first;
    invalid.frame_count = MAXUINT32;
    check(!queue.valid(invalid), "bad packet length");
    invalid = first;
    invalid.sample_timestamp = 1;
    check(!queue.valid(invalid), "bad timestamp");
    invalid = first;
    invalid.sequence = MAXUINT64;
    check(!queue.valid(invalid), "timestamp multiplication overflow");
    check(beamit::mic::elapsed_frames(0, 10'000'000) == 0, "clock starts at zero");
    check(beamit::mic::elapsed_frames(10'000, 10'000'000) == 48, "one ms capture clock");
    check(beamit::mic::elapsed_frames(100'000, 10'000'000) == BEAMIT_MIC_FRAMES_PER_PACKET, "ten ms capture clock");
    check(beamit::mic::elapsed_frames(1'000'000'000, 10'000'000) == 4'800'000, "long running clock");
    check(beamit::mic::elapsed_frames(123'456'789, 10'000'000) == 592'592, "fractional elapsed clock");
    check(beamit::mic::elapsed_frames(3'000'000, 3'000'000) == 48'000, "different QPC frequency");
    check(beamit::mic::frame_ticks(480, 10'000'000) == 100'000, "packet start timestamp conversion");
    check(beamit::mic::frame_ticks(48'000ULL * 100'000'000, 10'000'000) == 1'000'000'000'000'000ULL,
          "long running timestamp without intermediate multiplication overflow");
    std::printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
