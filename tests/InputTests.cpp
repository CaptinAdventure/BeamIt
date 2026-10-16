#include "host/InputInjector.hpp"

#include <catch2/catch_test_macros.hpp>

#include <utility>

using namespace beamit::host::input;

TEST_CASE("KeyboardEvent.code values map to Set-1 scan codes", "[input]") {
    const auto a = key_for_code("KeyA");
    REQUIRE(a);
    CHECK(a->scan == 0x1E);
    CHECK_FALSE(a->extended);
    CHECK(a->virtual_key == 0);

    const auto up = key_for_code("ArrowUp");
    REQUIRE(up);
    CHECK(up->scan == 0x48);
    CHECK(up->extended);

    const auto enter = key_for_code("NumpadEnter");
    REQUIRE(enter);
    CHECK(enter->scan == 0x1C);
    CHECK(enter->extended);

    CHECK(key_for_code("MetaLeft")->scan == 0x5B);
    CHECK(key_for_code("OSLeft") == key_for_code("MetaLeft"));
    CHECK(key_for_code("Pause")->virtual_key == VK_PAUSE);
    CHECK(key_for_code("NumLock")->virtual_key == VK_NUMLOCK);
    CHECK(key_for_code("IntlBackslash")->scan == 0x56);
    CHECK_FALSE(key_for_code("NoSuchKey"));
    CHECK_FALSE(key_for_code("keya"));
}

TEST_CASE("Mouse modes and buttons parse", "[input]") {
    CHECK(parse_mouse_mode("absolute") == MouseMode::Absolute);
    CHECK(parse_mouse_mode("relative") == MouseMode::Relative);
    CHECK_FALSE(parse_mouse_mode("Absolute"));
    CHECK(mouse_mode_name(MouseMode::Relative) == "relative");
    CHECK(parse_mouse_button("left") == MouseButton::Left);
    CHECK(parse_mouse_button("forward") == MouseButton::Forward);
    CHECK_FALSE(parse_mouse_button("primary"));
}

TEST_CASE("Absolute video coordinates map onto the target rectangle", "[input]") {
    const ScreenRect window{100, 50, 900, 650}; // 800x600 on screen

    CHECK(map_to_rect(0, 0, 800, 600, window).x == 100);
    CHECK(map_to_rect(0, 0, 800, 600, window).y == 50);
    CHECK(map_to_rect(799.9, 599.9, 800, 600, window).x == 899);
    CHECK(map_to_rect(799.9, 599.9, 800, 600, window).y == 649);

    // Video at half resolution (e.g. scaled) maps proportionally.
    const auto scaled = map_to_rect(200, 150, 400, 300, window);
    CHECK(scaled.x == 500);
    CHECK(scaled.y == 350);

    // Out-of-range positions clamp inside the rectangle.
    const auto outside = map_to_rect(-50, 10'000, 800, 600, window);
    CHECK(outside.x == 100);
    CHECK(outside.y == 649);

    // Negative screen coordinates (monitor left of the primary) work too.
    const auto left = map_to_rect(10, 10, 1920, 1080, ScreenRect{-1920, 0, 0, 1080});
    CHECK(left.x == -1910);
    CHECK(left.y == 10);
}

TEST_CASE("Screen points normalize to the virtual desktop", "[input]") {
    const ScreenRect desk{-1920, 0, 1920, 1080}; // 3840 wide
    const auto denormalize = [](long value, long low, long size) { return low + (value * size) / 65536; };

    for (const long x : {-1920L, -1L, 0L, 1L, 1000L, 1919L}) {
        const auto normalized = normalize_to_virtual_desk({x, 540}, desk);
        CHECK(normalized.x >= 0);
        CHECK(normalized.x <= 65535);
        CHECK(denormalize(normalized.x, -1920, 3840) == x);
        CHECK(denormalize(normalized.y, 0, 1080) == 540);
    }
}

TEST_CASE("Text input is validated and converted to UTF-16", "[input]") {
    CHECK(text_to_utf16("hello") == std::wstring(L"hello"));
    const auto emoji = text_to_utf16("h\xC3\xA9 \xF0\x9F\x91\x8B");
    REQUIRE(emoji);
    CHECK(emoji->size() == 5); // h, é, space, surrogate pair
    CHECK(text_to_utf16("a\tb\r\nc"));
    CHECK_FALSE(text_to_utf16("bell\x07"));
    CHECK_FALSE(text_to_utf16("\xFF\xFE"));
    CHECK_FALSE(text_to_utf16(std::string(max_text_units + 1, 'x')));
    CHECK(text_to_utf16(std::string(max_text_units, 'x')));
    CHECK(text_to_utf16("")->empty());
}

TEST_CASE("Screen points map back to video coordinates", "[input]") {
    const ScreenRect window{100, 50, 900, 650};
    for (const auto [x, y] : {std::pair{0.0, 0.0}, {123.0, 456.0}, {799.0, 599.0}}) {
        const auto screen = map_to_rect(x, y, 800, 600, window);
        const auto video = map_from_rect(screen, 800, 600, window);
        CHECK(video.x == static_cast<long>(x));
        CHECK(video.y == static_cast<long>(y));
    }
    // Odd-sized window cropped to an even video size round-trips too.
    const ScreenRect odd{207, 233, 207 + 627, 233 + 473};
    for (long v = 0; v < 472; ++v) {
        CHECK(map_from_rect(map_to_rect(static_cast<double>(v), static_cast<double>(v), 626, 472, odd), 626, 472, odd).y ==
              v);
    }
    // Scaled video: screen 500,350 is video 200,150 at half resolution.
    CHECK(map_from_rect({500, 350}, 400, 300, window).x == 200);
    CHECK(map_from_rect({500, 350}, 400, 300, window).y == 150);
    // Points outside (e.g. another monitor) are not clamped.
    CHECK(map_from_rect({-1920, 10}, 1920, 1080, ScreenRect{0, 0, 1920, 1080}).x == -1920);
    CHECK(rect_contains(window, {100, 50}));
    CHECK_FALSE(rect_contains(window, {900, 100}));
    CHECK_FALSE(rect_contains(window, {99, 100}));
}
