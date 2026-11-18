#pragma once

#include "AppUtil.hpp"
#include "AudioClient.hpp"
#include "ClientSession.hpp"
#include "ClipboardManager.hpp"
#include "HostWindows.hpp"
#include "StreamWindow.hpp"

#include <windows.h>

#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

namespace beamit::client {

// Owns the single host session of this process, the connection/source window, the tray icon
// and all stream windows. Runs on the UI thread.
class ClientApp : public std::enable_shared_from_this<ClientApp> {
public:
    static std::shared_ptr<ClientApp> create();
    ~ClientApp();

private:
    ClientApp() = default;
    void initialize();
    void build_connect_view();
    void build_source_view();
    void update_connect_state();
    void refresh_sources();
    void refresh_streams();
    void set_error(const std::string& message);
    void show_window();
    void hide_window();

    void connect();
    void on_connected();
    void end_session(std::optional<std::string> error);
    void handle_message(const json::object& message);
    void start_audio();
    void stop_audio(bool notify_host);
    void audio_failed(std::string message);
    void clipboard_context();
    void clipboard_status(ClipboardManager::Status status);
    winrt::fire_and_forget confirm_clipboard(std::uint32_t roots, std::uint64_t bytes, std::function<void(bool)> done);

    void start_stream(json::object source, bool activate, bool from_picker, std::optional<std::int64_t> follow);
    void reconcile();
    void place(StreamWindow& window, const std::string& hwnd);
    void stream_closed(const std::string& stream_id);

    bool add_tray();
    void remove_tray();
    void tray_menu();
    void exit();
    static LRESULT CALLBACK subclass(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT message(UINT, WPARAM, LPARAM);

    Settings settings_;
    std::unique_ptr<Session> session_;
    std::shared_ptr<AudioClient> audio_client_;
    std::string audio_id_;
    std::unique_ptr<ClipboardManager> clipboard_;
    bool connecting_{};
    HostWindowList host_windows_;
    std::map<std::string, std::shared_ptr<StreamWindow>> streams_;
    std::set<std::int64_t> followed_;
    std::set<std::string> handled_;
    int starting_{};

    winrt::Microsoft::UI::Xaml::Window window_{nullptr};
    HWND hwnd_{};
    UINT taskbar_created_{};
    bool tray_added_{};
    bool exiting_{};

    winrt::Microsoft::UI::Xaml::Controls::StackPanel connect_view_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::StackPanel source_view_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBox url_box_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::PasswordBox password_box_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBox fingerprint_box_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::CheckBox trust_box_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock trust_hint_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button connect_button_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ProgressRing progress_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock connect_error_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock source_error_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock host_text_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::StackPanel streams_panel_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::StackPanel applications_panel_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock applications_count_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBox search_box_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ComboBox mode_box_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ComboBox audio_source_box_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button audio_button_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button stop_audio_button_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock audio_status_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock clipboard_text_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button clipboard_cancel_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ContentDialog clipboard_dialog_{nullptr};
};

} // namespace beamit::client
