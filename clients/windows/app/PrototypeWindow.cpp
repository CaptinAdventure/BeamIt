#include "PrototypeWindow.hpp"

#include "DecoderProbe.hpp"

#include <microsoft.ui.xaml.window.h>
#include <shellapi.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Windows.System.h>
#include <algorithm>
#include <cmath>
#include <string>

namespace beamit::client {
namespace {

constexpr UINT tray_message = WM_APP + 1;
constexpr UINT show_command = 0x1000;
constexpr UINT fullscreen_command = 0x1010;
constexpr UINT popup_command = 0x1020;
constexpr UINT decoder_command = 0x1030;
constexpr UINT exit_command = 0x1040;
constexpr UINT icon_id = 1;
constexpr wchar_t popup_class[] = L"BeamIt.Client.PopupPrototype";

void append_menu(HMENU menu, UINT flags, UINT_PTR id, const wchar_t* label) {
    winrt::check_bool(AppendMenuW(menu, flags, id, label));
}

} // namespace

class Popup {
public:
    explicit Popup(HWND parent) {
        WNDCLASSEXW klass{sizeof(WNDCLASSEXW)};
        klass.hInstance = GetModuleHandleW(nullptr);
        klass.lpfnWndProc = procedure;
        klass.lpszClassName = popup_class;
        klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!RegisterClassExW(&klass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            winrt::throw_last_error();
        }
        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, popup_class,
            L"BeamIt popup prototype", WS_POPUP, 0, 0, 320, 180, parent, nullptr, klass.hInstance, this);
        winrt::check_bool(hwnd_);
        try {
            surface_ = std::make_unique<Surface>(hwnd_, 320, 180);
            position(parent);
            surface_->present();
        } catch (...) {
            DestroyWindow(hwnd_);
            throw;
        }
    }

    ~Popup() {
        surface_.reset();
        if (hwnd_) DestroyWindow(hwnd_);
    }

    void position(HWND parent) {
        POINT origin{40, 100};
        winrt::check_bool(ClientToScreen(parent, &origin));
        winrt::check_bool(SetWindowPos(hwnd_, HWND_TOP, origin.x, origin.y, 320, 180,
            SWP_NOACTIVATE | SWP_SHOWWINDOW));
    }

    void hide() { ShowWindow(hwnd_, SW_HIDE); }

private:
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{};
            BeginPaint(window, &paint);
            EndPaint(window, &paint);
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    HWND hwnd_{};
    std::unique_ptr<Surface> surface_;
};

std::shared_ptr<PrototypeWindow> PrototypeWindow::create() {
    auto result = std::shared_ptr<PrototypeWindow>(new PrototypeWindow);
    result->initialize();
    return result;
}

void PrototypeWindow::initialize() {
    using namespace winrt::Microsoft::UI::Xaml;
    using namespace winrt::Microsoft::UI::Xaml::Controls;
    window_ = Window();
    window_.Title(L"BeamIt - Windows integration prototype");
    winrt::check_hresult(window_.as<IWindowNative>()->get_WindowHandle(&hwnd_));

    StackPanel content;
    content.Spacing(12);
    content.Margin(Thickness{20, 20, 20, 20});
    TextBlock title;
    title.Text(L"Windows integration prototype");
    title.FontSize(24);
    content.Children().Append(title);
    TextBlock explanation;
    explanation.Text(L"No host connection or remote input yet. The blue surface exercises D3D11/WinUI composition.\n"
        L"Close hides to the tray. Alt+Space opens local options, including in fullscreen.");
    explanation.TextWrapping(TextWrapping::Wrap);
    content.Children().Append(explanation);

    const auto weak = weak_from_this();
    const auto button = [&](const wchar_t* text, UINT id) {
        Button control;
        control.Content(winrt::box_value(text));
        control.Click([weak, id](auto&&, auto&&) {
            if (const auto self = weak.lock()) {
                try {
                    self->command(id);
                } catch (winrt::hresult_error const& error) {
                    self->report_error(error.message());
                } catch (std::exception const& error) {
                    self->report_error(winrt::to_hstring(error.what()));
                }
            }
        });
        content.Children().Append(control);
    };
    button(L"Fullscreen / restore", fullscreen_command);
    button(L"Show nonactivating owned popup prototype", popup_command);
    button(L"List installed Media Foundation decoders", decoder_command);

    panel_ = SwapChainPanel();
    panel_.Height(300);
    panel_.Loaded([weak](auto&&, auto&&) {
        if (const auto self = weak.lock()) {
            try {
                self->surface_ = std::make_unique<Surface>(self->panel_);
                self->resize_surface();
            } catch (winrt::hresult_error const& error) {
                self->report_error(error.message());
            }
        }
    });
    panel_.SizeChanged([weak](auto&&, auto&&) {
        if (const auto self = weak.lock()) {
            try {
                self->resize_surface();
            } catch (winrt::hresult_error const& error) {
                self->report_error(error.message());
            }
        }
    });
    content.Children().Append(panel_);
    content.KeyDown([weak](auto&&, winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& event) {
        if (const auto self = weak.lock(); self && self->fullscreen_ &&
            event.Key() == winrt::Windows::System::VirtualKey::Escape && !event.KeyStatus().WasKeyDown) {
            SendMessageW(self->hwnd_, WM_KEYDOWN, VK_ESCAPE, 0);
            event.Handled(true);
        }
    });
    window_.Content(content);
    winrt::check_bool(SetWindowSubclass(hwnd_, subclass, 1, reinterpret_cast<DWORD_PTR>(this)));
    taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
    winrt::check_bool(taskbar_created_ != 0);

    const auto menu = GetSystemMenu(hwnd_, FALSE);
    winrt::check_bool(menu);
    append_menu(menu, MF_SEPARATOR, 0, nullptr);
    append_menu(menu, MF_STRING, fullscreen_command, L"Fullscreen / restore");
    append_menu(menu, MF_STRING, popup_command, L"Show popup prototype");
    append_menu(menu, MF_STRING, decoder_command, L"List Media Foundation decoders");
    tray_added_ = add_tray();
    window_.Activate();
    if (!tray_added_) report_error(L"Could not register the tray icon. Close will keep this window visible.");
}

