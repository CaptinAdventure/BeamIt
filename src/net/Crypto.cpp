#include "net/Crypto.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace beamit::net {
namespace {

void check(NTSTATUS status, const char* operation) {
    if (!BCRYPT_SUCCESS(status)) {
        throw std::runtime_error(fmt::format("{} failed (NTSTATUS 0x{:08X})", operation,
                                             static_cast<std::uint32_t>(status)));
    }
}

ULONG size(std::size_t value) {
    if (value > std::numeric_limits<ULONG>::max()) throw std::length_error("CNG input too large");
    return static_cast<ULONG>(value);
}

constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

} // namespace

std::vector<std::byte> random_bytes(std::size_t count) {
    std::vector<std::byte> bytes(count);
    check(BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(bytes.data()), size(count),
                          BCRYPT_USE_SYSTEM_PREFERRED_RNG), "BCryptGenRandom");
    return bytes;
}

std::string base64url_encode(std::span<const std::byte> bytes) {
    std::string text;
    text.reserve((bytes.size() * 4 + 2) / 3);
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const auto byte : bytes) {
        buffer = (buffer << 8) | std::to_integer<std::uint32_t>(byte);
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            text.push_back(alphabet[(buffer >> bits) & 0x3F]);
        }
    }
    if (bits > 0) text.push_back(alphabet[(buffer << (6 - bits)) & 0x3F]);
    return text;
}

std::string base64_encode(std::span<const std::byte> bytes) {
    constexpr std::string_view standard = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string text;
    text.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t index = 0;
    for (; index + 3 <= bytes.size(); index += 3) {
        const auto value = (std::to_integer<std::uint32_t>(bytes[index]) << 16) |
                           (std::to_integer<std::uint32_t>(bytes[index + 1]) << 8) |
                           std::to_integer<std::uint32_t>(bytes[index + 2]);
        for (const int shift : {18, 12, 6, 0}) text.push_back(standard[(value >> shift) & 0x3F]);
    }
    if (const auto remaining = bytes.size() - index; remaining > 0) {
        auto value = std::to_integer<std::uint32_t>(bytes[index]) << 16;
        if (remaining == 2) value |= std::to_integer<std::uint32_t>(bytes[index + 1]) << 8;
        text.push_back(standard[(value >> 18) & 0x3F]);
        text.push_back(standard[(value >> 12) & 0x3F]);
        text.push_back(remaining == 2 ? standard[(value >> 6) & 0x3F] : '=');
        text.push_back('=');
    }
    return text;
}

std::optional<std::vector<std::byte>> base64_decode(std::string_view text) {
    if (text.size() % 4 == 0 && text.ends_with("==")) {
        text.remove_suffix(2);
    } else if (text.size() % 4 == 0 && text.ends_with('=')) {
        text.remove_suffix(1);
    }
    if (text.find('-') != std::string_view::npos || text.find('_') != std::string_view::npos) return std::nullopt;
    std::string url(text);
    std::ranges::replace(url, '+', '-');
    std::ranges::replace(url, '/', '_');
    return base64url_decode(url);
}

std::optional<std::vector<std::byte>> base64url_decode(std::string_view text) {
    if (text.size() % 4 == 1) return std::nullopt;
    std::vector<std::byte> bytes;
    bytes.reserve(text.size() * 3 / 4);
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const char character : text) {
        const auto value = alphabet.find(character);
        if (value == std::string_view::npos) return std::nullopt;
        buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes.push_back(static_cast<std::byte>((buffer >> bits) & 0xFF));
        }
    }
    // Reject non-canonical encodings whose unused trailing bits are set.
    if ((buffer & ((1U << bits) - 1)) != 0) return std::nullopt;
    return bytes;
}

Sha256 pbkdf2_sha256(std::string_view password, std::span<const std::byte> salt, std::uint32_t iterations) {
    BCRYPT_ALG_HANDLE algorithm{};
    check(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG),
          "Open PBKDF2 provider");
    Sha256 key{};
    const NTSTATUS status = BCryptDeriveKeyPBKDF2(
        algorithm,
        reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())), size(password.size()),
        reinterpret_cast<PUCHAR>(const_cast<std::byte*>(salt.data())), size(salt.size()),
        iterations, reinterpret_cast<PUCHAR>(key.data()), size(key.size()), 0);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    check(status, "BCryptDeriveKeyPBKDF2");
    return key;
}

Sha256 sha256(std::span<const std::byte> message) {
    Sha256 digest{};
    check(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                     reinterpret_cast<PUCHAR>(const_cast<std::byte*>(message.data())), size(message.size()),
                     reinterpret_cast<PUCHAR>(digest.data()), size(digest.size())),
          "SHA-256");
    return digest;
}

Sha256 hmac_sha256(std::span<const std::byte> key, std::span<const std::byte> message) {
    Sha256 digest{};
    check(BCryptHash(BCRYPT_HMAC_SHA256_ALG_HANDLE,
                     reinterpret_cast<PUCHAR>(const_cast<std::byte*>(key.data())), size(key.size()),
                     reinterpret_cast<PUCHAR>(const_cast<std::byte*>(message.data())), size(message.size()),
                     reinterpret_cast<PUCHAR>(digest.data()), size(digest.size())),
          "HMAC-SHA256");
    return digest;
}

bool constant_time_equal(std::span<const std::byte> left, std::span<const std::byte> right) noexcept {
    if (left.size() != right.size()) return false;
    std::byte difference{};
    for (std::size_t index = 0; index < left.size(); ++index) difference |= left[index] ^ right[index];
    return difference == std::byte{};
}

Sha256 auth_proof(std::span<const std::byte> key, std::string_view role,
                  std::span<const std::byte> server_nonce, std::span<const std::byte> client_nonce) {
    const std::string label = fmt::format("BeamIt v1 {}", role);
    std::vector<std::byte> message;
    message.reserve(label.size() + 1 + server_nonce.size() + client_nonce.size());
    for (const char character : label) message.push_back(static_cast<std::byte>(character));
    message.push_back(std::byte{0});
    message.insert(message.end(), server_nonce.begin(), server_nonce.end());
    message.insert(message.end(), client_nonce.begin(), client_nonce.end());
    return hmac_sha256(key, message);
}

} // namespace beamit::net
