#pragma once

// Boost.Asio/Beast must be included before boost-wintls.
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/json.hpp>
#include <wintls.hpp>
#include <wintls/beast.hpp>

#include "net/Protocol.hpp"

#include <chrono>

// Internal building blocks of the WSS host (PROTOCOL.md); only HostServer.cpp and src/host/wss use them.
namespace beamit::host::wss {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace json = boost::json;
namespace protocol = beamit::net::protocol;
using tcp = asio::ip::tcp;
using TlsStream = wintls::stream<tcp::socket>;
using WebSocket = websocket::stream<TlsStream>;
using asio::awaitable;
using asio::use_awaitable;
using Clock = std::chrono::steady_clock;

} // namespace beamit::host::wss
