#include "StreamWindow.hpp"

#include "AppUtil.hpp"
#include "KeyCodes.hpp"

#include <microsoft.ui.xaml.media.dxinterop.h>
#include <microsoft.ui.xaml.window.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Core.h>

#include <atomic>
#include <cmath>
#include <format>
#include <fstream>
#include <limits>
#include <mutex>

namespace beamit::client {

struct StreamSink {
    std::atomic<bool> active{true};
    std::shared_ptr<VideoPipeline> pipeline;
};

// Lets the pipeline thread reach the session without owning the window, pipeline or session.
struct SessionLink {
    std::mutex mutex;
    Session* session{};
};

// UIElement.ProtectedCursor is only settable from a derived element; it applies to the video panel inside it.
struct CursorHost : winrt::Microsoft::UI::Xaml::Controls::ContentControlT<CursorHost> {
    void cursor(winrt::Microsoft::UI::Input::InputCursor const& value) { ProtectedCursor(value); }
};

namespace {

namespace xaml = winrt::Microsoft::UI::Xaml;
using winrt::Microsoft::UI::Dispatching::DispatcherQueue;

constexpr UINT capture_command = 0x1100;
constexpr UINT scaling_command = 0x1110;
constexpr UINT half_command = 0x1120;
constexpr UINT actual_command = 0x1130;
constexpr UINT double_command = 0x1140;
constexpr UINT fullscreen_command = 0x1150;
constexpr UINT screenshot_command = 0x1160;
constexpr UINT info_command = 0x1170;
constexpr UINT rate_command = 0x1180;
constexpr UINT first_command = capture_command;
constexpr UINT last_command = rate_command;

template<class Function>
void post(DispatcherQueue const& queue, Function&& function) {
    queue.TryEnqueue([function = std::forward<Function>(function)]() mutable { function(); });
}

bool held(int key) { return (GetKeyState(key) & 0x8000) != 0; }

// Shape names are the CSS cursor keywords the host sends; unknown names use the arrow.
winrt::Microsoft::UI::Input::InputSystemCursorShape cursor_shape(std::string_view name) {
    using enum winrt::Microsoft::UI::Input::InputSystemCursorShape;
    static constexpr std::pair<std::string_view, winrt::Microsoft::UI::Input::InputSystemCursorShape> shapes[]{
        {"text", IBeam},           {"pointer", Hand},
        {"wait", Wait},            {"progress", AppStarting},
        {"crosshair", Cross},      {"not-allowed", UniversalNo},
        {"move", SizeAll},         {"ns-resize", SizeNorthSouth},
        {"ew-resize", SizeWestEast}, {"nwse-resize", SizeNorthwestSoutheast},
        {"nesw-resize", SizeNortheastSouthwest}, {"help", Help},
    };
    for (const auto& [shape_name, shape] : shapes) {
        if (shape_name == name) return shape;
    }
    return Arrow;
}

xaml::Controls::Button bar_button(wchar_t glyph, std::wstring_view tip) {
    xaml::Controls::FontIcon icon;
    icon.FontFamily(xaml::Media::FontFamily(L"Segoe Fluent Icons,Segoe MDL2 Assets"));
    icon.Glyph(winrt::hstring(std::wstring(1, glyph)));
    icon.FontSize(14);
    xaml::Controls::Button button;
    button.Content(icon);
    button.Padding(xaml::Thickness{10, 6, 10, 6});
    button.IsTabStop(false);
    xaml::Controls::ToolTipService::SetToolTip(button, winrt::box_value(winrt::hstring(tip)));
    return button;
}

HICON icon_from_png(const std::vector<std::byte>& png, int size) {
    winrt::com_ptr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put())))) return nullptr;
    winrt::com_ptr<IWICStream> stream;
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    winrt::com_ptr<IWICBitmapScaler> scaler;
    winrt::com_ptr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateStream(stream.put())) ||
        FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<std::byte*>(png.data())), static_cast<DWORD>(png.size()))) ||
        FAILED(factory->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.put())) ||
        FAILED(decoder->GetFrame(0, frame.put())) || FAILED(factory->CreateBitmapScaler(scaler.put())) ||
        FAILED(scaler->Initialize(frame.get(), size, size, WICBitmapInterpolationModeHighQualityCubic)) ||
        FAILED(factory->CreateFormatConverter(converter.put())) ||
        FAILED(converter->Initialize(scaler.get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom))) {
        return nullptr;
    }
    std::vector<BYTE> pixels(static_cast<std::size_t>(size) * size * 4);
    if (FAILED(converter->CopyPixels(nullptr, size * 4, static_cast<UINT>(pixels.size()), pixels.data()))) return nullptr;
    const auto color = CreateBitmap(size, size, 1, 32, pixels.data());
    const auto mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO info{TRUE, 0, 0, mask, color};
    const auto icon = CreateIconIndirect(&info);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

} // namespace

std::shared_ptr<StreamWindow> StreamWindow::create(Session& session, StreamOffer offer, Callbacks callbacks) {
    auto window = std::shared_ptr<StreamWindow>(new StreamWindow(session, std::move(offer), std::move(callbacks)));
    window->initialize();
    return window;
}

