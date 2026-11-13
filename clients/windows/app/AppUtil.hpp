#pragma once

#include <windows.h>
// winbase.h's GetCurrentTime macro breaks the XAML animation projection.
#undef GetCurrentTime

#include <winrt/base.h>

#include <filesystem>
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

    [[nodiscard]] static std::filesystem::path path();
    [[nodiscard]] static Settings load();
    void save() const;
};

} // namespace beamit::client
