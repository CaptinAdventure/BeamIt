#pragma once

#include <windows.h>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <stdexcept>

inline void check_hresult(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        throw std::runtime_error(
            fmt::format("{} failed (HRESULT 0x{:08X})", operation, static_cast<std::uint32_t>(result)));
    }
}

class FrameTimer {
public:
    using Clock = std::chrono::steady_clock;

    FrameTimer() {
        // Sleep-based polling can round a 1 ms GPU wait up to a full scheduler tick.
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                       TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (!timer_) {
            check_hresult(HRESULT_FROM_WIN32(GetLastError()), "Create high-resolution frame timer");
        }
    }
    ~FrameTimer() { CloseHandle(timer_); }
    FrameTimer(const FrameTimer&) = delete;
    FrameTimer& operator=(const FrameTimer&) = delete;

    void wait_until(Clock::time_point deadline) {
        const auto remaining = deadline - Clock::now();
        if (remaining <= Clock::duration::zero()) return;
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<LONGLONG>(
            1, std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count() / 100);
        if (!SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0)) {
            check_hresult(HRESULT_FROM_WIN32(GetLastError()), "Set frame timer");
        }
        const DWORD result = WaitForSingleObject(timer_, INFINITE);
        if (result != WAIT_OBJECT_0) {
            if (result == WAIT_FAILED) {
                check_hresult(HRESULT_FROM_WIN32(GetLastError()), "Wait for frame timer");
            }
            throw std::runtime_error("Unexpected frame timer wait result");
        }
    }

private:
    HANDLE timer_{};
};
