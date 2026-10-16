#include "media/BitstreamInfo.hpp"

#include <spdlog/fmt/fmt.h>

#include <array>
#include <vector>

namespace beamit::media {
namespace {

class BitReader {
public:
    explicit BitReader(std::span<const std::uint8_t> data) : data_(data) {}

    std::uint32_t bits(unsigned count) {
        std::uint32_t value = 0;
        for (unsigned index = 0; index < count; ++index) {
            if (position_ >= data_.size() * 8) {
                failed_ = true;
                return 0;
            }
            const auto bit = (data_[position_ / 8] >> (7 - position_ % 8)) & 1U;
            value = (value << 1) | bit;
            ++position_;
        }
        return value;
    }

    void skip(unsigned count) {
        position_ += count;
        if (position_ > data_.size() * 8) failed_ = true;
    }

    std::uint32_t ue() {
        unsigned zeros = 0;
        while (!failed_ && bits(1) == 0) {
            if (++zeros > 31) {
                failed_ = true;
                return 0;
            }
        }
        if (failed_) return 0;
        return ((1U << zeros) - 1) + bits(zeros);
    }

    [[nodiscard]] bool failed() const noexcept { return failed_; }

private:
    std::span<const std::uint8_t> data_;
    std::size_t position_{};
    bool failed_{};
};

// Returns the payload of the first NAL unit of the requested type, without start code.
std::span<const std::uint8_t> find_nal(std::span<const std::uint8_t> data, VideoCodec codec, unsigned type) {
    std::size_t index = 0;
    while (index + 3 <= data.size()) {
        if (data[index] == 0 && data[index + 1] == 0 && data[index + 2] == 1) {
            const auto start = index + 3;
            auto end = start;
            while (end + 3 <= data.size() &&
                   !(data[end] == 0 && data[end + 1] == 0 && (data[end + 2] == 1 ||
                     (data[end + 2] == 0 && end + 3 < data.size() && data[end + 3] == 1)))) {
                ++end;
            }
            if (end + 3 > data.size()) end = data.size();
            if (start < end) {
                const unsigned nal_type = codec == VideoCodec::H264 ? (data[start] & 0x1FU)
                                                                    : ((data[start] >> 1) & 0x3FU);
                if (nal_type == type) return data.subspan(start, end - start);
            }
            index = end;
        } else {
            ++index;
        }
    }
    return {};
}

std::vector<std::uint8_t> to_rbsp(std::span<const std::uint8_t> nal) {
    std::vector<std::uint8_t> rbsp;
    rbsp.reserve(nal.size());
    unsigned zeros = 0;
    for (const auto byte : nal) {
        if (zeros >= 2 && byte == 3) {
            zeros = 0;
            continue;
        }
        zeros = byte == 0 ? zeros + 1 : 0;
        rbsp.push_back(byte);
    }
    return rbsp;
}

std::uint32_t reverse_bits(std::uint32_t value) {
    std::uint32_t result = 0;
    for (int bit = 0; bit < 32; ++bit) {
        result = (result << 1) | (value & 1U);
        value >>= 1;
    }
    return result;
}

bool has_chroma_syntax(std::uint32_t profile_idc) {
    constexpr std::array profiles{100U, 110U, 122U, 244U, 44U, 83U, 86U, 118U, 128U, 138U, 139U, 134U, 135U};
    for (const auto profile : profiles) {
        if (profile == profile_idc) return true;
    }
    return false;
}

std::optional<BitstreamInfo> parse_h264(std::span<const std::uint8_t> data) {
    const auto rbsp = to_rbsp(find_nal(data, VideoCodec::H264, 7));
    if (rbsp.size() < 4) return std::nullopt;
    BitReader reader({rbsp.data() + 1, rbsp.size() - 1});
    BitstreamInfo info;
    const auto profile = reader.bits(8);
    const auto constraints = reader.bits(8);
    const auto level = reader.bits(8);
    info.codec_string = fmt::format("avc1.{:02X}{:02X}{:02X}", profile, constraints, level);
    reader.ue();
    if (has_chroma_syntax(profile)) {
        info.chroma_format_idc = reader.ue();
        if (info.chroma_format_idc == 3) reader.skip(1);
        info.bit_depth_luma = reader.ue() + 8;
        info.bit_depth_chroma = reader.ue() + 8;
    }
    if (reader.failed() || info.chroma_format_idc > 3) return std::nullopt;
    return info;
}

std::optional<BitstreamInfo> parse_h265(std::span<const std::uint8_t> data) {
    const auto rbsp = to_rbsp(find_nal(data, VideoCodec::H265, 33));
    if (rbsp.size() < 16) return std::nullopt;
    BitReader reader({rbsp.data() + 2, rbsp.size() - 2});
    reader.skip(4);
    const auto max_sub_layers_minus1 = reader.bits(3);
    reader.skip(1);
    const auto profile_space = reader.bits(2);
    const auto tier = reader.bits(1);
    const auto profile = reader.bits(5);
    const auto compatibility = reverse_bits(reader.bits(32));
    std::array<std::uint32_t, 6> constraints{};
    for (auto& byte : constraints) byte = reader.bits(8);
    const auto level = reader.bits(8);
    std::array<bool, 8> profile_present{};
    std::array<bool, 8> level_present{};
    for (unsigned layer = 0; layer < max_sub_layers_minus1; ++layer) {
        profile_present[layer] = reader.bits(1) != 0;
        level_present[layer] = reader.bits(1) != 0;
    }
    if (max_sub_layers_minus1 > 0) {
        for (unsigned layer = max_sub_layers_minus1; layer < 8; ++layer) reader.skip(2);
    }
    for (unsigned layer = 0; layer < max_sub_layers_minus1; ++layer) {
        if (profile_present[layer]) reader.skip(88);
        if (level_present[layer]) reader.skip(8);
    }
    BitstreamInfo info;
    reader.ue();
    info.chroma_format_idc = reader.ue();
    if (info.chroma_format_idc == 3) reader.skip(1);
    reader.ue();
    reader.ue();
    if (reader.bits(1) != 0) {
        for (int offset = 0; offset < 4; ++offset) reader.ue();
    }
    info.bit_depth_luma = reader.ue() + 8;
    info.bit_depth_chroma = reader.ue() + 8;
    if (reader.failed() || info.chroma_format_idc > 3) return std::nullopt;

    constexpr std::array<const char*, 4> spaces{"", "A", "B", "C"};
    info.codec_string = fmt::format("hvc1.{}{}.{:X}.{}{}", spaces[profile_space], profile, compatibility,
                                    tier ? 'H' : 'L', level);
    auto used = constraints.size();
    while (used > 0 && constraints[used - 1] == 0) --used;
    for (std::size_t index = 0; index < used; ++index) {
        info.codec_string += fmt::format(".{:02X}", constraints[index]);
    }
    return info;
}

} // namespace

std::optional<BitstreamInfo> parse_bitstream_info(std::span<const std::byte> annex_b, VideoCodec codec) {
    const std::span bytes{reinterpret_cast<const std::uint8_t*>(annex_b.data()), annex_b.size()};
    return codec == VideoCodec::H264 ? parse_h264(bytes) : parse_h265(bytes);
}

const char* chroma_format_name(std::uint32_t chroma_format_idc) noexcept {
    switch (chroma_format_idc) {
    case 0: return "4:0:0";
    case 1: return "4:2:0";
    case 2: return "4:2:2";
    case 3: return "4:4:4";
    default: return "unknown";
    }
}

} // namespace beamit::media
