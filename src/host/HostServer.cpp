// Process-level host setup: TLS listener, desktop monitors and the network thread. The protocol itself lives in
// src/host/wss (HttpFrontend: HTTP/upgrades, ControlSession*: signaling, StreamSocket: video delivery).
#include "host/wss/ControlSession.hpp"
#include "host/wss/HttpFrontend.hpp"

#include "FrameTimer.hpp"

#include <spdlog/spdlog.h>

#include <csignal>

namespace beamit::host {

using namespace wss;

void run_host_server(HostServerOptions options) {
    // Pipeline threads join the MTA and exit with each stream. Without a process-wide MTA reference, COM tears the
    // MTA down after the last stream and unloads WinRT DLLs that C++/WinRT's factory cache still points into.
    CO_MTA_USAGE_COOKIE mta_cookie{};
    check_hresult(CoIncrementMTAUsage(&mta_cookie), "Keeping the COM multithreaded apartment alive");
    // Input mapping uses physical pixels for window rectangles, the cursor and the virtual desktop.
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        spdlog::warn("Could not enable per-monitor DPI awareness (error {}); remote input may be offset on scaled "
                     "displays", GetLastError());
    }
    Server server(std::move(options));
    server.tls.use_certificate(server.options.identity.certificate.get());
    const tcp::endpoint endpoint{asio::ip::make_address(server.options.bind_address), server.options.port};
    tcp::acceptor acceptor(server.io);
    acceptor.open(endpoint.protocol());
    // Prevent other processes from binding the same port (Windows SO_REUSEADDR allows hijacking).
    acceptor.set_option(asio::detail::socket_option::boolean<SOL_SOCKET, SO_EXCLUSIVEADDRUSE>(true));
    acceptor.bind(endpoint);
    acceptor.listen();
    spdlog::info("TLS: {}; SHA-256 fingerprint {}", server.options.identity.description,
                 server.options.identity.sha256_fingerprint);
    spdlog::info("Serving https://{}:{}/ (signaling wss://<host>:<port>/control); press Ctrl+C to stop",
                 server.options.bind_address, server.options.port);

    // Taskbar flash notifications arrive on the monitor's thread; track and forward them on the network thread.
    AttentionMonitor attention_monitor([&server](const AttentionEvent& event) {
        asio::post(server.io, [&server, event] {
            spdlog::debug("Attention {} for window {} (pid {})", event.requested ? "set" : "clear",
                          hwnd_text(event.window), event.pid);
            if (event.requested) {
                server.attention[event.window] = event;
            } else {
                server.attention.erase(event.window);
            }
            if (auto session = server.active.lock()) session->on_attention(event);
        });
    });

    // Clipboard notifications arrive on the clipboard thread; only the authenticated session receives them.
    ClipboardMonitor clipboard_monitor([&server](const ClipboardChange& change) {
        asio::post(server.io, [&server, change] {
            if (auto session = server.active.lock()) session->on_clipboard_changed(change);
        });
    });
    server.clipboard = &clipboard_monitor;

    // Window snapshots arrive on the tracking thread; the authenticated session receives the differences.
    WindowMonitor window_monitor([&server](std::vector<WindowState> windows) {
        asio::post(server.io, [&server, windows = std::move(windows)] {
            const auto events = server.windows.update(windows);
            if (auto session = server.active.lock()) {
                for (const auto& event : events) session->send_window_event(event);
            }
        });
    });

    asio::signal_set signals(server.io, SIGINT, SIGTERM);
    signals.async_wait([&](beast::error_code error, int) {
        if (error) return;
        spdlog::info("Shutting down");
        beast::error_code ignored;
        acceptor.close(ignored);
        // Join every pipeline before the io_context stops accepting their posted callbacks.
        if (auto session = server.active.lock()) session->close(websocket::close_code::going_away, "host shutting down");
        server.io.stop();
    });
    asio::co_spawn(server.io, accept_loop(server, acceptor), asio::detached);
    server.io.run();
}

} // namespace beamit::host
