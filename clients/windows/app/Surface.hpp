#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <winrt/base.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

namespace beamit::client {

class Surface {
public:
    explicit Surface(winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel const& panel);
    Surface(HWND window, unsigned width, unsigned height);
    void resize(unsigned width, unsigned height);
    void present();

private:
    void create_device();
    void create_swap_chain(HWND window, unsigned width, unsigned height);
    winrt::com_ptr<ID3D11Device> device_;
    winrt::com_ptr<ID3D11DeviceContext> context_;
    winrt::com_ptr<IDXGISwapChain1> swap_chain_;
    unsigned width_{};
    unsigned height_{};
};

} // namespace beamit::client
