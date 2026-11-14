#include "host/ClipboardBulkStorage.hpp"

#include "net/Crypto.hpp"

#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace beamit::host {
namespace {

namespace bulk = net::clipboard_bulk;
constexpr std::size_t png_header_size = 24;

struct DirectoryPins {
    std::vector<HANDLE> handles;
    ~DirectoryPins() { clear(); }
    void clear() {
        for (const auto handle : handles) CloseHandle(handle);
        handles.clear();
    }
    void add(const std::filesystem::path& path) {
        // Attribute-only handles do not participate in Windows share-access
        // checks. Directory data access is needed for deny-delete to pin names.
        const auto handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot pin staging directory");
        FILE_ATTRIBUTE_TAG_INFO info{};
        if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info, sizeof(info)) ||
            !(info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            CloseHandle(handle);
            throw std::runtime_error("unsafe staging directory");
        }
        handles.push_back(handle);
    }
    void tree(const std::filesystem::path& path) {
        const auto absolute = std::filesystem::absolute(path).lexically_normal();
        auto current = absolute.root_path();
        add(current);
        for (const auto& component : absolute.relative_path()) {
            current /= component;
            if (!CreateDirectoryW(current.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
                throw std::runtime_error("cannot create staging directory");
            }
            add(current); // Denying delete sharing pins every ancestor against substitution.
        }
    }
};

std::wstring widen_utf8(std::string_view text) {
    if (text.empty() || text.size() > INT_MAX) throw std::invalid_argument("invalid UTF-8 path");
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) throw std::invalid_argument("invalid UTF-8 path");
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                            result.data(), length) != length) {
        throw std::invalid_argument("invalid UTF-8 path");
    }
    return result;
}

void check_directory(const std::filesystem::path& path) {
    if (!CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "create staging directory");
    }
    const HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "open staging directory");
    }
    FILE_ATTRIBUTE_TAG_INFO info{};
    const BOOL inspected = GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info, sizeof(info));
    CloseHandle(handle);
    if (!inspected || !(info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        throw std::runtime_error("staging path is not a regular directory");
    }
}

void check_parent_chain(const std::filesystem::path& root, const std::filesystem::path& relative) {
    auto current = root;
    for (auto component = relative.begin(); component != relative.end(); ++component) {
        auto next = component;
        if (++next == relative.end()) break;
        current /= *component;
        check_directory(current);
    }
}

HANDLE create_staged_file(const std::filesystem::path& root, const std::filesystem::path& relative) {
    check_parent_chain(root, relative);
    const auto destination = root / relative;
    const HANDLE handle = CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return handle;
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info, sizeof(info))) {
        const auto error = GetLastError();
        CloseHandle(handle);
        throw std::system_error(static_cast<int>(error), std::system_category(), "inspect staged file");
    }
    if (info.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
        CloseHandle(handle);
        throw std::runtime_error("staged file is not a regular file");
    }
    return handle;
}

void restrict_to_current_user(const std::filesystem::path& path) {
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "open process token");
    }
    DWORD required{};
    GetTokenInformation(token, TokenUser, nullptr, 0, &required);
    std::vector<std::byte> token_data(required);
    if (!required || !GetTokenInformation(token, TokenUser, token_data.data(), required, &required)) {
        const auto error = GetLastError();
        CloseHandle(token);
        throw std::system_error(static_cast<int>(error), std::system_category(), "read process token");
    }
    CloseHandle(token);
    const auto* user = reinterpret_cast<const TOKEN_USER*>(token_data.data());

    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID system_sid{};
    if (!AllocateAndInitializeSid(&authority, 1, SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0, &system_sid)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "create system SID");
    }
    std::array<EXPLICIT_ACCESSW, 2> entries{};
    const auto set_entry = [](EXPLICIT_ACCESSW& entry, PSID sid, TRUSTEE_TYPE type) {
        entry.grfAccessPermissions = FILE_ALL_ACCESS;
        entry.grfAccessMode = SET_ACCESS;
        entry.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entry.Trustee.TrusteeType = type;
        entry.Trustee.ptstrName = static_cast<LPWSTR>(sid);
    };
    set_entry(entries[0], user->User.Sid, TRUSTEE_IS_USER);
    set_entry(entries[1], system_sid, TRUSTEE_IS_WELL_KNOWN_GROUP);
    PACL acl{};
    const DWORD acl_result = SetEntriesInAclW(static_cast<ULONG>(entries.size()), entries.data(), nullptr, &acl);
    FreeSid(system_sid);
    if (acl_result != ERROR_SUCCESS) {
        throw std::system_error(static_cast<int>(acl_result), std::system_category(), "create staging ACL");
    }
    const DWORD result = SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
                                               DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                               nullptr, nullptr, acl, nullptr);
    LocalFree(acl);
    if (result != ERROR_SUCCESS) {
        throw std::system_error(static_cast<int>(result), std::system_category(), "restrict staging ACL");
    }
}