StreamWindow::StreamWindow(Session& session, StreamOffer offer, Callbacks callbacks)
    : session_(session), offer_(std::move(offer)), video_(offer_.video), callbacks_(std::move(callbacks)) {}

StreamWindow::~StreamWindow() {
    if (sink_) sink_->active = false;
    if (link_) {
        std::scoped_lock lock(link_->mutex);
        link_->session = nullptr;
    }
    if (icon_) DestroyIcon(icon_);
    if (small_icon_) DestroyIcon(small_icon_);
}

std::wstring StreamWindow::title() const {
    if (!offer_.source.title.empty()) return widen(offer_.source.title);
    if (offer_.source.kind == "desktop") return L"Desktop · " + widen(offer_.source.capture);
    return L"BeamIt stream";
}

void StreamWindow::initialize() {
    const auto queue = DispatcherQueue::GetForCurrentThread();
    const std::weak_ptr<StreamWindow> weak = weak_from_this();
    const auto id = offer_.stream_id;
    link_ = std::make_shared<SessionLink>();
    link_->session = &session_;
    pipeline_ = std::make_shared<VideoPipeline>(VideoPipeline::Callbacks{
        [queue, weak](const std::string& message) {
            post(queue, [weak, message] {
                if (const auto self = weak.lock(); self && !self->closed_) {
                    self->report(message);
                    self->close(true);
                }
            });
        },
        [link = link_, id] {
            std::scoped_lock lock(link->mutex);
            if (link->session) link->session->restart_stream_decoding(id);
        },
    });
    sink_ = std::make_shared<StreamSink>();
    sink_->pipeline = pipeline_;
    pipeline_->configure(video_);

    window_ = xaml::Window();
    window_.Title(title());
    winrt::check_hresult(window_.as<IWindowNative>()->get_WindowHandle(&hwnd_));
    apply_window_icon(hwnd_);
    focus_ = xaml::Controls::ContentControl();
    focus_.IsTabStop(true);
    focus_.UseSystemFocusVisuals(false);
    focus_.HorizontalContentAlignment(xaml::HorizontalAlignment::Stretch);
    focus_.VerticalContentAlignment(xaml::VerticalAlignment::Stretch);
    focus_.Background(xaml::Media::SolidColorBrush(winrt::Windows::UI::Color{255, 0, 0, 0}));
    panel_ = xaml::Controls::SwapChainPanel();
    cursor_host_ = winrt::make_self<CursorHost>();
    cursor_host_->IsTabStop(false);
    cursor_host_->HorizontalContentAlignment(xaml::HorizontalAlignment::Stretch);
    cursor_host_->VerticalContentAlignment(xaml::VerticalAlignment::Stretch);
    cursor_host_->Content(panel_);
    root_ = xaml::Controls::Grid();
    root_.Children().Append(*cursor_host_);
    focus_.Content(root_);
    window_.Content(focus_);
    winrt::check_hresult(panel_.as<ISwapChainPanelNative>()->SetSwapChain(pipeline_->swap_chain()));
    build_bar();

    panel_.SizeChanged([weak](auto&&, auto&&) {
        if (const auto self = weak.lock()) self->resize_surface();
    });
    panel_.PointerEntered([weak](auto&&, auto&&) {
        if (const auto self = weak.lock()) {
            self->pointer_inside_ = true;
            self->apply_cursor_visibility();
        }
    });
    panel_.PointerExited([weak](auto&&, auto&&) {
        if (const auto self = weak.lock()) {
            self->pointer_inside_ = false;
            self->apply_cursor_visibility();
        }
    });
    focus_.KeyDown([weak](auto&&, xaml::Input::KeyRoutedEventArgs const& args) {
        if (const auto self = weak.lock()) self->key(args, true);
    });
    focus_.KeyUp([weak](auto&&, xaml::Input::KeyRoutedEventArgs const& args) {
        if (const auto self = weak.lock()) self->key(args, false);
    });
    panel_.PointerMoved([weak](auto&&, xaml::Input::PointerRoutedEventArgs const& args) {
        if (const auto self = weak.lock()) self->pointer_moved(args);
    });
    panel_.PointerPressed([weak](auto&&, xaml::Input::PointerRoutedEventArgs const& args) {
        if (const auto self = weak.lock()) self->pointer_button(args, true);
    });
    panel_.PointerReleased([weak](auto&&, xaml::Input::PointerRoutedEventArgs const& args) {
        if (const auto self = weak.lock()) self->pointer_button(args, false);
    });
    panel_.PointerWheelChanged([weak](auto&&, xaml::Input::PointerRoutedEventArgs const& args) {
        if (const auto self = weak.lock()) self->pointer_wheel(args);
    });
    window_.Activated([weak](auto&&, xaml::WindowActivatedEventArgs const& args) {
        const auto self = weak.lock();
        if (!self || self->closed_) return;
        if (args.WindowActivationState() == xaml::WindowActivationState::Deactivated) {
            self->capture_mouse(false);
            self->release_all();
            self->pointer_inside_ = false;
            self->apply_cursor_visibility();
        } else {
            self->flash(false);
            self->focus_.Focus(xaml::FocusState::Programmatic);
        }
    });
    window_.AppWindow().Closing([weak](auto&&, auto&&) {
        if (const auto self = weak.lock(); self && !self->closed_) self->close(true);
    });

    winrt::check_bool(SetWindowSubclass(hwnd_, subclass, 2, reinterpret_cast<DWORD_PTR>(this)));
    const auto menu = GetSystemMenu(hwnd_, FALSE);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, capture_command, L"Capture mouse\tCtrl+Alt+G releases");
    AppendMenuW(menu, MF_STRING, scaling_command, L"Allow scaling");
    AppendMenuW(menu, MF_STRING, half_command, L"Size 0.5×");
    AppendMenuW(menu, MF_STRING, actual_command, L"Size 1× (one video pixel per screen pixel)");
    AppendMenuW(menu, MF_STRING, double_command, L"Size 2×");
    AppendMenuW(menu, MF_STRING, fullscreen_command, L"Fullscreen\tEsc Esc exits");
    AppendMenuW(menu, MF_STRING, screenshot_command, L"Save PNG screenshot…");
    AppendMenuW(menu, MF_STRING, info_command, L"Stream info…");
    AppendMenuW(menu, MF_STRING, rate_command, L"Change parameters…");
    set_scaling(false);
    load_icon();
}

