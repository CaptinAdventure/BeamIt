#include <windows.h>
#include <setupapi.h>
#include <newdev.h>
#include <initguid.h>
#include <devpkey.h>

#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <vector>

namespace {
constexpr GUID media_class{0x4d36e96c, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
constexpr wchar_t hardware_id[] = L"Root\\BeamItMicrophone";
struct DeviceList {
    HDEVINFO value{INVALID_HANDLE_VALUE};
    ~DeviceList() { if (value != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(value); }
};

int error(const char* operation) {
    std::fprintf(stderr, "%s failed (Win32 %lu). Installation/removal requires an elevated terminal.\n",
                 operation, GetLastError());
    return 1;
}

// Returns 1 if found, 0 if absent, -1 on enumeration failure.
int find_device(HDEVINFO devices, SP_DEVINFO_DATA& found) {
    for (DWORD index = 0;; ++index) {
        SP_DEVINFO_DATA device{sizeof(device)};
        if (!SetupDiEnumDeviceInfo(devices, index, &device)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) return 0;
            error("Enumerate root devices");
            return -1;
        }
        DWORD type{}, size{};
        if (!SetupDiGetDeviceRegistryPropertyW(devices, &device, SPDRP_HARDWAREID, &type, nullptr, 0, &size)) {
            if (GetLastError() == ERROR_INVALID_DATA) continue;
            if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) { error("Query hardware ID size"); return -1; }
        }
        if (type != REG_MULTI_SZ || size < 2 * sizeof(wchar_t)) continue;
        std::vector<wchar_t> ids(size / sizeof(wchar_t) + 2, 0);
        if (!SetupDiGetDeviceRegistryPropertyW(devices, &device, SPDRP_HARDWAREID, &type,
            reinterpret_cast<PBYTE>(ids.data()), size, nullptr)) { error("Read hardware ID"); return -1; }
        for (auto id = ids.data(); *id; id += std::wcslen(id) + 1) {
            if (_wcsicmp(id, hardware_id) == 0) { found = device; return 1; }
        }
    }
}
} // namespace

int install_driver(const wchar_t* inf_path) {
    std::vector<wchar_t> full_path(32768);
    const auto length = GetFullPathNameW(inf_path, static_cast<DWORD>(full_path.size()), full_path.data(), nullptr);
    if (!length || length >= full_path.size()) return error("Resolve INF path");
    if (GetFileAttributesW(full_path.data()) == INVALID_FILE_ATTRIBUTES) return error("Find INF");
    const DeviceList devices{SetupDiGetClassDevsW(nullptr, L"ROOT", nullptr, DIGCF_ALLCLASSES)};
    if (devices.value == INVALID_HANDLE_VALUE) return error("Enumerate root device list");
    SP_DEVINFO_DATA device{sizeof(device)};
    const auto exists = find_device(devices.value, device);
    if (exists < 0) return 1;
    bool created{};
    if (!exists) {
        if (!SetupDiCreateDeviceInfoW(devices.value, L"BeamItMicrophone", &media_class, L"BeamIt Microphone",
                                      nullptr, DICD_GENERATE_ID, &device)) return error("Create root device");
        constexpr wchar_t ids[] = L"Root\\BeamItMicrophone\0";
        if (!SetupDiSetDeviceRegistryPropertyW(devices.value, &device, SPDRP_HARDWAREID,
            reinterpret_cast<const BYTE*>(ids), sizeof(ids))) return error("Set hardware ID");
        if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, devices.value, &device)) return error("Register root device");
        created = true;
    }
    BOOL reboot{};
    if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, hardware_id, full_path.data(), 0, &reboot)) {
        error("Install signed driver package");
        if (created) {
            BOOL ignored{};
            if (!DiUninstallDevice(GetDesktopWindow(), devices.value, &device, 0, &ignored)) error("Roll back new root device");
        }
        return 1;
    }
    std::printf("BeamIt Microphone package installed. Reboot required: %s\n", reboot ? "yes" : "no");
    return 0;
}

int remove_driver() {
    const DeviceList devices{SetupDiGetClassDevsW(nullptr, L"ROOT", nullptr, DIGCF_ALLCLASSES)};
    if (devices.value == INVALID_HANDLE_VALUE) return error("Enumerate root device list");
    SP_DEVINFO_DATA device{sizeof(device)};
    const auto exists = find_device(devices.value, device);
    if (exists < 0) return 1;
    if (!exists) { std::fprintf(stderr, "BeamIt Microphone root device was not found.\n"); return 1; }
    wchar_t inf[260]{};
    DEVPROPTYPE type{};
    if (!SetupDiGetDevicePropertyW(devices.value, &device, &DEVPKEY_Device_DriverInfPath, &type,
        reinterpret_cast<PBYTE>(inf), sizeof(inf), nullptr, 0) || type != DEVPROP_TYPE_STRING)
        return error("Read installed INF name");
    BOOL reboot{};
    if (!DiUninstallDevice(GetDesktopWindow(), devices.value, &device, 0, &reboot)) return error("Remove root device");
    if (!SetupUninstallOEMInfW(inf, 0, nullptr)) {
        const auto status = error("Remove driver-store package");
        std::fwprintf(stderr, L"Device removed; remaining package: %ls. Close all microphone handles and retry using pnputil /delete-driver %ls.\n",
                      inf, inf);
        return status;
    }
    std::printf("BeamIt Microphone removed. Reboot required: %s\n", reboot ? "yes" : "no");
    return 0;
}
