#include "host/HostServer.hpp"
#include "net/Crypto.hpp"
#include "net/Protocol.hpp"
#include "beamit/capture/DxgiCapturer.hpp"
#include "beamit/capture/WgcCapturer.hpp"
#include "beamit/media/AutoVideoEncoder.hpp"

#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

std::string read_password(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot open --password-file " + path.string());
    std::string password{std::istreambuf_iterator<char>(file), {}};
    while (!password.empty() && (password.back() == '\n' || password.back() == '\r')) password.pop_back();
    if (password.empty()) throw std::runtime_error("--password-file is empty");
    if (password.size() < 12) spdlog::warn("The password is shorter than 12 characters; use a long random password");
    return password;
}

} // namespace

int main(int argc, char** argv) {
    CLI::App app{"BeamIt low-latency application-remoting host (WSS protocol, see PROTOCOL.md)"};
    std::string password_file;
    std::string tls_cert;
    std::string tls_key;
    unsigned short port{};
    std::string bind_address{"0.0.0.0"};
    bool elevate_input{};
    app.add_flag("--elevate-input", elevate_input,
        "Use a separate UAC-elevated input helper; stop the host if it cannot start or stops");
    app.add_option("--port", port, "TCP listening port")->required()->check(CLI::Range(1, 65535));
    app.add_option("--bind", bind_address, "Numeric listen address (IPv4 or IPv6)")->default_val("0.0.0.0");
    app.add_option("--password-file", password_file,
        "File containing the shared password (keep it readable only by you)")->required();
    auto* cert_option = app.add_option("--tls-cert", tls_cert,
        "PEM certificate (default: persistent self-signed certificate)");
    auto* key_option = app.add_option("--tls-key", tls_key, "PEM PKCS#8 RSA private key for --tls-cert");
    cert_option->needs(key_option);
    key_option->needs(cert_option);
    CLI11_PARSE(app, argc, argv);

    try {
        beamit::host::HostServerOptions options;
        options.bind_address = bind_address;
        options.port = port;
        options.elevate_input = elevate_input;
        options.identity = *cert_option ? beamit::host::load_pem_identity(tls_cert, tls_key)
                                        : beamit::host::self_signed_identity();
        options.auth_salt = beamit::net::random_bytes(16);
        options.auth_iterations = beamit::net::protocol::pbkdf2_iterations;
        {
            auto password = read_password(password_file);
            options.auth_key = beamit::net::pbkdf2_sha256(password, options.auth_salt, options.auth_iterations);
            SecureZeroMemory(password.data(), password.size());
        }
        options.make_capturer = [](beamit::host::CaptureBackend backend)
            -> std::unique_ptr<beamit::capture::Capturer> {
            if (backend == beamit::host::CaptureBackend::Wgc) return std::make_unique<beamit::capture::WgcCapturer>();
            return std::make_unique<beamit::capture::DxgiCapturer>();
        };
        options.make_encoder = [](beamit::media::VideoCodec) -> std::unique_ptr<beamit::media::Encoder> {
            return std::make_unique<beamit::media::AutoVideoEncoder>();
        };
        beamit::host::run_host_server(std::move(options));
        return 0;
    } catch (const std::exception& error) {
        spdlog::error("Host failed: {}", error.what());
        return 1;
    }
}
