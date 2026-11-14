#include "net/ClipboardBulk.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <set>

namespace beamit::net::clipboard_bulk {
namespace {

void put(std::span<std::byte> output, std::size_t offset, std::uint64_t value, std::size_t width) {
    for (std::size_t index = 0; index < width; ++index) {
        output[offset + index] = static_cast<std::byte>((value >> (8 * (width - index - 1))) & 0xFF);
    }
}

std::uint64_t get(std::span<const std::byte> input, std::size_t offset, std::size_t width) {
    std::uint64_t value{};
    for (std::size_t index = 0; index < width; ++index) {
        value = (value << 8) | std::to_integer<unsigned>(input[offset + index]);
    }
    return value;
}

std::optional<std::uint64_t> unsigned_field(const boost::json::object& object, std::string_view name) {
    const auto* field = object.if_contains(name);
    if (!field) return std::nullopt;
    if (field->is_uint64()) return field->as_uint64();
    if (field->is_int64() && field->as_int64() >= 0) return static_cast<std::uint64_t>(field->as_int64());
    return std::nullopt;
}

bool valid_utf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i]);
        if (first < 0x80) {
            if (first == 0) return false;
            ++i;
            continue;
        }
        std::size_t count{};
        std::uint32_t code{};
        if ((first & 0xE0) == 0xC0) { count = 2; code = first & 0x1F; }
        else if ((first & 0xF0) == 0xE0) { count = 3; code = first & 0x0F; }
        else if ((first & 0xF8) == 0xF0) { count = 4; code = first & 0x07; }
        else return false;
        if (i + count > text.size()) return false;
        for (std::size_t j = 1; j < count; ++j) {
            const auto next = static_cast<unsigned char>(text[i + j]);
            if ((next & 0xC0) != 0x80) return false;
            code = (code << 6) | (next & 0x3F);
        }
        if ((count == 2 && code < 0x80) || (count == 3 && code < 0x800) ||
            (count == 4 && code < 0x10000) || code > 0x10FFFF ||
            (code >= 0xD800 && code <= 0xDFFF)) return false;
        i += count;
    }
    return true;
}

bool valid_relative_path(std::string_view path) {
    if (path.empty() || path.size() > 4096 || path.front() == '/' || path.back() == '/' || !valid_utf8(path)) return false;
    std::size_t start{};
    while (start < path.size()) {
        const auto end = path.find('/', start);
        const auto component = path.substr(start, end == std::string_view::npos ? path.size() - start : end - start);
        if (component.empty() || component == "." || component == ".." || component.size() > 255 ||
            component.back() == '.' || component.back() == ' ' ||
            component.find_first_of("\\:*?\"<>|") != std::string_view::npos) return false;
        for (const auto ch : component) {
            if (static_cast<unsigned char>(ch) < 0x20) return false;
        }
        const auto dot = component.find('.');
        const auto stem = component.substr(0, dot);
        std::string upper;
        upper.reserve(stem.size());
        for (const auto ch : stem) upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
        if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL" ||
            (upper.size() == 4 && (upper.starts_with("COM") || upper.starts_with("LPT")) &&
             upper[3] >= '1' && upper[3] <= '9')) return false;
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return true;
}

std::optional<std::wstring> collision_key(std::string_view path) {
    const int input_size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(),
                                                static_cast<int>(path.size()), nullptr, 0);
    if (input_size <= 0) return std::nullopt;
    std::wstring input(static_cast<std::size_t>(input_size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()),
                            input.data(), input_size) != input_size) return std::nullopt;
    std::wstring normalized(static_cast<std::size_t>(input_size) * 3, L'\0');
    const int normalized_size =
        NormalizeString(NormalizationC, input.data(), input_size, normalized.data(),
                        static_cast<int>(normalized.size()));
    if (normalized_size <= 0) return std::nullopt;
    normalized.resize(static_cast<std::size_t>(normalized_size));
    std::wstring folded(normalized.size() * 3 + 1, L'\0');
    const int folded_size = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, normalized.data(),
                                          normalized_size, folded.data(), static_cast<int>(folded.size()),
                                          nullptr, nullptr, 0);
    if (folded_size <= 0) return std::nullopt;
    folded.resize(static_cast<std::size_t>(folded_size));
    return folded;
}

} // namespace

