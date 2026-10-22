#include "host/AudioCapture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <algorithm>
#include <limits>
#include <thread>
#include <windows.h>
#include <filesystem>
#include <future>
#include <cmath>

using namespace beamit::host;
using namespace std::chrono_literals;

TEST_CASE("Audio capture chunks are assembled into derived packet sizes", "[audio][audio-capture]") {
    const beamit::media::AudioConfiguration configuration;
    std::vector<std::vector<float>> packets;
    std::vector<bool> discontinuities;
    AudioPacketizer packetizer(configuration, [&](std::span<const float> samples, bool discontinuity) {
        packets.emplace_back(samples.begin(), samples.end());
        discontinuities.push_back(discontinuity);
        return AudioSubmitResult::Accepted;
    });
    packetizer.append(std::vector<float>(160, 0.25f));
    CHECK(packets.empty());
    packetizer.append(std::vector<float>(800, 0.5f));
    REQUIRE(packets.size() == 1);
    CHECK(packets[0].size() == configuration.sample_count());
    CHECK(packets[0][159] == 0.25f);
    CHECK(packets[0][160] == 0.5f);
    CHECK(discontinuities[0]);
    packetizer.silence(configuration.frame_size() * 2);
    REQUIRE(packets.size() == 3);
    CHECK(std::ranges::all_of(packets[1], [](float value) { return value == 0; }));
    CHECK_FALSE(discontinuities[1]);
    CHECK_FALSE(discontinuities[2]);
    packetizer.append(std::vector<float>(100, 0.75f));
    packetizer.silence(configuration.frame_size(), true);
    REQUIRE(packets.size() == 4);
    CHECK(std::ranges::all_of(packets.back(), [](float value) { return value == 0; }));
    CHECK(discontinuities.back());
    packetizer.append(std::vector<float>(configuration.sample_count(), 1.5f));
    CHECK(packets.back().front() == 1.0f);
    CHECK_THROWS_AS(packetizer.append(std::vector<float>(1)), std::invalid_argument);
    CHECK_THROWS_AS(packetizer.append(std::vector<float>(2, std::numeric_limits<float>::infinity())),
                    std::runtime_error);
}

TEST_CASE("Audio capture drops mark the next accepted packet discontinuous", "[audio][audio-capture]") {
    const beamit::media::AudioConfiguration configuration;
    unsigned calls{};
    std::vector<bool> discontinuities;
    AudioPacketizer packetizer(configuration, [&](std::span<const float>, bool discontinuity) {
        discontinuities.push_back(discontinuity);
        return ++calls == 2 ? AudioSubmitResult::QueueFull : AudioSubmitResult::Accepted;
    });
    packetizer.silence(configuration.frame_size() * 4);
    REQUIRE(discontinuities.size() == 4);
    CHECK(discontinuities[0]);
    CHECK_FALSE(discontinuities[1]);
    CHECK(discontinuities[2]);
    CHECK_FALSE(discontinuities[3]);
    AudioPacketizer detached(configuration, [](std::span<const float>, bool) {
        return AudioSubmitResult::NotConnected;
    });
    CHECK_THROWS_AS(detached.silence(configuration.frame_size()), std::runtime_error);
}

