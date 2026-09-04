#include "openmoq/publisher/endpoint_failover.h"

#include <chrono>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

using openmoq::publisher::EndpointFailoverEvent;
using openmoq::publisher::EndpointFailoverEventKind;
using openmoq::publisher::LiveCatalogMode;
using openmoq::publisher::LiveMediaType;
using openmoq::publisher::LiveObject;
using openmoq::publisher::LiveObjectSource;
using openmoq::publisher::LiveTrack;
using openmoq::publisher::ReplayableLiveObjectSource;
using openmoq::publisher::publish_with_failover;
using openmoq::publisher::transport::EndpointConfig;
using openmoq::publisher::transport::FailureKind;
using openmoq::publisher::transport::TransportStatus;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

EndpointConfig endpoint(std::string host) {
    EndpointConfig result;
    result.host = std::move(host);
    result.port = 4433;
    return result;
}

}  // namespace

int main() {
    bool ok = true;

    {
        const std::vector<EndpointConfig> endpoints{endpoint("primary"), endpoint("backup")};
        std::vector<std::string> attempts;
        std::vector<std::chrono::milliseconds> waits;
        std::vector<EndpointFailoverEvent> events;
        const TransportStatus status = publish_with_failover(
            endpoints, 2,
            [&attempts](const EndpointConfig& current) {
                attempts.push_back(current.host);
                if (attempts.size() < 3) {
                    return TransportStatus::failure("closed", FailureKind::kRetryable);
                }
                return TransportStatus::success();
            },
            [&waits](std::chrono::milliseconds delay) { waits.push_back(delay); },
            [&events](const EndpointFailoverEvent& event) { events.push_back(event); });

        ok &= expect(status.ok, "expected a successful retry to finish publication");
        ok &= expect(attempts == std::vector<std::string>({"primary", "primary", "primary"}),
                     "expected retries to remain on the current endpoint");
        ok &= expect(waits == std::vector<std::chrono::milliseconds>(
                                  {std::chrono::seconds(1), std::chrono::seconds(1)}),
                     "expected a one-second pause before each retry");
        ok &= expect(events.size() == 5 &&
                         events[0].kind == EndpointFailoverEventKind::kAttempt &&
                         events[1].kind == EndpointFailoverEventKind::kRetry,
                     "expected attempt and retry events to be observable");
    }

    {
        const std::vector<EndpointConfig> endpoints{endpoint("primary"), endpoint("backup")};
        std::vector<std::string> attempts;
        const TransportStatus status = publish_with_failover(
            endpoints, 1,
            [&attempts](const EndpointConfig& current) {
                attempts.push_back(current.host);
                return current.host == "backup"
                           ? TransportStatus::success()
                           : TransportStatus::failure("closed", FailureKind::kRetryable);
            });
        ok &= expect(status.ok, "expected the backup endpoint to complete publication");
        ok &= expect(attempts == std::vector<std::string>({"primary", "primary", "backup"}),
                     "expected failover only after the primary retry budget is exhausted");
    }

    {
        const std::vector<EndpointConfig> endpoints{endpoint("primary"), endpoint("backup")};
        std::vector<std::string> attempts;
        const TransportStatus status = publish_with_failover(
            endpoints, 3,
            [&attempts](const EndpointConfig& current) {
                attempts.push_back(current.host);
                return current.host == "primary"
                           ? TransportStatus::failure("unauthorized", FailureKind::kEndpointPermanent)
                           : TransportStatus::success();
            });
        ok &= expect(status.ok, "expected an endpoint-permanent failure to advance to backup");
        ok &= expect(attempts == std::vector<std::string>({"primary", "backup"}),
                     "expected an endpoint-permanent failure to skip same-endpoint retries");
    }

    {
        const std::vector<EndpointConfig> endpoints{endpoint("primary"), endpoint("backup")};
        std::size_t attempts = 0;
        const TransportStatus status = publish_with_failover(
            endpoints, 3,
            [&attempts](const EndpointConfig&) {
                ++attempts;
                return TransportStatus::failure("invalid media", FailureKind::kFatal);
            });
        ok &= expect(!status.ok && status.message == "invalid media",
                     "expected a fatal source failure to be returned unchanged");
        ok &= expect(attempts == 1, "expected fatal failures not to retry or fail over");
    }

    {
        const std::vector<EndpointConfig> endpoints{endpoint("primary")};
        const TransportStatus status = publish_with_failover(
            endpoints, std::numeric_limits<std::size_t>::max(),
            [](const EndpointConfig&) { return TransportStatus::success(); });
        ok &= expect(!status.ok && status.message == "retry count is too large",
                     "expected an overflowing public retry count to be rejected");
    }

    {
        const std::vector<EndpointConfig> endpoints{endpoint("primary"), endpoint("backup")};
        std::size_t attempts = 0;
        const TransportStatus status = publish_with_failover(
            endpoints, 2,
            [&attempts](const EndpointConfig&) {
                ++attempts;
                return TransportStatus::failure("operator cancelled", FailureKind::kCancelled);
            });
        ok &= expect(!status.ok && status.failure_kind == FailureKind::kCancelled,
                     "expected cancellation to propagate unchanged");
        ok &= expect(attempts == 1, "expected cancellation not to trigger retry or failover");
    }

    {
        std::vector<LiveObject> input{
            {.track_name = "catalog", .group_id = 0, .object_id = 0, .payload = {0xc0}},
            {.track_name = "video", .group_id = 7, .object_id = 0, .payload = {0x70}},
            {.track_name = "audio", .group_id = 7, .object_id = 0, .payload = {0x71}},
            {.track_name = "video", .group_id = 8, .object_id = 0, .payload = {0x80}},
            {.track_name = "audio", .group_id = 8, .object_id = 0, .payload = {0x81}},
        };
        std::size_t next = 0;
        LiveObjectSource source{
            .tracks = {
                LiveTrack{.track_name = "catalog"},
                LiveTrack{.track_name = "video", .media_type = LiveMediaType::kVideo},
                LiveTrack{.track_name = "audio", .media_type = LiveMediaType::kAudio},
            },
            .next_object = [&input, &next]() -> std::optional<LiveObject> {
                if (next == input.size()) {
                    return std::nullopt;
                }
                return input[next++];
            },
            .catalog_mode = LiveCatalogMode::kSourceObject,
        };
        ReplayableLiveObjectSource replayable(std::move(source), 4);

        replayable.begin_attempt();
        auto first_attempt = replayable.source();
        ok &= expect(first_attempt.next_object()->track_name == "catalog",
                     "expected the first attempt to begin with the source catalog");
        static_cast<void>(first_attempt.next_object());
        static_cast<void>(first_attempt.next_object());
        static_cast<void>(first_attempt.next_object());

        replayable.begin_attempt();
        auto second_attempt = replayable.source();
        const auto replayed_catalog = second_attempt.next_object();
        const auto replayed_video = second_attempt.next_object();
        const auto resumed_audio = second_attempt.next_object();
        ok &= expect(replayed_catalog && replayed_catalog->track_name == "catalog",
                     "expected a replacement session to receive the retained catalog first");
        ok &= expect(replayed_video && replayed_video->track_name == "video" &&
                         replayed_video->group_id == 8,
                     "expected retry replay to start at the newest retained video group");
        ok &= expect(resumed_audio && resumed_audio->track_name == "audio" &&
                         resumed_audio->group_id == 8,
                     "expected publication to resume from the paused underlying source");
        ok &= expect(replayable.buffered_objects() <= 4,
                     "expected the live replay buffer to remain bounded");
    }

    return ok ? 0 : 1;
}
