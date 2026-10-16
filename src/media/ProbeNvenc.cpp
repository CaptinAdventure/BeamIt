#include "ProbeNvenc.hpp"
#include "NvencApi.hpp"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <nvEncodeAPI.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check_hresult(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        throw std::runtime_error(fmt::format("{} failed (HRESULT 0x{:08X})",
                                            operation, static_cast<std::uint32_t>(result)));
    }
}

void check_nvenc(NVENCSTATUS status, const char* operation) {
    if (status != NV_ENC_SUCCESS) {
        throw std::runtime_error(fmt::format("{} failed (NVENC status {})",
                                            operation, static_cast<int>(status)));
    }
}

struct Driver {
    HMODULE module{};
    ~Driver() { if (module) FreeLibrary(module); }
    Driver() = default;
    Driver(const Driver&) = delete;
    Driver& operator=(const Driver&) = delete;
};

struct Session {
    NV_ENCODE_API_FUNCTION_LIST& api;
    void* handle{};
    ~Session() {
        if (handle) {
            const auto result = api.nvEncDestroyEncoder(handle);
            if (result != NV_ENC_SUCCESS) {
                spdlog::error("NVENC probe session cleanup failed (status {})", static_cast<int>(result));
            }
        }
    }
};

const char* format_name(NV_ENC_BUFFER_FORMAT format) {
    switch (format) {
    case NV_ENC_BUFFER_FORMAT_NV12: return "NV12";
    case NV_ENC_BUFFER_FORMAT_YV12: return "YV12";
    case NV_ENC_BUFFER_FORMAT_IYUV: return "IYUV";
    case NV_ENC_BUFFER_FORMAT_YUV444: return "YUV444 planar";
    case NV_ENC_BUFFER_FORMAT_YUV420_10BIT: return "YUV420 10-bit";
    case NV_ENC_BUFFER_FORMAT_YUV444_10BIT: return "YUV444 10-bit planar";
    case NV_ENC_BUFFER_FORMAT_ARGB: return "ARGB";
    case NV_ENC_BUFFER_FORMAT_ABGR: return "ABGR";
    case NV_ENC_BUFFER_FORMAT_ARGB10: return "ARGB10";
    case NV_ENC_BUFFER_FORMAT_ABGR10: return "ABGR10";
    default: return "other";
    }
}

std::string adapter_name(const wchar_t* text) {
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (!length) {
        check_hresult(HRESULT_FROM_WIN32(GetLastError()), "Convert adapter name");
    }
    std::string result(length, '\0');
    if (!WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), length, nullptr, nullptr)) {
        check_hresult(HRESULT_FROM_WIN32(GetLastError()), "Convert adapter name");
    }
    result.pop_back();
    return result;
}

} // namespace

