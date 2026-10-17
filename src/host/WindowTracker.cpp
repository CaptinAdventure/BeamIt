#include "host/WindowTracker.hpp"

#include "Utf8.hpp"

#include <dwmapi.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <future>
#include <thread>
#include <utility>

namespace beamit::host {

WindowRole classify_window(DWORD style, DWORD ex_style, bool owned, std::wstring_view class_name) {
    if (class_name == L"#32768") return WindowRole::Popup; // standard menu
    // Taskbar rule: WS_EX_APPWINDOW forces a button; otherwise unowned, non-tool, activatable windows get one.
    if (ex_style & WS_EX_APPWINDOW) return WindowRole::Main;
    if (!owned && !(ex_style & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE))) return WindowRole::Main;
    if (ex_style & WS_EX_NOACTIVATE) return WindowRole::Popup;
    const bool captioned = (style & WS_CAPTION) == WS_CAPTION;
    if (!captioned && ((style & WS_POPUP) || (ex_style & WS_EX_TOOLWINDOW))) return WindowRole::Popup;
    if (ex_style & WS_EX_TOOLWINDOW) return WindowRole::Tool;
    return WindowRole::Owned;
}

std::string_view window_role_name(WindowRole role) {
    switch (role) {
    case WindowRole::Main: return "main";
    case WindowRole::Owned: return "owned";
    case WindowRole::Popup: return "popup";
    case WindowRole::Tool: return "tool";
    }
    return "owned";
}

std::string_view window_show_state_name(WindowShowState state) {
    switch (state) {
    case WindowShowState::Normal: return "normal";
    case WindowShowState::Minimized: return "minimized";
    case WindowShowState::Maximized: return "maximized";
    }
    return "normal";
}

std::wstring process_image_path(DWORD pid) {
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    std::wstring path(32768, L'\0');
    auto size = static_cast<DWORD>(path.size());
    const bool queried = QueryFullProcessImageNameW(process, 0, path.data(), &size);
    CloseHandle(process);
    if (!queried) return {};
    path.resize(size);
    return path;
}

std::string process_image_name(DWORD pid) {
    const auto path = process_image_path(pid);
    if (path.empty()) return {};
    return to_utf8(std::wstring_view(path).substr(path.find_last_of(L"\\/") + 1));
}

std::vector<WindowEvent> WindowList::update(const std::vector<WindowState>& snapshot) {
    const auto find = [](const std::vector<WindowState>& list, const WindowState& window) {
        return std::ranges::find_if(list, [&](const WindowState& other) {
            return other.hwnd == window.hwnd && other.pid == window.pid;
        });
    };
    std::vector<WindowEvent> events;
    for (const auto& window : windows_) {
        if (find(snapshot, window) == snapshot.end()) events.push_back({WindowEvent::Kind::Closed, window});
    }
    for (const auto& window : snapshot) {
        if (find(windows_, window) == windows_.end()) events.push_back({WindowEvent::Kind::Opened, window});
    }
    for (const auto& window : snapshot) {
        const auto previous = find(windows_, window);
        if (previous != windows_.end() && *previous != window) events.push_back({WindowEvent::Kind::Changed, window});
    }
    windows_ = snapshot;
    return events;
}

struct WindowMonitor::Impl {
    using Clock = std::chrono::steady_clock;
    // Bursts (create, show, move) arrive within a few ms; batching them avoids reporting half-initialized windows.
    static constexpr auto batch_delay = std::chrono::milliseconds(2);
    // Caps snapshot work and window_changed traffic during drags to about one per 60 Hz frame.
    static constexpr auto min_interval = std::chrono::milliseconds(16);
    static constexpr DWORD poll_interval_ms = 250;

    Callback callback;
    DWORD thread_id{};
    std::jthread thread;
    HANDLE timer{};
    bool pending{};
    Clock::time_point last_snapshot{};
    std::vector<WindowState> previous;
    std::map<DWORD, std::string> image_names;
    std::map<HWND, WindowRect> restored_rects; // last rectangle seen while not minimized