[[noreturn]] void write_failure(DWORD error) {
    if (error == ERROR_DISK_FULL || error == ERROR_HANDLE_DISK_FULL) throw ClipboardBulkStorageError("disk_full");
    if (error == ERROR_ACCESS_DENIED || error == ERROR_WRITE_PROTECT || error == ERROR_PRIVILEGE_NOT_HELD) {
        throw ClipboardBulkStorageError("permission_denied");
    }
    throw ClipboardBulkStorageError("storage_failed");
}

void write_all(HANDLE handle, std::span<const std::byte> bytes, const ClipboardBulkWriteCheck& check = {}) {
    std::size_t offset{};
    while (offset < bytes.size()) {
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, MAXDWORD));
        DWORD written{};
        if (check) {
            if (const auto error = check(); error != ERROR_SUCCESS) write_failure(error);
        }
        if (!WriteFile(handle, bytes.data() + offset, request, &written, nullptr)) write_failure(GetLastError());
        if (written == 0) write_failure(ERROR_WRITE_FAULT);
        offset += written;
    }
}

std::optional<std::pair<std::uint32_t, std::uint32_t>> png_size(std::span<const std::byte> bytes) {
    constexpr std::array<std::byte, 8> signature{std::byte{0x89}, std::byte{'P'}, std::byte{'N'}, std::byte{'G'},
                                                 std::byte{0x0D}, std::byte{0x0A}, std::byte{0x1A}, std::byte{0x0A}};
    if (bytes.size() < png_header_size || !std::ranges::equal(bytes.first(signature.size()), signature) ||
        std::memcmp(bytes.data() + 12, "IHDR", 4) != 0) return std::nullopt;
    const auto read = [&](std::size_t offset) {
        std::uint32_t value{};
        for (std::size_t index = 0; index < 4; ++index) {
            value = (value << 8) | std::to_integer<std::uint32_t>(bytes[offset + index]);
        }
        return value;
    };
    return std::pair{read(16), read(20)};
}

struct FileHandle {
    HANDLE value{INVALID_HANDLE_VALUE};
    explicit FileHandle(HANDLE handle) : value(handle) {}
    ~FileHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    FileHandle(const FileHandle&) = delete;
};

struct SourceStamp {
    DWORD volume{}, index_high{}, index_low{}, attributes{};
    std::uint64_t size{};
    LONGLONG written{}, changed{};
    bool operator==(const SourceStamp&) const = default;
};

SourceStamp source_stamp(HANDLE file) {
    BY_HANDLE_FILE_INFORMATION info{};
    FILE_BASIC_INFO basic{};
    if (GetFileType(file) != FILE_TYPE_DISK || !GetFileInformationByHandle(file, &info) ||
        !GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic))) {
        throw ClipboardBulkStorageError("source_unavailable");
    }
    if (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) {
        throw ClipboardBulkStorageError("unsupported_entry");
    }
    return {.volume = info.dwVolumeSerialNumber, .index_high = info.nFileIndexHigh,
            .index_low = info.nFileIndexLow, .attributes = info.dwFileAttributes,
            .size = (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow,
            .written = basic.LastWriteTime.QuadPart, .changed = basic.ChangeTime.QuadPart};
}

HANDLE open_source(const std::filesystem::path& path) {
    // Use identical data-read access for preflight and copying. Besides checking
    // readability, this avoids comparing attribute-only cached change timestamps
    // against the refreshed timestamps returned when the data stream is opened.
    const auto handle = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw ClipboardBulkStorageError("source_unavailable");
    return handle;
}

void source_parents(DirectoryPins& pins, const std::filesystem::path& path) {
    if (!path.is_absolute()) throw ClipboardBulkStorageError("unsupported_entry");
    auto current = path.root_path();
    pins.add(current);
    for (const auto& component : path.relative_path().parent_path()) {
        current /= component;
        pins.add(current);
    }
}