void probe_nvenc() {
    Driver driver;
    NV_ENCODE_API_FUNCTION_LIST api{};
    check_hresult(load_nvenc_api(driver.module, api), "Load NVENC driver API");
    if (!api.nvEncOpenEncodeSessionEx || !api.nvEncDestroyEncoder ||
        !api.nvEncGetEncodeGUIDCount || !api.nvEncGetEncodeGUIDs ||
        !api.nvEncGetEncodeProfileGUIDCount || !api.nvEncGetEncodeProfileGUIDs ||
        !api.nvEncGetInputFormatCount || !api.nvEncGetInputFormats || !api.nvEncGetEncodeCaps) {
        throw std::runtime_error("NVENC driver returned an incomplete probe function table");
    }
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    check_hresult(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create DXGI factory");
    bool found = false;
    bool failed = false;
    for (UINT index = 0;; ++index) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        const auto result = factory->EnumAdapters1(index, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND) break;
        check_hresult(result, "Enumerate adapters");
        DXGI_ADAPTER_DESC1 desc{};
        check_hresult(adapter->GetDesc1(&desc), "Read adapter description");
        if (desc.VendorId != 0x10de || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        found = true;
        spdlog::info("NVENC adapter {}: {}, LUID {:08X}:{:08X}", index, adapter_name(desc.Description),
                     static_cast<UINT>(desc.AdapterLuid.HighPart), desc.AdapterLuid.LowPart);
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        const auto created = D3D11CreateDevice(
            adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr);
        if (FAILED(created)) {
            spdlog::error("  D3D11 device creation failed (HRESULT 0x{:08X})",
                          static_cast<std::uint32_t>(created));
            failed = true;
            continue;
        }
        Session session{api};
        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{};
        open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
        open.apiVersion = NVENCAPI_VERSION;
        open.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
        open.device = device.Get();
        const auto status = api.nvEncOpenEncodeSessionEx(&open, &session.handle);
        if (status != NV_ENC_SUCCESS) {
            spdlog::error("  Open NVENC D3D11 session failed (status {})", static_cast<int>(status));
            failed = true;
            continue;
        }
        std::uint32_t count{}, written{};
        check_nvenc(api.nvEncGetEncodeGUIDCount(session.handle, &count), "Count NVENC codecs");
        std::vector<GUID> codecs(count);
        if (count) {
            check_nvenc(api.nvEncGetEncodeGUIDs(session.handle, codecs.data(), count, &written),
                        "Enumerate NVENC codecs");
        }
        if (written > count) throw std::runtime_error("NVENC codec count exceeds buffer");
        codecs.resize(written);
        if (std::ranges::none_of(codecs, [](const GUID& guid) { return IsEqualGUID(guid, NV_ENC_CODEC_H264_GUID); })) {
            spdlog::info("  H.264 codec unsupported");
            continue;
        }
        check_nvenc(api.nvEncGetEncodeProfileGUIDCount(session.handle, NV_ENC_CODEC_H264_GUID, &count),
                    "Count H.264 profiles");
        std::vector<GUID> profiles(count);
        written = 0;
        if (count) {
            check_nvenc(api.nvEncGetEncodeProfileGUIDs(
                session.handle, NV_ENC_CODEC_H264_GUID, profiles.data(), count, &written),
                "Enumerate H.264 profiles");
        }
        if (written > count) throw std::runtime_error("NVENC profile count exceeds buffer");
        profiles.resize(written);
        bool high444 = false;
        for (const auto& guid : profiles) {
            high444 |= IsEqualGUID(guid, NV_ENC_H264_PROFILE_HIGH_444_GUID) != FALSE;
            spdlog::info("  H.264 profile {:08X}-{:04X}-{:04X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{}",
                guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1],
                guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7],
                IsEqualGUID(guid, NV_ENC_H264_PROFILE_HIGH_444_GUID) ? " [High 4:4:4 Predictive]" : "");
        }
        NV_ENC_CAPS_PARAM caps{};
        caps.version = NV_ENC_CAPS_PARAM_VER;
        caps.capsToQuery = NV_ENC_CAPS_SUPPORT_YUV444_ENCODE;
        int yuv444{};
        check_nvenc(api.nvEncGetEncodeCaps(session.handle, NV_ENC_CODEC_H264_GUID, &caps, &yuv444),
                    "Query H.264 YUV444 capability");
        check_nvenc(api.nvEncGetInputFormatCount(session.handle, NV_ENC_CODEC_H264_GUID, &count),
                    "Count H.264 input formats");
        std::vector<NV_ENC_BUFFER_FORMAT> formats(count);
        written = 0;
        if (count) {
            check_nvenc(api.nvEncGetInputFormats(
                session.handle, NV_ENC_CODEC_H264_GUID, formats.data(), count, &written),
                "Enumerate H.264 input formats");
        }
        if (written > count) throw std::runtime_error("NVENC format count exceeds buffer");
        formats.resize(written);
        for (const auto format : formats) {
            spdlog::info("  H.264 input {} (0x{:08X})", format_name(format), static_cast<unsigned>(format));
        }
        spdlog::info("  H.264 High444 profile: {}; YUV444 encode capability: {}; planar YUV444 input: {}",
                     high444 ? "YES" : "NO", yuv444 ? "YES" : "NO",
                     std::ranges::find(formats, NV_ENC_BUFFER_FORMAT_YUV444) != formats.end() ? "YES" : "NO");
    }
    if (!found) throw std::runtime_error("No NVIDIA hardware adapter found");
    if (failed) throw std::runtime_error("NVENC probing failed on one or more NVIDIA adapters; see errors above");
    spdlog::info("NVENC probe only: no encoder initialized, no capture/conversion/encode performed; D3D12 unchanged");
}
