#pragma once

#include "net/Crypto.hpp"

#include <boost/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace beamit::net::clipboard_bulk {

inline constexpr std::size_t frame_header_size = 40;
inline constexpr std::size_t max_chunk_size = 256 * 1024;
inline constexpr std::size_t max_queued_payload = 1024 * 1024;
inline constexpr std::uint64_t default_byte_threshold = 500'000'000;
inline constexpr std::uint32_t default_root_threshold = 5;
inline constexpr std::uint32_t max_roots = 64;
inline constexpr std::uint32_t max_nodes = 4096;
inline constexpr std::size_t max_manifest_bytes = 4 * 1024 * 1024;
inline constexpr std::uint64_t max_file_bytes = 2'000'000'000;
inline constexpr std::uint64_t max_png_bytes = 64 * 1024 * 1024;
inline constexpr std::uint32_t max_png_dimension = 16'384;
inline constexpr std::uint64_t max_png_pixels = 64 * 1024 * 1024;

enum class FrameKind : std::uint8_t { File = 1, Png = 2, End = 3 };

struct FrameHeader {
    FrameKind kind{};
    std::array<std::byte, 16> transfer_id{};
    std::uint32_t node_index{};
    std::uint64_t offset{};
    std::uint32_t payload_size{};
};

struct Node {
    enum class Kind { Directory, File, Png };
    Kind kind{};
    std::string path;
    std::uint64_t size{};
    Sha256 sha256{};
    std::uint32_t width{};
    std::uint32_t height{};
};

struct Manifest {
    std::uint32_t selected_roots{};
    std::size_t encoded_size{};
    std::vector<Node> nodes;
};

struct TransferPolicy {
    std::uint64_t byte_threshold{default_byte_threshold};
    std::uint32_t root_threshold{default_root_threshold};
};

struct SocketCredentials {
    std::array<std::byte, 16> transfer_id{};
    std::array<std::byte, 32> token{};
    std::string transfer_id_text;
    std::string token_text;
};

class SingleUseToken {
public:
    explicit SingleUseToken(std::array<std::byte, 32> token) : token_(token) {}
    [[nodiscard]] bool claim(std::string_view candidate);
    [[nodiscard]] bool used() const noexcept { return used_; }

private:
    std::array<std::byte, 32> token_{};
    bool used_{};
};

enum class ValidationError {
    TooManyRoots,
    TooManyNodes,
    ManifestTooLarge,
    InvalidPath,
    DuplicatePath,
    InvalidNode,
    TooManyFileBytes,
    PngTooLarge,
    InvalidPngDimensions,
};

[[nodiscard]] std::array<std::byte, frame_header_size> encode_header(const FrameHeader& header);
[[nodiscard]] std::expected<FrameHeader, std::string> decode_header(std::span<const std::byte> bytes);
[[nodiscard]] SocketCredentials issue_socket_credentials();
[[nodiscard]] bool valid_transfer_id(std::string_view value) noexcept;
[[nodiscard]] std::expected<Manifest, std::string> parse_manifest(const boost::json::value& value,
                                                                    std::size_t encoded_size);
[[nodiscard]] boost::json::object manifest_json(const Manifest& manifest);
[[nodiscard]] std::expected<void, ValidationError> validate_manifest(const Manifest& manifest);
[[nodiscard]] bool requires_confirmation(std::uint64_t regular_file_bytes, std::uint32_t selected_roots) noexcept;
[[nodiscard]] bool requires_confirmation(std::uint64_t regular_file_bytes, std::uint32_t selected_roots,
                                         const TransferPolicy& policy) noexcept;
[[nodiscard]] bool valid_policy(const TransferPolicy& policy) noexcept;
[[nodiscard]] std::uint64_t payload_bytes(const Manifest& manifest) noexcept;

// Validates manifest order and contiguous offsets as frames arrive. The receiver must still hash
// the bytes as it writes them and compare each digest before promotion.
class FrameSequence {
public:
    FrameSequence(std::array<std::byte, 16> transfer_id, std::span<const Node> nodes);
    [[nodiscard]] std::expected<FrameHeader, std::string> accept(std::span<const std::byte> frame);
    [[nodiscard]] bool ended() const noexcept { return ended_; }

private:
    void skip_empty();
    std::array<std::byte, 16> transfer_id_;
    std::vector<Node> nodes_;
    std::size_t next_node_{};
    std::uint64_t node_offset_{};
    std::uint64_t total_offset_{};
    bool ended_{};
};

} // namespace beamit::net::clipboard_bulk