std::string path_component(const std::filesystem::path& path) {
    const auto value = path.filename().u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

void require_current(const ClipboardBulkSnapshot::Guard& current) {
    if (!current()) throw ClipboardBulkStorageError("source_changed");
}

struct Selection {
    bulk::Manifest manifest;
    std::vector<std::filesystem::path> paths;
    std::vector<SourceStamp> stamps;
};

Selection scan_selection(const ClipboardContent& content, const ClipboardBulkSnapshot::Guard& current) {
    require_current(current);
    if (content.excluded || std::ranges::any_of(content.omitted, [](const auto& omission) {
            return omission.reason == "excluded";
        })) throw ClipboardBulkStorageError("excluded");
    if (content.files.size() > bulk::max_roots) throw ClipboardBulkStorageError("unsupported_selection");
    for (const auto& omission : content.omitted) {
        if (omission.format == clipboard_files) throw ClipboardBulkStorageError("unsupported_selection");
    }
    Selection result;
    result.manifest.selected_roots = static_cast<std::uint32_t>(content.files.size());
    std::vector<std::pair<std::filesystem::path, std::string>> pending;
    for (const auto& root : content.files) pending.emplace_back(root, path_component(root));
    DirectoryPins directory_pins;
    std::uint64_t regular_bytes{};
    for (std::size_t index = 0; index < pending.size(); ++index) {
        require_current(current);
        const auto [source, relative] = pending[index];
        if (relative.size() > 4096 || pending.size() > bulk::max_nodes) {
            throw ClipboardBulkStorageError("unsupported_selection");
        }
        DirectoryPins parents;
        source_parents(parents, source);
        FileHandle handle(open_source(source));
        const auto stamp = source_stamp(handle.value);
        const bool directory = (stamp.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!directory) {
            if (stamp.size > bulk::max_file_bytes - regular_bytes) {
                throw ClipboardBulkStorageError("unsupported_selection");
            }
            regular_bytes += stamp.size;
        }
        result.manifest.nodes.push_back({.kind = directory ? bulk::Node::Kind::Directory : bulk::Node::Kind::File,
                                         .path = relative, .size = directory ? 0 : stamp.size});
        result.paths.push_back(source);
        result.stamps.push_back(stamp);
        if (directory) {
            directory_pins.add(source);
            std::vector<std::filesystem::path> children;
            for (const auto& child : std::filesystem::directory_iterator(source)) {
                require_current(current);
                if (pending.size() + children.size() >= bulk::max_nodes) {
                    throw ClipboardBulkStorageError("unsupported_selection");
                }
                children.push_back(child.path());
            }
            std::ranges::sort(children);
            for (auto& child : children) pending.emplace_back(child, relative + "/" + path_component(child));
        }
        if (source_stamp(handle.value) != stamp) throw ClipboardBulkStorageError("source_changed");
    }
    // Detect mutations of already-enumerated directories and path replacement.
    for (std::size_t index = 0; index < result.paths.size(); ++index) {
        require_current(current);
        FileHandle handle(open_source(result.paths[index]));
        if (source_stamp(handle.value) != result.stamps[index]) throw ClipboardBulkStorageError("source_changed");
    }
    if (!bulk::validate_manifest(result.manifest)) throw ClipboardBulkStorageError("unsupported_selection");
    if (content.image) {
        const auto& image = *content.image;
        if (image.png.size() > bulk::max_png_bytes) throw ClipboardBulkStorageError("unsupported_selection");
        std::size_t suffix{};
        do {
            const auto path = ".beamit-image-" + std::to_string(suffix++) + ".png";
            result.manifest.nodes.push_back({.kind = bulk::Node::Kind::Png, .path = path,
                .size = image.png.size(), .width = image.width, .height = image.height});
            const auto validation = bulk::validate_manifest(result.manifest);
            if (validation) break;
            result.manifest.nodes.pop_back();
            if (validation.error() != bulk::ValidationError::DuplicatePath) {
                throw ClipboardBulkStorageError("unsupported_selection");
            }
        } while (suffix <= bulk::max_nodes);
    }
    if (!bulk::validate_manifest(result.manifest) ||
        boost::json::serialize(bulk::manifest_json(result.manifest)).size() > bulk::max_manifest_bytes) {
        throw ClipboardBulkStorageError("unsupported_selection");
    }
    require_current(current);
    return result;
}

class StreamingHash {
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    std::vector<UCHAR> object;
public:
    StreamingHash() {
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
            throw std::runtime_error("hash provider failed");
        }
        DWORD length{}, returned{};
        if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&length),
                              sizeof(length), &returned, 0) < 0) {
            BCryptCloseAlgorithmProvider(std::exchange(algorithm, nullptr), 0);
            throw std::runtime_error("hash provider configuration failed");
        }
        object.resize(length);
        if (BCryptCreateHash(algorithm, &hash, object.data(), length, nullptr, 0, 0) < 0) {
            BCryptCloseAlgorithmProvider(std::exchange(algorithm, nullptr), 0);
            throw std::runtime_error("hash initialization failed");
        }
    }
    ~StreamingHash() {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    void update(std::span<const std::byte> bytes) {
        if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())),
                           static_cast<ULONG>(bytes.size()), 0) < 0) throw std::runtime_error("hash update failed");
    }
    net::Sha256 finish() {
        net::Sha256 result{};
        if (BCryptFinishHash(hash, reinterpret_cast<PUCHAR>(result.data()),
                             static_cast<ULONG>(result.size()), 0) < 0) throw std::runtime_error("hash failed");
        return result;
    }
};

} // namespace

// All processes using the application root serialize admission/recovery on the same lock.
// Actual data is counted once; each live ledger adds only its not-yet-materialized bytes
// and scratch. A ledger is removed only after deletion or promotion under this lock.
struct ClipboardStagingReservation {
    static constexpr std::uint64_t quota = 8'000'000'000;
    static constexpr std::uint64_t scratch = 128 * 1024 * 1024;
    std::filesystem::path root, ledger;
    DirectoryPins pins;

