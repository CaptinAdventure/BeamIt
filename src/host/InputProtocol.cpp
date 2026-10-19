#include "host/InputProtocol.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace beamit::host::input::ipc {

bool valid(const Request& r) {
    if (r.revision != version || r.operation > Operation::Shutdown ||
        r.length > max_text_units || r.width > 65536 || r.height > 65536 ||
        !std::isfinite(r.x) || !std::isfinite(r.y) ||
        std::abs(r.x) > 1'000'000 || std::abs(r.y) > 1'000'000) return false;
    Request canonical;
    canonical.operation = r.operation;
    canonical.stream = r.stream;
    if (r.operation != Operation::Ping && r.operation != Operation::Shutdown && r.stream == 0) return false;
    switch (r.operation) {
    case Operation::Ping:
    case Operation::Shutdown:
        if (r.stream != 0) return false;
        break;
    case Operation::Create:
        if (r.window > std::numeric_limits<std::uintptr_t>::max()) return false;
        canonical.window = r.window;
        break;
    case Operation::Absolute:
    case Operation::Relative:
        canonical.x = r.x; canonical.y = r.y;
        [[fallthrough]];
    case Operation::Cursor:
        canonical.width = r.width; canonical.height = r.height;
        break;
    case Operation::Button:
        if (r.first < 0 || r.first > static_cast<int>(MouseButton::Forward) || r.flags > 1) return false;
        canonical.first = r.first; canonical.flags = r.flags;
        break;
    case Operation::Key:
        if (r.flags > 3 || r.first < 0 || r.first > 0x7f || r.second < 0 || r.second > 255 ||
            !supported_key({static_cast<std::uint16_t>(r.first), (r.flags & 2) != 0,
                            static_cast<std::uint16_t>(r.second)})) return false;
        canonical.first = r.first; canonical.second = r.second; canonical.flags = r.flags;
        break;
    case Operation::Wheel:
        if (r.first < -12000 || r.first > 12000 || r.second < -12000 || r.second > 12000) return false;
        canonical.first = r.first; canonical.second = r.second;
        break;
    case Operation::Text: {
        const std::wstring_view text(r.text, r.length);
        if (std::ranges::any_of(text, [](wchar_t c) {
            return (c < 0x20 && c != L'\t' && c != L'\r' && c != L'\n') || c == 0x7f;
        })) return false;
        if (!text.empty() && WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
            static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr) <= 0) return false;
        canonical.length = r.length;
        std::ranges::copy(text, canonical.text);
        break;
    }
    default: break;
    }
    return std::memcmp(&canonical, &r, sizeof(r)) == 0;
}

bool valid(const Response& r) {
    return r.revision == version && r.result <= static_cast<unsigned>(InputResult::TargetGone) &&
           r.present <= 1 && r.modifiers <= 127 && r.diagnostic_present <= 1 &&
           (!r.diagnostic_present || (r.diagnostic.inserted <= r.diagnostic.requested &&
             r.diagnostic.requested <= max_text_units * 2 && r.diagnostic.injector_pid != 0));
}

Response Session::dispatch(const Request& request) {
    if (!valid(request)) throw std::runtime_error("Elevated input: invalid IPC request");
    if (request.operation == Operation::Shutdown) {
        cancel();
        return {};
    }
    (void)take_injection_diagnostic();
    Response response;
    const auto complete = [&] {
        if (const auto diagnostic = take_injection_diagnostic()) {
            response.diagnostic_present = 1;
            response.diagnostic = *diagnostic;
        }
        return response;
    };
    std::shared_ptr<StreamInput> stream;
    {
        std::lock_guard lock(mutex_);
        if (cancelled_) throw std::runtime_error("Elevated input: session revoked");
        if (request.operation == Operation::Create) {
            if (streams_.size() >= max_streams || streams_.contains(request.stream))
                throw std::runtime_error("Elevated input: invalid stream creation");
            const auto window = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(request.window));
            streams_.emplace(request.stream, std::make_shared<StreamInput>(window));
        } else if (request.operation == Operation::Destroy) {
            if (!streams_.erase(request.stream)) throw std::runtime_error("Elevated input: unknown stream");
        } else if (request.operation != Operation::Ping) {
            const auto found = streams_.find(request.stream);
            if (found == streams_.end()) throw std::runtime_error("Elevated input: unknown stream");
            stream = found->second;
        }
    }
    if (!stream) return complete();
    InputResult result = InputResult::Ok;
    switch (request.operation) {
    case Operation::Absolute:
        result = stream->move_absolute(request.x, request.y, request.width, request.height); break;
    case Operation::Relative:
        result = stream->move_relative(request.x, request.y, request.width, request.height); break;
    case Operation::Button:
        result = stream->button(static_cast<MouseButton>(request.first), request.flags != 0); break;
    case Operation::Wheel: result = stream->wheel(request.first, request.second); break;
    case Operation::Key:
        result = stream->key({static_cast<std::uint16_t>(request.first), (request.flags & 2) != 0,
            static_cast<std::uint16_t>(request.second)}, (request.flags & 1) != 0); break;
    case Operation::Text: result = stream->text({request.text, request.length}); break;
    case Operation::Release: stream->release_all(); break;
    case Operation::Cursor:
        if (const auto point = stream->cursor_position(request.width, request.height)) {
            response.present = 1;
            response.x = point->x;
            response.y = point->y;
        }
        break;
    case Operation::Modifiers: {
        const auto m = modifier_state();
        response.modifiers = (m.shift ? 1u : 0u) | (m.control ? 2u : 0u) | (m.alt ? 4u : 0u) |
            (m.meta ? 8u : 0u) | (m.caps_lock ? 16u : 0u) | (m.num_lock ? 32u : 0u) |
            (m.scroll_lock ? 64u : 0u);
        break;
    }
    default: throw std::runtime_error("Elevated input: invalid stream operation");
    }
    response.result = static_cast<std::uint32_t>(result);
    return complete();
}

void Session::cancel() {
    std::lock_guard lock(mutex_);
    cancelled_ = true;
    for (auto& [id, stream] : streams_) stream->cancel();
}

} // namespace beamit::host::input::ipc
