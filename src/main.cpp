#include "TcpVideoSink.hpp"
#include "FrameTimer.hpp"
#include "host/HostServer.hpp"
#include "net/Crypto.hpp"
#include "net/Protocol.hpp"
#include "ApplicationWindows.hpp"
#include "media/ProbeEncoders.hpp"
#ifdef BEAMIT_HAS_NVENC_PROBE
#include "media/ProbeNvenc.hpp"
#include "beamit/media/NvencVideoEncoder.hpp"
#endif
#include "beamit/capture/DxgiCapturer.hpp"
#include "beamit/capture/WgcCapturer.hpp"
#include "beamit/media/D3D12VideoEncoder.hpp"
#include "beamit/media/AutoVideoEncoder.hpp"

#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <algorithm>
#include <cstdint>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <memory>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <span>
#include <functional>

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

std::unique_ptr<beamit::media::Encoder> make_encoder(const std::string& backend, beamit::media::VideoCodec codec) {
    if (backend == "nvenc") {
        if (codec != beamit::media::VideoCodec::H264) {
            throw std::runtime_error("NVENC currently supports H.264 4:4:4 only");
        }
#ifdef BEAMIT_HAS_NVENC_PROBE
        return std::make_unique<beamit::media::NvencVideoEncoder>();
#else
        throw std::runtime_error("NVENC was not built; enable BEAMIT_FETCH_NVENC_INTERFACE in CMake");
#endif
    }
    if (backend == "auto") return std::make_unique<beamit::media::AutoVideoEncoder>();
    return std::make_unique<beamit::media::D3D12VideoEncoder>();
}

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

