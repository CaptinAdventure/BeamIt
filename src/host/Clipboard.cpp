#include "host/Clipboard.hpp"

#include "Utf8.hpp"
#include "net/Crypto.hpp"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <charconv>
#include <climits>
#include <cstring>
#include <deque>
#include <future>
#include <mutex>
#include <thread>

namespace beamit::host {
namespace {

using Microsoft::WRL::ComPtr;

// Upper bounds for data copied out of the clipboard before conversion; larger items are reported as too large.
constexpr std::size_t max_html_source_bytes = 64ULL << 20;
constexpr std::size_t max_dib_bytes = 256ULL << 20;

std::uint32_t read_u32_le(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint32_t value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

std::uint16_t read_u16_le(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint16_t value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

std::uint32_t read_u32_be(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint32_t value{};
    for (std::size_t i = 0; i < 4; ++i) value = (value << 8) | std::to_integer<std::uint32_t>(bytes[offset + i]);
    return value;
}

// Returns `text` unchanged if it is valid UTF-8, otherwise with invalid sequences replaced by U+FFFD.
std::string valid_utf8(std::string_view text) {
    if (text.empty() || text.size() > INT_MAX) return {};
    const int size = static_cast<int>(text.size());
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size, nullptr, 0) > 0) return std::string(text);
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), size, nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(std::max(length, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), size, wide.data(), length);
    return to_utf8(wide);
}

std::optional<std::size_t> header_offset(std::string_view header, std::string_view key) {
    const auto position = header.find(key);
    if (position == std::string_view::npos) return std::nullopt;
    const char* first = header.data() + position + key.size();
    std::size_t value{};
    const auto [end, error] = std::from_chars(first, header.data() + header.size(), value);
    if (error != std::errc{} || end == first) return std::nullopt;
    return value;
}

std::optional<ClipboardImage> png_from_bitmap_file(std::span<const std::byte> bitmap) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> input;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IStream> output;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> target;
    UINT width{};
    UINT height{};
    WICPixelFormatGUID format{};
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(result)) result = factory->CreateStream(&input);
    if (SUCCEEDED(result)) {
        result = input->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<std::byte*>(bitmap.data())),
                                             static_cast<DWORD>(bitmap.size()));
    }
    if (SUCCEEDED(result)) {
        result = factory->CreateDecoderFromStream(input.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
    }
    if (SUCCEEDED(result)) result = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(result)) result = frame->GetSize(&width, &height);
    if (SUCCEEDED(result)) result = frame->GetPixelFormat(&format);
    if (SUCCEEDED(result)) result = CreateStreamOnHGlobal(nullptr, TRUE, &output);
    if (SUCCEEDED(result)) result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(result)) result = encoder->Initialize(output.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(result)) result = encoder->CreateNewFrame(&target, nullptr);
    if (SUCCEEDED(result)) result = target->Initialize(nullptr);
    if (SUCCEEDED(result)) result = target->SetSize(width, height);
    if (SUCCEEDED(result)) result = target->SetPixelFormat(&format); // may pick the nearest PNG format
    if (SUCCEEDED(result)) result = target->WriteSource(frame.Get(), nullptr);
    if (SUCCEEDED(result)) result = target->Commit();
    if (SUCCEEDED(result)) result = encoder->Commit();
    HGLOBAL memory{};
    STATSTG stat{};
    if (SUCCEEDED(result)) result = GetHGlobalFromStream(output.Get(), &memory);
    if (SUCCEEDED(result)) result = output->Stat(&stat, STATFLAG_NONAME);
    if (FAILED(result)) {
        spdlog::warn("Clipboard bitmap to PNG conversion failed (HRESULT 0x{:08X})", static_cast<std::uint32_t>(result));
        return std::nullopt;
    }
    const auto* data = static_cast<const std::byte*>(GlobalLock(memory));
    if (!data) return std::nullopt;
    ClipboardImage image{.png = std::vector<std::byte>(data, data + stat.cbSize.QuadPart), .width = width,
                         .height = height};
    GlobalUnlock(memory);
    return image;
}

} // namespace

