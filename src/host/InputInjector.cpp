#include "host/InputInjector.hpp"
#include "host/ElevatedInput.hpp"

#include <dwmapi.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <span>
#include <vector>

namespace beamit::host::input {
namespace {

thread_local std::optional<InjectionDiagnostic> pending_diagnostic;

struct KeyEntry {
    std::string_view code;
    KeyCode key;
};

constexpr KeyCode scan(std::uint16_t code) {
    return {.scan = static_cast<std::uint16_t>(code & 0xFF), .extended = (code & 0xFF00) == 0xE000};
}

// UI Events KeyboardEvent.code -> PC Set-1 scan code (0xE0xx = E0-prefixed), matching Chromium's Windows mapping.
constexpr std::array key_table{
    KeyEntry{"Escape", scan(0x01)}, KeyEntry{"Digit1", scan(0x02)}, KeyEntry{"Digit2", scan(0x03)},
    KeyEntry{"Digit3", scan(0x04)}, KeyEntry{"Digit4", scan(0x05)}, KeyEntry{"Digit5", scan(0x06)},
    KeyEntry{"Digit6", scan(0x07)}, KeyEntry{"Digit7", scan(0x08)}, KeyEntry{"Digit8", scan(0x09)},
    KeyEntry{"Digit9", scan(0x0A)}, KeyEntry{"Digit0", scan(0x0B)}, KeyEntry{"Minus", scan(0x0C)},
    KeyEntry{"Equal", scan(0x0D)}, KeyEntry{"Backspace", scan(0x0E)}, KeyEntry{"Tab", scan(0x0F)},
    KeyEntry{"KeyQ", scan(0x10)}, KeyEntry{"KeyW", scan(0x11)}, KeyEntry{"KeyE", scan(0x12)},
    KeyEntry{"KeyR", scan(0x13)}, KeyEntry{"KeyT", scan(0x14)}, KeyEntry{"KeyY", scan(0x15)},
    KeyEntry{"KeyU", scan(0x16)}, KeyEntry{"KeyI", scan(0x17)}, KeyEntry{"KeyO", scan(0x18)},
    KeyEntry{"KeyP", scan(0x19)}, KeyEntry{"BracketLeft", scan(0x1A)}, KeyEntry{"BracketRight", scan(0x1B)},
    KeyEntry{"Enter", scan(0x1C)}, KeyEntry{"ControlLeft", scan(0x1D)}, KeyEntry{"KeyA", scan(0x1E)},
    KeyEntry{"KeyS", scan(0x1F)}, KeyEntry{"KeyD", scan(0x20)}, KeyEntry{"KeyF", scan(0x21)},
    KeyEntry{"KeyG", scan(0x22)}, KeyEntry{"KeyH", scan(0x23)}, KeyEntry{"KeyJ", scan(0x24)},
    KeyEntry{"KeyK", scan(0x25)}, KeyEntry{"KeyL", scan(0x26)}, KeyEntry{"Semicolon", scan(0x27)},
    KeyEntry{"Quote", scan(0x28)}, KeyEntry{"Backquote", scan(0x29)}, KeyEntry{"ShiftLeft", scan(0x2A)},
    KeyEntry{"Backslash", scan(0x2B)}, KeyEntry{"KeyZ", scan(0x2C)}, KeyEntry{"KeyX", scan(0x2D)},
    KeyEntry{"KeyC", scan(0x2E)}, KeyEntry{"KeyV", scan(0x2F)}, KeyEntry{"KeyB", scan(0x30)},
    KeyEntry{"KeyN", scan(0x31)}, KeyEntry{"KeyM", scan(0x32)}, KeyEntry{"Comma", scan(0x33)},
    KeyEntry{"Period", scan(0x34)}, KeyEntry{"Slash", scan(0x35)}, KeyEntry{"ShiftRight", scan(0x36)},
    KeyEntry{"NumpadMultiply", scan(0x37)}, KeyEntry{"AltLeft", scan(0x38)}, KeyEntry{"Space", scan(0x39)},
    KeyEntry{"CapsLock", scan(0x3A)}, KeyEntry{"F1", scan(0x3B)}, KeyEntry{"F2", scan(0x3C)},
    KeyEntry{"F3", scan(0x3D)}, KeyEntry{"F4", scan(0x3E)}, KeyEntry{"F5", scan(0x3F)},
    KeyEntry{"F6", scan(0x40)}, KeyEntry{"F7", scan(0x41)}, KeyEntry{"F8", scan(0x42)},
    KeyEntry{"F9", scan(0x43)}, KeyEntry{"F10", scan(0x44)},
    KeyEntry{"NumLock", {.scan = 0x45, .extended = true, .virtual_key = VK_NUMLOCK}},
    KeyEntry{"ScrollLock", scan(0x46)}, KeyEntry{"Numpad7", scan(0x47)}, KeyEntry{"Numpad8", scan(0x48)},
    KeyEntry{"Numpad9", scan(0x49)}, KeyEntry{"NumpadSubtract", scan(0x4A)}, KeyEntry{"Numpad4", scan(0x4B)},
    KeyEntry{"Numpad5", scan(0x4C)}, KeyEntry{"Numpad6", scan(0x4D)}, KeyEntry{"NumpadAdd", scan(0x4E)},
    KeyEntry{"Numpad1", scan(0x4F)}, KeyEntry{"Numpad2", scan(0x50)}, KeyEntry{"Numpad3", scan(0x51)},
    KeyEntry{"Numpad0", scan(0x52)}, KeyEntry{"NumpadDecimal", scan(0x53)},
    KeyEntry{"IntlBackslash", scan(0x56)}, KeyEntry{"F11", scan(0x57)}, KeyEntry{"F12", scan(0x58)},
    KeyEntry{"NumpadEqual", scan(0x59)}, KeyEntry{"F13", scan(0x64)}, KeyEntry{"F14", scan(0x65)},
    KeyEntry{"F15", scan(0x66)}, KeyEntry{"F16", scan(0x67)}, KeyEntry{"F17", scan(0x68)},
    KeyEntry{"F18", scan(0x69)}, KeyEntry{"F19", scan(0x6A)}, KeyEntry{"F20", scan(0x6B)},
    KeyEntry{"F21", scan(0x6C)}, KeyEntry{"F22", scan(0x6D)}, KeyEntry{"F23", scan(0x6E)},
    KeyEntry{"KanaMode", scan(0x70)}, KeyEntry{"Lang2", scan(0x71)}, KeyEntry{"Lang1", scan(0x72)},
    KeyEntry{"IntlRo", scan(0x73)}, KeyEntry{"F24", scan(0x76)}, KeyEntry{"Convert", scan(0x79)},
    KeyEntry{"NonConvert", scan(0x7B)}, KeyEntry{"IntlYen", scan(0x7D)}, KeyEntry{"NumpadComma", scan(0x7E)},
    KeyEntry{"MediaTrackPrevious", scan(0xE010)}, KeyEntry{"MediaTrackNext", scan(0xE019)},
    KeyEntry{"NumpadEnter", scan(0xE01C)}, KeyEntry{"ControlRight", scan(0xE01D)},
    KeyEntry{"AudioVolumeMute", scan(0xE020)}, KeyEntry{"LaunchApp2", scan(0xE021)},
    KeyEntry{"MediaPlayPause", scan(0xE022)}, KeyEntry{"MediaStop", scan(0xE024)},
    KeyEntry{"AudioVolumeDown", scan(0xE02E)}, KeyEntry{"AudioVolumeUp", scan(0xE030)},
    KeyEntry{"BrowserHome", scan(0xE032)}, KeyEntry{"NumpadDivide", scan(0xE035)},
    KeyEntry{"PrintScreen", scan(0xE037)}, KeyEntry{"AltRight", scan(0xE038)},
    KeyEntry{"Pause", {.scan = 0x45, .virtual_key = VK_PAUSE}},
    KeyEntry{"Home", scan(0xE047)}, KeyEntry{"ArrowUp", scan(0xE048)}, KeyEntry{"PageUp", scan(0xE049)},
    KeyEntry{"ArrowLeft", scan(0xE04B)}, KeyEntry{"ArrowRight", scan(0xE04D)}, KeyEntry{"End", scan(0xE04F)},
    KeyEntry{"ArrowDown", scan(0xE050)}, KeyEntry{"PageDown", scan(0xE051)}, KeyEntry{"Insert", scan(0xE052)},
    KeyEntry{"Delete", scan(0xE053)}, KeyEntry{"MetaLeft", scan(0xE05B)}, KeyEntry{"MetaRight", scan(0xE05C)},
    KeyEntry{"OSLeft", scan(0xE05B)}, KeyEntry{"OSRight", scan(0xE05C)}, KeyEntry{"ContextMenu", scan(0xE05D)},
    KeyEntry{"Power", scan(0xE05E)}, KeyEntry{"Sleep", scan(0xE05F)}, KeyEntry{"WakeUp", scan(0xE063)},
    KeyEntry{"BrowserSearch", scan(0xE065)}, KeyEntry{"BrowserFavorites", scan(0xE066)},
    KeyEntry{"BrowserRefresh", scan(0xE067)}, KeyEntry{"BrowserStop", scan(0xE068)},
    KeyEntry{"BrowserForward", scan(0xE069)}, KeyEntry{"BrowserBack", scan(0xE06A)},
    KeyEntry{"LaunchApp1", scan(0xE06B)}, KeyEntry{"LaunchMail", scan(0xE06C)},
    KeyEntry{"MediaSelect", scan(0xE06D)},
};

INPUT keyboard_input(KeyCode key, bool down) {
    INPUT input{.type = INPUT_KEYBOARD};
    auto& keyboard = input.ki;
    keyboard.wScan = key.scan;
    keyboard.dwFlags = (key.extended ? KEYEVENTF_EXTENDEDKEY : 0) | (down ? 0 : KEYEVENTF_KEYUP);
    if (key.virtual_key != 0) {
        keyboard.wVk = key.virtual_key;
    } else {
        keyboard.dwFlags |= KEYEVENTF_SCANCODE;
    }
    return input;
}

INPUT mouse_input(DWORD flags, LONG dx = 0, LONG dy = 0, DWORD data = 0) {
    INPUT input{.type = INPUT_MOUSE};
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.mouseData = data;
    input.mi.dwFlags = flags;
    return input;
}

INPUT button_input(MouseButton physical, bool down) {
    switch (physical) {
    case MouseButton::Left: return mouse_input(down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP);
    case MouseButton::Right: return mouse_input(down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP);
    case MouseButton::Middle: return mouse_input(down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP);
    case MouseButton::Back: return mouse_input(down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP, 0, 0, XBUTTON1);
    case MouseButton::Forward: return mouse_input(down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP, 0, 0, XBUTTON2);
    }
    return mouse_input(0);
}

InputResult send(std::span<INPUT> inputs) {
    if (inputs.empty()) return InputResult::Ok;
    SetLastError(ERROR_SUCCESS);
    const UINT sent = SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    if (sent == inputs.size()) return InputResult::Ok;
    const DWORD error = GetLastError();

    // UIPI has no dedicated SendInput error; inspect tokens only on failure, at most once every five seconds.
    static thread_local std::optional<std::chrono::steady_clock::time_point> last_warning;
    const auto now = std::chrono::steady_clock::now();
    if (!last_warning || now - *last_warning >= std::chrono::seconds(5)) {
        last_warning = now;
        DWORD foreground_pid{};
        GetWindowThreadProcessId(GetForegroundWindow(), &foreground_pid);
        const auto host = process_integrity(GetCurrentProcessId());
        const auto foreground = process_integrity(foreground_pid);
        pending_diagnostic = InjectionDiagnostic{sent, static_cast<DWORD>(inputs.size()), error,
            GetCurrentProcessId(), foreground_pid, host, foreground};
        log_injection_diagnostic(*pending_diagnostic);
    }
    return InputResult::InjectFailed;
}

bool is_foreground_process(DWORD process_id) {
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) return false;
    DWORD foreground_pid{};
    GetWindowThreadProcessId(foreground, &foreground_pid);
    return foreground_pid == process_id;
}

} // namespace