void StreamWindow::open(std::function<void(std::expected<void, SessionError>)> opened) {
    const auto queue = DispatcherQueue::GetForCurrentThread();
    const std::weak_ptr<StreamWindow> weak = weak_from_this();
    StreamCallbacks callbacks;
    callbacks.configured = [sink = sink_, queue, weak](const VideoParameters& video) {
        if (!sink->active) return;
        sink->pipeline->configure(video);
        post(queue, [weak, video] {
            if (const auto self = weak.lock()) self->update_video(video);
        });
    };
    callbacks.frame = [sink = sink_](const VideoFrame& frame) {
        if (sink->active) sink->pipeline->submit(frame);
    };
    callbacks.failed = [queue, weak](const SessionError& error) {
        post(queue, [weak, error] {
            if (const auto self = weak.lock(); self && !self->closed_) {
                self->report("Stream connection failed: " + error.message);
                self->close(true);
            }
        });
    };
    session_.open_stream(offer_, std::move(callbacks), std::move(opened));
}

void StreamWindow::show(bool activate) {
    if (closed_) return;
    if (activate) {
        window_.Activate();
        focus_.Focus(xaml::FocusState::Programmatic);
    } else {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    }
}

void StreamWindow::close(bool send_stop) {
    if (closed_) return;
    closed_ = true;
    capture_mouse(false);
    apply_cursor_visibility();
    if (bar_timer_) bar_timer_.Stop();
    if (send_stop) release_all();
    sink_->active = false;
    if (send_stop) {
        // The host closes the stream socket after replying; close it locally once the reply arrives.
        session_.request("stop_stream", {{"stream_id", offer_.stream_id}}, "stream_stopped",
            [session = &session_, id = offer_.stream_id](auto&&) { session->close_stream(id); });
    } else {
        session_.close_stream(offer_.stream_id);
    }
    {
        std::scoped_lock lock(link_->mutex);
        link_->session = nullptr;
    }
    RemoveWindowSubclass(hwnd_, subclass, 2);
    window_.Close();
    if (callbacks_.closed) callbacks_.closed(offer_.stream_id);
}

void StreamWindow::close_after_host_stop() { close(false); }

void StreamWindow::update_video(const VideoParameters& video) {
    if (video.generation <= video_.generation) return;
    const bool resized = video.width != video_.width || video.height != video_.height;
    video_ = video;
    if (resized) apply_size();
}

void StreamWindow::apply_size() {
    if (closed_ || fullscreen_) return;
    const auto monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(MONITORINFO)};
    GetMonitorInfoW(monitor, &info);
    const auto available_width = static_cast<double>(info.rcWork.right - info.rcWork.left) * 0.95;
    const auto available_height = static_cast<double>(info.rcWork.bottom - info.rcWork.top) * 0.9;
    auto width = video_.width * scale_;
    auto height = video_.height * scale_;
    // Keep the window on screen; the presenter letterboxes to preserve the picture's aspect ratio.
    const auto fit = std::min({1.0, available_width / width, available_height / height});
    width *= fit;
    height *= fit;
    window_.AppWindow().ResizeClient({static_cast<int32_t>(std::lround(width)), static_cast<int32_t>(std::lround(height))});
}

void StreamWindow::resize_surface() {
    const auto root = panel_.XamlRoot();
    const auto scale = root ? root.RasterizationScale() : 1.0;
    const auto width = std::lround(panel_.ActualWidth() * scale);
    const auto height = std::lround(panel_.ActualHeight() * scale);
    if (width > 0 && height > 0) pipeline_->resize(static_cast<unsigned>(width), static_cast<unsigned>(height));
}

void StreamWindow::set_scaling(bool allowed) {
    allow_scaling_ = allowed;
    if (!allowed) scale_ = 1.0;
    if (const auto presenter = window_.AppWindow().Presenter().try_as<winrt::Microsoft::UI::Windowing::OverlappedPresenter>()) {
        presenter.IsResizable(allowed);
        presenter.IsMaximizable(allowed);
    }
    const auto menu = GetSystemMenu(hwnd_, FALSE);
    CheckMenuItem(menu, scaling_command, MF_BYCOMMAND | (allowed ? MF_CHECKED : MF_UNCHECKED));
    for (const auto id : {half_command, actual_command, double_command}) {
        EnableMenuItem(menu, id, MF_BYCOMMAND | (allowed ? MF_ENABLED : MF_GRAYED));
    }
    apply_size();
}