PrototypeWindow::~PrototypeWindow() {
    remove_tray();
    popup_.reset();
    if (hwnd_) RemoveWindowSubclass(hwnd_, subclass, 1);
}

bool PrototypeWindow::add_tray() {
    NOTIFYICONDATAW data{sizeof(NOTIFYICONDATAW)};
    data.hWnd = hwnd_;
    data.uID = icon_id;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = tray_message;
    data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(data.szTip, L"BeamIt - integration prototype");
    if (!Shell_NotifyIconW(NIM_ADD, &data)) return false;
    data.uVersion = NOTIFYICON_VERSION_4;
    if (!Shell_NotifyIconW(NIM_SETVERSION, &data)) {
        Shell_NotifyIconW(NIM_DELETE, &data);
        return false;
    }
    return true;
}

void PrototypeWindow::remove_tray() {
    if (!tray_added_) return;
    NOTIFYICONDATAW data{sizeof(NOTIFYICONDATAW)};
    data.hWnd = hwnd_;
    data.uID = icon_id;
    Shell_NotifyIconW(NIM_DELETE, &data);
    tray_added_ = false;
}

void PrototypeWindow::report_error(std::wstring_view message) const {
    const std::wstring text(message);
    OutputDebugStringW((L"BeamIt: " + text + L"\n").c_str());
    MessageBoxW(hwnd_, text.c_str(), L"BeamIt prototype error", MB_OK | MB_ICONERROR);
}

void PrototypeWindow::show() {
    ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(hwnd_);
}

void PrototypeWindow::hide() {
    if (!tray_added_) {
        show();
        report_error(L"Cannot hide without a tray icon. Use the taskbar or system menu to keep this window accessible.");
        return;
    }
    if (popup_) popup_->hide();
    ShowWindow(hwnd_, SW_HIDE);
}

void PrototypeWindow::exit() {
    exiting_ = true;
    remove_tray();
    popup_.reset();
    surface_.reset();
    window_.Close();
    winrt::Microsoft::UI::Xaml::Application::Current().Exit();
}

void PrototypeWindow::toggle_fullscreen() {
    using namespace winrt::Microsoft::UI::Windowing;
    if (!fullscreen_) {
        winrt::check_bool(GetWindowPlacement(hwnd_, &windowed_));
        window_.AppWindow().SetPresenter(AppWindowPresenterKind::FullScreen);
        fullscreen_ = true;
    } else {
        window_.AppWindow().SetPresenter(AppWindowPresenterKind::Overlapped);
        winrt::check_bool(SetWindowPlacement(hwnd_, &windowed_));
        fullscreen_ = false;
    }
    last_escape_ = 0;
}

void PrototypeWindow::system_menu() {
    POINT point{};
    winrt::check_bool(ClientToScreen(hwnd_, &point));
    const auto selected = TrackPopupMenu(GetSystemMenu(hwnd_, FALSE),
        TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
        point.x, point.y, 0, hwnd_, nullptr);
    if (selected) SendMessageW(hwnd_, WM_SYSCOMMAND, selected, 0);
}

