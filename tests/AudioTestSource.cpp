#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <shellapi.h>
#include <wrl.h>

#include <cmath>
#include <cstdint>
#include <memory>

using Microsoft::WRL::ComPtr;

namespace {

struct CloseHandleDeleter {
    void operator()(void* handle) const { CloseHandle(handle); }
};
using Handle = std::unique_ptr<void, CloseHandleDeleter>;

HRESULT render(HANDLE stop) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(result)) return result;
    ComPtr<IMMDevice> device;
    result = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(result)) return result;
    ComPtr<IAudioClient> client;
    result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
    if (FAILED(result)) return result;
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = 2;
    format.nSamplesPerSec = 48000;
    format.wBitsPerSample = 32;
    format.nBlockAlign = 8;
    format.nAvgBytesPerSec = 48000 * 8;
    result = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 0, 0, &format, nullptr);
    if (FAILED(result)) return result;
    Handle ready(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!ready) return HRESULT_FROM_WIN32(GetLastError());
    result = client->SetEventHandle(ready.get());
    if (FAILED(result)) return result;
    UINT32 capacity{};
    result = client->GetBufferSize(&capacity);
    if (FAILED(result)) return result;
    ComPtr<IAudioRenderClient> renderer;
    result = client->GetService(IID_PPV_ARGS(&renderer));
    if (FAILED(result)) return result;
    result = client->Start();
    if (FAILED(result)) return result;
    const auto began = GetTickCount64();
    const HANDLE events[]{stop, ready.get()};
    std::uint64_t frame{};
    while (GetTickCount64() - began < 15'000) {
        const auto wait = WaitForMultipleObjects(2, events, FALSE, 1000);
        if (wait == WAIT_OBJECT_0) break;
        if (wait == WAIT_TIMEOUT) continue;
        if (wait != WAIT_OBJECT_0 + 1) { result = HRESULT_FROM_WIN32(GetLastError()); break; }
        UINT32 padding{};
        result = client->GetCurrentPadding(&padding);
        if (FAILED(result)) break;
        const auto available = capacity - padding;
        if (!available) continue;
        BYTE* data{};
        result = renderer->GetBuffer(available, &data);
        if (FAILED(result)) break;
        auto* pcm = reinterpret_cast<float*>(data);
        for (UINT32 index = 0; index < available; ++index, ++frame) {
            const auto sample = static_cast<float>(0.02 * std::sin(6.283185307179586 * 997 * frame / 48000));
            pcm[index * 2] = pcm[index * 2 + 1] = sample;
        }
        result = renderer->ReleaseBuffer(available, 0);
        if (FAILED(result)) break;
    }
    const auto stopped = client->Stop();
    return FAILED(result) ? result : stopped;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int count{};
    auto** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments || count != 2) {
        if (arguments) LocalFree(arguments);
        return 1;
    }
    Handle stop(OpenEventW(SYNCHRONIZE, FALSE, arguments[1]));
    LocalFree(arguments);
    if (!stop) return 2;
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) return 3;
    const auto result = render(stop.get());
    CoUninitialize();
    return FAILED(result) ? 4 : 0;
}
