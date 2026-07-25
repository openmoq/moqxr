#pragma once

// libmoq-backed batch publish path. Only compiled when moqxr is built against
// the sibling libmoq service tier (OPENMOQ_HAS_LIBMOQ). The translation helpers
// below are pure and network-free so they can be unit-tested without a relay;
// the driver function drives a real moq_endpoint_t / moq_media_sender_t.
#ifdef OPENMOQ_HAS_LIBMOQ

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <mutex>
#include <string>
#include <vector>

#include <moq/cmaf.h>  // moq_sap_type_t / MOQ_SAP_*
#include <moq/endpoint.h>
#include <moq/media_object.h>
#include <moq/media_sender.h>

#include "openmoq/publisher/cmsf_packager.h"
#include "openmoq/publisher/live_srt_ingest.h"
#include "openmoq/publisher/publisher_api.h"
#include "openmoq/publisher/transport/publisher_transport.h"

namespace openmoq::publisher::transport {

// -- Translation (pure; unit-tested without a network) -------------------
//
// One libmoq media track per PublishPlan media track. Owns the backing
// storage (name, codec, init segment, ...) so cfg() can hand libmoq a
// borrowing moq_media_track_cfg_t that points into this object. The cfg is
// only valid while the owning LibmoqTrackTranslation is alive and unmoved.

struct LibmoqTrackTranslation {
    std::string name;
    moq_media_type_t media_type = MOQ_MEDIA_TYPE_VIDEO;
    moq_media_packaging_t packaging = MOQ_MEDIA_PACKAGING_CMAF;
    std::string codec;
    std::uint32_t timescale = 0;  // 0 => microseconds (libmoq default)
    std::vector<std::uint8_t> init_data;  // track-specific CMAF init segment
    bool is_live = false;                 // batch publish is VOD
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t framerate_millis = 0;
    std::uint32_t samplerate = 0;
    std::string channel_config;
    std::uint64_t bitrate = 0;  // max bitrate (bits/s); MSF-01 requires > 0

    moq_media_track_cfg_t cfg() const;
};

// One libmoq media send-object per PublishPlan media object. The typed timing
// and grouping fields are set here; the caller fills object().payload with a
// freshly created moq_rcbuf_t from this object's payload bytes before write().

struct LibmoqObjectTranslation {
    std::string track_name;
    std::size_t group_id = 0;
    std::size_t object_id = 0;
    bool starts_group = false;
    bool ends_group = false;
    bool is_sync = false;
    bool has_sap_type = false;  // true: moqxr declares a concrete CMSF SAP type
    moq_sap_type_t sap_type = MOQ_SAP_NONE;  // concrete 0..3; needed so a coalesced/
                                             // live group-start passes libmoq's §3.4 rule
    std::uint64_t decode_time_us = 0;
    std::uint64_t presentation_time_us = 0;
    std::vector<std::uint8_t> payload;  // full CMAF fragment bytes