    static thread_local Impl* current;

    static void CALLBACK on_event(HWINEVENTHOOK, DWORD event, HWND window, LONG object, LONG child, DWORD, DWORD) {
        if (!current || !window) return;
        const bool system_event = event < EVENT_OBJECT_CREATE;
        if (!system_event && (object != OBJID_WINDOW || child != CHILDID_SELF)) return;
        // Ignore child windows; destroyed windows can no longer be resolved and are accepted.
        if (IsWindow(window) && GetAncestor(window, GA_ROOT) != window) return;
        current->schedule();
    }

    void schedule() {
        if (pending || !timer) return;
        pending = true;
        const auto now = Clock::now();
        const auto due = std::max(now + batch_delay, last_snapshot + min_interval);
        const auto ticks = std::chrono::duration_cast<std::chrono::duration<long long, std::ratio<1, 10'000'000>>>(
                               due - now).count();
        LARGE_INTEGER relative{.QuadPart = -std::max(1LL, ticks)};
        if (!SetWaitableTimer(timer, &relative, 0, nullptr, nullptr, FALSE)) pending = false;
    }

    static WindowRect frame_rect(HWND window) {
        RECT rect{};
        if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect)))) {
            GetWindowRect(window, &rect);
        }
        return {rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top};
    }

    WindowRect minimized_rect(HWND window) const {
        if (const auto found = restored_rects.find(window); found != restored_rects.end()) return found->second;
        WINDOWPLACEMENT placement{.length = sizeof(WINDOWPLACEMENT)};
        if (!GetWindowPlacement(window, &placement)) return {};
        RECT normal = placement.rcNormalPosition;
        // Workspace coordinates are relative to the monitor's work area unless the window is a tool window.
        if (!(GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) {
            MONITORINFO info{.cbSize = sizeof(MONITORINFO)};
            if (GetMonitorInfoW(MonitorFromRect(&normal, MONITOR_DEFAULTTONEAREST), &info)) {
                OffsetRect(&normal, info.rcWork.left - info.rcMonitor.left, info.rcWork.top - info.rcMonitor.top);
            }
        }
        return {normal.left, normal.top, normal.right - normal.left, normal.bottom - normal.top};
    }

    struct Scan {
        Impl* self{};
        std::vector<WindowState> windows;
        std::map<DWORD, std::string> image_names;
        std::map<HWND, WindowRect> rects;
        DWORD own_pid{};
        std::exception_ptr error;
    };

    void add_window(Scan& scan, HWND window) const {
        if (!IsWindowVisible(window)) return;
        DWORD cloaked{};
        if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return;
        DWORD pid{};
        GetWindowThreadProcessId(window, &pid);
        if (pid == 0 || pid == scan.own_pid) return;

        WindowState state{.hwnd = window, .pid = pid, .owner = GetWindow(window, GW_OWNER)};
        state.show = IsIconic(window)   ? WindowShowState::Minimized
                     : IsZoomed(window) ? WindowShowState::Maximized
                                        : WindowShowState::Normal;
        state.rect = state.show == WindowShowState::Minimized ? minimized_rect(window) : frame_rect(window);
        if (state.rect.width <= 0 || state.rect.height <= 0) return;
        scan.rects[window] = state.rect;

        std::array<wchar_t, 256> class_name{};
        const int class_length = GetClassNameW(window, class_name.data(), static_cast<int>(class_name.size()));
        state.role = classify_window(static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)),
                                     static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE)),
                                     state.owner != nullptr,
                                     std::wstring_view(class_name.data(), std::max(class_length, 0)));

        auto image = scan.image_names.find(pid);
        if (image == scan.image_names.end()) {
            const auto cached = image_names.find(pid);
            image = scan.image_names.emplace(pid, cached != image_names.end() ? cached->second
                                                                              : process_image_name(pid)).first;
        }
        state.image_name = image->second;

        std::wstring title(static_cast<std::size_t>(GetWindowTextLengthW(window)) + 1, L'\0');
        title.resize(static_cast<std::size_t>(std::max(
            0, GetWindowTextW(window, title.data(), static_cast<int>(title.size())))));
        state.title = to_utf8(title);
        scan.windows.push_back(std::move(state));
    }

    void publish() {
        pending = false;
        last_snapshot = Clock::now();
        Scan scan{.self = this, .own_pid = GetCurrentProcessId()};
        const BOOL completed = EnumWindows(
            [](HWND window, LPARAM parameter) -> BOOL {
                auto& scan = *reinterpret_cast<Scan*>(parameter);
                try {
                    scan.self->add_window(scan, window);
                    return TRUE;
                } catch (...) {
                    scan.error = std::current_exception();
                    return FALSE;
                }
            },
            reinterpret_cast<LPARAM>(&scan));
        if (!completed) {
            if (scan.error) std::rethrow_exception(scan.error);
            spdlog::warn("Window enumeration failed (error {})", GetLastError());
            return;
        }
        // Process ids are reused, so cache image names only for processes that still own windows.
        image_names = std::move(scan.image_names);
        restored_rects = std::move(scan.rects);
        if (scan.windows == previous) return;
        previous = scan.windows;
        callback(std::move(scan.windows));
    }

    void run(std::promise<void>& started) {
        current = this;
        timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        MSG message{};
        PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE); // create the queue before announcing the thread id
        thread_id = GetCurrentThreadId();

        static constexpr std::array<std::pair<DWORD, DWORD>, 5> ranges{{
            {EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND},
            {EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND},
            {EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE},
            {EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_NAMECHANGE},
            {EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED},
        }};
        std::vector<HWINEVENTHOOK> hooks;
        for (const auto& [first, last] : ranges) {
            if (const auto hook = SetWinEventHook(first, last, nullptr, on_event, 0, 0,
                                                  WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS)) {
                hooks.push_back(hook);
            }
        }
        if (hooks.size() != ranges.size() || !timer) {
            spdlog::warn("Window tracking is degraded ({} of {} event hooks, timer {}); changes may be reported up "
                         "to {} ms late", hooks.size(), ranges.size(), timer ? "ok" : "unavailable", poll_interval_ms);
        }
        try {
            publish();
        } catch (const std::exception& error) {
            spdlog::error("Window snapshot failed: {}", error.what());
        }
        started.set_value();

        const DWORD handle_count = timer ? 1 : 0;
        for (bool running = true; running;) {
            const DWORD result =
                MsgWaitForMultipleObjectsEx(handle_count, &timer, poll_interval_ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            try {
                if (handle_count && result == WAIT_OBJECT_0) {
                    publish();
                } else if (result == WAIT_OBJECT_0 + handle_count) {
                    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                        if (message.message == WM_QUIT) {
                            running = false;
                            break;
                        }
                        DispatchMessageW(&message);
                    }
                } else if (result == WAIT_TIMEOUT) {
                    if (!pending) publish();
                } else {
                    spdlog::error("Window tracking stopped (error {})", GetLastError());
                    break;
                }
            } catch (const std::exception& error) {
                spdlog::error("Window snapshot failed: {}", error.what());
            }
        }
        for (const auto hook : hooks) UnhookWinEvent(hook);
        if (timer) CloseHandle(timer);
        current = nullptr;
    }
};

thread_local WindowMonitor::Impl* WindowMonitor::Impl::current = nullptr;

WindowMonitor::WindowMonitor(Callback callback) : impl_(std::make_unique<Impl>()) {
    impl_->callback = std::move(callback);
    std::promise<void> started;
    auto ready = started.get_future();
    impl_->thread = std::jthread([this, &started] { impl_->run(started); });
    ready.wait();
}

WindowMonitor::~WindowMonitor() {
    PostThreadMessageW(impl_->thread_id, WM_QUIT, 0, 0);
    impl_->thread.join();
}

} // namespace beamit::host