std::array<std::byte, frame_header_size> encode_header(const FrameHeader& header) {
    std::array<std::byte, frame_header_size> result{};
    result[0] = std::byte{'B'};
    result[1] = std::byte{'M'};
    result[2] = std::byte{'B'};
    result[3] = std::byte{'1'};
    result[4] = std::byte{1};
    result[5] = static_cast<std::byte>(header.kind);
    std::ranges::copy(header.transfer_id, result.begin() + 8);
    put(result, 24, header.node_index, 4);
    put(result, 28, header.offset, 8);
    put(result, 36, header.payload_size, 4);
    return result;
}

std::expected<FrameHeader, std::string> decode_header(std::span<const std::byte> bytes) {
    if (bytes.size() < frame_header_size) return std::unexpected("frame header is truncated");
    if (bytes[0] != std::byte{'B'} || bytes[1] != std::byte{'M'} ||
        bytes[2] != std::byte{'B'} || bytes[3] != std::byte{'1'} ||
        bytes[4] != std::byte{1}) return std::unexpected("frame magic or version is invalid");
    if (bytes[6] != std::byte{} || bytes[7] != std::byte{}) return std::unexpected("frame reserved field is nonzero");
    const auto kind = std::to_integer<std::uint8_t>(bytes[5]);
    if (kind < 1 || kind > 3) return std::unexpected("frame kind is invalid");
    FrameHeader result;
    result.kind = static_cast<FrameKind>(kind);
    std::ranges::copy(bytes.subspan(8, 16), result.transfer_id.begin());
    result.node_index = static_cast<std::uint32_t>(get(bytes, 24, 4));
    result.offset = get(bytes, 28, 8);
    result.payload_size = static_cast<std::uint32_t>(get(bytes, 36, 4));
    if (result.kind == FrameKind::End) {
        if (result.node_index != std::numeric_limits<std::uint32_t>::max() || result.payload_size != 0 ||
            bytes.size() != frame_header_size) return std::unexpected("END frame fields are invalid");
    } else if (result.payload_size == 0 || result.payload_size > max_chunk_size ||
               bytes.size() != frame_header_size + result.payload_size) {
        return std::unexpected("data frame length is invalid");
    }
    return result;
}

bool valid_transfer_id(std::string_view value) noexcept {
    return value.size() == 22 && std::ranges::all_of(value, [](char character) {
        return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9') || character == '-' || character == '_';
    });
}

bool SingleUseToken::claim(std::string_view candidate) {
    if (used_ || candidate.size() != 43 ||
        !std::ranges::all_of(candidate, [](char character) {
            return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
                   (character >= '0' && character <= '9') || character == '-' || character == '_';
        })) return false;
    const auto decoded = base64url_decode(candidate);
    if (!decoded || decoded->size() != token_.size() ||
        !constant_time_equal(*decoded, token_)) return false;
    used_ = true;
    return true;
}

SocketCredentials issue_socket_credentials() {
    const auto id = random_bytes(16);
    const auto token = random_bytes(32);
    SocketCredentials result;
    std::ranges::copy(id, result.transfer_id.begin());
    std::ranges::copy(token, result.token.begin());
    result.transfer_id_text = base64url_encode(result.transfer_id);
    result.token_text = base64url_encode(result.token);
    return result;
}