    struct Lock {
        HANDLE file{INVALID_HANDLE_VALUE};
        OVERLAPPED overlapped{};
        explicit Lock(const std::filesystem::path& root) {
            file = CreateFileW((root / L".quota-lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                               FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("staging quota lock unavailable");
            FILE_ATTRIBUTE_TAG_INFO info{};
            if (!GetFileInformationByHandleEx(file, FileAttributeTagInfo, &info, sizeof(info)) ||
                (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                CloseHandle(file);
                throw std::runtime_error("staging quota lock failed");
            }
            const auto deadline = GetTickCount64() + 2000;
            while (!LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                                0, MAXDWORD, MAXDWORD, &overlapped)) {
                if (GetLastError() != ERROR_LOCK_VIOLATION || GetTickCount64() >= deadline) {
                    CloseHandle(file);
                    throw std::runtime_error("staging quota lock failed");
                }
                Sleep(10);
            }
        }
        ~Lock() {
            UnlockFileEx(file, 0, MAXDWORD, MAXDWORD, &overlapped);
            CloseHandle(file);
        }
    };

    static std::uint64_t usage(const std::filesystem::path& path) {
        if (!std::filesystem::exists(path)) return 0;
        const auto root_attributes = GetFileAttributesW(path.c_str());
        if (root_attributes == INVALID_FILE_ATTRIBUTES || (root_attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            throw std::runtime_error("unsafe staging quota directory");
        }
        std::uint64_t total{};
        for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
            const auto attributes = GetFileAttributesW(entry.path().c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                throw std::runtime_error("unsafe staging quota entry");
            }
            if (entry.is_regular_file()) {
                const auto bytes = entry.file_size();
                if (bytes > quota || total > quota - bytes) throw ClipboardBulkStorageError("quota_exceeded");
                total += bytes;
            } else if (!entry.is_directory()) throw std::runtime_error("unsupported staging quota entry");
        }
        return total;
    }

    static std::uint64_t process_identity(HANDLE process) {
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) {
            throw std::runtime_error("staging owner identity unavailable");
        }
        return (std::uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    }

    static bool dead(DWORD pid, std::uint64_t identity) {
        const auto process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) return GetLastError() == ERROR_INVALID_PARAMETER;
        const bool ended = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
        bool different{};
        try { different = process_identity(process) != identity; }
        catch (...) { CloseHandle(process); throw; }
        CloseHandle(process);
        return ended || different;
    }

    static std::uint64_t reconcile(const std::filesystem::path& root) {
        std::uint64_t outstanding{};
        for (const auto& entry : std::filesystem::directory_iterator(root / L".reservations")) {
            const auto attributes = GetFileAttributesW(entry.path().c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_REPARSE_POINT |
                                                                          FILE_ATTRIBUTE_DIRECTORY))) {
                throw std::runtime_error("unsafe staging quota ledger");
            }
            if (entry.file_size() > 4096) throw std::runtime_error("invalid staging quota ledger");
            std::ifstream input(entry.path(), std::ios::binary);
            const std::string text{std::istreambuf_iterator<char>(input), {}};
            input.close(); // Windows cannot unlink a ledger while this stream holds it open.
            const auto record = boost::json::parse(text).as_object();
            const auto owner = record.at("pid").to_number<DWORD>();
            const auto identity = record.at("owner").to_number<std::uint64_t>();
            const auto reserved = record.at("bytes").to_number<std::uint64_t>();
            const std::string op(record.at("id").as_string());
            const std::string role(record.at("role").as_string());
            const std::string kind(record.at("kind").as_string());
            if (!bulk::valid_transfer_id(op) || (role != "host" && role != "client") ||
                (kind != "partial" && kind != "outgoing") || reserved > quota) {
                throw std::runtime_error("invalid staging quota ledger");
            }
            const auto operation = root / role / kind / widen_utf8(op);
            if (dead(owner, identity)) {
                DirectoryPins parents;
                parents.tree(root / role / kind);
                (void)usage(operation); // Never follow a reparse point during recovery.
                std::filesystem::remove_all(operation);
                std::filesystem::remove(entry.path());
            } else {
                const auto actual = usage(operation);
                const auto remaining = reserved > actual ? reserved - actual : 0;
                if (outstanding > quota - scratch || remaining > quota - scratch - outstanding) {
                    throw ClipboardBulkStorageError("quota_exceeded");
                }
                outstanding += remaining + scratch;
            }
        }
        return outstanding;
    }

    ClipboardStagingReservation(std::filesystem::path application_root, const std::string& id,
                               std::uint64_t bytes, std::string kind = "partial", std::string role = "host")
        : root(std::move(application_root)) {
        if (role != "host" && role != "client") throw std::invalid_argument("invalid staging role");
        pins.tree(root);
        restrict_to_current_user(root);
        check_directory(root / L".reservations");
        restrict_to_current_user(root / L".reservations");
        pins.add(root / L".reservations");
        Lock lock(root);
        const auto outstanding = reconcile(root);
        const auto actual = usage(root / L"host") + usage(root / L"client");
        if (bytes > quota - scratch || actual > quota - bytes - scratch ||
            outstanding > quota - bytes - scratch - actual) throw ClipboardBulkStorageError("quota_exceeded");
        ULARGE_INTEGER available{};
        if (!GetDiskFreeSpaceExW(root.c_str(), &available, nullptr, nullptr) ||
            available.QuadPart < bytes + scratch + outstanding) throw ClipboardBulkStorageError("insufficient_space");
        for (const auto* part : {L"host", L"client"}) {
            check_directory(root / part);
            restrict_to_current_user(root / part);
            pins.add(root / part);
        }
        for (const auto* part : {L"partial", L"committed", L"outgoing"}) {
            check_directory(root / role / part);
            restrict_to_current_user(root / role / part);
            pins.add(root / role / part);
        }
        ledger = root / L".reservations" / widen_utf8(id);
        const auto text = boost::json::serialize(boost::json::object{
            {"id", id}, {"role", role}, {"kind", kind}, {"pid", GetCurrentProcessId()},
            {"owner", process_identity(GetCurrentProcess())}, {"bytes", bytes}});
        const auto file = CreateFileW(ledger.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                      FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("could not reserve staging quota");
        try {
            write_all(file, std::as_bytes(std::span(text)));
            if (!FlushFileBuffers(file)) throw std::runtime_error("could not persist staging reservation");
        } catch (...) {
            CloseHandle(file);
            std::filesystem::remove(ledger);
            throw;
        }
        CloseHandle(file);
    }

    void release() {
        Lock lock(root);
        std::filesystem::remove(ledger);
    }
};

