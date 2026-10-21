#include "host/ElevatedInput.hpp"
#include "ThreadPriority.hpp"

#include <bcrypt.h>
#include <sddl.h>
#include <shellapi.h>
#include <objbase.h>
#include <spdlog/spdlog.h>

#include <array>
#include <filesystem>
#include <stdexcept>
#include <thread>
#include <vector>

namespace beamit::host::input {
namespace {

class Handle {
public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle() { if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    operator HANDLE() const { return value_; }
private:
    HANDLE value_;
};

class ShellApartment {
public:
    ShellApartment() : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {
        if (FAILED(result_) && result_ != RPC_E_CHANGED_MODE)
            throw std::runtime_error("Elevated input: cannot initialize COM for UAC launch");
    }
    ~ShellApartment() { if (SUCCEEDED(result_)) CoUninitialize(); }
private:
    HRESULT result_;
};

[[noreturn]] void fail(const char* what, DWORD error = ERROR_SUCCESS) {
    throw std::runtime_error(std::string("Elevated input: ") + what +
        (error == ERROR_SUCCESS ? "" : " (Win32 " + std::to_string(error) + ")"));
}

std::filesystem::path image_path(HANDLE process) {
    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &size)) fail("querying process image", GetLastError());
    path.resize(size);
    return path;
}

std::uint64_t creation_time(HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user))
        fail("querying process creation time", GetLastError());
    return (std::uint64_t{created.dwHighDateTime} << 32) | created.dwLowDateTime;
}

DWORD session_of(DWORD pid) {
    DWORD session{};
    if (!ProcessIdToSessionId(pid, &session) || session == 0) fail("interactive session required");
    return session;
}

bool elevated(HANDLE process) {
    Handle token([&] {
        HANDLE value{};
        if (!OpenProcessToken(process, TOKEN_QUERY, &value)) fail("opening process token", GetLastError());
        return value;
    }());
    TOKEN_ELEVATION elevation{};
    DWORD size{};
    return GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) &&
           elevation.TokenIsElevated != 0;
}

std::wstring pipe_name() {
    std::array<unsigned char, 24> random{};
    if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) fail("generating pipe name");
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring name = L"\\\\.\\pipe\\BeamIt.Input.";
    for (const auto byte : random) {
        name += hex[byte >> 4];
        name += hex[byte & 15];
    }
    return name;
}

std::wstring logon_sid() {
    HANDLE raw{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) fail("opening host token", GetLastError());
    Handle token(raw);
    DWORD size{};
    GetTokenInformation(token, TokenLogonSid, nullptr, 0, &size);
    if (!size) fail("querying logon SID");
    std::vector<std::byte> bytes(size);
    if (!GetTokenInformation(token, TokenLogonSid, bytes.data(), size, &size))
        fail("querying logon SID", GetLastError());
    auto* groups = reinterpret_cast<TOKEN_GROUPS*>(bytes.data());
    if (groups->GroupCount != 1) fail("missing logon SID");
    LPWSTR text{};
    if (!ConvertSidToStringSidW(groups->Groups[0].Sid, &text)) fail("formatting logon SID", GetLastError());
    std::wstring result(text);
    LocalFree(text);
    return result;
}

// Overlapped named-pipe operations are cancellable. No network-thread wait exceeds this RPC deadline.
void finish(HANDLE pipe, OVERLAPPED& operation, DWORD timeout, DWORD expected, bool pending) {
    DWORD transferred{};
    if (pending && WaitForSingleObject(operation.hEvent, timeout) != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &operation);
        GetOverlappedResult(pipe, &operation, &transferred, TRUE);
        fail("IPC deadline exceeded", ERROR_TIMEOUT);
    }
    if (!GetOverlappedResult(pipe, &operation, &transferred, FALSE))
        fail("invalid or disconnected pipe", GetLastError());
    if (transferred != expected) fail("incorrect IPC message size");
}

void transfer(HANDLE pipe, void* data, DWORD size, bool write, DWORD timeout) {
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) fail("creating IPC event", GetLastError());
    OVERLAPPED operation{};
    operation.hEvent = event;
    const bool done = write ? WriteFile(pipe, data, size, nullptr, &operation) != FALSE
                            : ReadFile(pipe, data, size, nullptr, &operation) != FALSE;
    if (!done && GetLastError() != ERROR_IO_PENDING) fail("pipe read/write", GetLastError());
    finish(pipe, operation, timeout, size, !done);
}

} // namespace

