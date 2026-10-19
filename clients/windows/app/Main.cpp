#include "PrototypeWindow.hpp"

#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.ApplicationModel.Activation.h>

namespace {

struct App : winrt::Microsoft::UI::Xaml::ApplicationT<App> {
    void OnLaunched(winrt::Microsoft::UI::Xaml::LaunchActivatedEventArgs const&) {
        window = beamit::client::PrototypeWindow::create();
    }

    std::shared_ptr<beamit::client::PrototypeWindow> window;
};

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        winrt::Microsoft::UI::Xaml::Application::Start([](auto&&) { winrt::make<App>(); });
        return 0;
    } catch (winrt::hresult_error const& error) {
        MessageBoxW(nullptr, error.message().c_str(), L"BeamIt startup failed", MB_OK | MB_ICONERROR);
    } catch (std::exception const& error) {
        MessageBoxW(nullptr, winrt::to_hstring(error.what()).c_str(), L"BeamIt startup failed", MB_OK | MB_ICONERROR);
    }
    return 1;
}
