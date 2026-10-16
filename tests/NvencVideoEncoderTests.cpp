#include "beamit/media/NvencVideoEncoder.hpp"
#include "beamit/media/AutoVideoEncoder.hpp"

#include <catch2/catch_test_macros.hpp>
#include <dxgi1_2.h>

#include <limits>
#include <thread>
#include <type_traits>

using namespace beamit::media;
using namespace std::chrono_literals;

static_assert(std::is_final_v<NvencVideoEncoder>);
static_assert(std::is_base_of_v<Encoder, NvencVideoEncoder>);

TEST_CASE("NVENC validates configuration without loading a driver", "[encoder][nvenc]") {
    NvencVideoEncoder encoder;
    REQUIRE_FALSE(encoder.is_running());
    REQUIRE_FALSE(encoder.start());
    REQUIRE(FAILED(encoder.last_error()));
    REQUIRE_FALSE(encoder.configure({}));
    REQUIRE(encoder.last_error() == E_INVALIDARG);
    EncoderOptions options{64, 64, 60, 12000, VideoCodec::H264};
    auto invalid = options;
    invalid.codec = VideoCodec::H265;
    REQUIRE_FALSE(encoder.configure(invalid));
    invalid = options;
    invalid.target_bitrate_kbps = std::numeric_limits<std::uint32_t>::max();
    REQUIRE_FALSE(encoder.configure(invalid));
    REQUIRE(encoder.configure(options));
    REQUIRE(encoder.last_error() == S_OK);
    REQUIRE(encoder.start());
    REQUIRE(encoder.is_running());
    REQUIRE_FALSE(encoder.try_receive());
    REQUIRE(encoder.try_repeat_frame() == EncoderSubmitResult::Failed);
    REQUIRE(encoder.last_error() == E_UNEXPECTED);
    REQUIRE(encoder.configure(options));
    REQUIRE(encoder.start());
    REQUIRE(encoder.try_submit({}) == EncoderSubmitResult::Failed);
    REQUIRE(encoder.last_error() == E_INVALIDARG);
    encoder.stop();
    encoder.stop();
    REQUIRE_FALSE(encoder.is_running());
}

TEST_CASE("NVENC D3D11 submits repeats and retains borrowed surfaces", "[.nvenc-hardware]") {
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    REQUIRE(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0;; ++index) {
        const auto result = factory->EnumAdapters1(index, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND) SKIP("No NVIDIA adapter");
        REQUIRE(SUCCEEDED(result));
        DXGI_ADAPTER_DESC1 desc{};
        REQUIRE(SUCCEEDED(adapter->GetDesc1(&desc)));
        if (desc.VendorId == 0x10de && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) break;
        adapter.Reset();
    }

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    REQUIRE(SUCCEEDED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr)));
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 64;
    desc.MipLevels = desc.ArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    std::vector<std::uint32_t> pixels(64 * 64, 0xff20a0f0);
    D3D11_SUBRESOURCE_DATA data{pixels.data(), 64 * 4, 0};
    CapturedFrame frame;
    REQUIRE(SUCCEEDED(device->CreateTexture2D(&desc, &data, &frame.texture)));
    frame.captured_at = std::chrono::steady_clock::now();
    frame.lifetime = std::make_shared<int>(1);
    const std::weak_ptr<void> lease = frame.lifetime;
    std::unique_ptr<Encoder> selected;
    SECTION("explicit NVENC") { selected = std::make_unique<NvencVideoEncoder>(); }
    SECTION("automatic H.264 selects NVENC 4:4:4") { selected = std::make_unique<AutoVideoEncoder>(); }
    auto& encoder = *selected;
    REQUIRE(encoder.configure({64, 64, 60, 12000, VideoCodec::H264}));
    REQUIRE(encoder.start());
    REQUIRE(encoder.try_submit(frame) == EncoderSubmitResult::Accepted);
    REQUIRE(encoder.backend_name() == L"NVENC DirectX 11");
    REQUIRE(encoder.try_submit(frame) == EncoderSubmitResult::WouldBlock);
    REQUIRE(encoder.try_repeat_frame() == EncoderSubmitResult::WouldBlock);
    frame.lifetime.reset();
    REQUIRE_FALSE(lease.expired());
    auto receive = [&] {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        std::optional<EncodedPacket> packet;
        while (!(packet = encoder.try_receive()) && std::chrono::steady_clock::now() < deadline) {
            REQUIRE(SUCCEEDED(encoder.last_error()));
            std::this_thread::sleep_for(1ms);
        }
        REQUIRE(packet.has_value());
        REQUIRE_FALSE(packet->data.empty());
        REQUIRE(packet->codec == VideoCodec::H264);
        REQUIRE(packet->captured_at == frame.captured_at);
        return *packet;
    };
    REQUIRE(receive().key_frame);
    REQUIRE(lease.expired());
    REQUIRE(encoder.try_repeat_frame() == EncoderSubmitResult::Accepted);
    REQUIRE_FALSE(receive().key_frame);
    REQUIRE(encoder.try_repeat_frame() == EncoderSubmitResult::Accepted);
    encoder.stop();
    REQUIRE_FALSE(encoder.is_running());
    REQUIRE(SUCCEEDED(encoder.last_error()));
    REQUIRE(encoder.configure({64, 64, 60, 12000, VideoCodec::H264}));
    REQUIRE(encoder.start());
    REQUIRE(encoder.try_submit(frame) == EncoderSubmitResult::Accepted);
    REQUIRE(receive().key_frame);
    encoder.stop();
    REQUIRE(SUCCEEDED(encoder.last_error()));
}

TEST_CASE("automatic H.264 falls back to D3D12 on a non-NVIDIA adapter", "[.auto-hardware]") {
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    REQUIRE(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0;; ++index) {
        const auto result = factory->EnumAdapters1(index, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND) SKIP("No non-NVIDIA hardware adapter");
        REQUIRE(SUCCEEDED(result));
        DXGI_ADAPTER_DESC1 desc{};
        REQUIRE(SUCCEEDED(adapter->GetDesc1(&desc)));
        if (desc.VendorId != 0x10de && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) break;
        adapter.Reset();
    }
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    REQUIRE(SUCCEEDED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr)));
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 192;
    desc.MipLevels = desc.ArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    std::vector<std::uint32_t> pixels(192 * 192, 0xff20a0f0);
    D3D11_SUBRESOURCE_DATA data{pixels.data(), 192 * 4, 0};
    CapturedFrame frame;
    REQUIRE(SUCCEEDED(device->CreateTexture2D(&desc, &data, &frame.texture)));
    AutoVideoEncoder encoder;
    REQUIRE(encoder.configure({192, 192, 30, 12000, VideoCodec::H264}));
    REQUIRE(encoder.start());
    const auto submitted = encoder.try_submit(frame);
    REQUIRE(encoder.backend_name() == L"Direct3D 12 Video Encode");
    if (submitted == EncoderSubmitResult::Accepted) {
        std::optional<EncodedPacket> packet;
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!(packet = encoder.try_receive()) && std::chrono::steady_clock::now() < deadline) {
            REQUIRE(SUCCEEDED(encoder.last_error()));
            std::this_thread::sleep_for(1ms);
        }
        REQUIRE(packet.has_value());
        REQUIRE(packet->codec == VideoCodec::H264);
    } else {
        REQUIRE(submitted == EncoderSubmitResult::Failed);
        REQUIRE(FAILED(encoder.last_error()));
    }
    encoder.stop();
}
