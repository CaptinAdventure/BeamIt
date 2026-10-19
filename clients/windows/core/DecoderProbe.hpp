#pragma once

#include <cstddef>
#include <span>
#include <string>

namespace beamit::client {

[[nodiscard]] std::wstring decoder_inventory_report();

struct DecoderProbeResult {
    bool decoded{};
    std::wstring report;
};

// Diagnostic only: submit one complete Annex B IDR access unit, including parameter sets.
[[nodiscard]] DecoderProbeResult probe_decoder(bool hevc, unsigned width, unsigned height,
                                               std::span<const std::byte> access_unit);

} // namespace beamit::client
