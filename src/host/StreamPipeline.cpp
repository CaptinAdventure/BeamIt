#include "host/StreamPipeline.hpp"

#include "FrameTimer.hpp"
#include "Utf8.hpp"
#include "media/BitstreamInfo.hpp"

#include <spdlog/spdlog.h>

#include <optional>
#include <stdexcept>

namespace beamit::host {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

struct StartFailure : std::runtime_error {
    StartFailure(const char* code, const std::string& message) : std::runtime_error(message), code(code) {}
    const char* code;
};

struct Stopped {};

} // namespace

StreamPipeline::StreamPipeline(std::unique_ptr<capture::Capturer> capturer,
                               std::unique_ptr<media::Encoder> encoder,
                               capture::CaptureTarget target, media::EncoderOptions options)
    : capturer_(std::move(capturer)), encoder_(std::move(encoder)), target_(target), options_(options) {}

StreamPipeline::~StreamPipeline() { stop(); }

void StreamPipeline::start(PipelineCallbacks callbacks) {
    callbacks_ = std::move(callbacks);
    thread_ = std::thread([this] { run(); });
}

void StreamPipeline::activate() noexcept {
    key_frame_requested_ = true;
    active_ = true;
}

void StreamPipeline::request_key_frame() noexcept { key_frame_requested_ = true; }

void StreamPipeline::stop() noexcept {
    stop_requested_ = true;
    if (thread_.joinable()) thread_.join();
}

void StreamPipeline::run() noexcept {
    try {
        loop();
    } catch (const Stopped&) {
    } catch (const StartFailure& failure) {
        spdlog::warn("Stream startup failed: {}", failure.what());
        callbacks_.start_failed(failure.code, failure.what());
    } catch (const std::exception& error) {
        spdlog::error("Stream pipeline failed: {}", error.what());
        callbacks_.stopped("error", error.what());
    } catch (...) {
        callbacks_.stopped("error", "unknown pipeline failure");
    }
    encoder_->stop();
    capturer_->stop();
    // Release GPU/WinRT objects on the thread that used them.
    encoder_.reset();
    capturer_.reset();
}

