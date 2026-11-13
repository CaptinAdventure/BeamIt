#include "beamit/media/OpusAudio.hpp"
#include "host/AudioEndpoint.hpp"
#include "host/BeamItMicrophoneOutput.hpp"
#include "host/WasapiMicrophoneOutput.hpp"
#include "host/wss/AudioSocket.hpp"
#include "net/AudioProtocol.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <future>
#include <mutex>
#include <thread>

using namespace beamit;
using namespace std::chrono_literals;

namespace {

template <class Predicate>
bool wait_for(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

std::vector<float> tone(const media::AudioConfiguration& configuration) {
    std::vector<float> pcm(configuration.sample_count());
    for (std::size_t frame = 0; frame < configuration.frame_size(); ++frame) {
        for (unsigned channel = 0; channel < configuration.channels; ++channel) {
            pcm[frame * configuration.channels + channel] =
                0.2f * std::sin(6.2831853f * (440 + channel * 220) * frame / configuration.sample_rate);
        }
    }
    return pcm;
}

struct MicrophoneDeviceState {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<host::BeamItMicPacket> written;
    bool first_write_started{};
    bool block_first_write{true};
    bool released{};
};

class RecordingMicrophoneDevice final : public host::BeamItMicrophoneDevice {
public:
    explicit RecordingMicrophoneDevice(std::shared_ptr<MicrophoneDeviceState> state) : state_(std::move(state)) {}
    void acquire() override {}
    void write(const host::BeamItMicPacket& packet) override {
        std::unique_lock lock(state_->mutex);
        if (state_->written.empty()) {
            state_->first_write_started = true;
            state_->changed.notify_all();
            state_->changed.wait(lock, [&] { return !state_->block_first_write; });
        }
        state_->written.push_back(packet);
        state_->changed.notify_all();
    }
    void release() noexcept override {
        std::lock_guard lock(state_->mutex);
        state_->released = true;
        state_->changed.notify_all();
    }
private:
    std::shared_ptr<MicrophoneDeviceState> state_;
};

struct FailedMicrophoneState {
    std::atomic<unsigned> created{};
    std::atomic<unsigned> writes{};
    std::atomic<unsigned> errors{};
};

class BusyMicrophoneDevice final : public host::BeamItMicrophoneDevice {
public:
    explicit BusyMicrophoneDevice(std::shared_ptr<FailedMicrophoneState> state) : state_(std::move(state)) {}
    void acquire() override { throw std::runtime_error("device busy"); }
    void write(const host::BeamItMicPacket&) override { ++state_->writes; }
    void release() noexcept override {}
private:
    std::shared_ptr<FailedMicrophoneState> state_;
};

} // namespace

TEST_CASE("Audio metadata is derived from the codec configuration", "[audio]") {
    const media::AudioConfiguration defaults;
    CHECK(defaults.frame_size() == 480);
    CHECK(defaults.sample_count() == 960);
    CHECK(defaults.duration_text() == "PT0.01S");
    for (const auto duration : {2500us, 5000us, 10000us, 20000us, 40000us, 60000us}) {
        media::AudioConfiguration configuration{.sample_rate = 48'000, .packet_duration = duration};
        configuration.validate();
        const auto object = host::wss::audio_configuration_json(configuration);
        const auto& opus = object.at("opusAudioConfiguration").as_object();
        CHECK(opus.at("sampleRate").as_uint64() == configuration.sample_rate);
        CHECK(opus.at("channels").as_uint64() == configuration.channels);
        CHECK(opus.at("frameSize").as_uint64() == configuration.frame_size());
        CHECK(opus.at("packetDuration").as_string() == configuration.duration_text());
        CHECK_FALSE(opus.at("useFec").as_bool());
        CHECK(opus.at("application").as_string() == "VOIP");
    }
    CHECK_THROWS_AS((media::AudioConfiguration{.sample_rate = 44100}.validate()), std::invalid_argument);
    CHECK_THROWS_AS((media::AudioConfiguration{.packet_duration = 7ms}.validate()), std::invalid_argument);
    CHECK_THROWS_AS((media::AudioConfiguration{.channels = 3}.validate()), std::invalid_argument);
}

TEST_CASE("Opus encodes and decodes packet-sized float PCM", "[audio]") {
    for (const auto rate : {8000u, 12000u, 16000u, 24000u, 48000u}) {
        for (const auto channels : {1u, 2u}) {
            media::AudioConfiguration configuration{.sample_rate = rate, .channels = channels};
            media::OpusAudio codec(configuration);
            const auto pcm = tone(configuration);
            float energy{};
            for (int index = 0; index < 4; ++index) {
                const auto packet = codec.encode(pcm);
                REQUIRE_FALSE(packet.empty());
                REQUIRE(packet.size() <= media::max_opus_packet_bytes);
                const auto decoded = codec.decode(packet, index == 0);
                REQUIRE(decoded.size() == pcm.size());
                CHECK(std::ranges::all_of(decoded, [](float value) { return std::isfinite(value); }));
                for (const auto value : decoded) energy += value * value;
            }
            CHECK(energy > 0.001f);
        }
    }
    media::OpusAudio codec;
    CHECK_THROWS_AS(codec.encode({}), std::invalid_argument);
    CHECK_THROWS_AS(codec.decode({}), std::invalid_argument);
    auto invalid = tone(codec.configuration());
    invalid.front() = std::numeric_limits<float>::quiet_NaN();
    CHECK_THROWS_AS(codec.encode(invalid), std::invalid_argument);
    media::OpusAudio other(media::AudioConfiguration{.packet_duration = 20ms});
    CHECK_THROWS_AS(codec.decode(other.encode(tone(other.configuration()))), std::invalid_argument);
}

TEST_CASE("Audio packets use network order and validate framing and continuity", "[audio][protocol]") {
    const media::AudioConfiguration configuration;
    net::protocol::AudioPacket packet{.track_id = 0x01020304, .sequence = 2, .sample_timestamp = 960, .discontinuity = true,
                                      .payload = {std::byte{0x12}, std::byte{0x34}}};
    auto bytes = net::protocol::encode_audio_packet(packet, configuration);
    REQUIRE(bytes.size() == 34);
    CHECK(bytes[0] == std::byte{2});
    CHECK(bytes[1] == std::byte{2});
    CHECK(bytes[3] == std::byte{32});
    CHECK(bytes[10] == std::byte{1});
    CHECK(bytes[11] == std::byte{0xe0});
    CHECK(bytes[12] == std::byte{1});
    CHECK(bytes[15] == std::byte{4});
    CHECK(bytes[23] == std::byte{2});
    CHECK(bytes[30] == std::byte{3});
    CHECK(bytes[31] == std::byte{0xc0});
    const auto decoded = net::protocol::decode_audio_packet(bytes, configuration, 0);
    CHECK(decoded.sequence == packet.sequence);
    CHECK(decoded.track_id == packet.track_id);
    CHECK(decoded.sample_timestamp == packet.sample_timestamp);
    CHECK(decoded.payload == packet.payload);
    CHECK(decoded.discontinuity);
    CHECK_THROWS(net::protocol::decode_audio_packet(bytes, configuration, 2));
    bytes[7] = std::byte{0};
    CHECK_THROWS(net::protocol::decode_audio_packet(bytes, configuration, 0));
    CHECK_THROWS(net::protocol::decode_audio_packet(bytes, configuration, std::nullopt));
    bytes[7] = std::byte{2};
    for (const auto offset : {0u, 1u, 3u, 11u, 31u}) {
        auto invalid = bytes;
        invalid[offset] ^= std::byte{0xff};
        CHECK_THROWS(net::protocol::decode_audio_packet(invalid, configuration, 0));
    }
    auto zero_track = bytes;
    std::fill(zero_track.begin() + 12, zero_track.begin() + 16, std::byte{});
    CHECK_THROWS(net::protocol::decode_audio_packet(zero_track, configuration, 0));
    CHECK_THROWS(net::protocol::decode_audio_packet(std::span(bytes).first(32), configuration, 0));
}

TEST_CASE("PCM endpoint runs bidirectional codecs and detaches cleanly", "[audio]") {
    host::AudioEndpoint endpoint;
    CHECK_FALSE(endpoint.connected());
    CHECK(endpoint.submit_pcm(2, {}) == host::AudioSubmitResult::NotConnected);
    std::atomic<unsigned> notifications{};
    auto pipeline = std::make_shared<host::AudioPipeline>(media::AudioConfiguration{}, [&] { ++notifications; });
    const host::AudioTrack microphone{.id = net::protocol::microphone_track_id};
    const host::AudioTrack capture{.id = 2, .kind = host::AudioTrack::Kind::Application, .pid = 1234};
    endpoint.attach(microphone, pipeline);
    endpoint.attach(capture, pipeline);
    CHECK(endpoint.connected());
    CHECK_THROWS_AS(endpoint.attach(capture, pipeline), std::logic_error);
    CHECK_THROWS_AS(endpoint.submit_pcm(microphone.id, {}), std::invalid_argument);
    CHECK(endpoint.tracks().size() == 2);
    CHECK(endpoint.tracks().back().pid == capture.pid);
    const auto pcm = tone(pipeline->configuration());
    CHECK(endpoint.submit_pcm(capture.id, pcm) == host::AudioSubmitResult::Accepted);
    std::optional<net::protocol::AudioPacket> encoded;
    REQUIRE(wait_for([&] { encoded = pipeline->receive_encoded(); return encoded.has_value(); }));
    CHECK(encoded->sequence == 0);
    CHECK(encoded->sample_timestamp == 0);
    CHECK(encoded->discontinuity);
    CHECK(pipeline->submit_encoded(*encoded));
    std::optional<host::AudioPcmPacket> decoded;
    REQUIRE(wait_for([&] { decoded = endpoint.receive_pcm(); return decoded.has_value(); }));
    CHECK(decoded->samples.size() == pcm.size());
    CHECK(decoded->sequence == 0);
    CHECK(notifications >= 1);
    endpoint.detach(capture.id, pipeline);
    CHECK(endpoint.submit_pcm(capture.id, pcm) == host::AudioSubmitResult::NotConnected);
    CHECK(endpoint.connected());
    endpoint.detach(microphone.id, pipeline);
    CHECK_FALSE(endpoint.connected());
    CHECK_FALSE(endpoint.receive_pcm());
    CHECK(endpoint.tracks().empty());
    pipeline->stop();
    pipeline->stop();
    CHECK(pipeline->submit_pcm(pcm) == host::AudioSubmitResult::NotConnected);
}

TEST_CASE("Audio decoder errors are explicit and stop the pipeline", "[audio]") {
    std::atomic<unsigned> notifications{};
    host::AudioPipeline pipeline({}, [&] { ++notifications; });
    REQUIRE(pipeline.submit_encoded({.payload = {std::byte{0xff}}}));
    REQUIRE(wait_for([&] { return !pipeline.error().empty(); }));
    CHECK_FALSE(pipeline.running());
    CHECK_FALSE(pipeline.error().empty());
    pipeline.stop();
    CHECK(notifications >= 1);
}

TEST_CASE("Audio submission queues reject overload instead of growing", "[audio]") {
    std::promise<void> entered;
    std::promise<void> resume;
    auto entered_future = entered.get_future();
    const auto resume_future = resume.get_future().share();
    std::atomic<bool> first{true};
    host::AudioPipeline pipeline({}, [&] {
        if (first.exchange(false)) {
            entered.set_value();
            resume_future.wait();
        }
    });
    struct ResumeWorker {
        std::promise<void>& promise;
        ~ResumeWorker() { promise.set_value(); }
    } resume_worker{resume};
    const auto pcm = tone(pipeline.configuration());
    media::OpusAudio codec;
    const auto payload = codec.encode(pcm);
    REQUIRE(pipeline.submit_pcm(pcm) == host::AudioSubmitResult::Accepted);
    REQUIRE(entered_future.wait_for(3s) == std::future_status::ready);
    for (std::size_t index = 0; index < host::max_queued_audio_packets; ++index) {
        CHECK(pipeline.submit_pcm(pcm) == host::AudioSubmitResult::Accepted);
        CHECK(pipeline.submit_encoded({.payload = payload}));
    }
    CHECK(pipeline.submit_pcm(pcm) == host::AudioSubmitResult::QueueFull);
    CHECK_FALSE(pipeline.submit_encoded({.payload = payload}));
}

TEST_CASE("BeamIt microphone output converts PCM and preserves discontinuities on overflow", "[audio]") {
    CHECK(host::microphone_sample_to_pcm16(-1.0f) == -32768);
    CHECK(host::microphone_sample_to_pcm16(-0.5f) == -16384);
    CHECK(host::microphone_sample_to_pcm16(0.5f) == 16384);
    CHECK(host::microphone_sample_to_pcm16(1.0f) == 32767);
    CHECK(host::microphone_sample_to_pcm16(std::numeric_limits<float>::infinity()) == 0);

    const auto state = std::make_shared<MicrophoneDeviceState>();
    host::BeamItMicrophoneOutput output({}, {}, [state] {
        return std::make_unique<RecordingMicrophoneDevice>(state);
    });
    const auto packet = [](std::uint64_t sequence) {
        return host::AudioPcmPacket{
            .sequence = sequence,
            .sample_timestamp = sequence * BEAMIT_MIC_FRAMES_PER_PACKET,
            .samples = std::vector<float>(BEAMIT_MIC_FRAMES_PER_PACKET * BEAMIT_MIC_CHANNELS, 0.5f),
        };
    };

    output.submit(packet(0));
    {
        std::unique_lock lock(state->mutex);
        REQUIRE(state->changed.wait_for(lock, 3s, [&] { return state->first_write_started; }));
    }
    output.submit(packet(1));
    output.submit(packet(2));
    output.submit(packet(3));
    output.submit(packet(4));
    {
        std::lock_guard lock(state->mutex);
        state->block_first_write = false;
        state->changed.notify_all();
    }
    {
        std::unique_lock lock(state->mutex);
        REQUIRE(state->changed.wait_for(lock, 3s, [&] { return state->written.size() == 4; }));
    }
    output.stop();

    REQUIRE(state->written.size() == 4);
    CHECK(state->written[0].header.sequence == 0);
    CHECK(state->written[1].header.sequence == 2);
    CHECK((state->written[1].header.flags & BeamItMicPcmDiscontinuity) != 0);
    CHECK(state->written[2].header.sequence == 3);
    CHECK(state->written[3].header.sequence == 4);
    CHECK(state->written[1].header.sample_timestamp == 960);
    CHECK(state->written[1].header.frame_count == BEAMIT_MIC_FRAMES_PER_PACKET);
    CHECK(state->written[1].samples.front() == 16384);
    CHECK(state->released);

    auto invalid = packet(1);
    invalid.sample_timestamp = 0;
    CHECK_THROWS_AS(host::make_beamit_mic_packet(invalid), std::invalid_argument);
    invalid = packet(0);
    invalid.samples.pop_back();
    CHECK_THROWS_AS(host::make_beamit_mic_packet(invalid), std::invalid_argument);
}

TEST_CASE("BeamIt microphone output reports a busy device without fallback", "[audio]") {
    const auto state = std::make_shared<FailedMicrophoneState>();
    std::string reported_error;
    host::BeamItMicrophoneOutput output({}, [&](std::string message) {
        reported_error = std::move(message);
        ++state->errors;
    }, [state] {
        ++state->created;
        return std::make_unique<BusyMicrophoneDevice>(state);
    });
    REQUIRE(wait_for([&] { return state->errors.load() == 1; }));
    CHECK(output.error() == "device busy");
    CHECK(reported_error == "device busy");
    CHECK(state->created == 1);
    CHECK(state->writes == 0);
    output.stop();
}

TEST_CASE("Microphone playback devices match exact names before unique substrings", "[audio]") {
    const std::vector<std::wstring> names{L"Speakers (Realtek(R) Audio)", L"CABLE Input (VB-Audio Virtual Cable)",
                                          L"CABLE Input", L"CABLE-A Input (VB-Audio Cable A)"};
    CHECK(host::match_render_endpoint(names, L"cable input") == 2);
    CHECK(host::match_render_endpoint(names, L"realtek") == 0);
    CHECK(host::match_render_endpoint(names, L"VB-Audio Virtual") == 1);
    CHECK_THROWS_WITH(host::match_render_endpoint(names, L"Input"), Catch::Matchers::ContainsSubstring("Several"));
    CHECK_THROWS_WITH(host::match_render_endpoint(names, L"Headphones"), Catch::Matchers::ContainsSubstring("No "));
    CHECK_THROWS(host::match_render_endpoint(names, L""));
    CHECK(host::match_render_endpoint({L"CABLE Input (VB-Audio Virtual Cable)"}, host::vb_cable_render_endpoint) == 0);
}

TEST_CASE("VB-Cable playback is found under its default or renamed endpoint name", "[audio]") {
    CHECK(host::match_vb_cable_endpoint({L"Speakers (Realtek(R) Audio)", L"CABLE In 16 Ch (VB-Audio Virtual Cable)",
                                         L"CABLE Input (VB-Audio Virtual Cable)"}) == 2);
    CHECK(host::match_vb_cable_endpoint({L"CABLE In 16ch (VB-Audio Virtual Cable)",
                                         L"Speakers (VB-Audio Virtual Cable)", L"Voicemeeter Input"}) == 1);
    CHECK_THROWS_WITH(host::match_vb_cable_endpoint({L"Speakers (Realtek(R) Audio)"}),
                      Catch::Matchers::ContainsSubstring("not installed"));
    CHECK_THROWS_WITH(host::match_vb_cable_endpoint({L"A (VB-Audio Virtual Cable)", L"B (VB-Audio Virtual Cable)"}),
                      Catch::Matchers::ContainsSubstring("Several"));
}

TEST_CASE("Microphone output falls back when the primary device cannot be acquired", "[audio]") {
    const auto busy = std::make_shared<FailedMicrophoneState>();
    const auto recording = std::make_shared<MicrophoneDeviceState>();
    recording->block_first_write = false;
    const auto factory = host::make_fallback_microphone_device(
        [busy] { return std::make_unique<BusyMicrophoneDevice>(busy); },
        [recording] { return std::make_unique<RecordingMicrophoneDevice>(recording); });
    auto device = factory();
    device->acquire();
    device->write(host::make_beamit_mic_packet({.samples = std::vector<float>(BEAMIT_MIC_FRAMES_PER_PACKET * 2)}));
    device->release();
    CHECK(recording->written.size() == 1);
    CHECK(recording->released);

    const auto failing = host::make_fallback_microphone_device(
        [busy] { return std::make_unique<BusyMicrophoneDevice>(busy); },
        [busy] { return std::make_unique<BusyMicrophoneDevice>(busy); });
    CHECK_THROWS_WITH(failing()->acquire(), "device busy; device busy");
}

// Plays 300 ms of silence into BEAMIT_TEST_MIC_OUTPUT ("vb-cable" or a device name), else the first playback device.
TEST_CASE("WASAPI microphone output plays into a real playback device", "[.microphone-render]") {
    std::wstring name;
    if (const auto* selected = _wgetenv(L"BEAMIT_TEST_MIC_OUTPUT")) name = selected;
    if (name == L"vb-cable") {
        name.clear();
    } else if (name.empty()) {
        const auto names = host::render_endpoint_names();
        if (names.empty()) SKIP("No active playback device");
        name = names.front();
    }
    auto device = host::make_wasapi_microphone_device(name);
    device->acquire();
    for (std::uint64_t sequence = 0; sequence < 30; ++sequence) {
        device->write(host::make_beamit_mic_packet({.sequence = sequence,
            .sample_timestamp = sequence * BEAMIT_MIC_FRAMES_PER_PACKET,
            .samples = std::vector<float>(BEAMIT_MIC_FRAMES_PER_PACKET * 2)}));
        std::this_thread::sleep_for(10ms);
    }
    device->release();
}
