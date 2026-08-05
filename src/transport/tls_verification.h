#pragma once

#include <cstdint>
#include <string>

#include "openmoq/publisher/transport/publisher_transport.h"

namespace openmoq::publisher::transport::tlsverify {

// Resolves the root-certificate (CA bundle) file that should be used to verify
// the server certificate for a connection configured with `tls`.
//
// Behaviour:
//   * tls.insecure_skip_verify == true  -> success, `root_certificate_file` is
//     cleared. The caller must install the null verifier explicitly.
//   * tls.ca_path is non-empty          -> the file must exist and contain at
//     least one PEM "CERTIFICATE" block, otherwise a failure with an
//     actionable message is returned. This guards against picoquic's silent
//     fallback to the null verifier when the root store fails to load.
//   * otherwise                         -> $SSL_CERT_FILE (if set), then a list
//     of well-known system CA bundle locations is probed. If none is usable, a
//     failure explaining the available options (ca_path /
//     insecure_skip_verify / SSL_CERT_FILE) is returned.
//
// On success with verification enabled, `root_certificate_file` holds the path
// that must be passed to picoquic_create() as `cert_root_file_name`.
TransportStatus resolve_root_certificate_file(const TlsConfig& tls, std::string& root_certificate_file);

// Returns a human readable description of a QUIC transport error code. Codes
// in the CRYPTO_ERROR range 0x0100-0x01ff (RFC 9001, section 4.8) are decoded
// into the embedded TLS alert, e.g. 0x0128 -> "TLS alert 40 (handshake_failure)".
std::string describe_quic_error(std::uint64_t error_code);

// True when `error_code` is a QUIC CRYPTO_ERROR whose TLS alert is one that is
// typically raised by X.509 certificate verification (bad_certificate,
// unknown_ca, certificate_expired, ...). Used to append remediation hints to
// connection failure messages.
bool is_certificate_verification_error(std::uint64_t error_code);

}  // namespace openmoq::publisher::transport::tlsverify
