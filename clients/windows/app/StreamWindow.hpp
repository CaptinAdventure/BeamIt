#pragma once

#include "AppUtil.hpp"
#include "ClientSession.hpp"
#include "ClipboardManager.hpp"
#include "VideoPipeline.hpp"

#include <windows.h>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>

#include <chrono>
#include <functional>
#include <memory>
#include <set>
#include <string>

namespace beamit::client {

struct StreamSink;
struct SessionLink;
struct CursorHost;

// One stream presented as a native top-level window (PROTOCOL.md §5.3-§5.10).
class StreamWindow : public std::enable_shared_from_this<StreamWindow> {
public:
    struct Callbacks {
        std::function<void(const std::string& stream_id)> closed;  // Window closed locally (stop requested).
        std::function<void(const std::string& message)> error;    // Show in the connection window.
        std::function<void(bool)> focused;
        std::function<void(std::function<void(bool)>)> paste;
        std::function<void()> cancel_clipboard;
    };

    static std::shared_ptr<StreamWindow> create(Session& session, StreamOffer offer, Callbacks callbacks);
    ~StreamWindow();

    // Opens the stream socket; `opened` runs on the UI thread.
    void open(std::function<void(std::expected<void, SessionError>)> opened);
    void show(bool activate);
    // The host stopped the stream or the session ended: close without sending stop_stream.
    void close_after_host_stop();
    void update_attention(const std::string& hwnd, bool requesting);
    void update_video(const VideoParameters& video);
    // Host pointer shape (PROTOCOL.md §5.9.11).
    void update_cursor(std::string shape);

    [[nodiscard]] const StreamOffer& offer() const { return offer_; }
    [[nodiscard]] std::wstring title() const;
    [[nodiscard]] HWND hwnd() const { return hwnd_; }
    [[nodiscard]] bool closed() const { return closed_; }
    [[nodiscard]] winrt::Microsoft::UI::Xaml::XamlRoot clipboard_root() const { return root_.XamlRoot(); }
    void clipboard_status(const ClipboardManager::Status&);

private:
    StreamWindow(Session& session, StreamOffer offer, Callbacks callbacks);
    void initialize();
    void close(bool send_stop);
    void apply_size();
    void resize_surface();
    void toggle_fullscreen();
    // Fullscreen connection bar: slides in at the top edge, hides again unless pinned.
    void build_bar();
    void show_bar(bool shown);
    void schedule_bar_hide(std::chrono::milliseconds delay);
    void minimize();
    void apply_cursor_visibility();
    void set_scaling(bool allowed);
    void set_scale(double scale);
    void system_menu();
    void command(UINT id);
    void save_screenshot();
    winrt::fire_and_forget change_parameters();
    void load_icon();
    void flash(bool on);
    void report(const std::string& message);

    // Input (PROTOCOL.md §5.9).
    void send_input(std::string type, json::object fields = {});
    void key(winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args, bool down);
    void pointer_moved(winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
    void pointer_button(winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args, bool down);
    void pointer_wheel(winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
    std::optional<std::pair<double, double>> video_position(winrt::Windows::Foundation::Point point) const;
    void release_all();
    void forward_paste(bool insert);
    void capture_mouse(bool capture);
    void raw_input(LPARAM lparam);

    static LRESULT CALLBACK subclass(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT message(UINT, WPARAM, LPARAM);

    Session& session_;
    StreamOffer offer_;
    VideoParameters video_;
    Callbacks callbacks_;
    std::shared_ptr<VideoPipeline> pipeline_;
    std::shared_ptr<StreamSink> sink_;
    std::shared_ptr<SessionLink> link_;
    winrt::Microsoft::UI::Xaml::Window window_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ContentControl focus_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Grid root_{nullptr};
    winrt::com_ptr<CursorHost> cursor_host_;
    winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel panel_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Border bar_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Primitives::ToggleButton pin_{nullptr};
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer bar_timer_{nullptr};
    HWND hwnd_{};
    HICON icon_{};
    HICON small_icon_{};
    std::set<std::string> attention_;
    std::set<std::string> held_buttons_;
    std::set<std::string> held_keys_;
    std::set<unsigned> suppressed_paste_;
    winrt::Microsoft::UI::Xaml::Controls::Border clipboard_panel_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock clipboard_text_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button clipboard_cancel_{nullptr};
    bool closed_{};
    bool fullscreen_{};
    bool allow_scaling_{};
    bool mouse_captured_{};
    bool mode_change_pending_{};
    bool bar_shown_{};
    bool bar_hover_{};
    bool pointer_inside_{};
    bool cursor_hidden_{};
    std::string cursor_shape_{"default"};
    double scale_{1.0};
    ULONGLONG last_escape_{};
    WINDOWPLACEMENT windowed_{sizeof(WINDOWPLACEMENT)};
    std::optional<std::pair<double, double>> last_position_;
};

} // namespace beamit::client
