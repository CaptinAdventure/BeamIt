#include "../../shared/BeamItMicIoctl.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cwchar>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <atomic>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
void check_hresult(HRESULT result, const char* action) {
    if (FAILED(result)) {
        char message[256]{};
        std::snprintf(message, sizeof(message), "%s (HRESULT 0x%08lX)", action, static_cast<unsigned long>(result));
        throw std::runtime_error(message);
    }
}
void require(bool condition, const char* action) {
    if (!condition) throw std::runtime_error(action);
}
struct HandleCloser {
    void operator()(void* handle) const { if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
using Handle = std::unique_ptr<void, HandleCloser>;
Handle open_control() {
    Handle handle(CreateFileW(BEAMIT_MIC_USER_PATH, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr));
    require(handle.get() != INVALID_HANDLE_VALUE, "Open control device");
    return handle;
}
bool ioctl(HANDLE handle, DWORD code, void* data = nullptr, DWORD length = 0) {
    DWORD bytes{};
    return DeviceIoControl(handle, code, data, length, nullptr, 0, &bytes, nullptr) != FALSE;
}
BeamItMicAcquireRequest acquire_request() {
    return {BEAMIT_MIC_DRIVER_VERSION, GetCurrentProcessId(),
            {BEAMIT_MIC_SAMPLE_RATE_HZ, BEAMIT_MIC_CHANNELS,
             BEAMIT_MIC_BITS_PER_SAMPLE, BEAMIT_MIC_FRAMES_PER_PACKET}};
}

ComPtr<IMMDevice> find_microphone() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    check_hresult(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)),
                  "Create device enumerator");
    ComPtr<IMMDeviceCollection> devices;
    check_hresult(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &devices), "Enumerate capture endpoints");
    UINT count{};
    check_hresult(devices->GetCount(&count), "Count capture endpoints");
    ComPtr<IMMDevice> selected;
    for (UINT index = 0; index < count; ++index) {
        ComPtr<IMMDevice> device;
        check_hresult(devices->Item(index, &device), "Read capture endpoint");
        ComPtr<IPropertyStore> properties;
        check_hresult(device->OpenPropertyStore(STGM_READ, &properties), "Open endpoint properties");
        PROPVARIANT name;
        PropVariantInit(&name);
        const auto result = properties->GetValue(PKEY_Device_FriendlyName, &name);
        const bool matches = SUCCEEDED(result) && name.vt == VT_LPWSTR && name.pwszVal &&
            std::wcsstr(name.pwszVal, L"BeamIt Microphone");
        PropVariantClear(&name);
        check_hresult(result, "Read endpoint name");
        if (matches) {
            require(!selected, "More than one BeamIt Microphone endpoint; remove duplicate installations");
            selected = device;
        }
    }
    require(selected != nullptr, "No active BeamIt Microphone endpoint (no fallback to a physical microphone)");
    return selected;
}

struct StopCapture {
    IAudioClient* client;
    ~StopCapture() {
        const auto result = client->Stop();
        if (FAILED(result)) std::fprintf(stderr, "Capture cleanup failed (HRESULT 0x%08lX).\n",
                                         static_cast<unsigned long>(result));
    }
};

double read_energy(IAudioCaptureClient* capture, HANDLE event, UINT64 duration_ms, UINT64& frames_read) {
    double energy{};
    frames_read = 0;
    const auto deadline = GetTickCount64() + duration_ms;
    while (GetTickCount64() < deadline) {
        const auto wait = WaitForSingleObject(event, 100);
        require(wait == WAIT_OBJECT_0 || wait == WAIT_TIMEOUT, "Wait for capture notification");
        UINT32 available{};
        check_hresult(capture->GetNextPacketSize(&available), "Read next capture packet size");
        while (available) {
            BYTE* data{};
            UINT32 frames{};
            DWORD flags{};
            check_hresult(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr), "Get captured PCM");
            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data) {
                const auto samples = reinterpret_cast<const INT16*>(data);
                for (UINT32 index = 0; index < frames * BEAMIT_MIC_CHANNELS; ++index) {
                    const auto normalized = samples[index] / 32768.0;
                    energy += normalized * normalized;
                }
            }
            frames_read += frames;
            check_hresult(capture->ReleaseBuffer(frames), "Release captured PCM");
            check_hresult(capture->GetNextPacketSize(&available), "Drain capture packets");
        }
    }
    require(frames_read >= BEAMIT_MIC_SAMPLE_RATE_HZ * duration_ms / 2000, "Capture clock/notifications did not advance");
    return energy / (static_cast<double>(frames_read) * BEAMIT_MIC_CHANNELS);
}