std::optional<InjectionDiagnostic> take_injection_diagnostic() {
    return std::exchange(pending_diagnostic, std::nullopt);
}

void log_injection_diagnostic(const InjectionDiagnostic& d) {
    const bool higher = d.injector.error == ERROR_SUCCESS && d.foreground.error == ERROR_SUCCESS &&
                        d.foreground.level > d.injector.level;
    spdlog::warn("Remote control blocked: SendInput inserted {}/{} events (Win32 error {}); "
                 "injector PID {}, foreground PID {}, injector integrity RID {} (query error {}), "
                 "foreground integrity RID {} (query error {}). {}",
                 d.inserted, d.requested, d.error, d.injector_pid, d.foreground_pid,
                 d.injector.level, d.injector.error, d.foreground.level, d.foreground.error,
                 higher ? "Higher-integrity foreground process: UIPI is suspected; consider --elevate-input."
                        : "Cause unknown; secure desktop, UIPI or another input restriction may be responsible.");
}

ProcessIntegrity process_integrity(DWORD process_id) {
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (!process) return {.error = GetLastError()};
    HANDLE token{};
    const bool opened = OpenProcessToken(process, TOKEN_QUERY, &token) != FALSE;
    const DWORD open_error = opened ? ERROR_SUCCESS : GetLastError();
    CloseHandle(process);
    if (!opened) return {.error = open_error};

    // A mandatory-label SID has one RID; reserve SECURITY_MAX_SID_SIZE to avoid a second token query.
    struct TokenLabel {
        TOKEN_MANDATORY_LABEL label;
        BYTE sid[SECURITY_MAX_SID_SIZE];
    } info{};
    DWORD size{};
    const bool queried = GetTokenInformation(token, TokenIntegrityLevel, &info, sizeof(info), &size) != FALSE;
    const DWORD query_error = queried ? ERROR_SUCCESS : GetLastError();
    CloseHandle(token);
    if (!queried) return {.error = query_error};
    const PSID sid = info.label.Label.Sid;
    if (!IsValidSid(sid) || *GetSidSubAuthorityCount(sid) == 0) return {.error = ERROR_INVALID_SID};
    return {.level = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1)};
}

