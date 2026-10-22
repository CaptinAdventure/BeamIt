#pragma once

#include "BeamItMicIoctl.h"

namespace beamit::mic {

// External synchronization is required; the capture DPC never allocates.
class PcmQueue {
public:
    static constexpr UINT32 capacity_frames = BEAMIT_MIC_FRAMES_PER_PACKET * BEAMIT_MIC_QUEUE_PACKETS;

    void reset() {
        head_ = count_ = 0;
        have_sequence_ = false;
        for (auto& sample : samples_) sample = 0;
    }

    bool valid(const BeamItMicPcmHeader& packet) const {
        if (packet.version != BEAMIT_MIC_DRIVER_VERSION ||
            packet.frame_count != BEAMIT_MIC_FRAMES_PER_PACKET || packet.reserved ||
            (packet.flags & ~BeamItMicPcmDiscontinuity)) return false;
        constexpr auto max_sequence = MAXUINT64 / BEAMIT_MIC_FRAMES_PER_PACKET;
        if (packet.sequence > max_sequence ||
            packet.sample_timestamp != packet.sequence * BEAMIT_MIC_FRAMES_PER_PACKET) return false;
        const bool discontinuity = (packet.flags & BeamItMicPcmDiscontinuity) != 0;
        if (!have_sequence_) return discontinuity;
        if (packet.sequence <= last_sequence_) return false;
        return discontinuity || packet.sequence == last_sequence_ + 1;
    }

    UINT32 write(const BeamItMicPcmHeader& packet, const INT16* samples) {
        if (packet.flags & BeamItMicPcmDiscontinuity) reset();
        UINT32 dropped{};
        const auto required = count_ + packet.frame_count;
        if (required > capacity_frames) {
            dropped = required - capacity_frames;
            head_ = (head_ + dropped) % capacity_frames;
            count_ -= dropped;
        }
        for (UINT32 frame = 0; frame < packet.frame_count; ++frame) {
            const auto target = (head_ + count_ + frame) % capacity_frames;
            for (UINT32 channel = 0; channel < BEAMIT_MIC_CHANNELS; ++channel)
                samples_[target * BEAMIT_MIC_CHANNELS + channel] = samples[frame * BEAMIT_MIC_CHANNELS + channel];
        }
        count_ += packet.frame_count;
        have_sequence_ = true;
        last_sequence_ = packet.sequence;
        return dropped;
    }

    UINT32 read(INT16* output, UINT32 frames) {
        const auto available = frames < count_ ? frames : count_;
        for (UINT32 frame = 0; frame < frames; ++frame) {
            for (UINT32 channel = 0; channel < BEAMIT_MIC_CHANNELS; ++channel) {
                output[frame * BEAMIT_MIC_CHANNELS + channel] = frame < available
                    ? samples_[((head_ + frame) % capacity_frames) * BEAMIT_MIC_CHANNELS + channel] : 0;
            }
        }
        head_ = (head_ + available) % capacity_frames;
        count_ -= available;
        return frames - available;
    }

    UINT32 queued() const { return count_; }

private:
    INT16 samples_[capacity_frames * BEAMIT_MIC_CHANNELS]{};
    UINT32 head_{};
    UINT32 count_{};
    UINT64 last_sequence_{};
    bool have_sequence_{};
};

} // namespace beamit::mic
