#pragma once

#include <wintls/certificate.hpp>

#include <filesystem>
#include <string>

namespace beamit::host {

struct TlsIdentity {
    wintls::cert_context_ptr certificate;
    std::string sha256_fingerprint;
    std::string description;
};

// Loads an operator-provided PEM certificate and RSA PKCS#8 PEM private key.
[[nodiscard]] TlsIdentity load_pem_identity(const std::filesystem::path& certificate,
                                            const std::filesystem::path& private_key);

// Reuses or creates a persistent self-signed certificate in CurrentUser\My.
[[nodiscard]] TlsIdentity self_signed_identity();

} // namespace beamit::host