void PrototypeWindow::tray_menu() {
    const HMENU menu = CreatePopupMenu();
    winrt::check_bool(menu);
    try {
        append_menu(menu, MF_STRING, show_command, L"Open BeamIt prototype");
        append_menu(menu, MF_STRING, fullscreen_command, L"Fullscreen / restore");
        append_menu(menu, MF_STRING, popup_command, L"Show popup prototype");
        append_menu(menu, MF_STRING, decoder_command, L"List Media Foundation decoders");
        append_menu(menu, MF_SEPARATOR, 0, nullptr);
        append_menu(menu, MF_STRING, exit_command, L"Exit");
        POINT point{};
        winrt::check_bool(GetCursorPos(&point));
        SetForegroundWindow(hwnd_);
        const auto selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
            point.x, point.y, 0, hwnd_, nullptr);
        PostMessageW(hwnd_, WM_NULL, 0, 0);
        DestroyMenu(menu);
        if (selected) command(selected);
    } catch (...) {
        // DestroyMenu must run if menu construction fails, before propagating the error to the UI boundary.
        if (IsMenu(menu)) DestroyMenu(menu);
        throw;
    }
}

void PrototypeWindow::show_popup() {
    show();
    popup_ = std::make_unique<Popup>(hwnd_);
}

void PrototypeWindow::decoder_inventory() {
    const auto entries = decoder_inventory_report();
    MessageBoxW(hwnd_, entries.c_str(), L"Media Foundation decoder enumeration (not a decode test)", MB_OK);
}

void PrototypeWindow::resize_surface() {
    if (!surface_) return;
    const auto root = panel_.XamlRoot();
    const auto scale = root ? root.RasterizationScale() : 1.0;
    const auto width = std::lround(panel_.ActualWidth() * scale);
    const auto height = std::lround(panel_.ActualHeight() * scale);
    if (width <= 0 || height <= 0) return;
    surface_->resize(static_cast<unsigned>(width), static_cast<unsigned>(height));
    surface_->present();
}

void PrototypeWindow::command(UINT id) {
    switch (id) {
    case show_command: show(); break;
    case fullscreen_command: show(); toggle_fullscreen(); break;
    case popup_command: show_popup(); break;
    case decoder_command: decoder_inventory(); break;
    case exit_command: exit(); break;
    default: break;
    }
}

LRESULT CALLBACK PrototypeWindow::subclass(HWND, UINT message, WPARAM wparam, LPARAM lparam,
                                           UINT_PTR, DWORD_PTR data) {
    const auto self = reinterpret_cast<PrototypeWindow*>(data);
    try {
        return self->message(message, wparam, lparam);
    } catch (winrt::hresult_error const& error) {
        self->report_error(error.message());
    } catch (std::exception const& error) {
        self->report_error(winrt::to_hstring(error.what()));
    }
    return 0;
}

LRESULT PrototypeWindow::message(UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == taskbar_created_ && taskbar_created_ != 0) {
        tray_added_ = add_tray();
        if (!tray_added_) {
            show();
            report_error(L"Could not restore the tray icon after Explorer restarted.");
        }
        return 0;
    }
    switch (message) {
    case WM_CLOSE:
        if (!exiting_) { hide(); return 0; }
        break;
    case WM_SYSKEYDOWN:
        if (wparam == VK_SPACE) { system_menu(); return 0; }
        break;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE && fullscreen_ && (lparam & (1LL << 30)) == 0) {
            const auto now = GetTickCount64();
            const auto previous = last_escape_;
            last_escape_ = now;
            if (previous != 0 && now - previous <= 1000) toggle_fullscreen();
            return 0;
        }
        break;
    case WM_SYSCOMMAND:
        if (const auto id = static_cast<UINT>(wparam & 0xFFF0); id >= show_command && id <= exit_command) {
            command(id);
            return 0;
        }
        break;
    case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_INACTIVE && popup_) popup_->hide();
        break;
    case WM_MOVE:
        if (popup_ && GetForegroundWindow() == hwnd_) popup_->position(hwnd_);
        break;
    case WM_DPICHANGED:
        if (surface_) resize_surface();
        break;
    case tray_message:
        switch (LOWORD(lparam)) {
        case NIN_SELECT:
        case NIN_KEYSELECT: show(); break;
        case WM_CONTEXTMENU: tray_menu(); break;
        default: break;
        }
        return 0;
    case WM_NCDESTROY:
        remove_tray();
        RemoveWindowSubclass(hwnd_, subclass, 1);
        {
            const auto result = DefSubclassProc(hwnd_, message, wparam, lparam);
            hwnd_ = nullptr;
            return result;
        }
    default: break;
    }
    return DefSubclassProc(hwnd_, message, wparam, lparam);
}

} // namespace beamit::client
