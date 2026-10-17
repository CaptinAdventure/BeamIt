#include "beamit/capture/WgcCapturer.hpp"
#include "capture/CaptureClock.hpp"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/base.h>

#include <chrono>
#include <iterator>
#include <utility>

namespace beamit::capture {

struct WgcCapturer::Impl {
    CaptureTarget target{};
    CaptureOptions options{};
    Microsoft::WRL::ComPtr<ID3D11Device> d3d_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d_context;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice winrt_device{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem capture_item{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool frame_pool{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session{nullptr};
    winrt::Windows::Graphics::SizeInt32 frame_size{};
    HRESULT last_error{S_OK};
    bool configured{false};
    bool running{false};
};

WgcCapturer::WgcCapturer() : impl_(std::make_unique<Impl>()) {}

WgcCapturer::~WgcCapturer() {
    stop();
}

std::wstring_view WgcCapturer::backend_name() const noexcept {
    return L"Windows.Graphics.Capture";
}

bool WgcCapturer::supports_target(const CaptureTarget& target) const noexcept {
    return target.kind == CaptureTargetKind::Desktop ||
           target.kind == CaptureTargetKind::Window;
}

bool WgcCapturer::configure(const CaptureTarget& target, const CaptureOptions& options) {
    stop();
    if (!supports_target(target) ||
        (target.kind == CaptureTargetKind::Window && !IsWindow(target.window))) {
        impl_->last_error = E_INVALIDARG;
        impl_->configured = false;
        return false;
    }

    impl_->target = target;
    impl_->options = options;
    impl_->configured = true;
    impl_->last_error = S_OK;
    return true;
}

bool WgcCapturer::start() {
    if (impl_->running) {
        return true;
    }
    if (!impl_->configured) {
        impl_->last_error = E_UNEXPECTED;
        return false;
    }

    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        if (!winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported()) {
            impl_->last_error = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
            return false;
        }

        if (impl_->target.kind == CaptureTargetKind::Window && !IsWindow(impl_->target.window)) {
            impl_->last_error = HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
            return false;
        }
        const HMONITOR monitor = impl_->target.kind == CaptureTargetKind::Window
                                     ? MonitorFromWindow(impl_->target.window, MONITOR_DEFAULTTONEAREST)
                                     : impl_->target.monitor != nullptr
                                     ? impl_->target.monitor
                                     : MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
        if (monitor == nullptr) {
            impl_->last_error = E_FAIL;
            return false;
        }

        Microsoft::WRL::ComPtr<IDXGIFactory1> dxgi_factory;
        HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&dxgi_factory));
        if (FAILED(result)) {
            impl_->last_error = result;
            return false;
        }

        Microsoft::WRL::ComPtr<IDXGIAdapter> capture_adapter;
        for (UINT adapter_index = 0; !capture_adapter; ++adapter_index) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            result = dxgi_factory->EnumAdapters1(adapter_index, &adapter);
            if (result == DXGI_ERROR_NOT_FOUND) {
                impl_->last_error = DXGI_ERROR_NOT_FOUND;
                return false;
            }
            if (FAILED(result)) {
                impl_->last_error = result;
                return false;
            }

            for (UINT output_index = 0;; ++output_index) {
                Microsoft::WRL::ComPtr<IDXGIOutput> output;
                result = adapter->EnumOutputs(output_index, &output);
                if (result == DXGI_ERROR_NOT_FOUND) {
                    break;
                }
                if (FAILED(result)) {
                    impl_->last_error = result;
                    return false;
                }

                DXGI_OUTPUT_DESC output_description{};
                result = output->GetDesc(&output_description);
                if (FAILED(result)) {
                    impl_->last_error = result;
                    return false;
                }
                if (output_description.Monitor == monitor) {
                    capture_adapter = adapter;
                    break;
                }
            }
        }

        constexpr D3D_FEATURE_LEVEL feature_levels[]{
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };
        D3D_FEATURE_LEVEL selected_feature_level{};
        result = D3D11CreateDevice(
            capture_adapter.Get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            feature_levels,
            static_cast<UINT>(std::size(feature_levels)),
            D3D11_SDK_VERSION,
            &impl_->d3d_device,
            &selected_feature_level,
            &impl_->d3d_context);
        if (FAILED(result)) {
            impl_->last_error = result;
            stop();
            return false;
        }

        Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
        result = impl_->d3d_device.As(&dxgi_device);
        if (FAILED(result)) {
            impl_->last_error = result;
            stop();
            return false;
        }

        auto capture_item_interop = winrt::get_activation_factory<
            winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
            IGraphicsCaptureItemInterop>();

        if (impl_->target.kind == CaptureTargetKind::Window) {
            result = capture_item_interop->CreateForWindow(
                impl_->target.window,
                winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
                winrt::put_abi(impl_->capture_item));
        } else {
            result = capture_item_interop->CreateForMonitor(
                monitor,
                winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
                winrt::put_abi(impl_->capture_item));
        }
        if (FAILED(result)) {
            impl_->last_error = result;
            stop();
            return false;
        }

