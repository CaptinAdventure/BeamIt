#pragma once

#include <Windows.h>

#include <chrono>
#include <cstdint>

namespace beamit::capture {

// Maps a QueryPerformanceCounter timestamp (as delivered by DXGI and Windows.Graphics.Capture) to steady_clock by
// measuring its age now, so the result does not depend on the steady_clock epoch. Implausible values (zero, in the
// future or older than one second) fall back to now.
inline std::chrono::steady_clock::time_point qpc_to_steady(std::int64_t qpc_ticks) noexcept {
    const auto now = std::chrono::steady_clock::now();
    LARGE_INTEGER counter{};
    LARGE_INTEGER frequency{};
    if (qpc_ticks <= 0 || !QueryPerformanceCounter(&counter) || !QueryPerformanceFrequency(&frequency) ||
        frequency.QuadPart <= 0) {
        return now;
    }
    const auto age_ticks = counter.QuadPart - qpc_ticks;
    if (age_ticks < 0 || age_ticks > frequency.QuadPart) return now;
    return now - std::chrono::nanoseconds(age_ticks * 1'000'000'000 / frequency.QuadPart);
}

// The same for QPC time already converted to 100 ns units (WinRT TimeSpan, e.g. Direct3D11CaptureFrame).
inline std::chrono::steady_clock::time_point qpc_100ns_to_steady(std::int64_t hundred_ns) noexcept {
    const auto now = std::chrono::steady_clock::now();
    LARGE_INTEGER counter{};
    LARGE_INTEGER frequency{};
    if (hundred_ns <= 0 || !QueryPerformanceCounter(&counter) || !QueryPerformanceFrequency(&frequency) ||
        frequency.QuadPart <= 0) {
        return now;
    }
    // Split to avoid overflow: whole seconds plus the remainder.
    const auto now_100ns = counter.QuadPart / frequency.QuadPart * 10'000'000 +
                           counter.QuadPart % frequency.QuadPart * 10'000'000 / frequency.QuadPart;
    const auto age = now_100ns - hundred_ns;
    if (age < 0 || age > 10'000'000) return now;
    return now - std::chrono::nanoseconds(age * 100);
}

} // namespace beamit::capture
