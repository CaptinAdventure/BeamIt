#include "ClientSession.hpp"
#include "VideoPipeline.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>

using namespace beamit::client;
using namespace std::chrono_literals;

namespace {

std::string environment(const char* name) {
    char* value{};
    std::size_t size{};
    if (_dupenv_s(&value, &size, name) != 0 || !value) return {};
    std::string result(value);
    std::free(value);
    return result;
}

class Inbox {
public:
    Session::Dispatcher dispatcher() {
        return [this](std::function<void()> function) {
            std::scoped_lock lock(mutex_);
            queue_.push_back(std::move(function));
        };
    }
    template<class Predicate>
    bool pump_until(Predicate predicate, std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!predicate()) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::function<void()> function;
            {
                std::scoped_lock lock(mutex_);
                if (!queue_.empty()) {
                    function = std::move(queue_.front());
                    queue_.pop_front();
                }
            }
            if (function) function();
            else std::this_thread::sleep_for(5ms);
        }
        return true;
    }

private:
    std::mutex mutex_;
    std::deque<std::function<void()>> queue_;
};

} // namespace

// Requires a running host: set BEAMIT_E2E_URL, BEAMIT_E2E_PASSWORD_FILE and BEAMIT_E2E_FINGERPRINT.
// Streams the host's primary monitor as HEVC; sends no input.
TEST_CASE("A real host desktop stream decodes and presents", "[.e2e-host]") {
    const auto url = environment("BEAMIT_E2E_URL");
    const auto password_file = environment("BEAMIT_E2E_PASSWORD_FILE");
    const auto fingerprint = environment("BEAMIT_E2E_FINGERPRINT");
    if (url.empty() || password_file.empty() || fingerprint.empty()) SKIP("Set BEAMIT_E2E_URL/PASSWORD_FILE/FINGERPRINT");
    std::ifstream file(password_file, std::ios::binary);
    std::string password{std::istreambuf_iterator<char>(file), {}};
    while (!password.empty() && (password.back() == '\n' || password.back() == '\r')) password.pop_back();

    Inbox inbox;
    std::vector<json::object> messages;
    Session session(inbox.dispatcher(), {[&](const json::object& message) { messages.push_back(message); }, {}});
    std::optional<std::expected<void, SessionError>> connected;
    session.connect({url, password, fingerprint}, [&](auto result) { connected = result; });
    REQUIRE(inbox.pump_until([&] { return connected.has_value(); }, 15s));
    INFO((*connected ? std::string("connected") : connected->error().message));
    REQUIRE(*connected);
    REQUIRE(inbox.pump_until([&] {
        return std::ranges::any_of(messages, [](const json::object& m) { return m.at("type") == "window_opened"; });
    }, 5s));

    for (const auto* capture : {"dxgi", "wgc"}) {
        DYNAMIC_SECTION(capture) {
            std::optional<std::expected<json::object, SessionError>> reply;
            session.request("start_stream",
                {{"source", json::object{{"kind", "desktop"}, {"capture", capture}}}, {"video", json::object{{"codec", "h265"}}}},
                "stream_offer", [&](auto result) { reply = std::move(result); });
            REQUIRE(inbox.pump_until([&] { return reply.has_value(); }, 15s));
            INFO((*reply ? json::serialize(**reply) : reply->error().message));
            REQUIRE(*reply);
            const auto offer = parse_stream_offer(**reply, *session.endpoint());
            REQUIRE(offer);
            CHECK(offer->video.codec == "h265");
            WARN("Video: " << offer->video.codec_string << " " << offer->video.chroma_format << " "
                                    << offer->video.bit_depth << "-bit " << offer->video.width << "x" << offer->video.height
                                    << " encoder " << offer->video.encoder);

            std::mutex mutex;
            std::string failure;
            auto pipeline = std::make_shared<VideoPipeline>(VideoPipeline::Callbacks{
                [&](const std::string& message) {
                    std::scoped_lock lock(mutex);
                    failure = message;
                },
                [&session, id = offer->stream_id] { session.restart_stream_decoding(id); }});
            pipeline->resize(1280, 720);
            StreamCallbacks callbacks;
            callbacks.configured = [pipeline](const VideoParameters& video) { pipeline->configure(video); };
            callbacks.frame = [pipeline](const VideoFrame& frame) { pipeline->submit(frame); };
            std::optional<std::expected<void, SessionError>> opened;
            session.open_stream(*offer, callbacks, [&](auto result) { opened = result; });
            REQUIRE(inbox.pump_until([&] { return opened.has_value(); }, 10s));
            REQUIRE(*opened);
            REQUIRE(inbox.pump_until([&] {
                std::scoped_lock lock(mutex);
                return pipeline->stats().presented >= 30 || !failure.empty();
            }, 20s));
            const auto stats = pipeline->stats();
            WARN("Decoder " << stats.decoder << ", output " << stats.output << " " << stats.output_format
                                     << ", received " << stats.received << ", decoded " << stats.decoded << ", presented "
                                     << stats.presented << ", decode avg " << stats.decode_ms_average << " ms");
            {
                std::scoped_lock lock(mutex);
                CHECK(failure.empty());
            }
            CHECK(stats.presented >= 30);
            const auto clock = session.clock();
            REQUIRE(clock);
            const auto now_us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now().time_since_epoch()).count();
            WARN("Approximate capture-to-present-enqueue latency of the last picture: "
                          << ((now_us + clock->offset_us) - static_cast<double>(stats.last_capture_time_us)) / 1000.0
                          << " ms upper bound (RTT " << clock->rtt_us / 1000.0 << " ms)");

            std::optional<std::expected<json::object, SessionError>> stopped;
            session.request("stop_stream", {{"stream_id", offer->stream_id}}, "stream_stopped",
                            [&](auto result) { stopped = std::move(result); });
            REQUIRE(inbox.pump_until([&] { return stopped.has_value(); }, 10s));
            session.close_stream(offer->stream_id);
            CHECK(*stopped);
        }
    }
}