    moq_media_send_object_t object() const;  // payload left NULL for the caller
};

struct LibmoqPlanTranslation {
    std::vector<LibmoqTrackTranslation> tracks;
    std::vector<LibmoqObjectTranslation> objects;  // media objects only
};

// Translate a *materialized* PublishPlan into libmoq track + object configs.
//
// moqxr's locally generated catalog object (kInitialization "catalog") and its
// generated timeline objects (kMetadata) are intentionally dropped: the libmoq
// media sender owns catalog publication and derives it from the configured
// tracks. Per-track init segments are taken from plan.track_initializations and
// carried into each track's init_data.
LibmoqPlanTranslation translate_plan_for_libmoq(const PublishPlan& plan);

// -- Live (stdin) translation (pure; unit-tested without a network) ------
//
// The live path streams fragments as they arrive rather than from a fully
// materialized plan, so the per-track config and per-object mapping are exposed
// directly. Tracks are marked isLive; init_data is the track-specific CMAF init
// segment built from the ftyp+moov header.

LibmoqTrackTranslation make_libmoq_live_track(const TrackDescription& track,
                                              const std::vector<std::uint8_t>& init_segment);

// Map one live MediaFragment (with its group_id/object_id already assigned by
// the caller's keyframe-grouping) into a libmoq send-object. ends_group is left
// false: in a live stream a group is closed by the next group-start object (and
// the final group by end_track), so boundaries are not known ahead of time.
LibmoqObjectTranslation make_libmoq_live_object(const MediaFragment& fragment);

// -- LiveObjectSource translation (pure; unit-tested without a network) ---
//
// A LiveTrack carries enough media metadata for libmoq when it declares a
// media_type and codec (and, for audio, sample_rate + channel_count). Bare
// legacy tracks (media_type kUnset) do not qualify.
bool live_track_has_media_metadata(const LiveTrack& track);

// Translate a LiveTrack into a libmoq track config (RAW or CMAF packaging).
LibmoqTrackTranslation make_libmoq_live_object_track(const LiveTrack& track);

// Translate a LiveObject into a libmoq send-object. starts_group/is_sync key off
// object_id==0; ends_group = final_in_subgroup && subgroup_contains_group_largest;
// timing comes from media_time_us.
LibmoqObjectTranslation make_libmoq_live_source_object(const LiveObject& object);

// Build a libmoq endpoint URL from an EndpointConfig:
//   raw QUIC      -> moqt://host:port/path
//   WebTransport  -> https://host:port/path
std::string libmoq_endpoint_url(const EndpointConfig& endpoint);

// -- Driver (batch publish over a real endpoint) -------------------------

struct LibmoqPublishStats {
    std::uint64_t bytes_published = 0;
    std::uint64_t objects_published = 0;
    std::uint64_t groups_published = 0;
};

// Connect an endpoint, attach a media sender, add the plan's tracks, write its
// media objects, end each finite track, then tear everything down cleanly.
// draft_version must be draft-16 or draft-18; the endpoint requests it exactly.
TransportStatus publish_plan_via_libmoq(const PublishPlan& materialized_plan,
                                        const PublisherConfig& config,
                                        const EndpointConfig& endpoint,
                                        const TlsConfig& tls,
                                        LibmoqPublishStats& out_stats);

// Shared handle for an in-flight libmoq live publish. Publisher owns it; the
// driver registers its endpoint (set_endpoint) once connected and clears it
// before teardown. disconnect() on another thread calls request_cancel(), which
// sets the cancel flag AND interrupts the live endpoint so a blocking libmoq
// wait returns IMMEDIATELY (rather than sitting until subscriber_timeout). The
// driver observes the cancel flag in its loops and tears down cleanly, returning
// success -- a cancel is a deliberate stop, matching the legacy MoqtSession
// close() semantics. A cancel set before the driver connects short-circuits
// without connecting. NOTE: stdin's blocking read still means cancel is observed
// once the current read returns; object/SRT/readiness waits stop promptly.
struct LibmoqLiveHandle {
    std::atomic<bool> cancel{false};
    std::mutex        ep_mutex;
    moq_endpoint_t*   ep = nullptr;  // valid only between set_endpoint(ep)/(nullptr)

    bool cancelled() const { return cancel.load(); }

    void set_endpoint(moq_endpoint_t* e) {
        std::lock_guard<std::mutex> lock(ep_mutex);
        ep = e;
    }

