#include "ApplicationWindows.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>

std::vector<ApplicationWindow> application_windows() {
    struct Context {
        std::vector<ApplicationWindow> windows;
        std::exception_ptr error;
    } context;
    const auto callback = [](HWND hwnd, LPARAM parameter) -> BOOL {
        auto& state = *reinterpret_cast<Context*>(parameter);
        try {
            if (!IsWindowVisible(hwnd)) {
                return TRUE;
            }
            const int length = GetWindowTextLengthW(hwnd);
            if (length == 0) {
                return TRUE;
            }
            std::wstring title(static_cast<std::size_t>(length) + 1, L'\0');
            const int copied = GetWindowTextW(hwnd, title.data(), static_cast<int>(title.size()));
            if (copied == 0) {
                return TRUE; // The window may disappear during enumeration.
            }
            title.resize(copied);
            DWORD pid{};
            GetWindowThreadProcessId(hwnd, &pid);
            RECT rect{};
            GetWindowRect(hwnd, &rect);
            const auto width = std::max<LONG>(0, rect.right - rect.left);
            const auto height = std::max<LONG>(0, rect.bottom - rect.top);
            state.windows.push_back({
                hwnd, pid, std::move(title),
                GetWindow(hwnd, GW_OWNER) == nullptr &&
                    (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) == 0,
                static_cast<unsigned long long>(width) * height});
            return TRUE;
        } catch (...) {
            state.error = std::current_exception();
            return FALSE;
        }
    };
    if (!EnumWindows(callback, reinterpret_cast<LPARAM>(&context))) {
        if (context.error) {
            std::rethrow_exception(context.error);
        }
        throw std::runtime_error("Unable to enumerate application windows");
    }
    return context.windows;
}

HWND select_application_window(DWORD pid) {
    const auto windows = application_windows();
    const ApplicationWindow* selected = nullptr;
    for (const auto& window : windows) {
        if (window.pid == pid &&
            (selected == nullptr ||
             (window.primary_candidate && !selected->primary_candidate) ||
             (window.primary_candidate == selected->primary_candidate && window.area > selected->area))) {
            selected = &window;
        }
    }
    if (selected == nullptr) {
        throw std::runtime_error("No visible titled window found for the selected PID");
    }
    return selected->hwnd;
}
