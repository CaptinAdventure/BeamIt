#include "AppUtil.hpp"

#include "ClientProtocol.hpp"

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

std::filesystem::path Settings::path() {
    PWSTR folder{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &folder))) return {};
    std::filesystem::path result(folder);
    CoTaskMemFree(folder);
    return result / L"BeamIt" / L"windows-client.json";
}

Settings Settings::load() {
    Settings settings;
    std::ifstream file(path(), std::ios::binary);
    if (!file) return settings;
    const std::string text{std::istreambuf_iterator<char>(file), {}};
    boost::system::error_code error;
    const auto value = json::parse(text, error);
    if (error || !value.is_object()) return settings;
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
