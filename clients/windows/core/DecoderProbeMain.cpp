#include "DecoderProbe.hpp"

#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

unsigned dimension(std::wstring_view text) {
    if (text.empty() || text.size() > 5) throw std::invalid_argument("Invalid fixture dimension");
    unsigned value{};
    for (const auto character : text) {
        if (character < L'0' || character > L'9') throw std::invalid_argument("Invalid fixture dimension");
        value = value * 10 + static_cast<unsigned>(character - L'0');
    }
    if (value == 0 || value > 16384) throw std::invalid_argument("Fixture dimensions must be 1..16384");
    return value;
}

std::vector<std::byte> read_fixture(std::filesystem::path const& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("Could not open fixture");
    const auto length = input.tellg();
    if (length <= 0 || length > 16 * 1024 * 1024) throw std::runtime_error("Fixture must be 1 byte..16 MiB");
    std::vector<std::byte> bytes(static_cast<std::size_t>(length));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(length))) {
        throw std::runtime_error("Could not read complete fixture");
    }
    return bytes;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) {
        std::cerr << "Could not initialize COM for decoder probe\n";
        return 1;
    }
    int exit_code = 1;
    try {
        if (argc == 2 && std::wstring_view(argv[1]) == L"--list") {
            std::wcout << beamit::client::decoder_inventory_report();
            exit_code = 0;
        } else {
            if (argc != 5 || (std::wstring_view(argv[1]) != L"h264" && std::wstring_view(argv[1]) != L"h265")) {
                throw std::invalid_argument("Usage: BeamItDecoderProbe --list | <h264|h265> <width> <height> <IDR.annexb>");
            }
            const auto bytes = read_fixture(argv[4]);
            const auto result = beamit::client::probe_decoder(std::wstring_view(argv[1]) == L"h265",
                dimension(argv[2]), dimension(argv[3]), bytes);
            std::wcout << result.report;
            exit_code = result.decoded ? 0 : 2;
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
    }
    CoUninitialize();
    return exit_code;
}
