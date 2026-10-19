#pragma once

#include "host/InputProtocol.hpp"

#include <memory>

namespace beamit::host::input {

// One connection per host instance. No password, network parsing, capture or codec code in the helper.
class ElevatedInput {
public:
    ElevatedInput();
    ~ElevatedInput();
    ElevatedInput(const ElevatedInput&) = delete;
    ElevatedInput& operator=(const ElevatedInput&) = delete;
    [[nodiscard]] bool healthy() const;
    [[nodiscard]] std::uint64_t create(HWND window);
    ipc::Response transact(const ipc::Request& request);
    void destroy(std::uint64_t stream) noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
    std::uint64_t next_stream_{};
};

// Invoked only by the separate helper executable.
int run_input_helper(int argc, wchar_t** argv);

} // namespace beamit::host::input
