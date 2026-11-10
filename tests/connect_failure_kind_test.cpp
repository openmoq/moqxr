#include "connect_failure_kind.h"

#include <cstdint>
#include <iostream>
#include <string>

namespace {

using openmoq::publisher::transport::FailureKind;
using openmoq::publisher::transport::handshake_failure_kind;
using openmoq::publisher::transport::resolve_failure_kind;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

// QUIC CRYPTO_ERROR for a TLS alert (RFC 9001, section 4.8).
constexpr std::uint64_t alert(std::uint64_t code) { return 0x0100 + code; }

}  // namespace

int main() {
    bool ok = true;
    // A configuration error: retrying the same endpoint cannot fix it.
    ok &= expect(resolve_failure_kind(true) == FailureKind::kEndpointPermanent,
                 "an unknown host name (NXDOMAIN) is permanent");
    ok &= expect(handshake_failure_kind(alert(48), 0) == FailureKind::kEndpointPermanent,
                 "a server certificate from an unknown CA is permanent");
    ok &= expect(handshake_failure_kind(alert(46), 0) == FailureKind::kEndpointPermanent,
                 "a server certificate that fails verification is permanent");
    ok &= expect(handshake_failure_kind(0, alert(120)) == FailureKind::kEndpointPermanent,
                 "a server that offers none of the ALPNs (no_application_protocol) is permanent");
    // A transient failure: the next attempt can succeed.
    ok &= expect(resolve_failure_kind(false) == FailureKind::kRetryable, "a temporary DNS failure is retryable");
    ok &= expect(handshake_failure_kind(0, 0) == FailureKind::kRetryable, "a handshake without an error is retryable");
    ok &= expect(handshake_failure_kind(0x0a, 0) == FailureKind::kRetryable, "a QUIC protocol error is retryable");
    ok &= expect(handshake_failure_kind(alert(45), 0) == FailureKind::kRetryable,
                 "an expired or not yet valid certificate is retryable: the clock may not be set yet");
    ok &= expect(handshake_failure_kind(0, alert(48)) == FailureKind::kRetryable,
                 "a server rejecting the client certificate is not a server configuration error");
    ok &= expect(handshake_failure_kind(0, alert(40)) == FailureKind::kRetryable,
                 "another TLS alert from the server is retryable");
    if (ok) {
        std::cout << "connect failure kind tests passed\n";
    }
    return ok ? 0 : 1;
}
