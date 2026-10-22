#pragma once

#include "BeamItMicIoctl.h"

namespace beamit::mic {

inline UINT64 elapsed_frames(UINT64 ticks, UINT64 frequency) {
    return (ticks / frequency) * BEAMIT_MIC_SAMPLE_RATE_HZ +
        (ticks % frequency) * BEAMIT_MIC_SAMPLE_RATE_HZ / frequency;
}

inline UINT64 frame_ticks(UINT64 frames, UINT64 frequency) {
    return (frames / BEAMIT_MIC_SAMPLE_RATE_HZ) * frequency +
        (frames % BEAMIT_MIC_SAMPLE_RATE_HZ) * frequency / BEAMIT_MIC_SAMPLE_RATE_HZ;
}

} // namespace beamit::mic