void StreamWindow::set_scale(double scale) {
    if (!allow_scaling_) return;
    if (fullscreen_) toggle_fullscreen();
    scale_ = scale;
    apply_size();
}

void StreamWindow::toggle_fullscreen() {
    using winrt::Microsoft::UI::Windowing::AppWindowPresenterKind;
    if (!fullscreen_) {
        GetWindowPlacement(hwnd_, &windowed_);
        window_.AppWindow().SetPresenter(AppWindowPresenterKind::FullScreen);
        fullscreen_ = true;
        // The connection bar stays hidden (auto-hide) until the pointer touches the top edge.
    } else {
        show_bar(false);
        window_.AppWindow().SetPresenter(AppWindowPresenterKind::Overlapped);
        SetWindowPlacement(hwnd_, &windowed_);
        fullscreen_ = false;
        set_scaling(allow_scaling_);
    }
    last_escape_ = 0;
}

void StreamWindow::build_bar() {
    const std::weak_ptr<StreamWindow> weak = weak_from_this();
    pin_ = xaml::Controls::Primitives::ToggleButton();
    {
        xaml::Controls::FontIcon icon;
        icon.FontFamily(xaml::Media::FontFamily(L"Segoe Fluent Icons,Segoe MDL2 Assets"));
        icon.Glyph(L"\uE718");
        icon.FontSize(14);
        pin_.Content(icon);
        pin_.Padding(xaml::Thickness{10, 6, 10, 6});
        pin_.IsTabStop(false);
        xaml::Controls::ToolTipService::SetToolTip(pin_, winrt::box_value(L"Pin the connection bar"));
    }
    pin_.Click([weak](auto&&, auto&&) {
        const auto self = weak.lock();
        if (self && !self->pin_.IsChecked().Value()) self->schedule_bar_hide(std::chrono::milliseconds(800));
    });
    xaml::Controls::TextBlock title;
    title.Text(this->title());
    title.VerticalAlignment(xaml::VerticalAlignment::Center);
    title.TextTrimming(xaml::TextTrimming::CharacterEllipsis);
    title.MaxWidth(420);
    title.Margin(xaml::Thickness{12, 0, 12, 0});
    auto minimize_button = bar_button(L'\uE921', L"Minimize (the stream stays connected)");
    minimize_button.Click([weak](auto&&, auto&&) {
        if (const auto self = weak.lock()) self->minimize();
    });
    auto restore_button = bar_button(L'\uE73F', L"Exit fullscreen");
    restore_button.Click([weak](auto&&, auto&&) {
        if (const auto self = weak.lock(); self && self->fullscreen_) self->toggle_fullscreen();
    });
    auto close_button = bar_button(L'\uE8BB', L"Close this stream");
    close_button.Click([weak](auto&&, auto&&) {
        if (const auto self = weak.lock(); self && !self->closed_) self->close(true);
    });
    xaml::Controls::StackPanel content;
    content.Orientation(xaml::Controls::Orientation::Horizontal);
    content.Spacing(4);
    content.Children().Append(pin_);
    content.Children().Append(title);
    content.Children().Append(minimize_button);
    content.Children().Append(restore_button);
    content.Children().Append(close_button);

    bar_ = xaml::Controls::Border();
    bar_.RequestedTheme(xaml::ElementTheme::Dark);
    bar_.Background(xaml::Media::SolidColorBrush(winrt::Windows::UI::Color{235, 32, 32, 32}));
    bar_.CornerRadius(xaml::CornerRadius{0, 0, 8, 8});
    bar_.Padding(xaml::Thickness{6, 4, 6, 4});
    bar_.HorizontalAlignment(xaml::HorizontalAlignment::Center);
    bar_.VerticalAlignment(xaml::VerticalAlignment::Top);
    bar_.Child(content);
    xaml::Media::Animation::EdgeUIThemeTransition slide;
    slide.Edge(xaml::Controls::Primitives::EdgeTransitionLocation::Top);
    xaml::Media::Animation::TransitionCollection transitions;
    transitions.Append(slide);
    bar_.Transitions(transitions);
    // Pointer events over the bar never reach the video panel, so they are not forwarded to the host.
    bar_.PointerEntered([weak](auto&&, auto&&) {
        if (const auto self = weak.lock()) {
            self->bar_hover_ = true;
            if (self->bar_timer_) self->bar_timer_.Stop();
        }
    });
    bar_.PointerExited([weak](auto&&, auto&&) {
        if (const auto self = weak.lock()) {
            self->bar_hover_ = false;
            self->schedule_bar_hide(std::chrono::milliseconds(800));
        }
    });

    bar_timer_ = DispatcherQueue::GetForCurrentThread().CreateTimer();
    bar_timer_.IsRepeating(false);
    bar_timer_.Tick([weak](auto&&, auto&&) {
        const auto self = weak.lock();
        if (self && !self->bar_hover_ && !self->pin_.IsChecked().Value()) self->show_bar(false);
    });
}

