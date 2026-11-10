#pragma once

#include <cstdint>
#include <string>

#include "openmoq/publisher/transport/publisher_transport.h"
#include "tls_verification.h"

namespace openmoq::publisher::transport {

// Whether a failed connection to an endpoint can succeed on a later attempt.
// An unknown host name, a certificate that fails verification, and an
// application protocol that the server does not offer are configuration
// errors: retrying the same endpoint cannot fix them.

// host_not_found: the resolver answered that the name does not exist, which
// a temporary resolver failure does not.
inline FailureKind resolve_failure_kind(bool host_not_found) {
    return host_not_found ? FailureKind::kEndpointPermanent : FailureKind::kRetryable;
}

// The failure to resolve `host`, which picoquic reports without a reason: the
// resolver is asked again for it. `what` names the transport in the message.
TransportStatus resolve_failure_status(const std::string& what, const std::string& host);

// The QUIC errors of a connection that closed before its handshake ended.
// Only the local error marks a certificate failure permanent: a certificate
// alert from the peer means the server rejected the client's certificate,
// which another endpoint or a rotated client certificate can fix.
// certificate_expired also reports a certificate that is not yet valid, as
// on a host whose clock is not set yet: that can heal, so it is retryable.
inline FailureKind handshake_failure_kind(std::uint64_t local_error, std::uint64_t remote_error) {
    constexpr std::uint64_t kNoApplicationProtocol = 0x0100 + 120;   // TLS alert 120 (RFC 9001, section 4.8)
    constexpr std::uint64_t kCertificateExpired = 0x0100 + 45;
    if (local_error == kCertificateExpired) {
        return FailureKind::kRetryable;
    }
    if (tlsverify::is_certificate_verification_error(local_error) || remote_error == kNoApplicationProtocol) {
        return FailureKind::kEndpointPermanent;
    }
    return FailureKind::kRetryable;
}

}  // namespace openmoq::publisher::transport
