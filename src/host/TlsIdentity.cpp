#include "host/TlsIdentity.hpp"

#include <wintls.hpp>

#include <bcrypt.h>
#include <wincrypt.h>

#include <spdlog/fmt/fmt.h>

#include <array>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace beamit::host {
namespace {

constexpr wchar_t friendly_name[] = L"BeamIt self-signed TLS";
constexpr char imported_key_container[] = "BeamIt-TLS-Imported";

[[noreturn]] void fail(const char* operation) {
    throw std::runtime_error(fmt::format("{} failed (Win32 error {})", operation, GetLastError()));
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot open " + path.string());
    return {std::istreambuf_iterator<char>(file), {}};
}

std::string fingerprint(const CERT_CONTEXT* certificate) {
    std::array<unsigned char, 32> digest{};
    if (!BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, certificate->pbCertEncoded,
                                   certificate->cbCertEncoded, digest.data(), static_cast<ULONG>(digest.size())))) {
        throw std::runtime_error("Certificate fingerprint failed");
    }
    std::string text;
    for (const auto byte : digest) {
        if (!text.empty()) text += ':';
        text += fmt::format("{:02X}", byte);
    }
    return text;
}

struct StoreCloser {
    void operator()(HCERTSTORE store) const noexcept { CertCloseStore(store, 0); }
};
using Store = std::unique_ptr<void, StoreCloser>;

Store open_user_store() {
    Store store{CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                              CERT_SYSTEM_STORE_CURRENT_USER | CERT_STORE_OPEN_EXISTING_FLAG, L"MY")};
    if (!store) fail("Open CurrentUser\\My certificate store");
    return store;
}

bool usable(const CERT_CONTEXT* certificate) {
    FILETIME soon{};
    ULARGE_INTEGER now{};
    GetSystemTimeAsFileTime(&soon);
    if (CompareFileTime(&certificate->pCertInfo->NotBefore, &soon) > 0) return false;
    now.LowPart = soon.dwLowDateTime;
    now.HighPart = soon.dwHighDateTime;
    now.QuadPart += 7ULL * 24 * 3600 * 10'000'000;
    soon.dwLowDateTime = now.LowPart;
    soon.dwHighDateTime = now.HighPart;
    if (CompareFileTime(&certificate->pCertInfo->NotAfter, &soon) <= 0) return false;
    HCRYPTPROV_OR_NCRYPT_KEY_HANDLE key{};
    DWORD spec{};
    BOOL free_key{};
    if (!CryptAcquireCertificatePrivateKey(certificate, CRYPT_ACQUIRE_COMPARE_KEY_FLAG, nullptr, &key, &spec,
                                           &free_key)) {
        return false;
    }
    if (free_key) CryptReleaseContext(key, 0);
    return true;
}

} // namespace

TlsIdentity load_pem_identity(const std::filesystem::path& certificate_path,
                              const std::filesystem::path& private_key_path) {
    const auto certificate_pem = read_file(certificate_path);
    auto key_pem = read_file(private_key_path);
    auto certificate = wintls::x509_to_cert_context(boost::asio::buffer(certificate_pem), wintls::file_format::pem);
    wintls::error_code ignored;
    wintls::delete_private_key(imported_key_container, ignored);
    wintls::import_private_key(boost::asio::buffer(key_pem), wintls::file_format::pem, imported_key_container);
    SecureZeroMemory(key_pem.data(), key_pem.size());
    wintls::assign_private_key(certificate.get(), imported_key_container);
    TlsIdentity identity{.sha256_fingerprint = fingerprint(certificate.get()),
                         .description = "certificate " + certificate_path.string()};
    identity.certificate = std::move(certificate);
    return identity;
}

TlsIdentity installed_identity() {
    auto store = open_user_store();
    const CERT_CONTEXT* candidate = nullptr;
    while ((candidate = CertEnumCertificatesInStore(store.get(), candidate)) != nullptr) {
        std::array<wchar_t, std::size(friendly_name)> name{};
        DWORD size = static_cast<DWORD>(sizeof(name));
        if (!CertGetCertificateContextProperty(candidate, CERT_FRIENDLY_NAME_PROP_ID, name.data(), &size) ||
            std::wstring_view{name.data()} != friendly_name) {
            continue;
        }
        if (usable(candidate)) {
            wintls::cert_context_ptr certificate{CertDuplicateCertificateContext(candidate)};
            CertFreeCertificateContext(candidate);
            TlsIdentity identity{.sha256_fingerprint = fingerprint(certificate.get()),
                                 .description = "persistent self-signed certificate (CurrentUser\\My)"};
            identity.certificate = std::move(certificate);
            return identity;
        }
    }
    throw MissingTlsCertificate("No usable BeamIt TLS certificate was found in CurrentUser\\My.");
}

} // namespace beamit::host
