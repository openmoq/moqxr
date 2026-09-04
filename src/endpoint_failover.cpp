#include "openmoq/publisher/endpoint_failover.h"

#include <limits>
#include <utility>

namespace openmoq::publisher {

namespace {

void emit(const EndpointFailoverEventSink& sink, EndpointFailoverEvent event) {
    if (sink) {
        sink(event);
    }
}

}  // namespace

ReplayableLiveObjectSource::ReplayableLiveObjectSource(
    LiveObjectSource source, std::size_t max_buffered_objects)
    : source_(std::move(source)),
      max_buffered_objects_(max_buffered_objects == 0 ? 1 : max_buffered_objects) {}

void ReplayableLiveObjectSource::begin_attempt() {
    ++attempt_count_;
    replay_.clear();
    if (attempt_count_ == 1) {
        return;
    }

    if (retained_catalog_.has_value()) {
        replay_.push_back(*retained_catalog_);
    }

    auto replay_begin = retained_.begin();
    for (auto it = retained_.begin(); it != retained_.end(); ++it) {
        if (is_video_track(it->track_name) && it->object_id == 0) {
            replay_begin = it;
        }
    }
    for (auto it = replay_begin; it != retained_.end(); ++it) {
        replay_.push_back(*it);
    }
    retained_.clear();
}

LiveObjectSource ReplayableLiveObjectSource::source() {
    return LiveObjectSource{
        .tracks = source_.tracks,
        .next_object = [this]() { return next_object(); },
        .is_finished = [this]() {
            return replay_.empty() &&
                   (!source_.is_finished || source_.is_finished());
        },
        .catalog_mode = source_.catalog_mode,
    };
}

std::size_t ReplayableLiveObjectSource::buffered_objects() const {
    return retained_.size() + replay_.size();
}

std::optional<LiveObject> ReplayableLiveObjectSource::next_object() {
    if (!replay_.empty()) {
        LiveObject object = std::move(replay_.front());
        replay_.pop_front();
        if (object.track_name != "catalog") {
            retain(object);
        }
        return object;
    }

    std::optional<LiveObject> object = source_.next_object();
    if (!object.has_value()) {
        return std::nullopt;
    }
    if (object->track_name == "catalog") {
        retained_catalog_ = *object;
    } else {
        retain(*object);
    }
    return object;
}

void ReplayableLiveObjectSource::retain(LiveObject object) {
    retained_.push_back(std::move(object));
    while (retained_.size() > max_buffered_objects_) {
        retained_.pop_front();
    }
}

bool ReplayableLiveObjectSource::is_video_track(const std::string& track_name) const {
    for (const auto& track : source_.tracks) {
        if (track.track_name == track_name) {
            return track.media_type == LiveMediaType::kVideo;
        }
    }
    return false;
}

transport::TransportStatus publish_with_failover(
    std::span<const transport::EndpointConfig> endpoints,
    std::size_t retry_count,
    const EndpointPublishAttempt& publish_attempt,
    const EndpointRetryWait& retry_wait,
    const EndpointFailoverEventSink& event_sink) {
    if (endpoints.empty()) {
        return transport::TransportStatus::failure("no publish endpoints configured");
    }
    if (!publish_attempt) {
        return transport::TransportStatus::failure("no publish attempt callback configured");
    }
    if (retry_count == std::numeric_limits<std::size_t>::max()) {
        return transport::TransportStatus::failure("retry count is too large");
    }

    const std::size_t max_attempts = retry_count + 1;
    transport::TransportStatus last_status =
        transport::TransportStatus::failure("all publish endpoints failed",
                                            transport::FailureKind::kRetryable);

    for (std::size_t endpoint_index = 0; endpoint_index < endpoints.size(); ++endpoint_index) {
        for (std::size_t attempt = 1; attempt <= max_attempts; ++attempt) {
            emit(event_sink, {
                .kind = EndpointFailoverEventKind::kAttempt,
                .endpoint_index = endpoint_index,
                .attempt = attempt,
                .max_attempts = max_attempts,
                .status = transport::TransportStatus::success(),
            });
            last_status = publish_attempt(endpoints[endpoint_index]);
            if (last_status.ok) {
                return last_status;
            }
            if (last_status.failure_kind == transport::FailureKind::kCancelled ||
                last_status.failure_kind == transport::FailureKind::kFatal) {
                return last_status;
            }
            if (last_status.failure_kind == transport::FailureKind::kEndpointPermanent ||
                attempt == max_attempts) {
                break;
            }

            constexpr std::chrono::milliseconds kRetryDelay{1000};
            emit(event_sink, {
                .kind = EndpointFailoverEventKind::kRetry,
                .endpoint_index = endpoint_index,
                .attempt = attempt + 1,
                .max_attempts = max_attempts,
                .delay = kRetryDelay,
                .status = last_status,
            });
            if (retry_wait) {
                retry_wait(kRetryDelay);
            }
        }

        if (endpoint_index + 1 < endpoints.size()) {
            emit(event_sink, {
                .kind = EndpointFailoverEventKind::kAdvance,
                .endpoint_index = endpoint_index,
                .max_attempts = max_attempts,
                .status = last_status,
            });
        }
    }

    emit(event_sink, {
        .kind = EndpointFailoverEventKind::kExhausted,
        .endpoint_index = endpoints.size() - 1,
        .max_attempts = max_attempts,
        .status = last_status,
    });
    return last_status;
}

}  // namespace openmoq::publisher
