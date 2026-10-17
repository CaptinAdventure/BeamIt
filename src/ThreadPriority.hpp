#pragma once

// Scheduling hints for latency-critical threads: MMCSS registration for the capture/encode and network threads, and
// a 1 ms system timer resolution while the host runs.
#include <windows.h>

#include <avrt.h>
#include <timeapi.h>

#include <cstdint>

namespace beamit {

// RAII guard that reverts a thread's MMCSS registration.
class MmcssTaskGuard {
    HANDLE _hTask = nullptr;

public:
    explicit MmcssTaskGuard(HANDLE hTask) : _hTask(hTask) {}
    ~MmcssTaskGuard() {
        if (_hTask != nullptr) {
            AvRevertMmThreadCharacteristics(_hTask);
        }
    }
    MmcssTaskGuard(const MmcssTaskGuard&) = delete;
    MmcssTaskGuard& operator=(const MmcssTaskGuard&) = delete;

    [[nodiscard]] bool registered() const noexcept { return _hTask != nullptr; }
};

// RAII guard for Windows timer resolution
class TimerResolutionGuard {
    UINT _period;
    bool _set = false;

public:
    explicit TimerResolutionGuard(UINT period_ms = 1) : _period(period_ms) {
        _set = (timeBeginPeriod(_period) == TIMERR_NOERROR);
    }
    ~TimerResolutionGuard() {
        if (_set) timeEndPeriod(_period);
    }
    TimerResolutionGuard(const TimerResolutionGuard&) = delete;
    TimerResolutionGuard& operator=(const TimerResolutionGuard&) = delete;

    [[nodiscard]] bool active() const noexcept { return _set; }
};

// Registers the calling thread with MMCSS ("Pro Audio", critical priority). Without MMCSS it falls back to a
// high-priority process class and a time-critical thread priority. Returns the MMCSS handle (or nullptr) for
// MmcssTaskGuard.
static inline HANDLE set_realtime_priority_with_handle([[maybe_unused]] std::uint64_t period_ns = 0) noexcept {
    DWORD taskIndex = 0;
    HANDLE hTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    if (hTask != nullptr) {
        AvSetMmThreadPriority(hTask, AVRT_PRIORITY_CRITICAL);
    } else {
        SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    }

    // TRUE disables dynamic priority boosting so the elevated priority stays constant.
    SetThreadPriorityBoost(GetCurrentThread(), TRUE);
    return hTask;
}

} // namespace beamit
