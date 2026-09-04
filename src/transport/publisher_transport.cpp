#include "openmoq/publisher/transport/publisher_transport.h"

namespace openmoq::publisher::transport {

TransportStatus TransportStatus::success() {
    return {.ok = true, .message = {}, .failure_kind = FailureKind::kNone};
}

TransportStatus TransportStatus::failure(std::string_view error_message,
                                         FailureKind failure_kind) {
    return {
        .ok = false,
        .message = std::string(error_message),
        .failure_kind = failure_kind,
    };
}

}  // namespace openmoq::publisher::transport