void StreamWindow::show_bar(bool shown) {
    if (shown == bar_shown_ || (shown && (!fullscreen_ || closed_))) return;
    bar_shown_ = shown;
    if (shown) {
        root_.Children().Append(bar_);
        return;
    }
    bar_timer_.Stop();
    bar_hover_ = false;
    if (uint32_t index{}; root_.Children().IndexOf(bar_, index)) root_.Children().RemoveAt(index);
}

void StreamWindow::schedule_bar_hide(std::chrono::milliseconds delay) {
    if (!bar_shown_ || pin_.IsChecked().Value()) return;
    bar_timer_.Stop();
    bar_timer_.Interval(delay);
    bar_timer_.Start();
}

void StreamWindow::minimize() {
    release_all();
    capture_mouse(false);
    show_bar(false);
    // The fullscreen presenter is kept, so restoring from the taskbar returns to fullscreen.
    ShowWindow(hwnd_, SW_MINIMIZE);
}

void StreamWindow::update_cursor(std::string shape) {
    if (closed_ || offer_.cursor != "client" || shape == cursor_shape_) return;
    cursor_shape_ = std::move(shape);
    if (cursor_shape_ != "none") {
        cursor_host_->cursor(winrt::Microsoft::UI::Input::InputSystemCursor::Create(cursor_shape(cursor_shape_)));
    }
    apply_cursor_visibility();
}

// The host hid its pointer (for example while typing); hide the local one over the video too. ShowCursor keeps a
// per-thread count, so this stays balanced with mouse capture.
void StreamWindow::apply_cursor_visibility() {
    const bool hide = !closed_ && pointer_inside_ && !mouse_captured_ && cursor_shape_ == "none";
    if (hide == cursor_hidden_) return;
    cursor_hidden_ = hide;
    ShowCursor(hide ? FALSE : TRUE);
}

void StreamWindow::system_menu() {
    release_all();
    capture_mouse(false);
    POINT point{};
    ClientToScreen(hwnd_, &point);
    const auto selected = TrackPopupMenu(GetSystemMenu(hwnd_, FALSE), TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
                                         point.x, point.y, 0, hwnd_, nullptr);
    if (selected) SendMessageW(hwnd_, WM_SYSCOMMAND, selected, 0);
}

void StreamWindow::command(UINT id) {
    switch (id) {
    case capture_command: capture_mouse(!mouse_captured_); break;
    case scaling_command: set_scaling(!allow_scaling_); break;
    case half_command: set_scale(0.5); break;
    case actual_command: set_scale(1.0); break;
    case double_command: set_scale(2.0); break;
    case fullscreen_command: toggle_fullscreen(); break;
    case screenshot_command: save_screenshot(); break;
    case rate_command: change_parameters(); break;
    case info_command: {
        const auto stats = pipeline_->stats();
        const auto clock = session_.clock();
        const auto text = std::format(
            "Source: {} {}\nCodec: {} {} {} {}-bit, generation {}, {}×{}\nHost encoder: {}\n"
            "Rate: {} FPS, {} kbit/s average, {} kbit/s peak ({})\nDecoder: {}\nOutput: {} ({})\n"
            "Received {} · decoded {} · presented (enqueued) {} · coalesced {} · dropped {}\n"
            "Decode time: {:.2f} ms average, {:.2f} ms max\nClock: {}\n\n"
            "Presentation counts measure swap-chain enqueue, not display completion.",
            offer_.source.kind, offer_.source.title, video_.codec, video_.codec_string, video_.chroma_format,
            video_.bit_depth, video_.generation, video_.width, video_.height,
            video_.encoder.empty() ? std::string("not reported") : video_.encoder, video_.frame_rate.value_or(0),
            video_.bitrate_kbps.value_or(0), video_.max_bitrate_kbps.value_or(0),
            video_.rate_control.empty() ? std::string("?") : video_.rate_control,
            stats.decoder.empty() ? std::string("none yet") : stats.decoder, stats.output, stats.output_format,
            stats.received, stats.decoded, stats.presented, stats.coalesced, stats.dropped, stats.decode_ms_average,
            stats.decode_ms_max,
            clock ? std::format("offset {:.0f} µs, RTT {:.0f} µs", clock->offset_us, clock->rtt_us) : std::string("no sample"));
        MessageBoxW(hwnd_, widen(text).c_str(), (title() + L" · stream info").c_str(), MB_OK | MB_ICONINFORMATION);
        break;
    }
    default: break;
    }
}

void StreamWindow::save_screenshot() {
    std::vector<std::byte> png;
    try {
        png = pipeline_->screenshot_png();
    } catch (const std::exception& error) {
        report(std::string("Screenshot failed: ") + error.what());
        return;
    }
    if (png.empty()) {
        report("No picture has been decoded yet.");
        return;
    }
    winrt::com_ptr<IFileSaveDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())))) return;
    const COMDLG_FILTERSPEC types[]{{L"PNG image", L"*.png"}};
    dialog->SetFileTypes(1, types);
    dialog->SetDefaultExtension(L"png");
    dialog->SetFileName(L"BeamIt screenshot.png");
    if (FAILED(dialog->Show(hwnd_))) return;
    winrt::com_ptr<IShellItem> item;
    PWSTR path{};
    if (FAILED(dialog->GetResult(item.put())) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    CoTaskMemFree(path);
    file.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    if (!file) report("The screenshot could not be written.");
}

