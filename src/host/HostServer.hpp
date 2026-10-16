#pragma once

#include "host/TlsIdentity.hpp"
#include "beamit/capture/Capturer.hpp"
#include "beamit/media/Encoder.hpp"
#include "net/Crypto.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace beamit::host {

struct HostServerOptions {
    std::string bind_address{"0.0.0.0"};
    unsigned short port{};
    TlsIdentity identity;
    // PBKDF2-HMAC-SHA256(password, salt, iterations); the password itself is not retained.
    net::Sha256 auth_key{};
    std::vector<std::byte> auth_salt;
    std::uint32_t auth_iterations{};
    media::VideoCodec codec{media::VideoCodec::H264};
    std::uint32_t frame_rate{30};
    std::uint32_t bitrate_kbps{12000};
    std::function<std::unique_ptr<capture::Capturer>(capture::CaptureTargetKind)> make_capturer;
    std::function<std::unique_ptr<media::Encoder>(media::VideoCodec)> make_encoder;
};

// Serves PROTOCOL.md on one TLS port until Ctrl+C.
void run_host_server(HostServerOptions options);

} // namespace beamit::host