void test_device() {
    const auto device = find_microphone();
    ComPtr<IAudioClient> client;
    check_hresult(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client), "Activate BeamIt Microphone");
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = BEAMIT_MIC_CHANNELS;
    format.nSamplesPerSec = BEAMIT_MIC_SAMPLE_RATE_HZ;
    format.wBitsPerSample = BEAMIT_MIC_BITS_PER_SAMPLE;
    format.nBlockAlign = BEAMIT_MIC_BYTES_PER_FRAME;
    format.nAvgBytesPerSec = BEAMIT_MIC_SAMPLE_RATE_HZ * BEAMIT_MIC_BYTES_PER_FRAME;
    check_hresult(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 0, 0, &format, nullptr), "Initialize capture format");
    Handle event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    require(event != nullptr, "Create capture event");
    check_hresult(client->SetEventHandle(event.get()), "Set capture event");
    ComPtr<IAudioCaptureClient> capture;
    check_hresult(client->GetService(IID_PPV_ARGS(&capture)), "Get capture service");
    check_hresult(client->Start(), "Start capture");
    const StopCapture stop{client.Get()};
    auto owner = open_control();
    auto rival = open_control();
    auto request = acquire_request();
    require(ioctl(owner.get(), IOCTL_BEAMIT_MIC_ACQUIRE, &request, sizeof(request)), "Acquire producer");
    require(!ioctl(rival.get(), IOCTL_BEAMIT_MIC_ACQUIRE, &request, sizeof(request)), "Second handle must not acquire");
    require(!ioctl(rival.get(), IOCTL_BEAMIT_MIC_RESET), "Non-owner reset must fail");
    UINT64 frames{};
    const auto silent = read_energy(capture.Get(), event.get(), 250, frames);
    require(silent == 0, "Empty producer queue must return silence");
    struct Packet {
        BeamItMicPcmHeader header;
        INT16 samples[BEAMIT_MIC_FRAMES_PER_PACKET * BEAMIT_MIC_CHANNELS];
    } packet{};
    packet.header = {BEAMIT_MIC_DRIVER_VERSION, BeamItMicPcmDiscontinuity, 0, 0, BEAMIT_MIC_FRAMES_PER_PACKET, 0};
    for (auto& sample : packet.samples) sample = 3000;
    auto invalid = packet;
    invalid.header.reserved = 1;
    require(!ioctl(owner.get(), IOCTL_BEAMIT_MIC_WRITE_PCM, &invalid, sizeof(invalid)), "Reserved bits rejected");
    require(!ioctl(owner.get(), IOCTL_BEAMIT_MIC_WRITE_PCM, &packet, sizeof(packet) - 1), "Short packet rejected");

    std::atomic<bool> running{true};
    std::atomic<bool> write_failed{};
    {
        std::jthread producer([&] {
            while (running) {
                if (!ioctl(owner.get(), IOCTL_BEAMIT_MIC_WRITE_PCM, &packet, sizeof(packet))) {
                    write_failed = true;
                    break;
                }
                packet.header.flags = 0;
                ++packet.header.sequence;
                packet.header.sample_timestamp += BEAMIT_MIC_FRAMES_PER_PACKET;
                Sleep(BEAMIT_MIC_PACKET_DURATION_MS);
            }
        });
        // Ensure exceptions cannot keep the bounded test producer alive.
        struct StopProducer {
            std::atomic<bool>& running;
            ~StopProducer() { running = false; }
        } stop_producer{running};
        const auto signal = read_energy(capture.Get(), event.get(), 750, frames);
        require(signal > 0.001, "Injected PCM did not reach WASAPI capture");
    }
    require(!write_failed, "PCM producer failed");
    require(!ioctl(owner.get(), IOCTL_BEAMIT_MIC_WRITE_PCM, &invalid, sizeof(invalid)), "Malformed packet still rejected");
    owner.reset(); // IRP_MJ_CLEANUP must release the lease without a release IOCTL.
    require(ioctl(rival.get(), IOCTL_BEAMIT_MIC_ACQUIRE, &request, sizeof(request)), "Handle cleanup releases ownership");
    require(!ioctl(rival.get(), IOCTL_BEAMIT_MIC_WRITE_PCM, &packet, sizeof(packet)), "New owner requires discontinuity");
    read_energy(capture.Get(), event.get(), 100, frames); // Drain the audio engine's already-delivered buffers.
    require(read_energy(capture.Get(), event.get(), 250, frames) == 0, "Old owner's samples leaked after cleanup");
    require(ioctl(rival.get(), IOCTL_BEAMIT_MIC_RELEASE), "Release second producer");
    check_hresult(client->Stop(), "Stop capture");
    check_hresult(client->Reset(), "Reset capture");
    check_hresult(client->Start(), "Restart capture");
    require(read_energy(capture.Get(), event.get(), 250, frames) == 0, "Capture restart must supply silence");
}
} // namespace

int device_tests() {
    const auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(apartment)) { std::fprintf(stderr, "COM initialization failed.\n"); return 1; }
    int result{};
    try {
        test_device();
        std::printf("Device checks passed: WASAPI silence/PCM, validation, ownership, cleanup and restart.\n");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Device test failed: %s\n", error.what());
        result = 1;
    }
    CoUninitialize();
    return result;
}