std::optional<MouseMode> parse_mouse_mode(std::string_view mode) {
    if (mode == "absolute") return MouseMode::Absolute;
    if (mode == "relative") return MouseMode::Relative;
    return std::nullopt;
}

std::string_view mouse_mode_name(MouseMode mode) {
    return mode == MouseMode::Absolute ? "absolute" : "relative";
}

std::optional<MouseButton> parse_mouse_button(std::string_view button) {
    if (button == "left") return MouseButton::Left;
    if (button == "right") return MouseButton::Right;
    if (button == "middle") return MouseButton::Middle;
    if (button == "back") return MouseButton::Back;
    if (button == "forward") return MouseButton::Forward;
    return std::nullopt;
}

std::optional<KeyCode> key_for_code(std::string_view code) {
    const auto found = std::ranges::find(key_table, code, &KeyEntry::code);
    if (found == key_table.end()) return std::nullopt;
    return found->key;
}

bool supported_key(KeyCode key) {
    return std::ranges::any_of(key_table, [&](const KeyEntry& entry) { return entry.key == key; });
}

ScreenPoint map_to_rect(double x, double y, std::uint32_t video_width, std::uint32_t video_height,
                        const ScreenRect& rect) {
    const auto axis = [](double value, std::uint32_t video, long low, long high) {
        if (high <= low) return low;
        double mapped = static_cast<double>(low);
        if (video > 0 && std::isfinite(value)) {
            mapped += std::floor(value * static_cast<double>(high - low) / static_cast<double>(video));
        }
        return static_cast<long>(std::clamp(mapped, static_cast<double>(low), static_cast<double>(high - 1)));
    };
    return {axis(x, video_width, rect.left, rect.right), axis(y, video_height, rect.top, rect.bottom)};
}