int record(
    beamit::capture::Capturer& capturer,
    beamit::media::Encoder& encoder,
    const beamit::capture::CaptureTarget& target,
    beamit::media::VideoCodec codec,
    std::uint32_t fps,
    std::uint32_t bitrate,
    const std::function<void(std::span<const std::byte>)>& write,
    bool streaming) {
    using namespace beamit::media;
    FrameTimer timer;
    if (!capturer.configure(target, {}) || !capturer.start()) {
        check_hresult(capturer.last_error(), "Capture startup");
        throw std::runtime_error("Capture startup failed without an HRESULT");
    }

    std::optional<CapturedFrame> latest;
    const auto first_frame_deadline = Clock::now() + 5s;
    while (!latest) {
        latest = capturer.try_acquire_frame();
        check_hresult(capturer.last_error(), "Capture");
        if (!latest) {
            if (Clock::now() >= first_frame_deadline) {
                throw std::runtime_error(
                    "No capture frame received within 5 seconds; ensure the target is not minimized "
                    "or capture-protected and is on an active interactive desktop");
            }
            timer.wait_until(Clock::now() + 1ms);
        }
    }
    D3D11_TEXTURE2D_DESC description{};
    latest->texture->GetDesc(&description);
    if (!encoder.configure({description.Width, description.Height, fps, bitrate, codec}) ||
        !encoder.start()) {
        check_hresult(encoder.last_error(), "Encoder startup");
        throw std::runtime_error("Encoder startup failed without an HRESULT");
    }

    spdlog::info(
        "{} {}x{} at {} FPS, {} kbps",
        streaming ? "Streaming" : "Recording for 5 seconds",
        description.Width, description.Height, fps, bitrate);
    const auto started = Clock::now();
    const auto frame_count = fps * 5;
    std::uint64_t frames_written = 0;
    std::uint64_t bytes_written = 0;
    auto stats_started = started;
    std::uint64_t stats_frames = 0;
    std::uint64_t stats_bytes = 0;
    double encode_ms = 0;
    double send_ms = 0;
    double peak_encode_ms = 0;
    double peak_send_ms = 0;
    auto next_frame = started;
    for (std::uint64_t index = 0; streaming || index < frame_count; ++index) {
        timer.wait_until(next_frame);
        next_frame = std::max(next_frame + std::chrono::nanoseconds(1'000'000'000 / fps),
                              Clock::now());
        if (target.kind == beamit::capture::CaptureTargetKind::Window && !IsWindow(target.window)) {
            spdlog::info("Selected window closed; ending stream");
            break;
        }
        if (auto frame = capturer.try_acquire_frame()) {
            latest = std::move(frame);
        }
        check_hresult(capturer.last_error(), "Capture");
        D3D11_TEXTURE2D_DESC current{};
        if (latest) latest->texture->GetDesc(&current);
        if (latest && (current.Width != description.Width || current.Height != description.Height)) {
            if (!encoder.configure({current.Width, current.Height, fps, bitrate, codec}) ||
                !encoder.start()) {
                check_hresult(encoder.last_error(), "Encoder resize");
                throw std::runtime_error("Encoder resize failed");
            }
            description = current;
        }
        // Repeat the last GPU frame when the desktop is idle, preserving file playback duration.
        const auto encode_started = Clock::now();
        const auto submitted = latest ? encoder.try_submit(*latest) : encoder.try_repeat_frame();
        if (submitted != EncoderSubmitResult::Accepted) {
            check_hresult(encoder.last_error(), "Frame submission");
            throw std::runtime_error("Encoder unexpectedly rejected a frame after output was drained");
        }
        const auto packet_deadline = Clock::now() + 5s;
        std::optional<EncodedPacket> packet;
        while (!packet) {
            packet = encoder.try_receive();
            check_hresult(encoder.last_error(), "Encoding/readback");
            if (!packet) {
                if (Clock::now() >= packet_deadline) {
                    throw std::runtime_error("Encoder produced no packet within 5 seconds");
                }
                timer.wait_until(Clock::now() + 1ms);
            }
        }
        const auto send_started = Clock::now();
        write(packet->data);
        const auto sent_at = Clock::now();
        ++frames_written;
        bytes_written += packet->data.size();
        if (streaming) {
            const double encode_time =
                std::chrono::duration<double, std::milli>(send_started - encode_started).count();
            const double send_time =
                std::chrono::duration<double, std::milli>(sent_at - send_started).count();
            encode_ms += encode_time;
            send_ms += send_time;
            peak_encode_ms = std::max(peak_encode_ms, encode_time);
            peak_send_ms = std::max(peak_send_ms, send_time);
            ++stats_frames;
            stats_bytes += packet->data.size();
            const double interval = std::chrono::duration<double>(sent_at - stats_started).count();
            if (interval >= 5) {
                spdlog::info(
                    "Host pipeline: {:.1f} FPS, {:.1f} Mbps; encode/readback avg/peak {:.2f}/{:.2f} ms, "
                    "socket write avg/peak {:.2f}/{:.2f} ms (excludes capture/player/network queues)",
                    stats_frames / interval, stats_bytes * 8.0 / interval / 1'000'000,
                    encode_ms / stats_frames, peak_encode_ms, send_ms / stats_frames, peak_send_ms);
                stats_started = sent_at;
                stats_frames = stats_bytes = 0;
                encode_ms = send_ms = peak_encode_ms = peak_send_ms = 0;
            }
        }
        if (latest && latest->lifetime) {
            // Idle submissions reuse the converted image, not a released capture surface.
            latest.reset();
        }
    }
    encoder.stop();
    capturer.stop();
    const auto elapsed = std::chrono::duration<double>(Clock::now() - started).count();
    if (!streaming) {
        spdlog::info("Wrote {} frames ({} bytes) in {:.2f}s; playback duration: {:.2f}s",
                    frames_written, bytes_written, elapsed, static_cast<double>(frames_written) / fps);
        if (elapsed > 5.5) {
            spdlog::warn("Encoding could not keep up with the requested frame rate");
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    CLI::App app{"BeamIt low-latency window streaming host"};
    std::string capture{"wgc"};
    std::string codec{"h264"};
    std::string encoder_backend{"auto"};
    std::string output;
    std::uint32_t fps{30};
    std::uint32_t bitrate{12000};
    bool list_apps{false};
    bool probe{false};
    bool nvenc_probe{false};
    bool insecure_stream{false};
    bool serve{false};
    std::string password_file;
    std::string tls_cert;
    std::string tls_key;
    bool desktop{false};
    unsigned short port{};
    DWORD pid{};
    std::string hwnd_text;
    std::string bind_address{"0.0.0.0"};
    app.add_flag("--list-apps", list_apps, "List visible application windows (PID, HWND, title)");
    app.add_flag("--probe-encoders", probe, "Probe D3D12 encoding capabilities on every hardware adapter");
    app.add_flag("--probe-nvenc", nvenc_probe, "Probe native NVIDIA H.264 4:4:4 capabilities without encoding");
    app.add_flag("--insecure-test-stream", insecure_stream,
                 "Opt in to UNAUTHENTICATED, UNENCRYPTED one-client TCP video");
    app.add_flag("--serve", serve, "Serve the WSS protocol (PROTOCOL.md) and test page on --port");
    auto* password_option = app.add_option("--password-file", password_file,
        "File containing the shared password for --serve (keep it readable only by you)");
    auto* cert_option = app.add_option("--tls-cert", tls_cert,
        "PEM certificate for --serve (default: persistent self-signed certificate)");
    auto* key_option = app.add_option("--tls-key", tls_key, "PEM PKCS#8 RSA private key for --tls-cert");
    cert_option->needs(key_option);
    key_option->needs(cert_option);
    auto* port_option = app.add_option("--port", port, "TCP listening port")
        ->check(CLI::Range(1, 65535));
    auto* pid_option = app.add_option("--app", pid, "Application PID; selects its primary-window candidate")
        ->check(CLI::Range(1ULL, 0xffffffffULL));
    auto* hwnd_option = app.add_option("--hwnd", hwnd_text, "Exact window handle (hex 0x... or decimal)");
    pid_option->excludes(hwnd_option);
    auto* desktop_option = app.add_flag("--desktop", desktop, "Capture the whole primary monitor");
    desktop_option->excludes(pid_option)->excludes(hwnd_option);
    auto* bind_option = app.add_option("--bind", bind_address, "Numeric listen address (IPv4; IPv6 also with --serve)")
        ->default_val("0.0.0.0");
    app.add_option("--capture", capture, "Capture backend")
        ->default_val("wgc")->check(CLI::IsMember({"wgc", "dxgi"}));
    app.add_option("--codec", codec, "Video codec")
        ->default_val("h264")->check(CLI::IsMember({"h264", "h265"}));
    auto* encoder_option = app.add_option("--encoder", encoder_backend, "Encoder backend (auto prefers 4:4:4 without changing codec)")
        ->default_val("auto")->check(CLI::IsMember({"auto", "d3d12", "nvenc"}));
    auto* output_option = app.add_option("-o,--output", output,
        "Five-second raw Annex B output file (overwrites existing file)");
    app.add_option("--fps", fps, "Frames per second")
        ->default_val(30)->check(CLI::Range(1, 240));
    app.add_option("--bitrate", bitrate, "Target bitrate in kbps")
        ->default_val(12000)->check(CLI::Range(1, 1'000'000));
    CLI11_PARSE(app, argc, argv);

    try {
        if (probe || nvenc_probe) {
            if (list_apps || *port_option || *pid_option || *hwnd_option || *desktop_option ||
                *output_option || *bind_option || *encoder_option || insecure_stream || serve ||
                *password_option || *cert_option || (probe && nvenc_probe)) {
                throw std::runtime_error("Select one probe mode without capture/streaming selectors");
            }
            if (nvenc_probe) {
#ifdef BEAMIT_HAS_NVENC_PROBE
                probe_nvenc();
#else
                throw std::runtime_error("NVENC probe was not built; enable BEAMIT_FETCH_NVENC_INTERFACE in CMake");
#endif
            } else {
                probe_encoders(fps, bitrate);
            }
            return 0;
        }
        if (list_apps) {
            if (*port_option || *pid_option || *hwnd_option || *desktop_option ||
                *output_option || *bind_option || *encoder_option || insecure_stream || serve ||
                *password_option || *cert_option) {
                throw std::runtime_error("--list-apps cannot be combined with recording/streaming selectors");
            }
            std::cout << "PID\tHWND\tPRIMARY?\tTITLE\n";
            for (const auto& window : application_windows()) {
                const int length = WideCharToMultiByte(CP_UTF8, 0, window.title.data(),
                    static_cast<int>(window.title.size()), nullptr, 0, nullptr, nullptr);
                if (length == 0) throw std::runtime_error("Window title conversion failed");
                std::string title(length, '\0');
                if (WideCharToMultiByte(CP_UTF8, 0, window.title.data(),
                    static_cast<int>(window.title.size()), title.data(), length, nullptr, nullptr) == 0) {
                    throw std::runtime_error("Window title conversion failed");
                }
                std::cout << fmt::format("{}\t0x{:X}\t{}\t{}\n", window.pid,
                    reinterpret_cast<std::uintptr_t>(window.hwnd),
                    window.primary_candidate ? "yes" : "no", title);
            }
            return 0;
        }
        if (serve) {
            if (!*port_option || !*password_option || insecure_stream || *output_option || *pid_option ||
                *hwnd_option || *desktop_option) {
                throw std::runtime_error(
                    "--serve needs --port and --password-file; sources are chosen by the client, so "
                    "--app/--hwnd/--desktop/--output/--insecure-test-stream are not allowed");
            }
            if (encoder_backend == "nvenc" && codec != "h264") {
                throw std::runtime_error("NVENC currently supports H.264 4:4:4 only");
            }
            beamit::host::HostServerOptions options;
            options.bind_address = bind_address;
            options.port = port;
            options.identity = *cert_option ? beamit::host::load_pem_identity(tls_cert, tls_key)
                                            : beamit::host::self_signed_identity();
            options.auth_salt = beamit::net::random_bytes(16);
            options.auth_iterations = beamit::net::protocol::pbkdf2_iterations;
            {
                auto password = read_password(password_file);
                options.auth_key = beamit::net::pbkdf2_sha256(password, options.auth_salt, options.auth_iterations);
                SecureZeroMemory(password.data(), password.size());
            }
            options.codec = codec == "h264" ? beamit::media::VideoCodec::H264 : beamit::media::VideoCodec::H265;
            options.frame_rate = fps;
            options.bitrate_kbps = bitrate;
            options.make_capturer = [capture](beamit::capture::CaptureTargetKind kind)
                -> std::unique_ptr<beamit::capture::Capturer> {
                if (kind == beamit::capture::CaptureTargetKind::Window || capture == "wgc") {
                    return std::make_unique<beamit::capture::WgcCapturer>();
                }
                return std::make_unique<beamit::capture::DxgiCapturer>();
            };
            options.make_encoder = [encoder_backend](beamit::media::VideoCodec selected) {
                return make_encoder(encoder_backend, selected);
            };
            beamit::host::run_host_server(std::move(options));
            return 0;
        }
        if (*password_option || *cert_option) {
            throw std::runtime_error("--password-file/--tls-cert/--tls-key require --serve");
        }
        if (insecure_stream != static_cast<bool>(*port_option) ||
            (*bind_option && !insecure_stream) ||
            (insecure_stream && (*output_option || (!desktop && !*pid_option && !*hwnd_option))) ||
            (!insecure_stream && !*output_option)) {
            throw std::runtime_error(
                "Use --output FILE, or --insecure-test-stream --port PORT with "
                "--desktop, --app PID or --hwnd HWND");
        }
        beamit::capture::CaptureTarget target{};
        if (*pid_option || *hwnd_option) {
            if (capture != "wgc") throw std::runtime_error("Application/window capture requires --capture wgc");
            target.kind = beamit::capture::CaptureTargetKind::Window;
            if (*pid_option) {
                target.window = select_application_window(pid);
            } else {
                std::string_view text{hwnd_text};
                int base = 10;
                if (text.starts_with("0x") || text.starts_with("0X")) {
                    base = 16;
                    text.remove_prefix(2);
                }
                std::uintptr_t handle{};
                const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), handle, base);
                if (error != std::errc{} || end != text.data() + text.size() || handle == 0) {
                    throw std::runtime_error("Invalid HWND; use the value from --list-apps");
                }
                target.window = reinterpret_cast<HWND>(handle);
            }
            if (!IsWindow(target.window) || GetAncestor(target.window, GA_ROOT) != target.window) {
                throw std::runtime_error("Selected HWND is not a valid top-level window");
            }
            if (!IsWindowVisible(target.window) || IsIconic(target.window)) {
                throw std::runtime_error("Selected window must be visible and not minimized; restore it first");
            }
            spdlog::info("Selected HWND 0x{:X}", reinterpret_cast<std::uintptr_t>(target.window));
        }
        std::unique_ptr<beamit::capture::Capturer> capturer;
        if (capture == "wgc") {
            capturer = std::make_unique<beamit::capture::WgcCapturer>();
        } else {
            capturer = std::make_unique<beamit::capture::DxgiCapturer>();
        }
        const auto selected_codec =
            codec == "h264" ? beamit::media::VideoCodec::H264 : beamit::media::VideoCodec::H265;
        auto encoder = make_encoder(encoder_backend, selected_codec);
        if (insecure_stream) {
            spdlog::warn("PLAINTEXT TEST MODE: anyone reaching {}:{} can view this window; no authentication",
                         bind_address, port);
            TcpVideoSink sink;
            sink.listen(bind_address, port);
            spdlog::info("Listening on {}:{}; waiting for one client", bind_address, port);
            sink.accept_client();
            return record(*capturer, *encoder, target, selected_codec, fps, bitrate,
                          [&](std::span<const std::byte> bytes) { sink.write(bytes); }, true);
        }
        std::ofstream file(std::filesystem::path(output), std::ios::binary | std::ios::trunc);
        if (!file) throw std::runtime_error("Cannot open output file: " + output);
        const int result = record(*capturer, *encoder, target,
                      selected_codec, fps, bitrate,
                      [&](std::span<const std::byte> bytes) {
                          file.write(reinterpret_cast<const char*>(bytes.data()),
                                     static_cast<std::streamsize>(bytes.size()));
                          if (!file) throw std::runtime_error("Writing output failed: " + output);
                      }, false);
        file.close();
        if (!file) throw std::runtime_error("Closing output failed: " + output);
        return result;
    } catch (const std::exception& error) {
        spdlog::error("Capture/stream failed: {}. Any output is incomplete.", error.what());
        return 1;
    }
}
