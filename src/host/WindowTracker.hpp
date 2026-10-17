#pragma once

#include <windows.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace beamit::host {

// How a client might treat a window when deciding whether to present it.
enum class WindowRole {
    Main,  // shown on the taskbar: unowned activatable windows and WS_EX_APPWINDOW windows (incl. pop-outs/meetings)
    Owned, // owned activatable windows such as dialogs
    Popup, // menus, tooltips, flyouts and other non-activatable or frameless transient windows
    Tool,  // captioned tool windows (palettes)
};

enum class WindowShowState { Normal, Minimized, Maximized };

// Virtual-screen physical pixels.
struct WindowRect {
    int x{};
    int y{};
    int width{};
    int height{};
    bool operator==(const WindowRect&) const = default;
};

// One visible, uncloaked, non-empty top-level window as observed by WindowMonitor.
struct WindowState {
    HWND hwnd{};
    DWORD pid{};
    std::string image_name; // executable file name (UTF-8), empty if the process could not be queried
    HWND owner{};
    std::string title; // UTF-8
    WindowRole role{};
    WindowRect rect;
    WindowShowState show{};
    bool operator==(const WindowState&) const = default;
};

[[nodiscard]] WindowRole classify_window(DWORD style, DWORD ex_style, bool owned, std::wstring_view class_name);
[[nodiscard]] std::string_view window_role_name(WindowRole role);
[[nodiscard]] std::string_view window_show_state_name(WindowShowState state);
// Executable file name of a process, e.g. "ms-teams.exe" (UTF-8); empty on failure.
[[nodiscard]] std::string process_image_name(DWORD pid);

struct WindowEvent {
    enum class Kind { Opened, Changed, Closed } kind{};
    WindowState window;
};

// Diffs successive snapshots into events, ordered closed, opened, changed. A handle reused by another process is
// reported as closed and opened.
class WindowList {
public:
    std::vector<WindowEvent> update(const std::vector<WindowState>& snapshot);
    [[nodiscard]] const std::vector<WindowState>& windows() const { return windows_; }

private:
    std::vector<WindowState> windows_;
};

// Watches top-level window creation, destruction, visibility, geometry and title changes through out-of-context
// WinEvent hooks on its own thread. Event bursts are coalesced into at most one snapshot per 16 ms; a slow periodic
// poll catches changes Windows does not report. Unchanged snapshots are not reported.
class WindowMonitor {
public:
    using Callback = std::function<void(std::vector<WindowState>)>;

    explicit WindowMonitor(Callback callback);
    ~WindowMonitor();
    WindowMonitor(const WindowMonitor&) = delete;
    WindowMonitor& operator=(const WindowMonitor&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace beamit::host
