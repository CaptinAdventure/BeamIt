#pragma once

#include <wintls/certificate.hpp>

#include <filesystem>
#include <string>
#include <stdexcept>

namespace beamit::host {

struct TlsIdentity {
    wintls::cert_context_ptr certificate;
    std::string sha256_fingerprint;
    std::string description;
};

// Loads an operator-provided PEM certificate and RSA PKCS#8 PEM private key.
[[nodiscard]] TlsIdentity load_pem_identity(const std::filesystem::path& certificate,
                                            const std::filesystem::path& private_key);

class MissingTlsCertificate : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Loads the persistent certificate installed by Install-BeamItCert.ps1 in CurrentUser\My.
[[nodiscard]] TlsIdentity installed_identity();

} // namespace beamit::host
