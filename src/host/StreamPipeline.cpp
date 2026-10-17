#include "host/StreamPipeline.hpp"

#include "FrameTimer.hpp"
#include "ThreadPriority.hpp"
#include "Utf8.hpp"
#include "media/BitstreamInfo.hpp"

#include <spdlog/spdlog.h>

#include <optional>
#include <stdexcept>

namespace beamit::host {
namespace {

using namespace std::chrono_literals;

struct StartFailure : std::runtime_error {
    StartFailure(const char* code, const std::string& message) : std::runtime_error(message), code(code) {}
    const char* code;
};

struct Stopped {};

// WGC delivers no frames for minimized windows, so restore the target first (like clicking its taskbar button). The
// asynchronous request cannot block on a hung target; the pipeline thread waits for the result instead.
template <typename CheckStop>
void restore_minimized_window(const capture::CaptureTarget& target, FrameTimer& timer, CheckStop&& check_stop) {
    if (target.kind != capture::CaptureTargetKind::Window || !IsIconic(target.window)) return;
    spdlog::info("Restoring minimized window 0x{:X} before capture", reinterpret_cast<std::uintptr_t>(target.window));
    ShowWindowAsync(target.window, SW_RESTORE);
    const auto deadline = Clock::now() + 2s;
    while (IsIconic(target.window)) {
        check_stop();
        if (!IsWindow(target.window)) throw StartFailure("capture_failed", "the window closed before capture started");
        if (Clock::now() >= deadline) {
            throw StartFailure("capture_failed", "the minimized window could not be restored within 2 seconds");
        }
        timer.wait_until(Clock::now() + 5ms);
    }
}

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

std::uint64_t StreamPipeline::request_rate_change(std::optional<std::uint32_t> bitrate_kbps,
                                                  std::optional<std::uint32_t> frame_rate) {
    std::lock_guard lock(rate_mutex_);
    if (bitrate_kbps) rate_change_.bitrate_kbps = bitrate_kbps;
    if (frame_rate) rate_change_.frame_rate = frame_rate;
    rate_change_.serial = ++rate_serial_;
    return rate_change_.serial;
}

PipelineStats StreamPipeline::take_stats() {
    const auto now = Clock::now();
    std::lock_guard lock(stats_mutex_);
    auto stats = std::exchange(stats_, PipelineStats{});
    stats.elapsed = now - std::exchange(stats_since_, now);
    return stats;
}

std::optional<StreamPipeline::RateChange> StreamPipeline::take_rate_change() {
    std::lock_guard lock(rate_mutex_);
    if (rate_change_.serial == 0) return std::nullopt;
    return std::exchange(rate_change_, RateChange{});
}

void StreamPipeline::stop() noexcept {
    stop_requested_ = true;
    if (thread_.joinable()) thread_.join();
}

void StreamPipeline::run() noexcept {
    // Capture, pacing and encode submission all run here; ask the scheduler to treat the thread as time critical.
    const MmcssTaskGuard mmcss(set_realtime_priority_with_handle());
    if (!mmcss.registered()) spdlog::warn("MMCSS registration failed; using a time-critical thread priority");
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

    restore_minimized_window(target_, timer, check_stop);
    std::optional<CapturedFrame> latest;
    // (Re)starts capture and waits for its first frame; a fresh capture session always delivers a full image.
    const auto start_capture = [&](bool starting) {
        const auto fail = [&](const std::string& message) {
            if (starting) throw StartFailure("capture_failed", message);
            throw std::runtime_error(message);
        };
        if (!capturer_->configure(target_, {}) || !capturer_->start()) {
            fail(fmt::format("capture startup failed (HRESULT 0x{:08X})",
                             static_cast<std::uint32_t>(capturer_->last_error())));
        }
        const auto first_frame_deadline = Clock::now() + 5s;
        while (!latest) {
            check_stop();
            latest = capturer_->try_acquire_frame();
            if (FAILED(capturer_->last_error())) {
                fail(fmt::format("capture failed (HRESULT 0x{:08X})",
                                 static_cast<std::uint32_t>(capturer_->last_error())));
            }
            if (!latest) {
                if (Clock::now() >= first_frame_deadline) {
                    fail("no frame within 5 seconds; the source may be minimized, protected or on an inactive desktop");
                }
                timer.wait_until(Clock::now() + 1ms);
            }
        }
    };
    start_capture(true);

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
    // A rate change that restarted the encoder is reported after the new generation is announced.
    std::optional<std::uint64_t> announced_rate_serial;
    const auto reconfigure = [&] {
        ++parameters.generation;
        configure(false);
        if (!latest) {
            // A borrowed (DXGI) surface was already returned and the new encoder has no image to repeat while the
            // source is idle, so restart capture to obtain a complete frame.
            capturer_->stop();
            start_capture(false);
        }
        announce = true;
    };
    const auto apply_rate_change = [&](const RateChange& change) {
        const auto frame_rate = change.frame_rate.value_or(options_.frame_rate);
        const auto bitrate = change.bitrate_kbps.value_or(options_.target_bitrate_kbps);
        auto result = RateChangeResult::Unchanged;
        if (frame_rate != options_.frame_rate) {
            // The frame rate is part of the SPS timing information, so it needs new parameter sets and an IDR.
            options_.frame_rate = frame_rate;
            options_.target_bitrate_kbps = bitrate;
            reconfigure();
            result = RateChangeResult::Reconfigured;
        } else if (bitrate != options_.target_bitrate_kbps) {
            options_.target_bitrate_kbps = bitrate;
            if (encoder_->try_update_bitrate(bitrate)) {
                result = RateChangeResult::InPlace;
            } else {
                check_hresult(encoder_->last_error(), "In-place bitrate change");
                reconfigure();
                result = RateChangeResult::Reconfigured;
            }
        }
        parameters.frame_rate = options_.frame_rate;
        parameters.bitrate_kbps = options_.target_bitrate_kbps;
        spdlog::info("Stream rate change {}: {} FPS, {} kbps ({})", change.serial, parameters.frame_rate,
                     parameters.bitrate_kbps,
                     result == RateChangeResult::InPlace        ? "in place"
                     : result == RateChangeResult::Reconfigured ? "encoder reconfigured"
                                                                : "unchanged");
        if (result == RateChangeResult::Reconfigured) {
            announced_rate_serial = change.serial;
        } else if (!announced_rate_serial) {
            callbacks_.rate_changed(parameters, change.serial, result);
        } else {
            // An earlier change of this iteration already restarted the encoder; report both after the announcement.
            announced_rate_serial = change.serial;
        }
    };
    std::uint64_t sequence = 0;
    auto next_frame = Clock::now();
    while (true) {
        check_stop();
        timer.wait_until(next_frame);
        const auto woke = Clock::now();
        const auto frame_interval = std::chrono::nanoseconds(1'000'000'000 / options_.frame_rate);
        // Whole frame intervals between the scheduled tick and now were skipped by the previous iteration.
        const auto skipped = static_cast<std::uint64_t>(std::max<Clock::duration::rep>(
            0, (woke - next_frame) / frame_interval));
        next_frame = std::max(next_frame + frame_interval, woke);
        if (target_.kind == capture::CaptureTargetKind::Window && !IsWindow(target_.window)) {
            callbacks_.stopped("source_closed", "the captured window closed");
            return;
        }
        bool fresh = false;
        if (auto frame = capturer_->try_acquire_frame()) {
            latest = std::move(frame);
            fresh = true;
        }
        check_hresult(capturer_->last_error(), "Capture");
        // Keep only the newest surface until the client connects, so activation shows current content.
        if (!active_) continue;
        if (const auto change = take_rate_change()) apply_rate_change(*change);
        if (latest) {
            D3D11_TEXTURE2D_DESC current{};
            latest->texture->GetDesc(&current);
            if (current.Width != parameters.width || current.Height != parameters.height) {
                parameters.width = current.Width;
                parameters.height = current.Height;
                reconfigure();
            }
        }
        if (key_frame_requested_.exchange(false)) encoder_->request_key_frame();
        const auto submit_time = Clock::now();
        const auto capture_age = fresh && latest ? submit_time - latest->captured_at : Clock::duration{};
        const auto submitted = latest ? encoder_->try_submit(*latest) : encoder_->try_repeat_frame();
        if (submitted != EncoderSubmitResult::Accepted) {
            check_hresult(encoder_->last_error(), "Frame submission");
            throw std::runtime_error("encoder unexpectedly rejected a frame after output was drained");
        }
        auto packet = receive(false);
        const auto encoded_at = Clock::now();
        // Without new content the picture still shows the source as of submission, so latency estimates based on
        // capture_time_us (PROTOCOL.md §7) stay meaningful while the source is idle.
        if (!fresh) packet.captured_at = submit_time;
        {
            std::lock_guard lock(stats_mutex_);
            stats_.frames_skipped += skipped;
            ++stats_.frames_encoded;
            if (fresh) {
                ++stats_.frames_captured;
                stats_.capture.add(capture_age);
            }
            if (packet.key_frame) ++stats_.key_frames;
            stats_.encoded_bytes += packet.data.size();
            stats_.encode.add(encoded_at - submit_time);
            stats_.busy += encoded_at - woke;
        }
        if (latest && latest->lifetime) latest.reset();
        if (announce) {
            describe(packet, false);
            spdlog::info("Stream reconfigured: generation {}, {}x{} {}", parameters.generation, parameters.width,
                         parameters.height, parameters.codec_string);
            callbacks_.reconfigured(parameters);
            announce = false;
            if (announced_rate_serial) {
                callbacks_.rate_changed(parameters, *announced_rate_serial, RateChangeResult::Reconfigured);
                announced_rate_serial.reset();
            }
        }
        auto frame = std::make_shared<EncodedFrame>();
        frame->packet = std::move(packet);
        frame->generation = parameters.generation;
        frame->sequence = sequence++;
        frame->encoded_at = encoded_at;
        callbacks_.frame(std::move(frame));
    }
}

} // namespace beamit::host