ScreenPoint map_from_rect(ScreenPoint point, std::uint32_t video_width, std::uint32_t video_height,
                          const ScreenRect& rect) {
    // Smallest video coordinate that map_to_rect sends to this pixel, i.e. ceil(offset * video / size), so the two
    // round-trip exactly when the video is cropped smaller than (or equal to) the window.
    const auto axis = [](long value, std::uint32_t video, long low, long high) -> long {
        const long long offset = value - low;
        if (high <= low || video == 0) return static_cast<long>(offset);
        const long long numerator = offset * video;
        const long long size = high - low;
        return static_cast<long>(numerator >= 0 ? (numerator + size - 1) / size : -((-numerator) / size));
    };
    return {axis(point.x, video_width, rect.left, rect.right), axis(point.y, video_height, rect.top, rect.bottom)};
}

bool rect_contains(const ScreenRect& rect, ScreenPoint point) {
    return point.x >= rect.left && point.x < rect.right && point.y >= rect.top && point.y < rect.bottom;
}

ModifierState modifier_state() {
    const auto held = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
    // Toggle states are global in the async key state table, which GetKeyState consults for threads
    // without their own keyboard input (such as the network thread).
    const auto toggled = [](int key) { return (GetKeyState(key) & 0x0001) != 0; };
    return {
        .shift = held(VK_SHIFT),
        .control = held(VK_CONTROL),
        .alt = held(VK_MENU),
        .meta = held(VK_LWIN) || held(VK_RWIN),
        .caps_lock = toggled(VK_CAPITAL),
        .num_lock = toggled(VK_NUMLOCK),
        .scroll_lock = toggled(VK_SCROLL),
    };
}