struct ElevatedInput::State {
    HANDLE pipe{INVALID_HANDLE_VALUE};
    HANDLE process{};
    bool failed{};
    ~State() {
        // Closing the only server handle also wakes the helper's independent disconnect watchdog.
        if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
        if (process) {
            if (WaitForSingleObject(process, 1500) == WAIT_TIMEOUT)
                spdlog::error("Elevated input helper did not finish shutdown; it remains bound to host lifetime");
            CloseHandle(process);
        }
    }
};

ElevatedInput::ElevatedInput() : state_(std::make_unique<State>()) {
    const auto session = session_of(GetCurrentProcessId());
    const auto name = pipe_name();
    // No Everyone/Authenticated Users and no FILE_CREATE_PIPE_INSTANCE for the logon SID. Administrators
    // permit over-the-shoulder UAC; exact kernel-reported peer PID is still mandatory on both ends.
    const auto sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x12019b;;;" + logon_sid() + L")";
    PSECURITY_DESCRIPTOR descriptor{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
        fail("creating pipe ACL", GetLastError());
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    state_->pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
        FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
        PIPE_REJECT_REMOTE_CLIENTS, 1, sizeof(ipc::Response), sizeof(ipc::Request), 0, &security);
    const auto pipe_error = GetLastError();
    LocalFree(descriptor);
    if (state_->pipe == INVALID_HANDLE_VALUE) fail("creating isolated input pipe", pipe_error);

    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) fail("creating connection event", GetLastError());
    OVERLAPPED connection{};
    connection.hEvent = event;
    if (ConnectNamedPipe(state_->pipe, &connection) || GetLastError() != ERROR_IO_PENDING)
        fail("starting input pipe connection", GetLastError());

    try {
        const auto helper = image_path(GetCurrentProcess()).parent_path() / L"BeamItInputHelper.exe";
        if (!std::filesystem::is_regular_file(helper)) fail("BeamItInputHelper.exe was not found beside BeamIt.exe");
        const auto arguments = L"\"" + name + L"\" " + std::to_wstring(GetCurrentProcessId()) + L" " +
                               std::to_wstring(creation_time(GetCurrentProcess()));
        SHELLEXECUTEINFOW launch{sizeof(launch)};
        launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        launch.lpVerb = L"runas";
        launch.lpFile = helper.c_str();
        launch.lpParameters = arguments.c_str();
        launch.nShow = SW_HIDE;
        spdlog::info("Elevated input requested: approving UAC starts only BeamItInputHelper.exe");
        const ShellApartment apartment;
        if (!ShellExecuteExW(&launch)) fail("UAC denied or helper launch failed", GetLastError());
        if (!launch.hProcess) fail("helper launch returned no process handle");
        state_->process = launch.hProcess;
        finish(state_->pipe, connection, 15000, 0, true);
        ULONG peer{};
        const DWORD pid = GetProcessId(state_->process);
        if (!GetNamedPipeClientProcessId(state_->pipe, &peer) || peer != pid ||
            session_of(pid) != session || !elevated(state_->process) ||
            !std::filesystem::equivalent(image_path(state_->process), helper))
            fail("helper identity/session/elevation verification failed");
        transact(ipc::Request{});
        const auto integrity = process_integrity(pid);
        const auto host_integrity = process_integrity(GetCurrentProcessId());
        spdlog::info("Elevated input helper ready: pid {}, integrity RID {:#x} (query error {}); "
                     "host PID {}, integrity RID {:#x} (query error {}), not elevated by this switch",
                     pid, integrity.level, integrity.error, GetCurrentProcessId(),
                     host_integrity.level, host_integrity.error);
    } catch (...) {
        CancelIoEx(state_->pipe, &connection);
        DWORD ignored{};
        GetOverlappedResult(state_->pipe, &connection, &ignored, TRUE);
        throw;
    }
}

ElevatedInput::~ElevatedInput() {
    if (!healthy()) return;
    try {
        ipc::Request request;
        request.operation = ipc::Operation::Shutdown;
        transact(request);
    } catch (const std::exception& error) {
        spdlog::warn("Elevated input channel closed during host shutdown: {}", error.what());
    }
}

bool ElevatedInput::healthy() const {
    return !state_->failed && state_->process &&
           WaitForSingleObject(state_->process, 0) == WAIT_TIMEOUT;
}