std::optional<std::string> html_clipboard_fragment(std::string_view cf_html) {
    // Offsets in the "Version:...\r\nStartHTML:...\r\n..." header are byte offsets from the start of the data.
    const auto header = cf_html.substr(0, std::min(cf_html.find('<'), cf_html.size()));
    auto start = header_offset(header, "StartFragment:");
    auto end = header_offset(header, "EndFragment:");
    if (!start || !end) {
        start = header_offset(header, "StartHTML:");
        end = header_offset(header, "EndHTML:");
    }
    if (!start || !end || *start > *end || *end > cf_html.size()) return std::nullopt;
    return valid_utf8(cf_html.substr(*start, *end - *start));
}

std::optional<std::vector<std::byte>> bitmap_file_from_dib(std::span<const std::byte> dib) {
    constexpr std::size_t file_header_size = 14;
    if (dib.size() < sizeof(BITMAPINFOHEADER) || dib.size() > UINT32_MAX - file_header_size) return std::nullopt;
    const std::uint32_t header_size = read_u32_le(dib, 0);
    if (header_size < sizeof(BITMAPINFOHEADER) || header_size > dib.size()) return std::nullopt;
    const std::uint16_t bit_count = read_u16_le(dib, 14);
    const std::uint32_t compression = read_u32_le(dib, 16);
    const std::uint32_t colors_used = read_u32_le(dib, 32);

    std::uint64_t pixels = header_size;
    // A plain BITMAPINFOHEADER is followed by the channel masks; V4/V5 headers contain them.
    if (header_size == sizeof(BITMAPINFOHEADER) && compression == BI_BITFIELDS) pixels += 3 * sizeof(DWORD);
    if (header_size == sizeof(BITMAPINFOHEADER) && compression == 6 /* BI_ALPHABITFIELDS */) pixels += 4 * sizeof(DWORD);
    const std::uint64_t colors = colors_used != 0 ? colors_used : (bit_count <= 8 ? 1ULL << bit_count : 0);
    pixels += colors * sizeof(RGBQUAD);
    if (pixels > dib.size()) return std::nullopt;

    std::vector<std::byte> file(file_header_size + dib.size());
    const auto put_u16 = [&](std::size_t offset, std::uint16_t value) { std::memcpy(&file[offset], &value, 2); };
    const auto put_u32 = [&](std::size_t offset, std::uint32_t value) { std::memcpy(&file[offset], &value, 4); };
    put_u16(0, 0x4D42); // "BM"
    put_u32(2, static_cast<std::uint32_t>(file.size()));
    put_u32(10, static_cast<std::uint32_t>(file_header_size + pixels));
    std::memcpy(file.data() + file_header_size, dib.data(), dib.size());
    return file;
}

std::string html_clipboard_document(std::string_view fragment) {
    constexpr std::string_view prefix = "<html><body>\r\n<!--StartFragment-->";
    constexpr std::string_view suffix = "<!--EndFragment-->\r\n</body></html>";
    constexpr auto header_size = std::string_view("Version:0.9\r\nStartHTML:0000000000\r\nEndHTML:0000000000\r\n"
                                                  "StartFragment:0000000000\r\nEndFragment:0000000000\r\n").size();
    const std::size_t start_fragment = header_size + prefix.size();
    const std::size_t end_fragment = start_fragment + fragment.size();
    std::string document = fmt::format("Version:0.9\r\nStartHTML:{:010}\r\nEndHTML:{:010}\r\nStartFragment:{:010}\r\n"
                                       "EndFragment:{:010}\r\n",
                                       header_size, end_fragment + suffix.size(), start_fragment, end_fragment);
    document.append(prefix).append(fragment).append(suffix);
    return document;
}

