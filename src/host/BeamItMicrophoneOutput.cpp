#include "host/BeamItMicrophoneOutput.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace beamit::host {
namespace {

std::string win32_error_text(DWORD error) {
    switch (error) {
    case ERROR_ACCESS_DENIED: return "access denied";
    case ERROR_BUSY:
    case ERROR_SHARING_VIOLATION: return "device busy (another producer owns the lease)";
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND: return "device unavailable (install and start the BeamIt Microphone driver)";
    default: return fmt::format("Win32 error {}", error);
    }
}

class Win32BeamItMicrophoneDevice final : public BeamItMicrophoneDevice {
public:
    ~Win32BeamItMicrophoneDevice() override { release(); }

    void acquire() override {
        handle_ = CreateFileW(BEAMIT_MIC_USER_PATH, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            handle_ = nullptr;
            throw std::runtime_error(fmt::format("BeamIt Microphone driver unavailable: {}",
                                                 win32_error_text(error)));
        }

        BeamItMicAcquireRequest request{
            BEAMIT_MIC_DRIVER_VERSION,
            GetCurrentProcessId(),
            {BEAMIT_MIC_SAMPLE_RATE_HZ, BEAMIT_MIC_CHANNELS, BEAMIT_MIC_BITS_PER_SAMPLE,
             BEAMIT_MIC_FRAMES_PER_PACKET},
        };
        DWORD returned{};
        if (!DeviceIoControl(handle_, IOCTL_BEAMIT_MIC_ACQUIRE, &request,
                             sizeof(request), nullptr, 0, &returned, nullptr)) {
            const auto error = GetLastError();
            CloseHandle(std::exchange(handle_, nullptr));
            throw std::runtime_error(fmt::format("Could not acquire BeamIt Microphone lease: {}",
                                                 win32_error_text(error)));
        }
        acquired_ = true;
    }

    void write(const BeamItMicPacket& packet) override {
        if (!acquired_) throw std::runtime_error("BeamIt Microphone lease is not acquired");
        std::array<std::byte, sizeof(BeamItMicPcmHeader) + BEAMIT_MIC_PACKET_BYTES> buffer{};
        std::memcpy(buffer.data(), &packet.header, sizeof(packet.header));
        std::memcpy(buffer.data() + sizeof(packet.header), packet.samples.data(), BEAMIT_MIC_PACKET_BYTES);
        DWORD returned{};
        if (!DeviceIoControl(handle_, IOCTL_BEAMIT_MIC_WRITE_PCM, buffer.data(),
                             static_cast<DWORD>(buffer.size()), nullptr, 0, &returned, nullptr)) {
            const auto error = GetLastError();
            throw std::runtime_error(fmt::format("BeamIt Microphone PCM write failed: {}", win32_error_text(error)));
        }
    }

    void release() noexcept override {
        if (handle_ == nullptr || handle_ == INVALID_HANDLE_VALUE) return;
        if (acquired_) {
            DWORD returned{};
            if (!DeviceIoControl(handle_, IOCTL_BEAMIT_MIC_RELEASE, nullptr, 0, nullptr, 0, &returned, nullptr)) {
                spdlog::error("Could not release BeamIt Microphone lease (Win32 error {})", GetLastError());
            }
            acquired_ = false;
        }
        CloseHandle(std::exchange(handle_, nullptr));
    }

private:
    HANDLE handle_{};
    bool acquired_{};
};

} // namespace

std::int16_t microphone_sample_to_pcm16(float sample) noexcept {
    if (!std::isfinite(sample)) return 0;
    if (sample <= -1.0f) return std::numeric_limits<std::int16_t>::min();
    if (sample >= 1.0f) return std::numeric_limits<std::int16_t>::max();
    return static_cast<std::int16_t>(std::lround(sample * 32768.0f));
}

