#pragma once

#include "host/wss/WebSocket.hpp"

namespace beamit::host::wss {

struct Server;

// Accepts TLS connections; serves the test page at `/` and upgrades `/control` and `/streams/{id}?token=...` to
// WebSockets (PROTOCOL.md §2). Returns when the acceptor is closed.
awaitable<void> accept_loop(Server& server, tcp::acceptor& acceptor);

} // namespace beamit::host::wss