std::optional<ClipboardDib> dib_from_png(std::span<const std::byte> png) {
    constexpr UINT max_side = 16384;
    constexpr std::uint64_t max_pixels = 64ULL << 20;
    const auto size = png_dimensions(png);
    if (!size || size->first == 0 || size->second == 0 || size->first > max_side || size->second > max_side ||
        std::uint64_t{size->first} * size->second > max_pixels) {
        return std::nullopt;
    }
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> input;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    UINT width{};
    UINT height{};
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(result)) result = factory->CreateStream(&input);
    if (SUCCEEDED(result)) {
        result = input->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<std::byte*>(png.data())),
                                             static_cast<DWORD>(png.size()));
    }
    if (SUCCEEDED(result)) {
        result = factory->CreateDecoderFromStream(input.Get(), &GUID_ContainerFormatPng, WICDecodeMetadataCacheOnDemand,
                                                  &decoder);
    }
    if (SUCCEEDED(result)) result = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(result)) result = frame->GetSize(&width, &height);
    if (SUCCEEDED(result) && (width != size->first || height != size->second)) result = E_INVALIDARG;
    if (SUCCEEDED(result)) result = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(result)) {
        result = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                       WICBitmapPaletteTypeCustom);
    }
    const std::size_t stride = std::size_t{width} * 4;
    ClipboardDib output{.dib = std::vector<std::byte>(sizeof(BITMAPINFOHEADER) + stride * height), .width = width,
                        .height = height};
    auto* pixels = output.dib.data() + sizeof(BITMAPINFOHEADER);
    // Rows are copied bottom-up, the orientation every paste target understands.
    for (UINT row = 0; SUCCEEDED(result) && row < height; ++row) {
        const WICRect line{0, static_cast<INT>(row), static_cast<INT>(width), 1};
        result = converter->CopyPixels(&line, static_cast<UINT>(stride), static_cast<UINT>(stride),
                                       reinterpret_cast<BYTE*>(pixels + stride * (height - 1 - row)));
    }
    if (FAILED(result)) {
        spdlog::warn("Clipboard PNG decoding failed (HRESULT 0x{:08X})", static_cast<std::uint32_t>(result));
        return std::nullopt;
    }
    for (std::size_t offset = 0; offset < stride * height; offset += 4) {
        // Premultiplied colour over white: c + (255 - alpha), then opaque.
        const auto alpha = std::to_integer<unsigned>(pixels[offset + 3]);
        for (std::size_t channel = 0; channel < 3; ++channel) {
            pixels[offset + channel] =
                static_cast<std::byte>(std::min(255U, std::to_integer<unsigned>(pixels[offset + channel]) + 255 - alpha));
        }
        pixels[offset + 3] = std::byte{0xFF};
    }
    const BITMAPINFOHEADER header{.biSize = sizeof(BITMAPINFOHEADER), .biWidth = static_cast<LONG>(width),
                                  .biHeight = static_cast<LONG>(height), .biPlanes = 1, .biBitCount = 32,
                                  .biCompression = BI_RGB, .biSizeImage = static_cast<DWORD>(stride * height)};
    std::memcpy(output.dib.data(), &header, sizeof(header));
    return output;
}

std::optional<ClipboardImage> png_from_dib(std::span<const std::byte> dib) {
    const auto bitmap = bitmap_file_from_dib(dib);
    return bitmap ? png_from_bitmap_file(*bitmap) : std::nullopt;
}

std::optional<std::pair<std::uint32_t, std::uint32_t>> png_dimensions(std::span<const std::byte> png) {
    constexpr unsigned char signature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (png.size() < 24 || std::memcmp(png.data(), signature, sizeof(signature)) != 0 ||
        std::memcmp(png.data() + 12, "IHDR", 4) != 0) {
        return std::nullopt;
    }
    return std::pair{read_u32_be(png, 16), read_u32_be(png, 20)};
}

struct ClipboardMonitor::Impl {
    struct Request {
        unsigned formats{};
        std::size_t max_bytes{};
        ReadCallback done;
    };

    struct WriteRequest {
        ClipboardWrite content;
        std::function<void(ClipboardWriteResult)> done;
    };

    static constexpr UINT wake_message = WM_APP + 1;

    ChangeCallback on_change;
    std::mutex mutex;
    std::deque<std::function<void()>> tasks;
    DWORD thread_id{};
    HWND window{};
    DWORD last_sequence{};
    UINT html_format{};
    UINT png_format{};
    UINT exclude_format{};
    std::jthread thread;

    [[nodiscard]] bool excluded() const {
        return exclude_format != 0 && IsClipboardFormatAvailable(exclude_format);
    }

