#include "media/BitstreamInfo.hpp"
#include "net/Crypto.hpp"
#include "net/Protocol.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

using beamit::media::VideoCodec;
namespace net = beamit::net;

std::vector<std::byte> bytes(std::string_view text) {
    std::vector<std::byte> result;
    for (const char character : text) result.push_back(static_cast<std::byte>(character));
    return result;
}

std::string hex(std::span<const std::byte> data) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    for (const auto byte : data) {
        text += digits[std::to_integer<unsigned>(byte) >> 4];
        text += digits[std::to_integer<unsigned>(byte) & 15];
    }
    return text;
}

class BitWriter {
public:
    void bits(std::uint64_t value, unsigned count) {
        for (unsigned index = count; index-- > 0;) bit((value >> index) & 1U);
    }
    void ue(std::uint32_t value) {
        const auto coded = static_cast<std::uint64_t>(value) + 1;
        unsigned length = 0;
        while ((coded >> length) > 1) ++length;
        bits(0, length);
        bits(coded, length + 1);
    }
    // Adds rbsp_trailing_bits, emulation prevention and a start code.
    std::vector<std::byte> nal(std::vector<std::uint8_t> header) {
        bit(1);
        while (used_ != 0) bit(0);
        header.insert(header.end(), data_.begin(), data_.end());
        std::vector<std::byte> out{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}};
        unsigned zeros = 0;
        for (const auto value : header) {
            if (zeros >= 2 && value <= 3) {
                out.push_back(std::byte{3});
                zeros = 0;
            }
            zeros = value == 0 ? zeros + 1 : 0;
            out.push_back(static_cast<std::byte>(value));
        }
        return out;
    }

private:
    void bit(unsigned value) {
        if (used_ == 0) data_.push_back(0);
        data_.back() |= static_cast<std::uint8_t>(value << (7 - used_));
        used_ = (used_ + 1) % 8;
    }
    std::vector<std::uint8_t> data_;
    unsigned used_{};
};

std::vector<std::byte> h264_sps(std::uint8_t profile, std::uint32_t chroma) {
    BitWriter writer;
    writer.bits(profile, 8);
    writer.bits(0, 8);
    writer.bits(0x1F, 8);
    writer.ue(0);
    if (profile == 100 || profile == 244) {
        writer.ue(chroma);
        if (chroma == 3) writer.bits(0, 1);
        writer.ue(0);
        writer.ue(0);
    }
    writer.ue(0);
    return writer.nal({0x67});
}

std::vector<std::byte> h265_sps(unsigned profile, std::uint32_t compatibility_bits, std::uint8_t first_constraint,
                                unsigned level, std::uint32_t chroma, std::uint32_t bit_depth_minus8) {
    BitWriter writer;
    writer.bits(0, 4);
    writer.bits(0, 3);
    writer.bits(1, 1);
    writer.bits(0, 2);
    writer.bits(0, 1);
    writer.bits(profile, 5);
    writer.bits(compatibility_bits, 32);
    writer.bits(first_constraint, 8);
    writer.bits(0, 40);
    writer.bits(level, 8);
    writer.ue(0);
    writer.ue(chroma);
    if (chroma == 3) writer.bits(0, 1);
    writer.ue(1920);
    writer.ue(1080);
    writer.bits(1, 1);
    for (int offset = 0; offset < 4; ++offset) writer.ue(offset == 3 ? 4 : 0);
    writer.ue(bit_depth_minus8);
    writer.ue(bit_depth_minus8);
    return writer.nal({0x42, 0x01});
}

std::vector<std::byte> with_aud(std::vector<std::byte> sps, VideoCodec codec) {
    std::vector<std::byte> stream = codec == VideoCodec::H264
        ? std::vector<std::byte>{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}, std::byte{0x09}, std::byte{0xF0}}
        : std::vector<std::byte>{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}, std::byte{0x46}, std::byte{0x01},
                                 std::byte{0x50}};
    stream.insert(stream.end(), sps.begin(), sps.end());
    return stream;
}

} // namespace

TEST_CASE("base64url follows RFC 4648 section 5 without padding", "[protocol]") {
    CHECK(net::base64url_encode(bytes("")) == "");
    CHECK(net::base64url_encode(bytes("f")) == "Zg");
    CHECK(net::base64url_encode(bytes("fo")) == "Zm8");
    CHECK(net::base64url_encode(bytes("foo")) == "Zm9v");
    CHECK(net::base64url_encode(bytes("foobar")) == "Zm9vYmFy");
    const std::vector<std::byte> url_safe{std::byte{0xFB}, std::byte{0xFF}};
    CHECK(net::base64url_encode(url_safe) == "-_8");
    CHECK(net::base64url_decode("-_8") == url_safe);
    CHECK(net::base64url_decode("Zm9vYmFy") == bytes("foobar"));
    CHECK_FALSE(net::base64url_decode("Zm9v=").has_value());
    CHECK_FALSE(net::base64url_decode("Z").has_value());
    CHECK_FALSE(net::base64url_decode("Zh").has_value());
    CHECK_FALSE(net::base64url_decode("Zm+v").has_value());
}

