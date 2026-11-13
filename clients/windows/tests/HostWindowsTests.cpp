#include "HostWindows.hpp"
#include "KeyCodes.hpp"

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

using namespace beamit::client;

namespace {

HostWindow window(std::string hwnd, std::int64_t pid, std::string role, std::string title,
                  std::string state = "normal", std::string image = "app.exe") {
    HostWindow result;
    result.hwnd = std::move(hwnd);
    result.pid = pid;
    result.role = std::move(role);
    result.title = std::move(title);
    result.state = std::move(state);
    result.image_name = std::move(image);
    result.rect = {10, 20, 300, 200};
    return result;
}

} // namespace

TEST_CASE("Host windows parse PROTOCOL.md section 5.12 objects", "[host-windows]") {
    const auto parsed = parse_host_window(boost::json::parse(R"({"hwnd":"0x2A0C14","pid":4242,"image_name":"ms-teams.exe",
        "title":"Meeting","role":"main","owner":"0x4E","rect":{"x":-807,"y":150.5,"width":1280,"height":720},"state":"normal"})"));
    REQUIRE(parsed);
    CHECK(parsed->pid == 4242);
    CHECK(parsed->owner == "0x4E");
    CHECK(parsed->rect.x == -807);
    CHECK(parsed->rect.y == 150.5);
    CHECK_FALSE(parse_host_window(boost::json::parse(R"({"hwnd":"0x1","pid":1,"role":"main","state":"normal"})")));
    CHECK_FALSE(parse_host_window(boost::json::parse(R"({"hwnd":"","pid":1,"role":"main","state":"normal",
        "rect":{"x":0,"y":0,"width":1,"height":1}})")));
}

TEST_CASE("Applications group streamable windows with main windows first", "[host-windows]") {
    HostWindowList list;
    CHECK(list.update(window("0x3", 7, "owned", "Settings"), false));
    CHECK(list.update(window("0x1", 7, "main", "Teams"), false));
    CHECK(list.update(window("0x4", 7, "popup", "Menu"), false));
    CHECK(list.update(window("0x2", 3, "main", "", "normal", "notepad.exe"), false));
    auto moved = window("0x1", 7, "main", "Teams");
    moved.rect.x = 99;
    CHECK_FALSE(list.update(moved, false));
    CHECK(list.find("0x1")->rect.x == 99);

    const auto applications = list.applications();
    REQUIRE(applications.size() == 2);
    CHECK(applications[0].title == "notepad.exe");
    CHECK(applications[1].title == "Teams");
    REQUIRE(applications[1].windows.size() == 2);
    CHECK(applications[1].windows[0].hwnd == "0x1");

    CHECK(filter_applications(applications, "  teams   settings ").size() == 1);
    CHECK(filter_applications(applications, "7").size() == 1);
    CHECK(filter_applications(applications, "NOTEPAD").size() == 1);
    CHECK(filter_applications(applications, "teams notepad").empty());
    CHECK(matching_window_title(applications[1], "settings") == "Settings");
    CHECK_FALSE(matching_window_title(applications[1], "teams"));

    CHECK(list.update(window("0x5", 7, "tool", "Palette", "minimized"), false));
    const auto follow = windows_to_follow(list, {7}, {"0x1"});
    REQUIRE(follow.size() == 1);
    CHECK(follow[0].hwnd == "0x3");
    CHECK(list.update(window("0x3", 7, "owned", "Settings"), true));
    CHECK_FALSE(list.update(window("0x3", 7, "owned", "Settings"), true));
}

TEST_CASE("Scan codes map to KeyboardEvent.code like the host table", "[keys]") {
    CHECK(keyboard_code(0x1E, false, 'A') == "KeyA");
    CHECK(keyboard_code(0x1D, true, VK_CONTROL) == "ControlRight");
    CHECK(keyboard_code(0x1D, false, VK_CONTROL) == "ControlLeft");
    CHECK(keyboard_code(0x1C, true, VK_RETURN) == "NumpadEnter");
    CHECK(keyboard_code(0x45, true, VK_NUMLOCK) == "NumLock");
    CHECK(keyboard_code(0x45, false, VK_PAUSE) == "Pause");
    CHECK(keyboard_code(0x5B, true, VK_LWIN) == "MetaLeft");
    CHECK(keyboard_code(0x56, false, VK_OEM_102) == "IntlBackslash");
    CHECK_FALSE(keyboard_code(0x00, false, 0));
}
