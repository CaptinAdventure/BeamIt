#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace beamit::net {

using Sha256 = std::array<std::byte, 32>;

[[nodiscard]] std::vector<std::byte> random_bytes(std::size_t count);
[[nodiscard]] std::string base64url_encode(std::span<const std::byte> bytes);
// RFC 4648 section 4 base64 with padding (browser atob/data: URL compatible).
[[nodiscard]] std::string base64_encode(std::span<const std::byte> bytes);
// Strict RFC 4648 section 4 decoding; padding is optional, whitespace is rejected.
[[nodiscard]] std::optional<std::vector<std::byte>> base64_decode(std::string_view text);
[[nodiscard]] std::optional<std::vector<std::byte>> base64url_decode(std::string_view text);
[[nodiscard]] Sha256 pbkdf2_sha256(std::string_view password, std::span<const std::byte> salt,
                                   std::uint32_t iterations);
[[nodiscard]] Sha256 sha256(std::span<const std::byte> message);
[[nodiscard]] Sha256 hmac_sha256(std::span<const std::byte> key, std::span<const std::byte> message);
[[nodiscard]] bool constant_time_equal(std::span<const std::byte> left, std::span<const std::byte> right) noexcept;

// Proofs for PROTOCOL.md §4.2; role is "client" or "server".
[[nodiscard]] Sha256 auth_proof(std::span<const std::byte> key, std::string_view role,
                                std::span<const std::byte> server_nonce,
                                std::span<const std::byte> client_nonce);

} // namespace beamit::net