ipc::Response ElevatedInput::transact(const ipc::Request& request) {
    try {
        if (!healthy() || !ipc::valid(request)) fail("helper unavailable or invalid request");
        // One total 200 ms budget, rather than a separate timeout for each half of the exchange.
        const auto started = GetTickCount64();
        transfer(state_->pipe, const_cast<ipc::Request*>(&request), sizeof(request), true, ipc::request_timeout_ms);
        const auto elapsed = GetTickCount64() - started;
        if (elapsed >= ipc::request_timeout_ms) fail("input write deadline exceeded");
        ipc::Response response;
        transfer(state_->pipe, &response, sizeof(response), false,
                 ipc::request_timeout_ms - static_cast<DWORD>(elapsed));
        if (!ipc::valid(response)) fail("invalid helper response");
        if (response.diagnostic_present) log_injection_diagnostic(response.diagnostic);
        return response;
    } catch (...) {
        state_->failed = true;
        // Revoke the channel immediately, even if a foreground API is hung in the helper.
        if (state_->pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(state_->pipe);
            state_->pipe = INVALID_HANDLE_VALUE;
        }
        throw;
    }
}

std::uint64_t ElevatedInput::create(HWND window) {
    ipc::Request request;
    request.operation = ipc::Operation::Create;
    request.stream = ++next_stream_;
    request.window = reinterpret_cast<std::uintptr_t>(window);
    transact(request);
    return request.stream;
}

void ElevatedInput::destroy(std::uint64_t stream) noexcept {
    if (!healthy()) return;
    try {
        ipc::Request request;
        request.operation = ipc::Operation::Destroy;
        request.stream = stream;
        transact(request);
    } catch (const std::exception& error) {
        spdlog::error("{}", error.what());
    }
}

int run_input_helper(int argc, wchar_t** argv) {
    try {
        if (argc != 4 || !elevated(GetCurrentProcess())) fail("helper requires UAC and a host binding");
        const std::wstring name(argv[1]);
        if (!name.starts_with(L"\\\\.\\pipe\\BeamIt.Input.") || name.size() != 70 ||
            name.find_first_not_of(L"0123456789abcdef", 22) != std::wstring::npos)
            fail("invalid pipe name");
        std::size_t used{};
        const auto host_id = std::stoull(argv[2], &used);
        if (used != std::wcslen(argv[2]) || host_id == 0 || host_id > MAXDWORD) fail("invalid host PID");
        const auto host_created = std::stoull(argv[3], &used);
        if (used != std::wcslen(argv[3])) fail("invalid host creation time");
        Handle host(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                static_cast<DWORD>(host_id)));
        const auto host_path = image_path(GetCurrentProcess()).parent_path() / L"BeamIt.exe";
        if (!host || session_of(static_cast<DWORD>(host_id)) != session_of(GetCurrentProcessId()) ||
            creation_time(host) != host_created ||
            !std::filesystem::equivalent(image_path(host), host_path) ||
            WaitForSingleObject(host, 0) != WAIT_TIMEOUT) fail("host identity/session verification failed");
        Handle pipe(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
        ULONG server{};
        if (pipe == INVALID_HANDLE_VALUE || !GetNamedPipeServerProcessId(pipe, &server) || server != host_id)
            fail("pipe server does not match expected host");
        DWORD mode = PIPE_READMODE_MESSAGE;
        if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) fail("setting pipe message mode");
        if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
            fail("setting helper DPI awareness", GetLastError());

        const TimerResolutionGuard timer_resolution;
        const MmcssTaskGuard mmcss(set_realtime_priority_with_handle());
        if (!mmcss.registered()) spdlog::warn("Input helper MMCSS registration failed; using priority fallback");

        ipc::Session session;
        Handle shutdown(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!shutdown) fail("creating shutdown event");
        // Independent of input/focus execution: a hung foreground window cannot retain injected keys
        // after the host closes its channel or exits. No IPC operation is performed on this thread.
        std::jthread watchdog([&] {
            HANDLE waits[]{host, shutdown};
            for (;;) {
                const auto status = WaitForMultipleObjects(2, waits, FALSE, 20);
                if (status == WAIT_OBJECT_0 + 1) return;
                DWORD available{};
                if (status != WAIT_OBJECT_0 && PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) continue;
                session.cancel();
                ExitProcess(0);
            }
        });
        try {
            for (;;) {
                ipc::Request request;
                transfer(pipe, &request, sizeof(request), false, INFINITE);
                auto response = session.dispatch(request);
                transfer(pipe, &response, sizeof(response), true, ipc::request_timeout_ms);
                if (request.operation == ipc::Operation::Shutdown) break;
            }
            SetEvent(shutdown);
        } catch (...) {
            session.cancel();
            SetEvent(shutdown);
            throw;
        }
        return 0;
    } catch (const std::exception& error) {
        spdlog::error("{}", error.what());
        return 1;
    }
}

} // namespace beamit::host::input
