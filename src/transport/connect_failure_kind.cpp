#include "connect_failure_kind.h"

#ifdef OPENMOQ_HAS_PICOQUIC
#include <picosocks.h>
#endif

namespace openmoq::publisher::transport {

TransportStatus resolve_failure_status(const std::string& what, const std::string& host) {
    const std::string message = "failed to resolve " + what + " server address";
#ifdef OPENMOQ_HAS_PICOQUIC
    addrinfo hints{};
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    const int lookup = getaddrinfo(host.c_str(), nullptr, &hints, &result);
    if (result != nullptr) {
        freeaddrinfo(result);
    }
    if (lookup == 0) {
        // The resolver found the host, so picoquic rejected something else.
        return TransportStatus::failure(message, FailureKind::kRetryable);
    }
    return TransportStatus::failure(message + ": " + gai_strerror(lookup), resolve_failure_kind(lookup == EAI_NONAME));
#else
    static_cast<void>(host);
    return TransportStatus::failure(message, FailureKind::kRetryable);
#endif
}

}  // namespace openmoq::publisher::transport