TEST_CASE("Capture discontinuity survives the Opus pipeline", "[audio][audio-capture]") {
    AudioPipeline pipeline({}, [] {});
    const std::vector<float> pcm(pipeline.configuration().sample_count(), 0);
    for (unsigned index = 0; index < 3; ++index) {
        REQUIRE(pipeline.submit_pcm(pcm, index == 2) == AudioSubmitResult::Accepted);
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        std::optional<beamit::net::protocol::AudioPacket> packet;
        while (!(packet = pipeline.receive_encoded()) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        REQUIRE(packet);
        CHECK(packet->sequence == index);
        CHECK(packet->discontinuity == (index != 1));
    }
}

TEST_CASE("WASAPI rejects invalid source processes asynchronously", "[audio][audio-capture]") {
    auto endpoint = std::make_shared<AudioEndpoint>();
    std::atomic<bool> failed{};
    std::atomic<bool> started{};
    auto capture = make_wasapi_audio_capture(
        {.id = 2, .kind = AudioTrack::Kind::Application, .pid = UINT32_MAX}, endpoint,
        {.started = [&] { started = true; }, .failed = [&](std::string message) { failed = !message.empty(); }});
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!failed && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    capture->stop();
    CHECK(failed);
    CHECK_FALSE(started);
    capture->stop();
}

TEST_CASE("WASAPI process loopback captures another process and excludes the host tree", "[.wasapi-hardware]") {
    const auto name = L"Local\\BeamItAudioTest-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                      std::to_wstring(GetTickCount64());
    struct Resources {
        HANDLE stop{};
        HANDLE process{};
        ~Resources() {
            if (stop) SetEvent(stop);
            if (process) {
                WaitForSingleObject(process, 16'000);
                CloseHandle(process);
            }
            if (stop) CloseHandle(stop);
        }
    } resources;
    resources.stop = CreateEventW(nullptr, TRUE, FALSE, name.c_str());
    REQUIRE(resources.stop);
    wchar_t executable[32768]{};
    REQUIRE(GetModuleFileNameW(nullptr, executable, 32768) != 0);
    const auto source = std::filesystem::path(executable).parent_path() / L"BeamItAudioTestSource.exe";
    auto command = L"\"" + source.wstring() + L"\" \"" + name + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    REQUIRE(CreateProcessW(source.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                           nullptr, nullptr, &startup, &process));
    resources.process = process.hProcess;
    CloseHandle(process.hThread);
    auto endpoint = std::make_shared<AudioEndpoint>();
    auto pipeline = std::make_shared<AudioPipeline>(beamit::media::AudioConfiguration{}, [] {});
    AudioTrack application{.id = 2, .kind = AudioTrack::Kind::Application, .pid = process.dwProcessId};
    endpoint->attach(application, pipeline);
    std::promise<std::string> activated;
    std::atomic<bool> activation_reported{};
    std::atomic<bool> capture_failed{};
    auto activation = activated.get_future();
    auto capture = make_wasapi_audio_capture(application, endpoint,
        {.started = [&] { if (!activation_reported.exchange(true)) activated.set_value({}); },
         .failed = [&](std::string message) {
             capture_failed = true;
             if (!activation_reported.exchange(true)) activated.set_value(std::move(message));
         }});
    REQUIRE(activation.wait_for(12s) == std::future_status::ready);
    const auto error = activation.get();
    INFO(error);
    REQUIRE(error.empty());
    beamit::media::OpusAudio decoder;
    double tone_energy{};
    double application_tone{};
    unsigned packet_count{};
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline && packet_count < 60) {
        if (const auto packet = pipeline->receive_encoded()) {
            const auto pcm = decoder.decode(packet->payload, packet->discontinuity);
            REQUIRE(pcm.size() == application.configuration.sample_count());
            for (const auto value : pcm) tone_energy += value * value;
            double sine{}, cosine{};
            for (std::size_t frame = 0; frame < application.configuration.frame_size(); ++frame) {
                const auto phase = 6.283185307179586 * 997 * frame / application.configuration.sample_rate;
                sine += pcm[frame * 2] * std::sin(phase);
                cosine += pcm[frame * 2] * std::cos(phase);
            }
            application_tone += 2 * std::hypot(sine, cosine) / application.configuration.frame_size();
            ++packet_count;
        } else {
            std::this_thread::sleep_for(1ms);
        }
    }
    DWORD source_exit{};
    REQUIRE(GetExitCodeProcess(resources.process, &source_exit));
    INFO("Test tone process exit code: " << source_exit);
    CHECK(source_exit == STILL_ACTIVE);
    REQUIRE(packet_count >= 10);
    CHECK(tone_energy > 0.01);
    application_tone /= packet_count;
    INFO("Included test-tone amplitude: " << application_tone);
    REQUIRE(application_tone > 0.001);
    CHECK_FALSE(capture_failed);
    capture->stop();
    endpoint->detach(application.id, pipeline);
    pipeline->stop();

    // Desktop exclusion includes this test's child tone process in the excluded tree.
    auto desktop_pipeline = std::make_shared<AudioPipeline>(beamit::media::AudioConfiguration{}, [] {});
    AudioTrack desktop{.id = 3, .kind = AudioTrack::Kind::Desktop};
    endpoint->attach(desktop, desktop_pipeline);
    std::promise<std::string> desktop_activated;
    std::atomic<bool> desktop_activation_reported{};
    std::atomic<bool> desktop_failed{};
    auto desktop_activation = desktop_activated.get_future();
    auto desktop_capture = make_wasapi_audio_capture(desktop, endpoint,
        {.started = [&] { if (!desktop_activation_reported.exchange(true)) desktop_activated.set_value({}); },
         .failed = [&](std::string message) {
             desktop_failed = true;
             if (!desktop_activation_reported.exchange(true)) desktop_activated.set_value(std::move(message));
         }});
    REQUIRE(desktop_activation.wait_for(12s) == std::future_status::ready);
    const auto desktop_error = desktop_activation.get();
    INFO(desktop_error);
    REQUIRE(desktop_error.empty());
    unsigned desktop_packets{};
    double desktop_tone{};
    const auto desktop_deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < desktop_deadline && desktop_packets < 60) {
        if (const auto packet = desktop_pipeline->receive_encoded()) {
            const auto pcm = decoder.decode(packet->payload, packet->discontinuity || desktop_packets == 0);
            double sine{}, cosine{};
            for (std::size_t frame = 0; frame < desktop.configuration.frame_size(); ++frame) {
                const auto phase = 6.283185307179586 * 997 * frame / desktop.configuration.sample_rate;
                sine += pcm[frame * 2] * std::sin(phase);
                cosine += pcm[frame * 2] * std::cos(phase);
            }
            desktop_tone += 2 * std::hypot(sine, cosine) / desktop.configuration.frame_size();
            ++desktop_packets;
        } else {
            std::this_thread::sleep_for(1ms);
        }
    }
    REQUIRE(GetExitCodeProcess(resources.process, &source_exit));
    CHECK(source_exit == STILL_ACTIVE);
    REQUIRE(desktop_packets >= 10);
    desktop_tone /= desktop_packets;
    INFO("Excluded test-tone amplitude: " << desktop_tone);
    CHECK(desktop_tone < application_tone * 0.25);
    desktop_capture->stop();
    desktop_capture->stop();
    CHECK_FALSE(desktop_failed);
    CHECK(desktop_pipeline->error().empty());
    endpoint->detach(desktop.id, desktop_pipeline);
    desktop_pipeline->stop();
}
