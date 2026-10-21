#pragma once

#include "net/Crypto.hpp"

#include <deque>
#include <chrono>
#include <mutex>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace beamit::host {

namespace input { class ElevatedInput; }

struct HostStreamInfo {
    std::string id;
    std::string description;
    std::string statistics;
    std::chrono::steady_clock::time_point sampled_at{};
};

enum class InputElevation { Starting, NotElevated, Elevated, Unknown };

struct HostStatus {
    std::string client;
    std::vector<HostStreamInfo> streams;
    bool listening{};
    InputElevation input_elevation{InputElevation::Starting};
};

struct HostCommand {
    enum class Kind { Exit, ChangePassword, DisconnectStream, InstallElevatedInput };
    Kind kind{};
    std::string stream_id;
    net::Sha256 auth_key{};
    std::vector<std::byte> auth_salt;
    bool disconnect_client{};
    std::shared_ptr<input::ElevatedInput> elevated_input;
};

struct HostNotice {
    bool success{};
    std::string message;
};

// Only derived credentials cross from the UI to the network thread. No Win32 UI runs on the network thread.
class HostControl {
public:
    void submit(HostCommand command) {
        std::lock_guard lock(mutex_);
        commands_.push_back(std::move(command));
    }

    std::deque<HostCommand> take_commands() {
        std::lock_guard lock(mutex_);
        return std::exchange(commands_, {});
    }

    void publish(HostStatus status) {
        std::lock_guard lock(mutex_);
        status_ = std::move(status);
    }

    HostStatus status() const {
        std::lock_guard lock(mutex_);
        return status_;
    }

    void notify(HostNotice notice) {
        std::lock_guard lock(mutex_);
        notices_.push_back(std::move(notice));
    }

    std::deque<HostNotice> take_notices() {
        std::lock_guard lock(mutex_);
        return std::exchange(notices_, {});
    }

private:
    mutable std::mutex mutex_;
    std::deque<HostCommand> commands_;
    HostStatus status_;
    std::deque<HostNotice> notices_;
};

[[nodiscard]] inline std::string generate_connection_password() {
    return net::base64url_encode(net::random_bytes(27));
}

} // namespace beamit::host
