#include "host/ClipboardBulkStorage.hpp"

#include "net/Crypto.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <iterator>
#include <windows.h>
#include <winioctl.h>

namespace bulk = beamit::net::clipboard_bulk;

namespace {

struct TemporaryRoot {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 ("BeamItBulkStorage-" + beamit::net::base64url_encode(
                                     beamit::net::random_bytes(12)));
    ~TemporaryRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

bulk::Manifest small_manifest(std::span<const std::byte> content, bool correct_hash = true) {
    bulk::Manifest manifest{
        .selected_roots = 2,
        .nodes = {{.kind = bulk::Node::Kind::Directory, .path = "folder"},
                  {.kind = bulk::Node::Kind::File, .path = "folder/hello.txt", .size = content.size()},
                  {.kind = bulk::Node::Kind::Directory, .path = "empty"}}};
    if (correct_hash) manifest.nodes[1].sha256 = beamit::net::sha256(content);
    return manifest;
}

std::vector<std::byte> data_frame(const std::array<std::byte, 16>& id, std::uint32_t node,
                                  std::uint64_t offset, std::span<const std::byte> payload) {
    const auto header = bulk::encode_header({.kind = bulk::FrameKind::File, .transfer_id = id,
                                              .node_index = node, .offset = offset,
                                              .payload_size = static_cast<std::uint32_t>(payload.size())});
    std::vector<std::byte> result(header.begin(), header.end());
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}

std::array<std::byte, 16> decode_id(std::string_view text) {
    const auto decoded = beamit::net::base64url_decode(text);
    REQUIRE(decoded);
    REQUIRE(decoded->size() == 16);
    std::array<std::byte, 16> result{};
    std::ranges::copy(*decoded, result.begin());
    return result;
}

struct PrivateHandle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~PrivateHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

std::string read_fixture(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

void sparse_fixture(const std::filesystem::path& path, std::uint64_t size) {
    std::filesystem::create_directories(path.parent_path());
    {
        PrivateHandle file{CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr)};
        REQUIRE(file.value != INVALID_HANDLE_VALUE);
        DWORD returned{};
        REQUIRE(DeviceIoControl(file.value, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr));
    }
    std::filesystem::resize_file(path, size);
}

} // namespace

TEST_CASE("Bulk receiver stages verifies and atomically promotes a nested selection",
          "[clipboard-bulk][host-storage]") {
    TemporaryRoot root;
    const auto id_text = beamit::net::base64url_encode(beamit::net::random_bytes(16));
    const auto id = decode_id(id_text);
    const std::string source = "abc";
    const auto payload = std::as_bytes(std::span(source.data(), source.size()));
    auto manifest = small_manifest(payload);

    std::filesystem::path partial;
    std::filesystem::path committed;
    {
        beamit::host::ClipboardBulkReceiver receiver(root.path, id_text, manifest);
        partial = receiver.staging_directory();
        REQUIRE(receiver.accept(data_frame(id, 1, 0, payload)));
        bulk::FrameHeader end{.kind = bulk::FrameKind::End, .transfer_id = id,
                              .node_index = UINT32_MAX, .offset = payload.size()};
        const auto end_frame = bulk::encode_header(end);
        REQUIRE(receiver.accept(end_frame));
        auto paths = receiver.promote();
        REQUIRE(paths);
        REQUIRE(paths->size() == 2);
        CHECK(paths->at(0).filename() == L"folder");
        CHECK(paths->at(1).filename() == L"empty");
        committed = paths->front().parent_path();
        std::ifstream file(paths->front() / L"hello.txt", std::ios::binary);
        const std::string received{std::istreambuf_iterator<char>(file), {}};
        CHECK(received == source);
        CHECK(std::filesystem::is_directory(paths->back()));
        receiver.published();
    }
    CHECK_FALSE(std::filesystem::exists(partial));
    CHECK(std::filesystem::exists(committed / L"folder" / L"hello.txt"));
}

TEST_CASE("Bulk receiver rejects a mismatched digest and removes uncommitted staging",
          "[clipboard-bulk][host-storage]") {
    TemporaryRoot root;
    const auto id_text = beamit::net::base64url_encode(beamit::net::random_bytes(16));
    const auto id = decode_id(id_text);
    const std::string source = "abc";
    const auto payload = std::as_bytes(std::span(source.data(), source.size()));
    auto manifest = small_manifest(payload, false);

    std::filesystem::path partial;
    {
        beamit::host::ClipboardBulkReceiver receiver(root.path, id_text, manifest);
        partial = receiver.staging_directory();
        CHECK_FALSE(receiver.accept(data_frame(id, 1, 0, payload)));
        CHECK_FALSE(receiver.promote());
    }
    CHECK_FALSE(std::filesystem::exists(partial));
}

TEST_CASE("Bulk receiver rejects unsafe manifests before creating staging", "[clipboard-bulk][host-storage]") {
    TemporaryRoot root;
    auto manifest = bulk::Manifest{.selected_roots = 1,
                                   .nodes = {{.kind = bulk::Node::Kind::File, .path = "../outside", .size = 1}}};
    const auto id_text = beamit::net::base64url_encode(beamit::net::random_bytes(16));
    CHECK_THROWS(beamit::host::ClipboardBulkReceiver(root.path, id_text, std::move(manifest)));
    CHECK_FALSE(std::filesystem::exists(root.path));
}

TEST_CASE("Shared quota reserves exact declared bytes plus scratch without double counting",
          "[clipboard-bulk][host-storage]") {
    TemporaryRoot root;
    const std::string content = "abc";
    const auto bytes = std::as_bytes(std::span(content));
    const auto retained = root.path / L"client" / L"committed" / L"retained";
    std::filesystem::create_directories(retained.parent_path());
    const auto file = CreateFileW(retained.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
    REQUIRE(file != INVALID_HANDLE_VALUE);
    DWORD returned{};
    const bool sparse = DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr);
    CloseHandle(file);
    REQUIRE(sparse);
    std::filesystem::resize_file(retained, 8'000'000'000 - 128 * 1024 * 1024 - bytes.size());
    const auto id_text = beamit::net::base64url_encode(beamit::net::random_bytes(16));
    beamit::host::ClipboardBulkReceiver receiver(root.path, id_text, small_manifest(bytes));
    REQUIRE(receiver.accept(data_frame(decode_id(id_text), 1, 0, bytes)));
    // A reconciler sees actual partial data plus only remaining reservation, not the
    // entire declaration on top of it. Exact-fit admission above did not allocate 8 GB.
    CHECK_NOTHROW(beamit::host::ClipboardBulkReceiver::recover(root.path));
    const auto other_id = beamit::net::base64url_encode(beamit::net::random_bytes(16));
    CHECK_THROWS_AS(beamit::host::ClipboardBulkReceiver(root.path, other_id, small_manifest(bytes)),
                    beamit::host::ClipboardBulkStorageError);
    // A second OS process uses the same production file lock and live-owner ledger.
    // Only the child inherits this private temporary path; no global configuration is changed.
    constexpr auto variable = L"BEAMIT_TEST_BULK_QUOTA_ROOT";
    REQUIRE(GetEnvironmentVariableW(variable, nullptr, 0) == 0);
    REQUIRE(SetEnvironmentVariableW(variable, root.path.c_str()));
    std::wstring executable(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    executable.resize(length);
    std::wstring command = L"\"" + executable + L"\" \"[.bulk-quota-child]\"";
    STARTUPINFOW startup{.cb = sizeof(STARTUPINFOW)};
    PROCESS_INFORMATION process{};
    const bool launched = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                         nullptr, nullptr, &startup, &process);
    SetEnvironmentVariableW(variable, nullptr);
    REQUIRE(launched);
    const auto wait = WaitForSingleObject(process.hProcess, 10000);
    DWORD exit{};
    GetExitCodeProcess(process.hProcess, &exit);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 1000);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CHECK(wait == WAIT_OBJECT_0);
    CHECK(exit == 0);
    CHECK(std::filesystem::exists(retained));
}

TEST_CASE("Child process respects another live receiver reservation", "[.bulk-quota-child]") {
    std::wstring path(32768, L'\0');
    const auto length = GetEnvironmentVariableW(L"BEAMIT_TEST_BULK_QUOTA_ROOT", path.data(),
                                               static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) SKIP("Invoked only by the shared-quota parent test");
    path.resize(length);
    const std::string content = "abc";
    const auto id = beamit::net::base64url_encode(beamit::net::random_bytes(16));
    CHECK_THROWS_AS(beamit::host::ClipboardBulkReceiver(path, id, small_manifest(std::as_bytes(std::span(content)))),
                    beamit::host::ClipboardBulkStorageError);
}

TEST_CASE("Recovery deletes only dead-owner incomplete operations and preserves committed files",
          "[clipboard-bulk][host-storage]") {
    TemporaryRoot root;
    const auto id = beamit::net::base64url_encode(beamit::net::random_bytes(16));
    const auto partial = root.path / L"client" / L"outgoing" / id;
    const auto committed = root.path / L"host" / L"committed" / id;
    std::filesystem::create_directories(partial);
    std::filesystem::create_directories(committed);
    std::ofstream(partial / L"data").put('x');
    std::ofstream(committed / L"data").put('y');
    std::filesystem::create_directories(root.path / L".reservations");
    const auto record = boost::json::serialize(boost::json::object{
        {"id", id}, {"role", "client"}, {"kind", "outgoing"}, {"pid", UINT32_MAX}, {"owner", 0}, {"bytes", 1}});
    std::ofstream(root.path / L".reservations" / id) << record;
    beamit::host::ClipboardBulkReceiver::recover(root.path);
    CHECK_FALSE(std::filesystem::exists(partial));
    CHECK_FALSE(std::filesystem::exists(root.path / L".reservations" / id));
    CHECK(std::filesystem::exists(committed / L"data"));
}

TEST_CASE("Payload write faults abort spool and receipt after real partial IO",
          "[host-storage-faults]") {
    for (const DWORD error : {ERROR_DISK_FULL, ERROR_ACCESS_DENIED}) {
        CAPTURE(error);
        TemporaryRoot source, staging;
        std::filesystem::create_directories(source.path);
        const auto file = source.path / L"source.bin";
        const std::string original(2 * bulk::max_chunk_size, 's');
        std::ofstream(file, std::ios::binary) << original;
        const auto bytes = std::as_bytes(std::span(original));
        const auto snapshot = beamit::host::ClipboardBulkSnapshot::inspect(
            {.sequence = 42, .files = {file}}, [] { return true; });
        const auto expected = error == ERROR_DISK_FULL ? "disk_full" : "permission_denied";
        const auto spool_id = beamit::net::base64url_encode(beamit::net::random_bytes(16));
        unsigned writes{};
        std::uint64_t prepared{};
        try {
            beamit::host::ClipboardBulkSpool spool(staging.path, spool_id, snapshot, [] { return true; },
                [&](auto count) { prepared = count; }, "host",
                [&]() -> DWORD { return ++writes == 2 ? error : ERROR_SUCCESS; });
            FAIL("A failed spool must not be returned to its sender");
        } catch (const beamit::host::ClipboardBulkStorageError& failure) {
            CHECK(failure.code() == expected);
        }
        CHECK(writes == 2);
        CHECK(prepared == bulk::max_chunk_size);
        CHECK(read_fixture(file) == original);
        CHECK_FALSE(std::filesystem::exists(staging.path / L"host" / L"outgoing" / spool_id));
        CHECK(std::filesystem::is_empty(staging.path / L".reservations"));

        const auto receiver_id = beamit::net::base64url_encode(beamit::net::random_bytes(16));
        const auto id = decode_id(receiver_id);
        std::filesystem::path partial;
        writes = 0;
        {
            beamit::host::ClipboardBulkReceiver receiver(staging.path, receiver_id, small_manifest(bytes), "host",
                [&]() -> DWORD { return ++writes == 2 ? error : ERROR_SUCCESS; });
            partial = receiver.staging_directory();
            REQUIRE(receiver.accept(data_frame(id, 1, 0, bytes.first(bulk::max_chunk_size))));
            CHECK(std::filesystem::file_size(partial / L"folder" / L"hello.txt") == bulk::max_chunk_size);
            const auto failed = receiver.accept(data_frame(id, 1, bulk::max_chunk_size,
                bytes.subspan(bulk::max_chunk_size)));
            REQUIRE_FALSE(failed);
            CHECK(failed.error() == expected);
            CHECK(writes == 2);
            const auto end = bulk::encode_header({.kind = bulk::FrameKind::End, .transfer_id = id,
                .node_index = UINT32_MAX, .offset = bytes.size()});
            CHECK_FALSE(receiver.accept(end));
            CHECK_FALSE(receiver.promote()); // No paths can reach a native publisher.
            CHECK(std::filesystem::is_empty(staging.path / L"host" / L"committed"));
        }
        CHECK_FALSE(std::filesystem::exists(partial));
        CHECK(std::filesystem::is_empty(staging.path / L".reservations"));
        CHECK(read_fixture(file) == original);
        CHECK(snapshot->unchanged([] { return true; }));
    }
}

TEST_CASE("Failed partial cleanup retains its quota reservation conservatively",
          "[host-storage-faults]") {
    TemporaryRoot root;
    constexpr auto declared = 2 * bulk::max_chunk_size;
    sparse_fixture(root.path / L"client" / L"committed" / L"retained",
        8'000'000'000 - 128 * 1024 * 1024 - declared);
    const std::string content(declared, 'p');
    const auto bytes = std::as_bytes(std::span(content));
    const auto text_id = beamit::net::base64url_encode(beamit::net::random_bytes(16));
    const auto id = decode_id(text_id);
    PrivateHandle cleanup_blocker;
    std::filesystem::path partial;
    {
        unsigned writes{};
        beamit::host::ClipboardBulkReceiver receiver(root.path, text_id, small_manifest(bytes), "host",
            [&]() -> DWORD { return ++writes == 2 ? ERROR_DISK_FULL : ERROR_SUCCESS; });
        partial = receiver.staging_directory();
        REQUIRE(receiver.accept(data_frame(id, 1, 0, bytes.first(bulk::max_chunk_size))));
        CHECK_FALSE(receiver.accept(data_frame(id, 1, bulk::max_chunk_size, bytes.subspan(bulk::max_chunk_size))));
        CHECK_FALSE(receiver.promote());
        // A private child handle denying delete sharing deterministically prevents
        // complete cleanup, without modifying an ACL or filling a volume.
        const auto blocked = partial / L"cleanup-blocked";
        REQUIRE(std::filesystem::create_directory(blocked));
        cleanup_blocker.value = CreateFileW(blocked.c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        REQUIRE(cleanup_blocker.value != INVALID_HANDLE_VALUE);
    }
    CHECK(std::filesystem::exists(partial));
    CHECK(std::filesystem::exists(root.path / L".reservations" / text_id));
    CHECK_NOTHROW(beamit::host::ClipboardBulkReceiver::recover(root.path));
    CHECK(std::filesystem::exists(root.path / L".reservations" / text_id)); // Live owner is preserved.
    const std::string other = "abc";
    try {
        beamit::host::ClipboardBulkReceiver receiver(root.path,
            beamit::net::base64url_encode(beamit::net::random_bytes(16)), small_manifest(std::as_bytes(std::span(other))));
        FAIL("Failed cleanup must not make its reservation available");
    } catch (const beamit::host::ClipboardBulkStorageError& failure) {
        CHECK(failure.code() == "quota_exceeded");
    }
    CHECK(std::filesystem::is_empty(root.path / L"host" / L"committed"));
}

TEST_CASE("Pinned destination parents and CREATE_NEW resist hostile private replacement",
          "[host-storage-faults]") {
    TemporaryRoot root, outside;
    std::filesystem::create_directories(outside.path);
    const auto sentinel = outside.path / L"untouched";
    const std::string original = "outside sentinel must not be overwritten";
    std::ofstream(sentinel, std::ios::binary) << original;
    const auto text_id = beamit::net::base64url_encode(beamit::net::random_bytes(16));
    const auto id = decode_id(text_id);
    const std::string content = "abc";
    const auto bytes = std::as_bytes(std::span(content));
    std::filesystem::path partial;
    {
        beamit::host::ClipboardBulkReceiver receiver(root.path, text_id, small_manifest(bytes));
        partial = receiver.staging_directory();
        // Attempt a parent substitution after validation, not just an unsafe
        // manifest. Production directory pins must deny the rename.
        const auto moved = MoveFileExW((partial / L"folder").c_str(), (outside.path / L"moved-parent").c_str(), 0);
        const auto denied = GetLastError();
        REQUIRE_FALSE(moved);
        CHECK((denied == ERROR_SHARING_VIOLATION || denied == ERROR_ACCESS_DENIED));
        const auto planted = partial / L"folder" / L"hello.txt";
        REQUIRE(CreateHardLinkW(planted.c_str(), sentinel.c_str(), nullptr));
        CHECK_FALSE(receiver.accept(data_frame(id, 1, 0, bytes)));
        const auto end = bulk::encode_header({.kind = bulk::FrameKind::End, .transfer_id = id,
            .node_index = UINT32_MAX, .offset = bytes.size()});
        CHECK_FALSE(receiver.accept(end));
        CHECK_FALSE(receiver.promote());
        CHECK(read_fixture(sentinel) == original);
    }
    CHECK(read_fixture(sentinel) == original);
    CHECK_FALSE(std::filesystem::exists(partial));
    CHECK(std::filesystem::is_empty(root.path / L".reservations"));
    CHECK(std::filesystem::is_empty(root.path / L"host" / L"committed"));
}
