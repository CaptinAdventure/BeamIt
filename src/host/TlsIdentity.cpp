#include "host/TlsIdentity.hpp"

#include <wintls.hpp>

#include <bcrypt.h>
#include <wincrypt.h>

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <array>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace beamit::host {
namespace {

constexpr wchar_t friendly_name[] = L"BeamIt self-signed TLS";
// wintls only accepts CryptoAPI private keys, so the key lives in a legacy RSA/AES CSP container.
constexpr wchar_t key_container[] = L"BeamIt-TLS-SelfSigned";
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

std::vector<BYTE> encode(LPCSTR type, const void* value) {
    DWORD size = 0;
    if (!CryptEncodeObjectEx(X509_ASN_ENCODING, type, value, 0, nullptr, nullptr, &size)) fail("CryptEncodeObjectEx");
    std::vector<BYTE> bytes(size);
    if (!CryptEncodeObjectEx(X509_ASN_ENCODING, type, value, 0, nullptr, bytes.data(), &size)) {
        fail("CryptEncodeObjectEx");
    }
    bytes.resize(size);
    return bytes;
}

wintls::cert_context_ptr create_self_signed(HCERTSTORE store) {
    HCRYPTPROV provider{};
    CryptAcquireContextW(&provider, key_container, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, CRYPT_DELETEKEYSET);
    if (!CryptAcquireContextW(&provider, key_container, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, CRYPT_NEWKEYSET)) {
        fail("Create TLS key container");
    }
    HCRYPTKEY key{};
    const bool generated = CryptGenKey(provider, AT_KEYEXCHANGE, (2048U << 16), &key);
    if (key) CryptDestroyKey(key);
    if (!generated) {
        CryptReleaseContext(provider, 0);
        fail("Generate RSA-2048 TLS key");
    }

    wchar_t host[256]{};
    DWORD host_size = static_cast<DWORD>(std::size(host));
    if (!GetComputerNameExW(ComputerNameDnsHostname, host, &host_size)) host[0] = L'\0';
    std::vector<CERT_ALT_NAME_ENTRY> names;
    const auto add_name = [&](wchar_t* dns_name) {
        CERT_ALT_NAME_ENTRY entry{};
        entry.dwAltNameChoice = CERT_ALT_NAME_DNS_NAME;
        entry.pwszDNSName = dns_name;
        names.push_back(entry);
    };
    add_name(const_cast<wchar_t*>(L"localhost"));
    if (host[0] != L'\0') add_name(host);
    CERT_ALT_NAME_INFO alt_names{static_cast<DWORD>(names.size()), names.data()};
    auto alt_names_encoded = encode(X509_ALTERNATE_NAME, &alt_names);
    LPSTR usages[] = {const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH)};
    CERT_ENHKEY_USAGE usage{1, usages};
    auto usage_encoded = encode(X509_ENHANCED_KEY_USAGE, &usage);
    std::array<CERT_EXTENSION, 2> extension_list{{
        {const_cast<LPSTR>(szOID_SUBJECT_ALT_NAME2), FALSE,
         {static_cast<DWORD>(alt_names_encoded.size()), alt_names_encoded.data()}},
        {const_cast<LPSTR>(szOID_ENHANCED_KEY_USAGE), FALSE,
         {static_cast<DWORD>(usage_encoded.size()), usage_encoded.data()}},
    }};
    CERT_EXTENSIONS extensions{static_cast<DWORD>(extension_list.size()), extension_list.data()};

    BYTE subject_bytes[256]{};
    DWORD subject_size = sizeof(subject_bytes);
    if (!CertStrToNameW(X509_ASN_ENCODING, L"CN=BeamIt host", CERT_X500_NAME_STR, nullptr, subject_bytes,
                        &subject_size, nullptr)) {
        CryptReleaseContext(provider, 0);
        fail("Encode certificate subject");
    }
    CERT_NAME_BLOB subject{subject_size, subject_bytes};
    CRYPT_KEY_PROV_INFO key_info{const_cast<wchar_t*>(key_container), const_cast<wchar_t*>(MS_ENH_RSA_AES_PROV_W),
                                 PROV_RSA_AES, 0, 0, nullptr, AT_KEYEXCHANGE};
    CRYPT_ALGORITHM_IDENTIFIER signature{const_cast<LPSTR>(szOID_RSA_SHA256RSA), {}};
    SYSTEMTIME start{};
    SYSTEMTIME end{};
    GetSystemTime(&start);
    FILETIME start_file{};
    SystemTimeToFileTime(&start, &start_file);
    ULARGE_INTEGER time{};
    time.LowPart = start_file.dwLowDateTime;
    time.HighPart = start_file.dwHighDateTime;
    time.QuadPart -= 3600ULL * 10'000'000;
    start_file.dwLowDateTime = time.LowPart;
    start_file.dwHighDateTime = time.HighPart;
    FileTimeToSystemTime(&start_file, &start);
    time.QuadPart += 2ULL * 365 * 24 * 3600 * 10'000'000;
    FILETIME end_file{time.LowPart, time.HighPart};
    FileTimeToSystemTime(&end_file, &end);

    const CERT_CONTEXT* created = CertCreateSelfSignCertificate(provider, &subject, 0, &key_info, &signature,
                                                                &start, &end, &extensions);
    CryptReleaseContext(provider, 0);
    if (!created) fail("CertCreateSelfSignCertificate");
    wintls::cert_context_ptr certificate{created};
    CRYPT_DATA_BLOB name{static_cast<DWORD>(sizeof(friendly_name)),
                         reinterpret_cast<BYTE*>(const_cast<wchar_t*>(friendly_name))};
    if (!CertSetCertificateContextProperty(certificate.get(), CERT_FRIENDLY_NAME_PROP_ID, 0, &name)) {
        fail("Set certificate friendly name");
    }
    const CERT_CONTEXT* stored{};
    if (!CertAddCertificateContextToStore(store, certificate.get(), CERT_STORE_ADD_REPLACE_EXISTING, &stored)) {
        fail("Store self-signed certificate");
    }
    return wintls::cert_context_ptr{stored};
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

TlsIdentity self_signed_identity() {
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
        // Expired or keyless; remove it and create a new one below.
        const CERT_CONTEXT* stale = CertDuplicateCertificateContext(candidate);
        CertDeleteCertificateFromStore(stale);
    }
    spdlog::info("Creating a new self-signed TLS certificate (CN=BeamIt host, RSA-2048, 2 years)");
    auto certificate = create_self_signed(store.get());
    TlsIdentity identity{.sha256_fingerprint = fingerprint(certificate.get()),
                         .description = "new persistent self-signed certificate (CurrentUser\\My)"};
    identity.certificate = std::move(certificate);
    return identity;
}

} // namespace beamit::host