    void request_cancel() {
        cancel.store(true);
        std::lock_guard<std::mutex> lock(ep_mutex);
        if (ep != nullptr) {
            moq_endpoint_set_interrupted(ep, true);
        }
    }
};

// Readiness-wait primitive, factored out so cancellation-during-readiness is
// unit-testable without a network: the ops are injected. Polls is_ready() with
// fatal/timeout/closed/cancel exits; wait(step_us) blocks up to step_us and
// returns a moq_result_t (e.g. MOQ_ERR_CLOSED). With the endpoint interrupt
// latch set by request_cancel(), the real wait() returns at once, so the loop
// observes the cancel flag promptly.
enum class LibmoqReadyOutcome { kReady, kCancelled, kFatal, kTimeout, kClosed };
struct LibmoqReadyOps {
    std::function<bool()> is_ready;
    std::function<bool()> is_fatal;
    std::function<int(std::uint64_t)> wait;
};
LibmoqReadyOutcome libmoq_wait_ready(std::atomic<bool>* cancel,
                                     std::uint64_t timeout_us, std::uint64_t step_us,
                                     const LibmoqReadyOps& ops);

// Demand-wait primitive: block until a downstream subscriber exists (lazy relays
// forward a SUBSCRIBE only when a player subscribes). Factored out like
// libmoq_wait_ready so it is unit-testable without a network. Polls
// has_subscriber() with fatal/closed/timeout/cancel exits; wait(step_us) blocks
// up to step_us OR until the demand callback wakes it. has_subscriber() is the
// authoritative check (the callback only nudges the wait).
enum class LibmoqDemandOutcome { kSubscriber, kCancelled, kFatal, kTimeout, kClosed };
struct LibmoqDemandOps {
    std::function<bool()> has_subscriber;
    std::function<bool()> is_fatal;
    std::function<bool()> is_closed;
    std::function<void(std::uint64_t)> wait;
};
LibmoqDemandOutcome libmoq_wait_demand(std::atomic<bool>* cancel,
                                       std::uint64_t timeout_us, std::uint64_t step_us,
                                       const LibmoqDemandOps& ops);

// Bounded retry for a blocking sender op (write / end_track), so a stalled send
// queue (e.g. the subscriber left, or the queue never drains) can no longer hang
// the publish. attempt() returns a moq_result_t: MOQ_OK -> kOk; a non-WOULD_BLOCK
// error -> kError (the code is written to *out_rc); MOQ_ERR_WOULD_BLOCK -> re-check
// cancel/fatal/closed/(optional)demand/timeout, then wait(step_us) and retry.
// Factored out for network-free unit tests. timeout_us bounds the total retry
// (PublisherConfig::subscriber_timeout); has_demand is optional (batch media
// writes set it so a departed subscriber yields kNoDemand).
enum class LibmoqRetryOutcome { kOk, kCancelled, kFatal, kClosed, kTimeout, kNoDemand, kError };
struct LibmoqRetryOps {
    std::function<int()> attempt;
    std::function<bool()> is_fatal;
    std::function<bool()> is_closed;
    std::function<bool()> has_demand;  // optional; when set and false -> kNoDemand
    std::function<void(std::uint64_t)> wait;
};
LibmoqRetryOutcome libmoq_retry_blocking(std::atomic<bool>* cancel,
                                         std::uint64_t timeout_us, std::uint64_t step_us,
                                         const LibmoqRetryOps& ops, int* out_rc);

// Live stdin publish over libmoq: read ftyp+moov for track discovery, attach a
// media sender, add the discovered tracks, then stream moof+mdat fragments as
// send-objects until stdin EOF, ending each track on the way out. (SRT and
// LiveObjectSource are handled by their own drivers below.)
TransportStatus publish_live_stdin_via_libmoq(std::istream& input,
                                              const PublisherConfig& config,
                                              const EndpointConfig& endpoint,
                                              const TlsConfig& tls,
                                              LibmoqPublishStats& out_stats,
                                              LibmoqLiveHandle* live = nullptr);

// LiveObjectSource publish over libmoq: add the source's (metadata-bearing)
// tracks, wait for readiness, then pull objects from source.next_object() until
// it returns nullopt, mapping each through make_libmoq_live_source_object().
// Objects for undeclared tracks are rejected. Callers should validate that every
// track satisfies live_track_has_media_metadata() before invoking this.
TransportStatus publish_live_objects_via_libmoq(const LiveObjectSource& source,
                                                const PublisherConfig& config,
                                                const EndpointConfig& endpoint,
                                                const TlsConfig& tls,
                                                LibmoqPublishStats& out_stats,
                                                LibmoqLiveHandle* live = nullptr);

// Live SRT publish over libmoq: start a LiveSrtIngestManager for the configured
// callers, attach a media sender, add the bootstrap-discovered tracks, then
// drain the manager's MediaFragment callback queue into send-objects until the
// source ends, ending each track on the way out. The manager is always stopped
// and joined on every exit path after a successful start(). (LiveObjectSource is
// handled by its own driver above.)
TransportStatus publish_live_srt_via_libmoq(std::vector<LiveSrtCallerRuntimeConfig> srt_callers,
                                            const PublisherConfig& config,
                                            const EndpointConfig& endpoint,
                                            const TlsConfig& tls,
                                            LibmoqPublishStats& out_stats,
                                            LibmoqLiveHandle* live = nullptr);

}  // namespace openmoq::publisher::transport

#endif  // OPENMOQ_HAS_LIBMOQ