winrt::fire_and_forget StreamWindow::change_parameters() {
    const auto self = shared_from_this();
    release_all();
    capture_mouse(false);
    const auto number = [](std::wstring_view header, double value, double maximum) {
        xaml::Controls::NumberBox box;
        box.Header(winrt::box_value(winrt::hstring(header)));
        box.Minimum(header.starts_with(L"Peak") ? 0 : 1);
        box.Maximum(maximum);
        box.SpinButtonPlacementMode(xaml::Controls::NumberBoxSpinButtonPlacementMode::Compact);
        box.Value(value);
        return box;
    };
    auto bitrate = number(L"Average bitrate (kbit/s, 1–1,000,000)", video_.bitrate_kbps.value_or(12000), 1'000'000);
    auto peak = number(L"Peak bitrate (kbit/s, 0 = twice the average; clear to keep the current policy)",
                       std::numeric_limits<double>::quiet_NaN(), 4'000'000);
    peak.PlaceholderText(winrt::hstring(std::format(L"Keep current policy (effective peak {} kbit/s)",
                                                    video_.max_bitrate_kbps.value_or(0))));
    auto frame_rate = number(L"Frame rate (1–240)", video_.frame_rate.value_or(60), 240);
    xaml::Controls::StackPanel content;
    content.Spacing(12);
    content.Children().Append(bitrate);
    content.Children().Append(peak);
    content.Children().Append(frame_rate);
    xaml::Controls::ContentDialog dialog;
    dialog.XamlRoot(focus_.XamlRoot());
    dialog.Title(winrt::box_value(L"Stream parameters"));
    dialog.Content(content);
    dialog.PrimaryButtonText(L"Apply");
    dialog.CloseButtonText(L"Cancel");
    dialog.DefaultButton(xaml::Controls::ContentDialogButton::Primary);
    if (co_await dialog.ShowAsync() != xaml::Controls::ContentDialogResult::Primary || closed_) co_return;
    if (std::isnan(bitrate.Value()) || std::isnan(frame_rate.Value())) {
        report("Enter an average bitrate and a frame rate.");
        co_return;
    }
    json::object fields{{"stream_id", offer_.stream_id},
                        {"bitrate_kbps", static_cast<std::int64_t>(std::clamp(bitrate.Value(), 1.0, 1'000'000.0))},
                        {"frame_rate", static_cast<std::int64_t>(std::clamp(frame_rate.Value(), 1.0, 240.0))}};
    if (!std::isnan(peak.Value())) fields["max_bitrate_kbps"] = static_cast<std::int64_t>(std::clamp(peak.Value(), 0.0, 4'000'000.0));
    const std::weak_ptr<StreamWindow> weak = self;
    // The request cannot be undone; the reply reports the values the host actually applied.
    session_.request("set_stream_rate", std::move(fields), "stream_rate", [weak](std::expected<json::object, SessionError> reply) {
        const auto window = weak.lock();
        if (!window || window->closed_) return;
        if (!reply) {
            window->report("Changing stream parameters failed: " + reply.error().message);
            return;
        }
        const auto generation = integer_field(*reply, "generation");
        const auto bitrate_value = integer_field(*reply, "bitrate_kbps");
        const auto frame_rate_value = integer_field(*reply, "frame_rate");
        const auto peak_value = integer_field(*reply, "max_bitrate_kbps");
        if (!generation || !bitrate_value || !frame_rate_value || *generation != window->video_.generation) return;
        window->video_.bitrate_kbps = static_cast<std::uint32_t>(*bitrate_value);
        window->video_.frame_rate = static_cast<std::uint32_t>(*frame_rate_value);
        if (peak_value) window->video_.max_bitrate_kbps = static_cast<std::uint32_t>(*peak_value);
    });
}

void StreamWindow::load_icon() {
    if (offer_.source.hwnd.empty()) return;
    const std::weak_ptr<StreamWindow> weak = weak_from_this();
    session_.request("get_window_icon", {{"hwnd", offer_.source.hwnd}, {"size", 256}}, "window_icon",
        [weak](std::expected<json::object, SessionError> reply) {
            const auto self = weak.lock();
            if (!self || self->closed_ || !reply) return;
            const auto data = string_field(*reply, "data");
            const auto png = data && data->size() <= 1024 * 1024 ? net::base64_decode(*data) : std::nullopt;
            if (!png || string_field(*reply, "mime_type") != "image/png") return;
            const auto large_icon = icon_from_png(*png, GetSystemMetrics(SM_CXICON));
            const auto small_icon = icon_from_png(*png, GetSystemMetrics(SM_CXSMICON));
            if (large_icon) SendMessageW(self->hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(large_icon));
            if (small_icon) SendMessageW(self->hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon));
            if (self->icon_) DestroyIcon(self->icon_);
            if (self->small_icon_) DestroyIcon(self->small_icon_);
            self->icon_ = large_icon;
            self->small_icon_ = small_icon;
        });
}

void StreamWindow::update_attention(const std::string& hwnd, bool requesting) {
    const bool changed = requesting ? attention_.insert(hwnd).second : attention_.erase(hwnd) != 0;
    if (!changed) return;
    flash(!attention_.empty() && GetForegroundWindow() != hwnd_);
}

