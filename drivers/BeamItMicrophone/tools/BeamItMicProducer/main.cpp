#include "../../shared/BeamItMicIoctl.h"

#include <windows.h>

#include <cmath>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

int self_tests();
int device_tests();
int install_driver(const wchar_t* inf);
int remove_driver();

namespace {

struct HandleCloser {
    void operator()(HANDLE value) const noexcept {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }
};

using UniqueHandle = std::unique_ptr<std::remove_pointer_t<HANDLE>, HandleCloser>;

bool control(HANDLE handle, DWORD code, void* buffer, DWORD size, DWORD* bytes = nullptr) {
    DWORD ignored{};
    return DeviceIoControl(handle, code, buffer, size, buffer, size, bytes ? bytes : &ignored, nullptr) != FALSE;
}

void print_last_error(const char* action) {
    const auto error = GetLastError();
    std::fprintf(stderr, "%s failed (Win32 %lu)\n", action, error);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wcscmp(argv[1], L"--self-test") == 0) return self_tests();
    if (argc == 2 && std::wcscmp(argv[1], L"--verify-device") == 0) return device_tests();
    if (argc == 3 && std::wcscmp(argv[1], L"--install") == 0) return install_driver(argv[2]);
    if (argc == 2 && std::wcscmp(argv[1], L"--remove") == 0) return remove_driver();
    unsigned long seconds = 5;
    if (argc == 2) {
        wchar_t* end{};
        errno = 0;
        seconds = std::wcstoul(argv[1], &end, 10);
        if (errno || end == argv[1] || *end || seconds < 1 || seconds > 600) {
            std::fprintf(stderr, "Duration must be 1..600 seconds.\n");
            return 1;
        }
    } else if (argc != 1) {
        std::fprintf(stderr, "Usage: BeamItMicProducer [seconds | --self-test | --verify-device | --install INF | --remove]\n");
        return 1;
    }
    const UniqueHandle handle(CreateFileW(BEAMIT_MIC_USER_PATH, GENERIC_READ | GENERIC_WRITE,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!handle || handle.get() == INVALID_HANDLE_VALUE) {
        print_last_error("Open BeamIt microphone control device");
        return 1;
    }

    BeamItMicAcquireRequest acquire{
        .version = BEAMIT_MIC_DRIVER_VERSION,
        .process_id = GetCurrentProcessId(),
        .format = {.sample_rate_hz = BEAMIT_MIC_SAMPLE_RATE_HZ,
                   .channels = BEAMIT_MIC_CHANNELS,
                   .bits_per_sample = BEAMIT_MIC_BITS_PER_SAMPLE,
                   .frames_per_packet = BEAMIT_MIC_FRAMES_PER_PACKET},
    };
    if (!control(handle.get(), IOCTL_BEAMIT_MIC_ACQUIRE, &acquire, sizeof(acquire))) {
        print_last_error("Acquire BeamIt microphone lease");
        return 2;
    }

    struct Packet {
        BeamItMicPcmHeader header;
        INT16 samples[BEAMIT_MIC_FRAMES_PER_PACKET * BEAMIT_MIC_CHANNELS];
    } packet{};
    static_assert(sizeof(Packet) == sizeof(BeamItMicPcmHeader) + BEAMIT_MIC_PACKET_BYTES);
    auto* header = &packet.header;
    auto* samples = packet.samples;

    BeamItMicStats ready{};
    if (!control(handle.get(), IOCTL_BEAMIT_MIC_GET_STATS, &ready, sizeof(ready))) {
        print_last_error("Read capture state");
        return 3;
    }
    if (!ready.capturing) {
        std::fprintf(stderr, "Open BeamIt Microphone in a recording application before sending the test tone.\n");
        return 3;
    }
    const UniqueHandle timer(CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                   TIMER_MODIFY_STATE | SYNCHRONIZE));
    if (!timer) { print_last_error("Create pacing timer"); return 3; }
    LARGE_INTEGER frequency;
    LARGE_INTEGER began;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&began);

    std::uint64_t sequence{};
    std::uint64_t sample_time{};
    const auto packet_count = seconds * 1000 / BEAMIT_MIC_PACKET_DURATION_MS;
    for (unsigned long index = 0; index < packet_count; ++index) {
        header->version = BEAMIT_MIC_DRIVER_VERSION;
        header->flags = (index == 0) ? BeamItMicPcmDiscontinuity : 0;
        header->sequence = sequence++;
        header->sample_timestamp = sample_time;
        header->frame_count = BEAMIT_MIC_FRAMES_PER_PACKET;
        header->reserved = 0;
        sample_time += BEAMIT_MIC_FRAMES_PER_PACKET;

        for (std::uint32_t frame = 0; frame < BEAMIT_MIC_FRAMES_PER_PACKET; ++frame) {
            const auto phase = 6.283185307179586 * 997.0 * (header->sample_timestamp + frame) /
                               static_cast<double>(BEAMIT_MIC_SAMPLE_RATE_HZ);
            const auto value = static_cast<int16_t>(std::sin(phase) * 3500.0);
            samples[frame * BEAMIT_MIC_CHANNELS] = value;
            samples[frame * BEAMIT_MIC_CHANNELS + 1] = value;
        }

        if (!control(handle.get(), IOCTL_BEAMIT_MIC_WRITE_PCM, &packet, sizeof(packet))) {
            print_last_error("Write BeamIt microphone PCM");
            control(handle.get(), IOCTL_BEAMIT_MIC_RELEASE, nullptr, 0);
            return 3;
        }
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const auto deadline = began.QuadPart +
            static_cast<LONGLONG>(index + 1) * BEAMIT_MIC_PACKET_DURATION_MS * frequency.QuadPart / 1000;
        if (deadline > now.QuadPart) {
            LARGE_INTEGER due;
            due.QuadPart = -(deadline - now.QuadPart) * 10'000'000 / frequency.QuadPart;
            if (!due.QuadPart) due.QuadPart = -1;
            if (!SetWaitableTimer(timer.get(), &due, 0, nullptr, nullptr, FALSE) ||
                WaitForSingleObject(timer.get(), INFINITE) != WAIT_OBJECT_0) {
                print_last_error("Wait for packet deadline");
                return 3;
            }
        }
    }

    BeamItMicStats stats{};
    DWORD returned{};
    if (!control(handle.get(), IOCTL_BEAMIT_MIC_GET_STATS, &stats, sizeof(stats), &returned)) {
        print_last_error("Read microphone statistics");
        return 4;
    }
    if (returned != sizeof(stats)) { std::fprintf(stderr, "Unexpected statistics size.\n"); return 4; }
    std::printf("owner=%lu leased=%lu packets=%llu frames=%llu rejected=%llu underruns=%llu overruns=%llu queued=%lu\n",
        stats.owner_process_id, stats.leased, stats.packets_accepted, stats.frames_accepted,
        stats.packets_rejected, stats.underrun_frames, stats.overrun_frames, stats.queued_frames);

    if (!control(handle.get(), IOCTL_BEAMIT_MIC_RELEASE, nullptr, 0)) {
        print_last_error("Release BeamIt microphone lease");
        return 4;
    }
    return 0;
}
