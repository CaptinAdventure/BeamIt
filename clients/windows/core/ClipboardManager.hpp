#pragma once

#include "ClientSession.hpp"
#include "host/ClipboardBulkStorage.hpp"

namespace beamit::client {

// All UI policy crosses through these callbacks; native clipboard ownership and
// filesystem/hash work stay on dedicated threads. Never log clipboard contents.
class ClipboardManager {
public:
    struct Context {
        bool enabled{}, active{};
        HWND foreground{};
        std::string stream, target;
        bool operator==(const Context&) const = default;
    };
    struct Status {
        std::string phase;
        std::uint64_t bytes{}, total{};
        bool busy{};
        std::string error;
    };
    struct Callbacks {
        std::function<void(std::uint32_t roots, std::uint64_t bytes, std::function<void(bool)>)> confirm;
        std::function<void(Status)> status;
    };
    struct Native {
        std::function<DWORD()> sequence{GetClipboardSequenceNumber};
        // Deadline clock seam; bandwidth pacing always uses the real monotonic clock.
        std::function<std::chrono::steady_clock::time_point()> clock{std::chrono::steady_clock::now};
        std::function<void(unsigned, bool, host::ClipboardMonitor::ReadCallback)> read;
        std::function<void(host::ClipboardWrite, std::function<void(host::ClipboardWriteResult)>)> write;
        std::filesystem::path staging_root; // Empty selects the shared per-user application root.
    };
    ClipboardManager(Session&, Session::Dispatcher, Callbacks, Native native = {});
    ~ClipboardManager();
    ClipboardManager(const ClipboardManager&) = delete;
    void start();
    void context(Context);
    void event(json::object);
    void local_changed(host::ClipboardChange);
    void upload(bool manual = true);
    void download(bool manual = true);
    void paste(std::function<void(bool)> completed);
    void cancel();
    void policy(net::clipboard_bulk::TransferPolicy);
private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace beamit::client