void StreamWindow::flash(bool on) {
    FLASHWINFO info{sizeof(FLASHWINFO), hwnd_, on ? static_cast<DWORD>(FLASHW_TRAY | FLASHW_TIMERNOFG) : FLASHW_STOP, 0, 0};
    FlashWindowEx(&info);
}

void StreamWindow::report(const std::string& message) {
    if (callbacks_.error) callbacks_.error(narrow(title()) + ": " + message);
}

// Input --------------------------------------------------------------------------------------------

void StreamWindow::send_input(std::string type, json::object fields) {
    if (closed_) return;
    fields["type"] = std::move(type);
    fields["stream_id"] = offer_.stream_id;
    session_.send(std::move(fields));
}

void StreamWindow::key(xaml::Input::KeyRoutedEventArgs const& args, bool down) {
    args.Handled(true);
    const auto virtual_key = static_cast<unsigned>(args.Key());
    const auto status = args.KeyStatus();
    if (down) {
        // Local shortcuts are consumed before forwarding (Alt+Space menu, Ctrl+Alt+G release, Esc Esc).
        if (virtual_key == VK_SPACE && held(VK_MENU) && !held(VK_CONTROL)) {
            system_menu();
            return;
        }
        if (virtual_key == 'G' && held(VK_CONTROL) && held(VK_MENU)) {
            capture_mouse(false);
            release_all();
            return;
        }
        if (virtual_key == VK_ESCAPE && fullscreen_ && !status.WasKeyDown) {
            const auto now = GetTickCount64();
            if (last_escape_ != 0 && now - last_escape_ <= 1000) {
                toggle_fullscreen();
                return;
            }
            last_escape_ = now;
        }
    }
    const auto code = keyboard_code(status.ScanCode, status.IsExtendedKey, virtual_key);
    if (!code) return;
    const std::string name(*code);
    if (!down && !held_keys_.erase(name)) return;
    if (down) held_keys_.insert(name);
    send_input("key", {{"code", name}, {"down", down}});
}

std::optional<std::pair<double, double>> StreamWindow::video_position(winrt::Windows::Foundation::Point point) const {
    const auto root = panel_.XamlRoot();
    const auto scale = root ? root.RasterizationScale() : 1.0;
    const auto width = panel_.ActualWidth() * scale;
    const auto height = panel_.ActualHeight() * scale;
    if (width <= 0 || height <= 0 || video_.width == 0 || video_.height == 0) return std::nullopt;
    const auto fit = std::min(width / video_.width, height / video_.height);
    const auto shown_width = video_.width * fit;
    const auto shown_height = video_.height * fit;
    const auto left = (width - shown_width) / 2;
    const auto top = (height - shown_height) / 2;
    auto x = (point.X * scale - left) / fit;
    auto y = (point.Y * scale - top) / fit;
    const bool inside = x >= 0 && y >= 0 && x < video_.width && y < video_.height;
    if (!inside && held_buttons_.empty()) return std::nullopt;
    x = std::clamp(x, 0.0, video_.width - 1.0);
    y = std::clamp(y, 0.0, video_.height - 1.0);
    return std::pair{std::round(x * 100) / 100, std::round(y * 100) / 100};
}

void StreamWindow::pointer_moved(xaml::Input::PointerRoutedEventArgs const& args) {
    args.Handled(true);
    if (mouse_captured_ || mode_change_pending_) return;
    const auto point = args.GetCurrentPoint(panel_).Position();
    if (fullscreen_ && held_buttons_.empty() && point.Y <= 1.0) show_bar(true);
    const auto position = video_position(point);
    if (!position || position == last_position_) return;
    last_position_ = position;
    send_input("mouse_move", {{"x", position->first}, {"y", position->second}});
}

void StreamWindow::pointer_button(xaml::Input::PointerRoutedEventArgs const& args, bool down) {
    using winrt::Microsoft::UI::Input::PointerUpdateKind;
    args.Handled(true);
    if (down) focus_.Focus(xaml::FocusState::Pointer);
    const auto point = args.GetCurrentPoint(panel_);
    const char* button{};
    switch (point.Properties().PointerUpdateKind()) {
    case PointerUpdateKind::LeftButtonPressed:
    case PointerUpdateKind::LeftButtonReleased: button = "left"; break;
    case PointerUpdateKind::RightButtonPressed:
    case PointerUpdateKind::RightButtonReleased: button = "right"; break;
    case PointerUpdateKind::MiddleButtonPressed:
    case PointerUpdateKind::MiddleButtonReleased: button = "middle"; break;
    case PointerUpdateKind::XButton1Pressed:
    case PointerUpdateKind::XButton1Released: button = "back"; break;
    case PointerUpdateKind::XButton2Pressed:
    case PointerUpdateKind::XButton2Released: button = "forward"; break;
    default: return;
    }
    if (mode_change_pending_) return;
    if (down) {
        if (!mouse_captured_) {
            const auto position = video_position(point.Position());
            if (!position) return;
            last_position_ = position;
            send_input("mouse_move", {{"x", position->first}, {"y", position->second}});
        }
        panel_.CapturePointer(args.Pointer());
        held_buttons_.insert(button);
    } else {
        if (!held_buttons_.erase(button)) return;
        if (held_buttons_.empty()) panel_.ReleasePointerCaptures();
    }
    send_input("mouse_button", {{"button", button}, {"down", down}});
}