TEST_CASE("CNG HMAC-SHA256 and PBKDF2 match published vectors", "[protocol][crypto]") {
    // RFC 4231 test case 2.
    CHECK(hex(net::hmac_sha256(bytes("Jefe"), bytes("what do ya want for nothing?"))) ==
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    // RFC 7914 section 11 / common PBKDF2-HMAC-SHA256 vectors.
    CHECK(hex(net::pbkdf2_sha256("password", bytes("salt"), 1)) ==
          "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    CHECK(hex(net::pbkdf2_sha256("password", bytes("salt"), 2)) ==
          "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43");
}

TEST_CASE("Authentication proofs bind role and both nonces", "[protocol][crypto]") {
    const auto key = net::pbkdf2_sha256("correct horse battery staple", bytes("0123456789abcdef"), 1000);
    const auto server_nonce = net::random_bytes(32);
    const auto client_nonce = net::random_bytes(32);
    auto message = bytes("BeamIt v1 client");
    message.push_back(std::byte{0});
    message.insert(message.end(), server_nonce.begin(), server_nonce.end());
    message.insert(message.end(), client_nonce.begin(), client_nonce.end());
    const auto client = net::auth_proof(key, "client", server_nonce, client_nonce);
    CHECK(client == net::hmac_sha256(key, message));
    CHECK(client != net::auth_proof(key, "server", server_nonce, client_nonce));
    CHECK(client != net::auth_proof(key, "client", client_nonce, server_nonce));
    CHECK(net::constant_time_equal(client, net::hmac_sha256(key, message)));
    CHECK_FALSE(net::constant_time_equal(client, std::span(client).first(31)));
}

TEST_CASE("Binary frame header is 32 big-endian bytes", "[protocol]") {
    const auto header = beamit::net::protocol::encode_frame_header({
        .flags = beamit::net::protocol::flag_key_frame | beamit::net::protocol::flag_discontinuity,
        .generation = 0x01020304,
        .sequence = 0x1112131415161718ULL,
        .capture_time_us = 0x2122232425262728ULL,
    });
    CHECK(hex(header) ==
          "01" "01" "0020" "00000003" "01020304" "00000000" "1112131415161718" "2122232425262728");
}

TEST_CASE("H.264 SPS yields WebCodecs codec string and sampling", "[protocol][bitstream]") {
    const auto high444 = beamit::media::parse_bitstream_info(with_aud(h264_sps(244, 3), VideoCodec::H264),
                                                            VideoCodec::H264);
    REQUIRE(high444);
    CHECK(high444->codec_string == "avc1.F4001F");
    CHECK(high444->chroma_format_idc == 3);
    CHECK(high444->bit_depth_luma == 8);

    const auto main = beamit::media::parse_bitstream_info(h264_sps(77, 1), VideoCodec::H264);
    REQUIRE(main);
    CHECK(main->codec_string == "avc1.4D001F");
    CHECK(main->chroma_format_idc == 1);
    CHECK_FALSE(beamit::media::parse_bitstream_info(bytes("no start code"), VideoCodec::H264));
}

TEST_CASE("H.265 SPS yields RFC 6381 codec string through emulation prevention", "[protocol][bitstream]") {
    // Main: compatibility flags 1 and 2, progressive source constraint, level 3.1.
    const auto main = beamit::media::parse_bitstream_info(
        with_aud(h265_sps(1, 0x60000000U, 0xB0, 93, 1, 0), VideoCodec::H265), VideoCodec::H265);
    REQUIRE(main);
    CHECK(main->codec_string == "hvc1.1.6.L93.B0");
    CHECK(main->chroma_format_idc == 1);
    CHECK(main->bit_depth_luma == 8);

    // Range extensions 4:4:4 10-bit with zero constraint bytes (all omitted).
    const auto rext = beamit::media::parse_bitstream_info(h265_sps(4, 0x08000000U, 0, 120, 3, 2), VideoCodec::H265);
    REQUIRE(rext);
    CHECK(rext->codec_string == "hvc1.4.10.L120");
    CHECK(rext->chroma_format_idc == 3);
    CHECK(rext->bit_depth_luma == 10);
}
