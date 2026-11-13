#include "HostWindows.hpp"

#include "ClientProtocol.hpp"

#include <windows.h>

#include <algorithm>
#include <ranges>

namespace beamit::client {
namespace {

std::wstring wide(std::string_view text) {
    if (text.empty()) return {};
    const auto size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(std::max(size, 0)), L'\0');
    if (size > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

bool contains(std::string_view haystack, std::string_view needle) {
    const auto text = wide(haystack);
    const auto term = wide(needle);
    if (term.empty()) return true;
    if (text.empty()) return false;
    return FindNLSStringEx(LOCALE_NAME_USER_DEFAULT, FIND_FROMSTART | LINGUISTIC_IGNORECASE | LINGUISTIC_IGNOREDIACRITIC,
                           text.data(), static_cast<int>(text.size()), term.data(), static_cast<int>(term.size()),
                           nullptr, nullptr, nullptr, 0) >= 0;
}

bool less_natural(std::string_view left, std::string_view right) {
    const auto a = wide(left);
    const auto b = wide(right);
    return CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS, a.data(),
                           static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), nullptr, nullptr, 0) ==
           CSTR_LESS_THAN;
}

std::vector<std::string> terms(std::string_view search) {
    std::vector<std::string> result;
    for (auto&& part : search | std::views::split(' ')) {
        std::string term(part.begin(), part.end());
        std::erase_if(term, [](char character) { return character == '\t' || character == '\r' || character == '\n'; });
        if (!term.empty()) result.push_back(std::move(term));
    }
    return result;
}

bool same_listing(const HostWindow& a, const HostWindow& b) {
    return a.pid == b.pid && a.role == b.role && a.state == b.state && a.title == b.title &&
           a.image_name == b.image_name && a.owner == b.owner;
}

} // namespace

std::string HostWindow::menu_title() const {
    auto text = title.empty() ? "Untitled " + role + " window" : title;
    if (state == "minimized") text += " (minimized)";
    return text;
}

std::optional<HostWindow> parse_host_window(const boost::json::value& value) {
    const auto* object = value.if_object();
    if (!object) return std::nullopt;
    const auto hwnd = string_field(*object, "hwnd");
    const auto pid = integer_field(*object, "pid");
    const auto role = string_field(*object, "role");
    const auto state = string_field(*object, "state");
    const auto* rect = object->if_contains("rect");
    if (!hwnd || hwnd->empty() || !pid || !role || !state || !rect || !rect->is_object()) return std::nullopt;
    HostWindow window;
    window.hwnd = *hwnd;
    window.pid = *pid;
    window.role = *role;
    window.state = *state;
    window.title = string_field(*object, "title").value_or("");
    window.image_name = string_field(*object, "image_name").value_or("");
    window.owner = string_field(*object, "owner").value_or("");
    const auto number = [&](std::string_view key) -> std::optional<double> {
        const auto* field = rect->get_object().if_contains(key);
        if (!field || !field->is_number()) return std::nullopt;
        return field->to_number<double>();
    };
    const auto x = number("x"), y = number("y"), width = number("width"), height = number("height");
    if (!x || !y || !width || !height) return std::nullopt;
    window.rect = {*x, *y, *width, *height};
    return window;
}

bool HostWindowList::update(const HostWindow& window, bool closed) {
    if (closed) return windows_.erase(window.hwnd) != 0;
    const auto found = windows_.find(window.hwnd);
    if (found == windows_.end()) {
        windows_.emplace(window.hwnd, window);
        return true;
    }
    const bool changed = !same_listing(found->second, window);
    found->second = window;
    return changed;
}

const HostWindow* HostWindowList::find(std::string_view hwnd) const {
    const auto found = windows_.find(hwnd);
    return found == windows_.end() ? nullptr : &found->second;
}

std::vector<HostApplication> HostWindowList::applications() const {
    std::map<std::int64_t, std::vector<HostWindow>> grouped;
    for (const auto& [hwnd, window] : windows_) {
        if (window.role != "popup") grouped[window.pid].push_back(window);
    }
    std::vector<HostApplication> result;
    for (auto& [pid, windows] : grouped) {
        std::ranges::sort(windows, [](const HostWindow& a, const HostWindow& b) {
            if ((a.role == "main") != (b.role == "main")) return a.role == "main";
            return less_natural(a.title, b.title);
        });
        HostApplication application{pid, {}, {}, std::move(windows)};
        for (const auto& window : application.windows) {
            if (application.image_name.empty()) application.image_name = window.image_name;
            if (application.title.empty()) application.title = window.title;
        }
        if (application.title.empty()) {
            application.title = application.image_name.empty() ? "Application" : application.image_name;
        }
        result.push_back(std::move(application));
    }
    std::ranges::sort(result, [](const HostApplication& a, const HostApplication& b) { return less_natural(a.title, b.title); });
    return result;
}

bool streamable_role(std::string_view role) {
    return role == "main" || role == "owned" || role == "tool";
}

std::vector<HostApplication> filter_applications(const std::vector<HostApplication>& applications,
                                                 std::string_view search) {
    const auto words = terms(search);
    if (words.empty()) return applications;
    std::vector<HostApplication> result;
    for (const auto& application : applications) {
        std::string haystack = application.title + "\n" + application.image_name + "\n" + std::to_string(application.pid);
        for (const auto& window : application.windows) haystack += "\n" + window.title;
        if (std::ranges::all_of(words, [&](const std::string& word) { return contains(haystack, word); })) {
            result.push_back(application);
        }
    }
    return result;
}

std::optional<std::string> matching_window_title(const HostApplication& application, std::string_view search) {
    const auto words = terms(search);
    if (words.empty() || std::ranges::all_of(words, [&](const std::string& word) { return contains(application.title, word); })) {
        return std::nullopt;
    }
    for (const auto& window : application.windows) {
        if (!window.title.empty() &&
            std::ranges::any_of(words, [&](const std::string& word) { return contains(window.title, word); })) {
            return window.title;
        }
    }
    return std::nullopt;
}

std::vector<HostWindow> windows_to_follow(const HostWindowList& list, const std::set<std::int64_t>& followed,
                                          const std::set<std::string>& excluded) {
    std::vector<HostWindow> result;
    for (const auto& [hwnd, window] : list.windows()) {
        if (followed.contains(window.pid) && streamable_role(window.role) && window.state != "minimized" &&
            !excluded.contains(hwnd)) {
            result.push_back(window);
        }
    }
    return result;
}

} // namespace beamit::client
