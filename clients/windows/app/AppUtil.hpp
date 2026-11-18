#pragma once

#include <windows.h>
// winbase.h's GetCurrentTime macro breaks the XAML animation projection.
#undef GetCurrentTime

#include <winrt/base.h>

#include <filesystem>
#include <functional>
#include <cstdint>
#include <string>
#include <string_view>

namespace beamit::client {

[[nodiscard]] std::wstring widen(std::string_view text);
[[nodiscard]] std::string narrow(std::wstring_view text);
[[nodiscard]] inline winrt::hstring hstring(std::string_view text) { return winrt::hstring(widen(text)); }

// Loads the embedded BeamIt icon at the system small or large size (shared handle; do not destroy).
[[nodiscard]] HICON app_icon(bool large);
// Sets the title-bar/taskbar icons of a top-level window.
void apply_window_icon(HWND hwnd);

// Non-secret preferences. Passwords are never persisted.
struct Settings {
    std::string server_url;
    std::string fingerprint;
    bool trust_any_certificate{};
    bool clipboard_sync{};
    bool anti_idle{};
    bool follow_all_windows{true};
    std::uint64_t clipboard_bulk_byte_threshold{500'000'000};
    std::uint32_t clipboard_bulk_entry_threshold{5};

    [[nodiscard]] static std::filesystem::path path();
    [[nodiscard]] static Settings load();
    // Parses non-secret settings without accessing the user's configuration file.
    // An omitted diagnostic callback uses the Windows debug diagnostic channel.
    [[nodiscard]] static Settings parse(std::string_view, std::function<void(std::string_view)> diagnostic = {});
    void save() const;
};

} // namespace beamit::client
