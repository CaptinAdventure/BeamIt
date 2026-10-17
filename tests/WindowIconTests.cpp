#include "host/Clipboard.hpp"
#include "host/WindowIcon.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace beamit;

TEST_CASE("Icons convert to PNGs no larger than the requested size", "[icons]") {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const HICON icon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512)); // IDI_APPLICATION: shared system icon; must not be destroyed
    REQUIRE(icon);
    const auto full = host::png_from_icon(icon, host::max_window_icon_size);
    const auto small_icon = host::png_from_icon(icon, 16);
    const auto none = host::png_from_icon(nullptr, 16);
    if (SUCCEEDED(com)) CoUninitialize();

    REQUIRE(full);
    CHECK(full->width == static_cast<std::uint32_t>(GetSystemMetrics(SM_CXICON)));
    CHECK(full->width == full->height);
    const auto size = host::png_dimensions(full->png);
    REQUIRE(size);
    CHECK(size->first == full->width);
    CHECK(size->second == full->height);

    REQUIRE(small_icon);
    CHECK(small_icon->width == 16);
    CHECK(small_icon->height == 16);
    CHECK_FALSE(none);
}

TEST_CASE("Window icons are preferred when large enough", "[icons]") {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    WNDCLASSEXW window_class{.cbSize = sizeof(window_class), .lpfnWndProc = DefWindowProcW,
                             .hInstance = GetModuleHandleW(nullptr), .lpszClassName = L"BeamItIconTestWindow"};
    RegisterClassExW(&window_class);
    const HWND window = CreateWindowExW(0, window_class.lpszClassName, L"icon test", WS_OVERLAPPEDWINDOW, 0, 0, 100,
                                        100, nullptr, nullptr, window_class.hInstance, nullptr);
    REQUIRE(window);
    // No window or class icon: the executable's icon (if the shell finds one) is the only source.
    const auto without = host::window_icon(window, 48);
    SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(LoadIconW(nullptr, MAKEINTRESOURCEW(32512))));
    const auto with = host::window_icon(window, 16);
    DestroyWindow(window);
    UnregisterClassW(window_class.lpszClassName, window_class.hInstance);
    if (SUCCEEDED(com)) CoUninitialize();

    if (without) {
        CHECK(without->source == "executable");
        CHECK(without->width <= 48);
        CHECK(without->height <= 48);
    }
    REQUIRE(with);
    CHECK(with->source == "window");
    CHECK(with->width == 16);
    CHECK(with->height == 16);
    CHECK(host::png_dimensions(with->png));
}
