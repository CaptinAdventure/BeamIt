#include "host/wss/HttpFrontend.hpp"

#include "HtmlAssets.hpp"
#include "host/wss/ControlSession.hpp"
#include "net/Crypto.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <span>

namespace beamit::host::wss {
namespace {

using namespace std::chrono_literals;

void configure_websocket(WebSocket& ws, std::size_t max_message) {
    ws.set_option(websocket::stream_base::timeout{
        .handshake_timeout = 10s, .idle_timeout = 30s, .keep_alive_pings = true});
    ws.set_option(websocket::stream_base::decorator([](websocket::response_type& response) {
        response.set(http::field::server, "BeamIt");
    }));
    ws.read_message_max(max_message);
    ws.auto_fragment(false);
}

// CSP source for an inline block: 'sha256-<standard padded base64>' of the exact text.
std::string csp_hash(std::string_view text) {
    auto encoded = beamit::net::base64url_encode(beamit::net::sha256(std::as_bytes(std::span(text))));
    std::ranges::replace(encoded, '-', '+');
    std::ranges::replace(encoded, '_', '/');
    encoded.append((4 - encoded.size() % 4) % 4, '=');
    return "'sha256-" + encoded + "'";
}

// Allows exactly the embedded page's inline <script>/<style> blocks and style="" attributes, nothing else inline.
std::string make_content_security_policy(std::string_view page) {
    std::string scripts;
    std::string styles;
    std::string style_attributes;
    const auto collect = [&](std::string_view open, std::string_view close, std::string& out) {
        for (std::size_t at = page.find(open); at != std::string_view::npos; at = page.find(open, at + 1)) {
            const auto tag_end = page.find('>', at);
            const auto end = page.find(close, tag_end);
            if (tag_end == std::string_view::npos || end == std::string_view::npos) break;
            if (page.substr(at, tag_end - at).find(" src=") != std::string_view::npos) continue;
            out += ' ' + csp_hash(page.substr(tag_end + 1, end - tag_end - 1));
        }
    };
    collect("<script", "</script>", scripts);
    collect("<style", "</style>", styles);
    for (std::size_t at = page.find(" style=\""); at != std::string_view::npos; at = page.find(" style=\"", at + 1)) {
        const auto begin = at + 8;
        const auto end = page.find('"', begin);
        if (end == std::string_view::npos) break;
        style_attributes += ' ' + csp_hash(page.substr(begin, end - begin));
    }
    std::string policy = "default-src 'none'; script-src 'self'" + scripts + "; style-src 'self'" + styles;
    if (!style_attributes.empty()) policy += " 'unsafe-hashes'" + style_attributes;
    policy += "; connect-src 'self'; img-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'";
    return policy;
}

const std::string& content_security_policy() {
    static const std::string policy = make_content_security_policy(html_assets::index);
    return policy;
}

http::response<http::string_body> make_response(http::status status, unsigned version, bool keep_alive,
                                                std::string_view content_type, std::string body) {
    http::response<http::string_body> response{status, version};
    response.set(http::field::server, "BeamIt");
    response.set(http::field::content_type, content_type);
    response.set(http::field::cache_control, "no-store");
    response.set("X-Content-Type-Options", "nosniff");
    response.set("Referrer-Policy", "no-referrer");
    response.set("Content-Security-Policy", content_security_policy());
    response.keep_alive(keep_alive);
    response.body() = std::move(body);
    response.prepare_payload();
    return response;
}

awaitable<void> write_status(WebSocket& ws, const http::request<http::empty_body>& request, http::status status) {
    auto response = make_response(status, request.version(), false, "text/plain; charset=utf-8",
                                  std::string(http::obsolete_reason(status)) + "\n");
    co_await http::async_write(ws.next_layer(), response, use_awaitable);
}

// Host header values that may be echoed into stream URLs.
bool valid_authority(std::string_view authority) {
    if (authority.empty() || authority.size() > 255) return false;
    return std::ranges::all_of(authority, [](char character) {
        return std::isalnum(static_cast<unsigned char>(character)) || character == '.' || character == '-' ||
               character == ':' || character == '[' || character == ']';
    });
}

awaitable<void> upgrade(Server& server, std::shared_ptr<WebSocket> ws, http::request<http::empty_body> request,
                        std::string remote) {
    const std::string target(request.target());
    const std::string host(request[http::field::host]);
    const std::string authority = valid_authority(host) ? host : fmt::format("localhost:{}", server.options.port);
    if (const auto origin = request[http::field::origin]; !origin.empty() && origin != "https://" + host) {
        spdlog::warn("Rejected WebSocket from {} with foreign origin {}", remote, std::string(origin));
        co_await write_status(*ws, request, http::status::forbidden);
        co_return;
    }
    if (target == "/control") {
        configure_websocket(*ws, protocol::max_signaling_message);
        co_await ws->async_accept(request, use_awaitable);
        spdlog::info("Control socket opened from {}", remote);
        co_await ControlSession::run(std::make_shared<ControlSession>(server, ws, authority, remote));
        co_return;
    }
    constexpr std::string_view audio_prefix = "/audio/";
    const auto audio_query = target.find("?token=");
    if (target.starts_with(audio_prefix) && audio_query != std::string::npos) {
        const auto audio_id = std::string_view(target).substr(audio_prefix.size(), audio_query - audio_prefix.size());
        const auto token = std::string_view(target).substr(audio_query + 7);
        auto session = server.active.lock();
        auto entry = session ? session->claim_audio(audio_id, token) : nullptr;
        if (!entry) {
            spdlog::warn("Rejected audio socket from {}: invalid, used or expired token", remote);
            co_await write_status(*ws, request, http::status::forbidden);
            co_return;
        }
        configure_websocket(*ws, protocol::max_audio_message);
        ws->binary(true);
        co_await ws->async_accept(request, use_awaitable);
        if (!session->has_audio(entry)) {
            co_await ws->async_close({websocket::close_code::normal, "audio stopped"}, use_awaitable);
            co_return;
        }
        session->attach_audio(entry, ws);
        co_return;
    }
    constexpr std::string_view prefix = "/streams/";
    const auto query = target.find("?token=");
    if (target.starts_with(prefix) && query != std::string::npos) {
        const std::string_view stream_id = std::string_view(target).substr(prefix.size(), query - prefix.size());
        const std::string_view token = std::string_view(target).substr(query + 7);
        auto session = server.active.lock();
        auto entry = session ? session->claim(stream_id, token) : nullptr;
        if (!entry) {
            spdlog::warn("Rejected stream socket from {}: invalid, used or expired token", remote);
            co_await write_status(*ws, request, http::status::forbidden);
            co_return;
        }
        configure_websocket(*ws, 1024);
        ws->binary(true);
        co_await ws->async_accept(request, use_awaitable);
        if (!session->has_stream(entry->id)) {
            co_await ws->async_close({websocket::close_code::normal, "stream stopped"}, use_awaitable);
            co_return;
        }
        session->attach(entry, ws);
        co_return;
    }
    co_await write_status(*ws, request, http::status::not_found);
}

http::response<http::string_body> static_response(const http::request<http::empty_body>& request) {
    const bool head = request.method() == http::verb::head;
    http::response<http::string_body> response;
    if (request.method() != http::verb::get && !head) {
        response = make_response(http::status::method_not_allowed, request.version(), false,
                                 "text/plain; charset=utf-8", "Method Not Allowed\n");
    } else if (request.target() == "/") {
        response = make_response(http::status::ok, request.version(), request.keep_alive(),
                                 "text/html; charset=utf-8", std::string(html_assets::index));
    } else if (request.target() == "/beamit.js") {
        response = make_response(http::status::ok, request.version(), request.keep_alive(),
                                 "text/javascript; charset=utf-8", std::string(html_assets::library));
    } else {
        response = make_response(http::status::not_found, request.version(), request.keep_alive(),
                                 "text/plain; charset=utf-8", "Not Found\n");
    }
    if (head) {
        const auto length = response.body().size();
        response.body().clear();
        response.content_length(length);
    }
    return response;
}

awaitable<void> serve_connection(Server& server, tcp::socket socket) {
    beast::error_code endpoint_error;
    const auto endpoint = socket.remote_endpoint(endpoint_error);
    const std::string remote = endpoint_error ? "unknown" : fmt::format("{}:{}", endpoint.address().to_string(),
                                                                        endpoint.port());
    socket.set_option(tcp::no_delay(true));
    auto ws = std::make_shared<WebSocket>(std::move(socket), server.tls);
    asio::steady_timer deadline(ws->get_executor());
    const auto arm = [&](Clock::duration timeout) {
        deadline.expires_after(timeout);
        deadline.async_wait([weak = std::weak_ptr(ws)](beast::error_code error) {
            auto stream = weak.lock();
            if (error || !stream) return;
            beast::error_code ignored;
            beast::get_lowest_layer(*stream).close(ignored);
        });
    };
    try {
        arm(10s);
        co_await ws->next_layer().async_handshake(wintls::handshake_type::server, use_awaitable);
        beast::flat_buffer buffer;
        while (true) {
            arm(30s);
            http::request_parser<http::empty_body> parser;
            parser.header_limit(8 * 1024);
            co_await http::async_read(ws->next_layer(), buffer, parser, use_awaitable);
            deadline.cancel();
            auto request = parser.release();
            if (websocket::is_upgrade(request)) {
                co_await upgrade(server, ws, std::move(request), remote);
                co_return;
            }
            auto response = static_response(request);
            co_await http::async_write(ws->next_layer(), response, use_awaitable);
            if (!response.keep_alive()) break;
        }
        deadline.cancel();
        co_await ws->next_layer().async_shutdown(use_awaitable);
    } catch (const std::exception& error) {
        spdlog::debug("Connection {} ended: {}", remote, error.what());
    }
}

} // namespace

awaitable<void> accept_loop(Server& server, tcp::acceptor& acceptor) {
    while (acceptor.is_open()) {
        auto [error, socket] = co_await acceptor.async_accept(asio::as_tuple(use_awaitable));
        if (error) {
            if (error == asio::error::operation_aborted) co_return;
            spdlog::warn("Accept failed: {}", error.message());
            continue;
        }
        asio::co_spawn(acceptor.get_executor(), serve_connection(server, std::move(socket)), asio::detached);
    }
}

} // namespace beamit::host::wss
