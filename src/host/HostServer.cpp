// Process-level host setup: TLS listener, desktop monitors and the network thread. The protocol itself lives in
// src/host/wss (HttpFrontend: HTTP/upgrades, ControlSession*: signaling, StreamSocket: video delivery).
#include "host/wss/ControlSession.hpp"
#include "host/wss/HttpFrontend.hpp"

#include "FrameTimer.hpp"
#include "ThreadPriority.hpp"
#include "host/ElevatedInput.hpp"

#include <spdlog/spdlog.h>

#include <csignal>

namespace beamit::host {

using namespace wss;

void run_host_server(HostServerOptions options) {
    // Complete UAC and authenticate the local peer before opening any listener.
    if (options.elevate_input) options.elevated_input = std::make_shared<input::ElevatedInput>();
    // Pipeline threads join the MTA and exit with each stream. Without a process-wide MTA reference, COM tears the
    // MTA down after the last stream and unloads WinRT DLLs that C++/WinRT's factory cache still points into.
    CO_MTA_USAGE_COOKIE mta_cookie{};
    check_hresult(CoIncrementMTAUsage(&mta_cookie), "Keeping the COM multithreaded apartment alive");
    // Input mapping uses physical pixels for window rectangles, the cursor and the virtual desktop.
    if (!AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) &&
        !SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        spdlog::warn("Could not enable per-monitor DPI awareness (error {}); remote input may be offset on scaled "
                     "displays", GetLastError());
    }
    Server server(std::move(options));
    if (server.options.elevated_input) {
        server.input_elevation = InputElevation::Elevated;
    } else {
        const auto integrity = input::process_integrity(GetCurrentProcessId());
        if (integrity.error) {
            server.input_elevation = InputElevation::Unknown;
            spdlog::warn("Cannot determine input handler elevation (error {})", integrity.error);
        } else {
            server.input_elevation = integrity.level >= SECURITY_MANDATORY_HIGH_RID
                ? InputElevation::Elevated : InputElevation::NotElevated;
            if (server.input_elevation == InputElevation::NotElevated) {
                spdlog::warn("Input handler is not elevated; administrator windows may reject remote input. "
                             "Restart BeamIt with --elevate-input to use the separate UAC-elevated helper.");
            }
        }
    }
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
    asio::steady_timer control_timer(server.io);
    bool helper_failed{};
    const auto stop = [&] {
        spdlog::info("Shutting down");
        beast::error_code ignored;
        acceptor.close(ignored);
        control_timer.cancel();
        // Join every pipeline before the io_context stops accepting their posted callbacks.
        if (auto session = server.active.lock()) session->close(websocket::close_code::going_away, "host shutting down");
        server.io.stop();
    };
    signals.async_wait([&](beast::error_code error, int) {
        if (error) return;
        stop();
    });
    std::function<void()> poll_control;
    if (server.options.control) {
        poll_control = [&] {
            for (auto& command : server.options.control->take_commands()) {
                switch (command.kind) {
                case HostCommand::Kind::Exit:
                    stop();
                    return;
                case HostCommand::Kind::ChangePassword:
                    server.change_password(command);
                    SecureZeroMemory(command.auth_key.data(), command.auth_key.size());
                    break;
                case HostCommand::Kind::DisconnectStream:
                    if (auto session = server.active.lock()) session->disconnect_stream(command.stream_id);
                    break;
                case HostCommand::Kind::InstallElevatedInput:
                    try {
                        if (!command.elevated_input || !command.elevated_input->healthy()) {
                            throw std::runtime_error("The elevated input helper is not available");
                        }
                        if (auto session = server.active.lock()) session->upgrade_input(command.elevated_input);
                        server.options.elevated_input = std::move(command.elevated_input);
                        server.input_elevation = InputElevation::Elevated;
                        spdlog::info("Input elevated locally; active streams now use the helper");
                        server.options.control->notify({true, "Input is now elevated. Existing and new streams use "
                                                             "the separate input helper."});
                    } catch (const std::exception& error) {
                        spdlog::error("Could not activate elevated input: {}", error.what());
                        server.options.control->notify({false, std::string("Could not activate elevated input: ") +
                            error.what() + "\n\nThe host is still running with its previous input handler."});
                    }
                    break;
                }
            }
            server.publish_status();
            control_timer.expires_after(std::chrono::milliseconds(100));
            control_timer.async_wait([&](beast::error_code error) {
                if (!error) poll_control();
            });
        };
        poll_control();
    }
    asio::steady_timer helper_monitor(server.io);
    std::function<void()> check_helper;
    check_helper = [&] {
        helper_monitor.expires_after(std::chrono::milliseconds(50));
        helper_monitor.async_wait([&](beast::error_code error) {
            if (error) return;
            if (server.options.elevated_input && !server.options.elevated_input->healthy()) {
                helper_failed = true;
                spdlog::critical("Elevated input helper stopped or IPC failed; stopping host (no fallback)");
                stop();
                return;
            }
            check_helper();
        });
    };
    check_helper();
    asio::co_spawn(server.io, accept_loop(server, acceptor), asio::detached);
    // The io_context thread performs every TLS/WebSocket write and input injection, so it is time critical too.
    const TimerResolutionGuard timer_resolution;
    if (!timer_resolution.active()) spdlog::warn("Could not raise the system timer resolution to 1 ms");
    const MmcssTaskGuard mmcss(set_realtime_priority_with_handle());
    if (!mmcss.registered()) spdlog::warn("MMCSS registration failed; using a time-critical thread priority");
    server.io.run();
    if (helper_failed) throw std::runtime_error("Elevated input helper failed; host stopped");
}

} // namespace beamit::host
