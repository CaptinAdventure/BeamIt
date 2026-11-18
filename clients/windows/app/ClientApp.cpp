#include "ClientApp.hpp"

#include <microsoft.ui.xaml.window.h>
#include <commctrl.h>
#include <shellapi.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Text.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Text.h>

#include <format>

namespace beamit::client {
namespace {

namespace xaml = winrt::Microsoft::UI::Xaml;
namespace controls = xaml::Controls;
using winrt::Microsoft::UI::Dispatching::DispatcherQueue;

constexpr UINT tray_message = WM_APP + 1;
constexpr UINT tray_icon_id = 1;
constexpr UINT open_command = 0x2000;
constexpr UINT disconnect_command = 0x2010;
constexpr UINT exit_command = 0x2020;
constexpr UINT first_stream_command = 0x3000;

controls::TextBlock text(std::wstring_view value, double size = 14, bool bold = false) {
    controls::TextBlock block;
    block.Text(winrt::hstring(value));
    block.FontSize(size);
    block.TextWrapping(xaml::TextWrapping::Wrap);
    if (bold) block.FontWeight(winrt::Microsoft::UI::Text::FontWeights::SemiBold());
    return block;
}

controls::TextBlock caption(std::wstring_view value) {
    auto block = text(value, 12, true);
    block.Opacity(0.7);
    return block;
}

xaml::Media::SolidColorBrush brush(uint8_t r, uint8_t g, uint8_t b) {
    return xaml::Media::SolidColorBrush(winrt::Windows::UI::Color{255, r, g, b});
}

controls::Button source_button(std::wstring_view title, std::wstring_view subtitle) {
    controls::StackPanel content;
    content.Spacing(2);
    content.Children().Append(text(title, 14, true));
    auto detail = text(subtitle, 12);
    detail.Opacity(0.7);
    content.Children().Append(detail);
    controls::Button button;
    button.Content(content);
    button.HorizontalAlignment(xaml::HorizontalAlignment::Stretch);
    button.HorizontalContentAlignment(xaml::HorizontalAlignment::Left);
    button.Padding(xaml::Thickness{12, 8, 12, 8});
    return button;
}

void append_menu(HMENU menu, UINT flags, UINT_PTR id, const wchar_t* label) {
    winrt::check_bool(AppendMenuW(menu, flags, id, label));
}

} // namespace

std::shared_ptr<ClientApp> ClientApp::create() {
    auto app = std::shared_ptr<ClientApp>(new ClientApp);
    app->initialize();
    return app;
}

ClientApp::~ClientApp() {
    remove_tray();
    if (hwnd_) RemoveWindowSubclass(hwnd_, subclass, 1);
}

void ClientApp::initialize() {
    settings_ = Settings::load();
    window_ = xaml::Window();
    window_.Title(L"BeamIt");
    winrt::check_hresult(window_.as<IWindowNative>()->get_WindowHandle(&hwnd_));
    apply_window_icon(hwnd_);
    const auto scale = GetDpiForWindow(hwnd_) / 96.0;
    window_.AppWindow().ResizeClient({static_cast<int32_t>(620 * scale), static_cast<int32_t>(820 * scale)});

    build_connect_view();
    build_source_view();
    controls::Grid root;
    root.Padding(xaml::Thickness{28, 24, 28, 24});
    controls::ScrollViewer scroll;
    controls::StackPanel views;
    views.Children().Append(connect_view_);
    views.Children().Append(source_view_);
    scroll.Content(views);
    root.Children().Append(scroll);
    window_.Content(root);
    source_view_.Visibility(xaml::Visibility::Collapsed);

    winrt::check_bool(SetWindowSubclass(hwnd_, subclass, 1, reinterpret_cast<DWORD_PTR>(this)));
    taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
    window_.Activated([weak = weak_from_this()](auto&&, xaml::WindowActivatedEventArgs const& args) {
        if (auto self = weak.lock(); self && self->clipboard_) {
            if (args.WindowActivationState() == xaml::WindowActivationState::Deactivated) {
                self->clipboard_->context({.enabled = self->settings_.clipboard_sync});
            } else self->clipboard_context();
        }
    });
    tray_added_ = add_tray();
    window_.Activate();
    update_connect_state();
    if (!tray_added_) set_error("The tray icon could not be registered. Closing this window will keep it visible.");
}

void ClientApp::build_connect_view() {
    connect_view_ = controls::StackPanel();
    connect_view_.Spacing(14);
    connect_view_.MaxWidth(460);
    connect_view_.Children().Append(text(L"Connect to BeamIt", 28, true));
    auto intro = text(L"Enter the address, shared password and certificate fingerprint of your BeamIt host.");
    intro.Opacity(0.75);
    connect_view_.Children().Append(intro);

    connect_view_.Children().Append(caption(L"SERVER URL"));
    url_box_ = controls::TextBox();
    url_box_.PlaceholderText(L"wss://192.168.1.20:17389");
    url_box_.Text(hstring(settings_.server_url));
    connect_view_.Children().Append(url_box_);

    connect_view_.Children().Append(caption(L"PASSWORD"));
    password_box_ = controls::PasswordBox();
    password_box_.PlaceholderText(L"Shared host password");
    connect_view_.Children().Append(password_box_);

    connect_view_.Children().Append(caption(L"CERTIFICATE SHA-256"));
    fingerprint_box_ = controls::TextBox();
    fingerprint_box_.PlaceholderText(L"AA:BB:CC:…");
    fingerprint_box_.FontFamily(xaml::Media::FontFamily(L"Consolas"));
    fingerprint_box_.Text(hstring(settings_.fingerprint));
    connect_view_.Children().Append(fingerprint_box_);
    trust_hint_ = text(L"", 12);
    connect_view_.Children().Append(trust_hint_);
    trust_box_ = controls::CheckBox();
    trust_box_.Content(winrt::box_value(L"Trust any server certificate (not recommended)"));
    trust_box_.IsChecked(settings_.trust_any_certificate);
    connect_view_.Children().Append(trust_box_);

    connect_error_ = text(L"", 13);
    connect_error_.Foreground(brush(0xC4, 0x2B, 0x1C));
    connect_view_.Children().Append(connect_error_);

    controls::StackPanel actions;
    actions.Orientation(controls::Orientation::Horizontal);
    actions.Spacing(10);
    connect_button_ = controls::Button();
    connect_button_.Content(winrt::box_value(L"Connect"));
    if (const auto style = xaml::Application::Current().Resources().TryLookup(winrt::box_value(L"AccentButtonStyle"))) {
        connect_button_.Style(style.as<xaml::Style>());
    }
    progress_ = controls::ProgressRing();
    progress_.Width(20);
    progress_.Height(20);
    progress_.IsActive(false);
    actions.Children().Append(connect_button_);
    actions.Children().Append(progress_);
    connect_view_.Children().Append(actions);

    const std::weak_ptr<ClientApp> weak = weak_from_this();
    const auto changed = [weak](auto&&, auto&&) {
        if (const auto self = weak.lock()) self->update_connect_state();
    };
    url_box_.TextChanged(changed);
    fingerprint_box_.TextChanged(changed);
    password_box_.PasswordChanged(changed);
    trust_box_.Checked(changed);
    trust_box_.Unchecked(changed);
    connect_button_.Click([weak](auto&&, auto&&) {
        if (const auto self = weak.lock()) self->connect();
    });
    password_box_.KeyDown([weak](auto&&, xaml::Input::KeyRoutedEventArgs const& args) {
        if (args.Key() != winrt::Windows::System::VirtualKey::Enter) return;
        if (const auto self = weak.lock(); self && self->connect_button_.IsEnabled()) self->connect();
    });
}

void ClientApp::build_source_view() {
    source_view_ = controls::StackPanel();
    source_view_.Spacing(12);
    source_view_.MaxWidth(580);
    controls::Grid header;
    controls::ColumnDefinition grow;
    grow.Width(xaml::GridLengthHelper::FromValueAndType(1, xaml::GridUnitType::Star));
    header.ColumnDefinitions().Append(grow);
    header.ColumnDefinitions().Append(controls::ColumnDefinition());
    controls::StackPanel heading;
    heading.Spacing(4);
    heading.Children().Append(text(L"What do you want to stream?", 26, true));
    host_text_ = text(L"", 12);
    host_text_.Opacity(0.7);
    heading.Children().Append(host_text_);
    header.Children().Append(heading);
    controls::Button disconnect;
    disconnect.Content(winrt::box_value(L"Disconnect"));
    disconnect.VerticalAlignment(xaml::VerticalAlignment::Top);
    controls::Grid::SetColumn(disconnect, 1);
    header.Children().Append(disconnect);
    source_view_.Children().Append(header);

    source_error_ = text(L"", 13);
    source_error_.Foreground(brush(0xC4, 0x2B, 0x1C));
    source_view_.Children().Append(source_error_);
    controls::CheckBox clipboard_sync;
    clipboard_sync.Content(winrt::box_value(L"Synchronize clipboard while BeamIt is active"));
    clipboard_sync.IsChecked(settings_.clipboard_sync);
    clipboard_sync.Click([weak = weak_from_this(), clipboard_sync](auto&&, auto&&) {
        if (auto self = weak.lock()) {
            self->settings_.clipboard_sync = clipboard_sync.IsChecked().Value();
            self->settings_.save();
            self->clipboard_context();
        }
    });
    source_view_.Children().Append(clipboard_sync);
    controls::StackPanel clipboard_actions;
    clipboard_actions.Orientation(controls::Orientation::Horizontal);
    clipboard_actions.Spacing(8);
    controls::Button upload, download;
    upload.Content(winrt::box_value(L"Send clipboard"));
    download.Content(winrt::box_value(L"Get clipboard"));
    upload.Click([weak = weak_from_this()](auto&&, auto&&) {
        if (auto self = weak.lock(); self && self->clipboard_) { self->clipboard_context(); self->clipboard_->upload(); }
    });
    download.Click([weak = weak_from_this()](auto&&, auto&&) {
        if (auto self = weak.lock(); self && self->clipboard_) { self->clipboard_context(); self->clipboard_->download(); }
    });
    clipboard_cancel_ = controls::Button();
    clipboard_cancel_.Content(winrt::box_value(L"Cancel transfer"));
    clipboard_cancel_.Visibility(xaml::Visibility::Collapsed);
    clipboard_cancel_.Click([weak = weak_from_this()](auto&&, auto&&) {
        if (auto self = weak.lock(); self && self->clipboard_) {
            self->clipboard_->cancel();
            if (self->clipboard_dialog_) self->clipboard_dialog_.Hide();
        }
    });
    clipboard_actions.Children().Append(upload);
    clipboard_actions.Children().Append(download);
    clipboard_actions.Children().Append(clipboard_cancel_);
    source_view_.Children().Append(clipboard_actions);
    clipboard_text_ = text(L"", 12);
    source_view_.Children().Append(clipboard_text_);
    streams_panel_ = controls::StackPanel();
    streams_panel_.Spacing(4);
    source_view_.Children().Append(streams_panel_);

    source_view_.Children().Append(caption(L"TWO-WAY AUDIO"));
    audio_source_box_ = controls::ComboBox();
    audio_source_box_.HorizontalAlignment(xaml::HorizontalAlignment::Stretch);
    controls::ComboBoxItem desktop_audio;
    desktop_audio.Content(winrt::box_value(L"Desktop audio"));
    desktop_audio.Tag(winrt::box_value(std::int64_t{}));
    audio_source_box_.Items().Append(desktop_audio);
    audio_source_box_.SelectedIndex(0);
    source_view_.Children().Append(audio_source_box_);
    controls::StackPanel audio_actions;
    audio_actions.Orientation(controls::Orientation::Horizontal);
    audio_actions.Spacing(8);
    audio_button_ = controls::Button();
    audio_button_.Content(winrt::box_value(L"Start audio + microphone"));
    stop_audio_button_ = controls::Button();
    stop_audio_button_.Content(winrt::box_value(L"Stop audio"));
    stop_audio_button_.IsEnabled(false);
    audio_actions.Children().Append(audio_button_);
    audio_actions.Children().Append(stop_audio_button_);
    source_view_.Children().Append(audio_actions);
    audio_status_ = text(L"Audio off", 12);
    audio_status_.Opacity(0.75);
    source_view_.Children().Append(audio_status_);

    source_view_.Children().Append(caption(L"DESKTOP"));
    auto dxgi = source_button(L"Desktop · DXGI", L"Desktop Duplication of the primary monitor");
    auto wgc = source_button(L"Desktop · WGC", L"Windows.Graphics.Capture of the primary monitor");
    source_view_.Children().Append(dxgi);
    source_view_.Children().Append(wgc);

    controls::Grid applications_header;
    controls::ColumnDefinition applications_grow;
    applications_grow.Width(xaml::GridLengthHelper::FromValueAndType(1, xaml::GridUnitType::Star));
    applications_header.ColumnDefinitions().Append(applications_grow);
    applications_header.ColumnDefinitions().Append(controls::ColumnDefinition());
    applications_header.Children().Append(caption(L"APPLICATIONS"));
    applications_count_ = caption(L"");
    controls::Grid::SetColumn(applications_count_, 1);
    applications_header.Children().Append(applications_count_);
    source_view_.Children().Append(applications_header);

    mode_box_ = controls::ComboBox();
    mode_box_.Items().Append(winrt::box_value(L"All windows (main, dialogs and tool windows, including new ones)"));
    mode_box_.Items().Append(winrt::box_value(L"Main window only"));
    mode_box_.SelectedIndex(settings_.follow_all_windows ? 0 : 1);
    mode_box_.HorizontalAlignment(xaml::HorizontalAlignment::Stretch);
    source_view_.Children().Append(mode_box_);
    search_box_ = controls::TextBox();
    search_box_.PlaceholderText(L"Search applications and windows");
    source_view_.Children().Append(search_box_);
    applications_panel_ = controls::StackPanel();
    applications_panel_.Spacing(6);
    source_view_.Children().Append(applications_panel_);

    const std::weak_ptr<ClientApp> weak = weak_from_this();
    disconnect.Click([weak](auto&&, auto&&) {
        if (const auto self = weak.lock()) self->end_session(std::nullopt);
    });
    audio_button_.Click([weak](auto&&, auto&&) {
        if (const auto self = weak.lock()) self->start_audio();
    });
    stop_audio_button_.Click([weak](auto&&, auto&&) {
        if (const auto self = weak.lock()) self->stop_audio(true);
    });
    dxgi.Click([weak](auto&&, auto&&) {
        if (const auto self = weak.lock()) self->start_stream({{"kind", "desktop"}, {"capture", "dxgi"}}, true, true, std::nullopt);
    });
    wgc.Click([weak](auto&&, auto&&) {
        if (const auto self = weak.lock()) self->start_stream({{"kind", "desktop"}, {"capture", "wgc"}}, true, true, std::nullopt);
    });
    mode_box_.SelectionChanged([weak](auto&&, auto&&) {
        if (const auto self = weak.lock()) {
            self->settings_.follow_all_windows = self->mode_box_.SelectedIndex() == 0;
            self->settings_.save();
        }
    });
    search_box_.TextChanged([weak](auto&&, auto&&) {
        if (const auto self = weak.lock()) self->refresh_sources();
    });
    search_box_.KeyDown([weak](auto&&, xaml::Input::KeyRoutedEventArgs const& args) {
        const auto self = weak.lock();
        if (!self || args.Key() != winrt::Windows::System::VirtualKey::Enter) return;
        const auto matches = filter_applications(self->host_windows_.applications(), narrow(self->search_box_.Text()));
        if (matches.size() == 1) {
            const auto pid = matches.front().pid;
            self->start_stream({{"kind", "application"}, {"pid", pid}}, true, true,
                               self->settings_.follow_all_windows ? std::optional(pid) : std::nullopt);
        }
    });
}

void ClientApp::update_connect_state() {
    const bool trust = trust_box_.IsChecked().Value();
    const bool valid = parse_server_url(narrow(url_box_.Text())).has_value() && !password_box_.Password().empty() &&
                       (trust || parse_fingerprint(narrow(fingerprint_box_.Text())).has_value());
    connect_button_.IsEnabled(valid && !connecting_);
    connect_button_.Content(winrt::box_value(connecting_ ? L"Connecting…" : L"Connect"));
    progress_.IsActive(connecting_);
    fingerprint_box_.IsEnabled(!trust);
    trust_hint_.Text(trust ? L"Certificate checks are disabled. Only use this on a network you trust."
                           : L"Copy the fingerprint from the BeamIt host log and verify it before connecting.");
    trust_hint_.Foreground(trust ? brush(0xC0, 0x6A, 0x00) : brush(0x80, 0x80, 0x80));
    if (audio_button_) audio_button_.IsEnabled(session_ != nullptr && !audio_client_);
    if (stop_audio_button_) stop_audio_button_.IsEnabled(audio_client_ != nullptr);
    if (audio_source_box_) audio_source_box_.IsEnabled(session_ != nullptr && !audio_client_);
}

void ClientApp::set_error(const std::string& message) {
    const auto value = hstring(message);
    connect_error_.Text(value);
    source_error_.Text(value);
    if (!message.empty()) show_window();
}

void ClientApp::show_window() {
    ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(hwnd_);
}

void ClientApp::hide_window() {
    if (tray_added_) ShowWindow(hwnd_, SW_HIDE);
}

// Session ------------------------------------------------------------------------------------------

void ClientApp::connect() {
    if (connecting_ || session_) return;
    connecting_ = true;
    set_error({});
    update_connect_state();
    ConnectOptions options;
    options.server_url = narrow(url_box_.Text());
    options.password = narrow(password_box_.Password());
    options.fingerprint = narrow(fingerprint_box_.Text());
    options.trust_any_certificate = trust_box_.IsChecked().Value();
    const auto saved_url = options.server_url;
    const auto saved_fingerprint = options.fingerprint;
    const bool saved_trust = options.trust_any_certificate;

    const auto queue = DispatcherQueue::GetForCurrentThread();
    const std::weak_ptr<ClientApp> weak = weak_from_this();
    Session::Events events;
    events.message = [weak](const json::object& message) {
        if (const auto self = weak.lock()) self->handle_message(message);
    };
    events.disconnected = [weak](const SessionError& error) {
        if (const auto self = weak.lock()) self->end_session(error.message);
    };
    session_ = std::make_unique<Session>(
        [queue](std::function<void()> function) { queue.TryEnqueue([function = std::move(function)] { function(); }); },
        std::move(events));
    session_->connect(std::move(options), [weak, saved_url, saved_fingerprint, saved_trust](std::expected<void, SessionError> result) {
        const auto self = weak.lock();
        if (!self) return;
        self->connecting_ = false;
        if (!result) {
            self->session_.reset();
            self->update_connect_state();
            self->set_error(result.error().message);
            return;
        }
        self->settings_.server_url = saved_url;
        self->settings_.fingerprint = saved_trust ? self->settings_.fingerprint : saved_fingerprint;
        self->settings_.trust_any_certificate = saved_trust;
        self->settings_.save();
        self->password_box_.Password(L"");
        self->on_connected();
    });
}

void ClientApp::on_connected() {
    const auto queue = DispatcherQueue::GetForCurrentThread();
    ClipboardManager::Callbacks clipboard_callbacks;
    clipboard_callbacks.confirm = [weak = weak_from_this()](auto roots, auto bytes, auto done) {
        if (auto self = weak.lock()) self->confirm_clipboard(roots, bytes, std::move(done));
        else done(false);
    };
    clipboard_callbacks.status = [weak = weak_from_this()](auto status) {
        if (auto self = weak.lock()) self->clipboard_status(std::move(status));
    };
    clipboard_ = std::make_unique<ClipboardManager>(*session_,
        [queue](auto function) { queue.TryEnqueue([function = std::move(function)] { function(); }); },
        std::move(clipboard_callbacks));
    clipboard_->policy({settings_.clipboard_bulk_byte_threshold, settings_.clipboard_bulk_entry_threshold});
    clipboard_context();
    clipboard_->start();
    update_connect_state();
    connect_view_.Visibility(xaml::Visibility::Collapsed);
    source_view_.Visibility(xaml::Visibility::Visible);
    const auto endpoint = session_->endpoint();
    host_text_.Text(hstring(std::format("Connected to {} · certificate {}", endpoint ? endpoint->authority : std::string("host"),
                                        session_->certificate_fingerprint().substr(0, 23) + "…")));
    refresh_sources();
    refresh_streams();
}

void ClientApp::clipboard_context() {
    if (!clipboard_) return;
    ClipboardManager::Context context{.enabled = settings_.clipboard_sync};
    const auto foreground = GetForegroundWindow();
    if (foreground == hwnd_) {
        context.active = true; context.foreground = hwnd_;
    }
    for (const auto& [id, stream] : streams_) {
        if (!stream->closed() && stream->hwnd() == foreground) {
            context.active = true; context.foreground = foreground;
            context.stream = id; context.target = stream->offer().source.hwnd;
            break;
        }
    }
    clipboard_->context(std::move(context));
}

void ClientApp::clipboard_status(ClipboardManager::Status status) {
    auto label = std::format("Clipboard: {} · {} / {} bytes", status.phase, status.bytes, status.total);
    if (!status.error.empty()) label += " · " + status.error;
    clipboard_text_.Text(hstring(label));
    clipboard_cancel_.Visibility(status.busy ? xaml::Visibility::Visible : xaml::Visibility::Collapsed);
    if (!status.busy && clipboard_dialog_) clipboard_dialog_.Hide();
    for (const auto& [id, stream] : streams_) stream->clipboard_status(status);
}

winrt::fire_and_forget ClientApp::confirm_clipboard(std::uint32_t roots, std::uint64_t bytes,
                                                  std::function<void(bool)> done) {
    auto lifetime = shared_from_this();
    if (clipboard_dialog_) { done(false); co_return; }
    xaml::XamlRoot root{nullptr};
    const auto foreground = GetForegroundWindow();
    if (foreground == hwnd_) root = window_.Content().XamlRoot();
    for (const auto& [id, stream] : streams_) {
        if (!stream->closed() && stream->hwnd() == foreground) root = stream->clipboard_root();
    }
    if (!root) { done(false); co_return; }
    clipboard_dialog_ = controls::ContentDialog();
    clipboard_dialog_.XamlRoot(root);
    clipboard_dialog_.Title(winrt::box_value(L"Transfer clipboard selection?"));
    clipboard_dialog_.Content(winrt::box_value(hstring(std::format(
        "{} selected roots · {} bytes in regular files.\nFolders include every descendant. Transfer this selection?",
        roots, bytes))));
    clipboard_dialog_.PrimaryButtonText(L"Transfer");
    clipboard_dialog_.CloseButtonText(L"Cancel");
    bool approved{};
    try { approved = (co_await clipboard_dialog_.ShowAsync()) == controls::ContentDialogResult::Primary; }
    catch (const winrt::hresult_error&) { set_error("Clipboard confirmation could not be displayed."); }
    clipboard_dialog_ = nullptr;
    done(approved);
}

void ClientApp::end_session(std::optional<std::string> error) {
    if (!session_) return;
    stop_audio(false);
    if (clipboard_dialog_) clipboard_dialog_.Hide();
    clipboard_.reset(); // Joins native/disk work before Session is destroyed.
    for (auto& [id, stream] : std::exchange(streams_, {})) stream->close_after_host_stop();
    followed_.clear();
    handled_.clear();
    host_windows_.clear();
    audio_source_box_.Items().Clear();
    starting_ = 0;
    session_->disconnect();
    session_.reset();
    connecting_ = false;
    source_view_.Visibility(xaml::Visibility::Collapsed);
    connect_view_.Visibility(xaml::Visibility::Visible);
    search_box_.Text(L"");
    update_connect_state();
    set_error(error.value_or(""));
    show_window();
}

void ClientApp::handle_message(const json::object& message) {
    if (!session_) return;
    if (clipboard_) clipboard_->event(message);
    const auto type = string_field(message, "type").value_or("");
    if (type == "audio_stopped") {
        const auto reason = std::string(string_field(message, "reason").value_or(""));
        stop_audio(false);
        audio_status_.Text(L"Audio off");
        if (!reason.empty() && reason != "requested") set_error("Audio stopped: " + reason);
    } else if (type == "audio_track_stopped") {
        if (const auto track_id = integer_field(message, "track_id"); track_id && *track_id > 0) {
            session_->remove_audio_track(static_cast<std::uint32_t>(*track_id));
            if (audio_client_) audio_client_->remove_track(static_cast<std::uint32_t>(*track_id));
        }
        if (string_field(message, "reason") != "requested") {
            set_error(std::string(string_field(message, "message").value_or("Host audio track stopped.")));
        }
    } else if (type == "window_opened" || type == "window_changed" || type == "window_closed") {
        const auto* object = message.if_contains("window");
        const auto window = object ? parse_host_window(*object) : std::nullopt;
        if (!window) return;
        const bool closed = type == "window_closed";
        const bool listing = host_windows_.update(*window, closed);
        if (closed) handled_.erase(window->hwnd);
        if (followed_.contains(window->pid)) reconcile();
        if (listing) refresh_sources();
    } else if (type == "stream_stopped" && !message.contains("id")) {
        const std::string id(string_field(message, "stream_id").value_or(""));
        const auto reason = std::string(string_field(message, "reason").value_or(""));
        if (const auto found = streams_.find(id); found != streams_.end()) {
            const auto stream = found->second;
            stream->close_after_host_stop();
            if (reason != "requested" && reason != "source_closed") {
                set_error(std::format("Stream stopped: {}{}", reason,
                    message.contains("message") ? " (" + std::string(string_field(message, "message").value_or("")) + ")" : ""));
            }
        }
    } else if (type == "attention") {
        const std::string id(string_field(message, "stream_id").value_or(""));
        const auto state = string_field(message, "state");
        const auto* window = message.if_contains("window");
        if (!state || !window || !window->is_object()) return;
        const std::string hwnd(string_field(window->get_object(), "hwnd").value_or(""));
        if (const auto found = streams_.find(id); found != streams_.end()) {
            found->second->update_attention(hwnd, *state == "set");
        }
    } else if (type == "cursor") {
        const std::string id(string_field(message, "stream_id").value_or(""));
        const auto shape = string_field(message, "shape");
        if (const auto found = streams_.find(id); shape && found != streams_.end()) {
            found->second->update_cursor(std::string(*shape));
        }
    } else if (type == "error") {
        const auto text = std::string(string_field(message, "message").value_or(string_field(message, "code").value_or("Host error")));
        const std::string id(string_field(message, "stream_id").value_or(""));
        if (const auto found = streams_.find(id); found != streams_.end()) {
            set_error(std::format("{}: {}", narrow(found->second->title()), text));
        } else {
            set_error(text);
        }
    }
}

void ClientApp::start_audio() {
    if (!session_ || audio_client_) return;
    set_error({});
    audio_status_.Text(L"Opening authenticated audio socket…");
    update_connect_state();
    const std::weak_ptr<ClientApp> weak = weak_from_this();
    session_->request("start_audio", {}, "audio_offer",
        [weak](std::expected<json::object, SessionError> reply) {
            const auto self = weak.lock();
            if (!self || !self->session_) return;
            if (!reply) {
                self->audio_status_.Text(L"Audio unavailable");
                self->set_error(reply.error().message);
                self->update_connect_state();
                return;
            }
            const auto endpoint = self->session_->endpoint();
            const auto offer = endpoint ? parse_audio_offer(*reply, *endpoint)
                                        : std::expected<AudioOffer, std::string>(
                                              std::unexpected(std::string("The host endpoint is unavailable.")));
            if (!offer) {
                const auto audio_id = string_field(*reply, "audio_id");
                if (audio_id) {
                    self->session_->request("stop_audio", {{"audio_id", *audio_id}}, "audio_stopped", [](auto&&) {});
                }
                self->audio_status_.Text(L"Audio unavailable");
                self->set_error(offer.error());
                self->update_connect_state();
                return;
            }

            self->audio_id_ = offer->audio_id;
            std::int64_t pid{};
            if (const auto selected = self->audio_source_box_.SelectedItem()) {
                pid = winrt::unbox_value<std::int64_t>(selected.as<controls::ComboBoxItem>().Tag());
            }
            json::object source = pid > 0
                ? json::object{{"kind", "application"}, {"pid", pid}}
                : json::object{{"kind", "desktop"}};
            const auto queue = DispatcherQueue::GetForCurrentThread();
            const auto audio = std::make_shared<AudioClient>(offer->microphone,
                [session = self->session_.get()](std::vector<std::byte> packet) {
                    session->send_audio_packet(std::move(packet));
                },
                [weak, queue](std::string message) {
                    queue.TryEnqueue([weak, message = std::move(message)] {
                        if (const auto app = weak.lock()) app->audio_failed(std::move(message));
                    });
                });
            self->audio_client_ = audio;
            self->audio_status_.Text(L"Starting microphone…");
            AudioCallbacks callbacks;
            callbacks.packet = [audio_weak = std::weak_ptr(audio)](
                const net::protocol::AudioPacket& packet, const media::AudioConfiguration& configuration) {
                if (const auto client = audio_weak.lock()) client->receive(packet, configuration);
            };
            callbacks.failed = [weak, queue](const SessionError& error) {
                queue.TryEnqueue([weak, message = error.message] {
                    if (const auto app = weak.lock()) app->audio_failed(message);
                });
            };
            self->session_->open_audio(*offer, std::move(callbacks),
                [weak, audio, audio_id = offer->audio_id, source = std::move(source)](
                    std::expected<void, SessionError> opened) mutable {
                    const auto app = weak.lock();
                    if (!app || !app->session_ || app->audio_client_ != audio) return;
                    if (!opened) {
                        app->audio_failed(opened.error().message);
                        return;
                    }
                    try {
                        audio->start();
                    } catch (const std::exception& error) {
                        app->audio_failed(error.what());
                        return;
                    }
                    app->audio_status_.Text(L"Starting host audio track…");
                    app->session_->request("start_audio_track",
                        {{"audio_id", audio_id}, {"source", std::move(source)}}, "audio_track_started",
                        [weak, audio](std::expected<json::object, SessionError> started) {
                            const auto owner = weak.lock();
                            if (!owner || !owner->session_ || owner->audio_client_ != audio) return;
                            if (!started) {
                                owner->audio_failed(started.error().message);
                                return;
                            }
                            const auto* track_value = started->if_contains("track");
                            const auto track = track_value ? parse_audio_track(*track_value)
                                                           : std::expected<AudioTrackConfiguration, std::string>(
                                                                 std::unexpected(std::string(
                                                                     "The host omitted its audio track configuration.")));
                            if (!track) {
                                owner->audio_failed(track.error());
                                return;
                            }
                            try {
                                audio->add_track(*track);
                                owner->session_->configure_audio_track(*track);
                            } catch (const std::exception& error) {
                                owner->audio_failed(error.what());
                                return;
                            }
                            owner->audio_status_.Text(L"Microphone and desktop audio active");
                        });
                });
            self->update_connect_state();
        });
}

void ClientApp::stop_audio(bool notify_host) {
    auto audio = std::exchange(audio_client_, {});
    if (audio) audio->stop();
    const auto audio_id = std::exchange(audio_id_, {});
    if (session_) {
        session_->close_audio();
        if (notify_host && !audio_id.empty()) {
            const std::weak_ptr<ClientApp> weak = weak_from_this();
            session_->request("stop_audio", {{"audio_id", audio_id}}, "audio_stopped",
                [weak](std::expected<json::object, SessionError> result) {
                    if (!result && result.error().code != "not_found") {
                        if (const auto self = weak.lock()) self->set_error(result.error().message);
                    }
                });
        }
    }
    if (audio_status_) audio_status_.Text(L"Audio off");
    update_connect_state();
}

void ClientApp::audio_failed(std::string message) {
    stop_audio(true);
    audio_status_.Text(L"Audio unavailable");
    set_error(message);
}

// Streams ------------------------------------------------------------------------------------------

void ClientApp::start_stream(json::object source, bool activate, bool from_picker, std::optional<std::int64_t> follow) {
    if (!session_) return;
    if (from_picker) set_error({});
    ++starting_;
    refresh_streams();
    const std::weak_ptr<ClientApp> weak = weak_from_this();
    const bool window_source = string_field(source, "kind") == "window";
    // Windows decode HEVC (including 4:4:4) with the installed Media Foundation decoder; see README.
    // The local pointer mirrors the host pointer shape instead of a second pointer drawn into the video.
    json::object fields{{"source", std::move(source)}, {"video", json::object{{"codec", "h265"}}},
                        {"input", json::object{{"cursor", "client"}}}};
    session_->request("start_stream", std::move(fields), "stream_offer",
        [weak, activate, from_picker, follow, window_source](std::expected<json::object, SessionError> reply) {
            const auto self = weak.lock();
            if (!self || !self->session_) return;
            --self->starting_;
            self->refresh_streams();
            if (!reply) {
                self->set_error((window_source ? "Could not connect window: " : "") + reply.error().message);
                return;
            }
            auto offer = parse_stream_offer(*reply, *self->session_->endpoint());
            if (!offer) {
                if (const auto id = string_field(*reply, "stream_id")) {
                    self->session_->request("stop_stream", {{"stream_id", *id}}, "stream_stopped", [](auto&&) {});
                }
                self->set_error(offer.error());
                return;
            }
            const auto id = offer->stream_id;
            const auto hwnd = offer->source.hwnd;
            StreamWindow::Callbacks callbacks;
            callbacks.closed = [weak](const std::string& stream_id) {
                if (const auto app = weak.lock()) app->stream_closed(stream_id);
            };
            callbacks.error = [weak](const std::string& message) {
                if (const auto app = weak.lock()) app->set_error(message);
            };
            callbacks.focused = [weak](bool active) {
                if (auto app = weak.lock(); app && app->clipboard_) {
                    if (active) app->clipboard_context();
                    else app->clipboard_->context({.enabled = app->settings_.clipboard_sync});
                }
            };
            callbacks.paste = [weak](auto done) {
                if (auto app = weak.lock(); app && app->clipboard_) {
                    app->clipboard_context();
                    app->clipboard_->paste(std::move(done));
                }
                else done(false);
            };
            callbacks.cancel_clipboard = [weak] {
                if (auto app = weak.lock(); app && app->clipboard_) app->clipboard_->cancel();
            };
            auto window = StreamWindow::create(*self->session_, std::move(*offer), std::move(callbacks));
            self->streams_[id] = window;
            if (!hwnd.empty()) self->handled_.insert(hwnd);
            window->open([weak, id, activate, from_picker, follow, hwnd, window_source](std::expected<void, SessionError> opened) {
                const auto app = weak.lock();
                if (!app) return;
                const auto found = app->streams_.find(id);
                if (found == app->streams_.end()) return;
                const auto stream = found->second;
                if (!opened) {
                    app->set_error("Could not open the stream: " + opened.error().message);
                    stream->close_after_host_stop();
                    if (app->session_) app->session_->request("stop_stream", {{"stream_id", id}}, "stream_stopped", [](auto&&) {});
                    return;
                }
                if (window_source && !hwnd.empty()) app->place(*stream, hwnd);
                stream->show(activate);
                if (from_picker) app->hide_window();
                if (follow) {
                    app->followed_.insert(*follow);
                    app->reconcile();
                }
                app->refresh_streams();
            });
        });
}

void ClientApp::reconcile() {
    if (!session_) return;
    std::set<std::string> excluded = handled_;
    for (const auto& [id, stream] : streams_) {
        if (!stream->offer().source.hwnd.empty()) excluded.insert(stream->offer().source.hwnd);
    }
    for (const auto& window : windows_to_follow(host_windows_, followed_, excluded)) {
        handled_.insert(window.hwnd);
        start_stream({{"kind", "window"}, {"hwnd", window.hwnd}}, false, false, std::nullopt);
    }
}

void ClientApp::place(StreamWindow& window, const std::string& hwnd) {
    const auto* host = host_windows_.find(hwnd);
    if (!host) return;
    for (const auto& [id, other] : streams_) {
        if (&*other == &window || other->closed() || other->offer().source.pid != host->pid ||
            other->offer().source.hwnd.empty() || !IsWindowVisible(other->hwnd())) {
            continue;
        }
        const auto* anchor = host_windows_.find(other->offer().source.hwnd);
        if (!anchor) continue;
        // At 1× one video pixel is one local physical pixel, so host pixel offsets apply directly.
        RECT anchor_client{};
        GetClientRect(other->hwnd(), &anchor_client);
        MapWindowPoints(other->hwnd(), nullptr, reinterpret_cast<POINT*>(&anchor_client), 2);
        RECT frame{}, client{};
        GetWindowRect(window.hwnd(), &frame);
        GetClientRect(window.hwnd(), &client);
        MapWindowPoints(window.hwnd(), nullptr, reinterpret_cast<POINT*>(&client), 2);
        auto x = anchor_client.left + static_cast<LONG>(host->rect.x - anchor->rect.x) - (client.left - frame.left);
        auto y = anchor_client.top + static_cast<LONG>(host->rect.y - anchor->rect.y) - (client.top - frame.top);
        MONITORINFO monitor{sizeof(MONITORINFO)};
        GetMonitorInfoW(MonitorFromWindow(other->hwnd(), MONITOR_DEFAULTTONEAREST), &monitor);
        const auto width = frame.right - frame.left;
        const auto height = frame.bottom - frame.top;
        x = std::clamp(x, monitor.rcWork.left, std::max(monitor.rcWork.left, monitor.rcWork.right - width));
        y = std::clamp(y, monitor.rcWork.top, std::max(monitor.rcWork.top, monitor.rcWork.bottom - height));
        SetWindowPos(window.hwnd(), nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return;
    }
}

void ClientApp::stream_closed(const std::string& stream_id) {
    if (streams_.erase(stream_id) == 0) return;
    std::set<std::int64_t> streamed;
    for (const auto& [id, stream] : streams_) {
        if (stream->offer().source.pid) streamed.insert(*stream->offer().source.pid);
    }
    std::erase_if(followed_, [&](std::int64_t pid) { return !streamed.contains(pid); });
    refresh_streams();
    if (streams_.empty() && session_) {
        handled_.clear();
        show_window();
    }
}

void ClientApp::refresh_streams() {
    if (!streams_panel_) return;
    streams_panel_.Children().Clear();
    if (starting_ > 0) {
        streams_panel_.Children().Append(text(L"Starting stream…", 13));
    }
    if (streams_.empty()) return;
    streams_panel_.Children().Append(caption(std::format(L"{} ACTIVE STREAM{}", streams_.size(), streams_.size() == 1 ? L"" : L"S")));
    const std::weak_ptr<ClientApp> weak = weak_from_this();
    for (const auto& [id, stream] : streams_) {
        controls::Grid row;
        controls::ColumnDefinition grow;
        grow.Width(xaml::GridLengthHelper::FromValueAndType(1, xaml::GridUnitType::Star));
        row.ColumnDefinitions().Append(grow);
        row.ColumnDefinitions().Append(controls::ColumnDefinition());
        row.ColumnDefinitions().Append(controls::ColumnDefinition());
        auto label = text(stream->title(), 13);
        label.TextWrapping(xaml::TextWrapping::NoWrap);
        label.TextTrimming(xaml::TextTrimming::CharacterEllipsis);
        label.VerticalAlignment(xaml::VerticalAlignment::Center);
        row.Children().Append(label);
        controls::HyperlinkButton show;
        show.Content(winrt::box_value(L"Show"));
        controls::Grid::SetColumn(show, 1);
        controls::HyperlinkButton stop;
        stop.Content(winrt::box_value(L"Stop"));
        controls::Grid::SetColumn(stop, 2);
        row.Children().Append(show);
        row.Children().Append(stop);
        const std::weak_ptr<StreamWindow> target = stream;
        show.Click([target](auto&&, auto&&) {
            if (const auto window = target.lock()) window->show(true);
        });
        stop.Click([target](auto&&, auto&&) {
            if (const auto window = target.lock(); window && window->hwnd()) PostMessageW(window->hwnd(), WM_CLOSE, 0, 0);
        });
        streams_panel_.Children().Append(row);
    }
}

void ClientApp::refresh_sources() {
    if (!applications_panel_ || !session_) return;
    applications_panel_.Children().Clear();
    const auto all = host_windows_.applications();
    const auto selected_audio_pid = audio_source_box_.SelectedItem()
        ? winrt::unbox_value<std::int64_t>(audio_source_box_.SelectedItem().as<controls::ComboBoxItem>().Tag())
        : std::int64_t{};
    audio_source_box_.Items().Clear();
    controls::ComboBoxItem desktop_audio;
    desktop_audio.Content(winrt::box_value(L"Desktop audio"));
    desktop_audio.Tag(winrt::box_value(std::int64_t{}));
    audio_source_box_.Items().Append(desktop_audio);
    auto selected_audio_index = 0;
    for (const auto& application : all) {
        controls::ComboBoxItem item;
        item.Content(winrt::box_value(widen(application.title)));
        item.Tag(winrt::box_value(application.pid));
        audio_source_box_.Items().Append(item);
        if (application.pid == selected_audio_pid) selected_audio_index = static_cast<int>(audio_source_box_.Items().Size() - 1);
    }
    audio_source_box_.SelectedIndex(selected_audio_index);
    const auto search = narrow(search_box_.Text());
    const auto applications = filter_applications(all, search);
    applications_count_.Text(search.empty() ? std::to_wstring(all.size())
                                            : std::format(L"{} of {}", applications.size(), all.size()));
    if (all.empty()) {
        applications_panel_.Children().Append(text(L"The host has not reported any application windows.", 13));
        return;
    }
    if (applications.empty()) {
        applications_panel_.Children().Append(text(L"No applications match the search.", 13));
        return;
    }
    const std::weak_ptr<ClientApp> weak = weak_from_this();
    for (const auto& application : applications) {
        auto subtitle = std::format("{}{} window{} · PID {}", application.image_name.empty() ? "" : application.image_name + " · ",
                                    application.windows.size(), application.windows.size() == 1 ? "" : "s", application.pid);
        if (const auto match = matching_window_title(application, search)) subtitle += " · “" + *match + "”";
        auto button = source_button(widen(application.title), widen(subtitle));
        const auto pid = application.pid;
        button.Click([weak, pid](auto&&, auto&&) {
            if (const auto self = weak.lock()) {
                self->start_stream({{"kind", "application"}, {"pid", pid}}, true, true,
                                   self->settings_.follow_all_windows ? std::optional(pid) : std::nullopt);
            }
        });
        applications_panel_.Children().Append(button);
    }
}

// Tray and lifetime --------------------------------------------------------------------------------

bool ClientApp::add_tray() {
    NOTIFYICONDATAW data{sizeof(NOTIFYICONDATAW)};
    data.hWnd = hwnd_;
    data.uID = tray_icon_id;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = tray_message;
    data.hIcon = app_icon(false);
    wcscpy_s(data.szTip, L"BeamIt");
    if (!Shell_NotifyIconW(NIM_ADD, &data)) return false;
    data.uVersion = NOTIFYICON_VERSION_4;
    if (!Shell_NotifyIconW(NIM_SETVERSION, &data)) {
        Shell_NotifyIconW(NIM_DELETE, &data);
        return false;
    }
    return true;
}

void ClientApp::remove_tray() {
    if (!tray_added_) return;
    NOTIFYICONDATAW data{sizeof(NOTIFYICONDATAW)};
    data.hWnd = hwnd_;
    data.uID = tray_icon_id;
    Shell_NotifyIconW(NIM_DELETE, &data);
    tray_added_ = false;
}

void ClientApp::tray_menu() {
    const HMENU menu = CreatePopupMenu();
    const HMENU streams = CreatePopupMenu();
    std::vector<std::weak_ptr<StreamWindow>> targets;
    try {
        append_menu(menu, MF_STRING, open_command, session_ ? L"Add stream…" : L"Open connection…");
        for (const auto& [id, stream] : streams_) {
            append_menu(streams, MF_STRING, first_stream_command + static_cast<UINT>(targets.size()), stream->title().c_str());
            targets.push_back(stream);
        }
        append_menu(menu, MF_POPUP | (targets.empty() ? MF_GRAYED : 0), reinterpret_cast<UINT_PTR>(streams), L"Active streams");
        append_menu(menu, MF_SEPARATOR, 0, nullptr);
        append_menu(menu, MF_STRING | (session_ ? 0 : MF_GRAYED), disconnect_command, L"Disconnect");
        append_menu(menu, MF_STRING, exit_command, L"Exit");
    } catch (...) {
        DestroyMenu(streams);
        DestroyMenu(menu);
        throw;
    }
    POINT point{};
    GetCursorPos(&point);
    SetForegroundWindow(hwnd_);
    const auto selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu); // Also destroys the attached submenu.
    if (selected == open_command) show_window();
    else if (selected == disconnect_command) end_session(std::nullopt);
    else if (selected == exit_command) exit();
    else if (selected >= first_stream_command && selected < first_stream_command + targets.size()) {
        if (const auto window = targets[selected - first_stream_command].lock()) window->show(true);
    }
}

void ClientApp::exit() {
    exiting_ = true;
    end_session(std::nullopt);
    remove_tray();
    window_.Close();
    xaml::Application::Current().Exit();
}

LRESULT CALLBACK ClientApp::subclass(HWND, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR data) {
    const auto self = reinterpret_cast<ClientApp*>(data);
    try {
        return self->message(message, wparam, lparam);
    } catch (const winrt::hresult_error& error) {
        self->set_error(narrow(error.message()));
    } catch (const std::exception& error) {
        self->set_error(error.what());
    }
    return 0;
}

LRESULT ClientApp::message(UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == taskbar_created_ && taskbar_created_ != 0) {
        tray_added_ = add_tray();
        if (!tray_added_) {
            show_window();
            set_error("The tray icon could not be restored after Explorer restarted.");
        }
        return 0;
    }
    switch (message) {
    case WM_CLOSE:
        if (!exiting_ && tray_added_) {
            hide_window();
            return 0;
        }
        if (!exiting_) {
            exit();
            return 0;
        }
        break;
    case tray_message:
        switch (LOWORD(lparam)) {
        case NIN_SELECT:
        case NIN_KEYSELECT: show_window(); break;
        case WM_CONTEXTMENU: tray_menu(); break;
        default: break;
        }
        return 0;
    case WM_NCDESTROY: {
        remove_tray();
        const auto window = hwnd_;
        RemoveWindowSubclass(window, subclass, 1);
        hwnd_ = nullptr;
        return DefSubclassProc(window, message, wparam, lparam);
    }
    default: break;
    }
    return DefSubclassProc(hwnd_, message, wparam, lparam);
}

} // namespace beamit::client