        winrt::com_ptr<::IInspectable> inspectable_device;
        result = CreateDirect3D11DeviceFromDXGIDevice(
            dxgi_device.Get(),
            inspectable_device.put());
        if (FAILED(result)) {
            impl_->last_error = result;
            stop();
            return false;
        }
        impl_->winrt_device =
            inspectable_device.as<
                winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();

        const auto size = impl_->capture_item.Size();
        impl_->frame_size = size;
        impl_->frame_pool =
            winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
                impl_->winrt_device,
                winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                2,
                size);
        impl_->session = impl_->frame_pool.CreateCaptureSession(impl_->capture_item);
        impl_->session.IsCursorCaptureEnabled(impl_->options.capture_cursor);
        impl_->session.StartCapture();
        impl_->running = true;
        impl_->last_error = S_OK;
        return true;
    } catch (const winrt::hresult_error& error) {
        impl_->last_error = error.code();
        stop();
        return false;
    }
}

std::optional<media::CapturedFrame> WgcCapturer::try_acquire_frame() {
    if (!impl_->running || !impl_->frame_pool) {
        impl_->last_error = E_UNEXPECTED;
        return std::nullopt;
    }
    if (impl_->target.kind == CaptureTargetKind::Window && !IsWindow(impl_->target.window)) {
        impl_->last_error = HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
        return std::nullopt;
    }

    try {
        auto frame = impl_->frame_pool.TryGetNextFrame();
        if (!frame) {
            impl_->last_error = S_OK;
            return std::nullopt;
        }
        // Prefer the newest queued frame; bound the drain to the two-slot pool.
        if (auto newer = impl_->frame_pool.TryGetNextFrame()) {
            frame.Close();
            frame = std::move(newer);
        }

        const auto content_size = frame.ContentSize();
        if (content_size.Width <= 0 || content_size.Height <= 0) {
            impl_->last_error = S_OK;
            return std::nullopt;
        }
        if (content_size.Width != impl_->frame_size.Width ||
            content_size.Height != impl_->frame_size.Height) {
            impl_->frame_pool.Recreate(
                impl_->winrt_device,
                winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                2,
                content_size);
            impl_->frame_size = content_size;
            impl_->last_error = S_OK;
            return std::nullopt;
        }

        auto access = frame.Surface().as<
            ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        Microsoft::WRL::ComPtr<ID3D11Texture2D> source_texture;
        const HRESULT result = access->GetInterface(
            __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(source_texture.GetAddressOf()));
        if (FAILED(result)) {
            impl_->last_error = result;
            return std::nullopt;
        }

        D3D11_TEXTURE2D_DESC description{};
        source_texture->GetDesc(&description);
        description.Width = static_cast<UINT>(content_size.Width) & ~1U;
        description.Height = static_cast<UINT>(content_size.Height) & ~1U;
        if (description.Width == 0 || description.Height == 0) {
            impl_->last_error = E_INVALIDARG;
            return std::nullopt;
        }
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = 0;
        description.CPUAccessFlags = 0;
        description.MiscFlags = 0;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> owned_texture;
        const HRESULT copy_result =
            impl_->d3d_device->CreateTexture2D(&description, nullptr, &owned_texture);
        if (FAILED(copy_result)) {
            impl_->last_error = copy_result;
            return std::nullopt;
        }
        const D3D11_BOX box{0, 0, 0, description.Width, description.Height, 1};
        impl_->d3d_context->CopySubresourceRegion(
            owned_texture.Get(), 0, 0, 0, 0, source_texture.Get(), 0, &box);
        impl_->last_error = S_OK;
        return media::CapturedFrame{
            .texture = std::move(owned_texture),
            .captured_at = qpc_100ns_to_steady(frame.SystemRelativeTime().count()),
        };
    } catch (const winrt::hresult_error& error) {
        impl_->last_error = error.code();
        return std::nullopt;
    }
}

void WgcCapturer::stop() noexcept {
    impl_->running = false;
    try {
        if (impl_->session) {
            impl_->session.Close();
        }
        if (impl_->frame_pool) {
            impl_->frame_pool.Close();
        }
        impl_->session = nullptr;
        impl_->frame_pool = nullptr;
        impl_->capture_item = nullptr;
        impl_->winrt_device = nullptr;
    } catch (const winrt::hresult_error& error) {
        impl_->last_error = error.code();
    }
    impl_->d3d_context.Reset();
    impl_->d3d_device.Reset();
}

bool WgcCapturer::is_running() const noexcept {
    return impl_->running;
}

HRESULT WgcCapturer::last_error() const noexcept {
    return impl_->last_error;
}

} // namespace beamit::capture
