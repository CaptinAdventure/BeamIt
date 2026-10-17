#include "beamit/media/AutoVideoEncoder.hpp"
#include "beamit/media/D3D12VideoEncoder.hpp"
#ifdef BEAMIT_HAS_NVENC
#include "beamit/media/NvencVideoEncoder.hpp"
#endif

#include <spdlog/spdlog.h>

#include <limits>

namespace beamit::media {

std::wstring_view AutoVideoEncoder::backend_name() const noexcept {
    return selected_ ? selected_->backend_name() : L"Automatic encoder selection";
}

bool AutoVideoEncoder::configure(const EncoderOptions& options) {
    stop();
    configured_ = false;
    if (!options.width || !options.height || (options.width & 1u) || (options.height & 1u) ||
        !options.frame_rate || !options.target_bitrate_kbps ||
        options.target_bitrate_kbps > std::numeric_limits<std::uint32_t>::max() / 1000 ||
        (options.codec != VideoCodec::H264 && options.codec != VideoCodec::H265)) {
        error_ = E_INVALIDARG;
        spdlog::error("Automatic encoder: invalid dimensions, codec, FPS or bitrate");
        return false;
    }
    options_ = options;
    configured_ = true;
    error_ = S_OK;
    return true;
}

bool AutoVideoEncoder::start() {
    if (!configured_ || FAILED(error_)) {
        error_ = E_UNEXPECTED;
        spdlog::error("Automatic encoder: start requires valid configuration");
        return false;
    }
    running_ = true;
    return true;
}

EncoderSubmitResult AutoVideoEncoder::try_submit(const CapturedFrame& frame) {
    if (!running_ || FAILED(last_error()) || !frame.texture) {
        error_ = E_UNEXPECTED;
        spdlog::error("Automatic encoder: submission requires a running encoder and a capture texture");
        return EncoderSubmitResult::Failed;
    }
    if (!selected_) {
        D3D11_TEXTURE2D_DESC desc{};
        frame.texture->GetDesc(&desc);
        if (desc.Width != options_.width || desc.Height != options_.height ||
            desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM || desc.SampleDesc.Count != 1 ||
            desc.MipLevels != 1 || desc.ArraySize != 1) {
            error_ = E_INVALIDARG;
            spdlog::error("Automatic encoder: expected matching single-sample BGRA8 capture texture");
            return EncoderSubmitResult::Failed;
        }
        if (options_.codec == VideoCodec::H264) {
            spdlog::info("Automatic H.264: D3D12 has no 4:4:4 profile; trying NVENC 4:4:4 first");
#ifdef BEAMIT_HAS_NVENC
            auto nvenc = std::make_unique<NvencVideoEncoder>();
            if (!nvenc->configure(options_) || !nvenc->start()) {
                error_ = nvenc->last_error();
                return EncoderSubmitResult::Failed;
            }
            if (nvenc->prepare(frame)) {
                selected_ = std::move(nvenc);
                spdlog::info("Automatic encoder selected NVENC H.264 4:4:4");
            } else {
                const auto reason = nvenc->last_error();
                nvenc->stop();
                if (nvenc->last_error() != reason) {
                    error_ = nvenc->last_error();
                    return EncoderSubmitResult::Failed;
                }
                if (reason != DXGI_ERROR_UNSUPPORTED && reason != E_NOINTERFACE &&
                    reason != HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND) &&
                    reason != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) &&
                    reason != HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND)) {
                    error_ = reason;
                    return EncoderSubmitResult::Failed;
                }
                spdlog::warn("NVENC 4:4:4 unavailable (HRESULT 0x{:08X}); trying D3D12 H.264 4:2:0",
                             static_cast<std::uint32_t>(reason));
            }
#else
            spdlog::warn("NVENC support not built; trying D3D12 H.264 4:2:0");
#endif
        } else {
            spdlog::info("Automatic HEVC: D3D12 4:4:4, then 4:2:2/4:2:0; NVENC HEVC is not implemented");
        }
        if (!selected_) {
            auto d3d12 = std::make_unique<D3D12VideoEncoder>();
            if (!d3d12->configure(options_) || !d3d12->start()) {
                error_ = d3d12->last_error();
                return EncoderSubmitResult::Failed;
            }
            selected_ = std::move(d3d12);
        }
    }
    return selected_->try_submit(frame);
}

void AutoVideoEncoder::request_key_frame() noexcept {
    // Before selection the first submitted picture is an IDR anyway.
    if (selected_) selected_->request_key_frame();
}

bool AutoVideoEncoder::try_update_bitrate(std::uint32_t bitrate_kbps) {
    if (!configured_ || !bitrate_kbps || bitrate_kbps > std::numeric_limits<std::uint32_t>::max() / 1000) {
        return false;
    }
    // Before selection, the backend chosen later is configured with the new bitrate.
    if (selected_ && !selected_->try_update_bitrate(bitrate_kbps)) return false;
    options_.target_bitrate_kbps = bitrate_kbps;
    return true;
}

EncoderSubmitResult AutoVideoEncoder::try_repeat_frame() {
    if (!running_ || !selected_ || FAILED(last_error())) {
        error_ = E_UNEXPECTED;
        spdlog::error("Automatic encoder: repeat requires a successfully submitted image");
        return EncoderSubmitResult::Failed;
    }
    return selected_->try_repeat_frame();
}

std::optional<EncodedPacket> AutoVideoEncoder::try_receive() {
    return selected_ ? selected_->try_receive() : std::nullopt;
}

void AutoVideoEncoder::stop() noexcept {
    if (selected_) {
        selected_->stop();
        if (FAILED(selected_->last_error())) error_ = selected_->last_error();
        selected_.reset();
    }
    running_ = false;
}

bool AutoVideoEncoder::is_running() const noexcept {
    return running_ && (!selected_ || selected_->is_running());
}

HRESULT AutoVideoEncoder::last_error() const noexcept {
    return FAILED(error_) || !selected_ ? error_ : selected_->last_error();
}

} // namespace beamit::media
