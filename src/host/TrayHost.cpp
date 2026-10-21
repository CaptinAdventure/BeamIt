#include "host/TrayHost.hpp"

#include "host/HostControl.hpp"
#include "host/ElevatedInput.hpp"
#include "host/TrayHostResources.h"
#include "net/Protocol.hpp"

#include <shellapi.h>
#include <spdlog/spdlog.h>

#include <exception>
#include <stdexcept>
#include <thread>
#include <vector>

namespace beamit::host {
namespace {

constexpr UINT tray_message = WM_APP + 1;
constexpr UINT host_finished = WM_APP + 2;
constexpr UINT password_command = 1;
constexpr UINT exit_command = 2;
constexpr UINT statistics_command = 3;
constexpr UINT elevate_command = 4;
constexpr UINT first_stream_command = 100;
constexpr wchar_t window_class[] = L"BeamItHostTray";

const wchar_t* elevation_text(InputElevation elevation) {
    switch (elevation) {
    case InputElevation::Starting:
        return L"Input handler: starting...";
    case InputElevation::Elevated:
        return L"Input handler: elevated.";
    case InputElevation::NotElevated:
        return L"WARNING: Input handler is not elevated. Administrator windows may reject remote input.\r\n"
               L"Choose Elevate input... from the tray to start the separate UAC-elevated input helper.";
    case InputElevation::Unknown:
        return L"WARNING: Input handler elevation could not be determined. Administrator-window input is uncertain.\r\n"
               L"Choose Elevate input... from the tray to start the separate UAC-elevated input helper.";
    }
    return L"";
}

std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0);
    if (!size) throw std::runtime_error("Cannot decode UTF-8 text");
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

struct PasswordDialog {
    std::string initial;
    std::optional<std::string> result;
    std::exception_ptr error;
};

INT_PTR CALLBACK password_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<PasswordDialog*>(GetWindowLongPtrW(window, DWLP_USER));
    try {
        if (message == WM_INITDIALOG) {
            state = reinterpret_cast<PasswordDialog*>(lparam);
            SetWindowLongPtrW(window, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
            SendDlgItemMessageW(window, IDC_CONNECTION_PASSWORD, EM_SETLIMITTEXT, 1024, 0);
            SetDlgItemTextW(window, IDC_CONNECTION_PASSWORD, wide(state->initial).c_str());
            if (!state->initial.empty()) {
                CheckDlgButton(window, IDC_SHOW_PASSWORD, BST_CHECKED);
                SendDlgItemMessageW(window, IDC_CONNECTION_PASSWORD, EM_SETPASSWORDCHAR, 0, 0);
            }
            return TRUE;
        }
        if (message != WM_COMMAND || !state) return FALSE;
        switch (LOWORD(wparam)) {
        case IDC_SHOW_PASSWORD:
            SendDlgItemMessageW(window, IDC_CONNECTION_PASSWORD, EM_SETPASSWORDCHAR,
                                IsDlgButtonChecked(window, IDC_SHOW_PASSWORD) == BST_CHECKED ? 0 : L'*', 0);
            InvalidateRect(GetDlgItem(window, IDC_CONNECTION_PASSWORD), nullptr, TRUE);
            return TRUE;
        case IDC_GENERATE_PASSWORD: {
            auto password = generate_connection_password();
            SetDlgItemTextW(window, IDC_CONNECTION_PASSWORD, wide(password).c_str());
            SecureZeroMemory(password.data(), password.size());
            return TRUE;
        }
        case IDOK: {
            const int length = GetWindowTextLengthW(GetDlgItem(window, IDC_CONNECTION_PASSWORD));
            if (!length) {
                MessageBoxW(window, L"Enter a connection password.", L"BeamIt", MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            std::wstring password(length + 1, L'\0');
            GetDlgItemTextW(window, IDC_CONNECTION_PASSWORD, password.data(), length + 1);
            const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, password.data(), length,
                                                 nullptr, 0, nullptr, nullptr);
            if (!size) {
                SecureZeroMemory(password.data(), password.size() * sizeof(wchar_t));
                throw std::runtime_error("Cannot encode the connection password as UTF-8");
            }
            state->result.emplace(size, '\0');
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, password.data(), length,
                                state->result->data(), size, nullptr, nullptr);
            SecureZeroMemory(password.data(), password.size() * sizeof(wchar_t));
            SetDlgItemTextW(window, IDC_CONNECTION_PASSWORD, L"");
            EndDialog(window, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            SetDlgItemTextW(window, IDC_CONNECTION_PASSWORD, L"");
            EndDialog(window, IDCANCEL);
            return TRUE;
        default:
            return FALSE;
        }
    } catch (...) {
        if (state) state->error = std::current_exception();
        EndDialog(window, IDCANCEL);
        return TRUE;
    }
}

class TrayHost {
public:
    explicit TrayHost(HostServerOptions options)
        : port_(options.port), bind_address_(options.bind_address), fingerprint_(options.identity.sha256_fingerprint),
          control_(std::make_shared<HostControl>()) {
        options.control = control_;
        WNDCLASSW wc{};
        wc.lpfnWndProc = window_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = window_class;
        if (!RegisterClassW(&wc)) throw std::runtime_error("Cannot register the BeamIt tray window");
        window_ = CreateWindowExW(0, window_class, L"BeamIt", WS_OVERLAPPED, 0, 0, 0, 0,
                                  nullptr, nullptr, wc.hInstance, this);
        if (!window_) {
            UnregisterClassW(window_class, wc.hInstance);
            throw std::runtime_error("Cannot create the BeamIt tray window");
        }
        try {
            taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
            if (!taskbar_created_) throw std::runtime_error("Cannot register the taskbar restart message");
            add_icon();
            if (!SetTimer(window_, 1, 1000, nullptr)) throw std::runtime_error("Cannot start the tray refresh timer");
            thread_ = std::thread([this, options = std::move(options)]() mutable {
                try {
                    run_host_server(std::move(options));
                } catch (...) {
                    host_error_ = std::current_exception();
                }
                PostMessageW(window_, host_finished, 0, 0);
            });
        } catch (...) {
            remove_icon();
            if (statistics_window_) DestroyWindow(statistics_window_);
            DestroyWindow(window_);
            UnregisterClassW(window_class, wc.hInstance);
            throw;
        }
    }

    ~TrayHost() {
        if (thread_.joinable()) {
            control_->submit({.kind = HostCommand::Kind::Exit});
            thread_.join();
        }
        if (elevation_thread_.joinable()) elevation_thread_.join();
        remove_icon();
        if (statistics_window_) DestroyWindow(statistics_window_);
        DestroyWindow(window_);
        UnregisterClassW(window_class, GetModuleHandleW(nullptr));
    }

    void run() {
        MSG message{};
        int result{};
        while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            if (statistics_window_ && (message.hwnd == statistics_window_ ||
                                       IsChild(statistics_window_, message.hwnd)) &&
                IsDialogMessageW(statistics_window_, &message)) continue;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (result == -1) throw std::runtime_error("BeamIt tray message loop failed");
        control_->submit({.kind = HostCommand::Kind::Exit});
        thread_.join();
        if (host_error_) std::rethrow_exception(host_error_);
    }

private:
    void elevate_input() {
        const auto status = control_->status();
        if (elevation_pending_ || !status.listening || status.input_elevation == InputElevation::Elevated) return;
        if (elevation_thread_.joinable()) elevation_thread_.join();
        elevation_thread_ = std::thread([control = control_] {
            try {
                auto helper = std::make_shared<input::ElevatedInput>();
                control->submit({.kind = HostCommand::Kind::InstallElevatedInput,
                                 .elevated_input = std::move(helper)});
            } catch (const std::exception& error) {
                spdlog::error("Could not elevate input: {}", error.what());
                control->notify({false, std::string("Could not elevate input: ") + error.what() +
                    "\n\nThe host is still running with its previous input handler."});
            }
        });
        elevation_pending_ = true;
    }

    void refresh_statistics() {
        for (const auto& notice : control_->take_notices()) {
            elevation_pending_ = false;
            if (elevation_thread_.joinable()) elevation_thread_.join();
            MessageBoxW(window_, wide(notice.message).c_str(),
                        notice.success ? L"BeamIt - Input elevated" : L"BeamIt - Input elevation failed",
                        MB_OK | (notice.success ? MB_ICONINFORMATION : MB_ICONWARNING));
        }
        const auto status = control_->status();
        if (!elevation_warned_ && (status.input_elevation == InputElevation::NotElevated ||
                                   status.input_elevation == InputElevation::Unknown)) {
            elevation_warned_ = true;
            NOTIFYICONDATAW icon{sizeof(icon)};
            icon.hWnd = window_;
            icon.uID = 1;
            icon.uFlags = NIF_INFO;
            icon.dwInfoFlags = NIIF_WARNING;
            wcscpy_s(icon.szInfoTitle, L"BeamIt - Input elevation warning");
            wcscpy_s(icon.szInfo, status.input_elevation == InputElevation::NotElevated
                ? L"Input is not elevated. Administrator windows may reject remote input. "
                  L"Choose Elevate input from the tray to try the UAC helper."
                : L"Input elevation could not be determined. Open Statistics for details.");
            if (!Shell_NotifyIconW(NIM_MODIFY, &icon)) spdlog::warn("Could not display input elevation tray warning");
        }
        if (!statistics_window_) return;
        SetDlgItemTextW(statistics_window_, IDC_INPUT_ELEVATION, elevation_text(status.input_elevation));
        std::string text = status.listening ? "Listening: " : "Starting: ";
        text += bind_address_ + ":" + std::to_string(port_) + "\r\nTLS SHA-256 fingerprint: " + fingerprint_ +
                "\r\nClient: " + (status.client.empty() ? "none" : status.client) + "\r\n";
        if (status.streams.empty()) text += "\r\nNo connected streams.\r\n";
        for (const auto& stream : status.streams) {
            text += "\r\n" + stream.description + "\r\nStream: " + stream.id + "\r\n";
            if (stream.statistics.empty()) {
                text += "Waiting for first statistics sample (up to 10 seconds).\r\n";
            } else {
                const auto age = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - stream.sampled_at).count();
                text += "Last sample: " + std::to_string(age) + " seconds ago\r\n" + stream.statistics + "\r\n";
            }
        }
        if (text != statistics_text_) {
            const auto edit = GetDlgItem(statistics_window_, IDC_HOST_STATISTICS);
            const auto first_line = SendMessageW(edit, EM_GETFIRSTVISIBLELINE, 0, 0);
            DWORD selection_start{}, selection_end{};
            SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&selection_start),
                         reinterpret_cast<LPARAM>(&selection_end));
            if (!SetWindowTextW(edit, wide(text).c_str())) throw std::runtime_error("Cannot update host statistics");
            SendMessageW(edit, EM_SETSEL, selection_start, selection_end);
            SendMessageW(edit, EM_LINESCROLL, 0, first_line);
            statistics_text_ = std::move(text);
        }
    }

