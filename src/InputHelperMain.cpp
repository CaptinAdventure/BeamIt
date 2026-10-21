#include "host/ElevatedInput.hpp"

#include <shellapi.h>
#include <spdlog/spdlog.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc{};
    auto** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        spdlog::error("Elevated input: cannot parse helper arguments (Win32 {})", GetLastError());
        return 1;
    }
    const auto result = beamit::host::input::run_input_helper(argc, argv);
    LocalFree(argv);
    return result;
}
