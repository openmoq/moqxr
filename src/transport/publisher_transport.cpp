#include "openmoq/publisher/transport/publisher_transport.h"

namespace openmoq::publisher::transport {

TransportStatus TransportStatus::success() {
    return {.ok = true, .message = {}, .failure_kind = FailureKind::kNone,
            .migration_uri = std::nullopt, .migration_timeout_ms = std::nullopt};
}

TransportStatus TransportStatus::failure(std::string_view error_message,
                                         FailureKind failure_kind) {
    return {
        .ok = false,
        .message = std::string(error_message),
        .failure_kind = failure_kind,
        .migration_uri = std::nullopt,
        .migration_timeout_ms = std::nullopt,
    };
}

}  // namespace openmoq::publisher::transport
