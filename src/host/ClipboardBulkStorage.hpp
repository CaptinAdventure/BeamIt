#pragma once

#include "net/ClipboardBulk.hpp"
#include "host/Clipboard.hpp"

#include <filesystem>
#include <functional>
#include <atomic>
#include <memory>
#include <span>
#include <string>
#include <stdexcept>
#include <vector>

namespace beamit::host {

struct ClipboardStagingReservation;
// Optional failure-only I/O interceptor, called before a payload WriteFile on the
// serial disk worker. ERROR_SUCCESS still performs the real write. It cannot
// bypass validation, quota admission, hashing, or native filesystem operations.
using ClipboardBulkWriteCheck = std::function<DWORD()>;
class ClipboardBulkSnapshot {
public:
    using Guard = std::function<bool()>;
    using Progress = std::function<void(std::uint64_t)>;
    static std::shared_ptr<ClipboardBulkSnapshot> inspect(ClipboardContent content, Guard current);
    ~ClipboardBulkSnapshot();
    [[nodiscard]] const net::clipboard_bulk::Manifest& manifest() const noexcept;
    [[nodiscard]] boost::json::object preliminary() const;
    [[nodiscard]] bool unchanged(Guard current) const;
    [[nodiscard]] DWORD sequence() const noexcept;
    [[nodiscard]] const ClipboardContent& content() const noexcept;
private:
    ClipboardBulkSnapshot();
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend class ClipboardBulkSpool;
};

// Immutable sender-only files. Handles remain private and deny other writers; only
// bounded reads of this spool, never live source files, may feed the bulk socket.
class ClipboardBulkSpool {
public:
    ClipboardBulkSpool(std::filesystem::path application_root, const std::string& transfer_id,
                       std::shared_ptr<const ClipboardBulkSnapshot> snapshot,
                       ClipboardBulkSnapshot::Guard current, ClipboardBulkSnapshot::Progress progress,
                       std::string role = "host", ClipboardBulkWriteCheck write_check = {});
    ~ClipboardBulkSpool();
    [[nodiscard]] const net::clipboard_bulk::Manifest& manifest() const noexcept;
    [[nodiscard]] std::vector<std::byte> read(std::uint32_t node, std::uint64_t offset);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class ClipboardBulkStorageError : public std::runtime_error {
public:
    explicit ClipboardBulkStorageError(std::string code) : std::runtime_error(code), code_(std::move(code)) {}
    [[nodiscard]] const std::string& code() const noexcept { return code_; }
private:
    std::string code_;
};

// Receives one already-authorized manifest into private, same-volume staging. Calls to accept()
// are serialized by the owning transfer worker; disk writes and hashing complete before it returns.
class ClipboardBulkReceiver {
public:
    ClipboardBulkReceiver(std::filesystem::path application_root, std::string transfer_id,
                          net::clipboard_bulk::Manifest manifest, std::string role = "host",
                          ClipboardBulkWriteCheck write_check = {});
    ~ClipboardBulkReceiver();
    ClipboardBulkReceiver(const ClipboardBulkReceiver&) = delete;
    ClipboardBulkReceiver& operator=(const ClipboardBulkReceiver&) = delete;
    // Reconciles incomplete operations owned by processes confirmed dead. Committed
    // receiver directories are never recovered/deleted automatically.
    static void recover(const std::filesystem::path& application_root);

    [[nodiscard]] std::expected<void, std::string> accept(std::span<const std::byte> frame);
    [[nodiscard]] std::expected<std::vector<std::filesystem::path>, std::string> promote();
    [[nodiscard]] std::expected<std::vector<std::byte>, std::string> prepared_png() const;
    // May be called by the native clipboard callback before posting its result. Retention
    // must survive a stopped network executor after the OS has accepted the paths.
    void published() noexcept { published_.store(true); }
    [[nodiscard]] const std::filesystem::path& staging_directory() const noexcept { return partial_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::filesystem::path root_;
    std::filesystem::path partial_;
    std::filesystem::path committed_;
    net::clipboard_bulk::Manifest manifest_;
    bool promoted_{};
    std::atomic_bool published_{};
    std::unique_ptr<ClipboardStagingReservation> reservation_;
};

[[nodiscard]] std::filesystem::path clipboard_staging_root();

} // namespace beamit::host
