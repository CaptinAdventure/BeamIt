#pragma once

#include <optional>
#include <string_view>

namespace beamit::client {

// PC Set-1 scan code (from WM_KEYDOWN lParam bits 16-23, extended bit 24) -> KeyboardEvent.code
// (PROTOCOL.md §5.9.5). Inverse of the host's table; the Pause key is identified by virtual key.
[[nodiscard]] std::optional<std::string_view> keyboard_code(unsigned scan_code, bool extended, unsigned virtual_key);

} // namespace beamit::client