    [[nodiscard]] unsigned available_formats() const {
        unsigned formats{};
        if (IsClipboardFormatAvailable(CF_UNICODETEXT)) formats |= clipboard_text;
        if (html_format != 0 && IsClipboardFormatAvailable(html_format)) formats |= clipboard_html;
        if ((png_format != 0 && IsClipboardFormatAvailable(png_format)) || IsClipboardFormatAvailable(CF_DIB) ||
            IsClipboardFormatAvailable(CF_DIBV5) || IsClipboardFormatAvailable(CF_BITMAP)) {
            formats |= clipboard_image;
        }
        return formats;
    }

    void on_update() {
        const DWORD sequence = GetClipboardSequenceNumber();
        if (sequence == last_sequence) return; // several WM_CLIPBOARDUPDATEs can describe one change
        last_sequence = sequence;
        if (GetClipboardOwner() == window) return; // written by a client (write()); do not echo it back
        const bool hidden = excluded();
        on_change({.sequence = sequence, .formats = hidden ? 0U : available_formats(), .excluded = hidden});
    }

    // Copies a clipboard handle's bytes, or reports `too_large` without copying when larger than `limit`.
    static std::optional<std::vector<std::byte>> copy_data(UINT format, std::size_t limit, bool& too_large) {
        const HANDLE handle = GetClipboardData(format);
        if (!handle) return std::nullopt;
        const SIZE_T size = GlobalSize(handle);
        if (size > limit) {
            too_large = true;
            return std::nullopt;
        }
        const auto* data = static_cast<const std::byte*>(GlobalLock(handle));
        if (!data) return std::nullopt;
        std::vector<std::byte> copy(data, data + size);
        GlobalUnlock(handle);
        return copy;
    }

    [[nodiscard]] bool open_clipboard() const {
        // Another application may hold the clipboard briefly; retry for about 100 ms.
        for (int attempt = 0; attempt < 20; ++attempt) {
            if (OpenClipboard(window)) return true;
            Sleep(5);
        }
        return false;
    }

    void serve(Request& request) {
        if (!open_clipboard()) {
            request.done({.content = std::nullopt, .error = "the clipboard is in use by another application"});
            return;
        }
        ClipboardContent content{.sequence = GetClipboardSequenceNumber()};
        const unsigned wanted = request.formats & clipboard_all;
        const std::size_t max = request.max_bytes;
        std::optional<std::wstring> wide_text;
        std::optional<std::vector<std::byte>> html_data;
        std::optional<std::vector<std::byte>> png_data;
        std::optional<std::vector<std::byte>> dib_data;
        unsigned too_large{};
        if (excluded()) {
            for (const auto format : {clipboard_text, clipboard_html, clipboard_image}) {
                if (wanted & format) content.omitted.push_back({format, "excluded"});
            }
        } else {
            bool large = false;
            if ((wanted & clipboard_text) && IsClipboardFormatAvailable(CF_UNICODETEXT)) {
                // Each UTF-16 unit becomes at least one UTF-8 byte, so oversize text is rejected before conversion.
                if (auto data = copy_data(CF_UNICODETEXT, (max + 1) * sizeof(wchar_t), large)) {
                    const auto* characters = reinterpret_cast<const wchar_t*>(data->data());
                    wide_text.emplace(characters, wcsnlen(characters, data->size() / sizeof(wchar_t)));
                }
                if (large) too_large |= clipboard_text;
            }
            if ((wanted & clipboard_html) && html_format != 0 && IsClipboardFormatAvailable(html_format)) {
                large = false;
                html_data = copy_data(html_format, max_html_source_bytes, large);
                if (large) too_large |= clipboard_html;
            }
            if (wanted & clipboard_image) {
                large = false;
                // Prefer an application-provided PNG (keeps alpha); otherwise use the opaque device-independent bitmap,
                // which Windows synthesizes from CF_BITMAP/CF_DIBV5 when needed.
                if (png_format != 0 && IsClipboardFormatAvailable(png_format)) {
                    png_data = copy_data(png_format, max_dib_bytes, large);
                } else if (IsClipboardFormatAvailable(CF_DIB)) {
                    dib_data = copy_data(CF_DIB, max_dib_bytes, large);
                }
                if (large) too_large |= clipboard_image;
            }
        }
        CloseClipboard();

        std::size_t used{};
        const auto fits = [&](std::size_t bytes) {
            if (bytes > max - used) return false;
            used += bytes;
            return true;
        };
        const auto omit = [&](ClipboardFormat format, const char* reason) { content.omitted.push_back({format, reason}); };

        if (too_large & clipboard_text) omit(clipboard_text, "too_large");
        if (wide_text) {
            auto text = to_utf8(*wide_text);
            if (fits(text.size())) {
                content.text = std::move(text);
            } else {
                omit(clipboard_text, "too_large");
            }
        }
        if (too_large & clipboard_html) omit(clipboard_html, "too_large");
        if (html_data) {
            const std::string_view raw(reinterpret_cast<const char*>(html_data->data()),
                                       strnlen(reinterpret_cast<const char*>(html_data->data()), html_data->size()));
            if (auto fragment = html_clipboard_fragment(raw); !fragment) {
                omit(clipboard_html, "unsupported");
            } else if (fits(fragment->size())) {
                content.html = std::move(*fragment);
            } else {
                omit(clipboard_html, "too_large");
            }
        }
        if (too_large & clipboard_image) omit(clipboard_image, "too_large");
        std::optional<ClipboardImage> image;
        if (png_data) {
            if (const auto size = png_dimensions(*png_data)) {
                image = ClipboardImage{.png = std::move(*png_data), .width = size->first, .height = size->second};
            }
        } else if (dib_data) {
            image = png_from_dib(*dib_data);
        }
        if (image) {
            if (fits(base64_length(image->png.size()))) {
                content.image = std::move(image);
            } else {
                omit(clipboard_image, "too_large");
            }
        } else if (png_data || dib_data) {
            omit(clipboard_image, "unsupported");
        }
        request.done({.content = std::move(content), .error = {}});
    }

