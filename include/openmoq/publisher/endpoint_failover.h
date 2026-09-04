#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <string>

#include "openmoq/publisher/live_object.h"
#include "openmoq/publisher/transport/publisher_transport.h"

namespace openmoq::publisher {

enum class EndpointFailoverEventKind {
    kAttempt,
    kRetry,
    kAdvance,
    kExhausted,
};

struct EndpointFailoverEvent {
    EndpointFailoverEventKind kind = EndpointFailoverEventKind::kAttempt;
    std::size_t endpoint_index = 0;
    std::size_t attempt = 0;
    std::size_t max_attempts = 0;
    std::chrono::milliseconds delay{0};
    transport::TransportStatus status;
};

using EndpointPublishAttempt =
    std::function<transport::TransportStatus(const transport::EndpointConfig&)>;
using EndpointRetryWait = std::function<void(std::chrono::milliseconds)>;
using EndpointFailoverEventSink = std::function<void(const EndpointFailoverEvent&)>;

class ReplayableLiveObjectSource {
public:
    explicit ReplayableLiveObjectSource(LiveObjectSource source,
                                        std::size_t max_buffered_objects);

    void begin_attempt();
    LiveObjectSource source();
    std::size_t buffered_objects() const;

private:
    std::optional<LiveObject> next_object();
    void retain(LiveObject object);
    bool is_video_track(const std::string& track_name) const;

    LiveObjectSource source_;
    std::size_t max_buffered_objects_ = 1;
    std::size_t attempt_count_ = 0;
    std::optional<LiveObject> retained_catalog_;
    std::deque<LiveObject> retained_;
    std::deque<LiveObject> replay_;
};

transport::TransportStatus publish_with_failover(
    std::span<const transport::EndpointConfig> endpoints,
    std::size_t retry_count,
    const EndpointPublishAttempt& publish_attempt,
    const EndpointRetryWait& retry_wait = {},
    const EndpointFailoverEventSink& event_sink = {});

}  // namespace openmoq::publisher
