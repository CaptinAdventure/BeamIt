#pragma once

#include "Surface.hpp"

#include <windows.h>
#include <commctrl.h>
#include <memory>
#include <string_view>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

namespace beamit::client {

class Popup;

class PrototypeWindow : public std::enable_shared_from_this<PrototypeWindow> {
public:
    static std::shared_ptr<PrototypeWindow> create();
    ~PrototypeWindow();

private:
    PrototypeWindow() = default;
    void initialize();
    bool add_tray();
    void remove_tray();
    void show();
    void hide();
    void exit();
    void toggle_fullscreen();
    void system_menu();
    void tray_menu();
    void show_popup();
    void decoder_inventory();
    void resize_surface();
    void command(UINT command);
    void report_error(std::wstring_view message) const;
    static LRESULT CALLBACK subclass(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT message(UINT, WPARAM, LPARAM);

    winrt::Microsoft::UI::Xaml::Window window_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel panel_{nullptr};
    std::unique_ptr<Surface> surface_;
    std::unique_ptr<Popup> popup_;
    HWND hwnd_{};
    UINT taskbar_created_{};
    bool tray_added_{};
    bool exiting_{};
    bool fullscreen_{};
    WINDOWPLACEMENT windowed_{sizeof(WINDOWPLACEMENT)};
    ULONGLONG last_escape_{};
};

} // namespace beamit::client
