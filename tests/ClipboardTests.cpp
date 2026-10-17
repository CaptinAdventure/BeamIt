#include "host/Clipboard.hpp"
#include "net/Crypto.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <future>
#include <string>
#include <vector>

using namespace beamit;

namespace {

std::span<const std::byte> bytes(std::string_view text) { return std::as_bytes(std::span(text)); }

// Builds a CF_HTML payload with correct byte offsets.
std::string cf_html(std::string_view fragment) {
    const std::string prefix = "<html><body><!--StartFragment-->";
    const std::string suffix = "<!--EndFragment--></body></html>";
    const std::size_t header_size = 13 + 22 + 20 + 26 + 24;
    const std::size_t start_fragment = header_size + prefix.size();
    const std::size_t end_fragment = start_fragment + fragment.size();
    const auto number = [](std::size_t value) {
        std::string text = std::to_string(value);
        return std::string(10 - text.size(), '0') + text;
    };
    std::string header = "Version:0.9\r\nStartHTML:" + number(header_size) + "\r\nEndHTML:" +
                         number(end_fragment + suffix.size()) + "\r\nStartFragment:" + number(start_fragment) +
                         "\r\nEndFragment:" + number(end_fragment) + "\r\n";
    REQUIRE(header.size() == header_size);
    return header + prefix + std::string(fragment) + suffix;
}

std::vector<std::byte> dib_24bpp_2x2() {
    BITMAPINFOHEADER header{.biSize = sizeof(BITMAPINFOHEADER), .biWidth = 2, .biHeight = 2, .biPlanes = 1,
                            .biBitCount = 24, .biCompression = BI_RGB};
    std::vector<std::byte> dib(sizeof(header) + 2 * 8); // rows padded to 4 bytes
    std::memcpy(dib.data(), &header, sizeof(header));
    dib[sizeof(header) + 2] = std::byte{0xFF}; // bottom-left pixel red (BGR order)
    return dib;
}

} // namespace

TEST_CASE("base64 follows RFC 4648 section 4 with padding", "[protocol][clipboard]") {
    CHECK(net::base64_encode(bytes("")) == "");
    CHECK(net::base64_encode(bytes("f")) == "Zg==");
    CHECK(net::base64_encode(bytes("fo")) == "Zm8=");
    CHECK(net::base64_encode(bytes("foo")) == "Zm9v");
    CHECK(net::base64_encode(bytes("foobar")) == "Zm9vYmFy");
    const std::byte high[] = {std::byte{0xFB}, std::byte{0xFF}};
    CHECK(net::base64_encode(high) == "+/8=");
    CHECK(host::base64_length(0) == 0);
    CHECK(host::base64_length(1) == 4);
    CHECK(host::base64_length(6) == 8);
}

TEST_CASE("base64 decoding is strict", "[protocol][clipboard]") {
    const auto text = [](std::string_view encoded) -> std::optional<std::string> {
        const auto decoded = net::base64_decode(encoded);
        if (!decoded) return std::nullopt;
        return std::string(reinterpret_cast<const char*>(decoded->data()), decoded->size());
    };
    CHECK(text("Zm9vYmFy") == "foobar");
    CHECK(text("Zg==") == "f");
    CHECK(text("Zg") == "f");
    CHECK(text("Zm8=") == "fo");
    CHECK(text("+/8=") == std::string("\xFB\xFF"));
    CHECK_FALSE(net::base64_decode("-_8="));  // base64url alphabet
    CHECK_FALSE(net::base64_decode("Zg=a"));  // misplaced padding
    CHECK_FALSE(net::base64_decode("Zh=="));  // non-canonical trailing bits
    CHECK_FALSE(net::base64_decode("Zm9v\n")); // whitespace
}

TEST_CASE("HTML fragments round-trip through clipboard documents", "[clipboard]") {
    const std::string fragment = "<p>Gr\xC3\xBC\xC3\x9F <i>x</i></p>";
    const auto document = host::html_clipboard_document(fragment);
    CHECK(document.starts_with("Version:0.9\r\nStartHTML:"));
    CHECK(host::html_clipboard_fragment(document) == fragment);
    CHECK(host::html_clipboard_fragment(host::html_clipboard_document("")) == "");
}