struct ClipboardBulkSnapshot::Impl {
    ClipboardContent content;
    Selection selection;
};

ClipboardBulkSnapshot::ClipboardBulkSnapshot() : impl_(std::make_unique<Impl>()) {}
ClipboardBulkSnapshot::~ClipboardBulkSnapshot() = default;

std::shared_ptr<ClipboardBulkSnapshot> ClipboardBulkSnapshot::inspect(ClipboardContent content, Guard current) {
    auto result = std::shared_ptr<ClipboardBulkSnapshot>(new ClipboardBulkSnapshot);
    result->impl_->selection = scan_selection(content, current);
    result->impl_->content = std::move(content);
    return result;
}
const bulk::Manifest& ClipboardBulkSnapshot::manifest() const noexcept { return impl_->selection.manifest; }
DWORD ClipboardBulkSnapshot::sequence() const noexcept { return impl_->content.sequence; }
const ClipboardContent& ClipboardBulkSnapshot::content() const noexcept { return impl_->content; }

bool ClipboardBulkSnapshot::unchanged(Guard current) const {
    const auto fresh = scan_selection(impl_->content, current);
    return fresh.paths == impl_->selection.paths && fresh.stamps == impl_->selection.stamps &&
        bulk::manifest_json(fresh.manifest) == bulk::manifest_json(impl_->selection.manifest);
}

boost::json::object ClipboardBulkSnapshot::preliminary() const {
    auto object = bulk::manifest_json(manifest());
    for (auto& node : object.at("nodes").as_array()) node.as_object().erase("sha256");
    boost::json::array identities;
    for (std::size_t index = 0; index < impl_->selection.paths.size(); ++index) {
        const auto& node = manifest().nodes[index];
        if (node.path.find('/') != std::string::npos) continue;
        const auto& stamp = impl_->selection.stamps[index];
        // Encode identity fields individually; never hash structure padding or expose absolute paths.
        const std::array<std::uint64_t, 3> identity{stamp.volume, stamp.index_high, stamp.index_low};
        identities.push_back({{"path", node.path},
            {"identity", net::base64url_encode(net::sha256(std::as_bytes(std::span(identity))))}});
    }
    object["roots"] = std::move(identities);
    return object;
}

struct ClipboardBulkSpool::Impl {
    std::unique_ptr<ClipboardStagingReservation> reservation;
    std::filesystem::path directory;
    std::vector<std::unique_ptr<FileHandle>> files;
    std::unique_ptr<FileHandle> directory_handle;
    bulk::Manifest manifest;
    bool created{};
    ~Impl() {
        files.clear();
        directory_handle.reset();
        if (!reservation) return;
        std::error_code error;
        if (created) std::filesystem::remove_all(directory, error);
        if (!error) {
            try { reservation->release(); }
            catch (const std::exception&) {
                spdlog::warn("Clipboard outgoing reservation cleanup failed; recovery will recheck it");
            }
        } else spdlog::warn("Clipboard outgoing spool cleanup failed; data remains quota-accounted");
    }
};