ScreenPoint normalize_to_virtual_desk(ScreenPoint point, const ScreenRect& virtual_desk) {
    // Aim at the pixel centre so both of Windows' de-normalization roundings land on the intended pixel.
    const auto axis = [](long value, long low, long high) {
        const long long size = std::max(1L, high - low);
        const long long offset = std::clamp<long long>(value - low, 0, size - 1);
        return static_cast<long>(std::min<long long>((offset * 65536 + 32768) / size, 65535));
    };
    return {axis(point.x, virtual_desk.left, virtual_desk.right), axis(point.y, virtual_desk.top, virtual_desk.bottom)};
}

std::optional<std::wstring> text_to_utf16(std::string_view utf8) {
    if (utf8.empty()) return std::wstring{};
    if (utf8.size() > max_text_units * 4) return std::nullopt;
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
                                           nullptr, 0);
    if (length <= 0 || static_cast<std::size_t>(length) > max_text_units) return std::nullopt;
    std::wstring text(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), text.data(),
                        length);
    const bool valid = std::ranges::none_of(text, [](wchar_t unit) {
        return (unit < 0x20 && unit != L'\t' && unit != L'\n' && unit != L'\r') || unit == 0x7F;
    });
    if (!valid) return std::nullopt;
    return text;
}

StreamInput::StreamInput(HWND window, std::shared_ptr<ElevatedInput> elevated)
    : window_(window), elevated_(std::move(elevated)) {
    if (elevated_) remote_stream_ = elevated_->create(window);
    if (window_ != nullptr) GetWindowThreadProcessId(window_, &process_id_);
}

StreamInput::~StreamInput() {
    if (elevated_) elevated_->destroy(remote_stream_);
    else release_all();
}