TEST_CASE("HTML clipboard data yields its fragment", "[clipboard]") {
    CHECK(host::html_clipboard_fragment(cf_html("<b>caf\xC3\xA9</b>")) == "<b>caf\xC3\xA9</b>");
    CHECK(host::html_clipboard_fragment("Version:0.9\r\nStartHTML:-1\r\n") == std::nullopt);
    CHECK(host::html_clipboard_fragment("Version:0.9\r\nStartFragment:30\r\nEndFragment:99999\r\n") == std::nullopt);
    // Invalid UTF-8 is replaced rather than passed into JSON.
    CHECK(host::html_clipboard_fragment(cf_html("a\xFF" "b")) == "a\xEF\xBF\xBD" "b");
}

TEST_CASE("DIBs gain a bitmap file header with the pixel offset", "[clipboard]") {
    const auto dib = dib_24bpp_2x2();
    const auto file = host::bitmap_file_from_dib(dib);
    REQUIRE(file);
    BITMAPFILEHEADER header{};
    std::memcpy(&header, file->data(), sizeof(header));
    CHECK(header.bfType == 0x4D42);
    CHECK(header.bfSize == file->size());
    CHECK(header.bfOffBits == sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER));

    auto paletted = dib;
    auto* info = reinterpret_cast<BITMAPINFOHEADER*>(paletted.data());
    info->biBitCount = 8;
    CHECK_FALSE(host::bitmap_file_from_dib(paletted)); // 256-entry palette missing
    paletted.resize(paletted.size() + 256 * sizeof(RGBQUAD));
    REQUIRE(host::bitmap_file_from_dib(paletted));
    CHECK(reinterpret_cast<const BITMAPFILEHEADER*>(host::bitmap_file_from_dib(paletted)->data())->bfOffBits ==
          sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + 256 * sizeof(RGBQUAD));
    CHECK_FALSE(host::bitmap_file_from_dib(std::span(dib).first(10)));
}

TEST_CASE("DIBs convert to PNG", "[clipboard]") {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const auto image = host::png_from_dib(dib_24bpp_2x2());
    if (SUCCEEDED(com)) CoUninitialize();
    REQUIRE(image);
    CHECK(image->width == 2);
    CHECK(image->height == 2);
    const auto size = host::png_dimensions(image->png);
    REQUIRE(size);
    CHECK(size->first == 2);
    CHECK(size->second == 2);
    CHECK_FALSE(host::png_dimensions(bytes("not a png at all, really")));
}

TEST_CASE("PNGs convert to opaque bottom-up DIBs", "[clipboard]") {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const auto png = host::png_from_dib(dib_24bpp_2x2());
    REQUIRE(png);
    const auto dib = host::dib_from_png(png->png);
    CHECK_FALSE(host::dib_from_png(bytes("not a png at all, really")));
    if (SUCCEEDED(com)) CoUninitialize();
    REQUIRE(dib);
    CHECK(dib->width == 2);
    CHECK(dib->height == 2);
    REQUIRE(dib->dib.size() == sizeof(BITMAPINFOHEADER) + 2 * 2 * 4);
    BITMAPINFOHEADER header{};
    std::memcpy(&header, dib->dib.data(), sizeof(header));
    CHECK(header.biHeight == 2); // bottom-up
    CHECK(header.biBitCount == 32);
    const auto* pixels = dib->dib.data() + sizeof(BITMAPINFOHEADER);
    // Bottom-left pixel: red (BGRA), opaque.
    CHECK(pixels[0] == std::byte{0});
    CHECK(pixels[1] == std::byte{0});
    CHECK(pixels[2] == std::byte{0xFF});
    CHECK(pixels[3] == std::byte{0xFF});
    CHECK(pixels[6] == std::byte{0}); // bottom-right black
    // A forged IHDR with oversized dimensions is rejected before decoding.
    auto huge = png->png;
    huge[16] = std::byte{0x01};
    CHECK_FALSE(host::dib_from_png(huge));
}

