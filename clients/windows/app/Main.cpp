#include "ClientApp.hpp"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.UI.Xaml.Interop.h>

namespace {

void log_crash(std::wstring const& text) {
    OutputDebugStringW((text + L"\n").c_str());
    wchar_t path[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH)) return;
    std::wstring file = std::wstring(path) + L"\\BeamIt";
    CreateDirectoryW(file.c_str(), nullptr);
    file += L"\\client-crash.log";
    FILE* out = nullptr;
    if (_wfopen_s(&out, file.c_str(), L"a, ccs=UTF-8") == 0 && out) {
        fwprintf(out, L"%s\n", text.c_str());
        fclose(out);
    }
}

// Code-only WinUI apps must provide XAML type metadata themselves; without it the Fluent
// XamlControlsResources cannot resolve their control types, theme resources go missing and XAML
// eventually terminates the process (0xC000027B in Microsoft.UI.Xaml.dll).
struct App : winrt::Microsoft::UI::Xaml::ApplicationT<App, winrt::Microsoft::UI::Xaml::Markup::IXamlMetadataProvider> {
    winrt::Microsoft::UI::Xaml::Markup::IXamlType GetXamlType(winrt::Windows::UI::Xaml::Interop::TypeName const& type) {
        return metadata.GetXamlType(type);
    }
    winrt::Microsoft::UI::Xaml::Markup::IXamlType GetXamlType(winrt::hstring const& fullName) {
        return metadata.GetXamlType(fullName);
    }
    winrt::com_array<winrt::Microsoft::UI::Xaml::Markup::XmlnsDefinition> GetXmlnsDefinitions() {
        return metadata.GetXmlnsDefinitions();
    }

    winrt::Microsoft::UI::Xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider metadata;

    App() {
        UnhandledException([](auto&&, winrt::Microsoft::UI::Xaml::UnhandledExceptionEventArgs const& args) {
            wchar_t code[16]{};
            swprintf_s(code, L"0x%08X", static_cast<unsigned>(args.Exception().value));
            log_crash(L"BeamIt: unhandled exception " + std::wstring(code) + L": " + std::wstring(args.Message()));
        });
    }

    void OnLaunched(winrt::Microsoft::UI::Xaml::LaunchActivatedEventArgs const&) {
        try {
            Resources().MergedDictionaries().Append(winrt::Microsoft::UI::Xaml::Controls::XamlControlsResources());
        } catch (winrt::hresult_error const& error) {
            OutputDebugStringW((L"BeamIt: Fluent control resources unavailable: " + error.message() + L"\n").c_str());
        }
        client = beamit::client::ClientApp::create();
    }

    std::shared_ptr<beamit::client::ClientApp> client;
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
