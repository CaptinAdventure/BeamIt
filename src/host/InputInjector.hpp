#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <optional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace beamit::host::input {

class ElevatedInput;
enum class MouseMode { Absolute, Relative };
enum class MouseButton : std::uint8_t { Left, Right, Middle, Back, Forward };

// A key as injected: Set-1 scan code (with E0 prefix flag) or, for keys that have no unambiguous scan code
// (Pause, NumLock), a virtual-key code.
struct KeyCode {
    std::uint16_t scan{};
    bool extended{};
    std::uint16_t virtual_key{};

    friend auto operator<=>(const KeyCode&, const KeyCode&) = default;
};

struct ScreenRect {
    long left{};
    long top{};
    long right{};  // exclusive
    long bottom{}; // exclusive
};

struct ScreenPoint {
    long x{};
    long y{};
};

enum class InputResult { Ok, FocusFailed, InjectFailed, TargetGone };

struct ProcessIntegrity {
    DWORD level{};
    DWORD error{};
};

struct InjectionDiagnostic {
    DWORD inserted{};
    DWORD requested{};
    DWORD error{};
    DWORD injector_pid{};
    DWORD foreground_pid{};
    ProcessIntegrity injector;
    ProcessIntegrity foreground;
};
// Numeric-only diagnostics cross the helper boundary; never keys/text/titles.
[[nodiscard]] std::optional<InjectionDiagnostic> take_injection_diagnostic();
void log_injection_diagnostic(const InjectionDiagnostic& diagnostic);

// Token mandatory-integrity RID, or a Win32 error when the process/token cannot be queried.
[[nodiscard]] ProcessIntegrity process_integrity(DWORD process_id);

[[nodiscard]] std::optional<MouseMode> parse_mouse_mode(std::string_view mode);
[[nodiscard]] std::string_view mouse_mode_name(MouseMode mode);
[[nodiscard]] std::optional<MouseButton> parse_mouse_button(std::string_view button);
// Maps a UI Events KeyboardEvent.code value to the key to inject.
[[nodiscard]] std::optional<KeyCode> key_for_code(std::string_view code);
[[nodiscard]] bool supported_key(KeyCode key);

// Maps a video-pixel position (x right, y down from the stream origin) onto `rect`, clamped inside it.
[[nodiscard]] ScreenPoint map_to_rect(double x, double y, std::uint32_t video_width, std::uint32_t video_height,
                                      const ScreenRect& rect);
// Inverse of map_to_rect: maps a screen point to video pixels relative to `rect`'s origin (not clamped).
[[nodiscard]] ScreenPoint map_from_rect(ScreenPoint point, std::uint32_t video_width, std::uint32_t video_height,
                                        const ScreenRect& rect);
[[nodiscard]] bool rect_contains(const ScreenRect& rect, ScreenPoint point);

struct ModifierState {
    bool shift{};
    bool control{};
    bool alt{};
    bool meta{};
    bool caps_lock{};
    bool num_lock{};
    bool scroll_lock{};
};
// Current system-wide modifier and lock-key state.
[[nodiscard]] ModifierState modifier_state();

// Normalizes a screen point to the 0-65535 range SendInput uses with MOUSEEVENTF_VIRTUALDESK.
[[nodiscard]] ScreenPoint normalize_to_virtual_desk(ScreenPoint point, const ScreenRect& virtual_desk);
// Validates text input and converts it to UTF-16; returns nullopt for invalid UTF-8, disallowed control
// characters or more than max_text_units code units.
[[nodiscard]] std::optional<std::wstring> text_to_utf16(std::string_view utf8);

inline constexpr std::size_t max_text_units = 1024;

// Per-stream input state. Window targets get focus management and pointer confinement; a null window means the
// primary monitor (desktop stream). Methods must be called from one thread.
class StreamInput {
public:
    explicit StreamInput(HWND window, std::shared_ptr<ElevatedInput> elevated = {});
    ~StreamInput();
    StreamInput(const StreamInput&) = delete;
    StreamInput& operator=(const StreamInput&) = delete;

    MouseMode mouse_mode{MouseMode::Absolute};

    InputResult move_absolute(double x, double y, std::uint32_t video_width, std::uint32_t video_height);
    InputResult move_relative(double dx, double dy, std::uint32_t video_width, std::uint32_t video_height);
    InputResult button(MouseButton button, bool down);
    InputResult wheel(int dx, int dy);
    InputResult key(KeyCode key, bool down);
    InputResult text(std::wstring_view text);
    // Cursor in video pixels: relative to the primary monitor for desktop streams (may lie outside the video when
    // the cursor is on another monitor); for window streams relative to the window, or nullopt when outside it.
    [[nodiscard]] std::optional<ScreenPoint> cursor_position(std::uint32_t video_width,
                                                             std::uint32_t video_height) const;
    // Releases every key and button this stream holds down.
    void release_all();
    [[nodiscard]] ModifierState modifiers() const;
    // Helper watchdog only: revoke further injections and release held state, even during a hung focus call.
    void cancel();

private:
    [[nodiscard]] std::optional<ScreenRect> target_rect() const;
    [[nodiscard]] InputResult ensure_focus();
    InputResult send_mouse_to(ScreenPoint point);

    HWND window_{};
    std::shared_ptr<ElevatedInput> elevated_;
    std::uint64_t remote_stream_{};
    std::mutex held_mutex_;
    bool cancelled_{};
    DWORD process_id_{};
    std::set<KeyCode> pressed_keys_;
    // Physical button injected for each held logical button, so releases match presses even if the
    // primary/secondary swap setting changes in between.
    std::array<std::optional<MouseButton>, 5> pressed_buttons_{};
    double residual_x_{};
    double residual_y_{};
    std::optional<std::pair<double, double>> virtual_cursor_;
};

} // namespace beamit::host::input
