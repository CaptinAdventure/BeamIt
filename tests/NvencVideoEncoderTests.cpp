#include "beamit/media/NvencVideoEncoder.hpp"
#include "beamit/media/AutoVideoEncoder.hpp"

#include <catch2/catch_test_macros.hpp>
#include <dxgi1_2.h>

#include <limits>
#include <filesystem>
#include <fstream>
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

TEST_CASE("Bitrate updates before the first picture only change the options", "[encoder][nvenc]") {
    EncoderOptions options{64, 64, 60, 12000, VideoCodec::H264};
    NvencVideoEncoder nvenc;
    REQUIRE_FALSE(nvenc.try_update_bitrate(24000, 0));
    REQUIRE(nvenc.configure(options));
    REQUIRE_FALSE(nvenc.try_update_bitrate(0, 0));
    REQUIRE_FALSE(nvenc.try_update_bitrate(std::numeric_limits<std::uint32_t>::max(), 0));
    REQUIRE(nvenc.try_update_bitrate(24000, 0));
    REQUIRE(nvenc.last_error() == S_OK);

    AutoVideoEncoder automatic;
    REQUIRE_FALSE(automatic.try_update_bitrate(24000, 0));
    REQUIRE(automatic.configure(options));
    REQUIRE(automatic.try_update_bitrate(24000, 0));
    REQUIRE(automatic.last_error() == S_OK);
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

TEST_CASE("NVENC synthetic desktop GOP quality diagnostic", "[.nvenc-quality]") {
        std::wstring directory(32768, L'\0');
        const auto length = GetEnvironmentVariableW(L"BEAMIT_NVENC_DIAGNOSTIC_DIR", directory.data(),
                                                   static_cast<DWORD>(directory.size()));
        if (!length) SKIP("Set BEAMIT_NVENC_DIAGNOSTIC_DIR to an existing empty output directory");
        REQUIRE(length < directory.size());
        directory.resize(length);
        const std::filesystem::path output(directory);
        REQUIRE(std::filesystem::is_directory(output));
        REQUIRE(std::filesystem::is_empty(output));

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

        constexpr unsigned width = 1920, height = 1080;
        std::vector<std::uint32_t> pixels(width * height);
        std::uint32_t random = 0x12345678;
        for (unsigned y = 0; y < height; ++y) {
            for (unsigned x = 0; x < width; ++x) {
                random ^= random << 13;
                random ^= random >> 17;
                random ^= random << 5;
                // Flat UI panels, dense glyph-like edges, gradients and a textured image panel.
                const bool glyph = y % 20 >= 5 && y % 20 < 15 && x % 9 < 6 &&
                                   ((x / 3 + y / 2 + x / 90) % 5 < 2);
                const unsigned noise = random & 31;
                const unsigned r = x < width / 2 ? (glyph ? 230 : 32) : (x * 127 / width + noise);
                const unsigned g = x < width / 2 ? (glyph ? 220 : 36) : (y * 191 / height + noise);
                const unsigned b = x < width / 2 ? (glyph ? 210 : 40) : ((x + y) * 127 / (width + height) + noise);
                pixels[y * width + x] = 0xff000000u | (r << 16) | (g << 8) | b;
            }
        }
        std::ofstream reference(output / "reference.ppm", std::ios::binary);
        REQUIRE(reference.is_open());
        reference << "P6\n" << width << ' ' << height << "\n255\n";
        for (const auto pixel : pixels) {
            const char rgb[]{static_cast<char>(pixel >> 16), static_cast<char>(pixel >> 8), static_cast<char>(pixel)};
            reference.write(rgb, sizeof(rgb));
        }
        reference.close();
        REQUIRE(reference.good());

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = desc.ArraySize = 1;
        desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        const D3D11_SUBRESOURCE_DATA data{pixels.data(), width * 4, 0};
        CapturedFrame frame;
        REQUIRE(SUCCEEDED(device->CreateTexture2D(&desc, &data, &frame.texture)));

        for (const unsigned fps : {60u, 30u}) {
            const auto gop_length = key_frame_interval(fps);
            const auto frames = gop_length * 2 + 1;
            NvencVideoEncoder encoder;
            REQUIRE(encoder.configure({width, height, fps, 48000, VideoCodec::H264, 96000}));
            REQUIRE(encoder.start());
            const auto name = "nvenc-" + std::to_string(fps);
            std::ofstream bitstream(output / (name + ".h264"), std::ios::binary);
            std::ofstream metrics(output / (name + ".csv"));
            REQUIRE(bitstream.is_open());
            REQUIRE(metrics.is_open());
            metrics << "frame,gop_position,key_frame,bytes,encode_ms\n";
            const auto started = std::chrono::steady_clock::now();
            for (unsigned index = 0; index < frames; ++index) {
                std::this_thread::sleep_until(started + std::chrono::nanoseconds(1'000'000'000ULL * index / fps));
                frame.captured_at = std::chrono::steady_clock::now();
                const auto submit = index == 0 ? encoder.try_submit(frame) : encoder.try_repeat_frame();
                REQUIRE(submit == EncoderSubmitResult::Accepted);
                std::optional<EncodedPacket> packet;
                const auto deadline = std::chrono::steady_clock::now() + 5s;
                while (!(packet = encoder.try_receive()) && std::chrono::steady_clock::now() < deadline) {
                    REQUIRE(SUCCEEDED(encoder.last_error()));
                    std::this_thread::sleep_for(1ms);
                }
                REQUIRE(packet);
                REQUIRE(packet->key_frame == (index % gop_length == 0));
                const auto elapsed = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - frame.captured_at).count();
                bitstream.write(reinterpret_cast<const char*>(packet->data.data()),
                                static_cast<std::streamsize>(packet->data.size()));
                metrics << index << ',' << index % gop_length << ',' << packet->key_frame << ','
                        << packet->data.size() << ',' << elapsed << '\n';
            }
            encoder.stop();
            bitstream.close();
            metrics.close();
            REQUIRE(bitstream.good());
            REQUIRE(metrics.good());
        }
    }