    void serve_tasks() {
        for (;;) {
            std::function<void()> task;
            {
                std::scoped_lock lock(mutex);
                if (tasks.empty()) return;
                task = std::move(tasks.front());
                tasks.pop_front();
            }
            task();
        }
    }

    void post(std::function<void()> task) {
        {
            std::scoped_lock lock(mutex);
            tasks.push_back(std::move(task));
        }
        PostThreadMessageW(thread_id, wake_message, 0, 0);
    }

    static HGLOBAL global_copy(const void* data, std::size_t size) {
        const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, std::max<std::size_t>(size, 1));
        if (!memory) return nullptr;
        if (void* target = GlobalLock(memory)) {
            std::memcpy(target, data, size);
            GlobalUnlock(memory);
            return memory;
        }
        GlobalFree(memory);
        return nullptr;
    }

    void serve_write(WriteRequest& request) {
        const auto fail = [&](const char* code, std::string message) {
            request.done({.sequence = 0, .error_code = code, .error = std::move(message)});
        };
        auto& content = request.content;
        // Everything is converted before the clipboard is opened, so it is held only while handles are handed over.
        std::vector<std::pair<UINT, std::vector<std::byte>>> formats;
        const auto add = [&](UINT format, const void* data, std::size_t size) {
            const auto* first = static_cast<const std::byte*>(data);
            formats.emplace_back(format, std::vector<std::byte>(first, first + size));
        };
        if (content.text) {
            std::wstring wide;
            if (!content.text->empty()) {
                const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, content.text->data(),
                                                       static_cast<int>(content.text->size()), nullptr, 0);
                if (length <= 0) return fail("invalid_request", "text must be valid UTF-8");
                wide.resize(static_cast<std::size_t>(length));
                MultiByteToWideChar(CP_UTF8, 0, content.text->data(), static_cast<int>(content.text->size()),
                                    wide.data(), length);
            }
            add(CF_UNICODETEXT, wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
        }
        if (content.html) {
            if (html_format == 0) return fail("clipboard_unavailable", "the HTML clipboard format is unavailable");
            const auto document = html_clipboard_document(*content.html);
            add(html_format, document.c_str(), document.size() + 1);
        }
        if (content.image) {
            const auto png = net::base64_decode(*content.image);
            if (!png || !png_dimensions(*png)) return fail("invalid_request", "image.data must be a base64 PNG");
            const auto dib = dib_from_png(*png);
            if (!dib) return fail("invalid_request", "image.data could not be decoded (or exceeds 16384 px / 64 Mpx)");
            if (png_format != 0) add(png_format, png->data(), png->size());
            add(CF_DIB, dib->dib.data(), dib->dib.size());
        }
        if (content.sensitive) {
            const DWORD zero = 0;
            for (const auto* name : {L"ExcludeClipboardContentFromMonitorProcessing", L"CanIncludeInClipboardHistory",
                                     L"CanUploadToCloudClipboard"}) {
                if (const UINT format = RegisterClipboardFormatW(name)) add(format, &zero, sizeof(zero));
            }
        }

        if (!open_clipboard()) return fail("clipboard_unavailable", "the clipboard is in use by another application");
        bool ok = EmptyClipboard() != FALSE;
        for (const auto& [format, bytes] : formats) {
            if (!ok) break;
            const HGLOBAL memory = global_copy(bytes.data(), bytes.size());
            // On success the system owns the memory.
            if (!memory || !SetClipboardData(format, memory)) {
                if (memory) GlobalFree(memory);
                ok = false;
            }
        }
        if (!ok) EmptyClipboard(); // never leave a partial set of formats behind
        const DWORD error = GetLastError();
        CloseClipboard();
        if (!ok) return fail("clipboard_unavailable", fmt::format("writing the clipboard failed (error {})", error));
        request.done({.sequence = GetClipboardSequenceNumber(), .error_code = {}, .error = {}});
    }

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (self && message == WM_CLIPBOARDUPDATE) {
            self->on_update();
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    void run(std::promise<void>& started) {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED); // WIC image conversion
        constexpr wchar_t class_name[] = L"BeamItClipboardMonitor";
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW window_class{.cbSize = sizeof(WNDCLASSEXW), .lpfnWndProc = window_proc, .hInstance = instance,
                                 .lpszClassName = class_name};
        RegisterClassExW(&window_class);
        html_format = RegisterClipboardFormatW(L"HTML Format");
        png_format = RegisterClipboardFormatW(L"PNG");
        exclude_format = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
        window = CreateWindowExW(0, class_name, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
        MSG message{};
        PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE); // create the queue before announcing the thread id
        thread_id = GetCurrentThreadId();
        last_sequence = GetClipboardSequenceNumber();
        const bool listening = window != nullptr && AddClipboardFormatListener(window);
        if (!listening) spdlog::warn("Clipboard change notifications unavailable (error {})", GetLastError());
        started.set_value();

        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (message.hwnd == nullptr && message.message == wake_message) {
                serve_tasks();
            } else {
                DispatchMessageW(&message);
            }
        }
        if (window) {
            if (listening) RemoveClipboardFormatListener(window);
            DestroyWindow(window);
        }
        UnregisterClassW(class_name, instance);
        if (SUCCEEDED(com)) CoUninitialize();
    }
};