ClipboardBulkSpool::ClipboardBulkSpool(std::filesystem::path root, const std::string& id,
    std::shared_ptr<const ClipboardBulkSnapshot> snapshot, ClipboardBulkSnapshot::Guard current,
    ClipboardBulkSnapshot::Progress progress, std::string role, ClipboardBulkWriteCheck write_check)
    : impl_(std::make_unique<Impl>()) {
    if (!bulk::valid_transfer_id(id)) throw ClipboardBulkStorageError("invalid_transfer");
    require_current(current);
    if (!snapshot->unchanged(current)) throw ClipboardBulkStorageError("source_changed");
    impl_->manifest = snapshot->manifest();
    impl_->reservation = std::make_unique<ClipboardStagingReservation>(
        root, id, bulk::payload_bytes(impl_->manifest), "outgoing", role);
    impl_->directory = root / role / L"outgoing" / widen_utf8(id);
    if (!CreateDirectoryW(impl_->directory.c_str(), nullptr)) throw std::runtime_error("spool creation failed");
    impl_->created = true;
    impl_->directory_handle = std::make_unique<FileHandle>(CreateFileW(impl_->directory.c_str(),
        DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (impl_->directory_handle->value == INVALID_HANDLE_VALUE) throw std::runtime_error("spool pin failed");
    std::vector<std::byte> buffer(bulk::max_chunk_size);
    std::uint64_t completed{};
    for (std::size_t index = 0; index < impl_->manifest.nodes.size(); ++index) {
        require_current(current);
        auto& node = impl_->manifest.nodes[index];
        impl_->files.push_back(nullptr);
        if (node.kind == bulk::Node::Kind::Directory) continue;
        const auto path = impl_->directory / std::to_wstring(index);
        auto destination = std::make_unique<FileHandle>(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (destination->value == INVALID_HANDLE_VALUE) throw std::runtime_error("spool file creation failed");
        StreamingHash hash;
        std::uint64_t offset{};
        if (node.kind == bulk::Node::Kind::Png) {
            const auto& png = snapshot->content().image->png;
            while (offset < png.size()) {
                require_current(current);
                const auto bytes = std::span(png).subspan(static_cast<std::size_t>(offset),
                    std::min<std::size_t>(bulk::max_chunk_size, png.size() - static_cast<std::size_t>(offset)));
                write_all(destination->value, bytes, write_check);
                hash.update(bytes);
                offset += bytes.size();
                progress(completed + offset);
            }
        } else {
            const auto& selection = snapshot->impl_->selection;
            DirectoryPins parents;
            source_parents(parents, selection.paths[index]);
            FileHandle source(open_source(selection.paths[index]));
            const auto before_copy = source_stamp(source.value);
            if (before_copy != selection.stamps[index]) {
                throw ClipboardBulkStorageError("source_changed");
            }
            while (offset < node.size) {
                require_current(current);
                const auto count = static_cast<DWORD>(std::min<std::uint64_t>(buffer.size(), node.size - offset));
                DWORD read{};
                if (!ReadFile(source.value, buffer.data(), count, &read, nullptr)) {
                    throw ClipboardBulkStorageError("source_unavailable");
                }
                if (!read) throw ClipboardBulkStorageError("source_changed");
                const auto bytes = std::span(buffer).first(read);
                write_all(destination->value, bytes, write_check);
                hash.update(bytes);
                offset += read;
                progress(completed + offset);
            }
            if (source_stamp(source.value) != selection.stamps[index]) {
                throw ClipboardBulkStorageError("source_changed");
            }
            FileHandle path_now(open_source(selection.paths[index]));
            if (source_stamp(path_now.value) != selection.stamps[index]) {
                throw ClipboardBulkStorageError("source_changed");
            }
        }
        node.sha256 = hash.finish();
        if (!FlushFileBuffers(destination->value)) throw std::runtime_error("spool flush failed");
        completed += offset;
        impl_->files.back() = std::move(destination); // Keep handles denying any external writer until terminal cleanup.
    }
    require_current(current);
    if (!snapshot->unchanged(current)) throw ClipboardBulkStorageError("source_changed");
}
ClipboardBulkSpool::~ClipboardBulkSpool() = default;
const bulk::Manifest& ClipboardBulkSpool::manifest() const noexcept { return impl_->manifest; }

std::vector<std::byte> ClipboardBulkSpool::read(std::uint32_t node, std::uint64_t offset) {
    if (node >= impl_->files.size() || !impl_->files[node] || offset >= impl_->manifest.nodes[node].size) {
        throw std::runtime_error("invalid spool read");
    }
    const auto count = static_cast<DWORD>(std::min<std::uint64_t>(
        bulk::max_chunk_size, impl_->manifest.nodes[node].size - offset));
    std::vector<std::byte> result(count);
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    DWORD read{};
    const auto file = impl_->files[node]->value;
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN) ||
        !ReadFile(file, result.data(), count, &read, nullptr) || read != count) {
        throw std::runtime_error("spool read failed");
    }
    return result;
}

struct ClipboardBulkReceiver::Impl {
    explicit Impl(std::array<std::byte, 16> transfer_id, std::span<const bulk::Node> nodes)
        : sequence(transfer_id, nodes) {
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
            throw std::runtime_error("SHA-256 provider is unavailable");
        }
        DWORD length{};
        DWORD returned{};
        if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&length), sizeof(length),
                              &returned, 0) < 0 || returned != sizeof(length)) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
            algorithm = nullptr;
            throw std::runtime_error("SHA-256 provider configuration failed");
        }
        hash_object.resize(length);
    }

    ~Impl() {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        if (directory != INVALID_HANDLE_VALUE) CloseHandle(directory);
    }

    void begin_hash() {
        if (BCryptCreateHash(algorithm, &hash, reinterpret_cast<PUCHAR>(hash_object.data()),
                             static_cast<ULONG>(hash_object.size()), nullptr, 0, 0) < 0) {
            throw std::runtime_error("SHA-256 initialization failed");
        }
    }

    void finish_hash(const bulk::Node& node) {
        std::array<std::byte, 32> digest{};
        if (BCryptFinishHash(hash, reinterpret_cast<PUCHAR>(digest.data()), static_cast<ULONG>(digest.size()), 0) < 0) {
            throw std::runtime_error("SHA-256 finalization failed");
        }
        BCryptDestroyHash(std::exchange(hash, nullptr));
        if (!net::constant_time_equal(digest, node.sha256)) {
            throw ClipboardBulkStorageError("digest_mismatch");
        }
        if (node.kind == bulk::Node::Kind::Png) {
            const auto dimensions = png_size(png_prefix);
            if (!dimensions || dimensions->first != node.width || dimensions->second != node.height) {
                throw ClipboardBulkStorageError("invalid_png");
            }
        }
        CloseHandle(std::exchange(file, INVALID_HANDLE_VALUE));
        png_prefix.clear();
    }

    bulk::FrameSequence sequence;
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    std::vector<UCHAR> hash_object;
    HANDLE file{INVALID_HANDLE_VALUE};
    HANDLE directory{INVALID_HANDLE_VALUE};
    DirectoryPins directories;
    std::vector<std::byte> png_prefix;
    ClipboardBulkWriteCheck write_check;
    bool ended{};
    bool failed{};
};

