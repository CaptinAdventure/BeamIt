#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>

// Disposable plaintext, one-client Annex B test transport.
class TcpVideoSink {
public:
    TcpVideoSink() = default;
    TcpVideoSink(const TcpVideoSink&) = delete;
    TcpVideoSink& operator=(const TcpVideoSink&) = delete;

    ~TcpVideoSink() {
        if (client_ != INVALID_SOCKET) closesocket(client_);
        if (listener_ != INVALID_SOCKET) closesocket(listener_);
        if (initialized_) WSACleanup();
    }

    void listen(const std::string& address, unsigned short port) {
        WSADATA data{};
        const int startup = WSAStartup(MAKEWORD(2, 2), &data);
        if (startup != 0) {
            throw std::runtime_error("WSAStartup failed: " + std::to_string(startup));
        }
        initialized_ = true;
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == INVALID_SOCKET) fail("socket");
        BOOL exclusive = TRUE;
        if (setsockopt(listener_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                       reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == SOCKET_ERROR) {
            fail("SO_EXCLUSIVEADDRUSE");
        }
        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET;
        endpoint.sin_port = htons(port);
        if (InetPtonA(AF_INET, address.c_str(), &endpoint.sin_addr) != 1) {
            throw std::runtime_error("--bind must be a numeric IPv4 address");
        }
        if (bind(listener_, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == SOCKET_ERROR) {
            fail("bind");
        }
        if (::listen(listener_, 1) == SOCKET_ERROR) fail("listen");
    }

    void accept_client() {
        client_ = accept(listener_, nullptr, nullptr);
        if (client_ == INVALID_SOCKET) fail("accept");
        closesocket(listener_);
        listener_ = INVALID_SOCKET;
        const DWORD timeout = 5000;
        if (setsockopt(client_, SOL_SOCKET, SO_SNDTIMEO,
                       reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == SOCKET_ERROR) {
            fail("SO_SNDTIMEO");
        }
        BOOL no_delay = TRUE;
        if (setsockopt(client_, IPPROTO_TCP, TCP_NODELAY,
                       reinterpret_cast<const char*>(&no_delay), sizeof(no_delay)) == SOCKET_ERROR) {
            fail("TCP_NODELAY");
        }
    }

    void write(std::span<const std::byte> bytes) {
        while (!bytes.empty()) {
            const auto count = static_cast<int>(std::min<std::size_t>(
                bytes.size(), std::numeric_limits<int>::max()));
            const int sent = send(client_, reinterpret_cast<const char*>(bytes.data()), count, 0);
            if (sent == SOCKET_ERROR) fail("send (client disconnected or stalled)");
            if (sent == 0) throw std::runtime_error("TCP client disconnected");
            bytes = bytes.subspan(static_cast<std::size_t>(sent));
        }
    }

private:
    [[noreturn]] static void fail(const char* operation) {
        throw std::runtime_error(std::string(operation) + " failed (Winsock " +
                                 std::to_string(WSAGetLastError()) + ")");
    }
    SOCKET listener_{INVALID_SOCKET};
    SOCKET client_{INVALID_SOCKET};
    bool initialized_{false};
};