BeamItMicPacket make_beamit_mic_packet(const AudioPcmPacket& packet) {
    if (packet.samples.size() != BEAMIT_MIC_FRAMES_PER_PACKET * BEAMIT_MIC_CHANNELS) {
        throw std::invalid_argument("BeamIt Microphone requires exactly one 48 kHz stereo 10 ms PCM packet");
    }
    if (packet.sequence > std::numeric_limits<std::uint64_t>::max() / BEAMIT_MIC_FRAMES_PER_PACKET ||
        packet.sample_timestamp != packet.sequence * BEAMIT_MIC_FRAMES_PER_PACKET) {
        throw std::invalid_argument("Audio sample timestamp does not match the BeamIt Microphone packet sequence");
    }

    BeamItMicPacket result{
        .header = {
            .version = BEAMIT_MIC_DRIVER_VERSION,
            .flags = packet.discontinuity ? BeamItMicPcmDiscontinuity : 0,
            .sequence = packet.sequence,
            .sample_timestamp = packet.sample_timestamp,
            .frame_count = BEAMIT_MIC_FRAMES_PER_PACKET,
            .reserved = 0,
        },
        .samples = std::vector<std::int16_t>(packet.samples.size()),
    };
    std::ranges::transform(packet.samples, result.samples.begin(), microphone_sample_to_pcm16);
    return result;
}

std::unique_ptr<BeamItMicrophoneDevice> make_beamit_microphone_device() {
    return std::make_unique<Win32BeamItMicrophoneDevice>();
}

BeamItMicrophoneOutput::BeamItMicrophoneOutput(media::AudioConfiguration configuration, ErrorCallback on_error,
                                               BeamItMicrophoneDeviceFactory device_factory, ReadyCallback on_ready)
    : on_error_(std::move(on_error)), on_ready_(std::move(on_ready)), device_factory_(std::move(device_factory)) {
    configuration.validate();
    if (configuration.sample_rate != BEAMIT_MIC_SAMPLE_RATE_HZ ||
        configuration.channels != BEAMIT_MIC_CHANNELS ||
        configuration.frame_size() != BEAMIT_MIC_FRAMES_PER_PACKET) {
        throw std::invalid_argument("BeamIt Microphone requires 48 kHz stereo PCM with 10 ms packets");
    }
    if (!device_factory_) throw std::invalid_argument("BeamIt Microphone device factory is empty");
    worker_ = std::thread([this] { run(); });
}

BeamItMicrophoneOutput::~BeamItMicrophoneOutput() { stop(); }

void BeamItMicrophoneOutput::submit(const AudioPcmPacket& packet) {
    auto converted = make_beamit_mic_packet(packet);
    std::lock_guard lock(mutex_);
    if (stopping_ || !error_.empty()) return;
    if (queue_.size() == max_microphone_output_packets) {
        queue_.pop_front();
        if (!queue_.empty()) queue_.front().header.flags |= BeamItMicPcmDiscontinuity;
        else converted.header.flags |= BeamItMicPcmDiscontinuity;
        spdlog::warn("BeamIt Microphone queue full; dropping oldest packet and preserving discontinuity");
    }
    queue_.push_back(std::move(converted));
    wake_.notify_one();
}

void BeamItMicrophoneOutput::stop() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        queue_.clear();
    }
    wake_.notify_one();
    if (worker_.joinable()) worker_.join();
}

std::string BeamItMicrophoneOutput::error() const {
    std::lock_guard lock(mutex_);
    return error_;
}

void BeamItMicrophoneOutput::report_error(std::string message) {
    ErrorCallback callback;
    {
        std::lock_guard lock(mutex_);
        if (!error_.empty()) return;
        error_ = message;
        queue_.clear();
        callback = on_error_;
    }
    spdlog::error("BeamIt Microphone output failed: {}", message);
    if (callback) callback(std::move(message));
}

void BeamItMicrophoneOutput::run() {
    std::unique_ptr<BeamItMicrophoneDevice> device;
    try {
        device = device_factory_();
        if (!device) throw std::runtime_error("BeamIt Microphone device factory returned no device");
        device->acquire();
    } catch (const std::exception& error) {
        report_error(error.what());
        return;
    }
    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            device->release();
            return;
        }
    }
    if (on_ready_) on_ready_();

    while (true) {
        BeamItMicPacket packet;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
            if (stopping_) break;
            packet = std::move(queue_.front());
            queue_.pop_front();
        }
        try {
            device->write(packet);
        } catch (const std::exception& error) {
            report_error(error.what());
            break;
        }
    }
    device->release();
}

} // namespace beamit::host