ClipboardMonitor::ClipboardMonitor(ChangeCallback on_change) : impl_(std::make_unique<Impl>()) {
    impl_->on_change = std::move(on_change);
    std::promise<void> started;
    auto ready = started.get_future();
    impl_->thread = std::jthread([this, &started] { impl_->run(started); });
    ready.wait();
}

ClipboardMonitor::~ClipboardMonitor() {
    PostThreadMessageW(impl_->thread_id, WM_QUIT, 0, 0);
    impl_->thread.join();
}

void ClipboardMonitor::read(unsigned formats, std::size_t max_bytes, ReadCallback done) {
    impl_->post([impl = impl_.get(), request = std::make_shared<Impl::Request>(formats, max_bytes, std::move(done))] {
        try {
            impl->serve(*request);
        } catch (const std::exception& error) {
            spdlog::error("Clipboard read failed: {}", error.what());
            request->done({.content = std::nullopt, .error = "clipboard read failed"});
        }
    });
}

void ClipboardMonitor::write(ClipboardWrite content, std::function<void(ClipboardWriteResult)> done) {
    impl_->post([impl = impl_.get(),
                 request = std::make_shared<Impl::WriteRequest>(std::move(content), std::move(done))] {
        try {
            impl->serve_write(*request);
        } catch (const std::exception& error) {
            spdlog::error("Clipboard write failed: {}", error.what());
            request->done({.sequence = 0, .error_code = "clipboard_unavailable", .error = "clipboard write failed"});
        }
    });
}

} // namespace beamit::host
