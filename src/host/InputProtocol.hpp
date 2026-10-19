#pragma once

#include "host/InputInjector.hpp"

#include <cmath>
#include <map>
#include <mutex>

namespace beamit::host::input::ipc {

inline constexpr std::uint32_t version = 1;
inline constexpr DWORD request_timeout_ms = 200;
inline constexpr std::size_t max_streams = 64;
enum class Operation : std::uint32_t {
    Ping, Create, Destroy, Absolute, Relative, Button, Wheel, Key, Text, Cursor, Modifiers, Release, Shutdown
};

// Private, local Windows ABI, deliberately independent of the network protocol. Fixed-sized messages only.
#pragma pack(push, 1)
struct Request {
    std::uint32_t revision{version};
    Operation operation{};
    std::uint64_t stream{};
    std::uint64_t window{};
    double x{};
    double y{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::int32_t first{};
    std::int32_t second{};
    std::uint32_t flags{};
    std::uint32_t length{};
    wchar_t text[max_text_units]{};
};
struct Response {
    std::uint32_t revision{version};
    std::uint32_t result{};
    std::int32_t x{};
    std::int32_t y{};
    std::uint32_t present{};
    std::uint32_t modifiers{};
    std::uint32_t diagnostic_present{};
    InjectionDiagnostic diagnostic{};
};
#pragma pack(pop)
static_assert(sizeof(Request) == 2112);
static_assert(sizeof(Response) == 64);

[[nodiscard]] bool valid(const Request& request);
[[nodiscard]] bool valid(const Response& response);

// Shared by the tiny helper and driver-independent lifecycle tests.
class Session {
public:
    Response dispatch(const Request& request);
    void cancel();
private:
    std::mutex mutex_;
    bool cancelled_{};
    std::map<std::uint64_t, std::shared_ptr<StreamInput>> streams_;
};

} // namespace beamit::host::input::ipc
