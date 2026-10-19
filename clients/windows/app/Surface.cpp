#include "Surface.hpp"

#include <microsoft.ui.xaml.media.dxinterop.h>

namespace beamit::client {

void Surface::create_device() {
    constexpr D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, device_.put(), nullptr, context_.put()));
}

void Surface::create_swap_chain(HWND window, unsigned width, unsigned height) {
    auto dxgi_device = device_.as<IDXGIDevice>();
    winrt::com_ptr<IDXGIAdapter> adapter;
    winrt::check_hresult(dxgi_device->GetAdapter(adapter.put()));
    winrt::com_ptr<IDXGIFactory2> factory;
    winrt::check_hresult(adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void()));

    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = width;
    description.Height = height;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    description.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (window) {
        winrt::check_hresult(factory->CreateSwapChainForHwnd(device_.get(), window, &description,
            nullptr, nullptr, swap_chain_.put()));
    } else {
        winrt::check_hresult(factory->CreateSwapChainForComposition(device_.get(), &description,
            nullptr, swap_chain_.put()));
    }
    winrt::check_hresult(swap_chain_.as<IDXGISwapChain2>()->SetMaximumFrameLatency(1));
    width_ = width;
    height_ = height;
}

Surface::Surface(winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel const& panel) {
    create_device();
    create_swap_chain(nullptr, 640, 360);
    winrt::check_hresult(panel.as<ISwapChainPanelNative>()->SetSwapChain(swap_chain_.get()));
}

Surface::Surface(HWND window, unsigned width, unsigned height) {
    create_device();
    create_swap_chain(window, width, height);
}

void Surface::resize(unsigned width, unsigned height) {
    if (width == 0 || height == 0 || (width == width_ && height == height_)) return;
    context_->ClearState();
    winrt::check_hresult(swap_chain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN,
        DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT));
    width_ = width;
    height_ = height;
}

void Surface::present() {
    winrt::com_ptr<ID3D11Texture2D> buffer;
    winrt::check_hresult(swap_chain_->GetBuffer(0, __uuidof(ID3D11Texture2D), buffer.put_void()));
    winrt::com_ptr<ID3D11RenderTargetView> view;
    winrt::check_hresult(device_->CreateRenderTargetView(buffer.get(), nullptr, view.put()));
    constexpr float color[]{0.035f, 0.12f, 0.23f, 1.0f};
    context_->ClearRenderTargetView(view.get(), color);
    winrt::check_hresult(swap_chain_->Present(0, 0));
}

} // namespace beamit::client
