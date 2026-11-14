#include "net/ClipboardBulk.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <limits>
#include <ranges>

namespace bulk = beamit::net::clipboard_bulk;

TEST_CASE("Bulk frame headers use BMB1 and unsigned big-endian fields", "[protocol][clipboard-bulk]") {
    bulk::FrameHeader source{.kind = bulk::FrameKind::File, .node_index = 0x01020304,
                             .offset = 0x0102030405060708, .payload_size = 3};
    source.transfer_id[0] = std::byte{0xAB};
    const auto header = bulk::encode_header(source);
    CHECK(header.size() == 40);
    CHECK(header[0] == std::byte{'B'});
    CHECK(header[3] == std::byte{'1'});
    CHECK(header[24] == std::byte{1});
    CHECK(header[27] == std::byte{4});
    CHECK(header[28] == std::byte{1});
    CHECK(header[35] == std::byte{8});
    std::array<std::byte, 43> frame{};
    std::ranges::copy(header, frame.begin());
    const auto parsed = bulk::decode_header(frame);
    REQUIRE(parsed);
    CHECK(parsed->kind == source.kind);
    CHECK(parsed->transfer_id == source.transfer_id);
    CHECK(parsed->node_index == source.node_index);
    CHECK(parsed->offset == source.offset);
    CHECK(parsed->payload_size == source.payload_size);
}

TEST_CASE("Bulk frame decoder rejects malformed headers and data lengths", "[protocol][clipboard-bulk]") {
    bulk::FrameHeader source{.kind = bulk::FrameKind::File, .node_index = 0, .offset = 0, .payload_size = 1};
    auto encoded = bulk::encode_header(source);
    CHECK_FALSE(bulk::decode_header(std::span(encoded).first(39)));
    encoded[0] = std::byte{'X'};
    CHECK_FALSE(bulk::decode_header(encoded));
    encoded = bulk::encode_header(source);
    encoded[6] = std::byte{1};
    CHECK_FALSE(bulk::decode_header(encoded));
    encoded = bulk::encode_header(source);
    CHECK_FALSE(bulk::decode_header(encoded)); // declared payload is absent
    std::array<std::byte, 41> valid{};
    std::ranges::copy(bulk::encode_header(source), valid.begin());
    CHECK(bulk::decode_header(valid));

    bulk::FrameHeader end{.kind = bulk::FrameKind::End,
                          .node_index = std::numeric_limits<std::uint32_t>::max()};
    CHECK(bulk::decode_header(bulk::encode_header(end)));
    end.node_index = 0;
    CHECK_FALSE(bulk::decode_header(bulk::encode_header(end)));
}

TEST_CASE("Bulk manifest validates path safety, node and payload limits", "[protocol][clipboard-bulk]") {
    bulk::Manifest manifest{.selected_roots = 2,
                            .nodes = {{.kind = bulk::Node::Kind::Directory, .path = "folder"},
                                      {.kind = bulk::Node::Kind::File, .path = "folder/naïve.txt", .size = 42},
                                      {.kind = bulk::Node::Kind::File, .path = "empty.bin", .size = 0}}};
    REQUIRE(bulk::validate_manifest(manifest));
    CHECK(bulk::payload_bytes(manifest) == 42);

    manifest.selected_roots = 1;
    CHECK(bulk::validate_manifest(manifest).error() == bulk::ValidationError::InvalidNode);
    manifest.selected_roots = 2;
    manifest.nodes[1].path = "../outside";
    CHECK(bulk::validate_manifest(manifest).error() == bulk::ValidationError::InvalidPath);
    manifest.nodes[1].path = "folder/naïve.txt";
    manifest.nodes[2].path = "FOLDER/naïve.txt";
    CHECK(bulk::validate_manifest(manifest).error() == bulk::ValidationError::DuplicatePath);
    manifest.nodes[2].path = "NUL.txt";
    CHECK(bulk::validate_manifest(manifest).error() == bulk::ValidationError::InvalidPath);
    manifest.nodes[2].path = "empty.bin";
    manifest.nodes[2].size = bulk::max_file_bytes;
    CHECK(bulk::validate_manifest(manifest).error() == bulk::ValidationError::TooManyFileBytes);
    manifest.nodes[2].size = 0;
    manifest.encoded_size = bulk::max_manifest_bytes + 1;
    CHECK(bulk::validate_manifest(manifest).error() == bulk::ValidationError::ManifestTooLarge);
}

