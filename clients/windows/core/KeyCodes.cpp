#include "KeyCodes.hpp"

#include <windows.h>

#include <array>
#include <utility>

namespace beamit::client {
namespace {

struct Entry {
    unsigned scan; // 0xE0xx = E0-prefixed (extended)
    std::string_view code;
};

constexpr std::array table{
    Entry{0x01, "Escape"}, Entry{0x02, "Digit1"}, Entry{0x03, "Digit2"}, Entry{0x04, "Digit3"},
    Entry{0x05, "Digit4"}, Entry{0x06, "Digit5"}, Entry{0x07, "Digit6"}, Entry{0x08, "Digit7"},
    Entry{0x09, "Digit8"}, Entry{0x0A, "Digit9"}, Entry{0x0B, "Digit0"}, Entry{0x0C, "Minus"},
    Entry{0x0D, "Equal"}, Entry{0x0E, "Backspace"}, Entry{0x0F, "Tab"}, Entry{0x10, "KeyQ"},
    Entry{0x11, "KeyW"}, Entry{0x12, "KeyE"}, Entry{0x13, "KeyR"}, Entry{0x14, "KeyT"},
    Entry{0x15, "KeyY"}, Entry{0x16, "KeyU"}, Entry{0x17, "KeyI"}, Entry{0x18, "KeyO"},
    Entry{0x19, "KeyP"}, Entry{0x1A, "BracketLeft"}, Entry{0x1B, "BracketRight"}, Entry{0x1C, "Enter"},
    Entry{0x1D, "ControlLeft"}, Entry{0x1E, "KeyA"}, Entry{0x1F, "KeyS"}, Entry{0x20, "KeyD"},
    Entry{0x21, "KeyF"}, Entry{0x22, "KeyG"}, Entry{0x23, "KeyH"}, Entry{0x24, "KeyJ"},
    Entry{0x25, "KeyK"}, Entry{0x26, "KeyL"}, Entry{0x27, "Semicolon"}, Entry{0x28, "Quote"},
    Entry{0x29, "Backquote"}, Entry{0x2A, "ShiftLeft"}, Entry{0x2B, "Backslash"}, Entry{0x2C, "KeyZ"},
    Entry{0x2D, "KeyX"}, Entry{0x2E, "KeyC"}, Entry{0x2F, "KeyV"}, Entry{0x30, "KeyB"},
    Entry{0x31, "KeyN"}, Entry{0x32, "KeyM"}, Entry{0x33, "Comma"}, Entry{0x34, "Period"},
    Entry{0x35, "Slash"}, Entry{0x36, "ShiftRight"}, Entry{0x37, "NumpadMultiply"}, Entry{0x38, "AltLeft"},
    Entry{0x39, "Space"}, Entry{0x3A, "CapsLock"}, Entry{0x3B, "F1"}, Entry{0x3C, "F2"},
    Entry{0x3D, "F3"}, Entry{0x3E, "F4"}, Entry{0x3F, "F5"}, Entry{0x40, "F6"},
    Entry{0x41, "F7"}, Entry{0x42, "F8"}, Entry{0x43, "F9"}, Entry{0x44, "F10"},
    Entry{0xE045, "NumLock"}, Entry{0x46, "ScrollLock"}, Entry{0x47, "Numpad7"}, Entry{0x48, "Numpad8"},
    Entry{0x49, "Numpad9"}, Entry{0x4A, "NumpadSubtract"}, Entry{0x4B, "Numpad4"}, Entry{0x4C, "Numpad5"},
    Entry{0x4D, "Numpad6"}, Entry{0x4E, "NumpadAdd"}, Entry{0x4F, "Numpad1"}, Entry{0x50, "Numpad2"},
    Entry{0x51, "Numpad3"}, Entry{0x52, "Numpad0"}, Entry{0x53, "NumpadDecimal"}, Entry{0x56, "IntlBackslash"},
    Entry{0x57, "F11"}, Entry{0x58, "F12"}, Entry{0x59, "NumpadEqual"}, Entry{0x64, "F13"},
    Entry{0x65, "F14"}, Entry{0x66, "F15"}, Entry{0x67, "F16"}, Entry{0x68, "F17"},
    Entry{0x69, "F18"}, Entry{0x6A, "F19"}, Entry{0x6B, "F20"}, Entry{0x6C, "F21"},
    Entry{0x6D, "F22"}, Entry{0x6E, "F23"}, Entry{0x70, "KanaMode"}, Entry{0x71, "Lang2"},
    Entry{0x72, "Lang1"}, Entry{0x73, "IntlRo"}, Entry{0x76, "F24"}, Entry{0x79, "Convert"},
    Entry{0x7B, "NonConvert"}, Entry{0x7D, "IntlYen"}, Entry{0x7E, "NumpadComma"},
    Entry{0xE010, "MediaTrackPrevious"}, Entry{0xE019, "MediaTrackNext"}, Entry{0xE01C, "NumpadEnter"},
    Entry{0xE01D, "ControlRight"}, Entry{0xE020, "AudioVolumeMute"}, Entry{0xE021, "LaunchApp2"},
    Entry{0xE022, "MediaPlayPause"}, Entry{0xE024, "MediaStop"}, Entry{0xE02E, "AudioVolumeDown"},
    Entry{0xE030, "AudioVolumeUp"}, Entry{0xE032, "BrowserHome"}, Entry{0xE035, "NumpadDivide"},
    Entry{0xE037, "PrintScreen"}, Entry{0xE038, "AltRight"}, Entry{0xE047, "Home"}, Entry{0xE048, "ArrowUp"},
    Entry{0xE049, "PageUp"}, Entry{0xE04B, "ArrowLeft"}, Entry{0xE04D, "ArrowRight"}, Entry{0xE04F, "End"},
    Entry{0xE050, "ArrowDown"}, Entry{0xE051, "PageDown"}, Entry{0xE052, "Insert"}, Entry{0xE053, "Delete"},
    Entry{0xE05B, "MetaLeft"}, Entry{0xE05C, "MetaRight"}, Entry{0xE05D, "ContextMenu"}, Entry{0xE05E, "Power"},
    Entry{0xE05F, "Sleep"}, Entry{0xE063, "WakeUp"}, Entry{0xE065, "BrowserSearch"}, Entry{0xE066, "BrowserFavorites"},
    Entry{0xE067, "BrowserRefresh"}, Entry{0xE068, "BrowserStop"}, Entry{0xE069, "BrowserForward"},
    Entry{0xE06A, "BrowserBack"}, Entry{0xE06B, "LaunchApp1"}, Entry{0xE06C, "LaunchMail"},
    Entry{0xE06D, "MediaSelect"},
};

} // namespace

std::optional<std::string_view> keyboard_code(unsigned scan_code, bool extended, unsigned virtual_key) {
    if (virtual_key == VK_PAUSE) return "Pause";
    // Windows reports NumLock with the extended flag set.
    const unsigned key = (extended ? 0xE000U : 0U) | (scan_code & 0xFFU);
    for (const auto& entry : table) {
        if (entry.scan == key) return entry.code;
    }
    return std::nullopt;
}

} // namespace beamit::client
