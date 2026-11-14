#pragma once

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace beamit::host {

// Bit flags naming the clipboard formats exposed by PROTOCOL.md §5.11.
enum ClipboardFormat : unsigned {
    clipboard_text = 1U << 0,
    clipboard_html = 1U << 1,
    clipboard_image = 1U << 2,
    clipboard_files = 1U << 3,
    clipboard_all = clipboard_text | clipboard_html | clipboard_image,
};

struct ClipboardImage {
    std::vector<std::byte> png;
    std::uint32_t width{};
    std::uint32_t height{};
};

struct ClipboardOmission {
    ClipboardFormat format{};
    std::string reason; // "too_large", "unsupported" or "excluded"
};

struct ClipboardContent {
    DWORD sequence{};
    std::optional<std::string> text; // UTF-8
    std::optional<std::string> html; // UTF-8 fragment of the "HTML Format" data
    std::optional<ClipboardImage> image;
    // Local file references. Never serialize these absolute paths as control metadata.
    std::vector<std::filesystem::path> files;
    std::vector<ClipboardOmission> omitted;
    bool excluded{};
};

struct ClipboardReadResult {
    std::optional<ClipboardContent> content;
    std::string error; // set when the clipboard could not be opened/read
};

// Content a client asks the host to place on the clipboard (PROTOCOL.md §5.11.3). It replaces the whole clipboard.
struct ClipboardWrite {
    std::optional<std::string> text;   // UTF-8
    std::optional<std::string> html;   // UTF-8 fragment
    std::optional<std::string> image;  // PNG, standard base64
    std::optional<std::vector<std::byte>> png; // Binary PNG from a verified bulk transfer.
    // Absolute paths to already staged, immutable files or directories.
    std::vector<std::filesystem::path> files;
    bool sensitive{};                  // hide from clipboard history, cloud clipboard and clipboard monitors
    std::optional<DWORD> expected_sequence;
    std::shared_ptr<std::atomic_bool> publication_allowed;
    std::optional<HWND> expected_foreground;
};

struct ClipboardWriteResult {
    DWORD sequence{};
    std::string error_code; // empty on success: "invalid_request" or "clipboard_unavailable"
    std::string error;
    // Conservative retention after an OS replacement/rollback failure. This does
    // not mean publication succeeded and must never produce a success acknowledgement.
    bool retain_files{};
};

// A PNG decoded into a packed bottom-up 32-bit CF_DIB (alpha composited onto white, as most paste targets ignore it).
struct ClipboardDib {
    std::vector<std::byte> dib;
    std::uint32_t width{};
    std::uint32_t height{};
};

struct ClipboardChange {
    DWORD sequence{};
    unsigned formats{}; // ClipboardFormat bits currently available (zero when excluded)
    bool excluded{};    // the owner asked monitors not to process this content (e.g. a password manager)
};

// Extracts the fragment (or, failing that, the whole document) of Windows "HTML Format" clipboard data as UTF-8.
[[nodiscard]] std::optional<std::string> html_clipboard_fragment(std::string_view cf_html);
// Wraps a UTF-8 HTML fragment into Windows "HTML Format" data with correct byte offsets.
[[nodiscard]] std::string html_clipboard_document(std::string_view fragment);
// Prepends a BITMAPFILEHEADER to a packed CF_DIB/CF_DIBV5 so that it can be decoded as a .bmp file.
[[nodiscard]] std::optional<std::vector<std::byte>> bitmap_file_from_dib(std::span<const std::byte> dib);
// Converts a packed CF_DIB to PNG with WIC; the calling thread must have initialized COM.
[[nodiscard]] std::optional<ClipboardImage> png_from_dib(std::span<const std::byte> dib);
// Decodes a PNG with WIC (at most 16384 pixels per side and 64 Mpixel); the calling thread must have initialized COM.
[[nodiscard]] std::optional<ClipboardDib> dib_from_png(std::span<const std::byte> png);
// Width and height from a PNG's IHDR chunk.
[[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint32_t>> png_dimensions(std::span<const std::byte> png);
// Builds a native CF_HDROP payload for already-staged local paths. Rejects missing or reparse-point roots.
[[nodiscard]] std::expected<std::vector<std::byte>, std::string>
encode_file_drop(std::span<const std::filesystem::path> paths);
// Base64 (RFC 4648 §4, padded) length of `bytes` input bytes.
[[nodiscard]] constexpr std::size_t base64_length(std::size_t bytes) { return (bytes + 2) / 3 * 4; }

// Owns a hidden message-only window on its own thread that listens for clipboard updates and serves read requests.
// All callbacks run on that thread; the clipboard is held open only while its data is copied out.
class ClipboardMonitor {
public:
    using ChangeCallback = std::function<void(const ClipboardChange&)>;
    using ReadCallback = std::function<void(ClipboardReadResult)>;

    explicit ClipboardMonitor(ChangeCallback on_change);
    ~ClipboardMonitor();
    ClipboardMonitor(const ClipboardMonitor&) = delete;
    ClipboardMonitor& operator=(const ClipboardMonitor&) = delete;

    // Queues a read of the requested `formats`. Text/HTML bytes plus the base64 PNG size must fit `max_bytes`;
    // formats that do not fit are reported in `omitted` as "too_large".
    void read(unsigned formats, std::size_t max_bytes, ReadCallback done);
    // Negotiated mode budgets text/HTML separately from the capped binary PNG.
    void read_bulk(unsigned formats, ReadCallback done);
    // Queues replacing the clipboard with `content`. Changes made this way are not reported to the change callback.
    void write(ClipboardWrite content, std::function<void(ClipboardWriteResult)> done);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace beamit::host