std::filesystem::path clipboard_staging_root() {
    PWSTR known_folder{};
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &known_folder);
    if (FAILED(result)) throw std::runtime_error("application data directory is unavailable");
    std::filesystem::path root(known_folder);
    CoTaskMemFree(known_folder);
    return root / L"BeamIt" / L"ClipboardStaging";
}

void ClipboardBulkReceiver::recover(const std::filesystem::path& application_root) {
    DirectoryPins pins;
    pins.tree(application_root);
    restrict_to_current_user(application_root);
    check_directory(application_root / L".reservations");
    restrict_to_current_user(application_root / L".reservations");
    pins.add(application_root / L".reservations");
    ClipboardStagingReservation::Lock lock(application_root);
    (void)ClipboardStagingReservation::reconcile(application_root);
}

ClipboardBulkReceiver::ClipboardBulkReceiver(std::filesystem::path application_root, std::string transfer_id,
                                             bulk::Manifest manifest, std::string role, ClipboardBulkWriteCheck write_check)
    : root_(application_root / role), manifest_(std::move(manifest)) {
    if (!bulk::valid_transfer_id(transfer_id)) throw std::invalid_argument("transfer identifier is invalid");
    const auto id = net::base64url_decode(transfer_id);
    if (!id || id->size() != 16) throw std::invalid_argument("transfer identifier is invalid");
    if (const auto valid = bulk::validate_manifest(manifest_); !valid) {
        throw std::invalid_argument("clipboard manifest is invalid");
    }
    reservation_ = std::make_unique<ClipboardStagingReservation>(
        application_root, transfer_id, bulk::payload_bytes(manifest_), "partial", role);

    bool created{};
    try {
        check_directory(root_);
        restrict_to_current_user(root_);
        const auto partial_root = root_ / L"partial";
        const auto committed_root = root_ / L"committed";
        check_directory(partial_root);
        check_directory(committed_root);
        partial_ = partial_root / widen_utf8(transfer_id);
        committed_ = committed_root / widen_utf8(transfer_id);
        if (!CreateDirectoryW(partial_.c_str(), nullptr)) {
            throw std::runtime_error("could not create transfer staging");
        }
        created = true;
        std::array<std::byte, 16> raw_id{};
        std::ranges::copy(*id, raw_id.begin());
        impl_ = std::make_unique<Impl>(raw_id, manifest_.nodes);
        impl_->write_check = std::move(write_check);
        impl_->directory = CreateFileW(partial_.c_str(), DELETE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (impl_->directory == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot pin transfer staging");

        std::vector<std::filesystem::path> directories;
        for (const auto& node : manifest_.nodes) {
            const auto relative = std::filesystem::path(widen_utf8(node.path));
            if (node.kind == bulk::Node::Kind::Directory) directories.push_back(relative);
        }
        std::ranges::sort(directories, [](const auto& left, const auto& right) {
            const auto left_depth = std::distance(left.begin(), left.end());
            const auto right_depth = std::distance(right.begin(), right.end());
            return left_depth == right_depth ? left.native() < right.native() : left_depth < right_depth;
        });
        for (const auto& directory : directories) {
            check_directory(partial_ / directory);
            impl_->directories.add(partial_ / directory);
        }

        for (const auto& node : manifest_.nodes) {
            if (node.kind != bulk::Node::Kind::Directory && node.size == 0) {
                const auto relative = std::filesystem::path(widen_utf8(node.path));
                const HANDLE handle = create_staged_file(partial_, relative);
                if (handle == INVALID_HANDLE_VALUE) {
                    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                            "create empty staged file");
                }
                impl_->file = handle;
                impl_->begin_hash();
                impl_->finish_hash(node);
            }
        }
    } catch (...) {
        impl_.reset();
        std::error_code ignored;
        if (created) std::filesystem::remove_all(partial_, ignored);
        if (!ignored) reservation_->release();
        throw;
    }
}

ClipboardBulkReceiver::~ClipboardBulkReceiver() {
    impl_.reset();
    if (!published_ && !partial_.empty()) {
        std::error_code ignored;
        std::filesystem::remove_all(promoted_ ? committed_ : partial_, ignored);
        if (ignored) spdlog::warn("Clipboard incomplete staging cleanup failed; retained data still consumes quota");
        if (!ignored && reservation_) {
            // Failure to remove a ledger is conservative: later admission still counts it
            // until dead-owner recovery. Destructors cannot report filesystem errors.
            try { reservation_->release(); }
            catch (const std::exception&) {
                spdlog::warn("Clipboard staging reservation release failed; dead-owner recovery will recheck it");
            }
        }
    }
}

std::expected<void, std::string> ClipboardBulkReceiver::accept(std::span<const std::byte> frame) {
    if (!impl_ || impl_->ended || impl_->failed) return std::unexpected("transfer is already terminal");
    const auto accepted = impl_->sequence.accept(frame);
    if (!accepted) {
        impl_->failed = true;
        return std::unexpected(accepted.error());
    }
    const auto& header = *accepted;
    if (header.kind == bulk::FrameKind::End) {
        impl_->ended = true;
        return {};
    }
    const auto& node = manifest_.nodes[header.node_index];
    if (node.size == 0 || node.kind == bulk::Node::Kind::Directory) {
        impl_->failed = true;
        return std::unexpected("unexpected payload for an empty manifest node");
    }
    if (impl_->file == INVALID_HANDLE_VALUE) {
        const auto relative = std::filesystem::path(widen_utf8(node.path));
        impl_->file = create_staged_file(partial_, relative);
        if (impl_->file == INVALID_HANDLE_VALUE) {
            impl_->failed = true;
            return std::unexpected("could not create staged file");
        }
        try {
            impl_->begin_hash();
        } catch (const std::exception& error) {
            CloseHandle(std::exchange(impl_->file, INVALID_HANDLE_VALUE));
            impl_->failed = true;
            return std::unexpected(error.what());
        }
        if (node.kind == bulk::Node::Kind::Png) impl_->png_prefix.reserve(png_header_size);
    }
    const auto payload = frame.subspan(bulk::frame_header_size);
    try {
        write_all(impl_->file, payload, impl_->write_check);
        if (BCryptHashData(impl_->hash, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(payload.data())),
                           static_cast<ULONG>(payload.size()), 0) < 0) {
            throw std::runtime_error("SHA-256 update failed");
        }
        if (node.kind == bulk::Node::Kind::Png && impl_->png_prefix.size() < png_header_size) {
            const auto count = std::min(png_header_size - impl_->png_prefix.size(), payload.size());
            impl_->png_prefix.insert(impl_->png_prefix.end(), payload.begin(), payload.begin() + count);
        }
        if (header.offset + header.payload_size == node.size) impl_->finish_hash(node);
    } catch (const ClipboardBulkStorageError& error) {
        impl_->failed = true;
        return std::unexpected(error.code());
    } catch (const std::exception&) {
        impl_->failed = true;
        return std::unexpected("storage_failed");
    }
    return {};
}

