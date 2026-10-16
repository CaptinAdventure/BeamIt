#pragma once

#include <windows.h>

#include <string>
#include <vector>

struct ApplicationWindow {
    HWND hwnd{};
    DWORD pid{};
    std::wstring title;
    bool primary_candidate{};
    unsigned long long area{};
};

[[nodiscard]] std::vector<ApplicationWindow> application_windows();
[[nodiscard]] HWND select_application_window(DWORD pid);
