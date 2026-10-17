// Desktop features: window list and events, icons, clipboard and taskbar attention (PROTOCOL.md §5.2, §5.10-5.13).
#include "host/wss/ControlSession.hpp"

#include "ApplicationWindows.hpp"
#include "Utf8.hpp"
#include "host/WindowIcon.hpp"
#include "net/Crypto.hpp"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <utility>

namespace beamit::host::wss {
namespace {

constexpr std::array<std::pair<ClipboardFormat, std::string_view>, 3> clipboard_format_names{{
    {clipboard_text, "text"}, {clipboard_html, "html"}, {clipboard_image, "image"}}};

std::string_view clipboard_format_name(ClipboardFormat format) {
    for (const auto& [value, name] : clipboard_format_names) {
        if (value == format) return name;
    }
    return "unknown";
}

json::array clipboard_format_list(unsigned formats) {
    json::array list;
    for (const auto& [value, name] : clipboard_format_names) {
        if (formats & value) list.emplace_back(name);
    }
    return list;
}

unsigned parse_clipboard_formats(const json::object& message) {
    const auto* field = message.if_contains("formats");
    if (!field) return clipboard_all;
    if (!field->is_array() || field->as_array().empty()) {
        throw ProtocolError{"invalid_request", "formats must be a non-empty array"};
    }
    unsigned formats = 0;
    for (const auto& item : field->as_array()) {
        const auto found = std::ranges::find_if(clipboard_format_names, [&](const auto& entry) {
            return item.is_string() && item.as_string() == entry.second;
        });
        if (found == clipboard_format_names.end()) {
            throw ProtocolError{"invalid_request", "formats may contain only text, html and image"};
        }
        formats |= found->first;
    }
    return formats;
}

json::object image_json(std::span<const std::byte> png, std::uint32_t width, std::uint32_t height) {
    return {{"mime_type", "image/png"}, {"width", width}, {"height", height},
            {"data", beamit::net::base64_encode(png)}};
}

json::object window_json(const WindowState& window) {
    json::object object{
        {"hwnd", hwnd_text(window.hwnd)},
        {"pid", window.pid},
        {"image_name", window.image_name},
        {"title", window.title},
        {"role", window_role_name(window.role)},
        {"rect", {{"x", window.rect.x}, {"y", window.rect.y}, {"width", window.rect.width},
                  {"height", window.rect.height}}},
        {"state", window_show_state_name(window.show)},
    };
    if (window.owner) object["owner"] = hwnd_text(window.owner);
    return object;
}

} // namespace

// --- Windows -------------------------------------------------------------------------------------------------------

void ControlSession::list_windows(const json::object&, const RequestId& id) {
    json::array windows;
    for (const auto& window : application_windows()) {
        windows.push_back({{"hwnd", hwnd_text(window.hwnd)}, {"pid", window.pid},
                           {"title", to_utf8(window.title)}, {"primary", window.primary_candidate}});
    }
    reply(id, {{"type", "windows"}, {"windows", std::move(windows)}});
}

void ControlSession::send_window_event(const WindowEvent& event) {
    static constexpr std::array<std::string_view, 3> types{"window_opened", "window_changed", "window_closed"};
    send({{"type", types[static_cast<std::size_t>(event.kind)]}, {"window", window_json(event.window)}});
}

void ControlSession::get_window_icon(const json::object& message, const RequestId& id) {
    const HWND window = required_hwnd(message, "hwnd");
    const auto size = static_cast<std::uint32_t>(
        optional_integer(message, "size", 16, max_window_icon_size).value_or(max_window_icon_size));
    if (!IsWindow(window) || GetAncestor(window, GA_ROOT) != window) {
        throw ProtocolError{"not_found", "hwnd is not an existing top-level window"};
    }
    asio::post(server_.icon_worker, [send_reply = deferred_reply(id), window, size] {
        const auto icon = window_icon(window, size);
        if (icon) {
            json::object reply{{"type", "window_icon"}, {"hwnd", hwnd_text(window)}, {"source", icon->source}};
            const auto image = image_json(icon->png, icon->width, icon->height);
            reply.insert(image.begin(), image.end());
            send_reply(std::move(reply));
        } else if (IsWindow(window)) {
            send_reply({{"type", "error"}, {"code", "icon_unavailable"},
                        {"message", "the window and its executable have no usable icon"}});
        } else {
            send_reply({{"type", "error"}, {"code", "not_found"}, {"message", "the window was closed"}});
        }
    });
}

// --- Clipboard -----------------------------------------------------------------------------------------------------

void ControlSession::get_clipboard(const json::object& message, const RequestId& id) {
    const unsigned formats = parse_clipboard_formats(message);
    if (!server_.clipboard) throw ProtocolError{"clipboard_unavailable", "clipboard access is unavailable"};
    // Reading and PNG conversion happen on the clipboard thread; the reply is serialized there too so that large
    // payloads do not stall the network thread.
    server_.clipboard->read(formats, protocol::max_clipboard_bytes,
                            [send_reply = deferred_reply(id)](ClipboardReadResult result) {
        if (!result.content) {
            send_reply({{"type", "error"}, {"code", "clipboard_unavailable"}, {"message", result.error}});
            return;
        }
        auto& content = *result.content;
        json::object reply{{"type", "clipboard"}, {"sequence", content.sequence}};
        if (content.text) reply["text"] = *content.text;
        if (content.html) reply["html"] = *content.html;
        if (content.image) reply["image"] = image_json(content.image->png, content.image->width, content.image->height);
        json::array omitted;
        for (const auto& omission : content.omitted) {
            omitted.push_back({{"format", clipboard_format_name(omission.format)}, {"reason", omission.reason}});
        }
        reply["omitted"] = std::move(omitted);
        send_reply(std::move(reply));
    });
}

void ControlSession::set_clipboard(const json::object& message, const RequestId& id) {
    ClipboardWrite content;
    std::size_t total{};
    const auto string_field = [&](std::string_view name) -> std::optional<std::string> {
        if (!message.contains(name)) return std::nullopt;
        const auto value = required_string(message, name);
        total += value.size();
        return std::string(value);
    };
    content.text = string_field("text");
    content.html = string_field("html");
    if (const auto* field = message.if_contains("image")) {
        const auto* data = field->is_object() ? field->as_object().if_contains("data") : nullptr;
        const auto* mime = field->is_object() ? field->as_object().if_contains("mime_type") : nullptr;
        if (!data || !data->is_string() || (mime && !(mime->is_string() && mime->as_string() == "image/png"))) {
            throw ProtocolError{"invalid_request", "image must be {\"mime_type\": \"image/png\", \"data\": base64}"};
        }
        total += data->as_string().size();
        content.image = std::string(data->as_string());
    }
    if (const auto* field = message.if_contains("sensitive")) {
        if (!field->is_bool()) throw ProtocolError{"invalid_request", "sensitive must be a boolean"};
        content.sensitive = field->as_bool();
    }
    if (!content.text && !content.html && !content.image) {
        throw ProtocolError{"invalid_request", "set_clipboard needs text, html or image"};
    }
    if (total > protocol::max_clipboard_bytes) throw ProtocolError{"invalid_request", "clipboard content exceeds 10 MiB"};
    if (!server_.clipboard) throw ProtocolError{"clipboard_unavailable", "clipboard access is unavailable"};
    server_.clipboard->write(std::move(content), [send_reply = deferred_reply(id)](ClipboardWriteResult result) {
        if (result.error_code.empty()) {
            send_reply({{"type", "clipboard_set"}, {"sequence", result.sequence}});
        } else {
            send_reply({{"type", "error"}, {"code", result.error_code}, {"message", result.error}});
        }
    });
}

void ControlSession::on_clipboard_changed(const ClipboardChange& change) {
    send({{"type", "clipboard_changed"}, {"sequence", change.sequence},
          {"formats", clipboard_format_list(change.formats)}, {"excluded", change.excluded}});
}

// --- Taskbar attention ---------------------------------------------------------------------------------------------

void ControlSession::on_attention(const AttentionEvent& event) {
    for (const auto& [id, entry] : streams_) {
        if (entry->ready) send_attention(*entry, event);
    }
}

void ControlSession::send_attention(const StreamEntry& entry, const AttentionEvent& event) {
    if (entry.pid != 0 && entry.pid != event.pid) return;
    json::object window{{"hwnd", hwnd_text(event.window)}, {"pid", event.pid}};
    if (IsWindow(event.window)) window["title"] = window_title(event.window);
    send({{"type", "attention"}, {"stream_id", entry.id}, {"state", event.requested ? "set" : "clear"},
          {"window", std::move(window)}});
}

} // namespace beamit::host::wss