TEST_CASE("Bulk auto-transfer thresholds require strict byte and inclusive root limits",
          "[protocol][clipboard-bulk]") {
    CHECK_FALSE(bulk::requires_confirmation(499'999'999, 5));
    CHECK(bulk::requires_confirmation(500'000'000, 5));
    CHECK(bulk::requires_confirmation(1, 6));
    CHECK_FALSE(bulk::requires_confirmation(0, 0));
    const bulk::TransferPolicy configured{.byte_threshold = 1'000, .root_threshold = 2};
    CHECK_FALSE(bulk::requires_confirmation(999, 2, configured));
    CHECK(bulk::requires_confirmation(1'000, 2, configured));
    CHECK(bulk::requires_confirmation(0, 3, configured));
    CHECK(bulk::valid_policy(configured));
    CHECK_FALSE(bulk::valid_policy({.byte_threshold = bulk::max_file_bytes + 1, .root_threshold = 2}));
    CHECK_FALSE(bulk::valid_policy({.byte_threshold = 1, .root_threshold = bulk::max_roots + 1}));
}

TEST_CASE("Bulk receiver sequence enforces ordered chunks and END drain barrier", "[protocol][clipboard-bulk]") {
    std::array<std::byte, 16> id{};
    id[3] = std::byte{7};
    const std::array nodes{
        bulk::Node{.kind = bulk::Node::Kind::Directory, .path = "empty"},
        bulk::Node{.kind = bulk::Node::Kind::File, .path = "a.bin", .size = 3},
        bulk::Node{.kind = bulk::Node::Kind::File, .path = "zero.bin", .size = 0},
        bulk::Node{.kind = bulk::Node::Kind::Png, .path = "image.png", .size = 2},
    };
    bulk::FrameSequence sequence(id, nodes);
    auto data = [&](bulk::FrameKind kind, std::uint32_t node, std::uint64_t offset, std::uint32_t size) {
        bulk::FrameHeader header{.kind = kind, .transfer_id = id, .node_index = node,
                                 .offset = offset, .payload_size = size};
        const auto encoded = bulk::encode_header(header);
        std::vector<std::byte> message(encoded.begin(), encoded.end());
        message.resize(message.size() + size);
        return message;
    };
    CHECK_FALSE(sequence.accept(data(bulk::FrameKind::File, 1, 1, 1))); // first offset must be zero
    REQUIRE(sequence.accept(data(bulk::FrameKind::File, 1, 0, 2)));
    REQUIRE(sequence.accept(data(bulk::FrameKind::File, 1, 2, 1)));
    REQUIRE(sequence.accept(data(bulk::FrameKind::Png, 3, 0, 2)));
    bulk::FrameHeader end{.kind = bulk::FrameKind::End, .transfer_id = id,
                          .node_index = std::numeric_limits<std::uint32_t>::max(), .offset = 5};
    REQUIRE(sequence.accept(bulk::encode_header(end)));
    CHECK(sequence.ended());
    CHECK_FALSE(sequence.accept(bulk::encode_header(end)));

    bulk::FrameSequence short_transfer(id, nodes);
    end.offset = 5;
    CHECK_FALSE(short_transfer.accept(bulk::encode_header(end)));

    bulk::FrameSequence wrong_id(id, nodes);
    auto other_id = id;
    other_id[0] = std::byte{1};
    bulk::FrameHeader wrong_transfer{.kind = bulk::FrameKind::File, .transfer_id = other_id,
                                     .node_index = 1, .payload_size = 1};
    auto encoded = bulk::encode_header(wrong_transfer);
    std::array<std::byte, 41> wrong_message{};
    std::ranges::copy(encoded, wrong_message.begin());
    CHECK_FALSE(wrong_id.accept(wrong_message));

    bulk::FrameSequence wrong_order(id, nodes);
    CHECK_FALSE(wrong_order.accept(data(bulk::FrameKind::File, 2, 0, 1)));

    bulk::FrameSequence overrun(id, nodes);
    CHECK_FALSE(overrun.accept(data(bulk::FrameKind::File, 1, 0, 4)));
}

TEST_CASE("Bulk manifest enforces bounded entry counts and PNG declarations", "[protocol][clipboard-bulk]") {
    bulk::Manifest manifest{.selected_roots = bulk::max_roots + 1};
    CHECK(bulk::validate_manifest(manifest).error() == bulk::ValidationError::TooManyRoots);

    manifest.selected_roots = 1;
    manifest.nodes.resize(bulk::max_nodes + 1);
    CHECK(bulk::validate_manifest(manifest).error() == bulk::ValidationError::TooManyNodes);

    manifest.nodes = {{.kind = bulk::Node::Kind::Png, .path = "image.png",
                       .size = bulk::max_png_bytes + 1, .width = 1, .height = 1}};
    CHECK(bulk::validate_manifest(manifest).error() == bulk::ValidationError::PngTooLarge);
    manifest.nodes[0].size = 1;
    manifest.nodes[0].width = bulk::max_png_dimension + 1;
    CHECK(bulk::validate_manifest(manifest).error() == bulk::ValidationError::InvalidPngDimensions);
}

TEST_CASE("Bulk manifest rejects Unicode normalization and case collisions", "[protocol][clipboard-bulk]") {
    bulk::Manifest manifest{.selected_roots = 2,
                            .nodes = {{.kind = bulk::Node::Kind::File, .path = "\xC3\x85.txt", .size = 1},
                                      {.kind = bulk::Node::Kind::File, .path = "A\xCC\x8A.txt", .size = 1}}};
    CHECK(bulk::validate_manifest(manifest).error() == bulk::ValidationError::DuplicatePath);
}

TEST_CASE("Bulk manifest requires directory parents and rejects file ancestors", "[protocol][clipboard-bulk]") {
    bulk::Manifest manifest{.selected_roots = 1,
                            .nodes = {{.kind = bulk::Node::Kind::File, .path = "parent", .size = 1},
                                      {.kind = bulk::Node::Kind::File, .path = "parent/child", .size = 1}}};
    CHECK(bulk::validate_manifest(manifest).error() == bulk::ValidationError::InvalidNode);

    manifest.nodes = {{.kind = bulk::Node::Kind::File, .path = "child", .size = 1}};
    CHECK(bulk::validate_manifest(manifest));

    manifest.nodes = {{.kind = bulk::Node::Kind::Directory, .path = "parent"},
                      {.kind = bulk::Node::Kind::File, .path = "parent/child", .size = 1}};
    CHECK(bulk::validate_manifest(manifest));
}

TEST_CASE("Bulk manifest JSON round-trips hashes and checks its byte summary", "[protocol][clipboard-bulk]") {
    bulk::Manifest source{
        .selected_roots = 1,
        .nodes = {{.kind = bulk::Node::Kind::Directory, .path = "folder"},
                  {.kind = bulk::Node::Kind::File, .path = "folder/a.txt", .size = 7},
                  {.kind = bulk::Node::Kind::Png, .path = "image.png", .size = 3, .width = 1, .height = 1}}};
    source.nodes[1].sha256[0] = std::byte{0xAB};
    source.nodes[2].sha256[31] = std::byte{0xCD};
    const auto json = bulk::manifest_json(source);
    const auto encoded = boost::json::serialize(json);
    auto parsed = bulk::parse_manifest(boost::json::parse(encoded), encoded.size());
    REQUIRE(parsed);
    CHECK(parsed->selected_roots == source.selected_roots);
    REQUIRE(parsed->nodes.size() == source.nodes.size());
    CHECK(parsed->nodes[1].sha256 == source.nodes[1].sha256);
    CHECK(parsed->nodes[2].width == 1);
    CHECK_FALSE(bulk::parse_manifest(boost::json::parse(encoded), bulk::max_manifest_bytes + 1));

    auto mismatched = json;
    mismatched["total_file_bytes"] = 6;
    const auto invalid = boost::json::serialize(mismatched);
    CHECK_FALSE(bulk::parse_manifest(boost::json::parse(invalid), invalid.size()));
}

TEST_CASE("Bulk socket credentials use the required shape and single-use token check", "[protocol][clipboard-bulk]") {
    const auto credentials = bulk::issue_socket_credentials();
    CHECK(bulk::valid_transfer_id(credentials.transfer_id_text));
    CHECK(credentials.token_text.size() == 43);
    bulk::SingleUseToken token(credentials.token);
    CHECK_FALSE(token.used());
    CHECK_FALSE(token.claim(std::string(43, 'A')));
    CHECK_FALSE(token.used());
    CHECK(token.claim(credentials.token_text));
    CHECK(token.used());
    CHECK_FALSE(token.claim(credentials.token_text));
}
