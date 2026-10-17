#include "host/WindowIcon.hpp"

#include "host/WindowTracker.hpp"

#include <shellapi.h>
#include <shlobj.h>
#include <spdlog/spdlog.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>

namespace beamit::host {
namespace {

using Microsoft::WRL::ComPtr;

// Width of an icon's colour (or, for monochrome icons, mask) bitmap; 0 if unknown.
std::uint32_t icon_size(HICON icon) {
    ICONINFO info{};
    if (!icon || !GetIconInfo(icon, &info)) return 0;
    BITMAP bitmap{};
    const HBITMAP measured = info.hbmColor ? info.hbmColor : info.hbmMask;
    const bool known = GetObjectW(measured, sizeof(bitmap), &bitmap) != 0;
    if (info.hbmColor) DeleteObject(info.hbmColor);
    if (info.hbmMask) DeleteObject(info.hbmMask);
    return known ? static_cast<std::uint32_t>(std::max<LONG>(bitmap.bmWidth, 0)) : 0;
}

HICON get_icon_message(HWND window, WPARAM which) {
    DWORD_PTR result{};
    if (!SendMessageTimeoutW(window, WM_GETICON, which, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 200, &result)) return nullptr;
    return reinterpret_cast<HICON>(result);
}

// Icons owned by the window or its class; never destroyed by the caller.
HICON borrowed_window_icon(HWND window) {
    if (const HICON icon = get_icon_message(window, ICON_BIG)) return icon;
    if (const auto icon = reinterpret_cast<HICON>(GetClassLongPtrW(window, GCLP_HICON))) return icon;
    if (const HICON icon = get_icon_message(window, ICON_SMALL2)) return icon;
    return reinterpret_cast<HICON>(GetClassLongPtrW(window, GCLP_HICONSM));
}

struct OwnedIcon {
    HICON icon{};
    OwnedIcon() = default;
    OwnedIcon(const OwnedIcon&) = delete;
    OwnedIcon& operator=(const OwnedIcon&) = delete;
    ~OwnedIcon() {
        if (icon) DestroyIcon(icon);
    }
};

} // namespace

std::optional<WindowIcon> png_from_icon(HICON icon, std::uint32_t max_size) {
    if (!icon || max_size == 0) return std::nullopt;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<IWICBitmapSource> source;
    ComPtr<IStream> output;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> target;
    UINT width{};
    UINT height{};
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    // Produces straight (non-premultiplied) 32bpp BGRA, applying the AND mask for icons without alpha.
    if (SUCCEEDED(result)) result = factory->CreateBitmapFromHICON(icon, &bitmap);
    if (SUCCEEDED(result)) result = bitmap->GetSize(&width, &height);
    if (SUCCEEDED(result) && (width == 0 || height == 0)) result = E_FAIL;
    if (SUCCEEDED(result)) {
        source = bitmap;
        if (width > max_size || height > max_size) {
            const double scale = static_cast<double>(max_size) / std::max(width, height);
            const auto scaled_width = std::max<UINT>(1, static_cast<UINT>(width * scale + 0.5));
            const auto scaled_height = std::max<UINT>(1, static_cast<UINT>(height * scale + 0.5));
            ComPtr<IWICBitmapScaler> scaler;
            result = factory->CreateBitmapScaler(&scaler);
            if (SUCCEEDED(result)) {
                result = scaler->Initialize(bitmap.Get(), scaled_width, scaled_height,
                                            WICBitmapInterpolationModeHighQualityCubic);
            }
            if (SUCCEEDED(result)) {
                source = scaler;
                width = scaled_width;
                height = scaled_height;
            }
        }
    }
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(result)) result = CreateStreamOnHGlobal(nullptr, TRUE, &output);
    if (SUCCEEDED(result)) result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(result)) result = encoder->Initialize(output.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(result)) result = encoder->CreateNewFrame(&target, nullptr);
    if (SUCCEEDED(result)) result = target->Initialize(nullptr);
    if (SUCCEEDED(result)) result = target->SetSize(width, height);
    if (SUCCEEDED(result)) result = target->SetPixelFormat(&format);
    if (SUCCEEDED(result)) result = target->WriteSource(source.Get(), nullptr);
    if (SUCCEEDED(result)) result = target->Commit();
    if (SUCCEEDED(result)) result = encoder->Commit();
    HGLOBAL memory{};
    STATSTG stat{};
    if (SUCCEEDED(result)) result = GetHGlobalFromStream(output.Get(), &memory);
    if (SUCCEEDED(result)) result = output->Stat(&stat, STATFLAG_NONAME);
    if (FAILED(result)) {
        spdlog::warn("Icon to PNG conversion failed (HRESULT 0x{:08X})", static_cast<std::uint32_t>(result));
        return std::nullopt;
    }
    const auto* data = static_cast<const std::byte*>(GlobalLock(memory));
    if (!data) return std::nullopt;
    WindowIcon image{.png = std::vector<std::byte>(data, data + stat.cbSize.QuadPart), .width = width,
                     .height = height};
    GlobalUnlock(memory);
    return image;
}

std::optional<WindowIcon> window_icon(HWND window, std::uint32_t max_size) {
    max_size = std::clamp<std::uint32_t>(max_size, 1, max_window_icon_size);
    const HICON own = borrowed_window_icon(window);
    if (own && icon_size(own) >= max_size) {
        if (auto image = png_from_icon(own, max_size)) {
            image->source = "window";
            return image;
        }
    }
    DWORD pid{};
    GetWindowThreadProcessId(window, &pid);
    if (const auto path = pid ? process_image_path(pid) : std::wstring{}; !path.empty()) {
        OwnedIcon extracted;
        if (SHDefExtractIconW(path.c_str(), 0, 0, &extracted.icon, nullptr, max_size) == S_OK && extracted.icon) {
            if (auto image = png_from_icon(extracted.icon, max_size)) {
                image->source = "executable";
                return image;
            }
        }
    }
    if (own) {
        if (auto image = png_from_icon(own, max_size)) {
            image->source = "window";
            return image;
        }
    }
    return std::nullopt;
}

} // namespace beamit::host
