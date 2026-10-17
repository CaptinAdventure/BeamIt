#include "beamit/capture/DxgiCapturer.hpp"
#include "capture/CaptureClock.hpp"

#include <chrono>
#include <iterator>
#include <utility>

namespace beamit::capture {
namespace {

Microsoft::WRL::ComPtr<IDXGIOutput> find_output(HMONITOR requested_monitor, HRESULT& result) {
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(result)) {
        return {};
    }

    HMONITOR monitor = requested_monitor;
    if (monitor == nullptr) {
        const HMONITOR primary = MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
        if (primary == nullptr) {
            result = E_FAIL;
            return {};
        }
        monitor = primary;
    }

    for (UINT adapter_index = 0;; ++adapter_index) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        result = factory->EnumAdapters1(adapter_index, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND) {
            result = DXGI_ERROR_NOT_FOUND;
            return {};
        }
        if (FAILED(result)) {
            return {};
        }

        for (UINT output_index = 0;; ++output_index) {
            Microsoft::WRL::ComPtr<IDXGIOutput> output;
            result = adapter->EnumOutputs(output_index, &output);
            if (result == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (FAILED(result)) {
                return {};
            }

            DXGI_OUTPUT_DESC description{};
            result = output->GetDesc(&description);
            if (FAILED(result)) {
                return {};
            }
            if (description.Monitor == monitor) {
                result = S_OK;
                return output;
            }
        }
    }
}

} // namespace

struct DxgiCapturer::FrameLease {
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication;
    bool held{true};
    HRESULT release() noexcept {
        if (!held) return S_OK;
        held = false;
        return duplication->ReleaseFrame();
    }
    ~FrameLease() { release(); }
};

DxgiCapturer::~DxgiCapturer() {
    stop();
}

bool DxgiCapturer::configure(const CaptureTarget& target, const CaptureOptions& options) {
    stop();
    if (!supports_target(target)) {
        set_error(E_INVALIDARG);
        configured_ = false;
        return false;
    }

    target_ = target;
    options_ = options;
    configured_ = true;
    set_error(S_OK);
    return true;
}

bool DxgiCapturer::start() {
    if (running_) {
        return true;
    }
    if (!configured_) {
        set_error(E_UNEXPECTED);
        return false;
    }

    if (!initialize_duplication()) {
        const HRESULT error = last_error_;
        stop();
        set_error(error);
        return false;
    }

    running_ = true;
    set_error(S_OK);
    return true;
}

std::optional<media::CapturedFrame> DxgiCapturer::try_acquire_frame() {
    if (!running_ || !duplication_) {
        set_error(E_UNEXPECTED);
        return std::nullopt;
    }

    if (frame_lease_) {
        if (frame_lease_.use_count() > 1) {
            // A borrowed surface cannot be released while a consumer still owns it.
            set_error(S_OK);
            return std::nullopt;
        }
        const HRESULT result = frame_lease_->release();
        frame_lease_.reset();
        if (FAILED(result)) {
            stop();
            set_error(result);
            return std::nullopt;
        }
    }

    auto lease = std::make_shared<FrameLease>();
    lease->duplication = duplication_;
    lease->held = false;
    DXGI_OUTDUPL_FRAME_INFO frame_info{};
    Microsoft::WRL::ComPtr<IDXGIResource> desktop_resource;
    const HRESULT acquire_result =
        duplication_->AcquireNextFrame(0, &frame_info, &desktop_resource);
    if (acquire_result == DXGI_ERROR_WAIT_TIMEOUT) {
        set_error(S_OK);
        return std::nullopt;
    }
    if (FAILED(acquire_result)) {
        stop();
        set_error(acquire_result);
        return std::nullopt;
    }
    lease->held = true;
    frame_lease_ = lease;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> acquired_texture;
    HRESULT result = desktop_resource.As(&acquired_texture);
    if (FAILED(result)) {
        stop();
        set_error(result);
        return std::nullopt;
    }

    set_error(S_OK);
    return media::CapturedFrame{
        .texture = std::move(acquired_texture),
        // Zero when only the pointer changed; that falls back to now.
        .captured_at = qpc_to_steady(frame_info.LastPresentTime.QuadPart),
        .lifetime = std::move(lease),
    };
}

void DxgiCapturer::stop() noexcept {
    running_ = false;
    if (frame_lease_ && frame_lease_.use_count() == 1) {
        const HRESULT result = frame_lease_->release();
        if (FAILED(result)) set_error(result);
    }
    frame_lease_.reset();
    duplication_.Reset();
    context_.Reset();
    device_.Reset();
}

bool DxgiCapturer::is_running() const noexcept {
    return running_;
}

HRESULT DxgiCapturer::last_error() const noexcept {
    return last_error_;
}

bool DxgiCapturer::initialize_duplication() {
    HRESULT result = S_OK;
    auto output = find_output(target_.monitor, result);
    if (FAILED(result) || !output) {
        set_error(result);
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
    result = output.As(&output1);
    if (FAILED(result)) {
        set_error(result);
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    result = output->GetParent(IID_PPV_ARGS(&adapter));
    if (FAILED(result)) {
        set_error(result);
        return false;
    }

    constexpr D3D_FEATURE_LEVEL feature_levels[]{
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D_FEATURE_LEVEL selected_feature_level{};
    result = D3D11CreateDevice(
        adapter.Get(),
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        feature_levels,
        static_cast<UINT>(std::size(feature_levels)),
        D3D11_SDK_VERSION,
        &device_,
        &selected_feature_level,
        &context_);
    if (FAILED(result)) {
        set_error(result);
        return false;
    }

    result = output1->DuplicateOutput(device_.Get(), &duplication_);
    if (FAILED(result)) {
        device_.Reset();
        context_.Reset();
        set_error(result);
        return false;
    }

    return true;
}

void DxgiCapturer::set_error(HRESULT error) noexcept {
    last_error_ = error;
}

} // namespace beamit::capture
