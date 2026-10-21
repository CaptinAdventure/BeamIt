#include "host/HostServer.hpp"
#include "host/HostControl.hpp"
#include "host/TrayHost.hpp"
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
    DWORD console_processes[2]{};
    const bool standalone_console = GetConsoleProcessList(console_processes, 2) == 1;
    if (standalone_console) FreeConsole();
    if (!AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) &&
        !SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        spdlog::warn("Could not enable per-monitor DPI awareness (error {})", GetLastError());
    }
    CLI::App app{"BeamIt low-latency application-remoting host (WSS protocol, see PROTOCOL.md)"};
    std::string password_file;
    std::string tls_cert;
    std::string tls_key;
    unsigned short port{17389};
    std::string bind_address{"0.0.0.0"};
    bool elevate_input{};
    app.add_flag("--elevate-input", elevate_input,
        "Use a separate UAC-elevated input helper; stop the host if it cannot start or stops");
    app.add_option("--port", port, "TCP listening port")->default_val(17389)->check(CLI::Range(1, 65535));
    app.add_option("--bind", bind_address, "Numeric listen address (IPv4 or IPv6)")->default_val("0.0.0.0");
    app.add_option("--password-file", password_file,
        "File containing the shared password (omitted: generate one and open the password dialog)");
    auto* cert_option = app.add_option("--tls-cert", tls_cert,
        "PEM certificate (default: certificate installed by Install-BeamItCert.ps1)");
    auto* key_option = app.add_option("--tls-key", tls_key, "PEM PKCS#8 RSA private key for --tls-cert");
    cert_option->needs(key_option);
    key_option->needs(cert_option);
    try {
        try {
            app.parse(argc, argv);
        } catch (const CLI::ParseError& error) {
            if (standalone_console) MessageBoxA(nullptr, error.what(), "BeamIt", MB_OK | MB_ICONINFORMATION);
            return app.exit(error);
        }
        beamit::host::HostServerOptions options;
        options.bind_address = bind_address;
        options.port = port;
        options.elevate_input = elevate_input;
        if (*cert_option && (!std::filesystem::exists(tls_cert) || !std::filesystem::exists(tls_key))) {
            throw beamit::host::MissingTlsCertificate("The specified TLS certificate or private key was not found.");
        }
        options.identity = *cert_option ? beamit::host::load_pem_identity(tls_cert, tls_key)
                                        : beamit::host::installed_identity();
        options.auth_salt = beamit::net::random_bytes(16);
        options.auth_iterations = beamit::net::protocol::pbkdf2_iterations;
        {
            std::string password;
            if (password_file.empty()) {
                auto chosen = beamit::host::connection_password_dialog(nullptr,
                    beamit::host::generate_connection_password());
                if (!chosen) return 0;
                password = std::move(*chosen);
            } else {
                password = read_password(password_file);
            }
            try {
                options.auth_key = beamit::net::pbkdf2_sha256(password, options.auth_salt, options.auth_iterations);
            } catch (...) {
                SecureZeroMemory(password.data(), password.size());
                throw;
            }
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
        beamit::host::run_tray_host(std::move(options));
        return 0;
    } catch (const beamit::host::MissingTlsCertificate& error) {
        spdlog::error("Host failed: {}", error.what());
        MessageBoxW(nullptr,
            L"No usable TLS certificate was found. BeamIt cannot start without TLS.\n\n"
            L"Run Install-BeamItCert.ps1 (next to BeamIt.exe, or in the repository's scripts folder) "
            L"as your Windows user, then reopen BeamIt.exe.\n\n"
            L"If you supplied --tls-cert/--tls-key, check those paths or omit them to use the installed certificate.",
            L"BeamIt - TLS certificate required", MB_OK | MB_ICONWARNING);
        return 1;
    } catch (const std::exception& error) {
        spdlog::error("Host failed: {}", error.what());
        if (standalone_console) MessageBoxA(nullptr, error.what(), "BeamIt - Cannot start host", MB_OK | MB_ICONERROR);
        return 1;
    }
}