std::optional<ScreenRect> StreamInput::target_rect() const {
    RECT rect{};
    if (window_ == nullptr) {
        MONITORINFO info{.cbSize = sizeof(MONITORINFO)};
        if (!GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &info)) return std::nullopt;
        rect = info.rcMonitor;
    } else {
        if (!IsWindow(window_)) return std::nullopt;
        // The visible frame (without the invisible resize borders) is what Windows.Graphics.Capture records.
        if (FAILED(DwmGetWindowAttribute(window_, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect))) &&
            !GetWindowRect(window_, &rect)) {
            return std::nullopt;
        }
    }
    return ScreenRect{rect.left, rect.top, rect.right, rect.bottom};
}

std::optional<ScreenPoint> StreamInput::cursor_position(std::uint32_t video_width,
                                                        std::uint32_t video_height) const {
    if (elevated_) {
        ipc::Request request;
        request.operation = ipc::Operation::Cursor;
        request.stream = remote_stream_;
        request.width = video_width;
        request.height = video_height;
        const auto response = elevated_->transact(request);
        return response.present ? std::optional(ScreenPoint{response.x, response.y}) : std::nullopt;
    }
    POINT cursor{};
    const auto rect = target_rect();
    if (!rect || !GetCursorPos(&cursor)) return std::nullopt;
    const ScreenPoint point{cursor.x, cursor.y};
    if (window_ != nullptr && (IsIconic(window_) || !rect_contains(*rect, point))) return std::nullopt;
    return map_from_rect(point, video_width, video_height, *rect);
}

InputResult StreamInput::ensure_focus() {
    if (window_ == nullptr) return InputResult::Ok;
    if (!IsWindow(window_)) return InputResult::TargetGone;
    if (is_foreground_process(process_id_)) return InputResult::Ok;
    if (IsIconic(window_)) ShowWindowAsync(window_, SW_RESTORE);
    if (SetForegroundWindow(window_) && is_foreground_process(process_id_)) return InputResult::Ok;

    // The foreground lock refused us; share the foreground thread's input state for the switch.
    const HWND foreground = GetForegroundWindow();
    const DWORD foreground_thread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const DWORD current_thread = GetCurrentThreadId();
    const bool attached = foreground_thread != 0 && foreground_thread != current_thread &&
                          AttachThreadInput(current_thread, foreground_thread, TRUE);
    BringWindowToTop(window_);
    SetForegroundWindow(window_);
    if (attached) AttachThreadInput(current_thread, foreground_thread, FALSE);
    return is_foreground_process(process_id_) ? InputResult::Ok : InputResult::FocusFailed;
}