    void show_statistics() {
        if (!statistics_window_) {
            statistics_window_ = CreateDialogParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_HOST_STATISTICS),
                window_, statistics_proc, reinterpret_cast<LPARAM>(this));
            if (!statistics_window_) throw std::runtime_error("Cannot open the host statistics window");
            SendDlgItemMessageW(statistics_window_, IDC_HOST_STATISTICS, EM_SETLIMITTEXT, 128 * 1024, 0);
        }
        refresh_statistics();
        ShowWindow(statistics_window_, SW_RESTORE);
        SetForegroundWindow(statistics_window_);
    }

    static INT_PTR CALLBACK statistics_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<TrayHost*>(GetWindowLongPtrW(window, DWLP_USER));
        if (message == WM_INITDIALOG) {
            SetWindowLongPtrW(window, DWLP_USER, lparam);
            return TRUE;
        }
        if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wparam) == IDCANCEL)) {
            DestroyWindow(window);
            return TRUE;
        }
        if (message == WM_NCDESTROY && self) {
            self->statistics_window_ = nullptr;
            self->statistics_text_.clear();
        }
        return FALSE;
    }

    void add_icon() {
        NOTIFYICONDATAW icon{sizeof(icon)};
        icon.hWnd = window_;
        icon.uID = 1;
        icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        icon.uCallbackMessage = tray_message;
        icon.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        const auto tip = L"BeamIt - port " + std::to_wstring(port_);
        wcscpy_s(icon.szTip, tip.c_str());
        if (!Shell_NotifyIconW(NIM_ADD, &icon)) throw std::runtime_error("Cannot add the BeamIt tray icon");
        icon.uVersion = NOTIFYICON_VERSION_4;
        if (!Shell_NotifyIconW(NIM_SETVERSION, &icon)) {
            remove_icon();
            throw std::runtime_error("Cannot configure the BeamIt tray icon");
        }
    }

    void remove_icon() const {
        NOTIFYICONDATAW icon{sizeof(icon)};
        icon.hWnd = window_;
        icon.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &icon);
    }

    void change_password() {
        auto password = connection_password_dialog(window_);
        if (!password) return;
        bool disconnect{};
        if (!control_->status().client.empty()) {
            const auto answer = MessageBoxW(window_,
                L"A client is connected. Disconnect it and all its streams when the password changes?\n\n"
                L"Choose No to leave the existing connection authorized.",
                L"BeamIt - Change connection password", MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2);
            if (answer == IDCANCEL) {
                SecureZeroMemory(password->data(), password->size());
                return;
            }
            disconnect = answer == IDYES;
        }
        try {
            HostCommand command{.kind = HostCommand::Kind::ChangePassword};
            command.auth_salt = net::random_bytes(16);
            command.auth_key = net::pbkdf2_sha256(*password, command.auth_salt, net::protocol::pbkdf2_iterations);
            command.disconnect_client = disconnect;
            control_->submit(std::move(command));
        } catch (...) {
            SecureZeroMemory(password->data(), password->size());
            throw;
        }
        SecureZeroMemory(password->data(), password->size());
    }

    void menu() {
        if (!IsWindowEnabled(window_)) {
            SetForegroundWindow(GetLastActivePopup(window_));
            return;
        }
        const auto status = control_->status();
        menu_streams_ = status.streams;
        HMENU popup = CreatePopupMenu();
        if (!popup) throw std::runtime_error("Cannot create the BeamIt tray menu");
        const auto append = [popup](UINT flags, UINT_PTR id, const wchar_t* label) {
            if (!AppendMenuW(popup, flags, id, label)) throw std::runtime_error("Cannot populate the BeamIt tray menu");
        };
        try {
            const auto header = L"BeamIt - port " + std::to_wstring(port_);
            append(MF_STRING | MF_DISABLED, 0, header.c_str());
            append(MF_STRING, password_command, L"Change connection password...");
            append(MF_STRING, statistics_command, L"Statistics...");
            append(MF_STRING | (elevation_pending_ || !status.listening ||
                               status.input_elevation == InputElevation::Elevated ? MF_DISABLED : 0),
                   elevate_command, elevation_pending_ ? L"Elevating input..." : L"Elevate input...");
            if (status.input_elevation == InputElevation::NotElevated) {
                append(MF_STRING | MF_DISABLED, 0, L"Warning: input handler is not elevated");
            } else if (status.input_elevation == InputElevation::Unknown) {
                append(MF_STRING | MF_DISABLED, 0, L"Warning: input handler elevation is unknown");
            }
            append(MF_SEPARATOR, 0, nullptr);
            const auto client = status.client.empty() ? L"No connected client" : L"Client: " + wide(status.client);
            append(MF_STRING | MF_DISABLED, 0, client.c_str());
            if (status.streams.empty()) append(MF_STRING | MF_DISABLED, 0, L"No connected streams");
            for (std::size_t i = 0; i < status.streams.size(); ++i) {
                auto label = wide(status.streams[i].description);
                if (label.size() > 80) label = label.substr(0, 77) + L"...";
                for (auto& c : label) if (c == L'&' || c == L'\t' || c == L'\r' || c == L'\n') c = L' ';
                label = L"Disconnect: " + label + L" [" + wide(status.streams[i].id.substr(0, 6)) + L"]";
                append(MF_STRING, first_stream_command + i, label.c_str());
            }
            append(MF_SEPARATOR, 0, nullptr);
            append(MF_STRING, exit_command, L"Exit");
            POINT point{};
            GetCursorPos(&point);
            SetForegroundWindow(window_);
            const auto selected = TrackPopupMenu(popup, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                                  point.x, point.y, 0, window_, nullptr);
            PostMessageW(window_, WM_NULL, 0, 0);
            DestroyMenu(popup);
            popup = nullptr;
            if (selected) PostMessageW(window_, WM_COMMAND, selected, 0);
        } catch (...) {
            if (popup) DestroyMenu(popup);
            throw;
        }
    }

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<TrayHost*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<TrayHost*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, wparam, lparam);
        try {
            if (message == WM_COMMAND) {
                const auto command = LOWORD(wparam);
                if (command == password_command) self->change_password();
                else if (command == statistics_command) self->show_statistics();
                else if (command == elevate_command) self->elevate_input();
                else if (command == exit_command) self->control_->submit({.kind = HostCommand::Kind::Exit});
                else if (command >= first_stream_command &&
                         command - first_stream_command < self->menu_streams_.size()) {
                    self->control_->submit({.kind = HostCommand::Kind::DisconnectStream,
                        .stream_id = self->menu_streams_[command - first_stream_command].id});
                }
                return 0;
            }
            if (message == WM_TIMER && wparam == 1) {
                self->refresh_statistics();
                return 0;
            }
            if (message == tray_message && (LOWORD(lparam) == WM_CONTEXTMENU ||
                                            LOWORD(lparam) == NIN_SELECT || LOWORD(lparam) == NIN_KEYSELECT)) {
                self->menu();
                return 0;
            }
            if (message == host_finished) {
                KillTimer(window, 1);
                if (self->statistics_window_) DestroyWindow(self->statistics_window_);
                // End a modal password dialog too, so a host failure cannot strand the UI.
                const auto dialog = GetLastActivePopup(window);
                if (dialog != window) SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
                PostQuitMessage(0);
                return 0;
            }
            if (message == WM_CLOSE || (message == WM_QUERYENDSESSION)) {
                self->control_->submit({.kind = HostCommand::Kind::Exit});
                return message == WM_QUERYENDSESSION ? TRUE : 0;
            }
            if (self->taskbar_created_ && message == self->taskbar_created_) {
                self->add_icon();
                return 0;
            }
        } catch (const std::exception& error) {
            spdlog::error("Tray action failed: {}", error.what());
            MessageBoxA(window, error.what(), "BeamIt", MB_OK | MB_ICONERROR);
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    unsigned short port_;
    std::string bind_address_;
    std::string fingerprint_;
    std::shared_ptr<HostControl> control_;
    HWND window_{};
    UINT taskbar_created_{};
    std::thread thread_;
    std::exception_ptr host_error_;
    std::vector<HostStreamInfo> menu_streams_;
    HWND statistics_window_{};
    std::string statistics_text_;
    bool elevation_warned_{};
    std::thread elevation_thread_;
    bool elevation_pending_{};
};

} // namespace

std::optional<std::string> connection_password_dialog(HWND owner, std::string initial) {
    PasswordDialog state{.initial = std::move(initial)};
    const auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_CONNECTION_PASSWORD),
                                        owner, password_proc, reinterpret_cast<LPARAM>(&state));
    SecureZeroMemory(state.initial.data(), state.initial.size());
    if (state.error) std::rethrow_exception(state.error);
    if (result == -1) throw std::runtime_error("Cannot open the connection password dialog");
    return std::move(state.result);
}

void run_tray_host(HostServerOptions options) { TrayHost(std::move(options)).run(); }

} // namespace beamit::host
