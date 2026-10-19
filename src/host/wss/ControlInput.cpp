// Remote keyboard and mouse input (PROTOCOL.md §5.9).
#include "host/wss/ControlSession.hpp"

#include <spdlog/fmt/fmt.h>

namespace beamit::host::wss {

input::MouseMode ControlSession::mouse_mode_field(const json::value& value) {
    const auto mode = value.is_string() ? input::parse_mouse_mode(value.as_string()) : std::nullopt;
    if (!mode) throw ProtocolError{"invalid_request", "mouse_mode must be absolute or relative"};
    return *mode;
}

// Success of injected input is silent; failures are reported by report_input_result.
void ControlSession::handle_input(const json::object& message, const RequestId& id) {
    const std::string_view type = message.at("type").as_string();
    const auto entry = find_stream(message);
    auto& state = *entry->input;
    if (type == "set_mouse_mode") {
        state.mouse_mode = mouse_mode_field(message.contains("mode") ? message.at("mode") : json::value{});
        reply(id, {{"type", "mouse_mode"}, {"stream_id", entry->id},
                   {"mode", input::mouse_mode_name(state.mouse_mode)}});
        return;
    }
    if (type == "get_input_state") {
        if (!entry->ready) throw ProtocolError{"not_ready", "the stream has not been offered yet"};
        const auto position =
            state.cursor_position(entry->video.width, entry->video.height).value_or(input::ScreenPoint{-1, -1});
        const auto modifiers = state.modifiers();
        reply(id, {
            {"type", "input_state"},
            {"stream_id", entry->id},
            {"x", position.x},
            {"y", position.y},
            {"modifiers", {{"shift", modifiers.shift}, {"control", modifiers.control}, {"alt", modifiers.alt},
                           {"meta", modifiers.meta}, {"caps_lock", modifiers.caps_lock},
                           {"num_lock", modifiers.num_lock}, {"scroll_lock", modifiers.scroll_lock}}},
            {"mouse_mode", input::mouse_mode_name(state.mouse_mode)},
        });
        return;
    }
    if (type == "release_all") {
        state.release_all();
        return;
    }
    if (!entry->socket) throw ProtocolError{"not_ready", "open the stream socket before sending input"};

    auto result = input::InputResult::Ok;
    if (type == "mouse_move") {
        const auto absolute = state.mouse_mode == input::MouseMode::Absolute;
        const auto first = optional_number(message, absolute ? "x" : "dx");
        const auto second = optional_number(message, absolute ? "y" : "dy");
        if (!first || !second || message.contains(absolute ? "dx" : "x") || message.contains(absolute ? "dy" : "y")) {
            throw ProtocolError{"invalid_request", absolute ? "absolute mouse_move needs x and y only"
                                                            : "relative mouse_move needs dx and dy only"};
        }
        result = absolute ? state.move_absolute(*first, *second, entry->video.width, entry->video.height)
                          : state.move_relative(*first, *second, entry->video.width, entry->video.height);
    } else if (type == "mouse_button") {
        const auto button = input::parse_mouse_button(required_string(message, "button"));
        if (!button) throw ProtocolError{"invalid_request", "button must be left, right, middle, back or forward"};
        result = state.button(*button, required_bool(message, "down"));
    } else if (type == "mouse_wheel") {
        const auto units = [&](std::string_view name) {
            const auto limit = protocol::max_wheel_units;
            return static_cast<int>(optional_integer(message, name, -limit, limit).value_or(0));
        };
        result = state.wheel(units("dx"), units("dy"));
    } else if (type == "key") {
        const auto code = required_string(message, "code");
        const auto key = input::key_for_code(code);
        if (!key) throw ProtocolError{"invalid_request", fmt::format("unsupported key code '{}'", code)};
        result = state.key(*key, required_bool(message, "down"));
    } else {
        const auto text = input::text_to_utf16(required_string(message, "text"));
        if (!text) {
            throw ProtocolError{"invalid_request",
                                "text must be valid UTF-8, at most 1024 UTF-16 units, without control characters"};
        }
        result = state.text(*text);
    }
    report_input_result(*entry, result, id);
}

// Reports focus/injection failures once until the next success so a stream of moves cannot flood the socket.
void ControlSession::report_input_result(StreamEntry& entry, input::InputResult result, const RequestId& id) {
    switch (result) {
    case input::InputResult::Ok:
        entry.focus_error_sent = false;
        entry.input_error_sent = false;
        return;
    case input::InputResult::FocusFailed:
        if (std::exchange(entry.focus_error_sent, true)) return;
        send_error(id, "focus_failed", "the target window could not be brought to the foreground", entry.id);
        return;
    case input::InputResult::InjectFailed:
    case input::InputResult::TargetGone:
        if (std::exchange(entry.input_error_sent, true)) return;
        send_error(id, "input_failed",
                   result == input::InputResult::TargetGone
                       ? "the target window no longer exists"
                       : "Windows rejected the input (secure desktop or elevated target?)",
                   entry.id);
        return;
    }
}

} // namespace beamit::host::wss
