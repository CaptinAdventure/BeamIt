#include "beamit/capture/Capturer.hpp"
#include "beamit/capture/DxgiCapturer.hpp"
#include "beamit/capture/WgcCapturer.hpp"
#include "beamit/media/D3D12VideoEncoder.hpp"
#include "beamit/media/Encoder.hpp"
#include "beamit/media/AutoVideoEncoder.hpp"
#include "media/EncodeSequence.hpp"
#include "capture/CaptureClock.hpp"
#include "host/StreamPipeline.hpp"

#ifdef BEAMIT_USE_CATCH2

#include <catch2/catch_test_macros.hpp>

#include <type_traits>

using namespace beamit::capture;

static_assert(std::is_abstract_v<beamit::media::Encoder>);
static_assert(std::is_final_v<beamit::media::D3D12VideoEncoder>);

TEST_CASE("QPC capture timestamps map onto steady_clock", "[capture]") {
    using namespace std::chrono_literals;
    LARGE_INTEGER counter{};
    LARGE_INTEGER frequency{};
    REQUIRE(QueryPerformanceCounter(&counter));
    REQUIRE(QueryPerformanceFrequency(&frequency));
    const auto before = std::chrono::steady_clock::now();
    const auto age_50ms = frequency.QuadPart / 20;
    const auto mapped = qpc_to_steady(counter.QuadPart - age_50ms);
    REQUIRE(mapped < before - 45ms);
    REQUIRE(mapped > before - 60ms);
    const auto now_100ns = counter.QuadPart / frequency.QuadPart * 10'000'000 +
                           counter.QuadPart % frequency.QuadPart * 10'000'000 / frequency.QuadPart;
    const auto mapped_100ns = qpc_100ns_to_steady(now_100ns - 500'000);
    REQUIRE(mapped_100ns < before - 45ms);
    REQUIRE(mapped_100ns > before - 60ms);
    // Missing, future and stale timestamps fall back to now.
    for (const auto value : {std::int64_t{0}, counter.QuadPart + frequency.QuadPart, counter.QuadPart - 5 * frequency.QuadPart}) {
        REQUIRE(qpc_to_steady(value) >= before);
    }
    REQUIRE(qpc_100ns_to_steady(0) >= before);
}

TEST_CASE("duration statistics keep count, total and maximum", "[stats]") {
    using namespace std::chrono_literals;
    beamit::host::DurationStats stats;
    stats.add(2ms);
    stats.add(6ms);
    stats.add(-1ms);
    REQUIRE(stats.count == 3);
    REQUIRE(stats.total == 8ms);
    REQUIRE(stats.max == 6ms);
}

TEST_CASE("capture target defaults to desktop", "[capture]") {
    const CaptureTarget target{};

    REQUIRE(target.kind == CaptureTargetKind::Desktop);
    REQUIRE(target.monitor == nullptr);
    REQUIRE(target.window == nullptr);
}

TEST_CASE("capture options default to cursor enabled", "[capture]") {
    const CaptureOptions options{};

    REQUIRE(options.capture_cursor);
}

TEST_CASE("DXGI capturer supports desktop but not window capture", "[capture][dxgi]") {
    const beamit::capture::DxgiCapturer capturer;
    const CaptureTarget desktop{};
    const CaptureTarget window{.kind = CaptureTargetKind::Window};

    REQUIRE(capturer.supports_target(desktop));
    REQUIRE_FALSE(capturer.supports_target(window));
}

TEST_CASE("DXGI capturer rejects window configuration", "[capture][dxgi]") {
    auto capturer = beamit::capture::DxgiCapturer{};
    const CaptureTarget window{.kind = CaptureTargetKind::Window};

    REQUIRE_FALSE(capturer.configure(window, CaptureOptions{}));
    REQUIRE(capturer.last_error() == E_INVALIDARG);
}

TEST_CASE("WGC capturer supports desktop and window capture", "[capture][wgc]") {
    const beamit::capture::WgcCapturer capturer;
    const CaptureTarget desktop{};
    const CaptureTarget window{.kind = CaptureTargetKind::Window};

    REQUIRE(capturer.supports_target(desktop));
    REQUIRE(capturer.supports_target(window));
}

TEST_CASE("WGC capturer rejects null window configuration", "[capture][wgc]") {
    auto capturer = beamit::capture::WgcCapturer{};
    const CaptureTarget window{.kind = CaptureTargetKind::Window};

    REQUIRE_FALSE(capturer.configure(window, CaptureOptions{}));
    REQUIRE(capturer.last_error() == E_INVALIDARG);
}

TEST_CASE("WGC capturer accepts an existing window configuration", "[capture][wgc]") {
    struct TestWindow {
        HWND handle{CreateWindowExW(0, L"STATIC", L"BeamIt capture configuration test",
                                   WS_POPUP, 0, 0, 64, 64, nullptr, nullptr,
                                   GetModuleHandleW(nullptr), nullptr)};
        ~TestWindow() { if (handle) DestroyWindow(handle); }
    } window;
    REQUIRE(window.handle != nullptr);
    auto capturer = beamit::capture::WgcCapturer{};
    REQUIRE(capturer.configure(
        CaptureTarget{.kind = CaptureTargetKind::Window, .window = window.handle}, {}));
    REQUIRE(capturer.last_error() == S_OK);
}

TEST_CASE("encoded packet defaults to no data and non-key-frame", "[encoder]") {
    const beamit::media::EncodedPacket packet{};

    REQUIRE(packet.codec == beamit::media::VideoCodec::H264);
    REQUIRE_FALSE(packet.key_frame);
}

TEST_CASE("encoder defaults to H.264 and accepts H.265 configuration", "[encoder]") {
    auto encoder = beamit::media::D3D12VideoEncoder{};
    const beamit::media::EncoderOptions invalid_options{};
    auto h265_options = beamit::media::EncoderOptions{
        .width = 1920,
        .height = 1080,
        .frame_rate = 60,
        .target_bitrate_kbps = 12000,
        .codec = beamit::media::VideoCodec::H265,
    };

    REQUIRE_FALSE(encoder.configure(invalid_options));
    REQUIRE(encoder.last_error() == E_INVALIDARG);
    REQUIRE(encoder.configure(h265_options));
    REQUIRE(encoder.start());
    REQUIRE(encoder.is_running());
    REQUIRE(encoder.try_repeat_frame() == beamit::media::EncoderSubmitResult::Failed);
    REQUIRE(encoder.last_error() == E_UNEXPECTED);
    encoder.stop();
    REQUIRE_FALSE(encoder.is_running());
}

TEST_CASE("automatic encoder validates configuration and requires an initial image", "[encoder][auto]") {
    using namespace beamit::media;
    AutoVideoEncoder encoder;
    REQUIRE_FALSE(encoder.is_running());
    REQUIRE_FALSE(encoder.start());
    REQUIRE(encoder.last_error() == E_UNEXPECTED);
    REQUIRE_FALSE(encoder.configure({}));
    REQUIRE(encoder.last_error() == E_INVALIDARG);
    const EncoderOptions options{1920, 1080, 60, 12000, VideoCodec::H264};
    auto invalid = options;
    invalid.width = 1919;
    REQUIRE_FALSE(encoder.configure(invalid));
    REQUIRE(encoder.configure(options));
    REQUIRE(encoder.start());
    REQUIRE(encoder.is_running());
    REQUIRE_FALSE(encoder.try_receive());
    REQUIRE(encoder.last_error() == S_OK);
    REQUIRE(encoder.try_repeat_frame() == EncoderSubmitResult::Failed);
    REQUIRE(encoder.last_error() == E_UNEXPECTED);
    REQUIRE(encoder.configure({1920, 1080, 60, 12000, VideoCodec::H265}));
    REQUIRE(encoder.start());
    REQUIRE(encoder.try_submit({}) == EncoderSubmitResult::Failed);
    REQUIRE(FAILED(encoder.last_error()));
    encoder.stop();
    encoder.stop();
    REQUIRE_FALSE(encoder.is_running());
}

TEST_CASE("low-delay sequence resets at IDRs and alternates reference surfaces", "[encoder]") {
    using namespace beamit::media::detail;
    for (std::uint64_t frame = 0; frame < 1000; ++frame) {
        const auto order = picture_order(frame);
        REQUIRE(order.index == frame % 60);
        REQUIRE(order.idr == (frame % 60 == 0));
        REQUIRE(order.output_slot != order.reference_slot);
        REQUIRE(order.index * 2 < (1U << (order_bits_minus4 + 4)));
        if (!order.idr) {
            REQUIRE(order.reference_slot == picture_order(frame - 1).output_slot);
        }
    }
}

#else

#include <cassert>

using namespace beamit::capture;

int main() {
    const CaptureTarget target{};
    const CaptureOptions options{};
    const beamit::capture::DxgiCapturer dxgi_capturer;
    const beamit::capture::WgcCapturer wgc_capturer;

    assert(target.kind == CaptureTargetKind::Desktop);
    assert(target.monitor == nullptr);
    assert(target.window == nullptr);
    assert(options.capture_cursor);
    assert(dxgi_capturer.supports_target(target));
    assert(!dxgi_capturer.supports_target(
        CaptureTarget{.kind = CaptureTargetKind::Window}));
    assert(wgc_capturer.supports_target(target));
    assert(wgc_capturer.supports_target(
        CaptureTarget{.kind = CaptureTargetKind::Window}));
    const beamit::media::EncodedPacket packet{};
    assert(packet.data.empty());
    assert(!packet.key_frame);
    const beamit::media::D3D12VideoEncoder encoder{};
    assert(encoder.backend_name() == L"Direct3D 12 Video Encode");
    return 0;
}

#endif
