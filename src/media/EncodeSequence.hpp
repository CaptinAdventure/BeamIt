#pragma once

#include <cstdint>

namespace beamit::media::detail {

inline constexpr unsigned gop_length = 60;
inline constexpr unsigned order_bits_minus4 = 4;

struct PictureOrder {
    unsigned index;
    unsigned output_slot;
    unsigned reference_slot;
    bool idr;
};

constexpr PictureOrder picture_order(std::uint64_t frame) noexcept {
    const auto index = static_cast<unsigned>(frame % gop_length);
    const auto slot = static_cast<unsigned>(frame % 2);
    return {index, slot, 1 - slot, index == 0};
}

} // namespace beamit::media::detail
