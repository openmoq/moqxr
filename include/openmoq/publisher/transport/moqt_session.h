#pragma once

#include "openmoq/publisher/cmsf_packager.h"
#include "openmoq/publisher/cat4moq.h"
#include "openmoq/publisher/live_object.h"
#include "openmoq/publisher/msf_catalog.h"
#include "openmoq/publisher/transport/moqt_control_messages.h"
#include "openmoq/publisher/transport/publisher_transport.h"

#include <atomic>
#include <functional>
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

using NowFunction = std::function<std::chrono::steady_clock::time_point()>;

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
                         openmoq::publisher::cat4moq::AuthorizationConfig authorization = {},
                         NowFunction now_function = std::chrono::steady_clock::now);

    explicit MoqtSession(PublisherTransport& transport,
                         std::string track_namespace = "media",
                         bool auto_forward = false,
                         bool publish_catalog = false,
                         bool paced = false,
                         bool loop = false,
                         std::chrono::seconds subscriber_timeout = std::chrono::seconds(30),
                         openmoq::publisher::cat4moq::AuthorizationConfig authorization = {},
                         NowFunction now_function = std::chrono::steady_clock::now);

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

    // MSF section 11.3. Publishes the final independent catalog via the
    // persistent catalog_publisher_ member -- isComplete with empty tracks
    // for kTerminate, or isLive false (with per-track trackDuration when
    // known) for kConvertToVod -- on a fresh subgroup stream for the catalog
    // track, then sends PUBLISH_DONE (status 0x2 Track Ended, already
    // hardcoded by encode_publish_done_message) for every request ID this
    // session still has bookkeeping for (publish_stream_id_by_request_id_) --
    // the only per-request state that survives outside the blocking
    // publish_*() loop. `draft_version` is threaded in explicitly because
    // MoqtSession does not persist the draft used by the in-progress publish
    // call; it is only ever a parameter to publish()/publish_live(), never a
    // member, so there is nothing to read it back from here.
    //
    // Per-track media durations are not tracked as persistent per-track
    // state on MoqtSession (see catalog_publisher_'s own comment), so
    // kConvertToVod is always called with an empty duration map: every track
    // is still correctly marked not live, it simply omits trackDuration
    // rather than reporting a fabricated figure.
    //
    // Per-track stream_count (the real count belongs to the per-track
    // SubgroupSenderState that lives inside the blocking publish loop, not
    // on MoqtSession) is not available here either, so every media
    // PUBLISH_DONE reports stream_count 0 rather than a fabricated number.
    TransportStatus end_broadcast(openmoq::publisher::EndBroadcastMode mode,
                                  openmoq::publisher::DraftVersion draft_version);
    PublishStats publish_stats() const;

    // MSF section 5.3: when non-zero, publish_live() periodically re-emits an
    // independent catalog (via CatalogPublisher::force_independent) even when
    // its content has not changed, bounding how many deltas a joining
    // subscriber must replay. Zero (the default) disables this and preserves
    // today's one-shot catalog delivery exactly.
    void set_catalog_republish_interval(std::chrono::seconds interval) {
        catalog_republish_interval_ = interval;
    }

    // See PublisherConfig::preannounce_tracks.
    void set_preannounce_tracks(bool enabled) {
        preannounce_tracks_ = enabled;
    }

private:
    void reset_publish_stats();
    void record_published_object(const std::string& track_name, std::uint64_t group_id, std::size_t payload_bytes);
    std::optional<std::vector<std::uint8_t>> setup_authorization_token() const;
    std::optional<std::vector<std::uint8_t>> action_authorization_token() const;

    TransportStatus ensure_setup(openmoq::publisher::DraftVersion draft);
    TransportStatus ensure_control_stream(openmoq::publisher::DraftVersion draft);
    TransportStatus write_frame(std::uint64_t stream_id, std::span<const std::uint8_t> frame, bool fin);

    // Write each CatalogObject onto its own fresh subgroup stream for
    // track_alias (a new group always means a new stream, so there is never
    // a previously-open stream to reuse here). Shared by the live catalog
    // send/republish path and by end_broadcast's final catalog write.
    TransportStatus send_catalog_objects(openmoq::publisher::DraftVersion draft_version,
                                         std::uint64_t track_alias,
                                         const std::vector<openmoq::publisher::CatalogObject>& objects,
                                         DeliveryTimeouts delivery_timeouts = {},
                                         std::size_t* streams_opened = nullptr);

    PublisherTransport& transport_;
    std::string track_namespace_;
    bool auto_forward_ = false;
    bool publish_catalog_ = false;
    bool preannounce_tracks_ = false;
    bool paced_ = false;
    bool loop_ = false;
    std::chrono::seconds subscriber_timeout_ = std::chrono::seconds(30);
    openmoq::publisher::cat4moq::AuthorizationConfig authorization_;
    NowFunction now_function_ = std::chrono::steady_clock::now;
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

    // Persistent catalog lifecycle state (MSF section 5 and 11.3). This is
    // the plumbing the "Catalog is a one-shot track" comments elsewhere in
    // moqt_session.cpp used to say MoqtSession lacked: it now survives across
    // the SUBSCRIBE handling closures inside publish_live() and is still here
    // when end_broadcast() is called, whether from the same call or (as
    // Publisher::end_broadcast does) from a different thread after it.
    //
    // catalog_track_alias_ is only meaningful once catalog_track_alias_known_
    // is true, which the two publish_live() overloads set right after they
    // compute the real alias. Only they run catalog_publisher_.publish() at
    // all, so a session driven through publish() (the batch/plan path) or
    // publish_live_objects() never sets this flag; end_broadcast() must
    // check it before writing a "final catalog" object, because alias 0 is
    // only guaranteed to mean "catalog" inside publish_live()'s own alias
    // map -- build_published_tracks() (used by the batch path) assigns
    // alias 0 to whichever track happens to appear first in the plan, which
    // need not be catalog. Without this guard, end_broadcast() on such a
    // session would inject a catalog JSON payload onto some other track's
    // alias.
    openmoq::publisher::CatalogPublisher catalog_publisher_;
    std::chrono::seconds catalog_republish_interval_{0};
    std::chrono::steady_clock::time_point last_catalog_published_at_{};
    std::uint64_t catalog_track_alias_ = 0;
    bool catalog_track_alias_known_ = false;
};

}  // namespace openmoq::publisher::transport
