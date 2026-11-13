#pragma once

#include <boost/json.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace beamit::client {

// Host top-level window from window_opened/changed/closed (PROTOCOL.md §5.12).
struct HostWindow {
    struct Rect {
        double x{};
        double y{};
        double width{};
        double height{};
    };

    std::string hwnd;
    std::int64_t pid{};
    std::string image_name;
    std::string title;
    std::string role; // main, owned, popup, tool
    std::string owner;
    Rect rect;
    std::string state; // normal, minimized, maximized

    [[nodiscard]] std::string menu_title() const;
};

[[nodiscard]] std::optional<HostWindow> parse_host_window(const boost::json::value& value);

struct HostApplication {
    std::int64_t pid{};
    std::string title;
    std::string image_name;
    std::vector<HostWindow> windows; // Main windows first, then by title.
};

// Live set of host windows. Roles main/owned/tool are streamable; popups are transient.
class HostWindowList {
public:
    // Returns true when a listing-relevant field changed (not for pure moves/resizes).
    bool update(const HostWindow& window, bool closed);
    void clear() { windows_.clear(); }

    [[nodiscard]] const std::map<std::string, HostWindow, std::less<>>& windows() const { return windows_; }
    [[nodiscard]] const HostWindow* find(std::string_view hwnd) const;
    [[nodiscard]] std::vector<HostApplication> applications() const;

private:
    std::map<std::string, HostWindow, std::less<>> windows_;
};

[[nodiscard]] bool streamable_role(std::string_view role);

// Every whitespace-separated term must occur (case-insensitively) in the application title,
// image name, PID or one of its window titles.
[[nodiscard]] std::vector<HostApplication> filter_applications(const std::vector<HostApplication>& applications,
                                                                std::string_view search);
// Title of a window that matched the search but is not already in the application title.
[[nodiscard]] std::optional<std::string> matching_window_title(const HostApplication& application,
                                                               std::string_view search);

// Windows of followed processes that should get a stream now. Minimized windows wait until
// restored, because starting a stream would restore and activate them on the host.
[[nodiscard]] std::vector<HostWindow> windows_to_follow(const HostWindowList& list, const std::set<std::int64_t>& followed,
                                                        const std::set<std::string>& excluded);

} // namespace beamit::client