std::expected<Manifest, std::string> parse_manifest(const boost::json::value& value, std::size_t encoded_size) {
    if (!value.is_object() || encoded_size > max_manifest_bytes) {
        return std::unexpected("manifest is invalid or too large");
    }
    const auto& object = value.as_object();
    const auto roots = unsigned_field(object, "selected_roots");
    const auto declared_bytes = unsigned_field(object, "total_file_bytes");
    const auto* node_values = object.if_contains("nodes");
    if (!roots || !declared_bytes || !node_values || !node_values->is_array() ||
        *roots > max_roots || *declared_bytes > max_file_bytes || node_values->as_array().size() > max_nodes) {
        return std::unexpected("manifest summary fields are invalid");
    }

    Manifest manifest{.selected_roots = static_cast<std::uint32_t>(*roots), .encoded_size = encoded_size};
    manifest.nodes.reserve(node_values->as_array().size());
    for (const auto& value_node : node_values->as_array()) {
        if (!value_node.is_object()) return std::unexpected("manifest node must be an object");
        const auto& node_object = value_node.as_object();
        const auto* path = node_object.if_contains("path");
        const auto* type = node_object.if_contains("type");
        if (!path || !path->is_string() || !type || !type->is_string()) {
            return std::unexpected("manifest node path or type is invalid");
        }
        Node node;
        node.path = std::string(path->as_string());
        const auto kind = type->as_string();
        if (kind == "directory") {
            node.kind = Node::Kind::Directory;
        } else if (kind == "file" || kind == "png") {
            node.kind = kind == "file" ? Node::Kind::File : Node::Kind::Png;
            const auto length = unsigned_field(node_object, "length");
            const auto* digest = node_object.if_contains("sha256");
            if (!length || !digest || !digest->is_string()) {
                return std::unexpected("manifest content length or digest is invalid");
            }
            node.size = *length;
            const auto decoded = base64url_decode(digest->as_string());
            if (!decoded || decoded->size() != node.sha256.size()) {
                return std::unexpected("manifest digest must be a 32-byte base64url SHA-256 value");
            }
            std::ranges::copy(*decoded, node.sha256.begin());
            if (node.kind == Node::Kind::Png) {
                const auto width = unsigned_field(node_object, "width");
                const auto height = unsigned_field(node_object, "height");
                if (!width || !height || *width > std::numeric_limits<std::uint32_t>::max() ||
                    *height > std::numeric_limits<std::uint32_t>::max()) {
                    return std::unexpected("PNG dimensions are invalid");
                }
                node.width = static_cast<std::uint32_t>(*width);
                node.height = static_cast<std::uint32_t>(*height);
            }
        } else {
            return std::unexpected("manifest node type is unsupported");
        }
        manifest.nodes.push_back(std::move(node));
    }

    if (const auto valid = validate_manifest(manifest); !valid) {
        return std::unexpected("manifest violates a path, entry, image or size limit");
    }
    std::uint64_t regular_bytes{};
    for (const auto& node : manifest.nodes) {
        if (node.kind == Node::Kind::File) regular_bytes += node.size;
    }
    if (regular_bytes != *declared_bytes) return std::unexpected("manifest total file bytes do not match its nodes");
    return manifest;
}

boost::json::object manifest_json(const Manifest& manifest) {
    boost::json::array nodes;
    nodes.reserve(manifest.nodes.size());
    for (const auto& node : manifest.nodes) {
        boost::json::object item{{"path", node.path}};
        if (node.kind == Node::Kind::Directory) {
            item["type"] = "directory";
        } else {
            item["type"] = node.kind == Node::Kind::File ? "file" : "png";
            item["length"] = node.size;
            item["sha256"] = base64url_encode(node.sha256);
            if (node.kind == Node::Kind::Png) {
                item["width"] = node.width;
                item["height"] = node.height;
            }
        }
        nodes.push_back(std::move(item));
    }
    std::uint64_t regular_bytes{};
    for (const auto& node : manifest.nodes) {
        if (node.kind == Node::Kind::File && node.size <= std::numeric_limits<std::uint64_t>::max() - regular_bytes) {
            regular_bytes += node.size;
        }
    }
    return {{"selected_roots", manifest.selected_roots}, {"total_file_bytes", regular_bytes},
            {"nodes", std::move(nodes)}};
}

