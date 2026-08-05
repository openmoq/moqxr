#include "tls_verification.h"

#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

namespace openmoq::publisher::transport::tlsverify {

namespace {

#ifndef _WIN32
// Well-known CA bundle locations, mirroring the probe lists used by curl,
// Go's crypto/x509, and OpenSSL distributions.
constexpr const char* kSystemCaBundlePaths[] = {
    "/etc/ssl/certs/ca-certificates.crt",                 // Debian/Ubuntu/Gentoo/Arch
    "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",  // Fedora/RHEL 7+
    "/etc/pki/tls/certs/ca-bundle.crt",                   // RHEL 6
    "/etc/ssl/ca-bundle.pem",                             // openSUSE
    "/etc/pki/tls/cacert.pem",                            // OpenELEC
    "/etc/ssl/cert.pem",                                  // Alpine/macOS/FreeBSD/OpenBSD
};
#else
constexpr const char* kSystemCaBundlePaths[] = {nullptr};
#endif

// Lightweight sanity check that `path` exists, is readable, and contains at
// least one PEM certificate block. picoquic loads the root file with
// X509_LOOKUP_load_file() and, if that load produces an empty store, silently
// downgrades the connection to the null verifier ("certificate will not be
// verified"). Rejecting obviously unusable files here converts that silent
// downgrade into a hard, actionable error.
bool file_contains_pem_certificate(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }
    std::string line;
    while (std::getline(file, line)) {
        if (line.find("-----BEGIN CERTIFICATE-----") != std::string::npos ||
            line.find("-----BEGIN TRUSTED CERTIFICATE-----") != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string to_hex(std::uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

const char* tls_alert_name(unsigned alert) {
    switch (alert) {
        case 0: return "close_notify";
        case 10: return "unexpected_message";
        case 20: return "bad_record_mac";
        case 40: return "handshake_failure";
        case 42: return "bad_certificate";
        case 43: return "unsupported_certificate";
        case 44: return "certificate_revoked";
        case 45: return "certificate_expired";
        case 46: return "certificate_unknown";
        case 47: return "illegal_parameter";
        case 48: return "unknown_ca";
        case 49: return "access_denied";
        case 50: return "decode_error";
        case 51: return "decrypt_error";
        case 70: return "protocol_version";
        case 71: return "insufficient_security";
        case 80: return "internal_error";
        case 109: return "missing_extension";
        case 110: return "unsupported_extension";
        case 112: return "unrecognized_name";
        case 113: return "bad_certificate_status_response";
        case 115: return "unknown_psk_identity";
        case 116: return "certificate_required";
        case 120: return "no_application_protocol";
        default: return nullptr;
    }
}

constexpr std::uint64_t kQuicCryptoErrorBase = 0x0100;  // RFC 9001, section 4.8
constexpr std::uint64_t kQuicCryptoErrorLast = 0x01ff;

}  // namespace

TransportStatus resolve_root_certificate_file(const TlsConfig& tls, std::string& root_certificate_file) {
    root_certificate_file.clear();

    if (tls.insecure_skip_verify) {
        return TransportStatus::success();
    }

    if (!tls.ca_path.empty()) {
        if (!file_contains_pem_certificate(tls.ca_path)) {
            return TransportStatus::failure(
                "TLS root certificate file \"" + tls.ca_path +
                "\" is missing, unreadable, or does not contain a PEM certificate; server "
                "certificate verification would silently be disabled. Provide a PEM CA bundle "
                "via ca_path (--ca), or set insecure_skip_verify (--insecure) to intentionally "
                "skip verification.");
        }
        root_certificate_file = tls.ca_path;
        return TransportStatus::success();
    }

    if (const char* env_bundle = std::getenv("SSL_CERT_FILE");
        env_bundle != nullptr && env_bundle[0] != '\0') {
        if (!file_contains_pem_certificate(env_bundle)) {
            return TransportStatus::failure(
                std::string("SSL_CERT_FILE points at \"") + env_bundle +
                "\" which is missing, unreadable, or does not contain a PEM certificate.");
        }
        root_certificate_file = env_bundle;
        return TransportStatus::success();
    }

    for (const char* candidate : kSystemCaBundlePaths) {
        if (candidate != nullptr && file_contains_pem_certificate(candidate)) {
            root_certificate_file = candidate;
            return TransportStatus::success();
        }
    }

    return TransportStatus::failure(
        "no CA bundle available for server certificate verification: set ca_path (--ca) to a "
        "PEM CA bundle, export SSL_CERT_FILE, or set insecure_skip_verify (--insecure) to "
        "intentionally skip verification.");
}

std::string describe_quic_error(std::uint64_t error_code) {
    if (error_code >= kQuicCryptoErrorBase && error_code <= kQuicCryptoErrorLast) {
        const unsigned alert = static_cast<unsigned>(error_code & 0xff);
        std::string text = "QUIC crypto error " + to_hex(error_code) + " (TLS alert " + std::to_string(alert);
        if (const char* name = tls_alert_name(alert); name != nullptr) {
            text += ": ";
            text += name;
        }
        text += ")";
        return text;
    }
    return "QUIC transport error " + to_hex(error_code);
}

bool is_certificate_verification_error(std::uint64_t error_code) {
    if (error_code < kQuicCryptoErrorBase || error_code > kQuicCryptoErrorLast) {
        return false;
    }
    switch (static_cast<unsigned>(error_code & 0xff)) {
        case 42:   // bad_certificate
        case 43:   // unsupported_certificate
        case 44:   // certificate_revoked
        case 45:   // certificate_expired
        case 46:   // certificate_unknown
        case 48:   // unknown_ca
        case 116:  // certificate_required
            return true;
        default:
            return false;
    }
}

}  // namespace openmoq::publisher::transport::tlsverify
