#include "AppUtil.hpp"

#include "AppResources.h"
#include "ClientProtocol.hpp"
#include "net/ClipboardBulk.hpp"

#include <shlobj.h>

#include <fstream>
#include <iterator>

namespace beamit::client {

std::wstring widen(std::string_view text) {
    if (text.empty()) return {};
    const auto size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(std::max(size, 0)), L'\0');
    if (size > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::string narrow(std::wstring_view text) {
    if (text.empty()) return {};
    const auto size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(std::max(size, 0)), '\0');
    if (size > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    }
    return result;
}

HICON app_icon(bool large) {
    return static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_BEAMIT), IMAGE_ICON,
                                         GetSystemMetrics(large ? SM_CXICON : SM_CXSMICON),
                                         GetSystemMetrics(large ? SM_CYICON : SM_CYSMICON), LR_SHARED));
}

void apply_window_icon(HWND hwnd) {
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(app_icon(false)));
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(app_icon(true)));
}

std::filesystem::path Settings::path() {
    PWSTR folder{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &folder))) return {};
    std::filesystem::path result(folder);
    CoTaskMemFree(folder);
    return result / L"BeamIt" / L"windows-client.json";
}

Settings Settings::load() {
    std::ifstream file(path(), std::ios::binary);
    if (!file) return {};
    const std::string text{std::istreambuf_iterator<char>(file), {}};
    return parse(text);
}

Settings Settings::parse(std::string_view text, std::function<void(std::string_view)> diagnostic) {
    Settings settings;
    if (!diagnostic) diagnostic = [](std::string_view message) {
        OutputDebugStringW((widen(message) + L"\n").c_str());
    };
    boost::system::error_code error;
    const auto value = json::parse(text, error);
    if (error || !value.is_object()) {
        diagnostic("BeamIt: invalid client settings; using documented defaults, including clipboard thresholds.");
        return settings;
    }
    const auto& object = value.get_object();
    settings.server_url = string_field(object, "server_url").value_or("");
    settings.fingerprint = string_field(object, "fingerprint").value_or("");
    const auto flag = [&](std::string_view key, bool fallback) {
        const auto* field = object.if_contains(key);
        return field && field->is_bool() ? field->get_bool() : fallback;
    };
    settings.trust_any_certificate = flag("trust_any_certificate", false);
    settings.clipboard_sync = flag("clipboard_sync", false);
    settings.anti_idle = flag("anti_idle", false);
    settings.follow_all_windows = flag("follow_all_windows", true);
    const auto threshold = [&](std::string_view key, std::uint64_t fallback, std::uint64_t maximum) {
        const auto value = integer_field(object, key);
        if (!value) {
            if (object.contains(key)) {
                diagnostic("BeamIt: invalid " + std::string(key) + "; using the documented default.");
            }
            return fallback;
        }
        if (*value < 0 || static_cast<std::uint64_t>(*value) > maximum) {
            diagnostic("BeamIt: invalid " + std::string(key) + "; using the documented default.");
            return fallback;
        }
        return static_cast<std::uint64_t>(*value);
    };
    settings.clipboard_bulk_byte_threshold = threshold(
        "clipboard_bulk_byte_threshold", net::clipboard_bulk::default_byte_threshold,
        net::clipboard_bulk::max_file_bytes);
    settings.clipboard_bulk_entry_threshold = static_cast<std::uint32_t>(threshold(
        "clipboard_bulk_entry_threshold", net::clipboard_bulk::default_root_threshold,
        net::clipboard_bulk::max_roots));
    return settings;
}

void Settings::save() const {
    const auto target = path();
    if (target.empty()) return;
    std::error_code ignored;
    std::filesystem::create_directories(target.parent_path(), ignored);
    const json::object object{
        {"server_url", server_url},
        {"fingerprint", fingerprint},
        {"trust_any_certificate", trust_any_certificate},
        {"clipboard_sync", clipboard_sync},
        {"anti_idle", anti_idle},
        {"follow_all_windows", follow_all_windows},
        {"clipboard_bulk_byte_threshold", clipboard_bulk_byte_threshold},
        {"clipboard_bulk_entry_threshold", clipboard_bulk_entry_threshold},
    };
    const auto temporary = target.wstring() + L".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) return;
        file << json::serialize(object);
    }
    MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING);
}

} // namespace beamit::client