std::expected<void, ValidationError> validate_manifest(const Manifest& manifest) {
    if (manifest.selected_roots > max_roots) return std::unexpected(ValidationError::TooManyRoots);
    if (manifest.nodes.size() > max_nodes) return std::unexpected(ValidationError::TooManyNodes);
    if (manifest.encoded_size > max_manifest_bytes) return std::unexpected(ValidationError::ManifestTooLarge);
    std::map<std::wstring, Node::Kind> paths;
    std::uint64_t regular_bytes{};
    for (const auto& node : manifest.nodes) {
        if (!valid_relative_path(node.path)) return std::unexpected(ValidationError::InvalidPath);
        const auto key = collision_key(node.path);
        if (!key) return std::unexpected(ValidationError::InvalidPath);
        if (paths.contains(*key)) return std::unexpected(ValidationError::DuplicatePath);
        if (node.kind == Node::Kind::Directory) {
            if (node.size != 0) return std::unexpected(ValidationError::InvalidNode);
        } else if (node.kind == Node::Kind::File) {
            if (node.size > max_file_bytes - regular_bytes) return std::unexpected(ValidationError::TooManyFileBytes);
            regular_bytes += node.size;
        } else {
            if (node.size > max_png_bytes) return std::unexpected(ValidationError::PngTooLarge);
            if (node.width == 0 || node.height == 0 || node.width > max_png_dimension ||
                node.height > max_png_dimension ||
                static_cast<std::uint64_t>(node.width) * node.height > max_png_pixels) {
                return std::unexpected(ValidationError::InvalidPngDimensions);
            }
        }
        paths.emplace(*key, node.kind);
    }

    // Every non-root path must have an explicit directory parent. This both preserves empty
    // directories and prevents a file or PNG node from being used as an ancestor.
    std::set<std::wstring> selected_roots;
    for (const auto& [path, kind] : paths) {
        if (kind != Node::Kind::Png) {
            const auto separator = path.find(L'/');
            selected_roots.insert(path.substr(0, separator));
        }
        for (auto separator = path.find(L'/'); separator != std::wstring::npos;
             separator = path.find(L'/', separator + 1)) {
            const auto parent = paths.find(path.substr(0, separator));
            if (parent == paths.end() || parent->second != Node::Kind::Directory) {
                return std::unexpected(ValidationError::InvalidNode);
            }
        }
        if (kind == Node::Kind::Directory) continue;
        const auto child_prefix = path + L'/';
        const auto child = paths.lower_bound(child_prefix);
        if (child != paths.end() && child->first.starts_with(child_prefix)) {
            return std::unexpected(ValidationError::InvalidNode);
        }
    }
    if (selected_roots.size() != manifest.selected_roots) {
        return std::unexpected(ValidationError::InvalidNode);
    }
    return {};
}

bool requires_confirmation(std::uint64_t regular_file_bytes, std::uint32_t selected_roots) noexcept {
    return requires_confirmation(regular_file_bytes, selected_roots, TransferPolicy{});
}

bool requires_confirmation(std::uint64_t regular_file_bytes, std::uint32_t selected_roots,
                           const TransferPolicy& policy) noexcept {
    return regular_file_bytes >= policy.byte_threshold || selected_roots > policy.root_threshold;
}

bool valid_policy(const TransferPolicy& policy) noexcept {
    return policy.byte_threshold <= max_file_bytes && policy.root_threshold <= max_roots;
}

std::uint64_t payload_bytes(const Manifest& manifest) noexcept {
    std::uint64_t total{};
    for (const auto& node : manifest.nodes) {
        if (node.kind != Node::Kind::Directory && node.size <= std::numeric_limits<std::uint64_t>::max() - total) {
            total += node.size;
        }
    }
    return total;
}

FrameSequence::FrameSequence(std::array<std::byte, 16> transfer_id, std::span<const Node> nodes)
    : transfer_id_(transfer_id), nodes_(nodes.begin(), nodes.end()) {
    skip_empty();
}

void FrameSequence::skip_empty() {
    while (next_node_ < nodes_.size() &&
           (nodes_[next_node_].kind == Node::Kind::Directory || nodes_[next_node_].size == 0)) {
        ++next_node_;
        node_offset_ = 0;
    }
}

std::expected<FrameHeader, std::string> FrameSequence::accept(std::span<const std::byte> frame) {
    if (ended_) return std::unexpected("frame received after END");
    auto parsed = decode_header(frame);
    if (!parsed) return std::unexpected(parsed.error());
    const auto header = *parsed;
    if (header.transfer_id != transfer_id_) return std::unexpected("transfer ID does not match");
    if (header.kind == FrameKind::End) {
        if (next_node_ != nodes_.size() || header.offset != total_offset_) {
            return std::unexpected("END arrived before the declared payload was drained");
        }
        ended_ = true;
        return header;
    }
    if (next_node_ >= nodes_.size()) return std::unexpected("unexpected data after declared items");
    const auto& node = nodes_[next_node_];
    const auto expected_kind = node.kind == Node::Kind::Png ? FrameKind::Png : FrameKind::File;
    if (header.kind != expected_kind || header.node_index != next_node_ || header.offset != node_offset_) {
        return std::unexpected("frame kind, node index or offset is not contiguous");
    }
    if (header.payload_size > node.size - node_offset_) return std::unexpected("frame exceeds declared item size");
    node_offset_ += header.payload_size;
    total_offset_ += header.payload_size;
    if (node_offset_ == node.size) {
        ++next_node_;
        node_offset_ = 0;
        skip_empty();
    }
    return header;
}

} // namespace beamit::net::clipboard_bulk
