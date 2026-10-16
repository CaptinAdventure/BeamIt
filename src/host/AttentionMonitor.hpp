#pragma once

#include <windows.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace beamit::host {

// A window started (`requested`) or stopped asking for attention by flashing its taskbar button.
struct AttentionEvent {
    HWND window{};
    DWORD pid{};
    bool requested{};
};

// Pure state machine that turns shell-hook notifications into set/clear transitions.
//
// The shell hook reports flashing as HSHELL_FLASH followed by irregular HSHELL_REDRAW messages that do not track the
// flash timer, and FLASHW_STOP produces only an HSHELL_REDRAW, indistinguishable from title/icon redraws. Measured on
// Windows 11: with FLASHW_TIMERNOFG one redraw arrived 2.6 s in and then nothing for 9 s while still flashing. So a
// window counts as requesting attention from its first flash until it is activated or destroyed; redraws are ignored.
class AttentionTracker {
public:
    std::optional<AttentionEvent> flashed(HWND window, DWORD pid, bool is_foreground);
    std::optional<AttentionEvent> activated(HWND window);
    std::optional<AttentionEvent> destroyed(HWND window);
    [[nodiscard]] std::vector<AttentionEvent> active() const;

private:
    std::optional<AttentionEvent> clear(HWND window);

    std::map<HWND, DWORD> windows_;
};

// Watches the interactive desktop's shell-hook notifications on its own thread (a hidden top-level window registered
// with RegisterShellHookWindow) and reports attention transitions through `callback` on that thread.
class AttentionMonitor {
public:
    using Callback = std::function<void(const AttentionEvent&)>;

    explicit AttentionMonitor(Callback callback);
    ~AttentionMonitor();
    AttentionMonitor(const AttentionMonitor&) = delete;
    AttentionMonitor& operator=(const AttentionMonitor&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace beamit::host