void StreamPipeline::loop() {
    using namespace media;
    FrameTimer timer;
    const auto check_stop = [&] {
        if (stop_requested_) throw Stopped{};
    };

    if (!capturer_->configure(target_, {}) || !capturer_->start()) {
        throw StartFailure("capture_failed",
            fmt::format("capture startup failed (HRESULT 0x{:08X})",
                        static_cast<std::uint32_t>(capturer_->last_error())));
    }
    std::optional<CapturedFrame> latest;
    const auto first_frame_deadline = Clock::now() + 5s;
    while (!latest) {
        check_stop();
        latest = capturer_->try_acquire_frame();
        if (FAILED(capturer_->last_error())) {
            throw StartFailure("capture_failed", fmt::format("capture failed (HRESULT 0x{:08X})",
                static_cast<std::uint32_t>(capturer_->last_error())));
        }
        if (!latest) {
            if (Clock::now() >= first_frame_deadline) {
                throw StartFailure("capture_failed",
                    "no frame within 5 seconds; the source may be minimized, protected or on an inactive desktop");
            }
            timer.wait_until(Clock::now() + 1ms);
        }
    }

    D3D11_TEXTURE2D_DESC description{};
    latest->texture->GetDesc(&description);
    VideoParameters parameters{.generation = 1, .codec = options_.codec, .width = description.Width,
                               .height = description.Height, .frame_rate = options_.frame_rate,
                               .bitrate_kbps = options_.target_bitrate_kbps};

    const auto receive = [&](bool starting) -> EncodedPacket {
        const auto deadline = Clock::now() + 5s;
        while (true) {
            check_stop();
            if (auto packet = encoder_->try_receive()) return std::move(*packet);
            if (FAILED(encoder_->last_error()) || Clock::now() >= deadline) {
                const auto message = fmt::format("encoding failed (HRESULT 0x{:08X})",
                    static_cast<std::uint32_t>(encoder_->last_error()));
                if (starting) throw StartFailure("encoder_failed", message);
                throw std::runtime_error(message);
            }
            timer.wait_until(Clock::now() + 1ms);
        }
    };
    const auto configure = [&](bool starting) {
        auto options = options_;
        options.width = parameters.width;
        options.height = parameters.height;
        if (!encoder_->configure(options) || !encoder_->start()) {
            const auto message = fmt::format("encoder configuration failed (HRESULT 0x{:08X})",
                static_cast<std::uint32_t>(encoder_->last_error()));
            if (starting) throw StartFailure("encoder_failed", message);
            throw std::runtime_error(message);
        }
    };
    const auto describe = [&](const EncodedPacket& packet, bool starting) {
        const auto info = parse_bitstream_info(packet.data, options_.codec);
        if (!packet.key_frame || !info) {
            const std::string message = "first encoded picture is not a parsable key frame";
            if (starting) throw StartFailure("encoder_failed", message);
            throw std::runtime_error(message);
        }
        parameters.codec_string = info->codec_string;
        parameters.chroma_format_idc = info->chroma_format_idc;
        parameters.bit_depth = info->bit_depth_luma;
        parameters.encoder = to_utf8(encoder_->backend_name());
    };

    configure(true);
    if (encoder_->try_submit(*latest) != EncoderSubmitResult::Accepted) {
        throw StartFailure("encoder_failed", fmt::format("first frame rejected (HRESULT 0x{:08X})",
            static_cast<std::uint32_t>(encoder_->last_error())));
    }
    describe(receive(true), true);
    if (latest->lifetime) latest.reset();
    spdlog::info("Stream ready: {}x{} {} {} {}-bit, {} FPS, {} kbps via {}", parameters.width, parameters.height,
                 parameters.codec_string, chroma_format_name(parameters.chroma_format_idc), parameters.bit_depth,
                 parameters.frame_rate, parameters.bitrate_kbps, parameters.encoder);
    callbacks_.ready(parameters);

    bool announce = false;
    std::uint64_t sequence = 0;
    auto next_frame = Clock::now();
    while (true) {
        check_stop();
        timer.wait_until(next_frame);
        next_frame = std::max(next_frame + std::chrono::nanoseconds(1'000'000'000 / options_.frame_rate),
                              Clock::now());
        if (target_.kind == capture::CaptureTargetKind::Window && !IsWindow(target_.window)) {
            callbacks_.stopped("source_closed", "the captured window closed");
            return;
        }
        if (auto frame = capturer_->try_acquire_frame()) latest = std::move(frame);
        check_hresult(capturer_->last_error(), "Capture");
        // Keep only the newest surface until the client connects, so activation shows current content.
        if (!active_) continue;
        if (latest) {
            D3D11_TEXTURE2D_DESC current{};
            latest->texture->GetDesc(&current);
            if (current.Width != parameters.width || current.Height != parameters.height) {
                parameters.width = current.Width;
                parameters.height = current.Height;
                ++parameters.generation;
                configure(false);
                announce = true;
            }
        }
        if (key_frame_requested_.exchange(false)) encoder_->request_key_frame();
        const auto submitted = latest ? encoder_->try_submit(*latest) : encoder_->try_repeat_frame();
        if (submitted != EncoderSubmitResult::Accepted) {
            check_hresult(encoder_->last_error(), "Frame submission");
            throw std::runtime_error("encoder unexpectedly rejected a frame after output was drained");
        }
        auto packet = receive(false);
        if (latest && latest->lifetime) latest.reset();
        if (announce) {
            describe(packet, false);
            spdlog::info("Stream reconfigured: generation {}, {}x{} {}", parameters.generation, parameters.width,
                         parameters.height, parameters.codec_string);
            callbacks_.reconfigured(parameters);
            announce = false;
        }
        auto frame = std::make_shared<EncodedFrame>();
        frame->packet = std::move(packet);
        frame->generation = parameters.generation;
        frame->sequence = sequence++;
        callbacks_.frame(std::move(frame));
    }
}

} // namespace beamit::host