std::expected<std::vector<std::filesystem::path>, std::string> ClipboardBulkReceiver::promote() {
    if (!impl_ || !impl_->ended || impl_->failed) return std::unexpected("transfer has no valid END frame");
    if (impl_->file != INVALID_HANDLE_VALUE || impl_->hash) {
        return std::unexpected("transfer ended before file verification completed");
    }
    ClipboardStagingReservation::Lock quota_lock(reservation_->root);
    impl_->directories.clear();
    const auto target = std::filesystem::absolute(committed_).native();
    std::vector<std::byte> rename_buffer(sizeof(FILE_RENAME_INFO) + target.size() * sizeof(wchar_t));
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(rename_buffer.data());
    rename->ReplaceIfExists = FALSE;
    rename->RootDirectory = nullptr;
    rename->FileNameLength = static_cast<DWORD>(target.size() * sizeof(wchar_t));
    std::memcpy(rename->FileName, target.data(), rename->FileNameLength);
    if (!SetFileInformationByHandle(impl_->directory, FileRenameInfo, rename,
                                    static_cast<DWORD>(rename_buffer.size()))) {
        return std::unexpected("could not commit staged clipboard content");
    }
    CloseHandle(std::exchange(impl_->directory, INVALID_HANDLE_VALUE));
    promoted_ = true;
    // Promotion and reservation conversion are one admission transaction: concurrent
    // processes cannot count both the newly committed files and the old reservation.
    std::filesystem::remove(reservation_->ledger);
    std::vector<std::filesystem::path> roots;
    for (const auto& node : manifest_.nodes) {
        if (node.kind == bulk::Node::Kind::Png || node.path.find('/') != std::string::npos) continue;
        const auto path = committed_ / std::filesystem::path(widen_utf8(node.path));
        if (std::ranges::find(roots, path) == roots.end()) roots.push_back(path);
    }
    return roots;
}

std::expected<std::vector<std::byte>, std::string> ClipboardBulkReceiver::prepared_png() const {
    for (const auto& node : manifest_.nodes) {
        if (node.kind != bulk::Node::Kind::Png) continue;
        const auto path = (promoted_ ? committed_ : partial_) / widen_utf8(node.path);
        std::ifstream file(path, std::ios::binary);
        std::vector<std::byte> bytes(static_cast<std::size_t>(node.size));
        if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
            return std::unexpected("could not read verified PNG");
        }
        return bytes;
    }
    return std::vector<std::byte>{};
}

} // namespace beamit::host
