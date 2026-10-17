#include "host/WindowTracker.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace beamit::host;

namespace {
const HWND main_window = reinterpret_cast<HWND>(0x1000);
const HWND meeting = reinterpret_cast<HWND>(0x2000);
const HWND menu = reinterpret_cast<HWND>(0x3000);

WindowState window(HWND hwnd, DWORD pid = 42, std::string title = "w") {
    return {.hwnd = hwnd, .pid = pid, .image_name = "ms-teams.exe", .title = std::move(title),
            .rect = {10, 20, 300, 200}};
}
} // namespace

TEST_CASE("Windows are classified like taskbar buttons", "[windows]") {
    CHECK(classify_window(WS_OVERLAPPEDWINDOW, 0, false, L"Chrome_WidgetWin_1") == WindowRole::Main);
    CHECK(classify_window(WS_POPUP, 0, false, L"Frameless") == WindowRole::Main);
    CHECK(classify_window(WS_OVERLAPPEDWINDOW, WS_EX_APPWINDOW, true, L"Pop-out") == WindowRole::Main);
    CHECK(classify_window(WS_POPUP | WS_CAPTION | WS_SYSMENU, WS_EX_DLGMODALFRAME, true, L"#32770") ==
          WindowRole::Owned);
    CHECK(classify_window(WS_POPUP, WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, false,
                          L"Chrome_WidgetWin_1") == WindowRole::Popup);
    CHECK(classify_window(WS_POPUP, WS_EX_TOOLWINDOW, false, L"tooltips_class32") == WindowRole::Popup);
    CHECK(classify_window(WS_POPUP, 0, true, L"#32768") == WindowRole::Popup);
    CHECK(classify_window(WS_OVERLAPPED | WS_CAPTION, WS_EX_TOOLWINDOW, true, L"Palette") == WindowRole::Tool);
}

TEST_CASE("The window list reports opened, changed and closed windows", "[windows]") {
    WindowList list;
    auto events = list.update({window(main_window), window(meeting)});
    REQUIRE(events.size() == 2);
    CHECK(events[0].kind == WindowEvent::Kind::Opened);
    CHECK(events[0].window.hwnd == main_window);
    CHECK(events[1].window.hwnd == meeting);
    CHECK(list.update({window(main_window), window(meeting)}).empty());
    // Stacking order alone is not reported.
    CHECK(list.update({window(meeting), window(main_window)}).empty());

    auto moved = window(meeting);
    moved.rect.x = 50;
    events = list.update({window(menu), window(main_window, 42, "Chat"), moved});
    REQUIRE(events.size() == 3);
    CHECK(events[0].kind == WindowEvent::Kind::Opened);
    CHECK(events[0].window.hwnd == menu);
    CHECK(events[1].kind == WindowEvent::Kind::Changed);
    CHECK(events[1].window.title == "Chat");
    CHECK(events[2].kind == WindowEvent::Kind::Changed);
    CHECK(events[2].window.rect.x == 50);

    events = list.update({window(main_window, 42, "Chat")});
    REQUIRE(events.size() == 2);
    CHECK(events[0].kind == WindowEvent::Kind::Closed);
    CHECK(events[0].window.hwnd == menu);
    CHECK(events[1].kind == WindowEvent::Kind::Closed);
    CHECK(events[1].window.hwnd == meeting);
    CHECK(list.windows().size() == 1);
}

TEST_CASE("A reused window handle is a new window", "[windows]") {
    WindowList list;
    list.update({window(main_window, 42)});
    const auto events = list.update({window(main_window, 99)});
    REQUIRE(events.size() == 2);
    CHECK(events[0].kind == WindowEvent::Kind::Closed);
    CHECK(events[0].window.pid == 42);
    CHECK(events[1].kind == WindowEvent::Kind::Opened);
    CHECK(events[1].window.pid == 99);
}

TEST_CASE("The image name of the current process is resolved", "[windows]") {
    CHECK(process_image_name(GetCurrentProcessId()) == "BeamItTests.exe");
    CHECK(process_image_name(0).empty());
}
