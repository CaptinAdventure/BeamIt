#include "host/AttentionMonitor.hpp"

#include <spdlog/spdlog.h>

#include <future>
#include <thread>

namespace beamit::host {

std::optional<AttentionEvent> AttentionTracker::flashed(HWND window, DWORD pid, bool is_foreground) {
    // The taskbar never highlights the active window; flashes reported right after activation are leftovers of a
    // counted flash.
    if (is_foreground) return std::nullopt;
    if (!windows_.try_emplace(window, pid).second) return std::nullopt;
    return AttentionEvent{.window = window, .pid = pid, .requested = true};
}

std::optional<AttentionEvent> AttentionTracker::activated(HWND window) { return clear(window); }

std::optional<AttentionEvent> AttentionTracker::destroyed(HWND window) { return clear(window); }

std::optional<AttentionEvent> AttentionTracker::clear(HWND window) {
    const auto found = windows_.find(window);
    if (found == windows_.end()) return std::nullopt;
    AttentionEvent event{.window = window, .pid = found->second, .requested = false};
    windows_.erase(found);
    return event;
}

std::vector<AttentionEvent> AttentionTracker::active() const {
    std::vector<AttentionEvent> events;
    for (const auto& [window, pid] : windows_) events.push_back({.window = window, .pid = pid, .requested = true});
    return events;
}

struct AttentionMonitor::Impl {
    Callback callback;
    AttentionTracker tracker;
    UINT shell_message{};
    DWORD thread_id{};
    std::jthread thread;

    void notify(const std::optional<AttentionEvent>& event) {
        if (event) callback(*event);
    }

    void on_shell(WPARAM code, HWND window) {
        switch (code) {
        case HSHELL_FLASH: {
            DWORD pid{};
            GetWindowThreadProcessId(window, &pid);
            notify(tracker.flashed(window, pid, GetForegroundWindow() == window));
            break;
        }
        case HSHELL_WINDOWACTIVATED:
        case HSHELL_RUDEAPPACTIVATED: notify(tracker.activated(window)); break;
        case HSHELL_WINDOWDESTROYED: notify(tracker.destroyed(window)); break;
        default: break;
        }
    }

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (self && message == self->shell_message) {
            self->on_shell(wparam, reinterpret_cast<HWND>(lparam));
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    void run(std::promise<void>& started) {
        constexpr wchar_t class_name[] = L"BeamItAttentionMonitor";
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW window_class{.cbSize = sizeof(WNDCLASSEXW), .lpfnWndProc = window_proc, .hInstance = instance,
                                 .lpszClassName = class_name};
        RegisterClassExW(&window_class);
        shell_message = RegisterWindowMessageW(L"SHELLHOOK");
        // Shell-hook notifications go to top-level windows; this one is never shown.
        const HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, class_name, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                            instance, this);
        MSG message{};
        PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE); // create the queue before announcing the thread id
        thread_id = GetCurrentThreadId();
        const bool registered = window != nullptr && shell_message != 0 && RegisterShellHookWindow(window);
        if (!registered) {
            spdlog::warn("Taskbar attention monitoring unavailable (error {})", GetLastError());
        }
        started.set_value();

        while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
        if (window) {
            if (registered) DeregisterShellHookWindow(window);
            DestroyWindow(window);
        }
        UnregisterClassW(class_name, instance);
    }
};

AttentionMonitor::AttentionMonitor(Callback callback) : impl_(std::make_unique<Impl>()) {
    impl_->callback = std::move(callback);
    std::promise<void> started;
    auto ready = started.get_future();
    impl_->thread = std::jthread([this, &started] { impl_->run(started); });
    ready.wait();
}

AttentionMonitor::~AttentionMonitor() {
    PostThreadMessageW(impl_->thread_id, WM_QUIT, 0, 0);
    impl_->thread.join();
}

} // namespace beamit::host