void StreamWindow::pointer_wheel(xaml::Input::PointerRoutedEventArgs const& args) {
    args.Handled(true);
    const auto point = args.GetCurrentPoint(panel_);
    if (!mouse_captured_ && !video_position(point.Position())) return;
    const auto properties = point.Properties();
    const auto delta = std::clamp(properties.MouseWheelDelta(), -12000, 12000);
    if (delta == 0) return;
    // Windows reports positive vertical deltas away from the user; the protocol uses DOM deltaY signs.
    if (properties.IsHorizontalMouseWheel()) send_input("mouse_wheel", {{"dx", delta}, {"dy", 0}});
    else send_input("mouse_wheel", {{"dx", 0}, {"dy", -delta}});
}

void StreamWindow::release_all() {
    if (held_keys_.empty() && held_buttons_.empty()) return;
    held_keys_.clear();
    held_buttons_.clear();
    if (panel_) panel_.ReleasePointerCaptures();
    send_input("release_all");
}

void StreamWindow::capture_mouse(bool capture) {
    if (capture == mouse_captured_ || mode_change_pending_ || (closed_ && capture)) return;
    if (!capture) {
        mouse_captured_ = false;
        ClipCursor(nullptr);
        ShowCursor(TRUE);
        apply_cursor_visibility();
        RAWINPUTDEVICE device{0x01, 0x02, RIDEV_REMOVE, nullptr};
        RegisterRawInputDevices(&device, 1, sizeof(device));
        window_.Title(title());
        if (closed_) return;
        mode_change_pending_ = true;
        const std::weak_ptr<StreamWindow> weak = weak_from_this();
        session_.request("set_mouse_mode", {{"stream_id", offer_.stream_id}, {"mode", "absolute"}}, "mouse_mode",
            [weak](auto&&) {
                if (const auto self = weak.lock()) self->mode_change_pending_ = false;
            });
        return;
    }
    // Input is blocked until the host acknowledges the mode, so deltas are never sent in absolute mode.
    mode_change_pending_ = true;
    const std::weak_ptr<StreamWindow> weak = weak_from_this();
    session_.request("set_mouse_mode", {{"stream_id", offer_.stream_id}, {"mode", "relative"}}, "mouse_mode",
        [weak](std::expected<json::object, SessionError> reply) {
            const auto self = weak.lock();
            if (!self) return;
            self->mode_change_pending_ = false;
            if (!reply) {
                self->report("Mouse capture failed: " + reply.error().message);
                return;
            }
            if (self->closed_ || GetForegroundWindow() != self->hwnd_) {
                self->capture_mouse(false);
                return;
            }
            RAWINPUTDEVICE device{0x01, 0x02, 0, self->hwnd_};
            if (!RegisterRawInputDevices(&device, 1, sizeof(device))) {
                self->report("Raw mouse input is unavailable.");
                self->mouse_captured_ = true;
                self->capture_mouse(false);
                return;
            }
            RECT client{};
            GetClientRect(self->hwnd_, &client);
            MapWindowPoints(self->hwnd_, nullptr, reinterpret_cast<POINT*>(&client), 2);
            ClipCursor(&client);
            ShowCursor(FALSE);
            self->mouse_captured_ = true;
            self->apply_cursor_visibility();
            self->window_.Title(self->title() + L" — Ctrl+Alt+G releases the mouse");
        });
}

void StreamWindow::raw_input(LPARAM lparam) {
    if (!mouse_captured_) return;
    RAWINPUT input{};
    UINT size = sizeof(input);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1) ||
        input.header.dwType != RIM_TYPEMOUSE || (input.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0) {
        return;
    }
    const auto dx = input.data.mouse.lLastX;
    const auto dy = input.data.mouse.lLastY;
    if (dx != 0 || dy != 0) send_input("mouse_move", {{"dx", dx}, {"dy", dy}});
}

LRESULT CALLBACK StreamWindow::subclass(HWND, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR data) {
    const auto self = reinterpret_cast<StreamWindow*>(data);
    try {
        return self->message(message, wparam, lparam);
    } catch (const winrt::hresult_error& error) {
        self->report(narrow(error.message()));
    } catch (const std::exception& error) {
        self->report(error.what());
    }
    return 0;
}

LRESULT StreamWindow::message(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_SYSCOMMAND:
        if (const auto id = static_cast<UINT>(wparam & 0xFFF0); id >= first_command && id <= last_command) {
            command(id);
            return 0;
        }
        if ((wparam & 0xFFF0) == SC_KEYMENU) release_all();
        break;
    case WM_INPUT:
        raw_input(lparam);
        break;
    case WM_DPICHANGED:
        resize_surface();
        break;
    case WM_NCDESTROY: {
        const auto window = hwnd_;
        RemoveWindowSubclass(window, subclass, 2);
        return DefSubclassProc(window, message, wparam, lparam);
    }
    default: break;
    }
    return DefSubclassProc(hwnd_, message, wparam, lparam);
}

} // namespace beamit::client