InputResult StreamInput::send_mouse_to(ScreenPoint point) {
    std::lock_guard lock(held_mutex_);
    if (cancelled_) return InputResult::InjectFailed;
    const ScreenRect desk{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                          GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                          GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    const auto normalized = normalize_to_virtual_desk(point, desk);
    INPUT input = mouse_input(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK, normalized.x,
                              normalized.y);
    return send(std::span(&input, 1));
}

InputResult StreamInput::move_absolute(double x, double y, std::uint32_t video_width, std::uint32_t video_height) {
    if (elevated_) {
        ipc::Request request;
        request.operation = ipc::Operation::Absolute;
        request.stream = remote_stream_;
        request.x = x; request.y = y;
        request.width = video_width; request.height = video_height;
        return static_cast<InputResult>(elevated_->transact(request).result);
    }
    const auto rect = target_rect();
    if (!rect) return InputResult::TargetGone;
    if (window_ != nullptr && IsIconic(window_)) return InputResult::Ok;
    const auto point = map_to_rect(x, y, video_width, video_height, *rect);
    virtual_cursor_ = {static_cast<double>(point.x), static_cast<double>(point.y)};
    return send_mouse_to(point);
}

InputResult StreamInput::move_relative(double dx, double dy, std::uint32_t video_width, std::uint32_t video_height) {
    if (elevated_) {
        ipc::Request request;
        request.operation = ipc::Operation::Relative;
        request.stream = remote_stream_;
        request.x = dx; request.y = dy;
        request.width = video_width; request.height = video_height;
        return static_cast<InputResult>(elevated_->transact(request).result);
    }
    if (!std::isfinite(dx) || !std::isfinite(dy)) return InputResult::Ok;
    if (window_ == nullptr) {
        std::lock_guard lock(held_mutex_);
        if (cancelled_) return InputResult::InjectFailed;
        // Desktop: genuine relative motion so raw-input applications see deltas.
        residual_x_ += dx;
        residual_y_ += dy;
        const auto step_x = static_cast<LONG>(std::trunc(residual_x_));
        const auto step_y = static_cast<LONG>(std::trunc(residual_y_));
        residual_x_ -= step_x;
        residual_y_ -= step_y;
        if (step_x == 0 && step_y == 0) return InputResult::Ok;
        INPUT input = mouse_input(MOUSEEVENTF_MOVE, step_x, step_y);
        return send(std::span(&input, 1));
    }

    // Window: move the cursor inside the window rectangle, scaled from video to screen pixels.
    const auto rect = target_rect();
    if (!rect) return InputResult::TargetGone;
    if (IsIconic(window_) || rect->right <= rect->left || rect->bottom <= rect->top) return InputResult::Ok;
    POINT cursor{};
    if (!GetCursorPos(&cursor)) return InputResult::InjectFailed;
    if (!virtual_cursor_ || std::lround(virtual_cursor_->first) != cursor.x ||
        std::lround(virtual_cursor_->second) != cursor.y) {
        virtual_cursor_ = {static_cast<double>(cursor.x), static_cast<double>(cursor.y)};
    }
    const double scale_x = video_width ? static_cast<double>(rect->right - rect->left) / video_width : 1.0;
    const double scale_y = video_height ? static_cast<double>(rect->bottom - rect->top) / video_height : 1.0;
    auto& [cursor_x, cursor_y] = *virtual_cursor_;
    cursor_x = std::clamp(cursor_x + dx * scale_x, static_cast<double>(rect->left),
                          static_cast<double>(rect->right - 1));
    cursor_y = std::clamp(cursor_y + dy * scale_y, static_cast<double>(rect->top),
                          static_cast<double>(rect->bottom - 1));
    return send_mouse_to({std::lround(cursor_x), std::lround(cursor_y)});
}

InputResult StreamInput::button(MouseButton logical, bool down) {
    if (elevated_) {
        ipc::Request request;
        request.operation = ipc::Operation::Button;
        request.stream = remote_stream_;
        request.first = static_cast<int>(logical);
        request.flags = down;
        return static_cast<InputResult>(elevated_->transact(request).result);
    }
    if (down) {
        if (const auto focus = ensure_focus(); focus != InputResult::Ok) return focus;
    }
    std::lock_guard lock(held_mutex_);
    if (cancelled_) return InputResult::InjectFailed;
    auto& held = pressed_buttons_[static_cast<std::size_t>(logical)];
    if (!down) {
        if (!held) return InputResult::Ok;
        INPUT input = button_input(*held, false);
        held.reset();
        return send(std::span(&input, 1));
    }
    MouseButton physical = logical;
    if (GetSystemMetrics(SM_SWAPBUTTON)) {
        if (logical == MouseButton::Left) physical = MouseButton::Right;
        if (logical == MouseButton::Right) physical = MouseButton::Left;
    }
    INPUT input = button_input(physical, true);
    const auto result = send(std::span(&input, 1));
    if (result == InputResult::Ok) held = physical;
    return result;
}

InputResult StreamInput::wheel(int dx, int dy) {
    if (elevated_) {
        ipc::Request request;
        request.operation = ipc::Operation::Wheel;
        request.stream = remote_stream_;
        request.first = dx; request.second = dy;
        return static_cast<InputResult>(elevated_->transact(request).result);
    }
    std::lock_guard lock(held_mutex_);
    if (cancelled_) return InputResult::InjectFailed;
    std::array<INPUT, 2> inputs{};
    std::size_t count = 0;
    // Protocol dy > 0 scrolls down (DOM deltaY); Windows uses positive for scrolling up.
    if (dy != 0) inputs[count++] = mouse_input(MOUSEEVENTF_WHEEL, 0, 0, static_cast<DWORD>(-dy));
    if (dx != 0) inputs[count++] = mouse_input(MOUSEEVENTF_HWHEEL, 0, 0, static_cast<DWORD>(dx));
    return send(std::span(inputs.data(), count));
}

InputResult StreamInput::key(KeyCode key, bool down) {
    if (elevated_) {
        ipc::Request request;
        request.operation = ipc::Operation::Key;
        request.stream = remote_stream_;
        request.first = key.scan;
        request.second = key.virtual_key;
        request.flags = (down ? 1u : 0u) | (key.extended ? 2u : 0u);
        return static_cast<InputResult>(elevated_->transact(request).result);
    }
    if (down) {
        if (const auto focus = ensure_focus(); focus != InputResult::Ok) return focus;
    }
    std::lock_guard lock(held_mutex_);
    if (cancelled_) return InputResult::InjectFailed;
    INPUT input = keyboard_input(key, down);
    const auto result = send(std::span(&input, 1));
    if (down && result == InputResult::Ok) pressed_keys_.insert(key);
    if (!down) pressed_keys_.erase(key);
    return result;
}

InputResult StreamInput::text(std::wstring_view text) {
    if (elevated_) {
        if (text.size() > max_text_units) return InputResult::InjectFailed;
        ipc::Request request;
        request.operation = ipc::Operation::Text;
        request.stream = remote_stream_;
        request.length = static_cast<std::uint32_t>(text.size());
        std::ranges::copy(text, request.text);
        return static_cast<InputResult>(elevated_->transact(request).result);
    }
    if (text.empty()) return InputResult::Ok;
    if (const auto focus = ensure_focus(); focus != InputResult::Ok) return focus;
    std::lock_guard lock(held_mutex_);
    if (cancelled_) return InputResult::InjectFailed;
    std::vector<INPUT> inputs;
    inputs.reserve(text.size() * 2);
    for (std::size_t index = 0; index < text.size(); ++index) {
        const wchar_t unit = text[index];
        // Line breaks and tabs are keys, not characters, for most applications.
        if (unit == L'\r' || unit == L'\n' || unit == L'\t') {
            if (unit == L'\r' && index + 1 < text.size() && text[index + 1] == L'\n') continue;
            const auto key = scan(unit == L'\t' ? 0x0F : 0x1C);
            inputs.push_back(keyboard_input(key, true));
            inputs.push_back(keyboard_input(key, false));
            continue;
        }
        INPUT input{.type = INPUT_KEYBOARD};
        input.ki.wScan = unit;
        input.ki.dwFlags = KEYEVENTF_UNICODE;
        inputs.push_back(input);
        input.ki.dwFlags |= KEYEVENTF_KEYUP;
        inputs.push_back(input);
    }
    return send(inputs);
}

void StreamInput::release_all() {
    if (elevated_) {
        ipc::Request request;
        request.operation = ipc::Operation::Release;
        request.stream = remote_stream_;
        elevated_->transact(request);
        return;
    }
    std::lock_guard lock(held_mutex_);
    std::vector<INPUT> inputs;
    for (const auto& key : pressed_keys_) inputs.push_back(keyboard_input(key, false));
    for (auto& held : pressed_buttons_) {
        if (held) inputs.push_back(button_input(*held, false));
        held.reset();
    }
    pressed_keys_.clear();
    if (!inputs.empty()) send(inputs);
}

void StreamInput::cancel() {
    {
        std::lock_guard lock(held_mutex_);
        cancelled_ = true;
    }
    release_all();
}

ModifierState StreamInput::modifiers() const {
    if (!elevated_) return modifier_state();
    ipc::Request request;
    request.operation = ipc::Operation::Modifiers;
    request.stream = remote_stream_;
    const auto bits = elevated_->transact(request).modifiers;
    return {bool(bits & 1), bool(bits & 2), bool(bits & 4), bool(bits & 8),
            bool(bits & 16), bool(bits & 32), bool(bits & 64)};
}

} // namespace beamit::host::input
