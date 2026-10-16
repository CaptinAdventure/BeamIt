#pragma once

#include <Windows.h>
#include <nvEncodeAPI.h>
#include <spdlog/spdlog.h>

#include <cstdint>

// The caller owns the module, including on failure, and must keep it alive through session destruction.
inline HRESULT load_nvenc_api(HMODULE& module, NV_ENCODE_API_FUNCTION_LIST& api) {
    module = LoadLibraryExW(L"nvEncodeAPI64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        const auto error = GetLastError();
        spdlog::error("Cannot load nvEncodeAPI64.dll from System32 (Win32 {})", error);
        return HRESULT_FROM_WIN32(error);
    }
    using GetVersion = NVENCSTATUS (NVENCAPI*)(std::uint32_t*);
    using CreateInstance = NVENCSTATUS (NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
    const auto version = reinterpret_cast<GetVersion>(
        GetProcAddress(module, "NvEncodeAPIGetMaxSupportedVersion"));
    const auto create = reinterpret_cast<CreateInstance>(
        GetProcAddress(module, "NvEncodeAPICreateInstance"));
    if (!version || !create) {
        spdlog::error("NVIDIA driver is missing NVENC version/function-list entry points");
        return E_NOINTERFACE;
    }
    std::uint32_t maximum{};
    auto status = version(&maximum);
    if (status != NV_ENC_SUCCESS) {
        spdlog::error("Query NVENC driver API version failed (status {})", static_cast<int>(status));
        return E_FAIL;
    }
    spdlog::info("NVENC API: headers {}.{}, driver maximum {}.{}",
                 NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION, maximum >> 4, maximum & 0xf);
    if (maximum < ((NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION)) {
        spdlog::error("NVIDIA driver API is older than the SDK headers; use a compatible driver/header pair "
                      "(no struct-version downgrading is attempted)");
        return E_NOINTERFACE;
    }
    api.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    status = create(&api);
    if (status != NV_ENC_SUCCESS) {
        spdlog::error("Create NVENC function list failed (status {})", static_cast<int>(status));
        return E_FAIL;
    }
    return S_OK;
}