// Overwrites the interactive user's clipboard, so it only runs when selected explicitly.
TEST_CASE("Clipboard monitor reads text and reports changes", "[.clipboard-live]") {
    std::promise<host::ClipboardChange> changed;
    bool notified = false;
    host::ClipboardMonitor monitor([&](const host::ClipboardChange& change) {
        if (!notified && (change.formats & host::clipboard_text)) {
            notified = true;
            changed.set_value(change);
        }
    });
    const std::wstring text = L"BeamIt clipboard \u00E9";
    // SetClipboardData fails when the clipboard is opened without an owner window.
    const HWND owner = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    REQUIRE(owner);
    REQUIRE(OpenClipboard(owner));
    EmptyClipboard();
    const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    std::memcpy(GlobalLock(memory), text.c_str(), (text.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(memory);
    REQUIRE(SetClipboardData(CF_UNICODETEXT, memory));
    CloseClipboard();
    DestroyWindow(owner);

    auto change = changed.get_future();
    REQUIRE(change.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    CHECK_FALSE(change.get().excluded);

    std::promise<host::ClipboardReadResult> read;
    monitor.read(host::clipboard_all, 1024, [&](host::ClipboardReadResult result) { read.set_value(std::move(result)); });
    auto result = read.get_future().get();
    REQUIRE(result.content);
    CHECK(result.content->text == "BeamIt clipboard \xC3\xA9");
    CHECK_FALSE(result.content->image);

    std::promise<host::ClipboardReadResult> limited_read;
    monitor.read(host::clipboard_text, 4, [&](host::ClipboardReadResult result) { limited_read.set_value(std::move(result)); });
    auto limited = limited_read.get_future().get();
    REQUIRE(limited.content);
    CHECK_FALSE(limited.content->text);
    REQUIRE(limited.content->omitted.size() == 1);
    CHECK(limited.content->omitted[0].reason == "too_large");

    std::promise<host::ClipboardWriteResult> written;
    monitor.write({.text = "written \xE2\x9C\x93", .html = "<b>written</b>", .image = std::nullopt, .sensitive = true},
                  [&](host::ClipboardWriteResult result) { written.set_value(std::move(result)); });
    const auto write_result = written.get_future().get();
    CHECK(write_result.error_code.empty());
    CHECK(write_result.sequence != 0);

    std::promise<host::ClipboardReadResult> reread;
    monitor.read(host::clipboard_all, 1024, [&](host::ClipboardReadResult result) { reread.set_value(std::move(result)); });
    auto after = reread.get_future().get();
    REQUIRE(after.content);
    // Sensitive content is hidden from clipboard monitors, including this one.
    CHECK_FALSE(after.content->text);
    CHECK(after.content->omitted.size() == 3);

    std::promise<host::ClipboardWriteResult> rejected;
    monitor.write({.text = std::nullopt, .html = std::nullopt, .image = "bm90IGEgcG5n", .sensitive = false},
                  [&](host::ClipboardWriteResult result) { rejected.set_value(std::move(result)); });
    CHECK(rejected.get_future().get().error_code == "invalid_request");

    std::promise<host::ClipboardWriteResult> plain;
    monitor.write({.text = "plain", .html = "<i>plain</i>", .image = std::nullopt, .sensitive = false},
                  [&](host::ClipboardWriteResult result) { plain.set_value(std::move(result)); });
    CHECK(plain.get_future().get().error_code.empty());
    std::promise<host::ClipboardReadResult> final_read;
    monitor.read(host::clipboard_all, 1024,
                 [&](host::ClipboardReadResult result) { final_read.set_value(std::move(result)); });
    auto final_content = final_read.get_future().get();
    REQUIRE(final_content.content);
    CHECK(final_content.content->text == "plain");
    CHECK(final_content.content->html == "<i>plain</i>");
}
