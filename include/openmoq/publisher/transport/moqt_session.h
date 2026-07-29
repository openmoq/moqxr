#pragma once

#include "openmoq/publisher/cmsf_packager.h"
#include "openmoq/publisher/cat4moq.h"
#include "openmoq/publisher/live_object.h"
#include "openmoq/publisher/msf_catalog.h"
#include "openmoq/publisher/transport/publisher_transport.h"

#include <atomic>
#include <iosfwd>
#include <optional>
#include <chrono>
#include <span>
#include <string>
#include <string_view>
#include <map>
#include <unordered_map>
#include <vector>

namespace openmoq::publisher::transport {

struct LiveSrtCallerOptions {
    std::string id;
    std::string endpoint;
    bool fragment_on_keyframe = true;
    bool empty_moov = true;
    bool default_base_moof = true;
    bool separate_moof_per_track = true;
    std::uint32_t target_fragment_duration_ms = 1000;
    std::uint32_t latency_ms = 120;
    bool auto_detect_program = true;
    std::optional<std::uint32_t> program_number;
    std::optional<std::uint32_t> video_pid;
    std::optional<std::uint32_t> audio_pid;
};

struct LiveIngestOptions {
    bool use_stdin = false;
    std::vector<LiveSrtCallerOptions> srt_callers;
};

class MoqtSession {
public:
    struct PublishStats {
        std::uint64_t bytes_published = 0;
        std::uint64_t objects_published = 0;
        std::uint64_t groups_published = 0;
    };

    explicit MoqtSession(PublisherTransport& transport,
                         std::string track_namespace,
                         bool auto_forward,
                         bool publish_catalog,
                         bool paced,
                         std::chrono::seconds subscriber_timeout,
                         openmoq::publisher::cat4moq::AuthorizationConfig authorization = {});

    explicit MoqtSession(PublisherTransport& transport,
                         std::string track_namespace = "media",
                         bool auto_forward = false,
                         bool publish_catalog = false,
                         bool paced = false,
                         bool loop = false,
                         std::chrono::seconds subscriber_timeout = std::chrono::seconds(30),
                         openmoq::publisher::cat4moq::AuthorizationConfig authorization = {});

    TransportStatus connect(const EndpointConfig& endpoint, const TlsConfig& tls);
    TransportStatus publish(const openmoq::publisher::PublishPlan& plan);
    TransportStatus publish_live(std::istream& input,
                                 openmoq::publisher::DraftVersion draft_version,
                                 bool split_cmaf_chunks,
                                 bool stream_per_object = false);
    TransportStatus publish_live(const LiveIngestOptions& ingest,
                                 std::istream* stdin_input,
                                 openmoq::publisher::DraftVersion draft_version,
                                 bool split_cmaf_chunks,
                                 bool stream_per_object = false);
    TransportStatus publish_live_objects(const openmoq::publisher::LiveObjectSource& source,
                                         openmoq::publisher::DraftVersion draft_version);
    TransportStatus close(std::uint64_t application_error_code = 0);

    // MSF section 11.3. Sends PUBLISH_DONE (status 0x2 Track Ended, already
    // hardcoded by encode_publish_done_message) for every request ID this
    // session still has bookkeeping for (publish_stream_id_by_request_id_) --
    // the only per-request state that survives outside the blocking
    // publish_*() loop. `draft_version` is threaded in explicitly because
    // MoqtSession does not persist the draft used by the in-progress publish
    // call; it is only ever a parameter to publish()/publish_live(), never a
    // member, so there is nothing to read it back from here.
    //
    // `mode` is accepted for the stable public signature but currently has
    // no effect: NOT YET IMPLEMENTED is publishing the final independent
    // catalog object (kConvertToVod vs. kTerminate only change that
    // catalog's contents). Doing so needs a persistent CatalogPublisher plus
    // catalog track/stream bookkeeping that MoqtSession does not have --
    // today the catalog track is a one-shot delivery local to each publish
    // call (see the "Catalog is a one-shot track" comment further down this
    // file). That plumbing, and making this method actually use `mode`, is
    // planned for Task 6.
    //
    // Per-track stream_count (the real count belongs to the per-track
    // SubgroupSenderState that lives inside the blocking publish loop, not
    // on MoqtSession) is not available here either, so every PUBLISH_DONE
    // reports stream_count 0 rather than a fabricated number.
    TransportStatus end_broadcast(openmoq::publisher::EndBroadcastMode mode,
                                  openmoq::publisher::DraftVersion draft_version);
    PublishStats publish_stats() const;

private:
    void reset_publish_stats();
    void record_published_object(const std::string& track_name, std::uint64_t group_id, std::size_t payload_bytes);
    std::optional<std::vector<std::uint8_t>> setup_authorization_token() const;
    std::optional<std::vector<std::uint8_t>> action_authorization_token() const;

    TransportStatus ensure_setup(openmoq::publisher::DraftVersion draft);
    TransportStatus ensure_control_stream(openmoq::publisher::DraftVersion draft);
    TransportStatus write_frame(std::uint64_t stream_id, std::span<const std::uint8_t> frame, bool fin);

    PublisherTransport& transport_;
    std::string track_namespace_;
    bool auto_forward_ = false;
    bool publish_catalog_ = false;
    bool paced_ = false;
    bool loop_ = false;
    std::chrono::seconds subscriber_timeout_ = std::chrono::seconds(30);
    openmoq::publisher::cat4moq::AuthorizationConfig authorization_;
    std::optional<EndpointConfig> endpoint_;
    std::uint64_t control_stream_id_ = 0;
    std::uint64_t peer_control_stream_id_ = 0;
    std::uint64_t peer_max_request_id_ = 0;
    std::vector<std::uint8_t> pending_control_bytes_;
    bool control_stream_open_ = false;
    bool peer_control_stream_open_ = false;
    bool setup_complete_ = false;
    std::uint64_t namespace_stream_id_ = 0;
    bool namespace_stream_open_ = false;
    std::map<std::uint64_t, std::uint64_t> publish_stream_id_by_request_id_;
    PublishStats publish_stats_{};
    std::unordered_map<std::string, std::uint64_t> last_group_by_track_;
    std::atomic<bool> stop_requested_{false};
};

}  // namespace openmoq::publisher::transport
