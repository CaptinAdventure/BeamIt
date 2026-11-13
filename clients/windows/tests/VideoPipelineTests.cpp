#include "VideoPipeline.hpp"

#include "net/Protocol.hpp"

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

using namespace beamit::client;
using namespace std::chrono_literals;

namespace {

// Single-IDR Annex B fixtures generated locally (see clients/windows/README.md); not checked in.
std::vector<std::byte> fixture(const char* name) {
    const auto path = std::filesystem::path(BEAMIT_CLIENT_FIXTURES) / name;
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::vector<char> bytes{std::istreambuf_iterator<char>(file), {}};
    return {reinterpret_cast<const std::byte*>(bytes.data()), reinterpret_cast<const std::byte*>(bytes.data()) + bytes.size()};
}

VideoParameters parameters(std::string codec, std::string chroma, std::uint32_t bit_depth) {
    VideoParameters video;
    video.generation = 1;
    video.codec = std::move(codec);
    video.width = 1920;
    video.height = 1080;
    video.chroma_format = std::move(chroma);
    video.bit_depth = bit_depth;
    video.frame_rate = 60;
    return video;
}

struct Outcome {
    std::mutex mutex;
    std::string failure;
    std::atomic<int> key_frame_requests{};
};

bool wait_for(auto predicate, std::chrono::milliseconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

} // namespace

TEST_CASE("The pipeline decodes and presents supported fixtures", "[video-pipeline]") {
    for (const auto& [name, codec, chroma, depth] : {
             std::tuple{"h264-high-420.annexb", "h264", "4:2:0", 8U},
             std::tuple{"hevc-main-420.annexb", "h265", "4:2:0", 8U},
             std::tuple{"hevc-main10-420.annexb", "h265", "4:2:0", 10U},
             std::tuple{"hevc-main444.annexb", "h265", "4:4:4", 8U},
         }) {
        DYNAMIC_SECTION(name) {
            const auto bytes = fixture(name);
            if (bytes.empty()) SKIP("Generate fixtures as described in clients/windows/README.md");
            Outcome outcome;
            VideoPipeline pipeline({[&](const std::string& message) {
                                        std::scoped_lock lock(outcome.mutex);
                                        outcome.failure = message;
                                    },
                                    [&] { ++outcome.key_frame_requests; }});
            REQUIRE(pipeline.swap_chain());
            pipeline.resize(960, 600);
            pipeline.configure(parameters(codec, chroma, depth));
            pipeline.submit({1, 0, 1000, true, false, bytes});
            REQUIRE(wait_for([&] { return pipeline.stats().presented >= 1; }));
            const auto stats = pipeline.stats();
            CHECK(stats.decoded == 1);
            CHECK_FALSE(stats.decoder.empty());
            CHECK_FALSE(stats.output_format.empty());
            CHECK(stats.last_capture_time_us == 1000);
            const auto png = pipeline.screenshot_png();
            REQUIRE(png.size() > 8);
            CHECK(std::to_integer<unsigned>(png[1]) == 'P');
            std::scoped_lock lock(outcome.mutex);
            CHECK(outcome.failure.empty());
        }
    }
}

TEST_CASE("Unsupported profiles fail visibly instead of switching codecs", "[video-pipeline]") {
    const auto bytes = fixture("h264-high444.annexb");
    if (bytes.empty()) SKIP("Generate fixtures as described in clients/windows/README.md");
    Outcome outcome;
    VideoPipeline pipeline({[&](const std::string& message) {
                                std::scoped_lock lock(outcome.mutex);
                                outcome.failure = message;
                            },
                            [&] { ++outcome.key_frame_requests; }});
    auto video = parameters("h264", "4:4:4", 8);
    video.codec_string = "avc1.F4001F";
    pipeline.configure(video);
    pipeline.submit({1, 0, 1000, true, false, bytes});
    REQUIRE(wait_for([&] {
        std::scoped_lock lock(outcome.mutex);
        return !outcome.failure.empty();
    }));
    std::scoped_lock lock(outcome.mutex);
    CHECK(outcome.failure.find("H.264 4:4:4 8-bit") != std::string::npos);
    CHECK(outcome.failure.find("avc1.F4001F") != std::string::npos);
    CHECK(pipeline.stats().presented == 0);
}

TEST_CASE("Pictures before a key frame are dropped, not decoded", "[video-pipeline]") {
    const auto bytes = fixture("hevc-main-420.annexb");
    if (bytes.empty()) SKIP("Generate fixtures as described in clients/windows/README.md");
    VideoPipeline pipeline({{}, {}});
    pipeline.configure(parameters("h265", "4:2:0", 8));
    pipeline.submit({1, 0, 1, false, false, bytes});
    pipeline.submit({1, 1, 2, true, false, bytes});
    REQUIRE(wait_for([&] { return pipeline.stats().presented >= 1; }));
    CHECK(pipeline.stats().dropped == 1);
    CHECK(pipeline.stats().last_capture_time_us == 2);
}
