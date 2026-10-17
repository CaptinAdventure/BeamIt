#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace beamit::host {

inline constexpr std::uint32_t max_window_icon_size = 256;

struct WindowIcon {
    std::vector<std::byte> png;
    std::uint32_t width{};
    std::uint32_t height{};
    // "window" (WM_GETICON / class icon) or "executable" (the process image's first icon resource).
    std::string_view source;
};

// Converts an icon to a straight-alpha BGRA PNG, scaled down to fit `max_size` x `max_size` (never scaled up).
// The calling thread must have initialized COM.
[[nodiscard]] std::optional<WindowIcon> png_from_icon(HICON icon, std::uint32_t max_size);

// The window's own icon if it is at least `max_size` pixels, otherwise its executable's icon extracted at `max_size`,
// otherwise whatever window icon exists. Blocks for up to ~200 ms on hung windows; call off the network thread.
// The calling thread must have initialized COM.
[[nodiscard]] std::optional<WindowIcon> window_icon(HWND window, std::uint32_t max_size);

} // namespace beamit::host
