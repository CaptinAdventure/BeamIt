#pragma once

#include "host/HostServer.hpp"

#include <windows.h>

#include <optional>
#include <string>

namespace beamit::host {

// With an initial value, displays the generated startup password; otherwise starts with a blank field.
[[nodiscard]] std::optional<std::string> connection_password_dialog(HWND owner, std::string initial = {});

// Runs the host on a worker thread while the calling thread owns the tray and native dialogs.
void run_tray_host(HostServerOptions options);

} // namespace beamit::host
